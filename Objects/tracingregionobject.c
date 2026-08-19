#include "Python.h"
#include "pycore_interp.h"
#include "pycore_gc.h"            // _PyObject_GC_IS_TRACKED()
#include "pycore_object.h"        // _PyObject_GC_TRACK(), _PyDebugAllocatorStats()
#include "pycore_descrobject.h"
#include "pycore_weakref.h"
#include "pycore_cown.h"

#define ERROR_OBJECT_REPORT_COUNT 5
#define ERROR_MERMAID_REPORT_LIMIT 50
#define ERROR_MERMAID_HIDE_IMMUTABLE true

#define REGION_TRACING

#ifdef REGION_TRACING
#define if_dbg(...) __VA_ARGS__
#define dbg_arg(arg) , (Py_uintptr_t)(arg)
#define dbg(msg, ...) \
    do { \
        printf(msg "\n" __VA_OPT__(,) __VA_ARGS__); \
    } while(0)
#else
#define if_dbg(...)
#define dbg_arg(...)
#define dbg(...)
#endif

/* Macro that jumps to error, if the expression `x` does not succeed. */
#define SUCCEEDS(x) do { int r = (x); if (r != 0) goto error; } while (0)

#define Region_Check(x) Py_IS_TYPE((x), &_PyTracingRegion_Type)
#define Cown_Check(x) Py_IS_TYPE((x), &_PyCown_Type)

// ###################################################################
// Copied from gc.c
// ###################################################################

#ifndef Py_GIL_DISABLED
#define GC_NEXT _PyGCHead_NEXT
#define GC_PREV _PyGCHead_PREV

static inline void
gc_set_old_space(PyGC_Head *g, int space)
{
    assert(space == 0 || space == _PyGC_NEXT_MASK_OLD_SPACE_1);
    g->_gc_next &= ~_PyGC_NEXT_MASK_OLD_SPACE_1;
    g->_gc_next |= space;
}

static inline void
gc_list_init(PyGC_Head *list)
{
    // List header must not have flags.
    // We can assign pointer by simple cast.
    list->_gc_prev = (uintptr_t)list;
    list->_gc_next = (uintptr_t)list;
}

static void
gc_list_move(PyGC_Head *node, PyGC_Head *list)
{
    /* Unlink from current list. */
    PyGC_Head *from_prev = GC_PREV(node);
    PyGC_Head *from_next = GC_NEXT(node);
    _PyGCHead_SET_NEXT(from_prev, from_next);
    _PyGCHead_SET_PREV(from_next, from_prev);

    /* Relink at end of new list. */
    // list must not have flags.  So we can skip macros.
    PyGC_Head *to_prev = (PyGC_Head*)list->_gc_prev;
    _PyGCHead_SET_PREV(node, to_prev);
    _PyGCHead_SET_NEXT(to_prev, node);
    list->_gc_prev = (uintptr_t)node;
    _PyGCHead_SET_NEXT(node, list);
}

static inline int
gc_list_is_empty(PyGC_Head *list)
{
    return (list->_gc_next == (uintptr_t)list);
}

static void
gc_list_merge(PyGC_Head *from, PyGC_Head *to)
{
    assert(from != to);
    if (!gc_list_is_empty(from)) {
        PyGC_Head *to_tail = GC_PREV(to);
        PyGC_Head *from_head = GC_NEXT(from);
        PyGC_Head *from_tail = GC_PREV(from);
        assert(from_head != from);
        assert(from_tail != from);

        _PyGCHead_SET_NEXT(to_tail, from_head);
        _PyGCHead_SET_PREV(from_head, to_tail);

        _PyGCHead_SET_NEXT(from_tail, to);
        _PyGCHead_SET_PREV(to, from_tail);
    }
    gc_list_init(from);
}

static struct _gc_runtime_state*
get_gc_state(void)
{
    PyInterpreterState *interp = _PyInterpreterState_GET();
    return &interp->gc;
}

static inline void
gc_clear_collecting(PyGC_Head *g)
{
    g->_gc_prev &= ~_PyGC_PREV_MASK_COLLECTING;
}

#elif // Py_GIL_DISABLED
#error "We need GIL"
#endif

// ###################################################################
// Copied from regions-main
// ###################################################################

static PyObject* list_pop(PyObject* s){
    PyObject* item;
    Py_ssize_t size = PyList_Size(s);
    if(size == 0){
        return NULL;
    }
    item = PyList_GetItem(s, size - 1);
    if(item == NULL){
        return NULL;
    }
    // This should never fail, since we shrink the size
    if(PyList_SetSlice(s, size - 1, size, NULL)){
        Py_DECREF(item);
        return NULL;
    }
    return item;
}

typedef enum {
    Py_MOVABLE_YES = 0,
    Py_MOVABLE_NO = 1,
    // The object should be frozen
    Py_MOVABLE_FREEZE = 2,
    // The object is not movable, but the reference is allowed. The object
    // should be skipped
    Py_MOVABLE_COWN = 3,
} movable_status;

