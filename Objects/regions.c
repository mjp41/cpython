
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
#include "pycore_pyerrors.h"

#define Py_REGION_VISITED_FLAG ((Py_region_ptr_t)0x2)
static inline Py_region_ptr_with_tags_t Py_TAGGED_REGION(PyObject *ob) {
    return ob->ob_region;
}
#define Py_TAGGED_REGION(ob) Py_TAGGED_REGION(_PyObject_CAST(ob))
#define REGION_SET_TAG(ob, tag) (Py_SET_TAGGED_REGION(ob, Py_region_ptr_with_tags(Py_TAGGED_REGION(ob).value | tag)))
#define REGION_GET_TAG(ob, tag) (Py_TAGGED_REGION(ob).value & tag)
#define REGION_CLEAR_TAG(ob, tag) (Py_SET_TAGGED_REGION(ob, Py_region_ptr_with_tags(Py_TAGGED_REGION(ob).value & (~tag))))
#define Py_REGION_DATA(ob) (_Py_CAST(regionmetadata*, Py_REGION(ob)))

typedef struct regionmetadata regionmetadata;
typedef struct PyRegionObject PyRegionObject;

static PyObject *PyRegion_add_object(PyRegionObject *self, PyObject *args);
static PyObject *PyRegion_remove_object(PyRegionObject *self, PyObject *args);
static const char *get_region_name(PyObject* obj);
#define Py_REGION_DATA(ob) (_Py_CAST(regionmetadata*, Py_REGION(ob)))

/**
 * Global status for performing the region check.
 */
bool invariant_do_region_check = false;

// The src object for an edge that invalidated the invariant.
PyObject* invariant_error_src = Py_None;

// The tgt object for an edge that invalidated the invariant.
PyObject* invariant_error_tgt = Py_None;

// Once an error has occurred this is used to surpress further checking
bool invariant_error_occurred = false;

// This uses the given arguments to create and throw a `RegionError`
static void throw_region_error(
    PyObject* src, PyObject* tgt,
    const char *format_str, PyObject *obj)
{
    // Don't stomp existing exception
    PyThreadState *tstate = _PyThreadState_GET();
    assert(tstate && "_PyThreadState_GET documentation says it's not safe, when?");
    if (_PyErr_Occurred(tstate)) {
        return;
    }

    // This disables the invariance check, as it could otherwise emit a runtime
    // error before the emitted `RegionError` could be handled.
    invariant_do_region_check = false;
    invariant_error_occurred = true;

    // Create the error, this sets the error value in `tstate`
    PyErr_Format(PyExc_RegionError, format_str, obj);

    // Set source and target fields
    PyRegionErrorObject* exc = _Py_CAST(PyRegionErrorObject*,
                                        PyErr_GetRaisedException());
    Py_XINCREF(src);
    exc->source = src;
    Py_XINCREF(tgt);
    exc->target = tgt;
    PyErr_SetRaisedException(_PyObject_CAST(exc));
}

struct PyRegionObject {
    PyObject_HEAD
    regionmetadata* metadata;
    PyObject *dict;
};

struct regionmetadata {
    int lrc;  // Integer field for "local reference count"
    int osc;  // Integer field for "open subregion count"
    int is_open;
    regionmetadata* parent;
    PyRegionObject* bridge;
    PyObject *name;   // Optional string field for "name"
    // TODO: Currently only used for invariant checking. If it's not used for other things
    // it might make sense to make this conditional in debug builds (or something)
    //
    // Intrinsic list for invariant checking
    regionmetadata* next;
};

static bool is_bridge_object(PyObject *op);
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

static stack* stack_new(void){
    stack* s = (stack*)malloc(sizeof(stack));
    if(s == NULL){
        return NULL;
    }

    s->head = NULL;

    return s;
}

static bool stack_push(stack* s, PyObject* object){
    node* n = (node*)malloc(sizeof(node));
    if(n == NULL){
        // FIXME: This DECREF should only be used by MakeImmutable, since
        // `add_to_region` and other functions only use weak refs.
        Py_DECREF(object);
        // Should we also free the stack?
        return true;
    }

    n->object = object;
    n->next = s->head;
    s->head = n;
    return false;
}

static PyObject* stack_pop(stack* s){
    if(s->head == NULL){
        return NULL;
    }

    node* n = s->head;
    PyObject* object = n->object;
    s->head = n->next;
    free(n);

    return object;
}

