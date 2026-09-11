
#include "Python.h"
#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include "pycore_descrobject.h"
#include "pycore_gc.h"
#include "pycore_object.h"
#include "pycore_immutability.h"
#include "pycore_interp.h"
#include "pycore_list.h"
#include "pycore_weakref.h"


// This file has many in progress aspects
//
// 1. Improve backtracking of freezing in the presence of failures.
// 2. Support GIL disabled mode properly.
// 3. Improve storage of freeze_location
// 4. Improve Mermaid output to handle re-entrancy
// 5. Add pre-freeze hook to allow custom objects to prepare for freezing.


// #define IMMUTABLE_TRACING

#ifdef IMMUTABLE_TRACING
#define debug(msg, ...) \
   do { \
       printf(msg __VA_OPT__(,) __VA_ARGS__); \
   } while(0)
#define debug_obj(msg, obj, ...) \
   do { \
       PyObject* repr = PyObject_Repr(obj); \
       printf(msg, PyUnicode_AsUTF8(repr), obj __VA_OPT__(,) __VA_ARGS__); \
       Py_DECREF(repr); \
   } while(0)
#else
#define debug(...)
#define debug_obj(...)
#endif

// #define MERMAID_TRACING
#ifdef MERMAID_TRACING
#define TRACE_MERMAID_START() \
    do { \
        FILE* f = fopen("freeze_trace.md", "w"); \
        if (f != NULL) { \
            fprintf(f, "```mermaid\n"); \
            fprintf(f, "graph LR\n"); \
            fclose(f); \
        } \
    } while(0)

#define TRACE_MERMAID_NODE(obj) \
    do { \
        FILE* f = fopen("freeze_trace.md", "a"); \
        if (f != NULL) { \
            fprintf(f, "    %p[\"%s (rc=%zd) - %p\"]\n", \
                (void*)obj, (PyObject*)obj->ob_type->tp_name, \
                Py_REFCNT(obj), (void*)obj); \
            fclose(f); \
        } \
    } while(0)

#define TRACE_MERMAID_EDGE(from, to) \
    do { \
        FILE* f = fopen("freeze_trace.md", "a"); \
        if (f != NULL) { \
            fprintf(f, "    %p --> %p\n", (void*)from, (void*)to); \
            fclose(f); \
        } \
    } while(0)

#define TRACE_MERMAID_END() \
    do { \
        FILE* f = fopen("freeze_trace.md", "a"); \
        if (f != NULL) { \
            fprintf(f, "```\n"); \
            fclose(f); \
        } \
    } while(0)
#else
#define TRACE_MERMAID_START()
#define TRACE_MERMAID_NODE(obj)
#define TRACE_MERMAID_EDGE(from, to)
#define TRACE_MERMAID_END()
#endif

#if SIZEOF_VOID_P > 4
#define IMMUTABLE_FLAG_FIELD(op) (op->ob_flags)
#else
#define IMMUTABLE_FLAG_FIELD(op) (op->ob_refcnt)
#endif

// Macro that jumps to error, if the expression `x` does not succeed.
#define SUCCEEDS(x) { do { int r = (x); if (r != 0) goto error; } while (0); }

static
int init_state(struct _Py_immutability_state *state)
{
    state->warned_types = _Py_hashtable_new(
        _Py_hashtable_hash_ptr,
        _Py_hashtable_compare_direct);
    if(state->warned_types == NULL){
        return -1;
    }

    state->shallow_immutable_types = _Py_hashtable_new(
        _Py_hashtable_hash_ptr,
        _Py_hashtable_compare_direct);
    if(state->shallow_immutable_types == NULL){
        _Py_hashtable_destroy(state->warned_types);
        state->warned_types = NULL;
        return -1;
    }

    // Register built-in shallow immutable types.
    // These types produce objects that are individually immutable
    // but may reference other objects (e.g. tuple elements).
    PyTypeObject *shallow_types[] = {
        &PyTuple_Type,
        &PyFrozenSet_Type,
        &PyCode_Type,
        &PyRange_Type,
        &PyBytes_Type,
        &PyUnicode_Type,
        &PyLong_Type,
        &PyFloat_Type,
        &PyComplex_Type,
        &PyBool_Type,
        &_PyNone_Type,
        &PyEllipsis_Type,
        &_PyNotImplemented_Type,
        &PyCFunction_Type,
        NULL
    };
    for (int i = 0; shallow_types[i] != NULL; i++) {
        if (_PyImmutability_RegisterShallowImmutable(shallow_types[i])) {
            return -1;
        }
    }

    PyTypeObject *builtin_freezable_types[] = {
        &PyType_Type,
        &PyBaseObject_Type,
        &PyFunction_Type,
        &PyList_Type,
        &PyDict_Type,
        &PySet_Type,
        &PyMemoryView_Type,
        &PyByteArray_Type,
        &PyGetSetDescr_Type,
        &PyMemberDescr_Type,
        &PyProperty_Type,
        &PyWrapperDescr_Type,
        &PyMethodDescr_Type,
        &PyClassMethod_Type, // TODO(Immutable): mjp I added this, is it correct? Discuss with maj
        &PyClassMethodDescr_Type,
        &PyStaticMethod_Type,
        &PyMethod_Type,
        &PyCapsule_Type,
        &PyCode_Type,
        &PyCell_Type,
        &PyFrame_Type,
        &_PyWeakref_RefType,
        &PyModule_Type, // TODO(Immutable): mjp I added this, is it correct? Discuss with maj
        &_PyImmModule_Type,
        &PyCFunction_Type,
        &_PyMethodWrapper_Type,
        NULL
    };
    for (int i = 0; builtin_freezable_types[i] != NULL; i++) {
        if (_PyImmutability_SetFreezable((PyObject*)builtin_freezable_types[i], _Py_FREEZABLE_YES)) {
            return -1;
        }
    }

    if (_PyImmutability_SetFreezable((PyObject*)&PyModule_Type, _Py_FREEZABLE_PROXY)) {
        return -1;
    }
    return 0;
}

static struct _Py_immutability_state* get_immutable_state(void)
{
    PyInterpreterState* interp = PyInterpreterState_Get();
    struct _Py_immutability_state *state = &interp->immutability;
    if(state->shallow_immutable_types == NULL){
        if(init_state(state) == -1){
            PyErr_SetString(PyExc_RuntimeError, "Failed to initialize immutability state");
            return NULL;
        }
    }

    return state;
}


static int push(PyObject* s, PyObject* item){
    if(item == NULL){
        return 0;
    }

    if(!PyList_Check(s)){
        PyErr_SetString(PyExc_TypeError, "Expected a list");
        return -1;
    }

    // Don't incref here, so that the algorithm doesn't have to account for the additional counts
    // from the dfs and pending.
    return _PyList_AppendTakeRef(_PyList_CAST(s), item);
}

// Returns a borrowed reference to the last item in the list.
static PyObject* peek(PyObject* s){
    PyObject* item;
    Py_ssize_t size = PyList_Size(s);
    if(size == 0){
        return NULL;
    }

    item = PyList_GetItem(s, size - 1);
    if(item == NULL){
        return NULL;
    }

    return item;
}

// Depend on internal list pop implementation to avoid
// unnecessary refcount operations.
static PyObject* pop(PyObject* s){
    PyObject* item;
    Py_ssize_t size = PyList_Size(s);
    if(size == 0){
        return NULL;
    }

    // The push doesn't incref, so can avoid the extra
    // incref/decref here by using the internal pop.
    item = _Py_ListPop((PyListObject *)s, size - 1);
    if(item == NULL){
        PyErr_SetString(PyExc_RuntimeError, "Internal error: Failed to pop from list");
        return NULL;
    }

    return item;
}



// Artifact[Implementation]: Explanation how a stack is used to implement the DFS based SCC algorithm
/**
 * The DFS walk for SCC calculations needs to perform actions on both
 * the pre-order and post-order visits to an object.  To achieve this
 * with a single stack we use a marker object (PostOrderMarker) to
 * indicate that the object being popped is a post-order visit.
 *
 * Effectively we do
 *   obj = pop()
 *   if obj is PostOrderMarker:
 *      obj = pop()
 *      post_order_action(obj)
 *   else:
 *      push(obj)
 *      push(PostOrderMarker)
 *      pre_order_action(obj)
 *
 * In pre_order_action, the children of obj can be pushed onto the stack,
 * and once all that work is completed, then the PostOrderMarker will pop out
 * and the post_order_action can be performed.
 *
 * Using a separate object means it cannot conflict with anything
 * in the actual python object graph.
 */
PyObject PostOrderMarkerStruct = _PyObject_HEAD_INIT(&_PyNone_Type);
static PyObject* PostOrderMarker = &PostOrderMarkerStruct;

/**
 * `tp_traverse` and `tp_reachable` **should** visit their types but
 * this is sometimes forgotten. To deal with this inconsistency we
 * push the type on to the stack and use this marker to indicate that
 * the type should only be visited if it is not marked as pending when
 * the marker is reached again. We can then manually visit the type
 * and print a warning.
 *
 * If the type is part of an SCC we may end up with a higher SCC-RC
 * since this can only account for one internal edge. But this will
 * just cause a memory leak instead of crashing.
 */
PyObject EnsureVisitedMarkerStruct = _PyObject_HEAD_INIT(&_PyNone_Type);
static PyObject* EnsureVisitedMarker = &EnsureVisitedMarkerStruct;

static bool is_c_wrapper(PyObject* obj){
    return PyCFunction_Check(obj) || Py_IS_TYPE(obj, &_PyMethodWrapper_Type) || Py_IS_TYPE(obj, &PyWrapperDescr_Type);
}

// Artifact[Implementation]: The state used to track a single freeze call and construct SCCs
/**
 * Used to track the state of an in progress freeze operation.
 *
 * TODO(Immutable):  This representation could mostly be done in the
 * GC header for the GIL enabled build.  Doing it externally works for
 * both builds, and we can optimize later.
 **/
struct FreezeState {
#ifndef GIL_DISABLED
    // Used to track traversal order
    PyObject *dfs;
    // Used to track SCC to handle cycles during traversal
    PyObject *pending;
#endif
    // Used to track visited nodes that don't have inline GC state.
    // This is required to be able to backtrack a failed freeze.
    // It is also used to track nodes in GIL_DISABLED builds.
    _Py_hashtable_t *visited;

    // The objects that freeze() was called directly on.
    _Py_hashtable_t *roots;

    // Intrusive linked list of completed SCC representatives
    // (threaded through _gc_prev / scc_parent), for rollback on error.
    // NULL-terminated; NULL means empty.
    PyObject *completed_sccs;