movable_status get_movable_status(PyObject *obj) {
    // FIXME(regions): xFrednet: Currently it's not possible to set
    // the movability per object. This instead returns the default
    // movability for objects. Note that some shallow immutable objects
    // will not return freeze as their movability.

    // Immortal object have no real RC, this makes it infeasible to have them
    // in a region and dynamically track their ownership. Immortal objects are
    // intended to be immutable in Python, so it should be safe to implicitly
    // freeze them.
    if (_Py_IsImmortal(obj)) {
        return Py_MOVABLE_FREEZE;
    }

    // Immutable objects don't need to be moved
    if (_Py_IsImmutable(obj)) {
        return Py_MOVABLE_FREEZE;
    }

    // Types are a pain for regions since it's likely that objects of one type may
    // end up in multiple regions, requiring the type to be frozen. Types also
    // have a lot of reference pointing to them. Let's hope there is no need to
    // keep them freezable
    if (PyType_Check(obj)) {
        return Py_MOVABLE_FREEZE;
    }

    // Module objects are also complicated. Freezing them should turn most modules
    // into proxys which should make them mostly usable.
    if (PyModule_Check(obj)) {
        return Py_MOVABLE_FREEZE;
    }

    // Functions are a mess as well, making the entire system reachable. Freezing
    // them should again just magically make most things work
    if (PyFunction_Check(obj)) {
        return Py_MOVABLE_FREEZE;
    }

    // CWrappers can't really be owned, but need some special handling since
    // interpreters could still race on their RC. Solution, throw them in the
    // freezer
    if (PyCFunction_Check(obj)
        || Py_IS_TYPE(obj, &_PyMethodWrapper_Type)
        || Py_IS_TYPE(obj, &PyWrapperDescr_Type)
    ) {
        return Py_MOVABLE_FREEZE;
    }

    // Cowns are not movable, but the reference is explicitly allowed.
    if (Cown_Check(obj)) {
        return Py_MOVABLE_COWN;
    }

    // Freezing or moving these objects is... complicated. In some cases it is
    // possible but more hassle than it's probably worth. For not we mark them
    // all as unmovable.
    if (PyFrame_Check(obj)
        || PyGen_CheckExact(obj)
        || PyCoro_CheckExact(obj)
        || PyAsyncGen_CheckExact(obj)
        || PyAsyncGenASend_CheckExact(obj)
    ) {
        return Py_MOVABLE_NO;
    }

    // Exceptions don't hold anything obviously problematic preventing them
    // from being moved into a region. The actual problem is that the runtime
    // stores references to them and that these are already emitted on an
    // error path. Moving them into a region could add more problems.
    // We should discuss how to handle these, maybe freezing is the correct
    // approach?
    if (PyExceptionInstance_Check(obj)) {
        return Py_MOVABLE_NO;
    }

    // Regions are theoretically only movable, if they're closed. The traversal
    // checks this manually.

    // For now, we define all other objects as movable by default. (Surely
    // this will not backfire)
    return Py_MOVABLE_YES;
}

// This uses the given arguments to create and throw a `RegionError`
static void throw_region_error(
    const char *format_str, const char *tp_name,
    PyObject* src, PyObject* tgt)
{
    // Don't stomp existing exception
    PyThreadState *tstate = PyThreadState_Get();
    if (_PyErr_Occurred(tstate)) {
        return;
    }

    PyErr_Format(PyExc_RuntimeError, format_str, tp_name);

    // Set source and target fields
    // Get the current exception (should be a RuntimeError)
    PyObject *exc = PyErr_GetRaisedException();
    assert(exc && PyObject_TypeCheck(exc, (PyTypeObject *)PyExc_RuntimeError));

    // Add 'source' and 'target' attributes to the exception
    PyObject_SetAttr(exc, &_Py_ID(source), src ? src : Py_None);
    PyObject_SetAttr(exc, &_Py_ID(target), tgt ? tgt : Py_None);

    PyErr_SetRaisedException((PyObject*)exc);
}

// Wrapper around tp_traverse that also visits the type object.
static int
traverse_via_tp_traverse(PyObject *obj, visitproc visit, void *state)
{
    PyTypeObject *tp = Py_TYPE(obj);

    // Visit the type with traverse
    traverseproc traverse = tp->tp_traverse;
    if (traverse != NULL) {
        int err = traverse(obj, visit, state);
        if (err) {
            return err;
        }
    }


    // Most `tp_traverse` don't visit the type even though they should.
    // Here it won't hurt to potentially visit it twice, since types
    // are non-movable but will be frozen.
    return visit((PyObject *)Py_TYPE(obj), state);
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

    if (tp->tp_traverse != NULL) {
        PySys_FormatStderr(
            "regions: type '%.100s' has tp_traverse but no tp_reachable\n",
            tp->tp_name);
    } else {
        PySys_FormatStderr(
            "regions: type '%.100s' has no tp_traverse and no tp_reachable\n",
            tp->tp_name);
    }

    // Always return the wrapper; even when tp_traverse is NULL, the wrapper
    // will still visit the type object which tp_reachable is expected to do.
    return traverse_via_tp_traverse;
}