static void stack_free(stack* s){
    while(s->head != NULL){
        PyObject* op = stack_pop(s);
        Py_DECREF(op);
    }

    free(s);
}

static bool stack_empty(stack* s){
    return s->head == NULL;
}

__attribute__((unused))
static void stack_print(stack* s){
    node* n = s->head;
    while(n != NULL){
        n = n->next;
    }
}

static bool is_c_wrapper(PyObject* obj){
    return PyCFunction_Check(obj) || Py_IS_TYPE(obj, &_PyMethodWrapper_Type) || Py_IS_TYPE(obj, &PyWrapperDescr_Type);
}

// Start of a linked list of bridge objects used to check for external uniqueness
// Bridge objects appear in this list if they are captured
#define CAPTURED_SENTINEL ((regionmetadata*) 0xc0defefe)
regionmetadata* captured = CAPTURED_SENTINEL;

/**
 * Enable the region check.
 */
static void notify_regions_in_use(void)
{
    // Do not re-enable, if we have detected a fault.
    if (!invariant_error_occurred)
        invariant_do_region_check = true;
}

PyObject* _Py_EnableInvariant(void)
{
    // Disable failure as program has explicitly requested invariant to be checked again.
    invariant_error_occurred = false;
    // Re-enable region check
    invariant_do_region_check = true;
    // Reset the error state
    Py_DecRef(invariant_error_src);
    invariant_error_src = Py_None;
    Py_DecRef(invariant_error_tgt);
    invariant_error_tgt = Py_None;
    return Py_None;
}

/**
 * Set the global variables for a failure.
 * This allows the interpreter to inspect what has failed.
 */
static void emit_invariant_error(PyObject* src, PyObject* tgt, const char* msg)
{
    Py_DecRef(invariant_error_src);
    Py_IncRef(src);
    invariant_error_src = src;
    Py_DecRef(invariant_error_tgt);
    Py_IncRef(tgt);
    invariant_error_tgt = tgt;

    /* Don't stomp existing exception */
    PyThreadState *tstate = _PyThreadState_GET();
    assert(tstate && "_PyThreadState_GET documentation says it's not safe, when?");
    if (_PyErr_Occurred(tstate)) {
        return;
    }

    _PyErr_Region(src, tgt, msg);

    // We have discovered a failure.
    // Disable region check, until the program switches it back on.
    invariant_do_region_check = false;
    invariant_error_occurred = true;
}

PyObject* _Py_InvariantSrcFailure(void)
{
    return Py_NewRef(invariant_error_src);
}

PyObject* _Py_InvariantTgtFailure(void)
{
    return Py_NewRef(invariant_error_tgt);
}


// Lifted from gcmodule.c
typedef struct _gc_runtime_state GCState;
#define GEN_HEAD(gcstate, n) (&(gcstate)->generations[n].head)
#define GC_NEXT _PyGCHead_NEXT
#define GC_PREV _PyGCHead_PREV
#define FROM_GC(g) ((PyObject *)(((char *)(g))+sizeof(PyGC_Head)))

#define IS_IMMUTABLE_REGION(r) ((Py_region_ptr_t)r == _Py_IMMUTABLE)
#define IS_LOCAL_REGION(r) ((Py_region_ptr_t)r == _Py_LOCAL_REGION)

/* A traversal callback for _Py_CheckRegionInvariant.
   - tgt is the target of the reference we are checking, and
   - src(_void) is the source of the reference we are checking.
*/
static int
visit_invariant_check(PyObject *tgt, void *src_void)
{
    PyObject *src = _PyObject_CAST(src_void);
    regionmetadata* src_region = Py_REGION_DATA(src);
    regionmetadata* tgt_region = Py_REGION_DATA(tgt);
     // Internal references are always allowed
    if (src_region == tgt_region)
        return 0;
    // Anything is allowed to point to immutable
    if (IS_IMMUTABLE_REGION(tgt_region))
        return 0;
    // Borrowed references are unrestricted
    if (IS_LOCAL_REGION(src_region))
        return 0;
    // Since tgt is not immutable, src also may not be as immutable may not point to mutable
    if (IS_IMMUTABLE_REGION(src_region)) {
        emit_invariant_error(src, tgt, "Reference from immutable object to mutable target");
        return 0;
    }

    // Cross-region references must be to a bridge
    if (!is_bridge_object(tgt)) {
        emit_invariant_error(src, tgt, "Reference from object in one region into another region");
        return 0;
    }
    // Check if region is already added to captured list
    if (tgt_region->next != NULL) {
        // Bridge object was already captured
        emit_invariant_error(src, tgt, "Reference to bridge is not externally unique");
        return 0;
    }
    // Forbid cycles in the region topology
    if (regionmetadata_has_ancestor(src_region, tgt_region)) {
        emit_invariant_error(src, tgt, "Regions create a cycle with subregions");
        return 0;
    }

    // First discovery of bridge -- add to list of captured bridge objects
    tgt_region->next = captured;
    captured = tgt_region;

    return 0;
}

