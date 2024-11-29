
#include "Python.h"
#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "object.h"
#include "pycore_dict.h"
#include "pycore_interp.h"
#include "pycore_object.h"
#include "pycore_regions.h"

typedef struct PyRegionObject PyRegionObject;
typedef struct regionmetadata regionmetadata;

static PyObject *PyRegion_add_object(PyRegionObject *self, PyObject *args);
static PyObject *PyRegion_remove_object(PyRegionObject *self, PyObject *args);

struct PyRegionObject {
    PyObject_HEAD
    regionmetadata* metadata;
    PyObject *name;   // Optional string field for "name"
    PyObject *dict;
};

struct regionmetadata {
    int lrc;  // Integer field for "local reference count"
    int osc;  // Integer field for "open subregion count"
    int is_open;
    regionmetadata* parent;
    PyRegionObject* bridge;
    // TODO: Currently only used for invariant checking. If it's not used for other things
    // it might make sense to make this conditional in debug builds (or something)
    //
    // Intrinsic list for invariant checking
    regionmetadata* next;
};

bool is_bridge_object(PyObject *op);
static int regionmetadata_has_ancestor(regionmetadata* data, regionmetadata* other);
static regionmetadata* PyRegion_get_metadata(PyRegionObject* obj);

/**
 * Simple implementation of stack for tracing during make immutable.
 * TODO: More efficient implementation
 */
typedef struct node_s {
    PyObject* object;
    struct node_s* next;
} node;

typedef struct stack_s {
    node* head;
} stack;

stack* stack_new(void){
    stack* s = (stack*)malloc(sizeof(stack));
    if(s == NULL){
        return NULL;
    }

    s->head = NULL;

    return s;
}

bool stack_push(stack* s, PyObject* object){
    node* n = (node*)malloc(sizeof(node));
    if(n == NULL){
        Py_DECREF(object);
        // Should we also free the stack?
        return true;
    }

    n->object = object;
    n->next = s->head;
    s->head = n;
    return false;
}

PyObject* stack_pop(stack* s){
    if(s->head == NULL){
        return NULL;
    }

    node* n = s->head;
    PyObject* object = n->object;
    s->head = n->next;
    free(n);

    return object;
}

void stack_free(stack* s){
    while(s->head != NULL){
        PyObject* op = stack_pop(s);
        Py_DECREF(op);
    }

    free(s);
}

bool stack_empty(stack* s){
    return s->head == NULL;
}

void stack_print(stack* s){
    node* n = s->head;
    while(n != NULL){
        n = n->next;
    }
}

bool is_c_wrapper(PyObject* obj){
    return PyCFunction_Check(obj) || Py_IS_TYPE(obj, &_PyMethodWrapper_Type) || Py_IS_TYPE(obj, &PyWrapperDescr_Type);
}

/**
 * Global status for performing the region check.
 */
bool do_region_check = false;

// The src object for an edge that invalidated the invariant.
PyObject* error_src = Py_None;

// The tgt object for an edge that invalidated the invariant.
PyObject* error_tgt = Py_None;

// Once an error has occurred this is used to surpress further checking
bool error_occurred = false;

// Start of a linked list of bridge objects used to check for external uniqueness
// Bridge objects appear in this list if they are captured
#define CAPTURED_SENTINEL ((regionmetadata*) 0xc0defefe)
regionmetadata* captured = CAPTURED_SENTINEL;

/**
 * Enable the region check.
 */
void notify_regions_in_use(void)
{
    // Do not re-enable, if we have detected a fault.
    if (!error_occurred)
        do_region_check = true;
}

PyObject* _Py_EnableInvariant(void)
{
    // Disable failure as program has explicitly requested invariant to be checked again.
    error_occurred = false;
    // Re-enable region check
    do_region_check = true;
    // Reset the error state
    Py_DecRef(error_src);
    error_src = Py_None;
    Py_DecRef(error_tgt);
    error_tgt = Py_None;
    return Py_None;
}

/**
 * Set the global variables for a failure.
 * This allows the interpreter to inspect what has failed.
 */
void set_failed_edge(PyObject* src, PyObject* tgt, const char* msg)
{
    Py_DecRef(error_src);
    Py_IncRef(src);
    error_src = src;
    Py_DecRef(error_tgt);
    Py_IncRef(tgt);
    error_tgt = tgt;
    PyErr_Format(PyExc_RuntimeError, "Error: Invalid edge %p -> %p: %s\n", src, tgt, msg);
    // We have discovered a failure.
    // Disable region check, until the program switches it back on.
    do_region_check = false;
    error_occurred = true;
}

