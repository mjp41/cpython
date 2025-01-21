#define PY_SSIZE_T_CLEAN
#include "Python.h"
#include <stdbool.h>
#include <time.h>
#include <structmember.h>
#include "pycore_regions.h"

/***************************************************************/
/*    Platform-specific Threading Aliases/Wrappers             */
/***************************************************************/

#if _WIN32
#include <windows.h>

typedef int PyBehaviorsLockStatus;
typedef LPCRITICAL_SECTION PyBehaviors_type_lock;

#define PY_BEHAVIORS_LOCK_SUCCESS 0
#define PY_BEHAVIORS_LOCK_BUSY 1
#define PY_BEHAVIORS_LOCK_ERROR 2

PyBehaviors_type_lock PyBehaviors_allocate_lock(void)
{
  LPCRITICAL_SECTION lock = (LPCRITICAL_SECTION)PyMem_RawMalloc(sizeof(CRITICAL_SECTION));
  if (lock == NULL)
  {
    PyErr_NoMemory();
    return NULL;
  }

  InitializeCriticalSection(lock);
  return lock;
}

void PyBehaviors_free_lock(PyBehaviors_type_lock lock)
{
  if (lock)
  {
    DeleteCriticalSection(lock);
    PyMem_RawFree(lock);
  }
}

PyBehaviorsLockStatus PyBehaviors_acquire_lock(PyBehaviors_type_lock lock, int waitflag)
{
  if (waitflag)
  {
    EnterCriticalSection(lock);
    return PY_BEHAVIORS_LOCK_SUCCESS;
  }

  if (TryEnterCriticalSection(lock))
  {
    return PY_BEHAVIORS_LOCK_SUCCESS;
  }

  return PY_BEHAVIORS_LOCK_BUSY;
}

PyBehaviorsLockStatus PyBehaviors_release_lock(PyBehaviors_type_lock lock)
{
  LeaveCriticalSection(lock);
  return PY_BEHAVIORS_LOCK_SUCCESS;
}

#elif __APPLE__
#include <pthread.h>

typedef int PyBehaviorsLockStatus;
typedef pthread_mutex_t *PyBehaviors_type_lock;

#define PY_BEHAVIORS_LOCK_SUCCESS 0
#define PY_BEHAVIORS_LOCK_BUSY EBUSY
#define PY_BEHAVIORS_LOCK_ERROR EINVAL

PyBehaviors_type_lock PyBehaviors_allocate_lock(void)
{
  pthread_mutex_t *lock = (pthread_mutex_t *)PyMem_RawMalloc(sizeof(pthread_mutex_t));
  if (lock == NULL)
  {
    PyErr_NoMemory();
    return NULL;
  }

  pthread_mutex_init(lock, NULL);
  return lock;
}

void PyBehaviors_free_lock(PyBehaviors_type_lock lock)
{
  int r;
  if (lock)
  {
    r = pthread_mutex_destroy(lock);
    if (r == EBUSY)
    {
      PyErr_SetString(PyExc_RuntimeError, "Lock is busy");
      return;
    }

    PyMem_RawFree(lock);
  }
}

PyBehaviorsLockStatus PyBehaviors_acquire_lock(PyBehaviors_type_lock lock, int waitflag)
{
  if (waitflag)
  {
    return pthread_mutex_lock(lock);
  }

  return pthread_mutex_trylock(lock);
}

PyBehaviorsLockStatus PyBehaviors_release_lock(PyBehaviors_type_lock lock)
{
  return pthread_mutex_unlock(lock);
}

#else
#include <threads.h>
typedef int PyBehaviorsLockStatus;
typedef mtx_t *PyBehaviors_type_lock;

#define PY_BEHAVIORS_LOCK_SUCCESS thrd_success
#define PY_BEHAVIORS_LOCK_BUSY thrd_busy
#define PY_BEHAVIORS_LOCK_ERROR thrd_error

