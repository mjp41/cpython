#include "Python.h"
#include <ctype.h>
#include <semaphore.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "methodobject.h"
#include "modsupport.h"
#include "object.h"
#include "pycore_ast.h"
#include "pycore_dict.h"
#include "pycore_interp.h"
#include "pycore_object.h"
#include "pycore_regions.h"
#include "pycore_pyerrors.h"
#include "pycore_atomic.h"
#include "pyerrors.h"
#include "pystate.h"

typedef enum {
    Cown_RELEASED        = 0,
    Cown_ACQUIRED        = 1,
    Cown_PENDING_RELEASE = 2,
} CownState;

typedef struct PyCownObject {
    PyObject_HEAD
    _Py_atomic_int state;
    size_t owning_thread;
    sem_t semaphore;
    PyObject* value;
} PyCownObject;

static PyObject *PyCown_set_unchecked(PyCownObject *self, PyObject *arg);
static PyObject *PyCown_set(PyCownObject *self, PyObject *arg);
static PyObject *PyCown_get(PyCownObject *self);
static PyObject *PyCown_acquire(PyCownObject *self);

#define POSIX_FAIL_GUARD(exp) \
    if ((exp)) {              \
        fprintf(stderr, "Unsuccessful return from %s", #exp); \
        abort();              \
    }

static void PyCown_dealloc(PyCownObject *self) {
    POSIX_FAIL_GUARD(sem_destroy(&self->semaphore));

    PyTypeObject *tp = Py_TYPE(self);
    PyObject_GC_UnTrack((PyObject *)self);
    Py_TRASHCAN_BEGIN(self, PyCown_dealloc)
    Py_CLEAR(self->value);
    PyObject_GC_Del(self);
    Py_DECREF(tp);
    Py_TRASHCAN_END
}

static int PyCown_init(PyCownObject *self, PyObject *args, PyObject *kwds) {
    // TODO: should not be needed in the future
    _Py_MakeImmutable(_PyObject_CAST(Py_TYPE(self)));
    _Py_notify_regions_in_use();

    POSIX_FAIL_GUARD(sem_init(&self->semaphore, 0, 0));
    Py_SET_REGION(self, _Py_COWN);

    static char *kwlist[] = {"value", NULL};
    PyObject *value = NULL;

    // See if we got a value as a keyword argument
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "|O", kwlist, &value)) {
        return -1;  // Return -1 on failure
    }

    if (value) {
        PyCown_set_unchecked(self, value);

    } else {
        _Py_atomic_store(&self->state, Cown_RELEASED);
        self->value = Py_None;
    }
    return 0;
}

static int PyCown_traverse(PyCownObject *self, visitproc visit, void *arg) {
    Py_VISIT(self->value);
    return 0;
}

#define STATE(op) op->state._value

#define BAIL_IF_OWNED(o, msg) \
    do { \
        /* Note: we must hold the GIL at this point -- note for future threading implementation. */ \
        if (o->owning_thread != 0) { \
            PyErr_Format(PyExc_RegionError, "%s: %S -- %zd", msg, o, o->owning_thread); \
            return NULL; \
        } \
    } while(0);

#define BAIL_UNLESS_OWNED(o, msg) \
    do { \
        /* Note: we must hold the GIL at this point -- note for future threading implementation. */ \
        PyThreadState *tstate = PyThreadState_Get(); \
        if (o->owning_thread != tstate->thread_id) { \
            PyErr_Format(PyExc_RegionError, "%s: %S", msg, o); \
            return NULL; \
        } \
    } while(0);

#define BAIL_UNLESS_IN_STATE(o, expected_state, msg) \
    do { \
        /* Note: we must hold the GIL at this point -- note for future threading implementation. */ \
        if (STATE(o) != expected_state) { \
            PyErr_Format(PyExc_RegionError, "%s: %S", msg, o); \
            return NULL; \
        } \
    } while(0);

#define BAIL_UNLESS_ACQUIRED(o, msg) \
    BAIL_UNLESS_OWNED(o, msg) \
    BAIL_UNLESS_IN_STATE(o, Cown_ACQUIRED, msg)

static PyObject *PyCown_acquire(PyCownObject *self) {
    int expected = Cown_RELEASED;

    // TODO: eventually replace this with something from pycore_atomic (nothing there now)
    while (!atomic_compare_exchange_strong(&self->state._value, &expected, Cown_ACQUIRED)) {
        sem_wait(&self->semaphore);
        expected = Cown_RELEASED;
    }

    // Note: we must hold the GIL at this point -- note for future
    // threading implementation.
    PyThreadState *tstate = PyThreadState_Get();
    self->owning_thread = tstate->thread_id;

    Py_RETURN_NONE;
}

static PyObject *PyCown_release(PyCownObject *self) {
    if (STATE(self) == Cown_RELEASED) {
        BAIL_IF_OWNED(self, "BUG: Released cown had owning thread: %p");
        Py_RETURN_NONE;
    }

    BAIL_UNLESS_OWNED(self, "Thread attempted to release a cown it did not own");

    self->owning_thread = 0;
    _Py_atomic_store(&self->state, Cown_RELEASED);
    sem_post(&self->semaphore);

    Py_RETURN_NONE;
}

int _PyCown_release(PyObject *self) {
    PyObject* res = PyCown_release((PyCownObject *)self);
    return res == Py_None ? 0 : -1;
}

