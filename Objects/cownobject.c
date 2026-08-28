#include "Python.h"
#include "pymacro.h"

#include "pycore_ceval.h"         // _PyEval_AddPendingCall()
#include "pycore_cown.h"
#include "pycore_immutability.h"
#include "pycore_interp.h"        // _PyInterpreterState_LookUpID()
#include "pycore_lock.h"
#include "pycore_time.h"          // _PyTime_FromSeconds()

/* Macro that jumps to error, if the expression `x` does not succeed. */
#define SUCCEEDS(x) { do { int r = (x); if (r != 0) goto error; } while (0); }

#define Region_Check(x) Py_IS_TYPE((x), &_PyTracingRegion_Type)

// The interpreter id 0 is used. This value will be used to indicate that
// no interpreter owns the cown.
#define RELEASED_IPID       ((_PyCown_ipid_t)0xff00ff00ff00ff00LL)
#define GC_IPID             ((_PyCown_ipid_t)0xffff00ff00ff00ffLL)
#define NO_BLOCKING_TIMEOUT -1
#define UNSET_THREAD_ID     ((_PyCown_ipid_t)0xff00000000000000LL)

typedef enum CownLockStatus {
    COWN_ACQUIRE_ERROR = -1,
    COWN_ACQUIRE_FAIL = 0,
    COWN_ACQUIRE_SUCCESS = 1
} CownLockStatus;

// Cowns rely on the immutability machinery for atomic reference counting:
// PyCown_init() freezes each instance once its initial value is installed.
struct _PyCownObject {
    PyObject_HEAD
    /* The id of the interpreter that currently owns this cown.
     *
     * This value may be read from and written to from different threads.
     * Only use atomic operations to access this field.
     */
    // FIXME(cowns): xFrednet: Make sure that an interpreter releases all
    // cowns on destruction.
    _PyCown_ipid_t owning_ip;

    /* The id of the thread that unlocked this cown.
     *
     * This is provided as additional information to users, it is not validated
     * or used by this cown implementation.
     */
    _PyCown_thread_id_t locking_thread;

    /* The value stored in the cown. This value may be immutable, another cown
     * or a region object.
     */
    PyObject* value;

    /* A lock used, mainly to support timeouts and queueing for locking.
     * All other functions should use `owning_ip` to determine if they can
     * access the data or not.
     *
     * Python's mutexes already implement queueing and timeouts in a good way.
     * Later we can role our own, if we need but for not this is better. Note
     * that the optional GIL release from the lock should not be used, as it
     * doesn't seem to account for waiting threads from different interpreters.
     * Therefore, we are responsible for releasing and acquireing the GIL.
     */
    PyMutex lock;
};

_PyCown_ipid_t _PyCown_ReleasedIpid(void) {
    return RELEASED_IPID;
}

_PyCown_thread_id_t _PyCown_UnsetThreadId(void) {
    return UNSET_THREAD_ID;
}

static _PyCown_ipid_t cown_get_owner(_PyCownObject *obj) {
    return _Py_atomic_load_uint64(&obj->owning_ip);
}

_PyCown_ipid_t _PyCown_Owner(PyObject *cown) {
    return cown_get_owner(_PyCownObject_CAST(cown));
}

_PyCown_thread_id_t _PyCown_LockingThread(PyObject *cown) {
    return _Py_atomic_load_uint64(&_PyCownObject_CAST(cown)->locking_thread);
}

#define BAIL_UNLESS_OWNED_BY(o, owned_by, result) \
    do {\
        _PyCown_ipid_t owning_ip = cown_get_owner(_PyCownObject_CAST(o)); \
        if (owning_ip != owned_by) { \
            PyErr_Format( \
                PyExc_RuntimeError, \
                "attempted to access a cown owned by %llu from %llu", \
                owning_ip, owned_by); \
            return result; \
        } \
    } while (0);
#define BAIL_UNLESS_OWNED(o, result) BAIL_UNLESS_OWNED_BY(o, _PyCown_ThisInterpreterId(), result)
#define BAIL_UNLESS_OWNED_NULL(o) BAIL_UNLESS_OWNED(o, NULL)