    // A pointer to enclosing freeze states taken from the
    // interpreter local immutable state
    struct FreezeState *enclosing;
    bool restart;
    // Error cleanup state for the item currently being traversed. If traversal
    // fails after add_visited(), discard DFS entries above error_dfs_limit and
    // undo add_visited() for error_visited_item before rolling back older SCCs.
    Py_ssize_t error_dfs_limit;
    PyObject *error_visited_item;
#ifdef Py_DEBUG
    bool traversing;
#endif
#ifdef Py_DEBUG
    // For debugging, track the stack trace of the freeze operation.
    PyObject* freeze_location;
#endif
#ifdef MERMAID_TRACING
    PyObject* start;
#endif
};

// Wrapper around tp_traverse that also visits the type object.
// tp_traverse does not visit the type for non-heap types, but
// tp_reachable should visit all reachable objects including the type.
static int
traverse_via_tp_traverse(PyObject *obj, visitproc visit, void *freeze_state_untyped)
{
    PyTypeObject *tp = Py_TYPE(obj);

    // `tp_traverse` of heap types *should* include a
    // `Py_VISIT(Py_TYPE(self));` since around Python 2.7 but
    // there are still plenty of types that don't. LLMs currently
    // also don't do this consistently. So, instead of visiting the
    // type directly we throw it on to the DFS stack to check the
    // correct behavior on back traversal.
    //
    // Only push the type if it's still mutable and not pending
    if (!_Py_IsImmutable(tp)) {
        struct FreezeState* freeze_state = (struct FreezeState *)freeze_state_untyped;
        SUCCEEDS(push(freeze_state->dfs, _PyObject_CAST(tp)));
        SUCCEEDS(push(freeze_state->dfs, EnsureVisitedMarker));
    }

    traverseproc traverse = tp->tp_traverse;
    if (traverse != NULL) {
        int err = traverse(obj, visit, freeze_state_untyped);
        if (err) {
            return err;
        }
    }

    // Manually visit the type if it's a static type
    if (!(tp->tp_flags & Py_TPFLAGS_HEAPTYPE)) {
        return visit((PyObject *)Py_TYPE(obj), freeze_state_untyped);
    }

    return 0;

error:
    return -1;
}

// Returns the appropriate traversal function for reaching all references
// from an object. Prefers tp_reachable, falls back to tp_traverse wrapped
// to also visit the type. Emits a warning once per type on fallback.
static traverseproc
get_reachable_proc(PyTypeObject *tp)
{
    if (tp->tp_reachable != NULL) {
        return tp->tp_reachable;
    }

    struct _Py_immutability_state *imm_state = get_immutable_state();
    if (imm_state != NULL &&
        _Py_hashtable_get(imm_state->warned_types, (void *)tp) == NULL)
    {
        _Py_hashtable_set(imm_state->warned_types, (void *)tp, (void *)1);
        if (tp->tp_traverse != NULL) {
            PySys_FormatStderr(
                "freeze: type '%.100s' has tp_traverse but no tp_reachable\n",
                tp->tp_name);
        } else {
            PySys_FormatStderr(
                "freeze: type '%.100s' has no tp_traverse and no tp_reachable\n",
                tp->tp_name);
        }
    }

    // Always return the wrapper; even when tp_traverse is NULL, the wrapper
    // will still visit the type object which tp_reachable is expected to do.
    return traverse_via_tp_traverse;
}

#ifdef GIL_DISABLED
static inline void _Py_SetImmutable(PyObject *op)
{
    if(op) {
        IMMUTABLE_FLAG_FIELD(op) |= _Py_IMMUTABLE_FLAG;
    }
}
#endif


#define REPRESENTATIVE_FLAG 1
#define COMPLETE_FLAG 2
#define REFCOUNT_SHIFT 2

/*
    In GIL builds we use the _gc_prev and _gc_next fields to store SCC information:
    - The _gc_prev field stores either the rank of the SCC (if the SCC is a
      representative), or a pointer to the parent representative (if not).
      The Collecting bit on the prev field is used to distinguish between the two.
      We cannot use the finalizer flag as that needs to be preserved.
      We could have a situation where an object is frozen after having a finalizer
      run on it, and we do not want to run the finalizer again.
    - The _gc_next field stores the next object in the cyclic list of objects
      in the SCC.
*/
#define SCC_RANK_FLAG _PyGC_PREV_MASK_COLLECTING

static int
is_root(struct FreezeState *state, PyObject *obj)
{
    return _Py_hashtable_get(state->roots, obj) != NULL;
}

static void deallocate_FreezeState(struct FreezeState *state);

static int init_freeze_state(struct FreezeState *state)
{
    state->dfs = NULL;
    state->pending = NULL;
    state->visited = NULL;
    state->roots = NULL;
    state->completed_sccs = NULL;
    state->enclosing = NULL;
    state->restart = false;
    state->error_dfs_limit = -1;
    state->error_visited_item = NULL;
#ifdef Py_DEBUG
    state->traversing = false;
    state->freeze_location = NULL;
#endif
#ifndef GIL_DISABLED
    state->dfs = PyList_New(0);
    if (state->dfs == NULL) {
        goto error;
    }
    state->pending = PyList_New(0);
    if (state->pending == NULL) {
        goto error;
    }
#endif
    state->visited = _Py_hashtable_new(
        _Py_hashtable_hash_ptr,
        _Py_hashtable_compare_direct);
    if (state->visited == NULL) {
        PyErr_NoMemory();
        goto error;
    }
    state->roots = _Py_hashtable_new(
        _Py_hashtable_hash_ptr,
        _Py_hashtable_compare_direct);
    if (state->roots == NULL) {
        PyErr_NoMemory();
        goto error;
    }

    return 0;

error:
    deallocate_FreezeState(state);
    return -1;
}

static void deallocate_FreezeState(struct FreezeState *state)
{
    if (state->visited != NULL) {
        _Py_hashtable_destroy(state->visited);
        state->visited = NULL;
    }
    if (state->roots != NULL) {
        _Py_hashtable_destroy(state->roots);
        state->roots = NULL;
    }

#ifndef GIL_DISABLED
    // We can't call the destructor directly as we didn't newref the objects
    // on push.  This is a slow path if there are still objects in the stack,
    // so there is no need to optimize it.
    if (state->pending != NULL) {
        while(PyList_Size(state->pending) > 0){
            pop(state->pending);
        }
        Py_DECREF(state->pending);
        state->pending = NULL;
    }
    if (state->dfs != NULL) {
        while(PyList_Size(state->dfs) > 0){
            pop(state->dfs);
        }
        Py_DECREF(state->dfs);
        state->dfs = NULL;
    }
#endif
}

static void set_direct_rc(PyObject* obj)
{
#ifndef GIL_DISABLED
    IMMUTABLE_FLAG_FIELD(obj) = (IMMUTABLE_FLAG_FIELD(obj) & ~_Py_IMMUTABLE_MASK) | _Py_IMMUTABLE_DIRECT;
#else
    (void)obj;
#endif
}

static void set_indirect_rc(PyObject* obj)
{
#ifndef GIL_DISABLED
    IMMUTABLE_FLAG_FIELD(obj) = (IMMUTABLE_FLAG_FIELD(obj) & ~_Py_IMMUTABLE_MASK) | _Py_IMMUTABLE_INDIRECT;
#else
    (void)obj;
#endif
}

static bool has_direct_rc(PyObject* obj)
{
#ifdef GIL_DISABLED
    return false;
#else
    return (IMMUTABLE_FLAG_FIELD(obj) & _Py_IMMUTABLE_MASK) == _Py_IMMUTABLE_DIRECT;
#endif
}


static int is_representative(PyObject* obj, struct FreezeState *state)
{
#ifdef GIL_DISABLED
    void* result = _Py_hashtable_get(state->rep, obj);
    return ((uintptr_t)result & REPRESENTATIVE_FLAG) != 0;
#else
    return (_Py_AS_GC(obj)->_gc_prev & SCC_RANK_FLAG) != 0;
#endif
}

static void set_scc_parent(PyObject* obj, PyObject* parent)
{
    PyGC_Head* gc = _Py_AS_GC(obj);
    // Use GC space for the parent pointer.
    assert(((uintptr_t)parent & ~_PyGC_PREV_MASK) == 0);
    uintptr_t finalized_bit = gc->_gc_prev & _PyGC_PREV_MASK_FINALIZED;
    gc->_gc_prev = finalized_bit | _Py_CAST(uintptr_t, parent);
}

static PyObject* scc_parent(PyObject* obj)
{
    // Use GC space for the parent pointer.
    assert((_Py_AS_GC(obj)->_gc_prev & SCC_RANK_FLAG) == 0);
    return _Py_CAST(PyObject*, _Py_AS_GC(obj)->_gc_prev & _PyGC_PREV_MASK);
}

static void set_scc_rank(PyObject* obj, size_t rank)
{
    // Use GC space for the rank.
    _Py_AS_GC(obj)->_gc_prev = (rank << _PyGC_PREV_SHIFT) | SCC_RANK_FLAG;
}

static size_t scc_rank(PyObject* obj)
{
    assert((_Py_AS_GC(obj)->_gc_prev & SCC_RANK_FLAG) == SCC_RANK_FLAG);
    // Use GC space for the rank.
    return _Py_AS_GC(obj)->_gc_prev >> _PyGC_PREV_SHIFT;
}

static void set_scc_next(PyObject* obj, PyObject* next)
{
    debug("   set_scc_next %p -> %p\n", obj, next);
    // Use GC space for the next pointer.
    _Py_AS_GC(obj)->_gc_next = (uintptr_t)next;
}

static PyObject* scc_next(PyObject* obj)
{
    // Use GC space for the next pointer.
    return _Py_CAST(PyObject*, _Py_AS_GC(obj)->_gc_next);
}

static void scc_init_non_trivial(PyObject* obj)
{
    // Check if this not been part of an SCC yet.
    if (scc_next(obj) == NULL) {
        // Set up a new SCC with a single element.
        set_scc_rank(obj, 0);
        set_scc_next(obj, obj);
    }
}

static void return_to_gc(PyObject* op)
{
    set_scc_next(op, NULL);
    set_scc_parent(op, NULL);
    // Use internal version as we don't satisfy all the invariants,
    // as we call this on state we are tearing down in SCC reclaiming.
    //    PyObject_GC_Track(op);
    _PyObject_GC_TRACK(op);
}

