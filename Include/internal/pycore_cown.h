#ifndef Py_INTERNAL_COWN_H
#define Py_INTERNAL_COWN_H
#ifdef __cplusplus
extern "C" {
#endif

#ifndef Py_BUILD_CORE
#  error "Py_BUILD_CORE must be defined to include this header"
#endif

#include "object.h"
#include "exports.h"

typedef struct _PyCownObject _PyCownObject;
#define _PyCownObject_CAST(op) _Py_CAST(_PyCownObject*, op)

PyAPI_DATA(PyTypeObject) _PyCown_Type;

typedef uint64_t _PyCown_ipid_t;
typedef uint64_t _PyCown_thread_id_t;

PyAPI_FUNC(_PyCown_ipid_t) _PyCown_ThisInterpreterId(void);
PyAPI_FUNC(_PyCown_thread_id_t) _PyCown_ThisThreadId(void);

/* The interpreter currently owning the cown, or `_PyCown_ReleasedIpid()` when
 * no interpreter does. Safe to call from any interpreter. */
PyAPI_FUNC(_PyCown_ipid_t) _PyCown_Owner(PyObject *cown);
PyAPI_FUNC(_PyCown_ipid_t) _PyCown_ReleasedIpid(void);

/* The thread that acquired the cown, or `_PyCown_UnsetThreadId()` when it was
 * acquired without the GIL. Not enforced, only reported. */
PyAPI_FUNC(_PyCown_thread_id_t) _PyCown_LockingThread(PyObject *cown);
PyAPI_FUNC(_PyCown_thread_id_t) _PyCown_UnsetThreadId(void);


#ifdef __cplusplus
}
#endif
#endif /* !Py_INTERNAL_COWN_H */