PyBehaviors_type_lock PyBehaviors_allocate_lock(void)
{
  mtx_t *lock = (mtx_t *)PyMem_RawMalloc(sizeof(mtx_t));
  if (lock == NULL)
  {
    PyErr_NoMemory();
    return NULL;
  }

  if (mtx_init(lock, mtx_timed) == thrd_success)
  {
    return lock;
  }

  PyMem_RawFree(lock);
  PyErr_SetString(PyExc_RuntimeError, "Error initialising lock");
  return NULL;
}

void PyBehaviors_free_lock(PyBehaviors_type_lock lock)
{
  if (lock)
  {
    mtx_destroy(lock);
    PyMem_RawFree(lock);
  }
}

PyBehaviorsLockStatus PyBehaviors_acquire_lock(PyBehaviors_type_lock lock, int waitflag)
{
  if (waitflag)
  {
    return mtx_lock(lock);
  }

  return mtx_trylock(lock);
}

PyBehaviorsLockStatus PyBehaviors_release_lock(PyBehaviors_type_lock lock)
{
  return mtx_unlock(lock);
}
#endif

static int
lock_acquire_parse_args(PyObject *args, PyObject *kwds,
                        bool *blocking)
{
  char *kwlist[] = {"blocking", NULL};
  *blocking = 1;

  if (!PyArg_ParseTupleAndKeywords(args, kwds, "|p:acquire", kwlist,
                                   &blocking))
    return -1;

  return 0;
}

/* lock object */

typedef struct behaviors_lock_object_s
{
  PyObject_HEAD
      PyBehaviors_type_lock lock_lock;
  PyObject *in_weakreflist;
  char locked; /* for sanity checking */
} lockobject;

static int
lock_PyBehaviors_traverse(lockobject *self, visitproc visit, void *arg)
{
  Py_VISIT(Py_TYPE(self));
  return 0;
}

static void
lock_PyBehaviors_dealloc(lockobject *self)
{
  PyObject_GC_UnTrack(self);
  if (self->in_weakreflist != NULL)
  {
    PyObject_ClearWeakRefs((PyObject *)self);
  }
  if (self->lock_lock != NULL)
  {
    /* Unlock the lock so it's safe to free it */
    if (self->locked)
      PyBehaviors_release_lock(self->lock_lock);
    PyBehaviors_free_lock(self->lock_lock);
  }
  PyTypeObject *tp = Py_TYPE(self);
  tp->tp_free((PyObject *)self);
  Py_DECREF(tp);
}

static PyObject *
lock_PyBehaviors_acquire_lock(lockobject *self, PyObject *args, PyObject *kwds)
{
  bool blocking;
  if (lock_acquire_parse_args(args, kwds, &blocking) < 0)
    return NULL;

  PyBehaviorsLockStatus r = PyBehaviors_acquire_lock(self->lock_lock, blocking);
  if (r == PY_BEHAVIORS_LOCK_ERROR)
  {
    return NULL;
  }

  if (r == PY_BEHAVIORS_LOCK_SUCCESS)
    self->locked = 1;
  return PyBool_FromLong(r == PY_BEHAVIORS_LOCK_SUCCESS);
}

PyDoc_STRVAR(acquire_doc,
             "acquire(blocking=True, timeout=-1) -> bool\n\
(acquire_lock() is an obsolete synonym)\n\
\n\
Lock the lock.  Without argument, this blocks if the lock is already\n\
locked (even by the same thread), waiting for another thread to release\n\
the lock, and return True once the lock is acquired.\n\
With an argument, this will only block if the argument is true,\n\
and the return value reflects whether the lock is acquired.\n\
The blocking operation is interruptible.");

static PyObject *
lock_PyBehaviors_release_lock(lockobject *self, PyObject *Py_UNUSED(ignored))
{
  /* Sanity check: the lock must be locked */
  if (!self->locked)
  {
    PyErr_SetString(PyExc_RuntimeError, "release unlocked lock");
    return NULL;
  }

  PyBehaviorsLockStatus r = PyBehaviors_release_lock(self->lock_lock);
  if (r == PY_BEHAVIORS_LOCK_ERROR)
  {
    PyErr_SetString(PyExc_RuntimeError, "cannot release lock");
    return NULL;
  }

  self->locked = 0;
  Py_RETURN_NONE;
}

