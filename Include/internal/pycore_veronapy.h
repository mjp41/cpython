#ifndef Py_INTERNAL_VERONAPY_H
#define Py_INTERNAL_VERONAPY_H

#ifdef __cplusplus
extern "C" {
#endif

#ifndef Py_BUILD_CORE
#  error "this header requires Py_BUILD_CORE define"
#endif

#include "object.h"

#define _Py_DEFAULT_REGION 0

#define Py_CHECKWRITE(op) ((op) && _PyObject_CAST(op)->ob_region != _Py_IMMUTABLE)

#ifdef __cplusplus
}
#endif
#endif /* !Py_INTERNAL_VERONAPY_H */