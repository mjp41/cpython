#include "Python.h"
#include "pycore_interp.h"
#include "pycore_gc.h"            // _PyObject_GC_IS_TRACKED()
#include "pycore_object.h"        // _PyObject_GC_TRACK(), _PyDebugAllocatorStats()
#include "pycore_descrobject.h"
#include "pycore_weakref.h"

#define ERROR_OBJECT_REPORT_COUNT 5
#define ERROR_MERMAID_REPORT_LIMIT 50
#define ERROR_MERMAID_HIDE_IMMUTABLE true

#define REGION_TRACING

#ifdef REGION_TRACING
#define if_trace(...) __VA_ARGS__
#define trace_arg(arg) , (Py_uintptr_t)(arg)
#define trace(msg, ...) \
    do { \
        printf(msg "\n" __VA_OPT__(,) __VA_ARGS__); \
    } while(0)
#else
#define if_trace(...)
#define trace_arg(...)
#define trace(...)
#endif

/* Macro that jumps to error, if the expression `x` does not succeed. */
#define SUCCEEDS(x) do { int r = (x); if (r != 0) goto error; } while (0)

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
    Py_MOVABLE_FREEZE = 2,
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
    //gc_list_merge(list, &(gc_state->young.head));
    gc_list_merge(list, &(gc_state->old[0].head));
}


typedef struct {
    // These are the objects with incoming references, that
    // should be highlighted in the graph.
    _Py_hashtable_t *error_objs;
    // Accumulates the mermaid edge/node definitions as the graph is traversed.
    PyUnicodeWriter *writer;
} mermaid_builder_t;

typedef struct {
    /// A list of all visited objects
    _Py_hashtable_t *visited;
    /// The number of refs coming into this object graph
    Py_ssize_t external_rc;
    // The GC list used for this trace, it may be null if the trace
    // should not move the objects from their current list.
    PyGC_Head* gc_list;
    // The source of the reference, this is used for error reporting
    PyObject *src;
    // List of pending objects that are not GC
    PyObject *pending;
    // Used to build a mermaid diagram for error reporting if
    // the field is not NULL.
    mermaid_builder_t *mermaid;
    // This is set if an object was frozen and the trace needs
    // to restart to be valid
    bool restart;
} trace_state_t;

static int mermaid_visit(PyObject* obj, trace_state_t* state) {
    if (_Py_IsImmutable(obj) && ERROR_MERMAID_HIDE_IMMUTABLE) {
        return 0;
    }

    // Emit one mermaid edge `src --> obj` per reference, labelling both
    // endpoints with a node of the form:
    //   0x<ptr>
    //   rc=<refcount>
    //   [<type>]
    // Node ids are prefixed with 'n' so they always start with a letter, and
    // the label is quoted so the `<br>` and `[...]` are not parsed as mermaid
    // syntax. Mermaid dedupes repeated node definitions, so re-emitting a
    // node's label on every incoming edge is harmless.
    mermaid_builder_t *mermaid = state->mermaid;
    PyUnicodeWriter *writer = mermaid->writer;
    PyObject *src = state->src;

    if (src != NULL) {
        if (PyUnicodeWriter_Format(writer,
                "    n%p[\"%p<br>rc=%zd<br>[%s]\"] --> ",
                src, src, Py_REFCNT(src), Py_TYPE(src)->tp_name) < 0) {
            return -1;
        }
    }

    if (PyUnicodeWriter_Format(writer,
            "n%p[\"%p<br>rc=%zd<br>[%s]\"]",
            obj, obj, Py_REFCNT(obj), Py_TYPE(obj)->tp_name) < 0) {
        return -1;
    }

    // Highlight immutable objects and the objects with outstanding incoming
    // references. These two sets never overlap: immutable objects are never
    // added to the trace's visited set that `error_objs` is derived from.
    if (_Py_IsImmutable(obj)) {
        if (PyUnicodeWriter_WriteUTF8(writer, ":::immutable", -1) < 0) {
            return -1;
        }
    } else if (_Py_hashtable_get_entry(mermaid->error_objs, (void*)obj) != NULL) {
        if (PyUnicodeWriter_WriteUTF8(writer, ":::error", -1) < 0) {
            return -1;
        }
    }

    if (PyUnicodeWriter_WriteUTF8(writer, "\n", -1) < 0) {
        return -1;
    }

    return 0;
}