PyObject* _Py_InvariantSrcFailure(void)
{
    return Py_NewRef(error_src);
}

PyObject* _Py_InvariantTgtFailure(void)
{
    return Py_NewRef(error_tgt);
}


// Lifted from gcmodule.c
typedef struct _gc_runtime_state GCState;
#define GEN_HEAD(gcstate, n) (&(gcstate)->generations[n].head)
#define GC_NEXT _PyGCHead_NEXT
#define GC_PREV _PyGCHead_PREV
#define FROM_GC(g) ((PyObject *)(((char *)(g))+sizeof(PyGC_Head)))

static int
is_immutable_region(regionmetadata* r)
{
    return ((Py_uintptr_t) r) == _Py_IMMUTABLE;
}

static int
is_default_region(regionmetadata* r)
{
    return ((Py_uintptr_t) r) == _Py_DEFAULT_REGION;
}

/* A traversal callback for _Py_CheckRegionInvariant.
   - op is the target of the reference we are checking, and
   - parent is the source of the reference we are checking.
*/
static int
visit_invariant_check(PyObject *tgt, void *parent)
{
    PyObject *src_op = _PyObject_CAST(parent);
    regionmetadata* src_region = (regionmetadata*) src_op->ob_region;
    regionmetadata* tgt_region = (regionmetadata*) tgt->ob_region;
     // Internal references are always allowed
    if (src_region == tgt_region)
        return 0;
    // Anything is allowed to point to immutable
    if (is_immutable_region(tgt_region))
        return 0;
    // Borrowed references are unrestricted
    if (is_default_region(src_region))
        return 0;
    // Since tgt is not immutable, src also may not be as immutable may not point to mutable
    if (is_immutable_region(src_region)) {
        set_failed_edge(src_op, tgt, "Destination is not immutable");
        return 0;
    }

    // Cross-region references must be to a bridge
    if (!is_bridge_object(tgt)) {
        set_failed_edge(src_op, tgt, "Destination is not in the same region");
        return 0;
    }
    // Check if region is already added to captured list
    if (tgt_region->next != NULL) {
        // Bridge object was already captured
        set_failed_edge(src_op, tgt, "Bridge object not externally unique");
        return 0;
    }
    // Forbid cycles in the region topology
    if (regionmetadata_has_ancestor((regionmetadata*)src_region, (regionmetadata*)tgt_region)) {
        set_failed_edge(src_op, tgt, "Region cycle detected");
        return 0;
    }

    // First discovery of bridge -- add to list of captured bridge objects
    tgt_region->next = captured;
    captured = tgt_region;

    return 0;
}

void invariant_reset_captured_list(void) {
    // Reset the captured list
    while (captured != CAPTURED_SENTINEL) {
        regionmetadata* m = captured;
        captured = m->next;
        m->next = NULL;
    }
}

/**
 * This uses checks that the region topology is valid.
 *
 * It is currently implemented using the GC data. This
 * means that not all objects are traversed as some objects
 * are considered to not participate in cycles, and hence
 * do not need to be understood for the cycle detector.
 *
 * This is not ideal for the region invariant, but is a good
 * first approximation.  We could actually walk the heap
 * in a subsequent more elaborate invariant check.
 *
 * Returns non-zero if the invariant is violated.
 */