static int cown_set_value_unchecked(_PyCownObject* self, PyObject* value) {
    // Storing a value requires ownership. The exception is the teardown of a
    // released cown, which nobody owns and only its last reference can reach.
    assert(cown_get_owner(self) == RELEASED_IPID
           || cown_get_owner(self) == _PyCown_ThisInterpreterId());

    // The region is moving out of the cown, so its region references answer to
    // the cown's owner from now on.
    if (self->value != value && Region_Check(self->value)) {
        _PyTracingRegion_SetMetaOwner(self->value, cown_get_owner(self));
    }

    // Update the value
    Py_XSETREF(self->value, Py_NewRef(value));

    // The region is now owned by this cown, so its region references resolve
    // through it and follow whoever holds it.
    if (Region_Check(value)) {
        _PyTracingRegion_SetMetaCown(value, _PyObject_CAST(self));
    }

    return 0;
}

static int cown_set_value(_PyCownObject* self, PyObject* value) {
    BAIL_UNLESS_OWNED(self, -1);

    // Bridge objects are allowed
    if (Region_Check(value)) {
        return cown_set_value_unchecked(self, value);
    }

    // Immutable objects are allowed
    if (_Py_IsImmutable(value)) {
        return cown_set_value_unchecked(self, value);
    }

    // Local objects are forbidden
    PyErr_Format(
        PyExc_RuntimeError,
        "attempted to store a local mutable object in a cown.\n"
        "Only regions, cown, and immutable objects are allowed");

    return -1;
}

/* Attempt to lock the cown.
 *
 * Timeout values:
 * (-1) => Non-blocking locking
 *  (0) => Block with no timeout
 *  (n) => Blocking with timeout
 */
static int cown_lock(_PyCownObject* self, PyTime_t timeout, _PyCown_ipid_t locking_ip, bool has_gil) {
    // A blocking time should only be set, if this call holds the GIL
    assert(has_gil || timeout == NO_BLOCKING_TIMEOUT);

    // Try to lock the mutex directly, without releasing the GIL first
    PyLockStatus r = _PyMutex_LockTimed(&self->lock, 0, _Py_LOCK_DONT_DETACH);

    // The cown is currently owned by something else. Release the GIL and
    // wait for the timeout.
    if (r != PY_LOCK_ACQUIRED && timeout != NO_BLOCKING_TIMEOUT) {
        // Release the GIL
        Py_BEGIN_ALLOW_THREADS;

        // Attempt to lock the mutex. This uses a PyMutex for the locking,
        // timeout and signal handling.
        r = _PyMutex_LockTimed(
            &self->lock,
            timeout,
            _Py_LOCK_DONT_DETACH | _PY_LOCK_HANDLE_SIGNALS
        );

        // Acquire the GIL
        Py_END_ALLOW_THREADS;
    }

    // The lock was interrupted
    if (r == PY_LOCK_INTR) {
        return COWN_ACQUIRE_ERROR;
    }

    // The lock acquisition failed
    if (r == PY_LOCK_FAILURE) {
        return COWN_ACQUIRE_FAIL;
    }

    // Set the owning_ip to the current interpreter, thereby taking ownership
    _PyCown_ipid_t released_value = RELEASED_IPID;
    if (!_Py_atomic_compare_exchange_uint64(
        &self->owning_ip,
        &released_value,
        locking_ip)
    ) {
        // Failed to set owning_ip, this should never happen and points
        // to a deeper issue.
        PyErr_Format(
            PyExc_RuntimeError,
            "[BUG] failed to set owner on a locked cown\n"
            "Cown: %U",
            self
        );

        _PyMutex_Unlock(&self->lock);
        return COWN_ACQUIRE_ERROR;
    }

    // Set the locking thread. Stored atomically because `_PyCown_LockingThread()`
    // reads it from interpreters that do not own the cown.
    _Py_atomic_store_uint64(
        &self->locking_thread,
        has_gil ? _PyCown_ThisThreadId() : UNSET_THREAD_ID);

    if (self->value && Region_Check(self->value)) {
        assert(!PyObject_GC_IsTracked(self->value));
        PyObject_GC_Track(self->value);
    }

    return COWN_ACQUIRE_SUCCESS;
}

/* Returns the interpreter id used by cowns.
 *
 * The caller must hold the GIL.
 */