static void trace_state_destroy(trace_state_t* state) {
    if (state->visited) {
        _Py_hashtable_destroy(state->visited);
        state->visited = NULL;
    }
    if (state->pending) {
        Py_DECREF(state->pending);
        state->pending = NULL;
    }
}
static int trace_state_init(trace_state_t* state, PyGC_Head *gc_list) {
    assert(gc_list == NULL || gc_list_is_empty(gc_list));

    state->visited = NULL;
    state->pending = NULL;

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

    state->external_rc = 0;
    state->restart = false;
    state->gc_list = gc_list;
    state->src = NULL;
    state->mermaid = NULL;

    return 0;
error:
    trace_state_destroy(state);
    return -1;
}
static int trace_state_reset(trace_state_t* state, PyGC_Head *gc_list) {
    _Py_hashtable_clear(state->visited);
    SUCCEEDS(PyList_Clear(state->pending));

    state->external_rc = 0;
    state->restart = false;
    state->gc_list = gc_list;
    state->src = NULL;
    state->mermaid = NULL;

    return 0;
error:
    trace_state_destroy(state);
    return -1;
}


typedef struct {
    _Py_hashtable_t *obj_table;
    Py_ssize_t objs;
    Py_ssize_t incoming_refs;
} trace_info_t;

const int TRACE_RES_ERR = -1;
const int TRACE_RES_DONE = 0;
const int TRACE_RES_RESTART = 1;

static int _move_obj(PyObject* obj, trace_state_t* state) {
    // Check the movability of the object:
    movable_status status = get_movable_status(obj);
    switch (status) {
    case Py_MOVABLE_YES:
        break;
    case Py_MOVABLE_NO:
        trace("    - %p is not movable", obj);
        throw_region_error(
            "Instances of type '%s' are not movable", Py_TYPE(obj)->tp_name,
            state->src, obj);
        return TRACE_RES_ERR;
    case Py_MOVABLE_FREEZE:
        // Freeze the object, this can invalidate our `external_rc`,
        // we restart after this trace
        trace("    - freezing %p", obj);
        if (_PyImmutability_Freeze(obj)) {
            return TRACE_RES_ERR;
        }

        state->restart = true;
        // Setting the gc_list to NULL will stop objects from being moved
        // between GC lists. Just a small thing we can avoid. The next (full)
        // trace will have this set again.
        state->gc_list = NULL;
        return 0;
    default:
        assert(false);
        break;
    }

    // Move the object
    Py_ssize_t lrc_change = Py_REFCNT(obj);
    if (state->src != NULL) {
        // -1 for the reference we just followed
        lrc_change -= 1;
    }
    trace("    - moving %p; LRC += %zd", obj, lrc_change);
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

    if (PyList_Append(state->pending, obj)) {
        return -1;
    }

    return 0;
}

static int _trace_visit(PyObject* obj, trace_state_t* state) {
    if (state->mermaid) {
        if (mermaid_visit(obj, state)) {
            return -1;
        }
    }

    // References to immutable objects are allowed
    if (_PyImmutability_CanViewAsImmutable(obj)) {
        assert(_Py_IsImmutable(obj));
        return 0;
    }

    // Check if the object is already part of the region
    _Py_hashtable_entry_t *entry = _Py_hashtable_get_entry(state->visited, (void*)obj);
    if (entry != NULL) {
        entry->value -= 1;
        trace("    - Internal reference to %p; LRC -= 1", obj);
        state->external_rc -= 1;
        return 0;
    }

    return _move_obj(obj, state);
}

typedef struct {
    _Py_hashtable_t *target;
    PyObject *region;
} error_ref_filter;

