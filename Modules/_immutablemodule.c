/* _immutable module */

#ifndef Py_BUILD_CORE_BUILTIN
#  define Py_BUILD_CORE_MODULE 1
#endif

#define MODULE_VERSION "1.0"

#include "Python.h"
#include <stdbool.h>
#include "pycore_object.h"
#include "pycore_immutability.h"
#include "pycore_critical_section.h"

/*[clinic input]
module _immutable
[clinic start generated code]*/
/*[clinic end generated code: output=da39a3ee5e6b4b0d input=292286c0a14cb0ff]*/

#include "clinic/_immutablemodule.c.h"

typedef struct {
    PyObject *not_freezable_error_obj;
    PyObject *interpreter_locals;  // dict: InterpreterLocal -> value
    PyObject *interpreterlocal_type;  // heap type object
    PyObject *sharedfield_type;  // heap type object
} immutable_state;

static struct PyModuleDef _immutablemodule;

static inline immutable_state*
get_immutable_state(PyObject *module)
{
    void *state = PyModule_GetState(module);
    assert(state != NULL);
    return (immutable_state *)state;
}

static int
immutable_clear(PyObject *module)
{
    immutable_state *module_state = PyModule_GetState(module);
    Py_CLEAR(module_state->not_freezable_error_obj);
    Py_CLEAR(module_state->interpreter_locals);
    Py_CLEAR(module_state->interpreterlocal_type);
    Py_CLEAR(module_state->sharedfield_type);
    return 0;
}

static int
immutable_traverse(PyObject *module, visitproc visit, void *arg)
{
    immutable_state *module_state = PyModule_GetState(module);
    Py_VISIT(module_state->not_freezable_error_obj);
    Py_VISIT(module_state->interpreter_locals);
    Py_VISIT(module_state->interpreterlocal_type);
    Py_VISIT(module_state->sharedfield_type);
    return 0;
}

static void
immutable_free(void *module)
{
   immutable_clear((PyObject *)module);
}

/*[clinic input]
_immutable.freeze
    *args: array

Freeze one or more objects and their graphs.
[clinic start generated code]*/

static PyObject *
_immutable_freeze_impl(PyObject *module, PyObject * const *args,
                       Py_ssize_t args_length)
/*[clinic end generated code: output=7be8a1c8b3aed004 input=6f071d066cb91bc8]*/
{
    if (args_length == 0) {
        PyErr_SetString(PyExc_TypeError,
                        "freeze() requires at least one argument");
        return NULL;
    }

    if (_PyImmutability_FreezeMany(args, args_length) < 0) {
        return NULL;
    }

    return Py_NewRef(args[0]);
}

/*[clinic input]
_immutable.is_frozen
    obj: object
    /

Check if an object is frozen (or can be viewed as immutable).

If the object graph can be viewed as immutable, it will be frozen as a
side effect and True is returned.
[clinic start generated code]*/

static PyObject *
_immutable_is_frozen(PyObject *module, PyObject *obj)
/*[clinic end generated code: output=880efe7d38b137b5 input=97c61fe65ccb1574]*/
{
    int result = _PyImmutability_CanViewAsImmutable(obj);
    if (result < 0) {
        return NULL;
    }
    if (result) {
        Py_RETURN_TRUE;
    }
    Py_RETURN_FALSE;
}

/*[clinic input]
_immutable.set_freezable
    obj: object
    status: int
    /

Set the freezable status of an object.

Status values:
  FREEZABLE_YES (0): always freezable
  FREEZABLE_NO (1): never freezable
  FREEZABLE_EXPLICIT (2): freezable only when freeze() is
                          called directly on it
  FREEZABLE_PROXY (3): reserved for future use
[clinic start generated code]*/

static PyObject *
_immutable_set_freezable_impl(PyObject *module, PyObject *obj, int status)
/*[clinic end generated code: output=73cad0b4df9a46f9 input=6528458c547e93a8]*/
{
    if (_PyImmutability_SetFreezable(obj, status) < 0) {
        return NULL;
    }
    Py_RETURN_NONE;
}

/*[clinic input]
_immutable.get_freezable
    obj: object
    /

Get the freezable status of an object.

Returns the freezable status, or -1 if no status has been set.
Status values:
  FREEZABLE_YES (0): always freezable
  FREEZABLE_NO (1): never freezable
  FREEZABLE_EXPLICIT (2): freezable only when freeze() is
                          called directly on it
  FREEZABLE_PROXY (3): reserved for future use
[clinic start generated code]*/