_PyCown_ipid_t _PyCown_ThisInterpreterId(void) {
    _PyCown_ipid_t ip = PyInterpreterState_GetID(PyInterpreterState_Get());
    // This should never happen... if it does... we have a problem...
    assert(ip != RELEASED_IPID);
    return ip;
}

/* Returns the thread id used by cowns.
 *
 * The caller must hold the GIL.
 */
_PyCown_thread_id_t _PyCown_ThisThreadId(void) {
    _PyCown_thread_id_t id = PyThreadState_GetID(PyThreadState_Get());
    return id;
}

static int PyCown_init(_PyCownObject *self, PyObject *args, PyObject *kwds) {
    // See if we got a value as a keyword argument
    static char *kwlist[] = {"value", NULL};
    PyObject *value = Py_None;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "|O", kwlist, &value)) {
        return -1;
    }
    self->value = Py_None;

    // Init the cown as being acquired by the current interpreter
    _PyCown_ipid_t this_ip = _PyCown_ThisInterpreterId();
    _Py_atomic_store_uint64(&self->owning_ip, RELEASED_IPID);
    if (cown_lock(self, NO_BLOCKING_TIMEOUT, this_ip, true) != COWN_ACQUIRE_SUCCESS) {
        PyErr_Format(
            PyExc_RuntimeError,
            "Newly created cown couldn't be acquired by interpreter %lld (this)",
            this_ip);
        return -1;
    }

    // Set the cown value using the internal function for full validation
    SUCCEEDS(cown_set_value(self, value));

    // Freeze the cown to enable atomic reference counting for it.
    PyObject_GC_UnTrack(self);
    SUCCEEDS(_PyImmutability_Freeze(_PyObject_CAST(self)));

    return 0;
error:
    return -1;
}

static int PyCown_traverse(_PyCownObject *self, visitproc _ignore1, void* _ignore2) {
    // tp_traverse should never be called on cowns since they're not
    // tracked by the GC or in any other GC list. The cown type
    // still defines `tp_traverse` to ensure that this is never
    // accidentally called. Later we may want to simple remove it
    // from the type.
    assert(false);
    return -1;
}

static int PyCown_reachable(_PyCownObject *self, visitproc visit, void *arg) {
    Py_VISIT(Py_TYPE(self));

    // The value is explicitly not visited. Freezing or moving cowns should
    // not propagate to the value.
    // Py_VISIT(self->value);

    return 0;
}

static int PyCown_clear(_PyCownObject *self) {
    if (_Py_IsImmutable(self->value)) {
        Py_CLEAR(self->value);
        return 0;
    }

    // A mutable value is a region, which may only be dropped by the interpreter
    // owning this cown. `PyCown_dealloc` makes sure that this runs there.
    cown_set_value_unchecked(self, Py_None);
    return 0;
}

/* Tears the cown down. Only the interpreter owning the cown may run this, see
 * `cown_handoff_dealloc`. */
static void cown_dealloc_owned(_PyCownObject *self) {
    // Clearing hands the region off, so no region reference points here any more.
    PyCown_clear(self);
    PyObject_GC_Del(self);
}

static int cown_pending_dealloc(void *arg) {
    cown_dealloc_owned((_PyCownObject *)arg);
    return 0;
}

/* Hands the teardown to the interpreter owning the cown and returns true, or
 * returns false when the caller should tear the cown down itself.
 *
 * A cown is immutable, so the last reference to it can be dropped by an
 * interpreter that never owned it. Its region can not be dropped there: while
 * the cown is acquired, the region is reference counted non-atomically and
 * tracked in the owner's GC list, so touching it would race with the owner.
 *
 * The cown itself is handed over as well, instead of only its region, because a
 * region reference resolving through this cown borrows the pointer. Freeing the
 * cown here would leave that pointer dangling until the scheduled call runs.
 *
 * A cown waiting for its owner is unreachable: its reference count is zero, it
 * supports no weak references, and `_Py_TryIncref_Immutable` refuses to
 * resurrect it. It must never be revived either, since a revived cown could be
 * released a second time, with its region already gone.
 */