static void scc_init(PyObject* obj)
{
    assert(_PyObject_IS_GC(obj));
    // Let the Immutable GC take over tracking the lifetime
    // of this object. This releases the space for the SCC
    // algorithm.
    if (_PyObject_GC_IS_TRACKED(obj)) {
        _PyObject_GC_UNTRACK(obj);
    }

    // The GC uses the collecting flag to identify objects part of the
    // current collection set. This flag remains while the finalizer
    // of unreachable objects is being called.
    //
    // If something calls `freeze(obj)` as part of their finalizer we
    // might receive an object with the flag set. This removes the flag
    // to prevent future GC collections to assume this object is currently
    // being collected.
    _PyGC_CLEAR_COLLECTING(obj);

    // Mark as pending so we can detect back edges in the traversal.

    IMMUTABLE_FLAG_FIELD(obj) |= _Py_IMMUTABLE_PENDING;
    set_scc_rank(obj, 0);
}

static bool scc_is_pending(PyObject* obj)
{
    return (IMMUTABLE_FLAG_FIELD(obj) & _Py_IMMUTABLE_MASK) == _Py_IMMUTABLE_PENDING;
}

static PyObject* get_representative(PyObject* obj, struct FreezeState *state)
{
    if (is_representative(obj, state)) {
        return obj;
    }
    // Grandparent path compression for union find.
    PyObject* grandparent = obj;
    PyObject* rep = scc_parent(obj);
    while (1) {
        if (is_representative(rep, state)) {
            break;
        }

        PyObject* parent = rep;
        rep = scc_parent(rep);
        set_scc_parent(grandparent, rep);
        grandparent = parent;
    }
    return rep;
}

static bool
union_scc(PyObject* a, PyObject* b, struct FreezeState *state)
{
    // Initialize SCC information for both objects.
    // If they are already in an SCC, this is a no-op.
    scc_init_non_trivial(a);
    scc_init_non_trivial(b);

    // TODO(Immutable): use rank and merge in correct direction.
    PyObject* rep_a = get_representative(a, state);
    PyObject* rep_b = get_representative(b, state);

    if (rep_a == rep_b)
        return false;

    // Determine rank, and switch so that rep_a has higher rank.
    size_t rank_a = scc_rank(rep_a);
    size_t rank_b = scc_rank(rep_b);
    if (rank_a < rank_b) {
        PyObject* temp = rep_a;
        rep_a = rep_b;
        rep_b = temp;
    } else if (rank_a == rank_b) {
        // Increase rank of new representative.
        set_scc_rank(rep_a, rank_a + 1);
    }

    set_scc_parent(rep_b, rep_a);

    // Merge the cyclic lists.
    PyObject* next_a = scc_next(rep_a);
    PyObject* next_b = scc_next(rep_b);
    set_scc_next(rep_a, next_b);
    set_scc_next(rep_b, next_a);
    return true;
}

static PyObject* get_next(PyObject* obj, struct FreezeState *freeze_state)
{
    (void)freeze_state;
    PyObject* next = scc_next(obj);
    return next;
}

static int has_visited(struct FreezeState *state, PyObject* obj)
{
#ifdef GIL_DISABLED
    return _Py_hashtable_get(state->visited, obj) != NULL;
#else
    return _Py_IsImmutable(obj);
#endif
}

#ifndef GIL_DISABLED
static PyObject* scc_root(PyObject* obj)
{
    assert(_Py_IsImmutable(obj));
    if (has_direct_rc(obj))
        return obj;

    // If the object is pending, then it is still being explored,
    // the final pass of the SCC algorithm will calculate the whole SCCs RC,
    // apply the ref count directly so we don't accidentally delete an object
    // that is still being explored.
    if (scc_is_pending(obj))
        return obj;

    PyObject* parent = scc_parent(obj);
    if (parent != NULL)
        return parent;

    assert(get_next(obj, NULL) == NULL);
    return obj;
}
#endif

// During the freeze, we removed the reference counts associated
// with the internal edges of the SCC.  This visitor detects these
// internal edges and re-adds the reference counts to the
// objects in the SCC.
static int scc_add_internal_refcount_visit(PyObject* obj, void* curr_root)
{
    if (obj == NULL)
        return 0;

    // Ignore mutable outgoing edges.
    if (!_Py_IsImmutable(obj))
        return 0;

    // Find the scc root.
    PyObject* root = scc_root(obj);

    // If it is different SCC, then we can ignore it.
    if (root != curr_root)
        return 0;

    // Increase the reference count as we found an interior edge for the SCC.
    debug_obj("Reinstate %s (%p) with rc %zu from %p\n", obj, Py_REFCNT(obj), curr_root);
    obj->ob_refcnt++;

    return 0;
}

struct SCCDetails {
    int has_weakreferences;
    int has_legacy_finalizers;
    int has_finalizers;
};

static void scc_set_refcounts_to_one(PyObject* obj)
{
    PyObject* n = obj;
    do {
        PyObject* c = n;
        n = scc_next(c);
        c->ob_refcnt = 1;
    } while (n != obj);
}

// This will restore the reference counts for the interior edges of the SCC.
// It calculates some properties of the SCC, to decide how it might be
// finalised.  Adds an RC to every element in the SCC.
static void scc_add_internal_refcounts(PyObject* obj, struct SCCDetails* details)
{
    assert(_Py_IsImmutable(obj));
    PyObject* root = scc_root(obj);

    details->has_weakreferences = 0;
    details->has_legacy_finalizers = 0;
    details->has_finalizers = 0;

    // Add back the reference counts for the interior edges.
    PyObject* n = obj;
    do {
        debug_obj("Unfreezing %s @ %p\n", n);
        PyObject* c = n;
        n = scc_next(c);
        //  WARNING
        //  CHANGES HERE NEED TO BE REFLECTED IN freeze_visit

        get_reachable_proc(Py_TYPE(c))(c, (visitproc)scc_add_internal_refcount_visit, root);

        if (PyWeakref_Check(c)) {
            // We followed weakreferences during freeze, so need to here as well.
            PyObject* wr = NULL;
            PyWeakref_GetRef(c, &wr);
            if (wr != NULL) {
                // This will increment the reference if it is in the same SCC
                // and do nothing otherwise.  We are treating the weakref as
                // a strong reference for the immutable state.
                scc_add_internal_refcount_visit(wr, root);
                Py_DECREF(wr);
            }
            details->has_weakreferences++;
        }

        if (Py_TYPE(c)->tp_del != NULL)
            details->has_legacy_finalizers++;
        if (Py_TYPE(c)->tp_finalize != NULL && !_PyGC_FINALIZED(c))
            details->has_finalizers++;
        if (_PyType_SUPPORTS_WEAKREFS(Py_TYPE(c)) &&
            *_PyObject_GET_WEAKREFS_LISTPTR_FROM_OFFSET(c) != NULL) {
            details->has_weakreferences++;
        }
    } while (n != obj);
}


// This takes an SCC and turns it back to mutable.
// Must be called after a call to
// scc_add_internal_refcount, so that the reference counts are correct.
static void scc_make_mutable(PyObject* obj)
{
    PyObject* n = obj;
    do {
        PyObject* c = n;
        n = scc_next(c);
        _Py_CLEAR_IMMUTABLE(c);
        if (PyWeakref_Check(c)) {
            PyObject* wr = NULL;
            PyWeakref_GetRef(c, &wr);
            if (wr != NULL) {
                // Turn back to weak reference. We made the weak references strong during freeze.
                Py_DECREF(wr);
                Py_DECREF(wr);
            }
        }
    } while (n != obj);
}

// Returns all the objects in the SCC to the Python cycle detector.
static void scc_return_to_gc(PyObject* obj, bool decref_required)
{
    PyObject* n = obj;
    do {
        PyObject* c = n;
        n = scc_next(c);
        return_to_gc(c);
        debug("Returned %p rc = %zu to GC\n", c, Py_REFCNT(c));
        if (decref_required) {
            Py_DECREF(c);
        }
    } while (n != obj);
}

static void undo_weakref_freeze_reference(PyObject *obj)
{
    if (!PyWeakref_Check(obj)) {
        return;
    }
    PyObject *wr = NULL;
    PyWeakref_GetRef(obj, &wr);
    if (wr != NULL) {
        // Drop the temporary reference and the strong reference retained
        // when traverse_freeze() followed the weak reference.
        Py_DECREF(wr);
        Py_DECREF(wr);
    }
}

// Copy-pasted from weakrefobject.c
static void weakref_handle_callback(PyWeakReference* ref, PyObject* callback)
{
    PyObject* cbresult = PyObject_CallOneArg(callback, (PyObject*)ref);

    if (cbresult == NULL) {
        PyErr_FormatUnraisable("Exception ignored while "
                               "calling weakref callback %R", callback);
    }
    else {
        Py_DECREF(cbresult);
    }
}

// Copy-pasted from weakrefobject.c
static void weakref_insert_head(PyWeakReference* newref, PyWeakReference** list)
{
    PyWeakReference* next = *list;

    newref->wr_prev = NULL;
    newref->wr_next = next;
    if (next != NULL)
        next->wr_prev = newref;
    *list = newref;
}

static void weakref_remove(PyWeakReference* self, PyWeakReference** list)
{
    if (*list == self) {
        *list = self->wr_next;
    }
    if (self->wr_prev != NULL) {
        self->wr_prev->wr_next = self->wr_next;
    }
    if (self->wr_next != NULL) {
        self->wr_next->wr_prev = self->wr_prev;
    }
    self->wr_prev = NULL;
    self->wr_next = NULL;
}

static void weakref_decref_weakrefs(PyWeakReference* head)
{
    while (head != NULL) {
        PyWeakReference* weakref = head;
        head = weakref->wr_next;
        weakref->wr_next = NULL;
        weakref->wr_prev = NULL;
        Py_DECREF(weakref);
    }
}

typedef struct {
    int32_t interpreters_remaining;
    PyObject* to_dealloc;
} callback_progress;

typedef struct {
    PyWeakReference* head;
    callback_progress* progress;
} pending_callbacks;

/* Signal that the current interpreter handled the callbacks.
 * If all interpreters have handled the callbacks, deallocate the object.
 */
static void weakref_signal_handled(callback_progress* progress)
{
    int32_t old = _Py_atomic_add_int32(
        &progress->interpreters_remaining, -1);
    if (old == 1) {
        // All callbacks handled, trigger deallocation again.
        Py_INCREF(progress->to_dealloc);
        Py_DECREF(progress->to_dealloc);
        PyMem_Free(progress);
    }
}

/* Call the pending callbacks.
 * This function can be executed asynchronously using Py_AddPendingCall.
 */