// ###################################################################
// Tracing Impl
// ###################################################################

static void
gc_list_dissolve(PyGC_Head *list) {
    struct _gc_runtime_state* gc_state = get_gc_state();
    gc_list_merge(list, &(gc_state->old[0].head));
}

static void detach_weak_refs(PyGC_Head *gc_list) {
    PyGC_Head *current = GC_NEXT(gc_list);
    while (current != gc_list) {
        PyObject *item = _Py_FROM_GC(current);
#ifdef PY_DEBUG
        Py_ssize_t weak_ctn = _PyWeakref_GetWeakrefCount(item);
        if (weak_ctn) {
            dbg("- Clearing %zd weak references to %p", weak_ctn, item);
        }
#endif
        if (_PyType_SUPPORTS_WEAKREFS(Py_TYPE(item))) {
            _PyWeakref_ClearWeakRefsNoCallbacks(item);
        }

        current = GC_NEXT(current);
    }
}

typedef struct {
    PyObject_HEAD
    PyObject *dict;
    // The GC list containing all objects while the region is closed. The bridge
    // object is not in this GC list but in the list of the owning region or in no
    // list if it's owned by a released cown.
    PyGC_Head gc_list;
    // FIXME(regions): This can be inferred from the status of the gc_list
    // or stored in the lower bits of the GC list. For now we keep it separate
    // for the prototype
    bool open;
    // This is the number of references from inside the region that reference
    // this bridge object.
    Py_ssize_t internal_bridge_refs;
} TracingRegionObject;

static void _region_close(TracingRegionObject *self, Py_ssize_t bridge_rc) {
    if (!self->open) {
        return;
    }

    dbg("Closing region %p", self);

    // FIXME: This can be optimized, for example by inserting all objects
    // with weak refs in the beginning.
    detach_weak_refs(&self->gc_list);

    // TODO(regions): explain RC magic
    if (bridge_rc != 0) {
        assert(bridge_rc >= 0);
        dbg("- subtracting %ld internal references from the bridge object %p", bridge_rc, self);
        _Py_RefcntAdd(self, -bridge_rc);
        self->internal_bridge_refs = bridge_rc;
    } else {
        assert(self->internal_bridge_refs == 0);
    }

    self->open = false;
}

static void _open_region(TracingRegionObject *self) {
    if (self->open) {
        return;
    }

    dbg("Opening region %p", self);

    // We re-add the internal references to the RC that have been subtracted during closing.
    if (self->internal_bridge_refs != 0) {
        assert(self->internal_bridge_refs >= 0);
        dbg("- adding %ld internal references from the bridge object %p", self->internal_bridge_refs, self);
        _Py_RefcntAdd(self, self->internal_bridge_refs);
        self->internal_bridge_refs = 0;
    }

    // This only dissolves this region, all sub-regions remain closed.
    gc_list_dissolve(&self->gc_list);
    assert(gc_list_is_empty(&self->gc_list));

    self->open = true;
}

const int PER_REGION_TRACE_LIMIT = 2;

typedef struct {
    // This is the stack of pending regions needing to be closed to close
    // this region tree. Objects will be inqueued `PER_REGION_TRACE_LIMIT`
    // times. It the region is not closed when it hits the limit, the closing
    // will fail.
    PyObject *pending;
    // This tracks per region in the tree how often it has been traversed.
    // Some things require the trace to be redone, namely freezing an object
    // as that may create references and finding an open sub-region, as that
    // one needs to be traced and closed first.
    //
    // We limit the number of times we restart the trace per region.
    // Theoretically, this may reject some programs that would eventually
    // reach a fixed point, but if somebody wants to do dark magic, that's
    // really not our problem.
    _Py_hashtable_t *traceing_counts;
} tree_trace_state_t;

static void tree_trace_state_destroy(tree_trace_state_t* state) {
    if (state->traceing_counts) {
        _Py_hashtable_destroy(state->traceing_counts);
        state->traceing_counts = NULL;
    }
    if (state->pending) {
        Py_CLEAR(state->pending);
    }
}

static int tree_trace_state_init(tree_trace_state_t* state) {
    state->traceing_counts = _Py_hashtable_new(
        _Py_hashtable_hash_ptr,
        _Py_hashtable_compare_direct);
    if (state->traceing_counts == NULL) {
        goto error;
    }

    state->pending = PyList_New(0);
    if (state->pending == NULL) {
        goto error;
    }

    return 0;
error:
    tree_trace_state_destroy(state);
    return -1;
}

typedef struct {
    // List of pending objects that are not GC
    PyObject *pending;
    // A list of all visited objects
    _Py_hashtable_t *visited;

    // The trace state belonging to the region tree that this region
    // is a part of.
    tree_trace_state_t *tree_trace_state;
    // The bridge object of the region that is currently being traced.
    PyObject* bridge;
    // The source of the reference, this is used for error reporting
    PyObject *src;

    // The number of refs coming into this object graph
    Py_ssize_t external_rc;
    // The number of refs coming from inside the region to the bridge object
    Py_ssize_t bridge_rc;

    // The GC list used for this trace, it may be null if the trace
    // should not move the objects from their current list.
    PyGC_Head* gc_list;


    // This is set if an object was frozen and the trace needs
    // to restart to be valid
    bool restart;
} region_trace_state_t;