PyDoc_STRVAR(release_doc,
             "release()\n\
(release_lock() is an obsolete synonym)\n\
\n\
Release the lock, allowing another thread that is blocked waiting for\n\
the lock to acquire the lock.  The lock must be in the locked state,\n\
but it needn't be locked by the same thread that unlocks it.");

static PyObject *
lock_PyBehaviors_locked(lockobject *self, PyObject *Py_UNUSED(ignored))
{
  return PyBool_FromLong((long)self->locked);
}

PyDoc_STRVAR(locked_doc,
             "locked() -> bool\n\
(locked_lock() is an obsolete synonym)\n\
\n\
Return whether the lock is in the locked state.");

static PyObject *
lock_PyBehaviors_repr(lockobject *self)
{
  return PyUnicode_FromFormat("<%s %s object at %p>",
                              self->locked ? "locked" : "unlocked", Py_TYPE(self)->tp_name, self);
}

static PyObject *
lock_PyBehaviors_new(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
  lockobject *self = (lockobject *)type->tp_alloc(type, 0);
  if (self == NULL)
  {
    return NULL;
  }

  self->in_weakreflist = NULL;
  self->locked = 0;
  self->lock_lock = PyBehaviors_allocate_lock();

  if (self->lock_lock == NULL)
  {
    Py_DECREF(self);
    return NULL;
  }
  return (PyObject *)self;
}

static PyMethodDef lock_methods[] = {
    {"acquire_lock", _PyCFunction_CAST(lock_PyBehaviors_acquire_lock),
     METH_VARARGS | METH_KEYWORDS, acquire_doc},
    {"acquire", _PyCFunction_CAST(lock_PyBehaviors_acquire_lock),
     METH_VARARGS | METH_KEYWORDS, acquire_doc},
    {"release_lock", (PyCFunction)lock_PyBehaviors_release_lock,
     METH_NOARGS, release_doc},
    {"release", (PyCFunction)lock_PyBehaviors_release_lock,
     METH_NOARGS, release_doc},
    {"locked_lock", (PyCFunction)lock_PyBehaviors_locked,
     METH_NOARGS, locked_doc},
    {"locked", (PyCFunction)lock_PyBehaviors_locked,
     METH_NOARGS, locked_doc},
    {"__enter__", _PyCFunction_CAST(lock_PyBehaviors_acquire_lock),
     METH_VARARGS | METH_KEYWORDS, acquire_doc},
    {"__exit__", (PyCFunction)lock_PyBehaviors_release_lock,
     METH_VARARGS, release_doc},
    {NULL, NULL} /* sentinel */
};

PyDoc_STRVAR(lock_doc,
             "A lock object is a synchronization primitive.  To create a lock,\n\
call behaviors.Lock().  Methods are:\n\
\n\
acquire() -- lock the lock, possibly blocking until it can be obtained\n\
release() -- unlock of the lock\n\
locked() -- test whether the lock is currently locked\n\
\n\
A lock is not owned by the thread that locked it; another thread may\n\
unlock it.  A thread attempting to lock a lock that it has already locked\n\
will block until another thread unlocks it.  Deadlocks may ensue.");

static PyMemberDef lock_type_members[] = {
    {"__weaklistoffset__", T_PYSSIZET, offsetof(lockobject, in_weakreflist), READONLY},
    {NULL},
};

static PyType_Slot lock_type_slots[] = {
    {Py_tp_dealloc, (destructor)lock_PyBehaviors_dealloc},
    {Py_tp_repr, (reprfunc)lock_PyBehaviors_repr},
    {Py_tp_doc, (void *)lock_doc},
    {Py_tp_methods, lock_methods},
    {Py_tp_new, lock_PyBehaviors_new},
    {Py_tp_traverse, lock_PyBehaviors_traverse},
    {Py_tp_members, lock_type_members},
    {0, 0}};

