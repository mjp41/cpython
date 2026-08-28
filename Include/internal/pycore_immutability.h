#ifndef Py_INTERNAL_IMMUTABILITY_H
#define Py_INTERNAL_IMMUTABILITY_H
#ifdef __cplusplus
extern "C" {
#endif

#ifndef Py_BUILD_CORE
#  error "Py_BUILD_CORE must be defined to include this header"
#endif

struct _PyRegionRefMetadata;

PyAPI_DATA(PyTypeObject) _PyTracingRegion_Type;
PyAPI_FUNC(int) _PyTracingRegion_Close(PyObject* region);
PyAPI_FUNC(int) _PyTracingRegion_IsClosed(PyObject* region);
PyAPI_FUNC(void) _PyTracingRegion_Open(PyObject* region);

/* Returns the region's metadata node, allocating it if this is the first
 * region reference the current close has found. Borrowed, and only valid while
 * the region stays closed. The caller must hold `_PyWeakref_Lock`. */
PyAPI_FUNC(struct _PyRegionRefMetadata*)
    _PyTracingRegion_MetaLockHeld(PyObject* region);

/* Hands a closed region's node to `cown`, used when a cown takes ownership of
 * the region. Does nothing for an open region. */
PyAPI_FUNC(void) _PyTracingRegion_SetMetaCown(PyObject* region, PyObject* cown);

/* Records who owns a closed region, used when it leaves the cown that owned it.
 * The owner is NOT necessarily the calling interpreter: a cown is immutable and
 * may be deallocated by anyone holding a reference, including an interpreter
 * that never owned it. Pass `_PyCown_ReleasedIpid()` when nobody owns it.
 * Does nothing for an open region. */
// FIXME: The deallocation will be fixed in a follow-up, then we can remove the
// owner argument and assert that it's always local.
PyAPI_FUNC(void) _PyTracingRegion_SetMetaOwner(
    PyObject* region, uint64_t owner);

struct _Py_immutability_state {
    int late_init_done;
    struct _Py_hashtable_t *shallow_immutable_types;
    struct _Py_hashtable_t *warned_types;
    // With the pre-freeze hook it can happen that freeze calls are
    // nested. This is stack of the enclosing freeze states.
    struct FreezeState *freeze_stack;
#ifdef Py_DEBUG
    PyObject *traceback_func;  // For debugging purposes, can be NULL
#endif
};

#ifdef __cplusplus
}
#endif
#endif /* !Py_INTERNAL_IMMUTABILITY_H */