static void region_trace_state_destroy(region_trace_state_t* state) {
    if (state->pending) {
        Py_CLEAR(state->pending);
    }
    if (state->visited) {
        _Py_hashtable_destroy(state->visited);
        state->visited = NULL;
    }
}

static int region_trace_state_reset(region_trace_state_t* state, PyGC_Head *gc_list) {
    assert(gc_list == NULL || gc_list_is_empty(gc_list));

    SUCCEEDS(PyList_Clear(state->pending));
    _Py_hashtable_clear(state->visited);

    // state->tree_trace_state stays unchanged
    // state->bridge stays unchanged
    state->src = NULL;

    state->external_rc = 0;
    state->bridge_rc = 0;
    state->gc_list = gc_list;
    state->restart = false;

    return 0;
error:
    region_trace_state_destroy(state);
    return -1;
}

static int region_trace_state_init(
    region_trace_state_t* state,
    PyObject* bridge,
    PyGC_Head* gc_list,
    tree_trace_state_t *tree_trace_state
) {
    state->pending = NULL;
    state->visited = NULL;

    state->pending = PyList_New(0);
    if (state->pending == NULL) {
        goto error;
    }

    state->visited = _Py_hashtable_new(
        _Py_hashtable_hash_ptr,
        _Py_hashtable_compare_direct);
    if (state->visited == NULL) {
        goto error;
    }


    state->bridge = bridge;
    state->tree_trace_state = tree_trace_state;

    return region_trace_state_reset(state, gc_list);
error:
    region_trace_state_destroy(state);
    return -1;
}

static void region_trace_state_set_restart(region_trace_state_t* state) {
    state->restart = true;
    // Setting the gc_list to NULL will stop objects from being moved
    // between GC lists. Just a small thing we can avoid. The next (full)
    // trace will have this set again.
    state->gc_list = NULL;
}

typedef struct {
    _Py_hashtable_t *obj_table;
    Py_ssize_t objs;
    Py_ssize_t incoming_refs;
} trace_info_t;

typedef struct {
    _Py_hashtable_t *obj_table;
    Py_ssize_t incoming_refs;
} close_error_info_t;

typedef struct {
    _Py_hashtable_t *target;
    PyObject *bridge;
    Py_ssize_t ignored_refs;
} close_error_filter_t;

typedef struct {
    PyUnicodeWriter *writer;
    Py_ssize_t accounted;
} incoming_ref_report_t;

typedef struct {
    PyUnicodeWriter *writer;
    _Py_hashtable_t *visited;
    _Py_hashtable_t *error_objs;
    PyObject *pending;
    PyObject *src;
} mermaid_dump_state_t;

const int TRACE_RES_ERR = -1;
const int TRACE_RES_DONE = 0;

static int
collect_close_error_obj(_Py_hashtable_t *ht, const void *key, const void *value, void *user_data)
{
    close_error_filter_t *filter = (close_error_filter_t *)user_data;
    Py_ssize_t refs = (Py_ssize_t)value;

    if ((PyObject *)key == filter->bridge) {
        refs -= 1;
        filter->ignored_refs += 1;
    }

    if (refs <= 0) {
        return 0;
    }
    if (_Py_hashtable_set(filter->target, key, (void *)refs) < 0) {
        return -1;
    }
    if (_Py_hashtable_len(filter->target) >= ERROR_OBJECT_REPORT_COUNT) {
        return 1;
    }
    return 0;
}

static int
close_error_info_init(close_error_info_t *info, region_trace_state_t *state)
{
    info->incoming_refs = state->external_rc;
    info->obj_table = _Py_hashtable_new(
        _Py_hashtable_hash_ptr,
        _Py_hashtable_compare_direct);
    if (info->obj_table == NULL) {
        return -1;
    }

    close_error_filter_t filter = {info->obj_table, state->bridge, 0};
    int res = _Py_hashtable_foreach(state->visited, collect_close_error_obj, &filter);
    if (res < 0) {
        _Py_hashtable_destroy(info->obj_table);
        info->obj_table = NULL;
        return -1;
    }
    info->incoming_refs -= filter.ignored_refs;
    return 0;
}

static void
close_error_info_destroy(close_error_info_t *info)
{
    if (info->obj_table != NULL) {
        _Py_hashtable_destroy(info->obj_table);
        info->obj_table = NULL;
    }
}

static int
report_incoming_ref(_Py_hashtable_t *ht, const void *key, const void *value, void *user_data)
{
    incoming_ref_report_t *report = (incoming_ref_report_t *)user_data;
    PyObject *obj = (PyObject *)key;
    Py_ssize_t refs = (Py_ssize_t)value;

    report->accounted += refs;

    if (PyUnicodeWriter_Format(report->writer,
            "- %zd incoming reference%s to '%S'\n",
            refs, (refs == 1) ? "" : "s", obj) < 0) {
        return -1;
    }
    return 0;
}