static PyObject *
_immutable_get_freezable(PyObject *module, PyObject *obj)
/*[clinic end generated code: output=bc22cd6d416850e3 input=a8ab19eb5ed3df08]*/
{
    int status = _PyImmutability_GetFreezable(obj);
    if (status == -2) {
        return NULL;  // Error occurred
    }
    return PyLong_FromLong(status);
}

/*[clinic input]
_immutable.unset_freezable
    obj: object
    /

Remove any explicitly set freezable status from an object.

After this call, get_freezable(obj) will no longer reflect a
per-object status and will fall back to the type's status (or
return -1 if neither has been set).
[clinic start generated code]*/

static PyObject *
_immutable_unset_freezable(PyObject *module, PyObject *obj)
/*[clinic end generated code: output=f4d96bda33ecbf57 input=96cdad3c7489fee0]*/
{
    if (_PyImmutability_UnsetFreezable(obj) < 0) {
        return NULL;
    }
    Py_RETURN_NONE;
}

// Artifact[Implementation]: The implementation of the `InterpreterLocal` type
/*
 * InterpreterLocal type
 *
 * An immutable indirection to per-interpreter mutable state.
 * tp_reachable hides per-interpreter values from the freeze walk.
 */

typedef struct {
    PyObject_HEAD
    PyObject *default_value;  // Immutable default, or NULL if factory form
    PyObject *factory;        // Frozen callable, or NULL if value form
} PyInterpreterLocalObject;

static PyObject *
interpreterlocal_get_locals(PyObject *self)
{
    PyObject *module = PyType_GetModuleByDef(Py_TYPE(self), &_immutablemodule);
    if (module == NULL) {
        return NULL;
    }
    return get_immutable_state(module)->interpreter_locals;
}

static PyObject *
interpreterlocal_lookup(PyInterpreterLocalObject *self)
{
    PyObject *locals = interpreterlocal_get_locals((PyObject *)self);
    if (locals == NULL) {
        return NULL;
    }

    PyObject *val = NULL;
    // Under free-threading (--disable-gil), multiple threads in the same
    // interpreter share this dict.  The critical section makes the
    // get-or-init compound operation atomic so the factory is called
    // at most once per interpreter.  On GIL builds this is a no-op,
    // which is safe because InterpreterLocal storage is per-interpreter
    // and each interpreter's GIL serializes its own threads.
    Py_BEGIN_CRITICAL_SECTION(locals);
    int ret = PyDict_GetItemRef(locals, (PyObject *)self, &val);
    if (ret == 0) {
        // Not found — initialise
        if (self->factory != NULL) {
            val = PyObject_CallNoArgs(self->factory);
        }
        else {
            val = Py_NewRef(self->default_value);
        }
        if (val != NULL) {
            if (PyDict_SetItem(locals, (PyObject *)self, val) < 0) {
                Py_CLEAR(val);
            }
        }
    }
    else if (ret < 0) {
        val = NULL;
    }
    Py_END_CRITICAL_SECTION();
    return val;
}

static PyObject *
interpreterlocal_get(PyObject *self, PyObject *Py_UNUSED(ignored))
{
    return interpreterlocal_lookup((PyInterpreterLocalObject *)self);
}

static PyObject *
interpreterlocal_set(PyObject *self, PyObject *value)
{
    PyObject *locals = interpreterlocal_get_locals(self);
    if (locals == NULL) {
        return NULL;
    }
    if (PyDict_SetItem(locals, self, value) < 0) {
        return NULL;
    }
    Py_RETURN_NONE;
}

static int
interpreterlocal_init(PyObject *self, PyObject *args, PyObject *kwds)
{
    static char *kwlist[] = {"default", NULL};
    PyObject *default_or_factory = NULL;
    PyInterpreterLocalObject *il = (PyInterpreterLocalObject *)self;

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "O", kwlist,
                                     &default_or_factory)) {
        return -1;
    }

    if (PyCallable_Check(default_or_factory)) {
        if (_PyImmutability_Freeze(default_or_factory) < 0) {
            return -1;
        }
        il->factory = Py_NewRef(default_or_factory);
        il->default_value = NULL;
    }
    else {
        if (_PyImmutability_Freeze(default_or_factory) < 0) {
            return -1;
        }
        il->default_value = Py_NewRef(default_or_factory);
        il->factory = NULL;
    }
    return 0;
}