static bool cown_handoff_dealloc(_PyCownObject *self) {
    _PyCown_ipid_t owner = cown_get_owner(self);
    // Nobody owns a released cown, which makes the caller the only one that can
    // reach the region.
    if (owner == RELEASED_IPID || owner == _PyCown_ThisInterpreterId()) {
        return false;
    }

    // The lookup raises when the interpreter is gone, and a deallocation can
    // happen mid-raise.
    PyObject *exc = PyErr_GetRaisedException();
    // FIXME(regions): Can the interpreter go away in the middle of scheduling?
    // `weakref_schedule_callbacks` in `Python/immutability.c` asks the same.
    PyInterpreterState *target = _PyInterpreterState_LookUpID((int64_t)owner);
    bool scheduled = target != NULL
        && _PyEval_AddPendingCall(target, cown_pending_dealloc, self, 0)
               == _Py_ADD_PENDING_SUCCESS;
    PyErr_SetRaisedException(exc);
    if (scheduled) {
        return true;
    }

    // The owner is gone, or its call queue is full. Tearing the cown down here
    // is all that is left to do, so the region ends up owned by nobody. An
    // interpreter that is already gone can at least not race with us.
    _Py_atomic_store_uint64(&self->owning_ip, RELEASED_IPID);
    return false;
}

static void PyCown_dealloc(_PyCownObject *self) {
    // Reaching zero returned the cown to this interpreter's GC list. Nothing may
    // traverse it, `PyCown_traverse` asserts as much.
    PyObject_GC_UnTrack(self);

    if (cown_handoff_dealloc(self)) {
        return;
    }
    cown_dealloc_owned(self);
}

static int
lock_acquire_parse_args(PyObject *args, PyObject *kwds,
                        PyTime_t *timeout)
{
    // Taken from `Modules/_threadmodule.c`

    char *kwlist[] = {"blocking", "timeout", NULL};
    int blocking = 1;
    PyObject *timeout_obj = NULL;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "|pO:acquire", kwlist,
                                     &blocking, &timeout_obj))
        return -1;

    const PyTime_t unset_timeout = _PyTime_FromSeconds(NO_BLOCKING_TIMEOUT);
    *timeout = unset_timeout;

    if (timeout_obj
        && _PyTime_FromSecondsObject(timeout,
                                     timeout_obj, _PyTime_ROUND_TIMEOUT) < 0)
        return -1;

    if (!blocking && *timeout != unset_timeout ) {
        PyErr_SetString(PyExc_ValueError,
                        "can't specify a timeout for a non-blocking call");
        return -1;
    }
    if (*timeout < 0 && *timeout != unset_timeout) {
        PyErr_SetString(PyExc_ValueError,
                        "timeout value must be a non-negative number");
        return -1;
    }
    if (!blocking)
        *timeout = 0;
    else if (*timeout != unset_timeout) {
        PyTime_t microseconds;

        microseconds = _PyTime_AsMicroseconds(*timeout, _PyTime_ROUND_TIMEOUT);
        if (microseconds > PY_TIMEOUT_MAX) {
            PyErr_SetString(PyExc_OverflowError,
                            "timeout value is too large");
            return -1;
        }
    }
    return 0;
}

static PyObject *
CownObject_acquire(_PyCownObject *self, PyObject *args, PyObject *kwds)
{
    // Parse the arguments
    PyTime_t timeout;
    if (lock_acquire_parse_args(args, kwds, &timeout) < 0) {
        return NULL;
    }

    // Attempt to lock the cown
    _PyCown_ipid_t this_ip = _PyCown_ThisInterpreterId();
    int res = cown_lock(self, timeout, this_ip, true);
    if (res == COWN_ACQUIRE_ERROR) {
        return NULL;
    }

    // Return the result
    return PyBool_FromLong(res == COWN_ACQUIRE_SUCCESS);
}

PyDoc_STRVAR(CownObject_acquire_doc,
"acquire($self, /, blocking=True, timeout=-1)\n\
--\n\
\n\
Attempts to acquires the cown.  With default arguments this will block\n\
until the cown can be aquired, even when acquire is called from the same\n\
interpreter.  The return indicates if the cown was\n\
was acquired.  The blocking operation is interruptible.");