static int weakref_call_callbacks(void* arg)
{
    pending_callbacks* pending = (pending_callbacks*)arg;
    PyWeakReference* head = pending->head;
    debug("Interpreter %zd handling callbacks for dying object %p\n",
        PyInterpreterState_GetID(PyInterpreterState_Get()),
        pending->progress->to_dealloc);

    while (head != NULL) {
        PyWeakReference* weakref = head;
        PyObject* callback = weakref->wr_callback;
        assert(callback != NULL);
        weakref->wr_callback = NULL;
        weakref_handle_callback(weakref, callback);
        Py_DECREF(callback);
        head = weakref->wr_next;
        weakref->wr_next = NULL;
        weakref->wr_prev = NULL;
        Py_DECREF(weakref);
    }

    weakref_signal_handled(pending->progress);
    PyMem_Free(pending);
    // Report success as per Py_AddPendingCall contract
    return 0;
}

/* Schedule the callbacks on the given interpreter. */
static void weakref_schedule_callbacks(int64_t ipid, pending_callbacks* pending)
{
    // FIXME(Immutable): Can the interpreter go away in the middle of scheduling?
    PyInterpreterState* target_is = _PyInterpreterState_LookUpID(ipid);
    if (target_is == NULL) {
        // Interpreter is already gone.
        goto abort;
    }
    // We just need to get any thread state to schedule the call.
    PyThreadState* tstate_target = PyInterpreterState_ThreadHead(target_is);
    if (tstate_target == NULL) {
        goto abort;
    }
    PyThreadState* tstate_old = PyThreadState_Swap(tstate_target);
    int schedule_res = Py_AddPendingCall(weakref_call_callbacks, (void*)pending);
    PyThreadState_Swap(tstate_old);
    if (schedule_res != 0) {
        goto abort;
    }
    return;

abort:
    PyMem_Free(pending);
    weakref_decref_weakrefs(pending->head);
    return;
}

/* Remove callbacks with the given ipid from the list.
 * Return them as a new list.
 */
static PyWeakReference* weakref_separate_ipid(PyWeakReference** list, int64_t ipid)
{
    PyWeakReference* result = NULL;
    PyWeakReference* next = *list;
    while (next != NULL) {
        PyWeakReference* current = next;
        next = next->wr_next;
        if (current->callback_ipid == ipid) {
            weakref_remove(current, list);
            weakref_insert_head(current, &result);
        }
    }
    return result;
}

/* Distribute the callbacks to their original interpreters.
 * Returns:
 * (true) The caller can proceed with deallocating 'to_dealloc'.
 * (false) Callbacks were scheduled, deallocation will be triggered again.
 */
static int weakref_distribute_callbacks(PyWeakReference* head, PyObject* to_dealloc)
{
    if (head == NULL) {
        return true;
    }

    debug("Clearing weakrefs of %p.\n", to_dealloc);
    // We want to continue with deallocation after calling all the callbacks.
    callback_progress* progress = PyMem_Malloc(sizeof(callback_progress));
    if (progress == NULL) {
        // Give up calling callbacks.
        weakref_decref_weakrefs(head);
        return true;
    }
    // Start with 1, decremented at the end of this function.
    // This way, we prevent hitting zero before all callbacks are scheduled.
    progress->interpreters_remaining = 1;
    progress->to_dealloc = to_dealloc;

    // Schedule the callbacks on their original interpreters.
    while (head != NULL) {
        int64_t ipid = head->callback_ipid;
        PyWeakReference* ip_callbacks = weakref_separate_ipid(&head, ipid);
        // Create a data structure to hold arguments for the async call.
        pending_callbacks* pending = PyMem_Malloc(sizeof(pending_callbacks));
        if (pending == NULL) {
            // Give up calling callbacks.
            weakref_decref_weakrefs(ip_callbacks);
            continue;
        }

        _Py_atomic_add_int32(&progress->interpreters_remaining, 1);
        pending->head = ip_callbacks;
        pending->progress = progress;
        if (PyInterpreterState_GetID(PyInterpreterState_Get()) == ipid) {
            // We can run the callback here.
            weakref_call_callbacks((void*)pending);
        }
        else {
            // We need to schedule the callback on the target interpreter.
            weakref_schedule_callbacks(ipid, pending);
        }
    }

    weakref_signal_handled(progress);
    return false;
}

/* Clear weakrefs with callbacks for an SCC, and call them.
 * Returns:
 * (true) Deallocation can continue.
 * (false) Callbacks were scheduled, deallocation will be triggered again.
 */
static int weakref_handle_callbacks_scc(PyObject* obj)
{
    // Collect weakrefs with callbacks into a list.
    PyWeakReference* head = NULL;
    PyObject* n = obj;
    do {
        PyObject* c = n;
        n = scc_next(c);
        if (_PyType_SUPPORTS_WEAKREFS(Py_TYPE(c))) {
            _PyImmutability_ClearWeakRefsWithCallback(c, &head);
        }
    } while (n != obj);

    return weakref_distribute_callbacks(head, obj);
}

/* Clear weakrefs with callbacks for a single object, and call them.
 * Returns:
 * (true) Deallocation can continue.
 * (false) Callbacks were scheduled, deallocation will be triggered again.
 */
static int weakref_handle_callbacks_single(PyObject* obj)
{
    if (!_PyType_SUPPORTS_WEAKREFS(Py_TYPE(obj))) {
        return true;
    }
    // Collect weakrefs with callbacks into a list.
    PyWeakReference* head = NULL;
    _PyImmutability_ClearWeakRefsWithCallback(obj, &head);
    return weakref_distribute_callbacks(head, obj);
}

static void unfreeze_and_finalize_scc(PyObject* obj)
{
    struct SCCDetails scc_details;
    debug_obj("Unfreezing and finalizing SCC starting at %s @ %p rc = %zd\n", obj, Py_REFCNT(obj));

    scc_set_refcounts_to_one(obj);
    scc_add_internal_refcounts(obj, &scc_details);

    // We don't handle legacy finalizers.
    // Return the state as mutable to the cycle detector to handle.
    if (scc_details.has_legacy_finalizers > 0) {
        debug("There are legacy finalizers in the SCC.  Let cycle detector handle this case.\n");
        debug("Legacy finalizers: %d\n", scc_details.has_legacy_finalizers);
        scc_make_mutable(obj);
        scc_return_to_gc(obj, true);
        return;
    }

    // But leave cyclic list in place for the SCC.
    scc_make_mutable(obj);

    PyObject* n = obj;
    if (scc_details.has_finalizers) {
        // Call the finalizers for all objects in the SCC.
        do {
            PyObject* c = n;
            n = scc_next(c);
            if (_PyGC_FINALIZED(c))
                continue;
            destructor finalize = Py_TYPE(c)->tp_finalize;
            if (finalize == NULL)
                continue;
            // Call the finalizer for the object.
            finalize(c);
            // Mark so we don't finalize it again.
            _PyGC_SET_FINALIZED(c);
        } while (n != obj);
    }

    if (scc_details.has_weakreferences) {
        // Clear the remaining weakrefs without calling callbacks.
        n = obj;
        do {
            PyObject* c = n;
            n = scc_next(c);
            if (_PyType_SUPPORTS_WEAKREFS(Py_TYPE(c))) {
                _PyWeakref_ClearWeakRefsNoCallbacks(c);
            }
        } while (n != obj);
    }

    // tp_clear all elements in the cycle.
    n = obj;
    do {
        debug("Clearing (%p)\n", n);
        PyObject* c = n;
        n = scc_next(c);
        inquiry clear;
        if ((clear = Py_TYPE(c)->tp_clear) != NULL) {
            clear(c);
            // TODO(Immutable): Should do something with the error? e.g.
            // if (_PyErr_Occurred(tstate)) {
            //     _PyErr_WriteUnraisableMsg("in tp_clear of",
            //                             (PyObject*)Py_TYPE(op));
            // }
        }
    } while (n != obj);
    // Return objects to the GC state, and drop reference counts on all the
    // elements of the SCC so that they can be reclaimed
    scc_return_to_gc(obj, true);
}

/*
  When we first visit an object, we create a partial SCC for it,
  this involves:
    * Using the next table, to add it to a cyclic list for its SCC, initially just itself
    * Adding an entry in the representative table marking it as a representative
      that is pending (not complete) with refcount equal to its current refcount.

  Returns -1 if there was a memory error.
  Otherwise returns 0.
*/
static int add_visited(PyObject* obj, struct FreezeState *state)
{
    assert (!has_visited(state, obj));

#ifdef Py_DEBUG
    // TODO(Immutable): Re-enable this code.
    // // We need to add this attribute before traversing, so that if it creates a
    // // dictionary, then this dictionary is frozen.
    // if (state->freeze_location != NULL) {
    //     // Some objects don't have attributes that can be set.
    //     // As this is a Debug only feature, we could potentially increase the object
    //     // size to allow this to be stored directly on the object.
    //     if (PyObject_SetAttrString(obj, "__freeze_location__", state->freeze_location) < 0) {
    //         // Ignore failure to set _freeze_location
    //         PyErr_Clear();
    //         // We still want to freeze the object, so we continue
    //     }
    // }
#endif
#ifdef GIL_DISABLED
    // TODO(Immutable): Need to mark as immutable but not deeply immutable here.
#else
    debug_obj("Adding visited  %s (%p)\n", obj);
    if (_PyObject_IS_GC(obj))
    {
        scc_init(obj);
        return 0;
    } else {
        set_direct_rc(obj);
    }
#endif
    if (_Py_hashtable_set(state->visited, obj, obj) == -1) {
#ifndef GIL_DISABLED
        // This clears the effects of set_direct_rc.
        _Py_CLEAR_IMMUTABLE(obj);
#endif
        return -1;
    }
    return 0;

}

/*
    Returns true if the object is part of an SCC that is still pending (not complete).
*/
static int
is_pending(PyObject* obj, struct FreezeState *state)
{
    return scc_is_pending(obj);
}