static PyObject *
build_close_error_message(close_error_info_t *info)
{
    PyUnicodeWriter *writer = PyUnicodeWriter_Create(0);
    if (writer == NULL) {
        return NULL;
    }

    incoming_ref_report_t report = {writer, 0};

    if (PyUnicodeWriter_WriteUTF8(writer,
            "The region could not be closed due to:\n", -1) < 0) {
        goto error;
    }

    if (_Py_hashtable_foreach(info->obj_table, report_incoming_ref, &report) < 0) {
        goto error;
    }

    if (report.accounted < info->incoming_refs) {
        Py_ssize_t others = info->incoming_refs - report.accounted;
        if (PyUnicodeWriter_Format(writer,
                "- %zd reference%s to other objects\n",
                others, (others == 1) ? "" : "s") < 0) {
            goto error;
        }
    }

    return PyUnicodeWriter_Finish(writer);

error:
    if (!PyErr_Occurred()) {
        PyErr_SetString(PyExc_RuntimeError, "failed to build region close error message");
    }
    PyUnicodeWriter_Discard(writer);
    return NULL;
}

static int
mermaid_write_node(PyUnicodeWriter *writer, PyObject *obj)
{
    if (Region_Check(obj)) {
        const char *status = ((TracingRegionObject *)obj)->open ? "open" : "closed";
        return PyUnicodeWriter_Format(writer,
            "n%p[[\"Region %p<br>rc=%zd<br>%s\"]]",
            obj, obj, Py_REFCNT(obj), status);
    }
    if (Cown_Check(obj)) {
        return PyUnicodeWriter_Format(writer,
            "n%p([\"Cown %p<br>rc=%zd\"])",
            obj, obj, Py_REFCNT(obj));
    }
    return PyUnicodeWriter_Format(writer,
        "n%p[\"%p<br>rc=%zd<br>[%s]\"]",
        obj, obj, Py_REFCNT(obj), Py_TYPE(obj)->tp_name);
}

static int
mermaid_write_class(PyUnicodeWriter *writer, PyObject *obj, _Py_hashtable_t *error_objs)
{
    if (_Py_IsImmutable(obj)) {
        return PyUnicodeWriter_WriteUTF8(writer, ":::immutable", -1);
    }
    if (_Py_hashtable_get_entry(error_objs, obj) != NULL) {
        return PyUnicodeWriter_WriteUTF8(writer, ":::error", -1);
    }
    return 0;
}

static int
mermaid_enqueue_if_needed(mermaid_dump_state_t *state, PyObject *obj)
{
    if (_Py_IsImmutable(obj) || Cown_Check(obj)) {
        return 0;
    }
    if (Region_Check(obj) && state->src != NULL) {
        return 0;
    }
    if (_Py_hashtable_get_entry(state->visited, obj) != NULL) {
        return 0;
    }
    if (_Py_hashtable_set(state->visited, obj, obj) < 0) {
        return -1;
    }
    return PyList_Append(state->pending, obj);
}

static int
mermaid_visit(PyObject *obj, mermaid_dump_state_t *state)
{
    if (_Py_IsImmutable(obj) && ERROR_MERMAID_HIDE_IMMUTABLE) {
        return 0;
    }

    if (state->src != NULL) {
        if (PyUnicodeWriter_WriteUTF8(state->writer, "    ", -1) < 0) {
            return -1;
        }
        if (mermaid_write_node(state->writer, state->src) < 0) {
            return -1;
        }
        if (PyUnicodeWriter_WriteUTF8(state->writer, " --> ", -1) < 0) {
            return -1;
        }
    } else if (PyUnicodeWriter_WriteUTF8(state->writer, "    ", -1) < 0) {
        return -1;
    }

    if (mermaid_write_node(state->writer, obj) < 0) {
        return -1;
    }
    if (mermaid_write_class(state->writer, obj, state->error_objs) < 0) {
        return -1;
    }
    if (PyUnicodeWriter_WriteUTF8(state->writer, "\n", -1) < 0) {
        return -1;
    }

    return mermaid_enqueue_if_needed(state, obj);
}

static void
mermaid_dump_state_destroy(mermaid_dump_state_t *state)
{
    if (state->writer != NULL) {
        PyUnicodeWriter_Discard(state->writer);
        state->writer = NULL;
    }
    Py_CLEAR(state->pending);
    if (state->visited != NULL) {
        _Py_hashtable_destroy(state->visited);
        state->visited = NULL;
    }
}

