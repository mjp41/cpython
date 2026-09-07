#ifndef Py_CPYTHON_IMMUTABLE_H
#  error "this header file must not be included directly"
#endif

typedef enum {
    _Py_FREEZABLE_YES = 0,
    _Py_FREEZABLE_NO = 1,
    _Py_FREEZABLE_EXPLICIT = 2,
    _Py_FREEZABLE_PROXY = 3,
} _Py_freezable_status;

PyAPI_FUNC(int) _PyImmutability_Freeze(PyObject*);
PyAPI_FUNC(int) _PyImmutability_FreezeMany(PyObject *const *, Py_ssize_t);
PyAPI_FUNC(int) _PyImmutability_RegisterShallowImmutable(PyTypeObject*);
PyAPI_FUNC(int) _PyImmutability_CanViewAsImmutable(PyObject*);
PyAPI_FUNC(int) _PyImmutability_SetFreezable(PyObject *, _Py_freezable_status);
PyAPI_FUNC(int) _PyImmutability_GetFreezable(PyObject *);
PyAPI_FUNC(int) _PyImmutability_UnsetFreezable(PyObject *);