static int _filter_visited(_Py_hashtable_t *ht, const void *key, const void *value, void *filter_void) {
    error_ref_filter *filter = (error_ref_filter *)filter_void;
    Py_ssize_t refs = (Py_ssize_t)value;

    // The caller holds one owning reference to the region object itself, which
    // is expected and not a reason the region couldn't be closed. Don't let it
    // consume one of the limited error-report slots.
    if ((PyObject *)key == filter->region) {
        refs -= 1;
    }

    // Only take objects with problematic incoming references.
    if (refs <= 0) {
        return 0;
    }
    if (_Py_hashtable_set(filter->target, key, (void*)refs)) {
        return -1;
    }
    if (_Py_hashtable_len(filter->target) >= ERROR_OBJECT_REPORT_COUNT) {
        return 1;
    }
    return 0;
}

static int _trace_once(PyObject* obj, trace_state_t* state) {
    trace("  - starting trace from %p", obj);
    int res = TRACE_RES_DONE;

    SUCCEEDS(_move_obj(obj, state));

    while (PyList_GET_SIZE(state->pending) > 0) {
        // Find the next pending item:
        PyObject *item = list_pop(state->pending);

        // Traverse item
        state->src = item;
        trace("  - traversing %p", item);
        traverseproc proc = get_reachable_proc(Py_TYPE(item));
        SUCCEEDS(proc(item, (visitproc)_trace_visit, (void*)state));

        // Weak refs need special handling
        assert(!PyWeakref_Check(item));
    }

    if (state->restart) {
        res = TRACE_RES_RESTART;
    }

    return res;
error:
    return TRACE_RES_ERR;
}

// Builds a mermaid diagram of the object graph reachable from `obj` and dumps
// it to `region-graph.md`. `error_objs` holds the objects with outstanding
// incoming references, which are highlighted in the diagram.
//
// The diagram is produced by re-tracing the graph with a mermaid builder
// attached to the trace state; `mermaid_visit` then appends one edge per
// reference. No `gc_list` is passed, so no objects are moved, and by this
// point every freezable object is already frozen.
//
// Writing the file is best-effort and silently skipped if it can't be opened.
// Returns 0 on success and -1 with a Python exception set on error.
static int dump_mermaid_diagram(PyObject* obj, _Py_hashtable_t *error_objs) {
    int res = -1;
    mermaid_builder_t mermaid = { error_objs, NULL };
    trace_state_t state;
    bool state_ready = false;
    PyObject *diagram = NULL;

    mermaid.writer = PyUnicodeWriter_Create(0);
    if (mermaid.writer == NULL) {
        goto finally;
    }

    // Top-down flowchart; `mermaid_visit` appends the edges as we traverse.
    if (PyUnicodeWriter_WriteUTF8(mermaid.writer, "flowchart TD\n", -1) < 0) {
        goto finally;
    }

    if (trace_state_init(&state, NULL)) {
        goto finally;
    }
    state_ready = true;
    state.mermaid = &mermaid;

    if (_trace_once(obj, &state) == TRACE_RES_ERR) {
        goto finally;
    }

    // `PyUnicodeWriter_Finish` consumes the writer regardless of outcome.
    diagram = PyUnicodeWriter_Finish(mermaid.writer);
    mermaid.writer = NULL;
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
    if (mermaid.writer != NULL) {
        PyUnicodeWriter_Discard(mermaid.writer);
    }
    if (state_ready) {
        trace_state_destroy(&state);
    }
    Py_XDECREF(diagram);
    return res;
}