static void invariant_reset_captured_list(void) {
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
    if(!invariant_do_region_check){
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
            if (_Py_IsLocal(op))
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
            if (invariant_error_occurred) {
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

static PyObject* make_global_immutable(PyObject* globals, PyObject* name)
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
static PyObject* make_function_immutable(PyObject* op, stack* frontier)
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

static int _makeimmutable_visit(PyObject* obj, void* frontier)
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
        Py_RETURN_NONE;
    }

    // We have started using regions, so notify to potentially enable checks.
    notify_regions_in_use();

    // Some built-in objects are direclty created immutable. However, their types
    // might be created in a mutable state. This therefore requres an additional
    // check to see if the type is also immutable.
    if(_Py_IsImmutable(obj) && _Py_IsImmutable(Py_TYPE(obj))){
        Py_RETURN_NONE;
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
            if (Py_REGION(&type->ob_base.ob_base) != _Py_IMMUTABLE) {
               // Why do we need to handle the type here, surely what ever made this immutable already did that?
            }
            goto handle_type;
        }

        _Py_SetImmutable(item);

        if(is_c_wrapper(item)) {
            // C functions are not mutable, so we can skip them.
            goto next;
        }

        if(PyFunction_Check(item)){
            _Py_MAKEIMMUTABLE_CALL(make_function_immutable, item, frontier);
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

typedef enum region_error_id {
    /* Adding this object to a region or creating this reference would
     * create a reference that points to a contained(non-bridge object)
     * inside another region.
     */
    ERR_CONTAINED_OBJ_REF,
    /* Adding this object to a region or creating this reference would
     * create a cycle in the region topology.
     */
    ERR_CYCLE_CREATION,
    /* Adding this object to a region or creating this reference would
     * isn't possible as the referenced bridge object already has a parent
     * region.
     */
    ERR_SHARED_CUSTODY,
    /* Functions can reference to global variables. That's why they need
     * special handling, as can be seen in `_Py_MakeImmutable`.
     * For now an error is emitted to see when this comes up and if
     * `make_function_immutable` can be reused.
     */
    ERR_WIP_FUNCTIONS,
} region_error_id;

/* An error that occurred in `add_to_region`. The struct contains all
 * informaiton needed to construct an error message or handle the error
 * differently.
 */
typedef struct regionerror {
    /* The source of the reference that created the region error.
     *
     * A weak reference, can be made into a strong reference with `Py_INCREF`
     */
    PyObject* src;
    /* The target of the reference that created the region error.
     *
     * A weak reference, can be made into a strong reference with `Py_INCREF`
     */
    PyObject* tgt;
    /* This ID indicates what kind of error occurred.
     */
    region_error_id id;
} regionerror;

/* Used by `_add_to_region_visit` to handle errors. The first argument is
 * the error information. The second argument is supplementary data
 * passed along by `add_to_region`.
 */
typedef int (*handle_add_to_region_error)(regionerror *, void *);

/* This takes the region error and emits it as a `RegionError` to the
 * user. This function will always return `false` to stop the propagation
 * from `add_to_region`
 *
 * This function borrows both arguments. The memory has to be managed
 * the caller.
 */
static int emit_region_error(regionerror *error, void* ignored) {
    const char* msg = NULL;

    switch (error->id)
    {
    case ERR_CONTAINED_OBJ_REF:
        msg = "References to objects in other regions are forbidden";
        break;
    case ERR_CYCLE_CREATION:
        msg = "Regions are not allowed to create cycles";
        break;
    case ERR_SHARED_CUSTODY:
        msg = "Regions can only have one parent at a time";
        break;
    case ERR_WIP_FUNCTIONS:
        msg = "WIP: Functions in regions are not supported yet";
        break;
    default:
        assert(false && "unreachable?");
        break;
    }
    throw_region_error(error->src, error->tgt, msg, NULL);

    // We never want to continue once an error has been emitted.
    return -1;
}

typedef struct addtoregionvisitinfo {
    stack* pending;
    // The source object of the reference. This is used to create
    // better error message
    PyObject* src;
    handle_add_to_region_error handle_error;
    void* handle_error_data;
} addtoregionvisitinfo;

static int _add_to_region_visit(PyObject* target, void* info_void)
{
    addtoregionvisitinfo *info = _Py_CAST(addtoregionvisitinfo *, info_void);

    // Region objects are allowed to reference immutable objects. Immutable
    // objects are only allowed to reference other immutable objects and cowns.
    // we therefore don't need to traverse them.
    if (_Py_IsImmutable(target)) {
        return 0;
    }

    // C wrappers can propergate through the entire system and draw
    // in a lot of unwanted objects. Since c wrappers don't have mutable
    // data, we just make it immutable and have the immutability impl
    // handle it. We then have an edge from our region to an immutable
    // object which is again valid.
    if (is_c_wrapper(target)) {
        _Py_MakeImmutable(target);
        return 0;
    }

    if (_Py_IsLocal(target)) {
        // Add reference to the object,
        // minus one for the reference we just followed
        Py_REGION_DATA(info->src)->lrc += target->ob_refcnt - 1;
        Py_SET_REGION(target, Py_REGION(info->src));

        if (stack_push(info->pending, target)) {
            PyErr_NoMemory();
            return -1;
        }
        return 0;
    }

    // The item was previously in the local region but has already been
    // added to the region by a previous iteration. We therefore only need
    // to adjust the LRC
    if (Py_REGION(target) == Py_REGION(info->src)) {
        // -1 for the refernce we just followed
        Py_REGION_DATA(target)->lrc -= 1;
        return 0;
    }

    // We push it onto the stack to be added to the region and traversed.
    // The actual addition of the object is done in `add_to_region`. We keep
    // it in the local region, to indicate to `add_to_region` that the object
    // should actually be processed.
    if (IS_LOCAL_REGION(Py_REGION(target))) {
        // The actual region update and write checks are done in the
        // main body of `add_to_region`
        if (stack_push(info->pending, target)) {
            PyErr_NoMemory();
            return -1;
        }
        return 0;
    }

    // At this point, we know that target is in another region.
    // If target is in a different region, it has to be a bridge object.
    // References to contained objects are forbidden.
    if (!is_bridge_object(target)) {
        regionerror err = {.src = info->src, .tgt = target,
                           .id = ERR_CONTAINED_OBJ_REF };
        return ((info->handle_error)(&err, info->handle_error_data));
    }

    // The target is a bridge object from another region. We now need to
    // if it already has a parent.
    regionmetadata *target_region = Py_REGION_DATA(target);
    if (target_region->parent != NULL) {
        regionerror err = {.src = info->src, .tgt = target,
                           .id = ERR_SHARED_CUSTODY};
        return ((info->handle_error)(&err, info->handle_error_data));
    }

    // Make sure that the new subregion relation won't create a cycle
    regionmetadata* region = Py_REGION_DATA(info->src);
    if (regionmetadata_has_ancestor(region, target_region)) {
        regionerror err = {.src = info->src, .tgt = target,
                           .id = ERR_CYCLE_CREATION};
        return ((info->handle_error)(&err, info->handle_error_data));
    }

    // From the previous checks we know that `target` is the bridge object
    // of a free region. Thus we can make it a sub region and allow the
    // reference.
    target_region->parent = region;

    return 0;
}

// This function visits all outgoing reference from `item` including the
// type. It will return `false` if the operation failed.
static int visit_object(PyObject *item, visitproc visit, void* info) {
    if (PyFunction_Check(item)) {
        // FIXME: This is a temporary error. It should be replaced by
        // proper handling of moving the function into the region
        regionerror err = {.src = NULL,
            .tgt = item, .id = ERR_WIP_FUNCTIONS };
        emit_region_error(&err, NULL);
        return false;
    } else {
        PyTypeObject *type = Py_TYPE(item);
        traverseproc traverse = type->tp_traverse;
        if (traverse != NULL) {
            if (traverse(item, visit, info)) {
                return false;
            }
        }
    }

    // Visit the type manually, since it's not included in the normal
    // `tp_treverse`.
    PyObject* type_ob = _PyObject_CAST(Py_TYPE(item));
    // Visit will return 0 if everything was okayw
    return ((visit)(type_ob, info) == 0);
}

// Add the transitive closure of objets in the local region reachable from obj to region
static PyObject *add_to_region(PyObject *obj, Py_region_ptr_t region)
{
    if (!obj) {
        Py_RETURN_NONE;
    }

    // Make sure there are no pending exceptions that would be overwritten
    // by us.
    PyThreadState *tstate = _PyThreadState_GET();
    if (_PyErr_Occurred(tstate)) {
        return NULL;
    }

    // The current implementation assumes region is a valid pointer. This
    // restriction can be lifted if needed
    assert(!IS_LOCAL_REGION(region) || !IS_IMMUTABLE_REGION(region));
    regionmetadata *region_data = _Py_CAST(regionmetadata *, region);

    // Early return if the object is already in the region or immutable
    if (Py_REGION(obj) == region || _Py_IsImmutable(obj)) {
        Py_RETURN_NONE;
    }

    addtoregionvisitinfo info = {
        .pending = stack_new(),
        // `src` is reassigned each iteration
        .src = _PyObject_CAST(region_data->bridge),
        .handle_error = emit_region_error,
        .handle_error_data = NULL,
    };
    if (info.pending == NULL) {
        return PyErr_NoMemory();
    }

    // The visit call is used to correctly add the object or
    // add it to the pending stack, for further processing.
    if (_add_to_region_visit(obj, &info)) {
        stack_free(info.pending);
        return NULL;
    }

    while (!stack_empty(info.pending)) {
        PyObject *item = stack_pop(info.pending);

        // Add `info.src` for better error messages
        info.src = item;

        if (!visit_object(item, (visitproc)_add_to_region_visit, &info)) {
            stack_free(info.pending);
            return NULL;
        }
    }

    stack_free(info.pending);

    Py_RETURN_NONE;
}


static bool is_bridge_object(PyObject *op) {
    Py_region_ptr_t region = Py_REGION(op);
    if (IS_LOCAL_REGION(region) || IS_IMMUTABLE_REGION(region)) {
        return false;
    }

    // It's not yet clear how immutability will interact with region objects.
    // It's likely that the object will remain in the object topology but
    // will use the properties of a bridge object. This therefore checks if
    // the object is equal to the regions bridge object rather than checking
    // that the type is `PyRegionObject`
    return ((Py_region_ptr_t)((regionmetadata*)region)->bridge == (Py_region_ptr_t)op);
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
static int regionmetadata_is_root(regionmetadata* data) {
    return regionmetadata_has_parent(data);
}

static int regionmetadata_has_ancestor(regionmetadata* data, regionmetadata* other) {
    do {
        if (data == other) {
            return true;
        }
        data = regionmetadata_get_parent(data);
    } while (data);
    return false;
}

static regionmetadata* PyRegion_get_metadata(PyRegionObject* obj) {
    return obj->metadata;
}


static void PyRegion_dealloc(PyRegionObject *self) {
    // Name is immutable and not in our region.
    Py_XDECREF(self->metadata->name);
    self->metadata->name = NULL;
    self->metadata->bridge = NULL;

    // The dictionary can be NULL if the Region constructor crashed
    if (self->dict) {
        // We need to clear the ownership, since this dictionary might be
        // returned to an object pool rather than freed. This would result
        // in an error if the dictionary has the previous region.
        PyRegion_remove_object(self, _PyObject_CAST(self->dict));
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
    if (!self->metadata) {
        PyErr_NoMemory();
        return -1;
    }
    self->metadata->bridge = self;

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "|U", kwlist, &self->metadata->name))
        return -1;
    if (self->metadata->name) {
        Py_XINCREF(self->metadata->name);
        // Freeze the name and it's type. Short strings in Python are interned
        // by default. This means that `id("AB") == id("AB")`. We therefore
        // need to either clone the name object or freeze it to share it
        // across regions. Freezing should be safe, since `+=` and other
        // operators return new strings and keep the old one intact
        //
        // FIXME: Implicit freezing should take care of this instead
        _Py_MakeImmutable(self->metadata->name);
    }

    // Make the region an owner of the bridge object
    Py_SET_REGION(self, self->metadata);
    _Py_MakeImmutable(_PyObject_CAST(Py_TYPE(self)));

    return 0;
}

static int PyRegion_traverse(PyRegionObject *self, visitproc visit, void *arg) {
    Py_VISIT(self->metadata->name);
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

    return add_to_region(args, Py_REGION(self));
}

// Remove args object to self region
static PyObject *PyRegion_remove_object(PyRegionObject *self, PyObject *args) {
    if (!args) {
        Py_RETURN_NONE;
    }

    regionmetadata* md = PyRegion_get_metadata(self);
    if (Py_REGION(args) == (Py_region_ptr_t) md) {
        Py_SET_REGION(args, _Py_LOCAL_REGION);
        Py_RETURN_NONE;
    } else {
        PyErr_SetString(PyExc_RuntimeError, "Object not a member of region!");
        return NULL;
    }
}

// Return True if args object is member of self region
static PyObject *PyRegion_owns_object(PyRegionObject *self, PyObject *args) {
    if (Py_REGION(self) == Py_REGION(args)) {
        Py_RETURN_TRUE;
    } else {
        Py_RETURN_FALSE;
    }
}

static PyObject *PyRegion_repr(PyRegionObject *self) {
    regionmetadata* data = self->metadata;
#ifdef NDEBUG
    // Debug mode: include detailed representation
    return PyUnicode_FromFormat(
        "Region(lrc=%d, osc=%d, name=%S, is_open=%d)",
        data->lrc,
        data->osc,
        self->metadata->name ? self->metadata->name : Py_None,
        data->is_open
    );
#else
    // Normal mode: simple representation
    return PyUnicode_FromFormat(
        "Region(name=%S, is_open=%d)",
        self->metadata->name ? self->metadata->name : Py_None,
        data->is_open
    );
#endif
}

// Define the RegionType with methods
static PyMethodDef PyRegion_methods[] = {
    {"open", (PyCFunction)PyRegion_open, METH_NOARGS, "Open the region."},
    {"close", (PyCFunction)PyRegion_close, METH_NOARGS, "Close the region."},
    {"is_open", (PyCFunction)PyRegion_is_open, METH_NOARGS, "Check if the region is open."},
    // Temporary methods for testing. These will be removed or at least renamed once
    // the write barrier is done.
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

void _PyErr_Region(PyObject *tgt, PyObject *new_ref, const char *msg) {
    const char *new_ref_region_name = get_region_name(new_ref);
    const char *tgt_region_name = get_region_name(tgt);
    PyObject *tgt_type_repr = PyObject_Repr(PyObject_Type(tgt));
    const char *tgt_desc = tgt_type_repr ? PyUnicode_AsUTF8(tgt_type_repr) : "<>";
    PyObject *new_ref_type_repr = PyObject_Repr(PyObject_Type(new_ref));
    const char *new_ref_desc = new_ref_type_repr ? PyUnicode_AsUTF8(new_ref_type_repr) : "<>";
    PyErr_Format(PyExc_RuntimeError, "Error: Invalid edge %p (%s in %s) -> %p (%s in %s) %s\n", tgt, tgt_desc, tgt_region_name, new_ref, new_ref_desc, new_ref_region_name, msg);
}

static const char *get_region_name(PyObject* obj) {
    if (_Py_IsLocal(obj)) {
        return "Default";
    } else if (_Py_IsImmutable(obj)) {
        return "Immutable";
    } else {
        const regionmetadata *md = Py_REGION_DATA(obj);
        return md->name
            ? PyUnicode_AsUTF8(md->name)
            : "<no name>";
    }
}

// TODO replace with write barrier code
bool _Pyrona_AddReference(PyObject *tgt, PyObject *new_ref) {
    if (Py_REGION(tgt) == Py_REGION(new_ref)) {
        // Nothing to do -- intra-region references are always permitted
        return true;
    }

    if (_Py_IsImmutable(new_ref) || _Py_IsCown(new_ref)) {
        // Nothing to do -- adding a ref to an immutable or a cown is always permitted
        return true;
    }

    if (_Py_IsLocal(new_ref)) {
        // Slurp emphemerally owned object into the region of the target object
#ifndef NDEBUG
        _Py_VPYDBG("Added %p --> %p (owner: '%s')\n", tgt, new_ref, get_region_name(tgt));
#endif
        add_to_region(new_ref, Py_REGION(tgt));
        return true;
    }

    _PyErr_Region(tgt, new_ref, "(in WB/add_ref)");
    return false; // Illegal reference
}

// Convenience function for moving multiple references into tgt at once
bool _Pyrona_AddReferences(PyObject *tgt, int new_refc, ...) {
    va_list args;
    va_start(args, new_refc);

    for (int i = 0; i < new_refc; i++) {
        int res = _Pyrona_AddReference(tgt, va_arg(args, PyObject*));
        if (!res) return false;
    }

    va_end(args);
    return true;
}