int _Py_CheckRegionInvariant(PyThreadState *tstate)
{
    // Check if we should perform the region invariant check
    if(!do_region_check){
        return 0;
    }

    // Use the GC data to find all the objects, and traverse them to
    // confirm all their references satisfy the region invariant.
    GCState *gcstate = &tstate->interp->gc;

    // There is an cyclic doubly linked list per generation of all the objects
    // in that generation.
    for (int i = NUM_GENERATIONS-1; i >= 0; i--) {
        PyGC_Head *containers = GEN_HEAD(gcstate, i);
        PyGC_Head *gc = GC_NEXT(containers);
        // Walk doubly linked list of objects.
        for (; gc != containers; gc = GC_NEXT(gc)) {
            PyObject *op = FROM_GC(gc);
            // Local can point to anything.  No invariant needed
            if (op->ob_region == _Py_DEFAULT_REGION)
                continue;
            // Functions are complex.
            // Removing from invariant initially.
            // TODO provide custom traverse here.
            if (PyFunction_Check(op))
                continue;

            // TODO the immutable code ignores c_wrappers
            // review if this is correct.
            if (is_c_wrapper(op))
                continue;

            // Use traverse proceduce to visit each field of the object.
            traverseproc traverse = Py_TYPE(op)->tp_traverse;
            (void) traverse(op,
                            (visitproc)visit_invariant_check,
                            op);

            // Also need to visit the type of the object
            // As this isn't covered by the traverse.
            // TODO: this might be covered by tp_traverse?
            PyObject* type_op = PyObject_Type(op);
            visit_invariant_check(type_op, op);
            Py_DECREF(type_op);

            // If we detected an error, stop so we don't
            // write too much.
            // TODO: The first error might not be the most useful.
            // So might not need to build all error edges as a structure.
            if (error_occurred) {
                invariant_reset_captured_list();
                return 1;
            }
        }
    }

    invariant_reset_captured_list();
    return 0;
}

#define _Py_VISIT_FUNC_ATTR(attr, frontier) do { \
    if(attr != NULL && !_Py_IsImmutable(attr)){ \
        Py_INCREF(attr); \
        if(stack_push(frontier, attr)){ \
            return PyErr_NoMemory(); \
        } \
    } \
} while(0)

PyObject* make_global_immutable(PyObject* globals, PyObject* name)
{
    PyObject* value = PyDict_GetItem(globals, name); // value.rc = x

    _PyDict_SetKeyImmutable((PyDictObject*)globals, name);

    if(!_Py_IsImmutable(value)){
        Py_INCREF(value);
        return value;
    }else{
        Py_RETURN_NONE;
    }
}

/**
 * Special function for walking the reachable graph of a function object.
 *
 * This is necessary because the function object has a pointer to the global
 * object, and this is problematic because freezing any function will make the
 * global object immutable, which is not always the desired behaviour.
 *
 * This function attempts to find the globals that a function will use, and freeze
 * just those, and prevent those keys from being updated in the global dictionary
 * from this point onwards.
 */