static int trace_object(PyObject* obj, trace_info_t* result, PyGC_Head *gc_list) {
    // We do two tracing attempts, the first one may freeze classes and objects
    // and require a retrace. The second attempt should pass since all objects
    // should now be frozen. Pre-freeze hooks can mess with this, but consenting
    // adults and such.
    //
    // The first trace also finds sub-regions that needed to be closed before this one can.
    const int TRIES = 2;
    trace("Starting trace for %p", obj);

    // Init trace state.
    trace_state_t state;
    if (trace_state_init(&state, gc_list)) {
        return TRACE_RES_ERR;
    }

    result->obj_table = NULL;

    int res = 0;
    for (int i = 0; i < TRIES; i++) {
        SUCCEEDS(trace_state_reset(&state, gc_list));

        // Trace object
        res = _trace_once(obj, &state);

        // Restart trace on demand
        if (res == TRACE_RES_RESTART) {
            trace("- restarting trace for %p", obj);
            if (gc_list != NULL) {
                gc_list_dissolve(gc_list);
                assert(gc_list_is_empty(gc_list));
            }
            continue;
        }

        break;
    }

    // The region can't be closed, we'll collect some extra meta data for
    // a better error message.
    if (state.external_rc > 1) {
        result->obj_table = _Py_hashtable_new(
            _Py_hashtable_hash_ptr,
            _Py_hashtable_compare_direct);
        if (result->obj_table == NULL) {
            goto error;
        }
        error_ref_filter filter = { result->obj_table, obj };
        int for_res = _Py_hashtable_foreach(state.visited, _filter_visited, (void*)&filter);
        if (for_res < -1) {
            _Py_hashtable_destroy(result->obj_table);
            result->obj_table = NULL;
            goto error;
        }

        // If the number of objects is below the limit we can build and dump
        // a mermaid diagram of the graph to `region-graph.md` for debugging.
        if (_Py_hashtable_len(state.visited) < ERROR_MERMAID_REPORT_LIMIT) {
            if (dump_mermaid_diagram(obj, result->obj_table)) {
                goto error; // propagate Python exception
            }
        }
    }

    goto finally;
error:
    res = TRACE_RES_RESTART;
finally:
    result->incoming_refs = state.external_rc;
    result->objs = _Py_hashtable_len(state.visited);
    trace_state_destroy(&state);

    return res;
}

static void detach_weak_refs(PyGC_Head *gc_list) {
    PyGC_Head *current = GC_NEXT(gc_list);
    while (current != gc_list) {
        PyObject *item = _Py_FROM_GC(current);
#ifdef PY_DEBUG
        Py_ssize_t weak_ctn = _PyWeakref_GetWeakrefCount(item);
        if (weak_ctn) {
            trace("- Clearing %zd weak references to %p", weak_ctn, item);
        }
#endif
        if (_PyType_SUPPORTS_WEAKREFS(Py_TYPE(item))) {
            _PyWeakref_ClearWeakRefsNoCallbacks(item);
        }

        current = GC_NEXT(current);
    }
}

// ###################################################################
// Region Object
// ###################################################################

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
} TracingRegionObject;

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

