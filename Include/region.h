#ifndef Py_REGION_H
#define Py_REGION_H
#ifdef __cplusplus
extern "C" {
#endif

#include "object.h"
#include "exports.h"

typedef enum {
    Py_MOVABLE_YES = 0,
    Py_MOVABLE_NO = 1,
    Py_MOVABLE_FREEZE = 2,
} _Py_movable_status;

typedef Py_uintptr_t PyRegion_staged_ref_t;
#define PyRegion_staged_ref_ERR 0

PyAPI_FUNC(int) _PyRegion_IsLocal(PyObject *obj);
#define PyRegion_IsLocal(obj) _PyRegion_IsLocal(_PyObject_CAST(obj))

/** This function returns true if the object is inside a region and
 * would needs a write barrier to be called.
 */
static inline int _PyRegion_NeedsReadBarrier(PyObject *obj) {
    return !(obj == NULL || PyRegion_IsLocal(obj) || _Py_IsImmutable(obj));
}
#define PyRegion_NeedsReadBarrier(obj) _PyRegion_NeedsReadBarrier(_PyObject_CAST(obj))

PyAPI_FUNC(int) _PyRegion_SameRegion(PyObject *a, PyObject *b);
#define PyRegion_SameRegion(a, b) _PyRegion_SameRegion(_PyObject_CAST(a), _PyObject_CAST(b))

PyAPI_FUNC(_Py_movable_status) _PyRegion_GetMoveability(PyObject *obj);

// Helper macros to count the number of arguments
#define _PyRegion__COUNT_ARGS(_1, _2, _3, _4, _5, _6, _7, _8, _9, _10, _11, _12, _13, _14, _15, _16, N, ...) N
#define _PyRegion_COUNT_ARGS(...) _PyRegion__COUNT_ARGS(__VA_ARGS__, 16, 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1)
#define _PyRegion_MAX_ARG_COUNT 16

PyAPI_FUNC(PyRegion_staged_ref_t) _PyRegion_StageRefs(PyObject *src, int tgt_count, ...);
#define PyRegion_StageRef(src, tgt) _PyRegion_StageRefs(_PyObject_CAST(src), 1, _PyObject_CAST(tgt))
#define PyRegion_StageRefs(src, ...) _PyRegion_StageRefs(_PyObject_CAST(src), _PyRegion_COUNT_ARGS(__VA_ARGS__), __VA_ARGS__)
PyAPI_FUNC(void) PyRegion_ResetStagedRef(PyRegion_staged_ref_t staged_ref);
PyAPI_FUNC(void) PyRegion_CommitStagedRef(PyRegion_staged_ref_t staged_ref);

// FIXME(regions): xFrednet: AddStaged and TakeStaged instead of commit (Names?)

PyAPI_FUNC(int) _PyRegion_AddRef(PyObject *src, PyObject *tgt);
PyAPI_FUNC(int) _PyRegion_AddRefs(PyObject *src, int tgt_count, ...);
PyAPI_FUNC(int) _PyRegion_AddRefsArray(PyObject *src, int tgt_count, PyObject** tgt_array);
#define PyRegion_AddRef(src, tgt) _PyRegion_AddRef(_PyObject_CAST(src), _PyObject_CAST(tgt))
#define PyRegion_AddRefs(src, ...) _PyRegion_AddRefs(_PyObject_CAST(src), _PyRegion_COUNT_ARGS(__VA_ARGS__), __VA_ARGS__)
#define PyRegion_AddRefsArray(src, tgt_count, tgt_array) _PyRegion_AddRefsArray(_PyObject_CAST(src), tgt_count, tgt_array)

PyAPI_FUNC(void) _PyRegion_RemoveRef(PyObject *src, PyObject *tgt);
#define PyRegion_RemoveRef(src, tgt) _PyRegion_RemoveRef(_PyObject_CAST(src), _PyObject_CAST(tgt))

PyAPI_FUNC(int) _PyRegion_AddLocalRef(PyObject *tgt);
PyAPI_FUNC(int) _PyRegion_AddLocalRefs(int tgt_count, ...);
#define PyRegion_AddLocalRef(tgt) _PyRegion_AddLocalRef(_PyObject_CAST(tgt))
#define PyRegion_AddLocalRefs(...) _PyRegion_AddLocalRefs(_PyRegion_COUNT_ARGS(__VA_ARGS__), __VA_ARGS__)

PyAPI_FUNC(void) _PyRegion_RemoveLocalRef(PyObject *tgt);
#define PyRegion_RemoveLocalRef(tgt) _PyRegion_RemoveLocalRef(_PyObject_CAST(tgt))

static inline PyObject* _PyRegion_NewRef(PyObject* tgt) {
    PyRegion_AddLocalRef(tgt);
    return Py_NewRef(tgt);
}
static inline PyObject* _PyRegion_XNewRef(PyObject* tgt) {
    if (!tgt) {
        return NULL;
    }

    return _PyRegion_NewRef(tgt);
}
#define PyRegion_NewRef(tgt) _PyRegion_NewRef(_PyObject_CAST(tgt))
#define PyRegion_XNewRef(tgt) _PyRegion_XNewRef(_PyObject_CAST(tgt))

static inline int _PyRegion_TakeRef(PyObject *src, PyObject *tgt) {
    int res = _PyRegion_AddRef(src, tgt);
    if (res != 0) {
        return res;
    }

    // Removing the local reference here is safe. There are three
    // interesting cases which can happen with this function:
    //
    // - src is local & tgt is in region Y
    //      In this case, Y will remain open, since the `AddRef` call above
    //      bumped the LRC, basically making this a no-op.
    // - src and tgt are in the same region
    //      This call will reduce the LRC, but the region will remain open
    //      since there is a remaining local reference to src
    // - src is in region X and tgt is the bridge object of Y
    //      Removing the local reference may close Y, but X as the new parent
    //      region of Y will remain open. Closing of Y will therefore only
    //      modify the OSC of X but not close X. This ensures that no cown is
    //      released or send off, while we still have remaining references into
    //      X and Y.
    _PyRegion_RemoveLocalRef(tgt);
    return 0;
}
PyAPI_FUNC(int) _PyRegion_TakeRefs(PyObject *src, int tgt_count, ...);
#define PyRegion_TakeRef(src, tgt) _PyRegion_TakeRef(_PyObject_CAST(src), _PyObject_CAST(tgt))
#define PyRegion_TakeRefs(src, ...) _PyRegion_TakeRefs(_PyObject_CAST(src), _PyRegion_COUNT_ARGS(__VA_ARGS__), __VA_ARGS__)

static inline int _PyRegion_XSetRef(PyObject *src, PyObject **field, PyObject *val) {
    PyObject *old = *field;
    if (PyRegion_TakeRef(src, val)) {
        return 1;
    }
    *field = val;
    PyRegion_RemoveRef(src, old);
    Py_XDECREF(old);

    return 0;
}
static inline int _PyRegion_XSetNewRef(PyObject *src, PyObject **field, PyObject *val) {
    PyObject *old = *field;
    if (PyRegion_AddRef(src, val)) {
        return 1;
    }
    *field = Py_XNewRef(val);
    PyRegion_RemoveRef(src, old);
    Py_XDECREF(old);

    return 0;
}
#define PyRegion_XSETREF(src, dst, val) _PyRegion_XSetRef(_PyObject_CAST(src), (PyObject **)&(dst), _PyObject_CAST(val))
#define PyRegion_XSETNEWREF(src, dst, val) _PyRegion_XSetNewRef(_PyObject_CAST(src), (PyObject **)&(dst), _PyObject_CAST(val))

static inline int _PyRegion_SetLocalRef(PyObject **field, PyObject *val) {
    PyObject *old = *field;
    *field = val;
    PyRegion_RemoveLocalRef(old);
    Py_XDECREF(old);

    return 0;
}
static inline int _PyRegion_SetNewLocalRef(PyObject **field, PyObject *val) {
    PyObject *old = *field;
    if (PyRegion_AddLocalRef(val)) {
        return 1;
    }
    *field = Py_NewRef(val);
    PyRegion_RemoveLocalRef(old);
    Py_XDECREF(old);

    return 0;
}
#define PyRegion_XSETLOCALREF(dst, val) _PyRegion_SetLocalRef((PyObject **)&(dst), _PyObject_CAST(val))
#define PyRegion_XSETLOCALNEWREF(dst, val) _PyRegion_SetNewLocalRef((PyObject **)&(dst), _PyObject_CAST(val))

static inline void _PyRegion_Clear(PyObject *src, PyObject **field) {
    PyObject* old = *field;
    if (old) {
        *field = NULL;
        PyRegion_RemoveRef(src, old);
        Py_DECREF(old);
    }
}
#define PyRegion_CLEAR(src, dst) _PyRegion_Clear(_PyObject_CAST(src), (PyObject **)&(dst))
#define PyRegion_CLEARLOCAL(local) PyRegion_XSETLOCALREF(local, NULL)

PyAPI_FUNC(void) PyRegion_NotifyTypeUse(PyTypeObject* type);
PyAPI_FUNC(void) PyRegion_RecycleObject(PyObject *obj);
PyAPI_FUNC(void) PyRegion_DirtyObjectRegion(PyObject* obj);
PyAPI_FUNC(void) PyRegion_DirtyAllRegions(const char* reason);

#ifdef __cplusplus
}
#endif
#endif /* !Py_REGION_H */