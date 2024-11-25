#ifndef Py_INTERNAL_REGIONS_H
#define Py_INTERNAL_REGIONS_H

#ifdef __cplusplus
extern "C" {
#endif

#ifndef Py_BUILD_CORE
#  error "this header requires Py_BUILD_CORE define"
#endif

#include "object.h"

#define Py_CHECKWRITE(op) ((op) && _PyObject_CAST(op)->ob_region != _Py_IMMUTABLE)
#define Py_REQUIREWRITE(op, msg) {if (Py_CHECKWRITE(op)) { _PyObject_ASSERT_FAILED_MSG(op, msg); }}

PyObject* _Py_MakeImmutable(PyObject* obj);
#define Py_MakeImmutable(op) _Py_MakeImmutable(_PyObject_CAST(op))

PyObject* _Py_InvariantSrcFailure(void);
#define Py_InvariantSrcFailure() _Py_InvariantSrcFailure()

PyObject* _Py_InvariantTgtFailure(void);
#define Py_InvariantTgtFailure() _Py_InvariantTgtFailure()

PyObject* _Py_EnableInvariant(void);
#define Py_EnableInvariant() _Py_EnableInvariant()

PyObject* _Py_ResetInvariant(void);
#define Py_ResetInvariant() _Py_ResetInvariant()

#ifdef NDEBUG
#define _Py_VPYDBG(fmt, ...)
#define _Py_VPYDBGPRINT(fmt, ...)
#else
#define _Py_VPYDBG(fmt, ...) printf(fmt, ##__VA_ARGS__)
#define _Py_VPYDBGPRINT(op) PyObject_Print(_PyObject_CAST(op), stdout, 0)
#endif

int _Py_CheckRegionInvariant(PyThreadState *tstate);

#ifdef __cplusplus
}
#endif
#endif /* !Py_INTERNAL_VERONAPY_H */