static int
mermaid_dump_state_init(mermaid_dump_state_t *state, _Py_hashtable_t *error_objs)
{
    state->writer = NULL;
    state->visited = NULL;
    state->pending = NULL;
    state->src = NULL;
    state->error_objs = error_objs;

    state->writer = PyUnicodeWriter_Create(0);
    if (state->writer == NULL) {
        goto error;
    }
    state->visited = _Py_hashtable_new(
        _Py_hashtable_hash_ptr,
        _Py_hashtable_compare_direct);
    if (state->visited == NULL) {
        goto error;
    }
    state->pending = PyList_New(0);
    if (state->pending == NULL) {
        goto error;
    }
    return 0;

error:
    mermaid_dump_state_destroy(state);
    return -1;
}

static int
dump_mermaid_diagram(PyObject *root, _Py_hashtable_t *error_objs)
{
    int res = -1;
    mermaid_dump_state_t state;
    PyObject *diagram = NULL;

    if (mermaid_dump_state_init(&state, error_objs) < 0) {
        return -1;
    }

    if (PyUnicodeWriter_WriteUTF8(state.writer, "flowchart TD\n", -1) < 0) {
        goto finally;
    }
    if (mermaid_visit(root, &state) < 0) {
        goto finally;
    }

    while (PyList_GET_SIZE(state.pending) > 0) {
        PyObject *item = list_pop(state.pending);
        state.src = item;
        traverseproc proc = get_reachable_proc(Py_TYPE(item));
        SUCCEEDS(proc(item, (visitproc)mermaid_visit, &state));
    }

    diagram = PyUnicodeWriter_Finish(state.writer);
    state.writer = NULL;
    if (diagram == NULL) {
        goto finally;
    }

    const char *body = PyUnicode_AsUTF8(diagram);
    if (body == NULL) {
        goto finally;
    }

    FILE *f = fopen("region-graph.md", "w");
    if (f != NULL) {
        fputs(
            "<div style='background: #fff'>\n"
            "\n"
            "```mermaid\n"
            "%%{init: {'theme': 'neutral', 'themeVariables': { 'fontSize': '16px' }}}%%\n"
            "\n",
            f);
        fputs(body, f);
        fputs(
            "\n"
            "classDef immutable fill:#94f7ff\n"
            "classDef error stroke-width:4px,stroke:red\n"
            "```\n"
            "</div>\n",
            f);
        fclose(f);
    }

    res = 0;

finally:
    mermaid_dump_state_destroy(&state);
    Py_XDECREF(diagram);
    return res;
error:
    goto finally;
}

static int _move_obj(PyObject* obj, region_trace_state_t* state) {
    // Check the movability of the object:
    movable_status status = get_movable_status(obj);
    switch (status) {
    case Py_MOVABLE_YES:
        break;
    case Py_MOVABLE_NO:
        dbg("    - %p is not movable", obj);
        throw_region_error(
            "Instances of type '%s' are not movable", Py_TYPE(obj)->tp_name,
            state->src, obj);
        return TRACE_RES_ERR;
    case Py_MOVABLE_FREEZE:
        // Freeze the object, this can invalidate our `external_rc`,
        // we restart after this trace
        dbg("    - freezing %p", obj);
        if (_PyImmutability_Freeze(obj)) {
            return TRACE_RES_ERR;
        }

        region_trace_state_set_restart(state);
        return 0;
    case Py_MOVABLE_COWN:
        return 0;
    default:
        assert(false);
        break;
    }

    // Update the LRC, -1 for the reference we just followed
    Py_ssize_t lrc_change = Py_REFCNT(obj) - 1;
    dbg("    - moving %p; LRC += %zd", obj, lrc_change);
    state->external_rc += lrc_change;

    // Mark the object as visited, this stores the lrc_change for better error reporting
    if (_Py_hashtable_set(state->visited, obj, (void*)lrc_change) == -1) {
        return -1;
    }

    // This moves the object into the region list, if provided.
    if (state->gc_list && PyObject_IS_GC(obj) && PyObject_GC_IsTracked(obj)) {
        // This flag may be set if the region is constructed as part of
        // a finalizer. If the flag remains set, for an object removed
        // from its GC list bad things can happen.
        gc_clear_collecting(_Py_AS_GC(obj));
        // Clearing the space flag makes it easy to merge this list back
        // into the local GC lists
        gc_set_old_space(_Py_AS_GC(obj), 0);
        gc_list_move(_Py_AS_GC(obj), state->gc_list);
    }

    // Bridge objects of sub-regions are moved, but shouldn't be traversed.
    if (!Region_Check(obj)) {
        if (PyList_Append(state->pending, obj)) {
            return -1;
        }
    }

    return 0;
}

static int
_enqueue_region_for_closing(tree_trace_state_t *state, PyObject *region)
{
    for (int i = 0; i < PER_REGION_TRACE_LIMIT; i++) {
        if (PyList_Append(state->pending, region) < 0) {
            return -1;
        }
    }
    return 0;
}

