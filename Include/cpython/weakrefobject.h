#ifndef Py_CPYTHON_WEAKREFOBJECT_H
#  error "this header file must not be included directly"
#endif

/* A region reference is a weak reference that survives its target's region
 * being closed. Instead of keeping the region open it checks on every
 * dereference whether this interpreter may reach the target, and opens the
 * region tree if it may. The metadata carrying that information lives in
 * `pycore_regionref.h`; it is opaque here.
 */
PyAPI_DATA(PyTypeObject) _PyRegionref_RefType;

#define _PyRegionRef_CheckExact(op) Py_IS_TYPE((op), &_PyRegionref_RefType)

struct _PyRegionRefMetadata;

/* PyWeakReference is the base struct for the Python ReferenceType, ProxyType,
 * and CallableProxyType.
 */
struct _PyWeakReference {
    PyObject_HEAD

    /* The object to which this is a weak reference, or Py_None if none.
     * Note that this is a stealth reference:  wr_object's refcount is
     * not incremented to reflect this pointer.
     */
    PyObject *wr_object;

    /* A callable to invoke when wr_object dies, or NULL if none. */
    PyObject *wr_callback;
    /* ID of the interpreter where the callback resides.
     * Used for immutable objects to know which interpreter to call back into.
     * This is -1 if the callback is NULL.
     */
    int64_t callback_ipid;

    /* A cache for wr_object's hash code.  As usual for hashes, this is -1
     * if the hash code isn't known yet.
     */
    Py_hash_t hash;

    /* If wr_object is weakly referenced, wr_object has a doubly-linked NULL-
     * terminated list of weak references to it.  These are the list pointers.
     * If wr_object goes away, wr_object is set to Py_None, and these pointers
     * have no meaning then.
     */
    PyWeakReference *wr_prev;
    PyWeakReference *wr_next;
    vectorcallfunc vectorcall;

#ifdef Py_GIL_DISABLED
    /* Pointer to the lock used when clearing in free-threaded builds.
     * Normally this can be derived from wr_object, but in some cases we need
     * to lock after wr_object has been set to Py_None.
     */
    PyMutex *weakrefs_lock;
#endif

    /* The ownership domain of `wr_object`, or NULL if this object doesn't have an ownership
     * domain. This can happen if this is a normal weakref or if the object is immutable.
     */
    struct _PyRegionRefMetadata *region_ref;
};

PyAPI_FUNC(void) _PyWeakref_ClearRef(PyWeakReference *self);

/* Region references reuse this struct but are deliberately not a subtype of
 * `_PyWeakref_RefType`, so that `PyWeakref_Check()` stays false for them and
 * the region close trace does not follow them. */
#define _PyWeakrefOrRegionRef_Check(op) \
    (PyWeakref_Check(op) || _PyRegionRef_CheckExact(op))

#define _PyWeakref_CAST(op) \
    (assert(_PyWeakrefOrRegionRef_Check(op)), _Py_CAST(PyWeakReference*, (op)))

// Test if a weak reference is dead.
PyAPI_FUNC(int) PyWeakref_IsDead(PyObject *ref);