static void
interpreterlocal_dealloc(PyObject *self)
{
    PyInterpreterLocalObject *il = (PyInterpreterLocalObject *)self;
    PyObject_GC_UnTrack(self);
    Py_CLEAR(il->default_value);
    Py_CLEAR(il->factory);
    PyTypeObject *tp = Py_TYPE(self);
    tp->tp_free(self);
    Py_DECREF(tp);
}

static int
interpreterlocal_traverse(PyObject *self, visitproc visit, void *arg)
{
    PyInterpreterLocalObject *il = (PyInterpreterLocalObject *)self;
    Py_VISIT(Py_TYPE(self));
    Py_VISIT(il->default_value);
    Py_VISIT(il->factory);
    return 0;
}

static int
interpreterlocal_reachable(PyObject *self, visitproc visit, void *arg)
{
    // Visit the type and the frozen fields.
    // Do NOT visit per-interpreter stored values — that's the escape hatch.
    PyInterpreterLocalObject *il = (PyInterpreterLocalObject *)self;
    Py_VISIT(Py_TYPE(self));
    Py_VISIT(il->default_value);
    Py_VISIT(il->factory);
    return 0;
}

static PyMethodDef interpreterlocal_methods[] = {
    {"get", interpreterlocal_get, METH_NOARGS,
     "Return the value for the current interpreter."},
    {"set", interpreterlocal_set, METH_O,
     "Set the value for the current interpreter."},
    {NULL, NULL}
};

static PyType_Slot interpreterlocal_slots[] = {
    {Py_tp_dealloc, interpreterlocal_dealloc},
    {Py_tp_init, interpreterlocal_init},
    {Py_tp_methods, interpreterlocal_methods},
    {Py_tp_traverse, interpreterlocal_traverse},
    {Py_tp_reachable, interpreterlocal_reachable},
    {Py_tp_new, PyType_GenericNew},
    {Py_tp_alloc, PyType_GenericAlloc},
    {Py_tp_free, PyObject_GC_Del},
    {0, NULL},
};

static PyType_Spec interpreterlocal_spec = {
    .name = "_immutable.InterpreterLocal",
    .basicsize = sizeof(PyInterpreterLocalObject),
    .flags = (Py_TPFLAGS_DEFAULT | Py_TPFLAGS_IMMUTABLETYPE |
              Py_TPFLAGS_HAVE_GC),
    .slots = interpreterlocal_slots,
};

// Artifact[Implementation]: The implementation of the `SharedField` type

/*
 * SharedField type
 *
 * A mutable field inside a frozen object that only holds frozen values.
 * Because the stored value is always immutable, it can be safely shared
 * across sub-interpreters.  All access is protected by a PyMutex to
 * avoid TOCTOU races between reading the pointer and adjusting reference
 * counts.  A PyMutex is used rather than Py_BEGIN_CRITICAL_SECTION
 * because the latter is a no-op on GIL-enabled builds, but SharedField
 * can be accessed concurrently from different sub-interpreters (each
 * with its own GIL).
 */
typedef struct {
    PyObject_HEAD
    PyObject *value;   // Always frozen; guarded by lock
    PyMutex lock;      // Protects value across sub-interpreters
} PySharedFieldObject;

static int
sharedfield_check_frozen(PyObject *value)
{
    if (!_PyImmutability_CanViewAsImmutable(value)) {
        PyErr_SetString(PyExc_TypeError,
                        "SharedField value must be frozen");
        return -1;
    }
    return 0;
}

static PyObject *
sharedfield_get(PyObject *self, PyObject *Py_UNUSED(ignored))
{
    PySharedFieldObject *sf = (PySharedFieldObject *)self;
    PyMutex_Lock(&sf->lock);
    PyObject *val = Py_NewRef(sf->value);
    PyMutex_Unlock(&sf->lock);
    return val;
}

static PyObject *
sharedfield_swap(PyObject *self, PyObject *new_value)
{
    if (sharedfield_check_frozen(new_value) < 0) {
        return NULL;
    }
    PySharedFieldObject *sf = (PySharedFieldObject *)self;
    Py_INCREF(new_value);
    PyMutex_Lock(&sf->lock);
    PyObject *old = sf->value;
    sf->value = new_value;
    PyMutex_Unlock(&sf->lock);
    return old;  // caller owns the reference
}