int _PyCown_is_released(PyObject *self) {
    PyCownObject *cown = (PyCownObject *)self;
    return STATE(cown) == Cown_RELEASED;
}

static PyObject *PyCown_get(PyCownObject *self) {
    BAIL_UNLESS_ACQUIRED(self, "Attempt to get value of unacquired cown");

    if (self->value) {
        return Py_NewRef(self->value);
    } else {
        Py_RETURN_NONE;
    }
}

// Needed to test for region object
extern PyTypeObject PyRegion_Type;
extern PyTypeObject PyCown_Type;

static PyObject *PyCown_set_unchecked(PyCownObject *self, PyObject *arg) {
    // Cowns are cells that hold a reference to a bridge object,
    // (or another cown or immutable object)
    const bool is_region_object =
        Py_IS_TYPE(arg, &PyRegion_Type) && _Py_is_bridge_object(arg);
    if (is_region_object ||
        arg->ob_type == &PyCown_Type ||
        _Py_IsImmutable(arg)) {

        PyObject* old = self->value;
        Py_XINCREF(arg);
        self->value = arg;

        // Tell the region that it is owned by a cown,
        // to enable it to release the cown on close
        if (is_region_object) {
            _PyRegion_set_cown_parent(arg, _PyObject_CAST(self));
            if (_PyRegion_is_closed(arg)) {
                PyCown_release(self);
            } else {
                _Py_atomic_store(&self->state, Cown_PENDING_RELEASE);
                // TODO: do we need an owning thread for this?
                PyThreadState *tstate = PyThreadState_Get();
                self->owning_thread = tstate->thread_id;
            }
        } else {
            // We can release this cown immediately
            // _Py_atomic_store(&self->state, Cown_RELEASED);
            // self->owning_thread = 0;
            PyCown_release(self);
        }

        return old ? old : Py_None;
    } else {
        // Invalid cown content
        PyErr_SetString(PyExc_RuntimeError,
            "Cowns can only store bridge objects, immutable objects or other cowns!");
        return NULL;
    }
}

static PyObject *PyCown_set(PyCownObject *self, PyObject *arg) {
    BAIL_UNLESS_ACQUIRED(self, "Attempt to set value of unacquired cown");
    return PyCown_set_unchecked(self, arg);
}

static int PyCown_clear(PyCownObject *self) {
    Py_CLEAR(self->value);
    return 0;
}

static PyObject *PyCown_repr(PyCownObject *self) {
#ifdef PYDEBUG
    if (STATE(self) == Cown_ACQUIRED) {
        return PyUnicode_FromFormat(
            "Cown(status=acquired by thread %zd,value=%S)",
            PyThreadState_Get()->thread_id,
            PyObject_Repr(self->value)
        );
    } else {
        return PyUnicode_FromFormat(
            "Cown(status=%s,value=%S)",
            STATE(self) == Cown_RELEASED
                ? "released"
                : "pending-release",
            PyObject_Repr(self->value)
        );
    }
#else
    if (STATE(self) == Cown_ACQUIRED) {
        return PyUnicode_FromFormat(
            "Cown(status=acquired by thread %zd)",
            PyThreadState_Get()->thread_id
        );
    } else {
        return PyUnicode_FromFormat(
            "Cown(status=%s)",
            STATE(self) == Cown_RELEASED
                ? "released"
                : "pending-release"
        );
    }
#endif
}

// Define the CownType with methods
static PyMethodDef PyCown_methods[] = {
    {"acquire", (PyCFunction)PyCown_acquire, METH_NOARGS, "Acquire the cown."},
    {"release", (PyCFunction)PyCown_release, METH_NOARGS, "Release the cown."},
    {"get",     (PyCFunction)PyCown_get,     METH_NOARGS, "Get contents of acquired cown."},
    {"set",     (PyCFunction)PyCown_set,     METH_O, "Set contents of acquired cown."},
    {NULL}  // Sentinel
};


PyTypeObject PyCown_Type = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0)
    "Cown",                                  /* tp_name */
    sizeof(PyCownObject),                    /* tp_basicsize */
    0,                                       /* tp_itemsize */
    (destructor)PyCown_dealloc,              /* tp_dealloc */
    0,                                       /* tp_vectorcall_offset */
    0,                                       /* tp_getattr */
    0,                                       /* tp_setattr */
    0,                                       /* tp_as_async */
    (reprfunc)PyCown_repr,                   /* tp_repr */
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
    0,                                       /* tp_doc */
    (traverseproc)PyCown_traverse,           /* tp_traverse */
    (inquiry)PyCown_clear,                   /* tp_clear */
    0,                                       /* tp_richcompare */
    0,                                       /* tp_weaklistoffset */
    0,                                       /* tp_iter */
    0,                                       /* tp_iternext */
    PyCown_methods,                          /* tp_methods */
    0,                                       /* tp_members */
    0,                                       /* tp_getset */
    0,                                       /* tp_base */
    0,                                       /* tp_dict */
    0,                                       /* tp_descr_get */
    0,                                       /* tp_descr_set */
    0,                                       /* tp_dictoffset */
    (initproc)PyCown_init,                   /* tp_init */
    0,                                       /* tp_alloc */
    PyType_GenericNew,                       /* tp_new */
};