/*
    Marks the SCC for the given object as complete.

    Decrements the reference count for the SCC by one, corresponding to
    removing the reference from the edge that initially entered this
    SCC.

    Returns true if the SCC's reference count has become zero.
*/
static void
complete_scc(PyObject* obj, struct FreezeState *state)
{
    PyObject* c = scc_next(obj);
    if (c == NULL) {
        debug_obj("Completing SCC %s (%p) with single member rc = %zd\n", obj, Py_REFCNT(obj));
        // This is not part of a cycle, just make it immutable.
        // Link into the completed SCCs list for rollback.
        set_scc_parent(obj, state->completed_sccs);
        state->completed_sccs = obj;
        set_direct_rc(obj);
        return;
    }
    size_t rc = Py_REFCNT(obj);
    size_t count = 1;
    while (c != obj)
    {
        debug("Adding %p to SCC %p\n", c, obj);
        rc += Py_REFCNT(c);
        // Set refcnt to zero, and mark as immutable indirect.
        set_indirect_rc(c);
        set_scc_parent(c, obj);
        c = scc_next(c);
        count++;
    }
    // We will have left an RC live for each element in the SCC, so
    // we need to remove that from the SCCs refcount.
    obj->ob_refcnt = rc - (count - 1);
    set_direct_rc(obj);
    // Link into the completed SCCs list for rollback.
    set_scc_parent(obj, state->completed_sccs);
    state->completed_sccs = obj;
    debug_obj("Completed SCC %s (%p) with %zu members with rc %zu \n", obj, count, rc - (count - 1));
}

static void add_internal_reference(PyObject* obj, struct FreezeState *state)
{
    obj->ob_refcnt--;
    debug_obj("Decrementing rc of %s (%p) to %zd\n", obj, _Py_REFCNT(obj));
    assert(_Py_REFCNT(obj) > 0);
}

// Dispatch for a postorder marker.  complete_scc() does the actual
// refcount conversion when the marker belongs to the current representative.
static void
finish_scc_at_postorder(PyObject *item, struct FreezeState *state)
{
    PyObject* current_scc = peek(state->pending);
    if (item == current_scc)
    {
        debug("Completed an SCC\n");
        pop(state->pending);
        debug_obj("Representative: %s (%p)\n", item);

        complete_scc(item, state);
    }
}

// Handle an edge to an object that is already pending: merge the active SCC
// path as needed and subtract that internal edge from the target refcount.
static void
process_pending_internal_edge(PyObject *item, struct FreezeState *state)
{
    PyObject *current_scc = peek(state->pending);
    if (current_scc == NULL) {
        Py_FatalError("freeze: pending object without pending SCC");
    }
    while (union_scc(current_scc, item, state)) {
        debug_obj("Representative: %s (%p)\n", current_scc);
        pop(state->pending);
        current_scc = peek(state->pending);
        if (current_scc == NULL) {
            Py_FatalError("freeze: SCC union emptied pending stack");
        }
    }
    add_internal_reference(item, state);
}

/*
  Visitor for undo_completed_scc walk 2.
  Re-adds internal reference counts that were subtracted by
  add_internal_reference during the freeze traversal.
  The arg is a _Py_hashtable_t* of ring members.
*/
static int rollback_refcount_visit(PyObject* obj, void* ring_ht)
{
    if (obj == NULL)
        return 0;

    // Only increment for edges pointing to objects in this SCC ring.
    if (_Py_hashtable_get((_Py_hashtable_t*)ring_ht, obj) == NULL)
        return 0;

    obj->ob_refcnt++;
    return 0;
}

/*
  Reverse the effects of complete_scc on a multi-member SCC,
  restoring each object's original reference count.

  Three walks of the ring:
    Walk 1: Build ring membership hashtable + compute root's external RC
    Walk 2: Re-add internal reference counts via tp_reachable
    Walk 3: Clear immutability flags + return objects to GC
*/
static void undo_completed_scc(PyObject* obj)
{
    debug("Rolling back SCC starting at %p\n", obj);

    _Py_hashtable_t *ring = _Py_hashtable_new(
        _Py_hashtable_hash_ptr,
        _Py_hashtable_compare_direct);

    // Walk 1: Build ring membership set and compute root's external RC.
    //
    // After complete_scc:
    //   root.ob_refcnt = total_ext - (count - 1)
    //   Mi.ob_refcnt = external_refs_to_Mi (preserved by set_indirect_rc)
    //
    // We subtract each non-root member's RC from root and add (count-1)
    // to recover: root.ob_refcnt = external_refs_to_root
    size_t count = 0;
    PyObject* n = obj;
    do {
        PyObject* c = n;
        n = scc_next(c);
        _Py_hashtable_set(ring, c, c);
        if (c != obj) {
            obj->ob_refcnt -= _Py_REFCNT(c);
            // Non-root members still carry their DFS discovery edge.
            // Walk 2 re-adds all internal edges, including that one.
            c->ob_refcnt--;
        }
        count++;
    } while (n != obj);
    obj->ob_refcnt += (count - 1);

    // Walk 2: Re-add internal reference counts.
    // For each edge X->Y where Y is in the ring, increment Y.ob_refcnt.
    // This reverses the add_internal_reference decrements from freeze.
    n = obj;
    do {
        PyObject* c = n;
        n = scc_next(c);
        get_reachable_proc(Py_TYPE(c))(c, (visitproc)rollback_refcount_visit, ring);

        // Handle weak references the same way as scc_add_internal_refcounts
        // TODO(Immutable): David and Fred this change will impact you weak reference work.
        if (PyWeakref_Check(c)) {
            PyObject* wr = NULL;
            PyWeakref_GetRef(c, &wr);
            if (wr != NULL) {
                rollback_refcount_visit(wr, ring);
                Py_DECREF(wr);
            }
        }
    } while (n != obj);

    _Py_hashtable_destroy(ring);

    // Walk 3: Clear immutability flags and return to GC.
    n = obj;
    do {
        PyObject* c = n;
        n = scc_next(c);
        undo_weakref_freeze_reference(c);
        _Py_CLEAR_IMMUTABLE(c);
        return_to_gc(c);  // clears scc_next and scc_parent, re-tracks in GC
    } while (n != obj);
}

/*
  Callback for _Py_hashtable_foreach to clear immutability flags
  on visited objects (handles non-GC objects not in any SCC ring).
*/
static int clear_immutable_visitor(
    _Py_hashtable_t* tbl, const void* key, const void* value, void* unused)
{
    (void)tbl;
    (void)value;
    (void)unused;
    _Py_CLEAR_IMMUTABLE((PyObject*)key);
    return 0;
}

#ifdef GIL_DISABLED
/*
  Function for use in _Py_hashtable_foreach.
  Marks the key as immutable/frozen.
*/
static int mark_frozen(_Py_hashtable_t* tbl, const void* key, const void* value, void* state)
{
    (void)tbl;
    (void)value;
    (void)state;
    // Mark as frozen, this can only reach immutable objects so safe.
    _Py_SetImmutable((PyObject*)key);
    return 0;
}
#endif

/*
  Marks all the objects visited by the freeze operation as frozen.
*/
static void mark_all_frozen(struct FreezeState *state)
{
#ifdef GIL_DISABLED
    _Py_hashtable_foreach(state->visited, mark_frozen, state);
#endif
}

static int freeze_visit(PyObject* obj, void* freeze_state_untyped)
{
    struct FreezeState* freeze_state = (struct FreezeState *)freeze_state_untyped;
    PyObject* dfs = freeze_state->dfs;
    if (obj == NULL) {
        return 0;
    }

    if (_Py_IsImmutable(obj) && !is_pending(obj, NULL)) {
        return 0;
    }

    debug_obj("-> %s (%p) rc=%zu\n", obj, Py_REFCNT(obj));

    TRACE_MERMAID_EDGE(freeze_state->start, obj);

    if(push(dfs, obj)){
        PyErr_NoMemory();
        return -1;
    }

    return 0;
}

static int check_freezable(struct _Py_immutability_state *state, PyObject* obj,
                           struct FreezeState *freeze_state)
{
    debug_obj("check_freezable  %s (%p)\n", obj);

    // Check per-object freezable status set via set_freezable().
    int obj_status = _PyImmutability_GetFreezable(obj);
    if (obj_status >= 0) {
        switch (obj_status) {
        case _Py_FREEZABLE_YES:
            return 0;
        case _Py_FREEZABLE_NO:
            goto error;
        case _Py_FREEZABLE_EXPLICIT:
            if (freeze_state != NULL && is_root(freeze_state, obj)) {
                return 0;
            }
            goto error;
        case _Py_FREEZABLE_PROXY:
            assert(PyModule_Check(obj) || obj == _PyObject_CAST(&PyModule_Type));
            return 0;
        }
    }

    // TODO(Immutable): Visit what the right balance of making Python types immutable is.
    if(!_PyType_HasExtensionSlots(obj->ob_type)){
        return 0;
    }

error:
    debug_obj("Not freezable  %s (%p)\n", obj);
    PyObject* error_msg = PyUnicode_FromFormat(
        "Cannot freeze instance of type %s",
        (obj->ob_type->tp_name));
    PyErr_SetObject(PyExc_TypeError, error_msg);
    Py_DECREF(error_msg);
    return -1;
}


int _PyImmutability_SetFreezable(PyObject *obj, _Py_freezable_status status)
{
    if (status < _Py_FREEZABLE_YES || status > _Py_FREEZABLE_PROXY) {
        PyErr_Format(PyExc_ValueError,
                     "Invalid freezable status: %d", status);
        return -1;
    }

    if (status == _Py_FREEZABLE_PROXY
        && !(PyModule_Check(obj) || obj == _PyObject_CAST(&PyModule_Type))
    ) {
        PyErr_SetString(PyExc_TypeError,
                        "FREEZABLE_PROXY can only be set on module objects");
        return -1;
    }

    // Try setting __freezable__ attribute on the object.
    PyObject *value = PyLong_FromLong(status);
    if (value == NULL) {
        return -1;
    }

    int rc = PyObject_SetAttr(obj, &_Py_ID(__freezable__), value);
    Py_DECREF(value);
    if (rc == 0) {
        return 0;
    }

    // If setting the attribute failed, only fall back to ob_flags for
    // "attribute not supported / read-only" cases. Propagate all other
    // exceptions to the caller.
    if (PyErr_ExceptionMatches(PyExc_AttributeError) ||
        PyErr_ExceptionMatches(PyExc_TypeError))
    {
        PyErr_Clear();
    }
    else {
        // Preserve the original error (e.g. MemoryError or a custom
        // tp_setattro exception).
        return -1;
    }

    // If the object doesn't support attribute setting, fall back
    // to ob_flags (64-bit only).
#if SIZEOF_VOID_P > 4
    // Store the freezable status in ob_flags bits 5-7.
    uint16_t flags = obj->ob_flags;
    flags &= ~(_Py_FREEZABLE_SET_FLAG | _Py_FREEZABLE_STATUS_MASK);
    flags |= _Py_FREEZABLE_SET_FLAG |
             ((status << _Py_FREEZABLE_STATUS_SHIFT) & _Py_FREEZABLE_STATUS_MASK);
    obj->ob_flags = flags;
    return 0;
#else
    // 32-bit builds do not have ob_flags for freezable status.
    assert(0 && "set_freezable ob_flags fallback not supported on 32-bit");
    PyErr_SetString(PyExc_TypeError,
                    "Cannot set freezable status: object has no attribute "
                    "support and ob_flags fallback is not available on 32-bit");
    return -1;
#endif
}