PyObject* walk_function(PyObject* op, stack* frontier)
{
    PyObject* builtins;
    PyObject* globals;
    PyObject* module;
    PyObject* module_dict;
    PyFunctionObject* f;
    PyObject* f_ptr;
    PyCodeObject* f_code;
    Py_ssize_t size;
    stack* f_stack;
    bool check_globals = false;
    _PyObject_ASSERT(op, PyFunction_Check(op));


    _Py_SetImmutable(op);


    f = (PyFunctionObject*)op;

    // TODO find a way to use traverse to avoid having to manually walk
    // the function's members
    // f->func_code needs special treatment (see below)
    // func_globals, func_builtins, and func_module can stay mutable, but depending on code we may need to make some keys immutable
    globals = f->func_globals;
    builtins = f->func_builtins;
    module = PyImport_Import(f->func_module);
    if(PyModule_Check(module)){
        module_dict = PyModule_GetDict(module);
    }else{
        module_dict = NULL;
    }

    _Py_VISIT_FUNC_ATTR(f->func_defaults, frontier);
    _Py_VISIT_FUNC_ATTR(f->func_kwdefaults, frontier);
    _Py_VISIT_FUNC_ATTR(f->func_doc, frontier);
    _Py_VISIT_FUNC_ATTR(f->func_name, frontier);
    _Py_VISIT_FUNC_ATTR(f->func_dict, frontier);
    _Py_VISIT_FUNC_ATTR(f->func_closure, frontier);
    _Py_VISIT_FUNC_ATTR(f->func_annotations, frontier);
    _Py_VISIT_FUNC_ATTR(f->func_typeparams, frontier);
    _Py_VISIT_FUNC_ATTR(f->func_qualname, frontier);

    f_stack = stack_new();
    if(f_stack == NULL){
        return PyErr_NoMemory();
    }

    f_ptr = f->func_code;
    if(stack_push(f_stack, f_ptr)){
        stack_free(f_stack);
        return PyErr_NoMemory();
    }

    Py_INCREF(f_ptr); // fp.rc = x + 1
    while(!stack_empty(f_stack)){
        f_ptr = stack_pop(f_stack); // fp.rc = x + 1
        _PyObject_ASSERT(f_ptr, PyCode_Check(f_ptr));
        f_code = (PyCodeObject*)f_ptr;


        size = 0;
        if (f_code->co_names != NULL)
          size = PySequence_Fast_GET_SIZE(f_code->co_names);
        for(Py_ssize_t i = 0; i < size; i++){
            PyObject* name = PySequence_Fast_GET_ITEM(f_code->co_names, i); // name.rc = x

            if(PyUnicode_CompareWithASCIIString(name, "globals") == 0){
                // if the code calls the globals() builtin, then any
                // cellvar or const in the function could, potentially, refer to
                // a global variable. As such, we need to check if the globals
                // dictionary contains that key and then make it immutable
                // from this point forwards.
                check_globals = true;
            }

            if(PyDict_Contains(globals, name)){
                PyObject* value = make_global_immutable(globals, name);
                if(!Py_IsNone(value)){
                    if(stack_push(frontier, value)){
                        stack_free(f_stack);
                        // frontier freed by the caller
                        return PyErr_NoMemory();
                    }
                }
            }else if(PyDict_Contains(builtins, name)){

                _PyDict_SetKeyImmutable((PyDictObject*)builtins, name);

                PyObject* value = PyDict_GetItem(builtins, name); // value.rc = x
                if(!_Py_IsImmutable(value)){
                    _Py_SetImmutable(value);
                }
            }else if(PyDict_Contains(module_dict, name)){
                PyObject* value = PyDict_GetItem(module_dict, name); // value.rc = x

                _PyDict_SetKeyImmutable((PyDictObject*)module_dict, name);

                if(!_Py_IsImmutable(value)){
                    Py_INCREF(value); // value.rc = x + 1
                    if(stack_push(frontier, value)){
                        stack_free(f_stack);
                        // frontier freed by the caller
                        return PyErr_NoMemory();
                    }
                }else{
                }
            }else{
                // TODO assert that it is an instance variable
            }
        }

        size = PySequence_Fast_GET_SIZE(f_code->co_consts);
        for(Py_ssize_t i = 0; i < size; i++){
            PyObject* value = PySequence_Fast_GET_ITEM(f_code->co_consts, i); // value.rc = x
            if(!_Py_IsImmutable(value)){
                Py_INCREF(value); // value.rc = x + 1
                if(PyCode_Check(value)){

                    _Py_SetImmutable(value);

                    if(stack_push(f_stack, value)){
                        stack_free(f_stack);
                        // frontier freed by the caller
                        return PyErr_NoMemory();
                    }
                }else{

                    if(stack_push(frontier, value)){
                        stack_free(f_stack);
                        // frontier freed by the caller
                        return PyErr_NoMemory();
                    }
                }
            }else{
            }

            if(check_globals && PyUnicode_Check(value)){
                PyObject* name = value;
                if(PyDict_Contains(globals, name)){
                    value = make_global_immutable(globals, name);
                    if(!Py_IsNone(value)){
                        if(stack_push(frontier, value)){
                            stack_free(f_stack);
                            // frontier freed by the caller
                            return PyErr_NoMemory();
                        }
                    }
                }else{

                }
            }
        }

        Py_DECREF(f_ptr); // fp.rc = x
    }

    stack_free(f_stack);

    if(check_globals){
        size = 0;
        if(f->func_closure != NULL)
            size = PySequence_Fast_GET_SIZE(f->func_closure);
        for(Py_ssize_t i=0; i < size; ++i){
            PyObject* cellvar = PySequence_Fast_GET_ITEM(f->func_closure, i); // cellvar.rc = x
            PyObject* value = PyCell_GET(cellvar); // value.rc = x

            if(PyUnicode_Check(value)){
                PyObject* name = value;
                if(PyDict_Contains(globals, name)){
                    value = make_global_immutable(globals, name);
                    if(!Py_IsNone(value)){
                        if(stack_push(frontier, value)){
                            stack_free(f_stack);
                            // frontier freed by the caller
                            return PyErr_NoMemory();
                        }
                    }
                }else{
                }
            }else{
            }
        }
    }

    Py_RETURN_NONE;
}

#define _Py_MAKEIMMUTABLE_CALL(f, item, frontier) do { \
    PyObject* err = f((item), (frontier));             \
    if(!Py_IsNone(err)){                               \
        Py_DECREF(item);                               \
        stack_free((frontier));                        \
        return err;                                    \
    }                                                  \
} while(0)