static PyObject *
sharedfield_set(PyObject *self, PyObject *new_value)
{
    PyObject *old = sharedfield_swap(self, new_value);
    if (old == NULL) {
        return NULL;
    }
    Py_DECREF(old);
    Py_RETURN_NONE;
}

static PyObject *
sharedfield_compare_and_swap(PyObject *self, PyObject *const *args,
                             Py_ssize_t nargs)
{
    if (nargs != 2) {
        PyErr_SetString(PyExc_TypeError,
                        "compare_and_swap() requires exactly 2 arguments"
                        " (old, new)");
        return NULL;
    }
    PyObject *expected = args[0];
    PyObject *new_value = args[1];

    if (sharedfield_check_frozen(new_value) < 0) {
        return NULL;
    }

    PySharedFieldObject *sf = (PySharedFieldObject *)self;
    int swapped;
    PyObject *old = NULL;
    Py_INCREF(new_value);
    PyMutex_Lock(&sf->lock);
    if (sf->value == expected) {
        old = sf->value;
        sf->value = new_value;
        swapped = 1;
    }
    else {
        swapped = 0;
    }
    PyMutex_Unlock(&sf->lock);
    if (swapped) {
        Py_DECREF(old);
        Py_RETURN_TRUE;
    }
    Py_DECREF(new_value);
    Py_RETURN_FALSE;
}

static int
sharedfield_init(PyObject *self, PyObject *args, PyObject *kwds)
{
    static char *kwlist[] = {"value", NULL};
    PyObject *initial = NULL;
    PySharedFieldObject *sf = (PySharedFieldObject *)self;

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "O", kwlist, &initial)) {
        return -1;
    }

    if (_PyImmutability_Freeze(initial) < 0) {
        return -1;
    }

    sf->value = Py_NewRef(initial);
    return 0;
}

static void
sharedfield_dealloc(PyObject *self)
{
    PySharedFieldObject *sf = (PySharedFieldObject *)self;
    PyObject_GC_UnTrack(self);
    Py_CLEAR(sf->value);
    PyTypeObject *tp = Py_TYPE(self);
    tp->tp_free(self);
    Py_DECREF(tp);
}

static int
sharedfield_traverse(PyObject *self, visitproc visit, void *arg)
{
    Py_VISIT(Py_TYPE(self));
    return 0;
}

static int
sharedfield_reachable(PyObject *self, visitproc visit, void *arg)
{
    Py_VISIT(Py_TYPE(self));
    return 0;
}

static PyMethodDef sharedfield_methods[] = {
    {"get", sharedfield_get, METH_NOARGS,
     "Return the current value."},
    {"set", sharedfield_set, METH_O,
     "Set a new value. The value must be frozen."},
    {"swap", sharedfield_swap, METH_O,
     "Replace the value and return the old value. The new value must be frozen."},
    {"compare_and_swap",
     _PyCFunction_CAST(sharedfield_compare_and_swap), METH_FASTCALL,
     "compare_and_swap(old, new) -> bool.\n"
     "If the current value is `old`, replace it with `new` and return True.\n"
     "Otherwise return False. The new value must be frozen."},
    {NULL, NULL}
};

static PyType_Slot sharedfield_slots[] = {
    {Py_tp_dealloc, sharedfield_dealloc},
    {Py_tp_init, sharedfield_init},
    {Py_tp_methods, sharedfield_methods},
    {Py_tp_traverse, sharedfield_traverse},
    {Py_tp_reachable, sharedfield_reachable},
    {Py_tp_new, PyType_GenericNew},
    {Py_tp_alloc, PyType_GenericAlloc},
    {Py_tp_free, PyObject_GC_Del},
    {0, NULL},
};

static PyType_Spec sharedfield_spec = {
    .name = "_immutable.SharedField",
    .basicsize = sizeof(PySharedFieldObject),
    .flags = (Py_TPFLAGS_DEFAULT | Py_TPFLAGS_IMMUTABLETYPE |
              Py_TPFLAGS_HAVE_GC),
    .slots = sharedfield_slots,
};


static PyType_Slot not_freezable_error_slots[] = {
    {0, NULL},
};

PyType_Spec not_freezable_error_spec = {
    .name = "_immutable.NotFreezableError",
    .flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,
    .slots = not_freezable_error_slots,
};

/*
 * MODULE
 */