int _PyImmutability_UnsetFreezable(PyObject *obj)
{
    // Try deleting the __freezable__ attribute.
    int rc = PyObject_SetAttr(obj, &_Py_ID(__freezable__), NULL);
    if (rc == 0) {
        goto clear_flags;
    }

    // If deletion failed with AttributeError/TypeError, the object
    // doesn't support attributes — fall through to ob_flags.
    if (PyErr_ExceptionMatches(PyExc_AttributeError) ||
        PyErr_ExceptionMatches(PyExc_TypeError))
    {
        PyErr_Clear();
    }
    else {
        return -1;
    }

    // The object doesn't support attributes; need ob_flags to clear.
#if SIZEOF_VOID_P <= 4
    // 32-bit builds do not have ob_flags for freezable status.
    assert(0 && "unset_freezable ob_flags fallback not supported on 32-bit");
    PyErr_SetString(PyExc_TypeError,
                    "Cannot unset freezable status: object has no attribute "
                    "support and ob_flags fallback is not available on 32-bit");
    return -1;
#endif

clear_flags:
#if SIZEOF_VOID_P > 4
    {
        uint16_t flags = obj->ob_flags;
        flags &= ~(_Py_FREEZABLE_SET_FLAG | _Py_FREEZABLE_STATUS_MASK);
        obj->ob_flags = flags;
    }
#endif
    return 0;
}


// Read the freezable status from ob_flags (64-bit only).
// Returns the status if set, or -1 if not set.
static inline int
_get_freezable_from_flags(PyObject *obj)
{
#if SIZEOF_VOID_P > 4
    uint16_t flags = obj->ob_flags;
    if (flags & _Py_FREEZABLE_SET_FLAG) {
        return (flags & _Py_FREEZABLE_STATUS_MASK) >> _Py_FREEZABLE_STATUS_SHIFT;
    }
#endif
    return -1;
}

int _PyImmutability_GetFreezable(PyObject *obj)
{
    // First, check for a __freezable__ attribute on the object.
    PyObject *attr = NULL;
    int found = PyObject_GetOptionalAttr(obj, &_Py_ID(__freezable__), &attr);
    if (found == 1) {
        int status = (int)PyLong_AsLong(attr);
        Py_DECREF(attr);
        if (status == -1 && PyErr_Occurred()) {
            return -2;
        }
        return status;
    }
    if (found == -1) {
        return -2;
    }

    // Check ob_flags for the object.
    int flags_status = _get_freezable_from_flags(obj);
    if (flags_status >= 0) {
        return flags_status;
    }

    // Not found for the object itself — check the object's type.
    PyObject *type_obj = (PyObject *)Py_TYPE(obj);
    PyObject *type_attr = NULL;
    int type_found = PyObject_GetOptionalAttr(type_obj,
                                              &_Py_ID(__freezable__),
                                              &type_attr);
    if (type_found == 1) {
        int status = (int)PyLong_AsLong(type_attr);
        Py_DECREF(type_attr);
        if (status == -1 && PyErr_Occurred()) {
            return -2;
        }
        return status;
    }
    if (type_found == -1) {
        return -2;
    }

    // Check ob_flags for the type.
    flags_status = _get_freezable_from_flags(type_obj);
    if (flags_status >= 0) {
        return flags_status;
    }

    return -1;  // Not found.
}


static int
is_shallow_immutable_type(struct _Py_immutability_state *state, PyTypeObject *tp)
{
    return _Py_hashtable_get(state->shallow_immutable_types, (void *)tp) != NULL;
}

int _PyImmutability_RegisterShallowImmutable(PyTypeObject* tp)
{
    struct _Py_immutability_state *state = get_immutable_state();
    if (state == NULL) {
        return -1;
    }

    // Idempotent — already registered is fine.
    if (is_shallow_immutable_type(state, tp)) {
        return 0;
    }

    if (_Py_hashtable_set(state->shallow_immutable_types,
                          (void *)tp, (void *)1) < 0) {
        PyErr_NoMemory();
        return -1;
    }

    // Mark the type also as freezable
    if (_PyImmutability_SetFreezable((PyObject*)tp, _Py_FREEZABLE_YES)) {
        return -1;
    }
    return 0;
}

// Check if a specific object is shallow immutable.
// (a) Its type is registered as shallow immutable (e.g. tuple instances,
//     float instances), OR
// (b) It is itself a type object with Py_TPFLAGS_IMMUTABLETYPE set
//     (e.g. the float type object — but not a mutable heap type).
static int
is_shallow_immutable(struct _Py_immutability_state *state, PyObject *obj)
{
    if (is_shallow_immutable_type(state, Py_TYPE(obj))) {
        return 1;
    }
    if (PyType_Check(obj)) {
        PyTypeObject *tp = (PyTypeObject *)obj;
        if (tp->tp_flags & Py_TPFLAGS_IMMUTABLETYPE) {
            return 1;
        }
    }
    return 0;
}

struct ImplicitCheckState {
    _Py_hashtable_t *visited;
    struct _Py_immutability_state *imm_state;
    PyObject *worklist;  // PyListObject used as a stack
};

// Visitor callback that adds objects to the worklist for iterative processing.
// Returns 0 if the object can be viewed as immutable and was added to the
// worklist, 1 if a mutable object was found, -1 on error.
static int
can_view_as_immutable_visit(PyObject *obj, void *arg)
{
    struct ImplicitCheckState *state = (struct ImplicitCheckState *)arg;
    if (obj == NULL) {
        return 0;
    }

    // Already visited — skip.
    if (_Py_hashtable_get(state->visited, obj) != NULL) {
        return 0;
    }

    // Already frozen — skip.
    if (_Py_IsImmutable(obj)) {
        return 0;
    }

    // Check if the object can be viewed as immutable.
    if (!is_shallow_immutable(state->imm_state, obj)) {
        // Found a mutable object — graph cannot be viewed as immutable.
        return 1;
    }

    // Mark visited.
    if (_Py_hashtable_set(state->visited, obj, (void *)1) < 0) {
        return -1;
    }

    // Add to worklist for traversal of referents.
    if (push(state->worklist, obj) < 0) {
        return -1;
    }

    return 0;
}

int _PyImmutability_CanViewAsImmutable(PyObject *obj)
{
    // Check if the object graph rooted at obj can be viewed as immutable.
    // An object graph can be viewed as immutable if every reachable object
    // is either already frozen, or is shallow immutable (its own state
    // cannot be mutated, though it may reference other objects).
    //
    // If the graph can be viewed as immutable, it is frozen (to set up
    // proper refcount management) and 1 is returned.
    // Returns 0 if the graph cannot be viewed as immutable, -1 on error.

    // Already frozen — trivially yes.
    if (_Py_IsImmutable(obj)) {
        return 1;
    }

    struct _Py_immutability_state *imm_state = get_immutable_state();
    if (imm_state == NULL) {
        return -1;
    }

    // The root must itself be shallow immutable to be viewed as immutable.
    if (!is_shallow_immutable(imm_state, obj))
    {
        return 0;
    }

    struct ImplicitCheckState state;
    state.imm_state = imm_state;
    state.visited = _Py_hashtable_new(
        _Py_hashtable_hash_ptr,
        _Py_hashtable_compare_direct);
    if (state.visited == NULL) {
        PyErr_NoMemory();
        return -1;
    }

    state.worklist = PyList_New(0);
    if (state.worklist == NULL) {
        _Py_hashtable_destroy(state.visited);
        return -1;
    }

    // Mark root visited and seed the worklist.
    if (_Py_hashtable_set(state.visited, obj, (void *)1) < 0) {
        _Py_hashtable_destroy(state.visited);
        Py_DECREF(state.worklist);
        PyErr_NoMemory();
        return -1;
    }
    if (push(state.worklist, obj) < 0) {
        _Py_hashtable_destroy(state.visited);
        Py_DECREF(state.worklist);
        return -1;
    }

    // Iterative DFS: pop from worklist, traverse referents.
    int result = 0;
    while (PyList_GET_SIZE(state.worklist) > 0) {
        PyObject *item = pop(state.worklist);
        traverseproc reachable = get_reachable_proc(Py_TYPE(item));
        result = reachable(item, can_view_as_immutable_visit, &state);
        if (result != 0) {
            break;
        }
    }

    _Py_hashtable_destroy(state.visited);

    // We can't call the destructor directly as we didn't newref the objects
    // on push. Breaking out of the loop above leaves the remaining objects
    // on the worklist, so drain it here. This is a slow path if there are
    // still objects in the stack, so there is no need to optimize it.
    while (PyList_Size(state.worklist) > 0) {
        pop(state.worklist);
    }
    Py_DECREF(state.worklist);

    if (result < 0) {
        return -1;
    }
    if (result > 0) {
        return 0;
    }

    // The graph can be viewed as immutable. Freeze to set up proper
    // refcount management.
    if (_PyImmutability_Freeze(obj) < 0) {
        return -1;
    }

    return 1;
}