int _makeimmutable_visit(PyObject* obj, void* frontier)
{
    if(!_Py_IsImmutable(obj)){
        if(stack_push((stack*)frontier, obj)){
            PyErr_NoMemory();
            return -1;
        }
    }

    return 0;
}

PyObject* _Py_MakeImmutable(PyObject* obj)
{
    if (!obj) {
        return NULL;
    }

    // We have started using regions, so notify to potentially enable checks.
    notify_regions_in_use();

    if(_Py_IsImmutable(obj) && _Py_IsImmutable(Py_TYPE(obj))){
        return obj;
    }

    stack* frontier = stack_new();
    if(frontier == NULL){
        return PyErr_NoMemory();
    }

    if(stack_push(frontier, obj)){
        stack_free(frontier);
        return PyErr_NoMemory();
    }

    Py_INCREF(obj); // obj.rc = x + 1
    while(!stack_empty(frontier)){
        PyObject* item = stack_pop(frontier); // item.rc = x + 1
        PyTypeObject* type = Py_TYPE(item);
        traverseproc traverse;
        PyObject* type_op = NULL;


        if(_Py_IsImmutable(item)){
            // Direct access like this is not recommended, but will be removed in the future as
            // this is just for debugging purposes.
            if(type->ob_base.ob_base.ob_region != _Py_IMMUTABLE){
               // Why do we need to handle the type here, surely what ever made this immutable already did that?
               // Log so we can investigate.
            }
            goto handle_type;
        }

        _Py_SetImmutable(item);

        if(is_c_wrapper(item)) {
            // C functions are not mutable, so we can skip them.
            goto next;
        }

        if(PyFunction_Check(item)){
            _Py_MAKEIMMUTABLE_CALL(walk_function, item, frontier);
            goto handle_type;
        }


        traverse = type->tp_traverse;
        if(traverse != NULL){
            if(traverse(item, (visitproc)_makeimmutable_visit, frontier)){
                Py_DECREF(item);
                stack_free(frontier);
                return NULL;
            }
        }else{
            // TODO: (mjp comment) These functions causes every character of
            // a string to become an immutable object, which is is not the
            // desired behavior.  Commenting so we can discuss.  I believe
            // we should depend solely on the tp_traverse function to
            // determine the objects an object depends on.
            //
            // _Py_MAKEIMMUTABLE_CALL(walk_sequence, item, frontier);
            // _Py_MAKEIMMUTABLE_CALL(walk_mapping, item, frontier);
        }

handle_type:
        type_op = PyObject_Type(item); // type_op.rc = x + 1
        if (!_Py_IsImmutable(type_op)){
            // Previously this included a check for is_leaf_type, but
            if (stack_push(frontier, type_op))
            {
                Py_DECREF(item);
                stack_free(frontier);
                return PyErr_NoMemory();
            }
        }
        else {
            Py_DECREF(type_op); // type_op.rc = x
        }

next:
        Py_DECREF(item); // item.rc = x
    }

    stack_free(frontier);


    Py_RETURN_NONE;
}

bool is_bridge_object(PyObject *op) {
    Py_uintptr_t region = op->ob_region;
    if (region == _Py_IMMUTABLE || region == _Py_DEFAULT_REGION) {
        return 0;
    }

    if ((Py_uintptr_t)((regionmetadata*)region)->bridge == (Py_uintptr_t)op) {
        return 1;
    } else {
        return 0;
    }
}

__attribute__((unused))
static void regionmetadata_inc_lrc(regionmetadata* data) {
    data->lrc += 1;
}

__attribute__((unused))
static void regionmetadata_dec_lrc(regionmetadata* data) {
    data->lrc -= 1;
}

__attribute__((unused))
static void regionmetadata_inc_osc(regionmetadata* data) {
    data->osc += 1;
}

__attribute__((unused))
static void regionmetadata_dec_osc(regionmetadata* data) {
    data->osc -= 1;
}

static void regionmetadata_open(regionmetadata* data) {
    data->is_open = 1;
}

static void regionmetadata_close(regionmetadata* data) {
    data->is_open = 0;
}

static bool regionmetadata_is_open(regionmetadata* data) {
    return data->is_open == 0;
}

static void regionmetadata_set_parent(regionmetadata* data, regionmetadata* parent) {
    data->parent = parent;
}

static bool regionmetadata_has_parent(regionmetadata* data) {
    return data->parent != NULL;
}

