#ifndef Py_INTERNAL_VERONAPY_H
#define Py_INTERNAL_VERONAPY_H

#ifdef __cplusplus
extern "C" {
#endif

#ifndef Py_BUILD_CORE
#  error "this header requires Py_BUILD_CORE define"
#endif

#include "object.h"

#define Py_CHECKWRITE(op) ((op) && _PyObject_CAST(op)->ob_region != _Py_IMMUTABLE)

#ifdef NDEBUG
#define _Py_VPYDBG(fmt, ...)
#define _Py_VPYDBGPRINT(fmt, ...)
#else
#define _Py_VPYDBG(fmt, ...) printf(fmt, ##__VA_ARGS__)
#define _Py_VPYDBGPRINT(op) PyObject_Print(op, stdout, 0)
#endif

#ifdef __cplusplus
}
#endif
#endif /* !Py_INTERNAL_VERONAPY_H */