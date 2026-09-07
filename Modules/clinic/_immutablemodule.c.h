/*[clinic input]
preserve
[clinic start generated code]*/

#include "pycore_modsupport.h"    // _PyArg_CheckPositional()

PyDoc_STRVAR(_immutable_freeze__doc__,
"freeze($module, /, *args)\n"
"--\n"
"\n"
"Freeze one or more objects and their graphs.");

#define _IMMUTABLE_FREEZE_METHODDEF    \
    {"freeze", _PyCFunction_CAST(_immutable_freeze), METH_FASTCALL, _immutable_freeze__doc__},

static PyObject *
_immutable_freeze_impl(PyObject *module, PyObject * const *args,
                       Py_ssize_t args_length);

static PyObject *
_immutable_freeze(PyObject *module, PyObject *const *args, Py_ssize_t nargs)
{
    PyObject *return_value = NULL;
    PyObject * const *__clinic_args;
    Py_ssize_t args_length;

    __clinic_args = args;
    args_length = nargs;
    return_value = _immutable_freeze_impl(module, __clinic_args, args_length);

    return return_value;
}

PyDoc_STRVAR(_immutable_is_frozen__doc__,
"is_frozen($module, obj, /)\n"
"--\n"
"\n"
"Check if an object is frozen (or can be viewed as immutable).\n"
"\n"
"If the object graph can be viewed as immutable, it will be frozen as a\n"
"side effect and True is returned.");

#define _IMMUTABLE_IS_FROZEN_METHODDEF    \
    {"is_frozen", (PyCFunction)_immutable_is_frozen, METH_O, _immutable_is_frozen__doc__},

PyDoc_STRVAR(_immutable_set_freezable__doc__,
"set_freezable($module, obj, status, /)\n"
"--\n"
"\n"
"Set the freezable status of an object.\n"
"\n"
"Status values:\n"
"  FREEZABLE_YES (0): always freezable\n"
"  FREEZABLE_NO (1): never freezable\n"
"  FREEZABLE_EXPLICIT (2): freezable only when freeze() is\n"
"                          called directly on it\n"
"  FREEZABLE_PROXY (3): reserved for future use");

#define _IMMUTABLE_SET_FREEZABLE_METHODDEF    \
    {"set_freezable", _PyCFunction_CAST(_immutable_set_freezable), METH_FASTCALL, _immutable_set_freezable__doc__},

static PyObject *
_immutable_set_freezable_impl(PyObject *module, PyObject *obj, int status);

static PyObject *
_immutable_set_freezable(PyObject *module, PyObject *const *args, Py_ssize_t nargs)
{
    PyObject *return_value = NULL;
    PyObject *obj;
    int status;

    if (!_PyArg_CheckPositional("set_freezable", nargs, 2, 2)) {
        goto exit;
    }
    obj = args[0];
    status = PyLong_AsInt(args[1]);
    if (status == -1 && PyErr_Occurred()) {
        goto exit;
    }
    return_value = _immutable_set_freezable_impl(module, obj, status);

exit:
    return return_value;
}

PyDoc_STRVAR(_immutable_get_freezable__doc__,
"get_freezable($module, obj, /)\n"
"--\n"
"\n"
"Get the freezable status of an object.\n"
"\n"
"Returns the freezable status, or -1 if no status has been set.\n"
"Status values:\n"
"  FREEZABLE_YES (0): always freezable\n"
"  FREEZABLE_NO (1): never freezable\n"
"  FREEZABLE_EXPLICIT (2): freezable only when freeze() is\n"
"                          called directly on it\n"
"  FREEZABLE_PROXY (3): reserved for future use");

#define _IMMUTABLE_GET_FREEZABLE_METHODDEF    \
    {"get_freezable", (PyCFunction)_immutable_get_freezable, METH_O, _immutable_get_freezable__doc__},

PyDoc_STRVAR(_immutable_unset_freezable__doc__,
"unset_freezable($module, obj, /)\n"
"--\n"
"\n"
"Remove any explicitly set freezable status from an object.\n"
"\n"
"After this call, get_freezable(obj) will no longer reflect a\n"
"per-object status and will fall back to the type\'s status (or\n"
"return -1 if neither has been set).");

#define _IMMUTABLE_UNSET_FREEZABLE_METHODDEF    \
    {"unset_freezable", (PyCFunction)_immutable_unset_freezable, METH_O, _immutable_unset_freezable__doc__},
/*[clinic end generated code: output=fc9ce9e40597f581 input=a9049054013a1b77]*/