PyDoc_STRVAR(immutable_module_doc,
"_immutable\n"
"--\n"
"\n"
"Module for immutability support.\n"
"\n"
"This module provides functions to freeze objects and their graphs,\n"
"making them immutable at runtime.");

static struct PyMethodDef immutable_methods[] = {
    _IMMUTABLE_FREEZE_METHODDEF
    _IMMUTABLE_IS_FROZEN_METHODDEF
    _IMMUTABLE_SET_FREEZABLE_METHODDEF
    _IMMUTABLE_GET_FREEZABLE_METHODDEF
    _IMMUTABLE_UNSET_FREEZABLE_METHODDEF
    { NULL, NULL }
};


static int
immutable_exec(PyObject *module) {
    immutable_state *module_state = get_immutable_state(module);

    /* Add version to the module. */
    if (PyModule_AddStringConstant(module, "__version__",
                                    MODULE_VERSION) == -1) {
        return -1;
    }

    PyObject *bases = PyTuple_Pack(1, PyExc_TypeError);
    if (bases == NULL) {
        return -1;
    }
    module_state->not_freezable_error_obj = PyType_FromModuleAndSpec(module, &not_freezable_error_spec,
                                                        bases);
    Py_DECREF(bases);
    if (module_state->not_freezable_error_obj == NULL) {
        return -1;
    }

    if (PyModule_AddType(module, (PyTypeObject *)module_state->not_freezable_error_obj) != 0) {
        return -1;
    }

    if (PyModule_AddType(module, &_PyImmModule_Type) != 0) {
        return -1;
    }

    /* Create InterpreterLocal heap type */
    module_state->interpreterlocal_type = PyType_FromModuleAndSpec(
        module, &interpreterlocal_spec, NULL);
    if (module_state->interpreterlocal_type == NULL) {
        return -1;
    }
    if (PyModule_AddType(module,
                         (PyTypeObject *)module_state->interpreterlocal_type) != 0) {
        return -1;
    }
    if (_PyImmutability_SetFreezable(
            module_state->interpreterlocal_type, _Py_FREEZABLE_YES) < 0) {
        return -1;
    }

    /* Create per-interpreter locals dict */
    module_state->interpreter_locals = PyDict_New();
    if (module_state->interpreter_locals == NULL) {
        return -1;
    }
    if (_PyImmutability_SetFreezable(
            module_state->interpreter_locals, _Py_FREEZABLE_NO) < 0) {
        return -1;
    }

    /* Create SharedField heap type */
    module_state->sharedfield_type = PyType_FromModuleAndSpec(
        module, &sharedfield_spec, NULL);
    if (module_state->sharedfield_type == NULL) {
        return -1;
    }
    if (PyModule_AddType(module,
                         (PyTypeObject *)module_state->sharedfield_type) != 0) {
        return -1;
    }
    if (_PyImmutability_SetFreezable(
            module_state->sharedfield_type, _Py_FREEZABLE_YES) < 0) {
        return -1;
    }

    if (PyModule_AddIntConstant(module, "FREEZABLE_YES",
                                _Py_FREEZABLE_YES) != 0) {
        return -1;
    }
    if (PyModule_AddIntConstant(module, "FREEZABLE_NO",
                                _Py_FREEZABLE_NO) != 0) {
        return -1;
    }
    if (PyModule_AddIntConstant(module, "FREEZABLE_EXPLICIT",
                                _Py_FREEZABLE_EXPLICIT) != 0) {
        return -1;
    }
    if (PyModule_AddIntConstant(module, "FREEZABLE_PROXY",
                                _Py_FREEZABLE_PROXY) != 0) {
        return -1;
    }

    return 0;
}

static PyModuleDef_Slot immutable_slots[] = {
    {Py_mod_exec, immutable_exec},
    {Py_mod_multiple_interpreters, Py_MOD_PER_INTERPRETER_GIL_SUPPORTED},
    {Py_mod_gil, Py_MOD_GIL_NOT_USED}, // TODO(Immutable):  This is probably not true, just enabling to see what breaks.
    {0, NULL}
};

static struct PyModuleDef _immutablemodule = {
    PyModuleDef_HEAD_INIT,
    "_immutable",
    immutable_module_doc,
    sizeof(immutable_state),
    immutable_methods,
    immutable_slots,
    immutable_traverse,
    immutable_clear,
    immutable_free
};

PyMODINIT_FUNC
PyInit__immutable(void)
{
    return PyModuleDef_Init(&_immutablemodule);
}