static int cown_release_unchecked(_PyCownObject* self, _PyCown_ipid_t unlocking_ip) {
    // Set owning_ip to indicate the released state
    if (!_Py_atomic_compare_exchange_uint64(&self->owning_ip, &unlocking_ip, RELEASED_IPID)) {
        PyErr_Format(
            PyExc_RuntimeError,
            "interpreter %lld (this) attempted to release a cown owned by someone else\n"
            "Cown: %U",
            unlocking_ip, self);
        return -1;
    }

    // Unlocking should always succeed
    int res = _PyMutex_TryUnlock(&self->lock);
    assert(res == 0);
    (void)res;

    return 0;
}

/* Checks that the cown is not released, and that the owner is as the current interpreter. */
static int cown_check_owner_before_release(_PyCownObject *self, _PyCown_ipid_t unlocking_ip) {
    _PyCown_ipid_t owning_ip = cown_get_owner(self);
    if (owning_ip == RELEASED_IPID) {
        PyErr_Format(
            PyExc_RuntimeError,
            "interpreter %lld attempted to release/switch a released cown",
            unlocking_ip
        );
        return -1;
    }
    if (owning_ip != unlocking_ip) {
        PyErr_Format(
            PyExc_RuntimeError,
            "interpreter %lld attempted to release/switch a cown owned by %lld",
            unlocking_ip, owning_ip
        );
        return -1;
    }
    return 0;
}

/* This attempts to close the region
 *
 * It returns non-zero if the closing failed
 */
static int cown_close_region(_PyCownObject *self) {
    assert(Region_Check(self->value));

    // Close the region
    int closing_res = _PyTracingRegion_Close(self->value);
    if (closing_res < 0) {
        return -1;
    }

    // Make sure that the cown owns the only external reference to the bridge object.
    if (Py_REFCNT(self->value) > 1) {
        PyErr_Format(
            PyExc_RuntimeError,
            "the cown couldn't be released, due to the bridge having incoming references");
        return -1;
    }

    // The region is closed and this is the only owner of the bridge. We untrack
    // from the current GC list.
    PyObject_GC_UnTrack(self->value);

    return 0;
}

static int cown_release(_PyCownObject *self, _PyCown_ipid_t unlocking_ip) {
    if (cown_check_owner_before_release(self, unlocking_ip) < 0) {
        return -1;
    }

    // Immutable objects are safe to share, the cown can be release directly
    if (_Py_IsImmutable(self->value)) {
        return cown_release_unchecked(self, unlocking_ip);
    }
    assert(Region_Check(self->value));

    // The contained region needs to be closed, to allow the cown to release
    if (cown_close_region(self)) {
        return -1;
    }

    // The close leaves the region local to this interpreter. Rooting it here,
    // after every check has passed, is what lets the next owner of the cown
    // dereference the region references pointing into it.
    _PyTracingRegion_SetMetaCown(self->value, _PyObject_CAST(self));

    // Region is closed, safe to release
    return cown_release_unchecked(self, unlocking_ip);
}

static PyObject* CownObject_release(_PyCownObject *self, PyObject *ignored) {
    _PyCown_ipid_t this_ip = _PyCown_ThisInterpreterId();
    if (cown_release(self, this_ip) < 0) {
        return NULL;
    }

    Py_RETURN_NONE;
}

PyDoc_STRVAR(CownObject_release_doc,
"release($self, /)\n\
--\n\
\n\
Release the cown, allowing another interpreter that is blocked waiting for\n\
the cown to acquire the cown.  The cown must be in the locked state\n\
and must be unlocked from the owning interpreter.  It may be unlocked \n\
by any thread on the owning interpreter.");

static PyObject *
CownObject_locked(_PyCownObject *op, PyObject *Py_UNUSED(dummy))
{
    return PyBool_FromLong(cown_get_owner(op) != RELEASED_IPID);
}

PyDoc_STRVAR(CownObject_locked_doc,
"locked($self, /)\n\
--\n\
\n\
Return whether the cown currently released or aquired.  \n\
Use `owned()` to check if the cown is aquired by the current interpreter.");

static PyObject *
CownObject_owned(_PyCownObject *op, PyObject *Py_UNUSED(dummy))
{
    return PyBool_FromLong(cown_get_owner(op) == _PyCown_ThisInterpreterId());
}

PyDoc_STRVAR(CownObject_owned_doc,
"owned($self, /)\n\
--\n\
\n\
Return true if the cown is currently aquired by this interpreter, false otherwise.");