static regionmetadata* regionmetadata_get_parent(regionmetadata* data) {
    return data->parent;
}

__attribute__((unused))
static void regionmetadata_unparent(regionmetadata* data) {
    regionmetadata_set_parent(data, NULL);
}

__attribute__((unused))
static PyObject* regionmetadata_is_root(regionmetadata* data) {
    if (regionmetadata_has_parent(data)) {
        Py_RETURN_TRUE;
    } else {
        Py_RETURN_FALSE;
    }
}

static int regionmetadata_has_ancestor(regionmetadata* data, regionmetadata* other) {
    do {
        if (data == other) {
            return 1;
        }
        data = regionmetadata_get_parent(data);
    } while (data);
    return 0;
}

static regionmetadata* PyRegion_get_metadata(PyRegionObject* obj) {
    return obj->metadata;
}


static void PyRegion_dealloc(PyRegionObject *self) {
    // Name is immutable and not in our region.
    Py_XDECREF(self->name);
    self->name = NULL;
    self->metadata->bridge = NULL;

    // The dictionary can be NULL if the Region constructor crashed
    if (self->dict) {
        // We need to clear the ownership, since this dictionary might be
        // returned to an object pool rather than freed. This would result
        // in an error if the dictionary has the previous region.
        PyRegion_remove_object(self, (PyObject*)self->dict);
        Py_DECREF(self->dict);
        self->dict = NULL;
    }

    PyObject_GC_UnTrack((PyObject *)self);

    // The lifetimes are joined for now
    free(self->metadata);
    Py_TYPE(self)->tp_free((PyObject *)self);
}

static int PyRegion_init(PyRegionObject *self, PyObject *args, PyObject *kwds) {
    notify_regions_in_use();

    static char *kwlist[] = {"name", NULL};
    self->metadata = (regionmetadata*)calloc(1, sizeof(regionmetadata));
    self->metadata->bridge = self;
    self->name = NULL;

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "|U", kwlist, &self->name))
        return -1;
    if (self->name) {
        Py_XINCREF(self->name);
        // Freeze the name and it's type. Short strings in Python are interned
        // by default. This means that `id("AB") == id("AB")`. We therefore
        // need to either clone the name object or freeze it to share it
        // across regions. Freezing should be safe, since `+=` and other
        // operators return new strings and keep the old one intact
        _Py_MakeImmutable(self->name);
        // FIXME: Implicit freezing should take care of this instead
        if (!_Py_IsImmutable(self->name)) {
            PyRegion_add_object(self, self->name);
        }
    }

    // Make the region an owner of the bridge object
    self->ob_base.ob_region = (Py_uintptr_t) self->metadata;
    _Py_MakeImmutable((PyObject*)Py_TYPE(self));

    // FIXME: Usually this is created on the fly. We need to do it manually to
    // set the region and freeze the type
    self->dict = PyDict_New();
    if (self->dict == NULL) {
        return -1; // Propagate memory allocation failure
    }
    _Py_MakeImmutable((PyObject*)Py_TYPE(self->dict));
    PyRegion_add_object(self, self->dict);

    return 0;
}

static int PyRegion_traverse(PyRegionObject *self, visitproc visit, void *arg) {
    Py_VISIT(self->name);
    Py_VISIT(self->dict);
    return 0;
}

// is_open method (returns True if the region is open, otherwise False)
static PyObject *PyRegion_is_open(PyRegionObject *self, PyObject *args) {
    if (regionmetadata_is_open(self->metadata)) {
        Py_RETURN_TRUE;  // Return True if the region is open
    } else {
        Py_RETURN_FALSE; // Return False if the region is closed
    }
}

// Open method (sets the region to "open")
static PyObject *PyRegion_open(PyRegionObject *self, PyObject *args) {
    regionmetadata_open(self->metadata);
    Py_RETURN_NONE;  // Return None (standard for methods with no return value)
}

// Close method (sets the region to "closed")
static PyObject *PyRegion_close(PyRegionObject *self, PyObject *args) {
    regionmetadata_close(self->metadata);  // Mark as closed
    Py_RETURN_NONE;  // Return None (standard for methods with no return value)
}