// Perform a decref on an immutable object
// returns true if the object should be deallocated.
int _Py_DecRef_Immutable(PyObject *op)
{
    // Decrement the reference count of an immutable object without
    // deallocating it.
    assert(_Py_IsImmutable(op));

#ifdef Py_GIL_DISABLED
    // Put the clear code in DecRefShared.
    _Py_DecRefShared(op);
    return false;
#else

    // Find SCC if required.
    op = scc_root(op);

#if SIZEOF_VOID_P > 4

    uint32_t old = _Py_atomic_add_uint32(&op->ob_refcnt, -1);
#else
    // TODO(Immutable 32): Find SCC if required.

    Py_ssize_t old = _Py_atomic_add_ssize(&op->ob_refcnt, -1);
    old = _Py_IMMUTABLE_FLAG_CLEAR(old);
#endif
    assert(old > 0);

    if (old != 1) {
        assert(_Py_IMMUTABLE_FLAG_CLEAR(op->ob_refcnt) != 0);
        // Context does not to dealloc this object.
        return false;
    }

    debug("DecRef reached zero for immutable %p of type %s\n",  op, op->ob_type->tp_name);

    assert(_Py_IMMUTABLE_FLAG_CLEAR(op->ob_refcnt) == 0);

    // First, we only clear weakrefs with callbacks.
    // Callbackless weakrefs are cleared after finalizers have run.
    // See the comment in Python/gc.c above handle_weakref_callbacks.

    if (PyObject_IS_GC(op) && scc_next(op) != NULL) {
        // This object is the root of an SCC.
        if (!weakref_handle_callbacks_scc(op)) {
            // Callbacks were scheduled, deallocation will be triggered again.
            return false;
        }
        // We need to turn the SCC back into mutable state
        // and correctly re-establish RCs.
        unfreeze_and_finalize_scc(op);
        return false;
    }
    if (!weakref_handle_callbacks_single(op)) {
        // Callbacks were scheduled, deallocation will be triggered again.
        return false;
    }
    if (PyObject_IS_GC(op)) {
        // This is a GC object, so we need to put it back on the GC list.
        debug("Returning to GC simple case %p\n", op);
        return_to_gc(op);
    }

    _Py_CLEAR_IMMUTABLE(op);

    if (PyWeakref_Check(op)) {
        debug("Handling weak reference %p\n", op);
        PyObject* wr;
        int res = PyWeakref_GetRef(op, &wr);
        if (res == 1) {
            // Make the weak reference weak.
            // Get ref increments the refcount, so we need to decref twice.
            Py_DECREF(wr);
            Py_DECREF(wr);
        }
        // TODO: Don't know how to handle failure here.  It should never happen,
        // as the reference was made strong during freezing.
    }

    return true;
#endif
}

// _Py_RefcntAdd_Immutable(op, 1);
void _Py_RefcntAdd_Immutable(PyObject *op, Py_ssize_t increment)
{
    assert(_Py_IsImmutable(op));
    op = scc_root(op);

    // Increment the reference count of an immutable object.
    assert(_Py_IsImmutable(op));
#if SIZEOF_VOID_P > 4
    _Py_atomic_add_uint32(&op->ob_refcnt, increment);
#else
    _Py_atomic_add_ssize(&op->ob_refcnt, increment);
#endif
}

/* Tries to incref op and returns 1 if successful or 0 otherwise.
 * Used when creating a strong reference from a weak reference.
 * Needs to hold the weakref list lock (LOCK_WEAKREFS).
 */
int _Py_TryIncref_Immutable(PyObject *op)
{
    assert(_Py_IsImmutable(op));
    op = scc_root(op);
    assert(_Py_IsImmutable(op));

#if SIZEOF_VOID_P > 4
    uint32_t old = _Py_atomic_load_uint32_relaxed(&op->ob_refcnt);
    while (old > 0) {
        if (_Py_atomic_compare_exchange_uint32(&op->ob_refcnt, &old, old + 1)) {
            return 1;
        }
    }
#else
    Py_ssize_t old = _Py_atomic_load_ssize_relaxed(&op->ob_refcnt);
    while (_Py_IMMUTABLE_FLAG_CLEAR(old) != 0) {
        if (_Py_atomic_compare_exchange_ssize(&op->ob_refcnt, &old, old + 1)) {
            return 1;
        }
    }
#endif
    return 0;
}

/* Returns 1 if there are no references to the object's SCC. */
int _Py_IsDead_Immutable(PyObject *op)
{
    assert(_Py_IsImmutable(op));
    op = scc_root(op);
    assert(_Py_IsImmutable(op));

#if SIZEOF_VOID_P > 4
    return _Py_atomic_load_uint32_relaxed(&op->ob_refcnt) == 0;
#else
    return _Py_IMMUTABLE_FLAG_CLEAR(_Py_atomic_load_ssize_relaxed(&op->ob_refcnt)) == 0;
#endif
}

static void make_weakrefs_safe_scc(PyObject* scc)
{
    PyObject* current = scc;
    do {
        _PyWeakref_OnObjectFreeze(current);
        current = scc_next(current);
    } while (current != NULL && current != scc);
}

static int make_weakrefs_safe_visited(
    _Py_hashtable_t* tbl, const void* key, const void* value, void* unused)
{
    (void)tbl;
    (void)value;
    (void)unused;
    _PyWeakref_OnObjectFreeze((PyObject*)key);
    return 0;
}

/* Make weakrefs to newly frozen objects thread-safe. */
static void make_weakrefs_safe(struct FreezeState* freeze_state)
{
    // Handle weakrefs to completed SCCs.
    PyObject *scc = freeze_state->completed_sccs;
    while (scc != NULL) {
        PyObject *next = scc_parent(scc);
        make_weakrefs_safe_scc(scc);
        scc = next;
    }
    // Handle weakrefs to non-GC visited objects.
    _Py_hashtable_foreach(freeze_state->visited,
                          make_weakrefs_safe_visited, NULL);

}

static void
undo_completed_freeze_work(struct FreezeState* state)
{
    PyObject *scc = state->completed_sccs;
    while (scc != NULL) {
        // Read next link before rollback clears _gc_prev.
        PyObject *next = scc_parent(scc);
        if (scc_next(scc) == NULL) {
            // Single-member SCC: just clear flags and return to GC.
            undo_weakref_freeze_reference(scc);
            _Py_CLEAR_IMMUTABLE(scc);
            return_to_gc(scc);
        } else {
            undo_completed_scc(scc);
        }
        scc = next;
    }
    state->completed_sccs = NULL;

    // Clear immutability flags on non-GC visited objects.
    _Py_hashtable_foreach(state->visited,
                          clear_immutable_visitor, NULL);
    _Py_hashtable_clear(state->visited);
}

static void
discard_unfinished_traversal(struct FreezeState *state)
{
    Py_ssize_t size = state->error_dfs_limit;
    if (size < 0) {
        return;
    }
    while (PyList_Size(state->dfs) > size) {
        pop(state->dfs);
    }
    state->error_dfs_limit = -1;
}

static void
undo_add_visited(struct FreezeState *state)
{
    PyObject *item = state->error_visited_item;
    if (item == NULL) {
        return;
    }
    state->error_visited_item = NULL;
#ifndef GIL_DISABLED
    if (_PyObject_IS_GC(item)) {
        assert(scc_is_pending(item));
        if (peek(state->pending) == item) {
            (void)pop(state->pending);
        }
        _Py_CLEAR_IMMUTABLE(item);
        return_to_gc(item);
        return;
    }
#endif
    _Py_CLEAR_IMMUTABLE(item);
}

static void
finish_pending_sccs_for_undo(struct FreezeState *state)
{
#ifdef Py_DEBUG
    assert(!state->traversing);
#endif

    // Every object still on pending completed its own traversal.  The only
    // object that can be pending with incomplete traversal is the current
    // error-path item, and the error handler removes it before calling here.
    // Thus the DFS still holds every not-yet-processed edge needed to finish
    // SCC bookkeeping, while unvisited objects can be ignored.
    while (PyList_Size(state->dfs) != 0) {
        PyObject* item = pop(state->dfs);

        if (item == PostOrderMarker) {
            item = pop(state->dfs);

            finish_scc_at_postorder(item, state);
            continue;
        }

        if (item == EnsureVisitedMarker) {
            (void)pop(state->dfs);
            continue;
        }

        if (has_visited(state, item)) {
            if (is_pending(item, state)) {
                process_pending_internal_edge(item, state);
            }
            continue;
        }
    }

    if (PyList_Size(state->pending) != 0) {
        Py_FatalError("freeze rollback: pending SCCs left after draining DFS");
    }
}

/* This undoes the work of an in-progress freeze operation. */
static void
undo_partial_freeze(struct FreezeState* state)
{
    // Artifact[Implementation]: The function that rolls back immutability on failure
    debug("Unfreezing all frozen objects belonging to %p\n", state);
    finish_pending_sccs_for_undo(state);
    undo_completed_freeze_work(state);
}

/* This undoes enclosing freezes and marks them to be restarted */
static void restart_enclosing_freezes(struct _Py_immutability_state* imm_state) {
    struct FreezeState* freeze_state = imm_state->freeze_stack;

    debug("Restarting enclosing freezes of %p\n", freeze_state);

    // Skip the current call, and only walk the enclosing ones
    freeze_state = freeze_state->enclosing;
    // Mark all enclosing freezes for restart
    while (freeze_state) {
        undo_partial_freeze(freeze_state);
        freeze_state->restart = true;
        freeze_state = freeze_state->enclosing;
    }
}

static int _run_pre_freeze_hook(struct _Py_immutability_state *imm_state, PyObject* obj) {
    // 1. Check for the `__pre_freeze__` name
    PyObject *attr = NULL;
    int res = PyObject_GetOptionalAttr(obj, &_Py_ID(__pre_freeze__), &attr);
    if (res == -1) {
        return -1;
    } else if (res == 1) {
        if (!PyCallable_Check(attr)) {
            PyErr_Format(
                PyExc_TypeError,
                "'%.200s.__pre_freeze__' is not callable",
                Py_TYPE(obj)->tp_name);
            Py_DECREF(attr);
            return -1;
        }
        PyObject *result = PyObject_CallNoArgs(attr);
        Py_DECREF(attr);
        if (result == NULL) {
            return -1;
        }
        Py_DECREF(result);
    }

    // 2. Check the type for `tp_prefreeze`
    prefreezeproc prefreeze = Py_TYPE(obj)->tp_prefreeze;
    if (prefreeze != NULL) {
        return prefreeze(obj);
    }

    // No pre-freeze hook, so we're good to go.
    return 0;
}

static int check_pre_freeze_hook(struct _Py_immutability_state *imm_state, PyObject* obj) {
    // Skip Python-level hook lookup for type objects. For classes,
    // `__pre_freeze__` resolves to an unbound function and calling it as a
    // normal bound method would fail with a missing 'self' argument.
    if (PyType_Check(obj)) {
        return 0;
    }

    // Pre-freeze hooks are never called for shallow immutable objects
    if (is_shallow_immutable(imm_state, obj)) {
        return 0;
    }

    // Check if the pre-freeze hook already ran for this object
#if SIZEOF_VOID_P > 4
    if ((obj->ob_flags & _Py_PREFREEZE_RAN_FLAG) != 0) {
        return 0;
    }
#else
#error "Immutability currently only works on 64bit platforms"
#endif

    // Mark pre-freeze hook as completed. This has to be set before calling
    // the pre-freeze hook in case the pre-freeze hook reenters to prevent
    // an infinite loop.
    obj->ob_flags |= _Py_PREFREEZE_RAN_FLAG;

    // Run the pre-freeze hook if it's present.
    return _run_pre_freeze_hook(imm_state, obj);
}