static int _trace_visit(PyObject* obj, region_trace_state_t* state) {
    // References to immutable objects are allowed
    if (_PyImmutability_CanViewAsImmutable(obj)) {
        assert(_Py_IsImmutable(obj));
        return 0;
    }

    // References to the bridge are tracked separately
    if (obj == state->bridge) {
        // This branch also accounts for references from the bridge object to itself.
        dbg("    - Internal reference to bridge from %p; bridge_rc += 1", state->src);
        state->bridge_rc += 1;
        return 0;
    }

    // References external regions turns them into sub-regions. These
    // need to be traversed and closed separately
    if (Region_Check(obj)) {
        if (_PyTracingRegion_IsClosed(obj)) {
            // If the child region is closed we can move it directly
            return _move_obj(obj, state);
        } else {
            // The child region is open, we need to traverse it first and then
            // retry closing this.
            if (_enqueue_region_for_closing(state->tree_trace_state, obj) < 0) {
                return -1;
            }
            region_trace_state_set_restart(state);
        }
        return 0;
    }

    // Check if the object is already part of the region
    _Py_hashtable_entry_t *entry = _Py_hashtable_get_entry(state->visited, (void*)obj);
    if (entry != NULL) {
        entry->value -= 1;
        dbg("    - Internal reference to %p; LRC -= 1", obj);
        state->external_rc -= 1;
        return 0;
    }

    return _move_obj(obj, state);
}


static int _try_close_region(PyObject *region_obj, tree_trace_state_t *tree_trace_state) {
    assert(Region_Check(region_obj));
    TracingRegionObject* region = (TracingRegionObject*)region_obj;
    
    // Init trace state.
    region_trace_state_t state;
    if (region_trace_state_init(&state, _PyObject_CAST(region), &region->gc_list, tree_trace_state)) {
        return TRACE_RES_ERR;
    }
    int region_trace_res = TRACE_RES_DONE;

    SUCCEEDS(PyList_Append(state.pending, _PyObject_CAST(region)));

    while (PyList_GET_SIZE(state.pending) > 0) {
        // Find the next pending item:
        PyObject *item = list_pop(state.pending);

        // Traverse item
        state.src = item;
        dbg("  - traversing %p", item);
        traverseproc proc = get_reachable_proc(Py_TYPE(item));
        SUCCEEDS(proc(item, (visitproc)_trace_visit, (void*)&state));

        // TODO(regions): Handle weakrefs
        assert(!PyWeakref_Check(item));
    }

    if (state.external_rc == 0) {
        _region_close(region, state.bridge_rc);
    } else {
        gc_list_dissolve(&region->gc_list);
        assert(gc_list_is_empty(&region->gc_list));

        if (state.restart) {
            goto finally;
        }

        dbg("- Failed to close region %p, there are %zd incoming references", region, state.external_rc);
        close_error_info_t error_info = {NULL, 0};
        if (close_error_info_init(&error_info, &state) < 0) {
            goto error;
        }
        if (_Py_hashtable_len(state.visited) < ERROR_MERMAID_REPORT_LIMIT) {
            // Borrowed error table; dump_mermaid_diagram() does not take ownership.
            if (dump_mermaid_diagram(region_obj, error_info.obj_table) < 0) {
                PyErr_Clear();
            }
        }

        PyObject *msg = build_close_error_message(&error_info);
        close_error_info_destroy(&error_info);
        if (msg == NULL) {
            goto error;
        }
        PyErr_SetObject(PyExc_RuntimeError, msg);
        Py_DECREF(msg);
        goto error;
    }

    goto finally;
error:
    region_trace_res = TRACE_RES_ERR;
finally:
    region_trace_state_destroy(&state);

    return region_trace_res;
}

static int try_close_region_tree(PyObject *root) {
    dbg("Starting region tree trace from %p", root);

    tree_trace_state_t state;
    if (tree_trace_state_init(&state)) {
        return -1;
    }

    _enqueue_region_for_closing(&state, root);

    int tree_trace_res = TRACE_RES_DONE;
    while (PyList_GET_SIZE(state.pending) > 0) {
        // Find the next pending item:
        PyObject *region = list_pop(state.pending);
        assert(Region_Check(region));

        // If the region is closed we can safely skip it. Regions can be enqueued
        // multiple times, this handles all safe cases.
        if (_PyTracingRegion_IsClosed(region)) {
            continue;
        }

        dbg("- tracing region %p", region);
        int res = _try_close_region(region, &state);
        if (res == TRACE_RES_ERR) {
            goto error;
        }

        _Py_hashtable_entry_t *entry = _Py_hashtable_get_entry(state.traceing_counts, (void*)region);
        if (entry != NULL) {
            if ((Py_uintptr_t)entry->value < PER_REGION_TRACE_LIMIT) {
                entry->value = (void*)(((Py_uintptr_t)entry->value) + 1);
            } else {
                // FIXME(regions): This should maybe be turned into a trace that creates a
                // error, the problem is, that this retrace may then close the region. This
                // means that this increase the tracing limit by one. There is also a question
                // how often this actually happens. This case is pretty specific for sub-regions
                // that can't be closed and pre-freeze hooks
                PyErr_Format(
                    PyExc_RuntimeError,
                    "the region %p could not be closed after %d tracing attempts",
                    (void *)region,
                    PER_REGION_TRACE_LIMIT);
                goto error;
            }
        } else {
            SUCCEEDS(_Py_hashtable_set(state.traceing_counts, (void*)region, (void*)1));
        }
    }

    goto finally;
error:
    tree_trace_res = TRACE_RES_ERR;
finally:
    tree_trace_state_destroy(&state);

    return tree_trace_res;
}

