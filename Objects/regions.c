
#include "Python.h"
#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "object.h"
#include "regions.h"
#include "pycore_dict.h"
#include "pycore_interp.h"
#include "pycore_object.h"
#include "pycore_regions.h"
#include "pycore_pyerrors.h"
#include "pyerrors.h"

// This tag indicates that the `regionmetadata` object has been merged
// with another region. The `parent` pointer points to the region it was
// merged with.
//
// This tag is only used for the parent pointer in `regionmetadata`.
#define Py_METADATA_MERGE_TAG ((Py_region_ptr_t)0x2)
static inline Py_region_ptr_with_tags_t Py_TAGGED_REGION(PyObject *ob) {
    return ob->ob_region;
}
#define Py_TAGGED_REGION(ob) Py_TAGGED_REGION(_PyObject_CAST(ob))
#define REGION_PRT_HAS_TAG(ptr, tag) ((ptr).value & tag)
#define REGION_PTR_SET_TAG(ptr, tag) (ptr = Py_region_ptr_with_tags((ptr).value | tag))
#define REGION_PTR_CLEAR_TAG(ptr, tag) (ptr = Py_region_ptr_with_tags((ptr).value & (~tag)))

#define REGION_DATA_CAST(r) (_Py_CAST(regionmetadata*, (r)))
#define REGION_PTR_CAST(r) (_Py_CAST(Py_region_ptr_t, (r)))
#define Py_REGION_DATA(ob) (REGION_DATA_CAST(Py_REGION(ob)))
#define Py_REGION_FIELD(ob) (ob->ob_region)

#define IS_IMMUTABLE_REGION(r) (REGION_PTR_CAST(r) == _Py_IMMUTABLE)
#define IS_LOCAL_REGION(r) (REGION_PTR_CAST(r) == _Py_LOCAL_REGION)
#define IS_COWN_REGION(r) (REGION_PTR_CAST(r) == _Py_COWN)
#define HAS_METADATA(r) (!IS_LOCAL_REGION(r) && !IS_IMMUTABLE_REGION(r) && !IS_COWN_REGION(r))

typedef struct regionmetadata regionmetadata;
typedef struct PyRegionObject PyRegionObject;

static regionmetadata* regionmetadata_get_parent(regionmetadata* self);
static PyObject *PyRegion_add_object(PyRegionObject *self, PyObject *args);
static PyObject *PyRegion_remove_object(PyRegionObject *self, PyObject *args);
static const char *get_region_name(PyObject* obj);
static void _PyErr_Region(PyObject *tgt, PyObject *new_ref, const char *msg);

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
#define throw_region_error(src, tgt, format_str, format_arg) \
    throw_region_error(_PyObject_CAST(src), _PyObject_CAST(tgt), \
                       format_str, format_arg)

struct PyRegionObject {
    PyObject_HEAD
    regionmetadata* metadata;
    PyObject *dict;
};

struct regionmetadata {
    // The number of references coming in from the local region.
    Py_ssize_t lrc;
    // The number of open subregions.
    Py_ssize_t osc;
    // The number of references to this object
    Py_ssize_t rc;
    bool is_open;
    // This field might either point to the parent region or another region
    // that this one was merged into. The `Py_METADATA_MERGE_TAG` tag is used
    // to indicate this points to a merged region.
    Py_region_ptr_with_tags_t parent;
    // A weak reference to the bridge object. The bridge object has increased the
    // rc of this metadata object. If this was a strong reference it could create
    // a cycle.
    PyRegionObject* bridge;
    PyObject *name;   // Optional string field for "name"
    // TODO: Currently only used for invariant checking. If it's not used for other things
    // it might make sense to make this conditional in debug builds (or something)
    //
    // Intrinsic list for invariant checking
    regionmetadata* next;
    PyObject* cown; // To be able to release a cown; to be integrated with parent
};

static Py_region_ptr_t regionmetadata_get_merge_tree_root(Py_region_ptr_t self)
{
    // Test for local and immutable region
    if (!HAS_METADATA(self)) {
        return self;
    }

    // Return self if it wasn't merged with another region
    regionmetadata* self_data = REGION_DATA_CAST(self);
    if (!REGION_PRT_HAS_TAG(self_data->parent, Py_METADATA_MERGE_TAG)) {
        return self;
    }

    // FIXME: It can happen that there are several layers in this union-find
    // structure. It would be efficient to directly update the parent pointers
    // for deeper nodes.
    return regionmetadata_get_merge_tree_root(Py_region_ptr(self_data->parent));
}
#define regionmetadata_get_merge_tree_root(self) \
    regionmetadata_get_merge_tree_root(REGION_PTR_CAST(self))