static PyType_Spec lock_type_spec = {
    .name = "_behaviors.lock",
    .basicsize = sizeof(lockobject),
    .flags = (Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC |
              Py_TPFLAGS_BASETYPE | Py_TPFLAGS_IMMUTABLETYPE),
    .slots = lock_type_slots,
};

/* recursive lock */

typedef struct behaviors_rlock_object_s
{
  PyObject_HEAD
      PyBehaviors_type_lock rlock_lock;
  uint64_t rlock_interp;
  unsigned long rlock_thread;
  uint64_t rlock_count;
  PyObject *in_weakreflist;
} rlockobject;

static int
rlock_PyBehaviors_traverse(rlockobject *self, visitproc visit, void *arg)
{
  Py_VISIT(Py_TYPE(self));
  return 0;
}

static void
rlock_PyBehaviors_dealloc(rlockobject *self)
{
  PyObject_GC_UnTrack(self);
  if (self->in_weakreflist != NULL)
  {
    PyObject_ClearWeakRefs((PyObject *)self);
  }

  if (self->rlock_count > 0)
  {
    PyBehaviors_release_lock(self->rlock_lock);
    PyBehaviors_free_lock(self->rlock_lock);
  }

  PyTypeObject *tp = Py_TYPE(self);
  tp->tp_free(self);
  Py_DECREF(tp);
}

static PyObject *
rlock_PyBehaviors_acquire(PyObject *lock, PyObject *args, PyObject *kwds)
{
  uint64_t iid;
  unsigned long tid;
  bool blocking;
  PyBehaviorsLockStatus r;

  rlockobject *self = (rlockobject *)lock;

  if (lock_acquire_parse_args(args, kwds, &blocking) < 0)
    return NULL;

  iid = PyInterpreterState_GetID(PyInterpreterState_Get());
  tid = PyThread_get_thread_ident();
  if (self->rlock_count > 0 && iid == self->rlock_interp && tid == self->rlock_thread)
  {
    uint64_t count = self->rlock_count + 1;
    if (count <= self->rlock_count)
    {
      PyErr_SetString(PyExc_OverflowError,
                      "Internal lock count overflowed");
      return NULL;
    }
    self->rlock_count = count;
    Py_RETURN_TRUE;
  }

  r = PyBehaviors_acquire_lock(self->rlock_lock, blocking);

  if (r == PY_BEHAVIORS_LOCK_SUCCESS)
  {
    assert(self->rlock_count == 0);
    self->rlock_interp = iid;
    self->rlock_thread = tid;
    self->rlock_count = 1;
  }
  else if (r == PY_BEHAVIORS_LOCK_ERROR)
  {
    PyErr_SetString(PyExc_RuntimeError,
                    "error acquiring lock");
    return NULL;
  }

  return PyBool_FromLong(r == PY_BEHAVIORS_LOCK_SUCCESS);
}

PyDoc_STRVAR(rlock_acquire_doc,
             "acquire(blocking=True) -> bool\n\
\n\
Lock the lock.  `blocking` indicates whether we should wait\n\
for the lock to be available or not.  If `blocking` is False\n\
and another thread holds the lock, the method will return False\n\
immediately.  If `blocking` is True and another thread holds\n\
the lock, the method will wait for the lock to be released,\n\
take it and then return True.\n\
(note: the blocking operation is interruptible.)\n\
\n\
In all other cases, the method will return True immediately.\n\
Precisely, if the current thread already holds the lock, its\n\
internal counter is simply incremented. If nobody holds the lock,\n\
the lock is taken and its internal counter initialized to 1.");

