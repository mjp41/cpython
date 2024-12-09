#ifndef Py_REGIONS_H
#define Py_REGIONS_H
#ifdef __cplusplus
extern "C" {
#endif

#include "object.h"

PyAPI_FUNC(int) _Py_IsImmutable(PyObject *op);
#define Py_IsImmutable(op) _Py_IsImmutable(_PyObject_CAST(op))


PyAPI_FUNC(int) _Py_IsLocal(PyObject *op);
#define Py_IsLocal(op) _Py_IsLocal(_PyObject_CAST(op))

#ifdef __cplusplus
}
#endif
#endif   // !Py_REGIONS_H