static void regionmetadata_open(regionmetadata* self) {
    assert(HAS_METADATA(self));
    self->is_open = true;
}

static bool regionmetadata_is_open(Py_region_ptr_t self) {
    if (HAS_METADATA(self)) {
        return REGION_DATA_CAST(self)->is_open;
    }

    // The immutable and local region are open by default and can't be closed.
    return true;
}
#define regionmetadata_is_open(self) \
    regionmetadata_is_open(REGION_PTR_CAST(self))

static void regionmetadata_inc_osc(Py_region_ptr_t self_ptr)
{
    if (!HAS_METADATA(self_ptr)) {
        return;
    }

    regionmetadata* self = REGION_DATA_CAST(self_ptr);
    self->osc += 1;
    regionmetadata_open(self);
}
#define regionmetadata_inc_osc(self) \
    (regionmetadata_inc_osc(REGION_PTR_CAST(self)))

static void regionmetadata_dec_osc(Py_region_ptr_t self_ptr)
{
    if (!HAS_METADATA(self_ptr)) {
        return;
    }

    REGION_DATA_CAST(self_ptr)->osc -= 1;
}
#define regionmetadata_dec_osc(self) \
    (regionmetadata_dec_osc(REGION_PTR_CAST(self)))

static void regionmetadata_inc_rc(Py_region_ptr_t self)
{
    if (HAS_METADATA(self)) {
        REGION_DATA_CAST(self)->rc += 1;
    }
}
#define regionmetadata_inc_rc(self) \
    (regionmetadata_inc_rc(REGION_PTR_CAST(self)))

static void regionmetadata_dec_rc(Py_region_ptr_t self_ptr)
{
    if (!HAS_METADATA(self_ptr)) {
        return;
    }

    // Update RC
    regionmetadata* self = REGION_DATA_CAST(self_ptr);
    self->rc -= 1;
    if (self->rc != 0) {
        return;
    }

    // Sort out the funeral by informing everyone about the future freeing
    Py_CLEAR(self->name);

    if (regionmetadata_is_open(self)) {
        regionmetadata_dec_osc(regionmetadata_get_parent(self));
    }

    // This access the parent directly to update the rc.
    // It also doesn't matter if the parent pointer is a
    // merge or subregion relation, since both cases have
    // increased the rc.
    regionmetadata_dec_rc(Py_region_ptr(self->parent));

    free(self);
}
#define regionmetadata_dec_rc(self) \
    (regionmetadata_dec_rc(REGION_PTR_CAST(self)))

static void regionmetadata_set_parent(regionmetadata* self, regionmetadata* parent) {
    // Just a sanity check, since these cases should never happen
    assert(HAS_METADATA(self) && "Can't set the parent on the immutable and local region");
    assert(REGION_PTR_CAST(self) == regionmetadata_get_merge_tree_root(self) && "Sanity Check");
    assert(REGION_PTR_CAST(parent) == regionmetadata_get_merge_tree_root(parent) && "Sanity Check");

    Py_region_ptr_t old_parent = Py_region_ptr(self->parent);
    Py_region_ptr_t new_parent = REGION_PTR_CAST(parent);
    self->parent = Py_region_ptr_with_tags(new_parent);

    // Update RCs
    regionmetadata_inc_rc(new_parent);
    if (regionmetadata_is_open(self)) {
        regionmetadata_inc_osc(new_parent);
        regionmetadata_dec_osc(old_parent);
    }
    regionmetadata_dec_rc(old_parent);
}

static regionmetadata* regionmetadata_get_parent(regionmetadata* self) {
    assert(REGION_PTR_CAST(self) == regionmetadata_get_merge_tree_root(self) && "Sanity check");
    if (!HAS_METADATA(self)) {
        // The local and immutable regions never have a parent
        return NULL;
    }

    Py_region_ptr_t parent_field = Py_region_ptr(self->parent);
    Py_region_ptr_t parent_root = regionmetadata_get_merge_tree_root(parent_field);

    // If the parent was merged with another region we want to update the
    // pointer to point at the root.
    if (parent_field != parent_root) {
        // set_parent ensures that the RC's are correctly updated
        regionmetadata_set_parent(self, REGION_DATA_CAST(parent_root));
    }

    return REGION_DATA_CAST(parent_root);
}
#define regionmetadata_get_parent(self) \
    regionmetadata_get_parent(REGION_DATA_CAST(self))