static void _open_region(TracingRegionObject *self) {
    if (self->open) {
        return;
    }

    trace("Opening region %p", self);

    // This only dissolves this region, all sub-regions remain closed.
    gc_list_dissolve(&self->gc_list);
    assert(gc_list_is_empty(&self->gc_list));

    self->open = true;
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

static PyObject* TracingRegion_trace(PyObject *op) {
    trace_info_t result;
    if (trace_object(op, &result, NULL)) {
        return NULL;  // propagate Python exception
    }

    if (result.obj_table != NULL) {
        _Py_hashtable_destroy(result.obj_table);
    }

    PyObject *t = Py_BuildValue("(ii)", result.objs, result.incoming_refs);
    if (t == NULL) {
        return NULL;  // propagate Python exception
    }

    return t;
}

// State threaded through `_report_incoming_ref` while building the
// "region could not be closed" error message.
typedef struct {
    PyUnicodeWriter *writer;
    // Sum of the (problematic) incoming references reported so far.
    Py_ssize_t accounted;
} incoming_ref_report;

// `_Py_hashtable_foreach` callback over `trace_info.obj_table`. Appends one
// "- N incoming reference(s) to 'obj'" line per object to the writer.
static int
_report_incoming_ref(_Py_hashtable_t *ht, const void *key, const void *value, void *user_data) {
    incoming_ref_report *report = (incoming_ref_report *)user_data;
    PyObject *obj = (PyObject *)key;
    Py_ssize_t refs = (Py_ssize_t)value;

    report->accounted += refs;

    // `%S` calls `str()` on the object.
    if (PyUnicodeWriter_Format(report->writer,
            "- %zd incoming reference%s to '%S'\n",
            refs, (refs == 1) ? "" : "s", obj) < 0) {
        return -1;
    }
    return 0;
}

// Builds the error message describing why a region could not be closed, listing
// the objects that still have incoming references. Returns a new reference to
// the message string, or NULL with an exception set.
static PyObject *
build_close_error_message(trace_info_t *trace_info) {
    PyUnicodeWriter *writer = PyUnicodeWriter_Create(0);
    if (writer == NULL) {
        return NULL;
    }

    incoming_ref_report report = { writer, 0 };

    if (PyUnicodeWriter_WriteUTF8(writer,
            "The region could not be closed due to:\n", -1) < 0) {
        goto error;
    }

    // `obj_table` maps each object with incoming references to the number of
    // such references. Emit one line per object.
    if (_Py_hashtable_foreach(trace_info->obj_table, _report_incoming_ref, &report) < 0) {
        goto error;
    }

    // One incoming reference is the expected owning reference to the region
    // itself; everything beyond that is a reason the region stayed open. The
    // `obj_table` is also capped at `ERROR_OBJECT_REPORT_COUNT` entries, so it
    // may not list every object. Summarise whatever wasn't reported above.
    Py_ssize_t problem_refs = trace_info->incoming_refs - 1;
    if (report.accounted < problem_refs) {
        Py_ssize_t others = problem_refs - report.accounted;
        if (PyUnicodeWriter_Format(writer,
                "- %zd reference%s to other objects\n",
                others, (others == 1) ? "" : "s") < 0) {
            goto error;
        }
    }

    return PyUnicodeWriter_Finish(writer);

error:
    PyUnicodeWriter_Discard(writer);
    return NULL;
}

/* This method traces the region and closes it, if there are no references
 * pointing into the region. References to the bridge are allowed.
 *
 * This function requires the GIL to be held.
 *
 * Returns -1 if an exception was raised. 0 if the region couldn't be closed
 * and 1 if the region was closed.
 */
int _PyTracingRegion_Close(PyObject* op) {
    TracingRegionObject *self = (TracingRegionObject*)op;
    if (!self->open) {
        return 1;
    }
    assert(gc_list_is_empty(&self->gc_list));

    int res = 0;
    trace_info_t trace_info;
    if (trace_object(op, &trace_info, &self->gc_list)) {
        goto error; // propagate Python exception
    }

    // Keep the region open, if the there are more incoming references
    // besides the expected owning one
    if (trace_info.incoming_refs > 1) {
        trace("- Failed to close region %p, there are %zd incoming references", self, trace_info.incoming_refs);
        gc_list_dissolve(&self->gc_list);
        assert(gc_list_is_empty(&self->gc_list));

        // Report which objects still have incoming references as a
        // `RuntimeError`, e.g.:
        //
        //   RuntimeError: The region could not be closed due to:
        //   - 1 incoming reference to '[1, 2, 3]'
        //   - 2 incoming references to '(6, 7)'
        PyObject *msg = build_close_error_message(&trace_info);
        if (msg != NULL) {
            PyErr_SetObject(PyExc_RuntimeError, msg);
            Py_DECREF(msg);
        }

        goto error;
    }

    // FIXME: This can be optimized, for example by inserting all objects
    // with weak refs in the beginning.
    detach_weak_refs(&self->gc_list);

    trace("- Closed region %p", self);
    assert(!gc_list_is_empty(&self->gc_list));
    self->open = false;
    res = 1;
    goto finally;
error:
    res = -1;
finally:
    if (trace_info.obj_table != NULL) {
        _Py_hashtable_destroy(trace_info.obj_table);
    }

    return res;
}

int _PyTracingRegion_IsClosed(PyObject* region) {
    TracingRegionObject *self = (TracingRegionObject*)region;
    return !self->open;
}

static PyMethodDef TracingRegion_methods[] = {
    {"trace", _PyCFunction_CAST(TracingRegion_trace), METH_NOARGS,
        "This traces the region and returns the number of incoming references"},
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