static PyObject *
rlock_PyBehaviors_release(PyObject *lock, PyObject *Py_UNUSED(ignored))
{
  rlockobject *self = (rlockobject *)lock;
  uint64_t iid = PyInterpreterState_GetID(PyInterpreterState_Get());
  unsigned long tid = PyThread_get_thread_ident();

  if (self->rlock_count == 0 || self->rlock_thread != tid || self->rlock_interp != iid)
  {
    PyErr_SetString(PyExc_RuntimeError,
                    "cannot release un-acquired lock");
    return NULL;
  }
  if (--self->rlock_count == 0)
  {
    self->rlock_interp = 0;
    self->rlock_thread = 0;
    if (PyBehaviors_release_lock(self->rlock_lock) == PY_BEHAVIORS_LOCK_ERROR)
    {
      PyErr_SetString(PyExc_RuntimeError,
                      "cannot release lock");
      return NULL;
    }
  }

  Py_RETURN_NONE;
}

PyDoc_STRVAR(rlock_release_doc,
             "release()\n\
\n\
Release the lock, allowing another thread that is blocked waiting for\n\
the lock to acquire the lock.  The lock must be in the locked state,\n\
and must be locked by the same thread that unlocks it; otherwise a\n\
`RuntimeError` is raised.\n\
\n\
Do note that if the lock was acquire()d several times in a row by the\n\
current thread, release() needs to be called as many times for the lock\n\
to be available for other threads.");

static PyObject *
rlock_PyBehaviors_acquire_restore(rlockobject *self, PyObject *args)
{
  uint64_t interp;
  unsigned long thread;
  unsigned long count;
  PyBehaviorsLockStatus r;

  interp = PyThreadState_GetID(PyThreadState_GET());

  if (!PyArg_ParseTuple(args, "(kk):_acquire_restore", &count, &thread))
    return NULL;

  r = PyBehaviors_acquire_lock(self->rlock_lock, 0);
  if (r == PY_BEHAVIORS_LOCK_BUSY)
  {
    Py_BEGIN_ALLOW_THREADS
        r = PyBehaviors_acquire_lock(self->rlock_lock, 1);
    Py_END_ALLOW_THREADS
  }

  if (r != PY_BEHAVIORS_LOCK_SUCCESS)
  {
    PyErr_SetString(PyExc_RuntimeError, "couldn't acquire lock");
    return NULL;
  }
  assert(self->rlock_count == 0);
  self->rlock_interp = interp;
  self->rlock_thread = thread;
  self->rlock_count = count;
  Py_RETURN_NONE;
}

PyDoc_STRVAR(rlock_acquire_restore_doc,
             "_acquire_restore(state) -> None\n\
\n\
For internal use by `threading.Condition`.");

static PyObject *
rlock_PyBehaviors_release_save(rlockobject *self, PyObject *Py_UNUSED(ignored))
{
  uint64_t interp;
  unsigned long thread;
  uint64_t count;
  PyBehaviorsLockStatus r;

  if (self->rlock_count == 0)
  {
    PyErr_SetString(PyExc_RuntimeError,
                    "cannot release un-acquired lock");
    return NULL;
  }

  interp = self->rlock_interp;
  thread = self->rlock_thread;
  count = self->rlock_count;
  self->rlock_count = 0;
  self->rlock_thread = 0;
  self->rlock_interp = 0;
  r = PyBehaviors_release_lock(self->rlock_lock);
  if (r != PY_BEHAVIORS_LOCK_SUCCESS)
  {
    PyErr_SetString(PyExc_RuntimeError,
                    "cannot release lock");
    return NULL;
  }

  return Py_BuildValue("kkk", count, thread, interp);
}

PyDoc_STRVAR(rlock_release_save_doc,
             "_release_save() -> tuple\n\
\n\
For internal use by `threading.Condition`.");

static PyObject *
rlock_PyBehaviors_is_owned(rlockobject *self, PyObject *Py_UNUSED(ignored))
{
  uint64_t iid = PyInterpreterState_GetID(PyInterpreterState_Get());
  unsigned long tid = PyThread_get_thread_ident();

  if (self->rlock_count > 0 && self->rlock_thread == tid && self->rlock_interp == iid)
  {
    Py_RETURN_TRUE;
  }
  Py_RETURN_FALSE;
}

PyDoc_STRVAR(rlock_is_owned_doc,
             "_is_owned() -> bool\n\
\n\
For internal use by `threading.Condition`.");