static PyObject *
CownObject_owned_by_thread(_PyCownObject *op, PyObject *Py_UNUSED(dummy))
{
    if (cown_get_owner(op) != _PyCown_ThisInterpreterId()) {
        Py_RETURN_FALSE;
    }

    return PyBool_FromLong(op->locking_thread == _PyCown_ThisThreadId());
}

PyDoc_STRVAR(CownObject_owned_by_thread_doc,
"owned($self, /)\n\
--\n\
\n\
Return true if the cown is currently aquired by this interpreter and was \n\
locked by the current thread, false otherwise.  \n\
Ownership on the thread level is not enforced, any thread on the owning\n\
interpreter can access and release the cown.  This is information is only\n\
provided to give more control for those who seek it.");

static PyObject *
CownObject_is_closed(_PyCownObject *self, PyObject *Py_UNUSED(dummy))
{
    if (!Region_Check(self->value)) {
        PyErr_SetString(PyExc_TypeError, "cown value is not a tracing region");
        return NULL;
    }

    return PyBool_FromLong(_PyTracingRegion_IsClosed(self->value));
}

PyDoc_STRVAR(CownObject_is_closed_doc,
"_is_closed($self, /)\n\
--\n\
\n\
Return true if the cown's tracing region value is closed.");


// Define the CownType with methods
static PyMethodDef PyCown_methods[] = {
    {"acquire", _PyCFunction_CAST(CownObject_acquire), METH_VARARGS | METH_KEYWORDS, CownObject_acquire_doc},
    {"release", _PyCFunction_CAST(CownObject_release), METH_NOARGS, CownObject_release_doc},
    {"locked", _PyCFunction_CAST(CownObject_locked), METH_NOARGS, CownObject_locked_doc},
    {"owned", _PyCFunction_CAST(CownObject_owned), METH_NOARGS, CownObject_owned_doc},
    {"owned_by_thread", _PyCFunction_CAST(CownObject_owned_by_thread), METH_NOARGS, CownObject_owned_by_thread_doc},
    {"_is_closed", _PyCFunction_CAST(CownObject_is_closed), METH_NOARGS, CownObject_is_closed_doc},
    {NULL}  // Sentinel
};

static PyObject *CownObject_get_value(_PyCownObject *self, void *closure) {
    BAIL_UNLESS_OWNED_NULL(self);

    return Py_NewRef(self->value);
}

static int CownObject_set_value(_PyCownObject *self, PyObject *value, void *closure) {
    BAIL_UNLESS_OWNED(self, -1);

    return cown_set_value(self, value);
}

static PyGetSetDef PyCownObject_getset[] = {
    {"value", (getter)CownObject_get_value, (setter)CownObject_set_value,
        "", NULL},
    {NULL, NULL, NULL, NULL, NULL}
};

static PyObject *PyCown_repr(_PyCownObject *self) {
    _PyCown_ipid_t owner = cown_get_owner(self);
    // On this interpreter we can access the cown and content
    // safely since we hold the GIL
    if (owner == _PyCown_ThisInterpreterId()) {
        return PyUnicode_FromFormat(
            "Cown(interpreter=%llu (this), value=%S)",
            owner,
            PyObject_Repr(self->value)
        );
    }

    // The cown is released and can be acquired
    if (owner == RELEASED_IPID) {
        return PyUnicode_FromFormat(
            "Cown(interpreter=None, status=Released)"
        );
    }

    // The cown is owned by a different interpreter
    return PyUnicode_FromFormat(
        "Cown(interpreter=%llu (other))",
        owner
    );
}

PyTypeObject _PyCown_Type = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0)
    .tp_name = "Cown",
    .tp_basicsize = sizeof(_PyCownObject),
    .tp_dealloc = (destructor)PyCown_dealloc,
    .tp_repr = (reprfunc)PyCown_repr,
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC | Py_TPFLAGS_IMMUTABLETYPE,
    .tp_traverse = (traverseproc)PyCown_traverse,
    .tp_reachable = (traverseproc)PyCown_reachable,
    .tp_clear = (inquiry)PyCown_clear,
    .tp_methods = PyCown_methods,
    .tp_getset = PyCownObject_getset,
    .tp_init = (initproc)PyCown_init,
    .tp_new = PyType_GenericNew,
};