// ###################################################################
// Region Object
// ###################################################################

static int
TracingRegion_init(TracingRegionObject *self, PyObject *args, PyObject *kwargs) {
    gc_list_init(&self->gc_list);
    // We make the region open by default, this ensures that the first close
    // will handle the region type correctly. Alternatively, we could make them
    // closed in the beginning, but then handle the cases specifically.
    self->open = true;
    return 0;
}

static int
TracingRegion_traverse(TracingRegionObject *self, visitproc visit, void *arg) {
    Py_VISIT(self->dict);
    return 0;
}

static int
TracingRegion_clear(TracingRegionObject *self) {
    // FIXME(regions): Special branch when closed to dealloc all

    // This is deallocating a closed region, we just dissolve it
    if (!gc_list_is_empty(&self->gc_list)) {
        gc_list_dissolve(&self->gc_list);
    }
    Py_CLEAR(self->dict);
    return 0;
}

static void
TracingRegion_dealloc(TracingRegionObject *self) {
    // FIXME(regions): Special branch when closed to dealloc all

    PyObject_GC_UnTrack(self);
    TracingRegion_clear(self);
    Py_TYPE(self)->tp_free((PyObject *)self);
}

static PyObject *
TracingRegion_getattro(PyObject *op, PyObject *name) {
    TracingRegionObject *self = (TracingRegionObject*)op;
    _open_region(self);

    return _PyObject_GenericGetAttrWithDict(op, name, self->dict, 0);
}

static int
TracingRegion_setattro(PyObject *op, PyObject *name, PyObject *value) {
    TracingRegionObject *self = (TracingRegionObject*)op;
    _open_region(self);

    // Allocate lazily because the generic helper only stores into a provided dict.
    if (self->dict == NULL) {
        self->dict = PyDict_New();
        if (self->dict == NULL) {
            return -1;
        }
    }

    return _PyObject_GenericSetAttrWithDict(op, name, value, self->dict);
}

static PyObject *
TracingRegion_get_dict(PyObject *op, void *Py_UNUSED(context)) {
    TracingRegionObject *self = (TracingRegionObject*)op;
    _open_region(self);

    if (self->dict == NULL) {
        self->dict = PyDict_New();
        if (self->dict == NULL) {
            return NULL;
        }
    }
    return Py_NewRef(self->dict);
}

static int
TracingRegion_set_dict(PyObject *op, PyObject *value, void *Py_UNUSED(context)) {
    TracingRegionObject *self = (TracingRegionObject*)op;
    _open_region(self);

    if (value == NULL) {
        PyErr_SetString(PyExc_TypeError, "cannot delete __dict__");
        return -1;
    }
    if (!PyDict_Check(value)) {
        PyErr_Format(PyExc_TypeError,
                     "__dict__ must be set to a dictionary, not a '%.200s'",
                     Py_TYPE(value)->tp_name);
        return -1;
    }
    Py_XSETREF(self->dict, Py_NewRef(value));
    return 0;
}


/* This method traces the region and closes it, if there are no references
 * pointing into the region. References to the bridge are allowed.
 *
 * This function requires the GIL to be held.
 *
 * Returns -1 if an exception was raised. 0 if the region could be closed.
 */
int _PyTracingRegion_Close(PyObject* op) {
    TracingRegionObject *self = (TracingRegionObject*)op;
    if (!self->open) {
        return 1;
    }
    assert(gc_list_is_empty(&self->gc_list));

    return try_close_region_tree(op);
}

int _PyTracingRegion_IsClosed(PyObject* region) {
    TracingRegionObject *self = (TracingRegionObject*)region;
    return !self->open;
}

static PyMethodDef TracingRegion_methods[] = {
    {NULL,              NULL}           /* sentinel */
};

static PyGetSetDef TracingRegion_getset[] = {
    {"__dict__", TracingRegion_get_dict, TracingRegion_set_dict},
    {NULL}
};

PyTypeObject _PyTracingRegion_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "TracingRegion",
    .tp_basicsize = sizeof(TracingRegionObject),
    .tp_dealloc = (destructor)TracingRegion_dealloc,
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC | Py_TPFLAGS_IMMUTABLETYPE,
    .tp_traverse = (traverseproc)TracingRegion_traverse,
    .tp_clear = (inquiry)TracingRegion_clear,
    .tp_getset = TracingRegion_getset,
    .tp_methods = TracingRegion_methods,
    .tp_getattro = TracingRegion_getattro,
    .tp_setattro = TracingRegion_setattro,
    .tp_init = (initproc)TracingRegion_init,
    .tp_new = PyType_GenericNew,
    .tp_reachable = _PyObject_ReachableVisitTypeAndTraverse,
};

// TODO: Weak-references part of the trace are not handled