static PyObject *
rlock_PyBehaviors_new(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
  rlockobject *self = (rlockobject *)type->tp_alloc(type, 0);
  if (self == NULL)
  {
    return NULL;
  }

  self->in_weakreflist = NULL;
  self->rlock_interp = 0;
  self->rlock_thread = 0;
  self->rlock_count = 0;
  self->rlock_lock = PyBehaviors_allocate_lock();

  if (self->rlock_lock == NULL)
  {
    Py_DECREF(self);
    return NULL;
  }
  return (PyObject *)self;
}

static PyObject *
rlock_PyBehaviors_repr(rlockobject *self)
{
  return PyUnicode_FromFormat("<%s %s object interp=%ld thread=%ld count=%lu at %p>",
                              self->rlock_count ? "locked" : "unlocked",
                              Py_TYPE(self)->tp_name, self->rlock_interp, self->rlock_thread,
                              self->rlock_count, self);
}

/*
 * This rlock implementation is meant, as much as possible, to be interchangeable with the Python Threads
 * version of RLock while operating across interpreters.
 */

static PyMethodDef rlock_methods[] = {
    {"acquire", _PyCFunction_CAST(rlock_PyBehaviors_acquire),
     METH_VARARGS | METH_KEYWORDS, rlock_acquire_doc},
    {"release", (PyCFunction)rlock_PyBehaviors_release,
     METH_NOARGS, rlock_release_doc},
    {"_is_owned", (PyCFunction)rlock_PyBehaviors_is_owned,
     METH_NOARGS, rlock_is_owned_doc},
    {"_acquire_restore", (PyCFunction)rlock_PyBehaviors_acquire_restore,
     METH_VARARGS, rlock_acquire_restore_doc},
    {"_release_save", (PyCFunction)rlock_PyBehaviors_release_save,
     METH_NOARGS, rlock_release_save_doc},
    {"__enter__", _PyCFunction_CAST(rlock_PyBehaviors_acquire),
     METH_VARARGS | METH_KEYWORDS, rlock_acquire_doc},
    {"__exit__", (PyCFunction)rlock_PyBehaviors_release,
     METH_VARARGS, rlock_release_doc},
    {NULL, NULL} /* Sentinel */
};

static PyMemberDef rlock_type_members[] = {
    {"__weaklistoffset__", T_PYSSIZET, offsetof(rlockobject, in_weakreflist), READONLY},
    {NULL},
};

static PyType_Slot rlock_type_slots[] = {
    {Py_tp_dealloc, (destructor)rlock_PyBehaviors_dealloc},
    {Py_tp_repr, (reprfunc)rlock_PyBehaviors_repr},
    {Py_tp_methods, rlock_methods},
    {Py_tp_alloc, PyType_GenericAlloc},
    {Py_tp_new, rlock_PyBehaviors_new},
    {Py_tp_members, rlock_type_members},
    {Py_tp_traverse, rlock_PyBehaviors_traverse},
    {0, 0},
};

static PyType_Spec rlock_type_spec = {
    .name = "_behaviors.RLock",
    .basicsize = sizeof(rlockobject),
    .flags = (Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE |
              Py_TPFLAGS_HAVE_GC | Py_TPFLAGS_IMMUTABLETYPE),
    .slots = rlock_type_slots,
};

typedef struct behaviors_state_s
{
  bool is_running;
} BehaviorsState;

static PyObject *behaviors_start(PyObject *self, PyObject *noargs)
{
  PyObject *behaviors, *ret;
  BehaviorsState *state;

  behaviors = PyImport_ImportModule("_behaviors");
  if (behaviors == NULL)
  {
    PyErr_SetString(PyExc_RuntimeError, "Unable to import behaviors module");
    return NULL;
  }

  ret = Py_MakeGlobalsImmutable();
  if (ret == NULL)
  {
    Py_DECREF(behaviors);
    return NULL;
  }

  state = (BehaviorsState *)PyModule_GetState(behaviors);
  state->is_running = true;
  Py_DECREF(behaviors);
  Py_RETURN_NONE;
}