static bool regionmetadata_has_parent(regionmetadata* self) {
    return regionmetadata_get_parent(self) != NULL;
}

static bool regionmetadata_has_ancestor(regionmetadata* self, regionmetadata* other) {
    // The immutable or local region can never be a parent
    if (!HAS_METADATA(other)) {
        return false;
    }

    while (self) {
        if (self == other) {
            return true;
        }
        self = regionmetadata_get_parent(self);
    }
    return false;
}


// This implementation merges `self` into `other`. Merging is not allowed
// to break external uniqueness. It's therefore not allowed if both regions
// to have a parent. Except cases, where one region has the other region as
// it's parent.
//
// This function expects `self` to be a valid object.
__attribute__((unused))
static PyObject* regionmetadata_merge(regionmetadata* self, Py_region_ptr_t other) {
    assert(HAS_METADATA(self) && "The immutable and local region can't be merged into another region");
    assert(REGION_PTR_CAST(self) == regionmetadata_get_merge_tree_root(self) && "Sanity Check");

    // If `other` is the parent of `self` we can merge it. We unset the the
    // parent which will also update the rc and other counts.
    regionmetadata* self_parent = regionmetadata_get_parent(self);
    if (REGION_PTR_CAST(self_parent) == other) {
        assert(HAS_METADATA(self_parent) && "The immutable and local region can never have children");

        regionmetadata_set_parent(self, NULL);
        self_parent = NULL;
    }

    // If only `self` has a parent we can make `other` the child and
    // remove the parent from `self`. The merged region will then again
    // have the correct parent.
    regionmetadata* other_parent = regionmetadata_get_parent(self);
    if (self_parent && HAS_METADATA(other) && other_parent == NULL) {
        // Make sure we don't create any cycles
        if (regionmetadata_has_ancestor(self_parent, REGION_DATA_CAST(other))) {
            throw_region_error(self->bridge, REGION_DATA_CAST(other)->bridge,
                        "Merging these regions would create a cycle", NULL);
            return NULL;
        }

        regionmetadata_set_parent(REGION_DATA_CAST(other), self_parent);
        regionmetadata_set_parent(self, NULL);
        self_parent = NULL;
    }

    // If `self` still has a parent we can't merge it into `other`
    if (self_parent != NULL) {
        PyObject* other_node = NULL;
        if (HAS_METADATA(other))  {
            other_node = _PyObject_CAST(REGION_DATA_CAST(other)->bridge);
        }
        throw_region_error(self->bridge, other_node,
                        "Unable to merge regions", NULL);
        return NULL;
    }

    regionmetadata_inc_rc(other);

    // Move LRC and OSC into the root.
    if (HAS_METADATA(other)) {
        // Move information into the merge root
        regionmetadata* other_data = REGION_DATA_CAST(other);
        other_data->lrc += self->lrc;
        other_data->osc += self->osc;
        other_data->is_open |= self->is_open;
        // remove information from self
        self->lrc = 0;
        self->osc = 0;
        self->is_open = false;
    }

    self->parent = Py_region_ptr_with_tags(other);
    REGION_PTR_SET_TAG(self->parent, Py_METADATA_MERGE_TAG);
    // No decref, since this is a weak reference. Otherwise we would get
    // a cycle between the `regionmetadata` as a non GC'ed object and the bridge.
    self->bridge = NULL;
    Py_RETURN_NONE;
}
#define regionmetadata_merge(self, other) \
  (regionmetadata_merge(self, REGION_PTR_CAST(other)));

static bool is_bridge_object(PyObject *op) {
    Py_region_ptr_t region = Py_REGION(op);
    // The local and immutable region (represented as NULL) never have a bridge object.
    if (!HAS_METADATA(region)) {
        return false;
    }

    // It's not yet clear how immutability will interact with region objects.
    // It's likely that the object will remain in the object topology but
    // will use the properties of a bridge object. This therefore checks if
    // the object is equal to the regions bridge object rather than checking
    // that the type is `PyRegionObject`
    return _PyObject_CAST(REGION_DATA_CAST(region)->bridge) == op;
}

int _Py_IsLocal(PyObject *op) {
    return IS_LOCAL_REGION(Py_REGION(op));
}

int _Py_IsImmutable(PyObject *op)
{
    return IS_IMMUTABLE_REGION(Py_REGION(op));
}
int _Py_IsCown(PyObject *op)
{
    return Py_REGION(op) == _Py_COWN;
}

