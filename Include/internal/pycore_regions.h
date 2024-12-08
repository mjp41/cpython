#ifndef Py_INTERNAL_REGIONS_H
#define Py_INTERNAL_REGIONS_H

#ifdef __cplusplus
extern "C" {
#endif

#ifndef Py_BUILD_CORE
#  error "this header requires Py_BUILD_CORE define"
#endif

#include "object.h"

#define Py_CHECKWRITE(op) ((op) && !_Py_IsImmutable(op))
#define Py_REQUIREWRITE(op, msg) {if (Py_CHECKWRITE(op)) { _PyObject_ASSERT_FAILED_MSG(op, msg); }}

void _Py_SET_TAGGED_REGION(PyObject *ob, Py_region_ptr_with_tags_t region);
#if !defined(Py_LIMITED_API) || Py_LIMITED_API+0 < 0x030b0000
#  define Py_SET_TAGGED_REGION(ob, region) _Py_SET_TAGGED_REGION(_PyObject_CAST(ob), (region))
#endif

static inline void Py_SET_REGION(PyObject *ob, Py_region_ptr_t region) {
    _Py_SET_TAGGED_REGION(ob, Py_region_ptr_with_tags(region & Py_REGION_MASK));
}
#if !defined(Py_LIMITED_API) || Py_LIMITED_API+0 < 0x030b0000
#  define Py_SET_REGION(ob, region) Py_SET_REGION(_PyObject_CAST(ob), _Py_CAST(Py_region_ptr_t, (region)))
#endif

/* This makes the given objects and all object reachable from the given
 * object immutable. This will also move the objects into the immutable
 * region.
 *
 * The argument is borrowed, meaning that it expects the calling context
 * to handle the reference count.
 *
 * The function will return `Py_None` by default.
 */
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

// Invariant placeholder
bool _Pyrona_AddReference(PyObject* target, PyObject* new_ref);
#define Pyrona_ADDREFERENCE(a, b) _Pyrona_AddReference(a, b)
// Helper macros to count the number of arguments
#define _COUNT_ARGS(_1, _2, _3, _4, _5, _6, _7, _8, _9, _10, _11, _12, _13, _14, _15, _16, N, ...) N
#define COUNT_ARGS(...) _COUNT_ARGS(__VA_ARGS__, 16, 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1)

bool _Pyrona_AddReferences(PyObject* target, int new_refc, ...);
#define Pyrona_ADDREFERENCES(a, ...) _Pyrona_AddReferences(a, COUNT_ARGS(__VA_ARGS__), __VA_ARGS__)

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