static PyObject *behaviors_running(PyObject *self, PyObject *noargs)
{
  PyObject *behaviors;
  BehaviorsState *state;
  bool is_running;

  behaviors = PyImport_ImportModule("_behaviors");
  if (behaviors == NULL)
  {
    PyErr_SetString(PyExc_RuntimeError, "Unable to import behaviors module");
    return NULL;
  }

  state = (BehaviorsState *)PyModule_GetState(behaviors);
  is_running = state->is_running;
  Py_DECREF(behaviors);

  if (is_running)
  {
    Py_RETURN_TRUE;
  }
  else
  {
    Py_RETURN_FALSE;
  }
}

static PyObject *behaviors_wait(PyObject *self, PyObject *noargs)
{
  PyObject *behaviors;
  BehaviorsState *state;

  behaviors = PyImport_ImportModule("_behaviors");
  if (behaviors == NULL)
  {
    PyErr_SetString(PyExc_RuntimeError, "Unable to import behaviors module");
    return NULL;
  }

  state = (BehaviorsState *)PyModule_GetState(behaviors);
  state->is_running = false;
  Py_DECREF(behaviors);
  Py_RETURN_NONE;
}

static PyObject *behaviors_get_ident(PyObject *self, PyObject *noargs)
{
  int64_t iid = PyInterpreterState_GetID(PyInterpreterState_Get());
  unsigned long tid = PyThread_get_thread_ident();
  return Py_BuildValue("(k,k)", iid, tid);
}

static PyMethodDef behaviors_methods[] = {
    {"start", (PyCFunction)behaviors_start, METH_NOARGS, NULL},
    {"running", (PyCFunction)behaviors_running, METH_NOARGS, NULL},
    {"wait", (PyCFunction)behaviors_wait, METH_NOARGS, NULL},
    {"get_ident", (PyCFunction)behaviors_get_ident, METH_NOARGS, NULL},
    {NULL} /* Sentinel */
};

static int behaviors_exec(PyObject *module)
{
  // Lock
  PyTypeObject *lock_type = (PyTypeObject *)PyType_FromSpec(&lock_type_spec);
  if (lock_type == NULL)
  {
    return -1;
  }
  if (PyModule_AddType(module, lock_type) < 0)
  {
    Py_DECREF(lock_type);
    return -1;
  }
  Py_DECREF(lock_type);

  // RLock
  PyTypeObject *rlock_type = (PyTypeObject *)PyType_FromSpec(&rlock_type_spec);
  if (rlock_type == NULL)
  {
    return -1;
  }
  if (PyModule_AddType(module, rlock_type) < 0)
  {
    Py_DECREF(rlock_type);
    return -1;
  }
  Py_DECREF(rlock_type);

  // BehaviorsState
  BehaviorsState *state = (BehaviorsState *)PyModule_GetState(module);
  state->is_running = false;

  return 0;
}

static void behaviors_free(PyObject *module)
{
}

#ifdef Py_mod_exec
static PyModuleDef_Slot behaviors_slots[] = {
    {Py_mod_exec, (void *)behaviors_exec},
    {Py_mod_multiple_interpreters, Py_MOD_PER_INTERPRETER_GIL_SUPPORTED},
    {0, NULL},
};
#endif

static PyModuleDef behaviorsmoduledef = {
    PyModuleDef_HEAD_INIT,
    .m_name = "behaviors",
    .m_methods = behaviors_methods,
    .m_free = (freefunc)behaviors_free,
#ifdef Py_mod_exec
    .m_slots = behaviors_slots,
#endif
    .m_size = sizeof(BehaviorsState)};

PyMODINIT_FUNC PyInit__behaviors(void)
{
#ifdef Py_mod_exec
  return PyModuleDef_Init(&behaviorsmoduledef);
#else
  PyObject *module;
  module = PyModule_Create(&behaviorsmoduledef);
  if (module == NULL)
    return NULL;

  if (behaviors_exec(module) != 0)
  {
    Py_DECREF(module);
    return NULL;
  }

  return module;
#endif
}