Py_region_ptr_t _Py_REGION(PyObject *ob) {
    if (!ob) {
        return REGION_PTR_CAST(NULL);
    }

    Py_region_ptr_t field_value = Py_region_ptr(Py_REGION_FIELD(ob));
    if (!HAS_METADATA(field_value)) {
        return field_value;
    }

    Py_region_ptr_t region = regionmetadata_get_merge_tree_root(field_value);
    // Update the region if we're not pointing to the root of the merge tree.
    // This can allow freeing of non root regions and speedup future lookups.
    if (region != field_value) {
        // We keep the tags, since the owning region stays the same.
        Py_region_ptr_t tags =  Py_region_ptr(Py_REGION_FIELD(ob)) & (~Py_REGION_MASK);
        _Py_SET_TAGGED_REGION(ob, Py_region_ptr_with_tags(region | tags));
    }

    return region;
}

void _Py_SET_TAGGED_REGION(PyObject *ob, Py_region_ptr_with_tags_t region) {
    // Here we access the field directly, since we want to update the RC of the
    // regions we're actually holding and not the root of the merge tree.
    Py_region_ptr_t old_region = Py_region_ptr(Py_REGION_FIELD(ob));

    ob->ob_region = region;

    // Update the RC of the region
    regionmetadata_inc_rc(Py_region_ptr(region));
    regionmetadata_dec_rc(old_region);
}

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
void _Py_notify_regions_in_use(void)
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

