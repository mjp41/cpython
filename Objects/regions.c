
#include "Python.h"
#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include "pycore_dict.h"
#include "pycore_interp.h"
#include "pycore_object.h"
#include "pycore_regions.h"

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

    _Py_VPYDBG("pushing ");
    _Py_VPYDBGPRINT(object);
    _Py_VPYDBG(" [rc=%ld]\n", object->ob_refcnt);
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
    _Py_VPYDBG("stack: ");
    node* n = s->head;
    while(n != NULL){
        _Py_VPYDBGPRINT(n->object);
        _Py_VPYDBG("[rc=%ld]\n", n->object->ob_refcnt);
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
    printf("Error: Invalid edge %p -> %p: %s\n", src, tgt, msg);
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


/* A traversal callback for _Py_CheckRegionInvariant.
   - op is the target of the reference we are checking, and
   - parent is the source of the reference we are checking.
*/
static int
visit_invariant_check(PyObject *op, void *parent)
{
    PyObject *src_op = _PyObject_CAST(parent);
    // Check Immutable only reaches immutable
    if ((src_op->ob_region == _Py_IMMUTABLE)
        && (op->ob_region != _Py_IMMUTABLE))
        {
            set_failed_edge(src_op, op, "Destination is not immutable");
            return 0;
        }
    // TODO: More checks to go here as we add more region
    // properties.

    return 0;
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
            PyObject* type_op = PyObject_Type(op);
            visit_invariant_check(type_op, op);
            Py_DECREF(type_op);

            // If we detected an error, stop so we don't
            // write too much.
            // TODO: The first error might not be the most useful.
            // So might not need to build all error edges as a structure.
            if (error_occurred)
                return 1;
        }
    }

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
    _Py_VPYDBG("value(");
    _Py_VPYDBGPRINT(value);
    _Py_VPYDBG(") -> ");

    _PyDict_SetKeyImmutable((PyDictObject*)globals, name);

    if(!_Py_IsImmutable(value)){
        _Py_VPYDBG("pushed\n");
        Py_INCREF(value);
        return value;
    }else{
        _Py_VPYDBG("immutable\n");
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

    _Py_VPYDBG("function: ");
    _Py_VPYDBGPRINT(op);
    _Py_VPYDBG("[rc=%ld]\n", Py_REFCNT(op));

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
    _Py_VPYDBG("function: adding captured vars/funcs/builtins\n");
    while(!stack_empty(f_stack)){
        f_ptr = stack_pop(f_stack); // fp.rc = x + 1
        _PyObject_ASSERT(f_ptr, PyCode_Check(f_ptr));
        f_code = (PyCodeObject*)f_ptr;

        _Py_VPYDBG("analysing code: ");
        _Py_VPYDBGPRINT(f_code->co_name);
        _Py_VPYDBG("\n");

        size = 0;
        if (f_code->co_names != NULL)
          size = PySequence_Fast_GET_SIZE(f_code->co_names);
        _Py_VPYDBG("Enumerating %ld names\n", size);
        for(Py_ssize_t i = 0; i < size; i++){
            PyObject* name = PySequence_Fast_GET_ITEM(f_code->co_names, i); // name.rc = x
            _Py_VPYDBG("name ");
            _Py_VPYDBGPRINT(name);
            _Py_VPYDBG(": ");

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
                _Py_VPYDBG("builtin\n");

                _PyDict_SetKeyImmutable((PyDictObject*)builtins, name);

                PyObject* value = PyDict_GetItem(builtins, name); // value.rc = x
                if(!_Py_IsImmutable(value)){
                    _Py_SetImmutable(value);
                }
            }else if(PyDict_Contains(module_dict, name)){
                PyObject* value = PyDict_GetItem(module_dict, name); // value.rc = x
                _Py_VPYDBG("module(");
                _Py_VPYDBGPRINT(value);
                _Py_VPYDBG(") -> ");

                _PyDict_SetKeyImmutable((PyDictObject*)module_dict, name);

                if(!_Py_IsImmutable(value)){
                    Py_INCREF(value); // value.rc = x + 1
                    if(stack_push(frontier, value)){
                        stack_free(f_stack);
                        // frontier freed by the caller
                        return PyErr_NoMemory();
                    }
                }else{
                    _Py_VPYDBG("immutable\n");
                }
            }else{
                _Py_VPYDBG("instance\n");
                // TODO assert that it is an instance variable
            }
        }

        size = PySequence_Fast_GET_SIZE(f_code->co_consts);
        _Py_VPYDBG("Enumerating %ld consts\n", size);
        for(Py_ssize_t i = 0; i < size; i++){
            PyObject* value = PySequence_Fast_GET_ITEM(f_code->co_consts, i); // value.rc = x
            _Py_VPYDBG("const ");
            _Py_VPYDBGPRINT(value);
            _Py_VPYDBG(": ");
            if(!_Py_IsImmutable(value)){
                Py_INCREF(value); // value.rc = x + 1
                if(PyCode_Check(value)){
                    _Py_VPYDBG("nested_func\n");

                    _Py_SetImmutable(value);

                    if(stack_push(f_stack, value)){
                        stack_free(f_stack);
                        // frontier freed by the caller
                        return PyErr_NoMemory();
                    }
                }else{
                    _Py_VPYDBG("pushed\n");

                    if(stack_push(frontier, value)){
                        stack_free(f_stack);
                        // frontier freed by the caller
                        return PyErr_NoMemory();
                    }
                }
            }else{
                _Py_VPYDBG("immutable\n");
            }

            if(check_globals && PyUnicode_Check(value)){
                _Py_VPYDBG("checking if");
                _Py_VPYDBGPRINT(value);
                _Py_VPYDBG(" is a global: ");
                PyObject* name = value;
                if(PyDict_Contains(globals, name)){
                    _Py_VPYDBG(" true ");
                    value = make_global_immutable(globals, name);
                    if(!Py_IsNone(value)){
                        if(stack_push(frontier, value)){
                            stack_free(f_stack);
                            // frontier freed by the caller
                            return PyErr_NoMemory();
                        }
                    }
                }else{
                    _Py_VPYDBG("false\n");

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
        _Py_VPYDBG("Enumerating %ld closure vars to check for global names\n", size);
        for(Py_ssize_t i=0; i < size; ++i){
            PyObject* cellvar = PySequence_Fast_GET_ITEM(f->func_closure, i); // cellvar.rc = x
            PyObject* value = PyCell_GET(cellvar); // value.rc = x
            _Py_VPYDBG("cellvar(");
            _Py_VPYDBGPRINT(value);
            _Py_VPYDBG(") is ");

            if(PyUnicode_Check(value)){
                PyObject* name = value;
                if(PyDict_Contains(globals, name)){
                    _Py_VPYDBG("a global ");
                    value = make_global_immutable(globals, name);
                    if(!Py_IsNone(value)){
                        if(stack_push(frontier, value)){
                            stack_free(f_stack);
                            // frontier freed by the caller
                            return PyErr_NoMemory();
                        }
                    }
                }else{
                    _Py_VPYDBG("not a global\n");
                }
            }else{
                _Py_VPYDBG("not a global\n");
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
    _Py_VPYDBG("visit(");
    _Py_VPYDBGPRINT(obj);
    _Py_VPYDBG(") region: %lu rc: %ld\n", Py_REGION(obj), Py_REFCNT(obj));
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
    // We have started using regions, so notify to potentially enable checks.
    notify_regions_in_use();

    _Py_VPYDBG(">> makeimmutable(");
    _Py_VPYDBGPRINT(obj);
    _Py_VPYDBG(") region: %lu rc: %ld\n", Py_REGION(obj), Py_REFCNT(obj));
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

        _Py_VPYDBG("item: ");
        _Py_VPYDBGPRINT(item);

        if(_Py_IsImmutable(item)){
            _Py_VPYDBG(" already immutable!\n");
            // Direct access like this is not recommended, but will be removed in the future as
            // this is just for debugging purposes.
            if(type->ob_base.ob_base.ob_region != _Py_IMMUTABLE){
               // Why do we need to handle the type here, surely what ever made this immutable already did that?
               // Log so we can investigate.
                _Py_VPYDBG("type ");
                _Py_VPYDBGPRINT(type_op);
                _Py_VPYDBG(" not immutable! but object is: ");
                _Py_VPYDBGPRINT(item);
                _Py_VPYDBG("\n");
            }
            goto handle_type;
        }
        _Py_VPYDBG("\n");

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
            _Py_VPYDBG("implements tp_traverse\n");
            if(traverse(item, (visitproc)_makeimmutable_visit, frontier)){
                Py_DECREF(item);
                stack_free(frontier);
                return NULL;
            }
        }else{
            _Py_VPYDBG("does not implements tp_traverse\n");
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

    _Py_VPYDBGPRINT(obj);
    _Py_VPYDBG(" region: %lu rc: %ld \n", Py_REGION(obj), Py_REFCNT(obj));
    _Py_VPYDBG("<< makeimmutable complete\n\n");

    return obj;
}

typedef struct PyRegionObject PyRegionObject;
typedef struct regionmetadata regionmetadata;

struct regionmetadata {
    int lrc;  // Integer field for "local reference count"
    int osc;  // Integer field for "open subregion count"
    int is_open;
    regionmetadata* parent;
    PyRegionObject* bridge;
};

struct PyRegionObject {
    PyObject_HEAD
    regionmetadata* metadata;
    PyObject *name;   // Optional string field for "name"
};

static void RegionMetadata_inc_lrc(regionmetadata* data) {
    data->lrc += 1;
}

static void RegionMetadata_dec_lrc(regionmetadata* data) {
    data->lrc -= 1;
}

static void RegionMetadata_inc_osc(regionmetadata* data) {
    data->osc += 1;
}

static void RegionMetadata_dec_osc(regionmetadata* data) {
    data->osc -= 1;
}

static void RegionMetadata_open(regionmetadata* data) {
    data->is_open = 1;
}

static void RegionMetadata_close(regionmetadata* data) {
    data->is_open = 0;
}

static bool RegionMetadata_is_open(regionmetadata* data) {
    return data->is_open == 0;
}

static void RegionMetadata_set_parent(regionmetadata* data, regionmetadata* parent) {
    data->parent = parent;
}

static bool RegionMetadata_has_parent(regionmetadata* data) {
    return data->parent != NULL;
}

static regionmetadata* RegionMetadata_get_parent(regionmetadata* data) {
    return data->parent;
}

static void RegionMetadata_unparent(regionmetadata* data) {
    RegionMetadata_set_parent(data, NULL);
}

static PyObject* RegionMetadata_is_root(regionmetadata* data) {
    if (RegionMetadata_has_parent(data)) {
        Py_RETURN_TRUE;
    } else {
        Py_RETURN_FALSE;
    }
}

static regionmetadata* Region_get_metadata(PyRegionObject* obj) {
    return obj->metadata;
}


static void Region_dealloc(PyRegionObject *self) {
    Py_XDECREF(self->name);
    self->metadata->bridge = NULL;
    // The lifetimes are joined for now
    free(self->metadata);
    Py_TYPE(self)->tp_free((PyObject *)self);
}

static int Region_init(PyRegionObject *self, PyObject *args, PyObject *kwds) {
    notify_regions_in_use();

    static char *kwlist[] = {"name", NULL};
    self->metadata = (regionmetadata*)calloc(1, sizeof(regionmetadata));
    self->metadata->bridge = self;
    self->name = NULL;

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "|U", kwlist, &self->name))
        return -1;

    Py_XINCREF(self->name);
    return 0;
}

// is_open method (returns True if the region is open, otherwise False)
static PyObject *Region_is_open(PyRegionObject *self, PyObject *args) {
    if (RegionMetadata_is_open(self->metadata)) {
        Py_RETURN_TRUE;  // Return True if the region is open
    } else {
        Py_RETURN_FALSE; // Return False if the region is closed
    }
}

// Open method (sets the region to "open")
static PyObject *Region_open(PyRegionObject *self, PyObject *args) {
    RegionMetadata_open(self->metadata);
    Py_RETURN_NONE;  // Return None (standard for methods with no return value)
}

// Close method (sets the region to "closed")
static PyObject *Region_close(PyRegionObject *self, PyObject *args) {
    RegionMetadata_close(self->metadata);  // Mark as closed
    Py_RETURN_NONE;  // Return None (standard for methods with no return value)
}

// Adds args object to self region
static PyObject *Region_add_object(PyRegionObject *self, PyObject *args) {
    regionmetadata* md = Region_get_metadata(self);
    if (args->ob_region == _Py_DEFAULT_REGION) {
        args->ob_region = (Py_uintptr_t) md;
        Py_RETURN_NONE;
    } else {
        PyErr_SetString(PyExc_RuntimeError, "Object already had an owner or was immutable!");
        return NULL;
    }
}

// Remove args object to self region
static PyObject *Region_remove_object(PyRegionObject *self, PyObject *args) {
    regionmetadata* md = Region_get_metadata(self);
    if (args->ob_region == (Py_uintptr_t) md) {
        args->ob_region = _Py_DEFAULT_REGION;
        Py_RETURN_NONE;
    } else {
        PyErr_SetString(PyExc_RuntimeError, "Object not a member of region!");
        return NULL;
    }
}

// Return True if args object is member of self region
static PyObject *Region_owns_object(PyRegionObject *self, PyObject *args) {
    if ((Py_uintptr_t) Region_get_metadata(self) == args->ob_region) {
        Py_RETURN_TRUE;
    } else {
        Py_RETURN_FALSE;
    }
}

static PyObject *Region_repr(PyRegionObject *self) {
    regionmetadata* data = self->metadata;
    // FIXME: deprecated flag, but config.parse_debug seems to not work?
    if (Py_DebugFlag) {
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
static PyMethodDef Region_methods[] = {
    {"open", (PyCFunction)Region_open, METH_NOARGS, "Open the region."},
    {"close", (PyCFunction)Region_close, METH_NOARGS, "Close the region."},
    {"is_open", (PyCFunction)Region_is_open, METH_NOARGS, "Check if the region is open."},
    {"add_object", (PyCFunction)Region_add_object, METH_O, "Add object to the region."},
    {"remove_object", (PyCFunction)Region_remove_object, METH_O, "Remove object from the region."},
    {"owns_object", (PyCFunction)Region_owns_object, METH_O, "Check if object is owned by the region."},
    {NULL}  // Sentinel
};


PyTypeObject PyRegion_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    "Region",                          /* tp_name */
    sizeof(PyRegionObject),              /* tp_basicsize */
    0,                                 /* tp_itemsize */
    (destructor)Region_dealloc,        /* tp_dealloc */
    0,                                 /* tp_vectorcall_offset */
    0,                                 /* tp_getattr */
    0,                                 /* tp_setattr */
    0,                                 /* tp_as_async */
    (reprfunc)Region_repr,             /* tp_repr */
    0,                                 /* tp_as_number */
    0,                                 /* tp_as_sequence */
    0,                                 /* tp_as_mapping */
    0,                                 /* tp_hash  */
    0,                                 /* tp_call */
    0,                                 /* tp_str */
    0,                                 /* tp_getattro */
    0,                                 /* tp_setattro */
    0,                                 /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT,                /* tp_flags */
    "TODO =^.^=",                      /* tp_doc */
    0,                                 /* tp_traverse */
    0,                                 /* tp_clear */
    0,                                 /* tp_richcompare */
    0,                                 /* tp_weaklistoffset */
    0,                                 /* tp_iter */
    0,                                 /* tp_iternext */
    Region_methods,                    /* tp_methods */
    0,                                 /* tp_members */
    0,                                 /* tp_getset */
    0,                                 /* tp_base */
    0,                                 /* tp_dict */
    0,                                 /* tp_descr_get */
    0,                                 /* tp_descr_set */
    0,                                 /* tp_dictoffset */
    (initproc)Region_init,             /* tp_init */
    0,                                 /* tp_alloc */
    PyType_GenericNew,                 /* tp_new */
};
