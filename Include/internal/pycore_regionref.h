#ifndef Py_INTERNAL_REGIONREF_H
#define Py_INTERNAL_REGIONREF_H
#ifdef __cplusplus
extern "C" {
#endif

#ifndef Py_BUILD_CORE
#  error "Py_BUILD_CORE must be defined to include this header"
#endif

#include "pycore_cown.h"          // _PyCown_ipid_t
#include "pycore_hashtable.h"     // _Py_hashtable_t

/* Every `RegionRef` points at a `_PyRegionRefMetadata` node, and those nodes
 * form a tree that mirrors the region hierarchy: a nested region's node
 * delegates to the node of its parent, and the outermost node names the owner,
 * either an interpreter or the cown holding the region. A cown node looks the
 * owner up on the cown, which is what makes acquiring and releasing free;
 * moving a closed region between owners restamps a single node.
 *
 * A reference to an object that is in no region at all owns its own node.
 */

typedef enum {
    /* A close is in progress. The referenced data is unavailable but should be
     * soon; dereferencing fails and the caller may retry. Only ever reachable
     * through a child's `parent`, never directly from a reference. */
    _Py_REGION_REF_WIP,
    /* Delegates to `value.parent`, the node of the enclosing region. */
    _Py_REGION_REF_META,
    /* Terminal. The region is held by `value.cown`, on which the owner is
     * looked up dynamically. */
    _Py_REGION_REF_COWN,
    /* Terminal, owned by one interpreter. */
    _Py_REGION_REF_IPID,
} _PyRegionRefKind;

typedef struct _PyRegionRefMetadata {
    uint32_t rc;
    /* A `_PyRegionRefKind`. */
    uint8_t kind;
    /* Borrowed. Set only on region nodes, and only while that region is
     * closed. Names the region a dereference has to open on its way down.
     *
     * Borrowing is safe because the pointer is only followed after the terminal
     * check established that this interpreter owns the region, and a region can
     * only be deallocated by its owner. */
    PyObject *region;
    union {
        struct _PyRegionRefMetadata *parent;  /* META */
        PyObject *cown;                       /* COWN, borrowed */
        _PyCown_ipid_t ipid;                  /* IPID */
    } value;
} _PyRegionRefMetadata;

/* Creates the node of a region that is being closed. Returns a new reference. */
extern _PyRegionRefMetadata *_PyRegionRef_NewRegionMetaLockHeld(PyObject *region);

extern void _PyRegionRef_MetaDecref(_PyRegionRefMetadata *meta);

// Ownership transitions.
extern void _PyRegionRef_MetaSetParentLockHeld(_PyRegionRefMetadata *meta,
                                               _PyRegionRefMetadata *parent);
/* Hands the node to `cown`, which is borrowed. The owner is from then on
 * whoever holds the cown. */
extern void _PyRegionRef_MetaSetCown(_PyRegionRefMetadata *meta, PyObject *cown);
/* Stamps an explicit owner, which need not be the current interpreter and may
 * be `_PyCown_ReleasedIpid()` to mean nobody owns the region. */
extern void _PyRegionRef_MetaSetIpid(_PyRegionRefMetadata *meta,
                                     _PyCown_ipid_t ipid);
extern void _PyRegionRef_MetaRegionOpened(_PyRegionRefMetadata *meta);
extern void _PyRegionRef_MetaResolveWip(_PyRegionRefMetadata *meta);

/* Re-homes every `RegionRef` pointing at `obj` onto `region`'s node, allocating
 * that node if this is the first reference the close has found. Every other
 * weak reference to `obj` is cleared unless it is listed in `keep`.
 *
 * This is the region close hook; it replaces `_PyWeakref_ClearWeakRefsExcept()`
 * for objects that are being closed into a region. */
extern void _PyRegionRef_CloseWeakRefs(PyObject *obj, _Py_hashtable_t *keep,
                                       PyObject *region);

#ifdef __cplusplus
}
#endif
#endif /* !Py_INTERNAL_REGIONREF_H */
