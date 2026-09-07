#ifndef Py_INTERNAL_CELL_H
#define Py_INTERNAL_CELL_H

#include "pycore_critical_section.h"
#include "pycore_object.h"
#include "pycore_stackref.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef Py_BUILD_CORE
#  error "this header requires Py_BUILD_CORE define"
#endif

// Sets the cell contents to `value` and return previous contents. Steals a
// reference to `value`.
static inline PyObject *
PyCell_SwapTakeRef(PyCellObject *cell, PyObject *value, int* result)
{
    PyObject *old_value = NULL;
    *result = 0;
    Py_BEGIN_CRITICAL_SECTION(cell);
    if(Py_CHECKWRITE(cell)){
        old_value = cell->ob_ref;
        FT_ATOMIC_STORE_PTR_RELEASE(cell->ob_ref, value);
    }
    else {
        *result = -1;
        Py_XDECREF(value);
    }
    Py_END_CRITICAL_SECTION();
    return old_value;
}

static inline int
PyCell_SetTakeRef(PyCellObject *cell, PyObject *value)
{
    int result = 0;
    PyObject *old_value = PyCell_SwapTakeRef(cell, value, &result);
    Py_XDECREF(old_value);
    return result;
}

// Gets the cell contents. Returns a new reference.
static inline PyObject *
PyCell_GetRef(PyCellObject *cell)
{
    PyObject *res;
    Py_BEGIN_CRITICAL_SECTION(cell);
#ifdef Py_GIL_DISABLED
    res = _Py_XNewRefWithLock(cell->ob_ref);
#else
    res = Py_XNewRef(cell->ob_ref);
#endif
    Py_END_CRITICAL_SECTION();
    return res;
}

static inline _PyStackRef
_PyCell_GetStackRef(PyCellObject *cell)
{
    PyObject *value;
#ifdef Py_GIL_DISABLED
    value = _Py_atomic_load_ptr(&cell->ob_ref);
    if (value == NULL) {
        return PyStackRef_NULL;
    }
    _PyStackRef ref;
    if (_Py_TryIncrefCompareStackRef(&cell->ob_ref, value, &ref)) {
        return ref;
    }
#endif
    value = PyCell_GetRef(cell);
    if (value == NULL) {
        return PyStackRef_NULL;
    }
    return PyStackRef_FromPyObjectSteal(value);
}

#ifdef __cplusplus
}
#endif
#endif   /* !Py_INTERNAL_CELL_H */