/* A traversal callback for _Py_CheckRegionInvariant.
   - tgt is the target of the reference we are checking, and
   - src(_void) is the source of the reference we are checking.
*/
static int
visit_invariant_check(PyObject *tgt, void *src_void)
{
    PyObject *src = _PyObject_CAST(src_void);

    Py_region_ptr_t src_region_ptr = Py_REGION(src);
    Py_region_ptr_t tgt_region_ptr = Py_REGION(tgt);
     // Internal references are always allowed
    if (src_region_ptr == tgt_region_ptr)
        return 0;

    // Anything is allowed to point to immutable
    if (Py_IsImmutable(tgt))
        return 0;
    // Borrowed references are unrestricted
    if (Py_IsLocal(src))
        return 0;
    // Since tgt is not immutable, src also may not be as immutable may not point to mutable
    if (Py_IsImmutable(src)) {
        emit_invariant_error(src, tgt, "Reference from immutable object to mutable target");
        return 0;
    }

    // Cross-region references must be to a bridge
    if (!_Py_is_bridge_object(tgt)) {
        emit_invariant_error(src, tgt, "Reference from object in one region into another region");
        return 0;
    }

    regionmetadata* src_region = REGION_DATA_CAST(src_region_ptr);
    // Region objects may be stored in cowns
    if (IS_COWN_REGION(src_region)) {
        return 0;
    }

    regionmetadata* tgt_region = REGION_DATA_CAST(tgt_region_ptr);
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
            if (Py_IsLocal(op))
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
    if(attr != NULL && !Py_IsImmutable(attr)){ \
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

    if(!Py_IsImmutable(value)){
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
                if(!Py_IsImmutable(value)){
                    _Py_SetImmutable(value);
                }
            }else if(PyDict_Contains(module_dict, name)){
                PyObject* value = PyDict_GetItem(module_dict, name); // value.rc = x

                _PyDict_SetKeyImmutable((PyDictObject*)module_dict, name);

                if(!Py_IsImmutable(value)){
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
            if(!Py_IsImmutable(value)){
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
    if(!Py_IsImmutable(obj)){
        if(stack_push((stack*)frontier, obj)){
            PyErr_NoMemory();
            return -1;
        }
    }

    return 0;
}

PyObject* _Py_MakeImmutable(PyObject* obj)
{
    if (!obj || _Py_IsCown(obj)) {
        Py_RETURN_NONE;
    }

    // We have started using regions, so notify to potentially enable checks.
    _Py_notify_regions_in_use();

    // Some built-in objects are direclty created immutable. However, their types
    // might be created in a mutable state. This therefore requres an additional
    // check to see if the type is also immutable.
    if(Py_IsImmutable(obj) && Py_IsImmutable(Py_TYPE(obj))){
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


        if(Py_IsImmutable(item)){
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
        if (!Py_IsImmutable(type_op)){
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
    if (Py_IsImmutable(target)) {
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

    regionmetadata* source_region = Py_REGION_DATA(info->src);
    if (Py_IsLocal(target)) {
        // Add reference to the object,
        // minus one for the reference we just followed
        source_region->lrc += target->ob_refcnt - 1;
        Py_SET_REGION(target, source_region);

        if (stack_push(info->pending, target)) {
            PyErr_NoMemory();
            return -1;
        }
        return 0;
    }

    // The target was previously in the local region but has already been
    // added to the region by a previous iteration. We therefore only need
    // to adjust the LRC
    if (Py_REGION_DATA(target) == source_region) {
        // -1 for the refernce we just followed
        source_region->lrc -= 1;
        return 0;
    }

    // We push it onto the stack to be added to the region and traversed.
    // The actual addition of the object is done in `add_to_region`. We keep
    // it in the local region, to indicate to `add_to_region` that the object
    // should actually be processed.
    if (Py_IsLocal(target)) {
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
    if (!_Py_is_bridge_object(target)) {
        regionerror err = {.src = info->src, .tgt = target,
                           .id = ERR_CONTAINED_OBJ_REF };
        return ((info->handle_error)(&err, info->handle_error_data));
    }

    // The target is a bridge object from another region. We now need to
    // if it already has a parent.
    regionmetadata *target_region = Py_REGION_DATA(target);
    if (regionmetadata_has_parent(target_region)) {
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
    regionmetadata_set_parent(target_region, region);

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

// Add the transitive closure of objects in the local region reachable from obj to region
static PyObject *add_to_region(PyObject *obj, Py_region_ptr_t region)
{
    if (!obj || _Py_IsCown(obj)) {
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
    assert(HAS_METADATA(region));
    regionmetadata *region_data = _Py_CAST(regionmetadata *, region);

    // Early return if the object is already in the region or immutable
    if (Py_REGION(obj) == region || Py_IsImmutable(obj)) {
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

int _Py_is_bridge_object(PyObject *op) {
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

static void regionmetadata_close(regionmetadata* data) {
    data->is_open = 0;
}

__attribute__((unused))
static void regionmetadata_unparent(regionmetadata* data) {
    regionmetadata_set_parent(data, NULL);
}

__attribute__((unused))
static int regionmetadata_is_root(regionmetadata* data) {
    return regionmetadata_has_parent(data);
}

static regionmetadata* PyRegion_get_metadata(PyRegionObject* obj) {
    return obj->metadata;
}

static void PyRegion_dealloc(PyRegionObject *self) {
    // Name is immutable and not in our region.

    // The object region has already been reset.
    // We now need to update the RC of our metadata field.
    if (self->metadata) {
        regionmetadata* data = self->metadata;
        self->metadata = NULL;
        data->bridge = NULL;
        regionmetadata_dec_rc(data);
    }

    PyTypeObject *tp = Py_TYPE(self);
    PyObject_GC_UnTrack(_PyObject_CAST(self));
    Py_TRASHCAN_BEGIN(self, PyRegion_dealloc);
    if (self->dict) {
        // We need to clear the ownership, since this dictionary might be
        // returned to an object pool rather than freed. This would result
        // in an error if the dictionary has the previous region.
        // TODO: revisit in #16
        Py_SET_REGION(self->dict, _Py_LOCAL_REGION);
        Py_CLEAR(self->dict);
    }

    PyObject_GC_Del(self);
    Py_DECREF(tp);
    Py_TRASHCAN_END
}

static int PyRegion_init(PyRegionObject *self, PyObject *args, PyObject *kwds) {
    // TODO: should not be needed in the future
    _Py_notify_regions_in_use();
    _Py_MakeImmutable(_PyObject_CAST(Py_TYPE(self)));

#ifndef NDEBUG
    fprintf(stderr, "Region created (%p)\n", self);
#endif

    static char *kwlist[] = {"name", NULL};
    self->metadata = (regionmetadata*)calloc(1, sizeof(regionmetadata));
    if (!self->metadata) {
        PyErr_NoMemory();
        return -1;
    }

    // Make sure the internal reference is also counted.
    regionmetadata_inc_rc(self->metadata);

    self->metadata->bridge = self;

    // Make the region an owner of the bridge object
    Py_SET_REGION(self, self->metadata);

    // Freeze the region type to share it with other regions
    _Py_MakeImmutable(_PyObject_CAST(Py_TYPE(self)));

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

    return 0;
}

static int PyRegion_traverse(PyRegionObject *self, visitproc visit, void *arg) {
    Py_VISIT(self->metadata->name);
    Py_VISIT(self->dict);
    return 0;
}

static int PyRegion_clear(PyRegionObject *self) {
    Py_CLEAR(self->metadata->name);
    Py_CLEAR(self->dict);
    return 0;
}

// is_open method (returns True if the region is open, otherwise False)
static PyObject *PyRegion_is_open(PyRegionObject *self, PyObject *args) {
    // FIXME: What is the behavior of a `PyRegionObject` that has been merged into another region?
    if (regionmetadata_is_open(self->metadata)) {
        Py_RETURN_TRUE;  // Return True if the region is open
    } else {
        Py_RETURN_FALSE; // Return False if the region is closed
    }
}

// Open method (sets the region to "open")
static PyObject *PyRegion_open(PyRegionObject *self, PyObject *args) {
    // `Py_REGION()` will fetch the root region of the merge tree.
    // this might be different from the region in `self->metadata`.
    regionmetadata_open(Py_REGION_DATA(self));
    Py_RETURN_NONE;  // Return None (standard for methods with no return value)
}

int _PyRegion_is_closed(PyObject* self) {
    return PyRegion_is_open((PyRegionObject *)self, NULL) == Py_False;
}

// Close method (attempts to set the region to "closed")
// TODO: integrate with #19 and associated PRs
static PyObject *PyRegion_close(PyRegionObject *self, PyObject *args) {
    regionmetadata* const md = REGION_DATA_CAST(Py_REGION(self));
    if (regionmetadata_is_open(md)) {
        regionmetadata_close(md);  // Mark as closed

        // Check if in a cown -- if so, release cown
        if (md->cown) {
            if (_PyCown_release(md->cown) != 0) {
                // Propagate error from release
                return NULL;
            }
        }
        Py_RETURN_NONE; // Return None (standard for methods with no return value)
    } else {
        Py_RETURN_NONE; // Double close is OK
    }
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

    regionmetadata* md = Py_REGION_DATA(self);
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
    regionmetadata* data = Py_REGION_DATA(self);
#ifdef NDEBUG
    // Debug mode: include detailed representation
    return PyUnicode_FromFormat(
        "Region(lrc=%d, osc=%d, name=%S, is_open=%s)",
        data->lrc,
        data->osc,
        data->name ? data->name : Py_None,
        data->is_open ? "yes" : "no"
    );
#else
    // Normal mode: simple representation
    return PyUnicode_FromFormat(
        "Region(name=%S, is_open=%s)",
        data->name ? data->name : Py_None,
        data->is_open ? "yes" : "no"
    );
#endif
}

// Define the RegionType with methods
static PyMethodDef PyRegion_methods[] = {
    {"open", (PyCFunction)PyRegion_open, METH_NOARGS, "Open the region."},
    {"close", (PyCFunction)PyRegion_close, METH_NOARGS, "Attempt to close the region."},
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
    (inquiry)PyRegion_clear,                 /* tp_clear */
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
    } else if (Py_IsImmutable(obj)) {
        return "Immutable";
    } else if (_Py_IsCown(obj)) {
        return "Cown";
    } else {
        const regionmetadata *md = Py_REGION_DATA(obj);
        return md->name
            ? PyUnicode_AsUTF8(md->name)
            : "<no name>";
    }
}

// x.f = y ==> _Pyrona_AddReference(src=x, tgt=y)
bool _Pyrona_AddReference(PyObject *src, PyObject *tgt) {
    if (Py_REGION(src) == Py_REGION(tgt)) {
        // Nothing to do -- intra-region references are always permitted
        return true;
    }

    if (Py_IsImmutable(tgt) || _Py_IsCown(tgt)) {
        // Nothing to do -- adding a ref to an immutable or a cown is always permitted
        return true;
    }

    if (_Py_IsLocal(src)) {
        // Slurp emphemerally owned object into the region of the target object
        // _Py_VPYDBG("Added borrowed ref %p --> %p (owner: '%s')\n", tgt, new_ref, get_region_name(tgt));
        return true;
    }

    // Try slurp emphemerally owned object into the region of the target object
    // _Py_VPYDBG("Added owning ref %p --> %p (owner: '%s')\n", tgt, new_ref, get_region_name(tgt));
    return add_to_region(tgt, Py_REGION(src)) == Py_None;
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

void _PyRegion_set_cown_parent(PyObject* region, PyObject* cown) {
    regionmetadata* md = PyRegion_get_metadata((PyRegionObject*) region);
    Py_XINCREF(cown);
    Py_XSETREF(md->cown, cown);
}