// Adds args object to self region
static PyObject *PyRegion_add_object(PyRegionObject *self, PyObject *args) {
    if (!args) {
        Py_RETURN_NONE;
    }

    regionmetadata* md = PyRegion_get_metadata(self);
    if (args->ob_region == _Py_DEFAULT_REGION) {
        args->ob_region = (Py_uintptr_t) md;
        Py_RETURN_NONE;
    } else {
        PyErr_SetString(PyExc_RuntimeError, "Object already had an owner or was immutable!");
        return NULL;
    }
}

// Remove args object to self region
static PyObject *PyRegion_remove_object(PyRegionObject *self, PyObject *args) {
    if (!args) {
        Py_RETURN_NONE;
    }

    regionmetadata* md = PyRegion_get_metadata(self);
    if (args->ob_region == (Py_uintptr_t) md) {
        args->ob_region = _Py_DEFAULT_REGION;
        Py_RETURN_NONE;
    } else {
        PyErr_SetString(PyExc_RuntimeError, "Object not a member of region!");
        return NULL;
    }
}

// Return True if args object is member of self region
static PyObject *PyRegion_owns_object(PyRegionObject *self, PyObject *args) {
    if ((Py_uintptr_t) PyRegion_get_metadata(self) == args->ob_region) {
        Py_RETURN_TRUE;
    } else {
        Py_RETURN_FALSE;
    }
}

static PyObject *PyRegion_repr(PyRegionObject *self) {
    regionmetadata* data = self->metadata;
    // FIXME: deprecated flag, but config.parse_debug seems to not work?
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
    if (Py_DebugFlag) {
#pragma GCC diagnostic pop
        // Debug mode: include detailed representation
        return PyUnicode_FromFormat(
            "Region(lrc=%d, osc=%d, name=%S, is_open=%d)", data->lrc, data->osc, self->name ? self->name : Py_None, data->is_open
        );
    } else {
        // Normal mode: simple representation
        return PyUnicode_FromFormat("Region(name=%S, is_open=%d)", self->name ? self->name : Py_None, data->is_open);
    }
    Py_RETURN_NONE;
}

// Define the RegionType with methods
static PyMethodDef PyRegion_methods[] = {
    {"open", (PyCFunction)PyRegion_open, METH_NOARGS, "Open the region."},
    {"close", (PyCFunction)PyRegion_close, METH_NOARGS, "Close the region."},
    {"is_open", (PyCFunction)PyRegion_is_open, METH_NOARGS, "Check if the region is open."},
    {"add_object", (PyCFunction)PyRegion_add_object, METH_O, "Add object to the region."},
    {"remove_object", (PyCFunction)PyRegion_remove_object, METH_O, "Remove object from the region."},
    {"owns_object", (PyCFunction)PyRegion_owns_object, METH_O, "Check if object is owned by the region."},
    {NULL}  // Sentinel
};


PyTypeObject PyRegion_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    "Region",                                /* tp_name */
    sizeof(PyRegionObject),                  /* tp_basicsize */
    0,                                       /* tp_itemsize */
    (destructor)PyRegion_dealloc,            /* tp_dealloc */
    0,                                       /* tp_vectorcall_offset */
    0,                                       /* tp_getattr */
    0,                                       /* tp_setattr */
    0,                                       /* tp_as_async */
    (reprfunc)PyRegion_repr,                 /* tp_repr */
    0,                                       /* tp_as_number */
    0,                                       /* tp_as_sequence */
    0,                                       /* tp_as_mapping */
    0,                                       /* tp_hash  */
    0,                                       /* tp_call */
    0,                                       /* tp_str */
    0,                                       /* tp_getattro */
    0,                                       /* tp_setattro */
    0,                                       /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC, /* tp_flags */
    "TODO =^.^=",                            /* tp_doc */
    (traverseproc)PyRegion_traverse,         /* tp_traverse */
    0,                                       /* tp_clear */
    0,                                       /* tp_richcompare */
    0,                                       /* tp_weaklistoffset */
    0,                                       /* tp_iter */
    0,                                       /* tp_iternext */
    PyRegion_methods,                        /* tp_methods */
    0,                                       /* tp_members */
    0,                                       /* tp_getset */
    0,                                       /* tp_base */
    0,                                       /* tp_dict */
    0,                                       /* tp_descr_get */
    0,                                       /* tp_descr_set */
    offsetof(PyRegionObject, dict),          /* tp_dictoffset */
    (initproc)PyRegion_init,                 /* tp_init */
    0,                                       /* tp_alloc */
    PyType_GenericNew,                       /* tp_new */
};