static int traverse_freeze(PyObject* obj, struct FreezeState* freeze_state)
{
    //  WARNING
    //  CHANGES HERE NEED TO BE REFLECTED IN freeze_visit
    int result = -1;

#ifdef Py_DEBUG
    assert(!freeze_state->traversing);
    freeze_state->traversing = true;
#endif

#ifdef MERMAID_TRACING
    freeze_state->start = obj;
    TRACE_MERMAID_NODE(obj);
#endif

    debug_obj("Traversing %s (%p) rc=%zd\n", obj, Py_REFCNT(obj));

    if(is_c_wrapper(obj)) {
        // FIXME(Immutable): Is this still needed?
        set_direct_rc(obj);
    }

    SUCCEEDS(get_reachable_proc(Py_TYPE(obj))(obj, (visitproc)freeze_visit, freeze_state));

    // Weak references are not followed by the GC, but should be
    // for immutability.  Otherwise, we could share mutable state
    // using a weak reference.
    if (PyWeakref_Check(obj)) {
        // Make the weak reference strong.
        // Get Ref increments the refcount.
        //
        // This could be done via a pre-freeze hook, but we only want to keep
        // the strong reference if freezing succeeds. Having this as a special
        // case makes this easier to handle.
        PyObject* wr;
        int res = PyWeakref_GetRef(obj, &wr);
        if (res == -1) {
            goto error;
        }
        if (res == 1) {
            if (freeze_visit(wr, freeze_state)) {
                // freeze_visit() passes wr to push(), which consumes the
                // reference even when appending to the DFS stack fails.
                goto error;
            }
        }
    }

    result = 0;
    goto finally;

error:
finally:
#ifdef Py_DEBUG
    freeze_state->traversing = false;
#endif
    return result;
}

// Mark importlib's mutable state as not freezable.
// Separated from init_state because _frozen_importlib is not
// available during early interpreter startup.
static void
late_init(struct _Py_immutability_state *state)
{
    state->late_init_done = true;

    PyObject *frozen_importlib = PyImport_ImportModule("_frozen_importlib");
    if (frozen_importlib == NULL) {
        PyErr_Clear();
        return;
    }

    PyObject *module_locks = PyObject_GetAttrString(frozen_importlib,
                                                    "_module_locks");
    if (module_locks != NULL) {
        if (_PyImmutability_SetFreezable(module_locks,
                                         _Py_FREEZABLE_NO) < 0) {
            PyErr_Clear();
        }
        Py_DECREF(module_locks);
    } else {
        PyErr_Clear();
    }

    PyObject *blocking_on = PyObject_GetAttrString(frozen_importlib,
                                                   "_blocking_on");
    if (blocking_on != NULL) {
        if (_PyImmutability_SetFreezable(blocking_on,
                                         _Py_FREEZABLE_NO) < 0) {
            PyErr_Clear();
        }
        Py_DECREF(blocking_on);
    } else {
        PyErr_Clear();
    }

    Py_DECREF(frozen_importlib);

#ifdef Py_DEBUG
    PyObject *traceback_module = PyImport_ImportModule("traceback");
    if (traceback_module != NULL) {
        state->traceback_func = PyObject_GetAttrString(traceback_module,
                                                       "format_stack");
        Py_DECREF(traceback_module);
    } else {
        PyErr_Clear();
    }
#endif
}

// Core freeze implementation that supports multiple root objects.
// All root objects are treated as directly-frozen for EXPLICIT freezable checks.
static int
freeze_impl(PyObject *const *objs, Py_ssize_t nobjs)
{
    // Artifact[Implementation]: The entry point to the `freeze()` function in C
    //
    // This is the central function to the freeze algorithm that handles
    // DFS traversal and calls into other functions to:
    // - Checking freezability
    // - Checking and calling the pre-freeze hook
    // - Construct SCCs
    // - Handle failures
    // - Remove objects from the GC list
    struct _Py_immutability_state* imm_state = NULL;
    int result = 0;
    TRACE_MERMAID_START();

    // Initialize the freeze state
    struct FreezeState freeze_state;
    if (init_freeze_state(&freeze_state) < 0) {
        result = -1;
        goto finally;
    }

    // Get Immutable state
    imm_state = get_immutable_state();
    if(imm_state == NULL){
        result = -1;
        goto finally;
    }
    freeze_state.enclosing = imm_state->freeze_stack;
    imm_state->freeze_stack = &freeze_state;
    debug("\nfreeze_impl start. State ptr: %p\n", &freeze_state);

    // The SCC algorithm can't handle nested calls directly. So, we
    // treat freezing like transactions. We simply roll back enclosing
    // freeze calls and mark them to be restarted.
    //
    // Pre-freeze hooks only run once, this ensures progress and that
    // we'll eventually terminate.
    if (freeze_state.enclosing) {
        restart_enclosing_freezes(imm_state);
    }

    // Register all roots and push onto the DFS stack
    for (Py_ssize_t i = 0; i < nobjs; i++) {
        if (_Py_IsImmutable(objs[i])) {
            continue;
        }
        // FIXME(immutable): It is not quite clear how `Explicit` should work
        // for nested freeze calls. One could argue that they should be frozen
        // if they're the root of at least one freeze call. Even if this is an
        // enclosing `freeze` call. For now we only allow `freeze` to explicitly
        // freeze root objects of its own freeze call and ignore enclosing ones.
        if (_Py_hashtable_set(freeze_state.roots, objs[i], objs[i]) < 0) {
            PyErr_NoMemory();
            goto error;
        }
        SUCCEEDS(push(freeze_state.dfs, objs[i]));
    }

    // If all objects are already immutable, nothing to do.
    if (_Py_hashtable_len(freeze_state.roots) == 0) {
        goto finally;
    }

    // Late-init: mark importlib mutable state as not freezable.
    if (!imm_state->late_init_done) {
        late_init(imm_state);
    }

#ifdef Py_DEBUG
    // In debug mode, we can set a freeze location for debugging purposes.
    // Get a traceback object to use as the freeze location.
    if (imm_state->traceback_func != NULL) {
        PyObject *stack = PyObject_CallFunctionObjArgs(imm_state->traceback_func, NULL);
        if (stack != NULL) {
            // Add the type name to the top of the stack, can be useful.
            PyObject* typename = PyObject_GetAttrString(_PyObject_CAST(Py_TYPE(objs[0])), "__name__");
            push(stack, typename);
            freeze_state.freeze_location = stack;
        }
    }
#endif

    // A nested `freeze()` call forced a restart of this freeze
    // the roots remain the same, but we have to repopulate the
    // DFS stack
    if (false) {
restart:
        assert(PyList_Size(freeze_state.dfs) == 0);
        assert(PyList_Size(freeze_state.pending) == 0);
        for (Py_ssize_t i = 0; i < nobjs; i++) {
            if (_Py_IsImmutable(objs[i])) {
                continue;
            }
            SUCCEEDS(push(freeze_state.dfs, objs[i]));
        }
        freeze_state.restart = false;
    }

    while (PyList_Size(freeze_state.dfs) != 0) {
        PyObject* item = pop(freeze_state.dfs);

        if (item == PostOrderMarker) {
            item = pop(freeze_state.dfs);

            // Have finished traversing graph reachable from item
            finish_scc_at_postorder(item, &freeze_state);
            continue;
        }

        if (item == EnsureVisitedMarker) {
            item = pop(freeze_state.dfs);

            // Ignore item if it has been visited
            if (has_visited(&freeze_state, item)) {
                continue;
            }

            // Print a warning to get this fixed quickly
            PyTypeObject* tp = Py_TYPE(item);
            if (PyType_Check(item)) {
                tp = _PyType_CAST(item);
            }
            PySys_FormatStderr(
                "freeze: type '%.100s' is not visiting the type during traversal\n",
                tp->tp_name);

            // Manually calling freeze will add it to the DFS stack
            freeze_visit(item, (void*)&freeze_state);

            continue;
        }

        if (has_visited(&freeze_state, item)) {
            debug_obj("Already visited: %s (%p)\n", item);
            // Check if it is pending.
            if (is_pending(item, &freeze_state)) {
                process_pending_internal_edge(item, &freeze_state);
            }
            continue;
        }

        // New object, check if freezable
        SUCCEEDS(check_freezable(imm_state, item, &freeze_state));

        // Call the pre-freeze hook if one is present
        SUCCEEDS(check_pre_freeze_hook(imm_state, item));

        // Pre-freeze hooks can force a restart of freezing.
        // We only restart if the pre-freeze hook succeeds.
        if (freeze_state.restart) {
            goto restart;
        }

        // If the pre-freeze hook turned the object immutable, we want to skip it.
        if (_Py_IsImmutable(item)) {
            continue;
        }

        // Add to visited before putting in internal datastructures, so don't have
        // to account of internal RC manipulations.
        SUCCEEDS(add_visited(item, &freeze_state));

        freeze_state.error_dfs_limit = PyList_Size(freeze_state.dfs);
        freeze_state.error_visited_item = item;
        if (_PyObject_IS_GC(item)) {
            // Add postorder step to dfs.
            SUCCEEDS(push(freeze_state.dfs, item));
            SUCCEEDS(push(freeze_state.dfs, PostOrderMarker));
            // Add to the SCC path
            SUCCEEDS(push(freeze_state.pending, item));
        }


        // Traverse the fields of the current object to add to the dfs.
        SUCCEEDS(traverse_freeze(item, &freeze_state));
        freeze_state.error_dfs_limit = -1;
        freeze_state.error_visited_item = NULL;
    }

    make_weakrefs_safe(&freeze_state);
    mark_all_frozen(&freeze_state);

    goto finally;

error:
    debug("Error during freeze\n");
#ifdef Py_DEBUG
    freeze_state.traversing = false;
#endif
    discard_unfinished_traversal(&freeze_state);
    undo_add_visited(&freeze_state);
    undo_partial_freeze(&freeze_state);
    result = -1;

finally:
    if (imm_state) {
        imm_state->freeze_stack = freeze_state.enclosing;
    }
    debug("freeze_impl end. State ptr: %p\n\n", &freeze_state);
    deallocate_FreezeState(&freeze_state);
    TRACE_MERMAID_END();
    return result;
}

// Main entry point to freeze an object and everything it can reach.
int _PyImmutability_Freeze(PyObject* obj)
{
    if(_Py_IsImmutable(obj)){
        return 0;
    }
    return freeze_impl(&obj, 1);
}

// Freeze multiple root objects and their reachable graphs together.
// All provided objects are treated as roots for EXPLICIT freezable checks.
int _PyImmutability_FreezeMany(PyObject *const *objs, Py_ssize_t nobjs)
{
    return freeze_impl(objs, nobjs);
}
