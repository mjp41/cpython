#include "Python.h"
#include "pycore_critical_section.h"
#include "pycore_lock.h"
#include "pycore_modsupport.h"    // _PyArg_NoKwnames()
#include "pycore_object.h"        // _PyObject_GET_WEAKREFS_LISTPTR()
#include "pycore_pyerrors.h"      // _PyErr_ChainExceptions1()
#include "pycore_pystate.h"
#include "pycore_weakref.h"       // _PyWeakref_GET_REF()
#include "pycore_cown.h"          // _PyCown_ThisInterpreterId()
#include "pycore_immutability.h"  // _PyTracingRegion_Open()
#include "pycore_interp.h"        // PyInterpreterState.immutability
#include "pycore_regionref.h"

// FIXME(region): Reusing the same weakref easily breaks region isolation
// without a simple way out for programmers. For now we disable the optimization.
// in the future, we may create a new object every time, but make sure that
// the internal metadata is shared until this is no longer possible due to regions.
#define WEAKREF_REUSE_BASIC_REFS 0

#ifdef Py_GIL_DISABLED
/*
 * Thread-safety for free-threaded builds
 * ======================================
 *
 * In free-threaded builds we need to protect mutable state of:
 *
 * - The weakref (wr_object, hash, wr_callback)
 * - The referenced object (its head-of-list pointer)
 * - The linked list of weakrefs
 *
 * For now we've chosen to address this in a straightforward way:
 *
 * - The weakref's hash is protected using atomic operations.
 * - The other mutable is protected by a striped lock keyed on the referenced
 *   object's address.
 * - The striped lock must be locked using `_Py_LOCK_DONT_DETACH` in order to
 *   support atomic deletion from WeakValueDictionaries. As a result, we must
 *   be careful not to perform any operations that could suspend while the
 *   lock is held.
 *
 * Since the world is stopped when the GC runs, it is free to clear weakrefs
 * without acquiring any locks.
 */

#else
// Artifact[Implementation]: Explanation how weak references work for immutable objects
/*
 * Thread-safety for immutable objects
 * ===================================
 *
 * Immutable objects, and their weakref lists, are shared across interpreters.
 * Moreover, basic weakrefs pointing to immutable objects are shared.
 * We need to protect mutable state of:
 *
 * - The weakref (wr_object, hash, wr_callback)
 * - The referenced object (its head-of-list pointer)
 * - The linked list of weakrefs
 *
 * For now we've chosen to address this in the following way:
 *
 * - The weakref's hash is protected using atomic operations.
 * - The other mutable state is protected by a global lock.
 * - The lock must be locked using `_Py_LOCK_DONT_DETACH` in order to
 *   support atomic deletion from WeakValueDictionaries. As a result, we must
 *   be careful not to perform any operations that could suspend while the
 *   lock is held.
 *
 * We also need to handle refcounts for the weakref object and the callback.
 *
 * - Basic weakrefs pointing to immutable objects are marked as immutable,
 *   which turns on atomic reference counting.
 * - Weakrefs with callbacks and pointing to immutable objects
 *   have their refcount pre-emptively incremented upon creation.
 *   That accounts for the TryIncref that would be called when clearing
 *   weakrefs, which would require atomic reference counting.
 *   However, we cannot easily achieve atomic reference counting for weakrefs
 *   with callbacks: we cannot make them immutable, and adding another branch
 *   to PY_INCREF and PY_DECREF would have a significant performance impact.
 *   The downside of our approach is that the weakref objects are kept alive
 *   until the immutable object dies.
 *   FIXME(Immutable): If the weakref is a part of an SCC, it never dies.
 * - We keep the callback in the weakref object until it is about to be called.
 *   That keeps it alive, so we don't need to increment its refcount.
 *
 * Calling the callback is tricky because it can reside on a different
 * interpreter than the interpreter that triggered deallocation.
 * Therefore, we keep track of the original interpreter of the callback.
 * When the immutable object is being deallocated, we schedule the callbacks
 * to be called on their original interpreters using an asynchronous call.
 * Once all callbacks have been called, we continue deallocating the object.
 *
 * Immutable objects are never GC-collected.
 */
#endif

PyMutex _PyWeakref_Lock;

#define GET_WEAKREFS_LISTPTR(o) \
        ((PyWeakReference **) _PyObject_GET_WEAKREFS_LISTPTR(o))


Py_ssize_t
_PyWeakref_GetWeakrefCount(PyObject *obj)
{
    if (!_PyType_SUPPORTS_WEAKREFS(Py_TYPE(obj))) {
        return 0;
    }

    LOCK_WEAKREFS(obj);
    Py_ssize_t count = 0;
    PyWeakReference *head = *GET_WEAKREFS_LISTPTR(obj);
    while (head != NULL) {
        ++count;
        head = head->wr_next;
    }
    UNLOCK_WEAKREFS(obj);
    return count;
}

// ###################################################################
// Region reference metadata
// ###################################################################

/* See `pycore_regionref.h` for the design.
 *
 * Every field of a `_PyRegionRefMetadata` is guarded by `_PyWeakref_Lock`. The
 * `_lock_held` helpers below expect the caller to hold it.
 */

/* In the default build the weakref list lock and the metadata lock are the same
 * global mutex, so code holding the former must not take the latter again. In
 * free-threaded builds they differ. */
#ifdef Py_GIL_DISABLED
#  define LOCK_META_UNDER_WEAKREFS()   LOCK_REGION_REF_META()
#  define UNLOCK_META_UNDER_WEAKREFS() UNLOCK_REGION_REF_META()
#else
#  define LOCK_META_UNDER_WEAKREFS()   ((void)0)
#  define UNLOCK_META_UNDER_WEAKREFS() ((void)0)
#endif

static void clear_weakref_lock_held(PyWeakReference *self, PyObject **callback);

static _PyRegionRefMetadata *
meta_new_lock_held(uint8_t kind)
{
    // Raw allocation on purpose: a node can outlive the interpreter that
    // created it, when a chain reaching it is still held elsewhere.
    _PyRegionRefMetadata *meta = PyMem_RawMalloc(sizeof(_PyRegionRefMetadata));
    if (meta == NULL) {
        return NULL;
    }
    meta->rc = 1;
    meta->kind = kind;
    meta->region = NULL;
    memset(&meta->value, 0, sizeof(meta->value));
    return meta;
}

static void
meta_incref_lock_held(_PyRegionRefMetadata *meta)
{
    if (meta != NULL) {
        assert(meta->rc > 0);
        meta->rc += 1;
    }
}

static void
meta_decref_lock_held(_PyRegionRefMetadata *meta)
{
    // Releasing a node releases its parent, and a chain is as deep as the
    // region tree. Walk it instead of recursing.
    while (meta != NULL) {
        assert(meta->rc > 0);
        if (--meta->rc > 0) {
            return;
        }
        _PyRegionRefMetadata *parent = NULL;
        if (meta->kind == _Py_REGION_REF_META) {
            parent = meta->value.parent;
        }
        PyMem_RawFree(meta);
        meta = parent;
    }
}

/* Returns a terminal node owned by this interpreter, for a reference to an
 * object that is in no region. New reference, NULL when out of memory. */
static _PyRegionRefMetadata *
meta_new_local_lock_held(void)
{
    _PyRegionRefMetadata *meta = meta_new_lock_held(_Py_REGION_REF_IPID);
    if (meta != NULL) {
        meta->value.ipid = _PyCown_ThisInterpreterId();
    }
    return meta;
}

/* Releases whatever the node delegated to. The callers below all assign the new
 * kind and value right after, so the stale union is never observed. */
static void
meta_clear_parent_lock_held(_PyRegionRefMetadata *meta)
{
    if (meta->kind == _Py_REGION_REF_META) {
        _PyRegionRefMetadata *parent = meta->value.parent;
        meta->kind = _Py_REGION_REF_WIP;
        meta_decref_lock_held(parent);
    }
}

static void
meta_set_parent_lock_held(_PyRegionRefMetadata *meta, _PyRegionRefMetadata *parent)
{
    assert(meta != NULL && parent != NULL);
    assert(meta != parent);

    // Increfing first keeps a self-assignment from freeing the parent.
    meta_incref_lock_held(parent);
    meta_clear_parent_lock_held(meta);
    meta->kind = _Py_REGION_REF_META;
    meta->value.parent = parent;
}

static void
meta_set_cown_lock_held(_PyRegionRefMetadata *meta, PyObject *cown)
{
    meta_clear_parent_lock_held(meta);
    meta->kind = _Py_REGION_REF_COWN;
    meta->value.cown = cown;
}

static void
meta_set_ipid_lock_held(_PyRegionRefMetadata *meta, _PyCown_ipid_t ipid)
{
    meta_clear_parent_lock_held(meta);
    meta->kind = _Py_REGION_REF_IPID;
    meta->value.ipid = ipid;
}

static void
set_region_ref_lock_held(PyWeakReference *self, _PyRegionRefMetadata *meta)
{
    // FIXME(regions): Why does this fail? assert(_PyRegionRef_CheckExact(self));
    if (self->region_ref == meta) {
        return;
    }
    meta_incref_lock_held(meta);
    meta_decref_lock_held(self->region_ref);
    self->region_ref = meta;
}

/* Drops a reference's metadata while its weakref list lock is held. */
static void
clear_region_ref_lock_held(PyWeakReference *self)
{
    // FIXME(regions): Why does this fail? assert(_PyRegionRef_CheckExact(self));
    LOCK_META_UNDER_WEAKREFS();
    set_region_ref_lock_held(self, NULL);
    UNLOCK_META_UNDER_WEAKREFS();
}

_PyRegionRefMetadata *
_PyRegionRef_NewRegionMetaLockHeld(PyObject *region)
{
    _PyRegionRefMetadata *meta = meta_new_lock_held(_Py_REGION_REF_WIP);
    if (meta != NULL) {
        meta->region = region;
    }
    return meta;
}

void
_PyRegionRef_MetaDecref(_PyRegionRefMetadata *meta)
{
    LOCK_REGION_REF_META();
    meta_decref_lock_held(meta);
    UNLOCK_REGION_REF_META();
}

void
_PyRegionRef_MetaSetParentLockHeld(_PyRegionRefMetadata *meta,
                                   _PyRegionRefMetadata *parent)
{
    meta_set_parent_lock_held(meta, parent);
}

void
_PyRegionRef_MetaSetCown(_PyRegionRefMetadata *meta, PyObject *cown)
{
    LOCK_REGION_REF_META();
    meta_set_cown_lock_held(meta, cown);
    UNLOCK_REGION_REF_META();
}

void
_PyRegionRef_MetaSetIpid(_PyRegionRefMetadata *meta, _PyCown_ipid_t ipid)
{
    // FIXME(regions): `ipid` should always be the current interpreter. It isn't
    // for a released cown, or when `PyCown_clear` runs on an interpreter that
    // doesn't own the cown; once that is refactored this can assert it.
    LOCK_REGION_REF_META();
    meta_set_ipid_lock_held(meta, ipid);
    UNLOCK_REGION_REF_META();
}

void
_PyRegionRef_MetaRegionOpened(_PyRegionRefMetadata *meta)
{
    LOCK_REGION_REF_META();
    meta->region = NULL;
    meta_set_ipid_lock_held(meta, _PyCown_ThisInterpreterId());
    UNLOCK_REGION_REF_META();
}

void
_PyRegionRef_MetaResolveWip(_PyRegionRefMetadata *meta)
{
    LOCK_REGION_REF_META();
    if (meta->kind == _Py_REGION_REF_WIP) {
        meta_set_ipid_lock_held(meta, _PyCown_ThisInterpreterId());
    }
    UNLOCK_REGION_REF_META();
}

void
_PyRegionRef_CloseWeakRefs(PyObject *obj, _Py_hashtable_t *keep, PyObject *region)
{
    PyWeakReference **list = _PyObject_GET_WEAKREFS_LISTPTR_FROM_OFFSET(obj);
    LOCK_WEAKREFS(obj);
    LOCK_META_UNDER_WEAKREFS();
    while (*list) {
        PyWeakReference *ref = *list;

        // Region references remain in the list, but their region reference meta
        // is repointed.
        if (_PyRegionRef_CheckExact((PyObject *)ref)) {
            _PyRegionRefMetadata *meta = _PyTracingRegion_MetaLockHeld(region);
            if (meta == NULL) {
                // Out of memory, we clear the reference and continue
                clear_weakref_lock_held(ref, NULL);
                continue;
            }
            set_region_ref_lock_held(ref, meta);
            list = &ref->wr_next;
        }
        else if (keep != NULL && _Py_hashtable_get_entry(keep, ref)) {
            list = &ref->wr_next;
        }
        else {
            clear_weakref_lock_held(ref, NULL);
        }
    }
    UNLOCK_META_UNDER_WEAKREFS();
    UNLOCK_WEAKREFS(obj);
}

// ###################################################################
// Region reference access
// ###################################################################

/* The regions a dereference has to open, innermost first. Most chains are
 * shallow, so the common case stays on the stack. */
#define REGIONREF_OPEN_STACK 8

typedef struct {
    PyObject **items;
    Py_ssize_t count;
    Py_ssize_t capacity;
    PyObject *stack[REGIONREF_OPEN_STACK];
} regionref_open_list_t;

static void
open_list_init(regionref_open_list_t *list)
{
    list->items = list->stack;
    list->count = 0;
    list->capacity = REGIONREF_OPEN_STACK;
}

static void
open_list_clear(regionref_open_list_t *list)
{
    if (list->items != list->stack) {
        PyMem_RawFree(list->items);
    }
    open_list_init(list);
}

/* Grows with the raw allocator so this stays safe to call under the metadata
 * lock, which must not run Python code. */
static int
open_list_push(regionref_open_list_t *list, PyObject *region)
{
    if (list->count == list->capacity) {
        Py_ssize_t capacity = list->capacity * 2;
        PyObject **items;
        if (list->items == list->stack) {
            items = PyMem_RawMalloc(capacity * sizeof(PyObject *));
            if (items != NULL) {
                memcpy(items, list->stack, list->count * sizeof(PyObject *));
            }
        }
        else {
            items = PyMem_RawRealloc(list->items, capacity * sizeof(PyObject *));
        }
        if (items == NULL) {
            return -1;
        }
        list->items = items;
        list->capacity = capacity;
    }
    list->items[list->count++] = region;
    return 0;
}

typedef enum {
    REGIONREF_ALLOWED,
    REGIONREF_DENIED_WIP,
    REGIONREF_DENIED_IPID,
    REGIONREF_DENIED_COWN,
    REGIONREF_DENIED_MEMORY,
} regionref_verdict_t;

/* Resolves the reference's metadata chain and decides whether this interpreter
 * may reach the target. On success `regions` lists the regions that still have
 * to be opened, innermost first; pass NULL to only ask the question.
 *
 * Returns 0 when access is allowed, -1 with an exception set otherwise.
 */
static int
regionref_check_access(PyWeakReference *self, regionref_open_list_t *regions,
                       bool quiet)
{
    const _PyCown_ipid_t this_ip = _PyCown_ThisInterpreterId();
    regionref_verdict_t verdict = REGIONREF_ALLOWED;
    _PyCown_ipid_t owner = 0;
    _PyCown_thread_id_t locking_thread = 0;
    bool wrong_thread = false;

    // Nothing inside this section may raise or allocate through Python.
    LOCK_REGION_REF_META();
    _PyRegionRefMetadata *meta = self->region_ref;
    while (meta != NULL) {
        if (meta->region != NULL && regions != NULL) {
            if (open_list_push(regions, meta->region) < 0) {
                verdict = REGIONREF_DENIED_MEMORY;
                break;
            }
        }
        if (meta->kind != _Py_REGION_REF_META) {
            break;
        }
        meta = meta->value.parent;
    }
    if (verdict == REGIONREF_ALLOWED && meta != NULL) {
        switch (meta->kind) {
        case _Py_REGION_REF_WIP:
            verdict = REGIONREF_DENIED_WIP;
            break;
        case _Py_REGION_REF_IPID:
            owner = meta->value.ipid;
            if (owner != this_ip) {
                verdict = REGIONREF_DENIED_IPID;
            }
            break;
        case _Py_REGION_REF_COWN:
            owner = _PyCown_Owner(meta->value.cown);
            if (owner != this_ip) {
                verdict = REGIONREF_DENIED_COWN;
            }
            else {
                locking_thread = _PyCown_LockingThread(meta->value.cown);
                wrong_thread = locking_thread != _PyCown_UnsetThreadId()
                               && locking_thread != _PyCown_ThisThreadId();
            }
            break;
        default:
            Py_UNREACHABLE();
        }
    }
    // A NULL node means the target was frozen, which makes it reachable from
    // everywhere. Every live region reference has a node from birth.
    UNLOCK_REGION_REF_META();

    if (wrong_thread) {
        // FIXME(regions): Thread ownership is not enforced, any thread of the
        // owning interpreter may reach the data. Whether that should change is
        // a question for once this has seen some use.
        fprintf(stderr,
                "RegionRef dereferenced from thread %llu, but the cown was "
                "acquired by thread %llu\n",
                (unsigned long long)_PyCown_ThisThreadId(),
                (unsigned long long)locking_thread);
    }

    if (verdict == REGIONREF_ALLOWED) {
        return 0;
    }
    if (quiet) {
        // Callers that only want the answer. Raising here and having them
        // clear it would destroy whatever the caller already had pending;
        // `repr()` in particular runs from error reporting paths.
        return -1;
    }

    switch (verdict) {
    case REGIONREF_ALLOWED:
        return 0;
    case REGIONREF_DENIED_MEMORY:
        PyErr_NoMemory();
        return -1;
    case REGIONREF_DENIED_WIP:
        PyErr_SetString(
            PyExc_RuntimeError,
            "the region holding this reference is currently being closed");
        return -1;
    case REGIONREF_DENIED_COWN:
        if (owner == _PyCown_ReleasedIpid()) {
            PyErr_Format(
                PyExc_RuntimeError,
                "interpreter %llu attempted to dereference a region reference "
                "into a released cown",
                (unsigned long long)this_ip);
            return -1;
        }
        _Py_FALLTHROUGH;
    case REGIONREF_DENIED_IPID:
        if (owner == _PyCown_ReleasedIpid()) {
            PyErr_Format(
                PyExc_RuntimeError,
                "interpreter %llu attempted to dereference a region reference "
                "into a region that no interpreter owns",
                (unsigned long long)this_ip);
            return -1;
        }
        PyErr_Format(
            PyExc_RuntimeError,
            "interpreter %llu attempted to dereference a region reference "
            "into a region owned by %llu",
            (unsigned long long)this_ip, (unsigned long long)owner);
        return -1;
    }
    Py_UNREACHABLE();
}

/* Returns a new strong reference to the target.
 *
 * Returns NULL without an exception when the target simply died, and NULL with
 * one set when this interpreter may not reach it.
 */
static PyObject *
regionref_get_ref(PyObject *op)
{
    PyWeakReference *self = _PyWeakref_CAST(op);

    // A dead target needs no ownership check; it is not in any region any more.
    if (_Py_atomic_load_ptr(&self->wr_object) == Py_None) {
        return NULL;
    }

    regionref_open_list_t regions;
    open_list_init(&regions);
    if (regionref_check_access(self, &regions, false) < 0) {
        open_list_clear(&regions);
        return NULL;
    }

    // Opening runs no Python code but does move GC lists, so it happens with
    // the metadata lock dropped. Parent regions first, so an open region never has a
    // closed ancestor. The borrowed region pointers stay valid because the
    // check above established that this interpreter owns them, and only an
    // owner can deallocate a region.
    for (Py_ssize_t i = regions.count - 1; i >= 0; i--) {
        _PyTracingRegion_Open(regions.items[i]);
    }
    open_list_clear(&regions);

    PyObject *obj = _Py_atomic_load_ptr(&self->wr_object);
    if (obj == Py_None) {
        return NULL;
    }
    LOCK_WEAKREFS(obj);
    PyObject *result = get_ref_lock_held(self, obj);
    UNLOCK_WEAKREFS(obj);
    return result;
}

static PyObject *weakref_vectorcall(PyObject *self, PyObject *const *args, size_t nargsf, PyObject *kwnames);
static PyObject *regionref_vectorcall(PyObject *self, PyObject *const *args, size_t nargsf, PyObject *kwnames);

static void
init_weakref(PyWeakReference *self, PyObject *ob, PyObject *callback)
{
    self->hash = -1;
    self->wr_object = ob;
    self->wr_prev = NULL;
    self->wr_next = NULL;
    self->wr_callback = Py_XNewRef(callback);
    if (callback == NULL) {
        self->callback_ipid = -1;
    }
    else {
        self->callback_ipid = PyInterpreterState_GetID(PyInterpreterState_Get());
    }
    // A region reference has to run its ownership check before handing out the
    // target, so it cannot share the plain weakref fast path.
    self->vectorcall = _PyRegionRef_CheckExact((PyObject *)self)
                       ? regionref_vectorcall : weakref_vectorcall;
#ifdef Py_GIL_DISABLED
    self->weakrefs_lock = &WEAKREF_LIST_LOCK(ob);
    _PyObject_SetMaybeWeakref(ob);
    _PyObject_SetMaybeWeakref((PyObject *)self);
#endif
    self->region_ref = NULL;
}

// Clear the weakref and steal its callback into `callback`, if provided.
static void
clear_weakref_lock_held(PyWeakReference *self, PyObject **callback)
{
    if (self->wr_object != Py_None) {
        PyWeakReference **list = GET_WEAKREFS_LISTPTR(self->wr_object);
        if (*list == self) {
            /* If 'self' is the end of the list (and thus self->wr_next ==
               NULL) then the weakref list itself (and thus the value of *list)
               will end up being set to NULL. */
            _Py_atomic_store_ptr(list, self->wr_next);
        }
        _Py_atomic_store_ptr(&self->wr_object, Py_None);
        if (self->wr_prev != NULL) {
            self->wr_prev->wr_next = self->wr_next;
        }
        if (self->wr_next != NULL) {
            self->wr_next->wr_prev = self->wr_prev;
        }
        self->wr_prev = NULL;
        self->wr_next = NULL;
    }
    if (callback != NULL) {
        *callback = self->wr_callback;
        self->wr_callback = NULL;
    }
    clear_region_ref_lock_held(self);
}

// Clear the weakref and its callback
static void
clear_weakref(PyObject *op)
{
    PyWeakReference *self = _PyWeakref_CAST(op);
    PyObject *callback = NULL;

    // self->wr_object may be Py_None if the GC cleared the weakref, so lock
    // using the pointer in the weakref.
    LOCK_WEAKREFS_FOR_WR(self);
    clear_weakref_lock_held(self, &callback);
    UNLOCK_WEAKREFS_FOR_WR(self);
    Py_XDECREF(callback);
}


/* Cyclic gc uses this to *just* clear the passed-in reference, leaving
 * the callback intact and uncalled.  It must be possible to call self's
 * tp_dealloc() after calling this, so self has to be left in a sane enough
 * state for that to work.  We expect tp_dealloc to decref the callback
 * then.  The reason for not letting clear_weakref() decref the callback
 * right now is that if the callback goes away, that may in turn trigger
 * another callback (if a weak reference to the callback exists) -- running
 * arbitrary Python code in the middle of gc is a disaster.  The convolution
 * here allows gc to delay triggering such callbacks until the world is in
 * a sane state again.
 */
void
_PyWeakref_ClearRef(PyWeakReference *self)
{
    assert(self != NULL);
    // Region references reuse this struct without being a weakref subtype.
    assert(_PyWeakrefOrRegionRef_Check(self));
    // Callers here hold no lock, but `region_ref` needs one. Callers that
    // already hold it use `clear_weakref_lock_held()` directly.
    LOCK_REGION_REF_META();
    clear_weakref_lock_held(self, NULL);
    UNLOCK_REGION_REF_META();
}

static void
weakref_dealloc(PyObject *self)
{
    PyObject_GC_UnTrack(self);
    clear_weakref(self);
    Py_TYPE(self)->tp_free(self);
}


static int
gc_traverse(PyObject *op, visitproc visit, void *arg)
{
    PyWeakReference *self = _PyWeakref_CAST(op);
    Py_VISIT(self->wr_callback);
    return 0;
}


static int
gc_clear(PyObject *op)
{
    PyWeakReference *self = _PyWeakref_CAST(op);
    PyObject *callback;
    // The world is stopped during GC in free-threaded builds. It's safe to
    // call this without holding the list lock. `region_ref` still needs the
    // metadata lock in the default build, where each interpreter has its own
    // GIL and the collector is not alone.
    LOCK_REGION_REF_META();
    clear_weakref_lock_held(self, &callback);
    UNLOCK_REGION_REF_META();
    Py_XDECREF(callback);
    return 0;
}


static PyObject *
weakref_vectorcall(PyObject *self, PyObject *const *args,
                   size_t nargsf, PyObject *kwnames)
{
    if (!_PyArg_NoKwnames("weakref", kwnames)) {
        return NULL;
    }
    Py_ssize_t nargs = PyVectorcall_NARGS(nargsf);
    if (!_PyArg_CheckPositional("weakref", nargs, 0, 0)) {
        return NULL;
    }
    PyObject *obj = _PyWeakref_GET_REF(self);
    if (obj == NULL) {
        Py_RETURN_NONE;
    }
    return obj;
}

static Py_hash_t
weakref_hash(PyObject *op)
{
    // Immutable objects and free-threaded builds require atomic operations
    PyWeakReference *self = _PyWeakref_CAST(op);
    Py_hash_t hash = _Py_atomic_load_ssize_relaxed(&self->hash);
    if (hash != -1) {
        return hash;
    }
    PyObject* obj = _PyWeakref_GET_REF((PyObject*)self);
    if (obj == NULL) {
        PyErr_SetString(PyExc_TypeError, "weak object has gone away");
        return -1;
    }
    hash = PyObject_Hash(obj);
    Py_DECREF(obj);
    _Py_atomic_store_ssize_relaxed(&self->hash, hash);
    return hash;
}

static PyObject *
weakref_repr(PyObject *self)
{
    PyObject* obj = _PyWeakref_GET_REF(self);
    if (obj == NULL) {
        return PyUnicode_FromFormat("<weakref at %p; dead>", self);
    }

    PyObject *name = _PyObject_LookupSpecial(obj, &_Py_ID(__name__));
    PyObject *repr;
    if (name == NULL || !PyUnicode_Check(name)) {
        repr = PyUnicode_FromFormat(
            "<weakref at %p; to '%T' at %p>",
            self, obj, obj);
    }
    else {
        repr = PyUnicode_FromFormat(
            "<weakref at %p; to '%T' at %p (%U)>",
            self, obj, obj, name);
    }
    Py_DECREF(obj);
    Py_XDECREF(name);
    return repr;
}

/* Weak references only support equality, not ordering. Two weak references
   are equal if the underlying objects are equal. If the underlying object has
   gone away, they are equal if they are identical. */

static PyObject *
weakref_richcompare(PyObject* self, PyObject* other, int op)
{
    if ((op != Py_EQ && op != Py_NE) ||
        !PyWeakref_Check(self) ||
        !PyWeakref_Check(other)) {
        Py_RETURN_NOTIMPLEMENTED;
    }
    PyObject* obj = _PyWeakref_GET_REF(self);
    PyObject* other_obj = _PyWeakref_GET_REF(other);
    if (obj == NULL || other_obj == NULL) {
        Py_XDECREF(obj);
        Py_XDECREF(other_obj);
        int res = (self == other);
        if (op == Py_NE)
            res = !res;
        if (res)
            Py_RETURN_TRUE;
        else
            Py_RETURN_FALSE;
    }
    PyObject* res = PyObject_RichCompare(obj, other_obj, op);
    Py_DECREF(obj);
    Py_DECREF(other_obj);
    return res;
}

/* Given the head of an object's list of weak references, extract the
 * two callback-less refs (ref and proxy).  Used to determine if the
 * shared references exist and to determine the back link for newly
 * inserted references.
 */
static void
get_basic_refs(PyWeakReference *head,
               PyWeakReference **refp, PyWeakReference **proxyp)
{
    *refp = NULL;
    *proxyp = NULL;

    if (head != NULL && head->wr_callback == NULL) {
        /* We need to be careful that the "basic refs" aren't
           subclasses of the main types.  That complicates this a
           little. */
        if (PyWeakref_CheckRefExact(head)) {
            *refp = head;
            head = head->wr_next;
        }
        if (head != NULL
            && head->wr_callback == NULL
            && PyWeakref_CheckProxy(head)) {
            *proxyp = head;
            /* head = head->wr_next; */
        }
    }
}

/* Insert 'newref' in the list after 'prev'.  Both must be non-NULL. */
static void
insert_after(PyWeakReference *newref, PyWeakReference *prev)
{
    newref->wr_prev = prev;
    newref->wr_next = prev->wr_next;
    if (prev->wr_next != NULL)
        prev->wr_next->wr_prev = newref;
    prev->wr_next = newref;
}

/* Insert 'newref' at the head of the list; 'list' points to the variable
 * that stores the head.
 */
static void
insert_head(PyWeakReference *newref, PyWeakReference **list)
{
    PyWeakReference *next = *list;

    newref->wr_prev = NULL;
    newref->wr_next = next;
    if (next != NULL)
        next->wr_prev = newref;
    *list = newref;
}

/* See if we can reuse either the basic ref or proxy in list instead of
 * creating a new weakref
 */
static PyWeakReference *
try_reuse_basic_ref(PyWeakReference *list, PyTypeObject *type,
                    PyObject *callback)
{
    if (!WEAKREF_REUSE_BASIC_REFS || callback != NULL) {
        return NULL;
    }

    PyWeakReference *ref, *proxy;
    get_basic_refs(list, &ref, &proxy);

    PyWeakReference *cand = NULL;
    if (type == &_PyWeakref_RefType) {
        cand = ref;
    }
    if ((type == &_PyWeakref_ProxyType) ||
        (type == &_PyWeakref_CallableProxyType)) {
        cand = proxy;
    }

    if (cand == NULL) {
        return NULL;
    }
    PyObject* candobj = _PyObject_CAST(cand);
    int incref_res = _Py_IsImmutable(candobj) ?
        _Py_TryIncref_Immutable(candobj) : _Py_TryIncref(candobj);
    if (incref_res) {
        return cand;
    }
    return NULL;
}

static int
is_basic_ref(PyWeakReference *ref)
{
    return (ref->wr_callback == NULL) && PyWeakref_CheckRefExact(ref);
}

static int
is_basic_proxy(PyWeakReference *proxy)
{
    return (proxy->wr_callback == NULL) && PyWeakref_CheckProxy(proxy);
}

static int
is_basic_ref_or_proxy(PyWeakReference *wr)
{
    return is_basic_ref(wr) || is_basic_proxy(wr);
}

/* Insert `newref` in the appropriate position in `list` */
static void
insert_weakref(PyWeakReference *newref, PyWeakReference **list)
{
    PyWeakReference *ref, *proxy;
    get_basic_refs(*list, &ref, &proxy);

    PyWeakReference *prev;
    if (is_basic_ref(newref)) {
        prev = NULL;
    }
    else if (is_basic_proxy(newref)) {
        prev = ref;
    }
    else {
        prev = (proxy == NULL) ? ref : proxy;
    }

    if (prev == NULL) {
        insert_head(newref, list);
    }
    else {
        insert_after(newref, prev);
    }
}

static void
immutable_make_weakref_safe(PyWeakReference *self)
{
    if (self->wr_callback == NULL) {
        // Turn on atomic reference counting for the weakref.
        // FIXME(Immutable): freezing a weakref makes it strong
        // _PyImmutability_Freeze(_PyObject_CAST(newref));
    }
    else {
        // Pre-emptively increment the weakref's refcount.
        // See the comment at the start of this file for details.
        Py_INCREF(self);
    }

}

/* Make weakrefs to the newly frozen object thread-safe. */
void
_PyWeakref_OnObjectFreeze(PyObject *object)
{
    assert(_Py_IsImmutable(object));
    if (!_PyType_SUPPORTS_WEAKREFS(Py_TYPE(object))) {
        return;
    }
    PyWeakReference **list = GET_WEAKREFS_LISTPTR(object);
    if (_Py_atomic_load_ptr(list) == NULL) {
        // Fast path for the common case
        return;
    }
    LOCK_WEAKREFS(object);
    LOCK_META_UNDER_WEAKREFS();
    PyWeakReference *current = *list;
    while (current != NULL) {
        // A frozen object is reachable from every interpreter, so a region
        // reference to it no longer needs an ownership check.
        set_region_ref_lock_held(current, NULL);
        immutable_make_weakref_safe(current);
        current = current->wr_next;
    }
    UNLOCK_META_UNDER_WEAKREFS();
    UNLOCK_WEAKREFS(object);
}

static PyWeakReference *
allocate_weakref(PyTypeObject *type, PyObject *obj, PyObject *callback)
{
    PyWeakReference *newref = (PyWeakReference *) type->tp_alloc(type, 0);
    if (newref == NULL) {
        return NULL;
    }
    init_weakref(newref, obj, callback);
    if (_Py_IsImmutable(obj)) {
        immutable_make_weakref_safe(newref);
    }
    return newref;
}

static PyWeakReference *
get_or_create_weakref(PyTypeObject *type, PyObject *obj, PyObject *callback)
{
    if (!_PyType_SUPPORTS_WEAKREFS(Py_TYPE(obj))) {
        PyErr_Format(PyExc_TypeError,
                     "cannot create weak reference to '%s' object",
                     Py_TYPE(obj)->tp_name);
        return NULL;
    }
    if (callback == Py_None)
        callback = NULL;

    PyWeakReference **list = GET_WEAKREFS_LISTPTR(obj);
    if ((type == &_PyWeakref_RefType) ||
        (type == &_PyWeakref_ProxyType) ||
        (type == &_PyWeakref_CallableProxyType))
    {
        LOCK_WEAKREFS(obj);
        PyWeakReference *basic_ref = try_reuse_basic_ref(*list, type, callback);
        if (basic_ref != NULL) {
            UNLOCK_WEAKREFS(obj);
            return basic_ref;
        }
        PyWeakReference *newref = allocate_weakref(type, obj, callback);
        if (newref == NULL) {
            UNLOCK_WEAKREFS(obj);
            return NULL;
        }
        insert_weakref(newref, list);
        UNLOCK_WEAKREFS(obj);
        return newref;
    }
    else {
        // We may not be able to safely allocate inside the lock
        PyWeakReference *newref = allocate_weakref(type, obj, callback);
        if (newref == NULL) {
            return NULL;
        }
        LOCK_WEAKREFS(obj);
        insert_weakref(newref, list);
        UNLOCK_WEAKREFS(obj);
        return newref;
    }
}

static int
parse_weakref_init_args(const char *funcname, PyObject *args, PyObject *kwargs,
                        PyObject **obp, PyObject **callbackp)
{
    return PyArg_UnpackTuple(args, funcname, 1, 2, obp, callbackp);
}

static PyObject *
weakref___new__(PyTypeObject *type, PyObject *args, PyObject *kwargs)
{
    PyObject *ob, *callback = NULL;
    if (parse_weakref_init_args("__new__", args, kwargs, &ob, &callback)) {
        return (PyObject *)get_or_create_weakref(type, ob, callback);
    }
    return NULL;
}

static int
weakref___init__(PyObject *self, PyObject *args, PyObject *kwargs)
{
    PyObject *tmp;

    if (!_PyArg_NoKeywords("ref", kwargs))
        return -1;

    if (parse_weakref_init_args("__init__", args, kwargs, &tmp, &tmp))
        return 0;
    else
        return -1;
}


static PyMemberDef weakref_members[] = {
    {"__callback__", _Py_T_OBJECT, offsetof(PyWeakReference, wr_callback), Py_READONLY},
    {NULL} /* Sentinel */
};

static PyMethodDef weakref_methods[] = {
    {"__class_getitem__",    Py_GenericAlias,
    METH_O|METH_CLASS,       PyDoc_STR("See PEP 585")},
    {NULL} /* Sentinel */
};

PyTypeObject
_PyWeakref_RefType = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0)
    .tp_name = "weakref.ReferenceType",
    .tp_basicsize = sizeof(PyWeakReference),
    .tp_dealloc = weakref_dealloc,
    .tp_vectorcall_offset = offsetof(PyWeakReference, vectorcall),
    .tp_call = PyVectorcall_Call,
    .tp_repr = weakref_repr,
    .tp_hash = weakref_hash,
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC |
                Py_TPFLAGS_HAVE_VECTORCALL | Py_TPFLAGS_BASETYPE,
    .tp_traverse = gc_traverse,
    // tp_reachable explicitly doesn't visit the weak reference to reflect the
    // actual RC of referenced objects. Changes to this will require adjustments
    // in freezing and region traversal code.
    .tp_reachable = _PyObject_ReachableVisitTypeAndTraverse,
    .tp_clear = gc_clear,
    .tp_richcompare = weakref_richcompare,
    .tp_methods = weakref_methods,
    .tp_members = weakref_members,
    .tp_init = weakref___init__,
    .tp_alloc = PyType_GenericAlloc,
    .tp_new = weakref___new__,
    .tp_free = PyObject_GC_Del,
};

// ###################################################################
// Region reference type
// ###################################################################

/* A region reference is a weak reference that survives its target's region
 * being closed. Instead of keeping the region open, every dereference asks
 * whether the target may be reached and opens the region tree if it
 * may.
 *
 * This type is deliberately not a subtype of `_PyWeakref_RefType`, which is
 * what keeps `PyWeakref_Check()` false for it.
 */

static PyObject *
regionref_vectorcall(PyObject *self, PyObject *const *args,
                     size_t nargsf, PyObject *kwnames)
{
    if (!_PyArg_NoKwnames("RegionRef", kwnames)) {
        return NULL;
    }
    if (!_PyArg_CheckPositional("RegionRef", PyVectorcall_NARGS(nargsf), 0, 0)) {
        return NULL;
    }
    PyObject *obj = regionref_get_ref(self);
    if (obj == NULL) {
        if (PyErr_Occurred()) {
            return NULL;
        }
        Py_RETURN_NONE;
    }
    return obj;
}

static Py_hash_t
regionref_hash(PyObject *op)
{
    PyWeakReference *self = _PyWeakref_CAST(op);
    // Checked before the cache is consulted: a cached hash would otherwise be
    // a standing answer about an object this interpreter may no longer touch.
    if (regionref_check_access(self, NULL, false) < 0) {
        return -1;
    }
    Py_hash_t hash = _Py_atomic_load_ssize_relaxed(&self->hash);
    if (hash != -1) {
        return hash;
    }
    PyObject *obj = regionref_get_ref(op);
    if (obj == NULL) {
        if (!PyErr_Occurred()) {
            PyErr_SetString(PyExc_TypeError, "weak object has gone away");
        }
        return -1;
    }
    hash = PyObject_Hash(obj);
    Py_DECREF(obj);
    _Py_atomic_store_ssize_relaxed(&self->hash, hash);
    return hash;
}

static PyObject *
regionref_repr(PyObject *self)
{
    PyWeakReference *ref = _PyWeakref_CAST(self);
    PyObject *obj = _Py_atomic_load_ptr(&ref->wr_object);
    if (obj == Py_None) {
        return PyUnicode_FromFormat("<RegionRef; dead>");
    }

    // Deliberately only asks the question instead of going through
    // `regionref_get_ref()`, so that printing a reference never opens a
    // region, matching `TracingRegion`'s repr.
    if (regionref_check_access(ref, NULL, true) < 0) {
        return PyUnicode_FromFormat("<RegionRef; unavailable>");
    }

    // This needs a lock, since the object may be in the middle of finalizing when this
    // is being called.
    LOCK_WEAKREFS(obj);
    PyObject *target = get_ref_lock_held(ref, obj);
    UNLOCK_WEAKREFS(obj);
    if (target == NULL) {
        return PyUnicode_FromFormat("<RegionRef; dead>");
    }
    PyObject *repr = PyUnicode_FromFormat(
        "<RegionRef; to '%T' at %p>", self, target, target);
    Py_DECREF(target);
    return repr;
}

/* Region references only support equality, and compare by target like weak
 * references do. A reference whose target is gone or out of reach falls back to
 * identity, since there is nothing to compare. */
static PyObject *
regionref_richcompare(PyObject *self, PyObject *other, int op)
{
    if ((op != Py_EQ && op != Py_NE)
        || !_PyRegionRef_CheckExact(self)
        || !_PyRegionRef_CheckExact(other))
    {
        Py_RETURN_NOTIMPLEMENTED;
    }

    // An unreachable target compares by identity, like a dead one. The check
    // runs quietly so that a denial never disturbs the caller's error state.
    PyObject *obj = NULL;
    PyObject *other_obj = NULL;
    if (regionref_check_access(_PyWeakref_CAST(self), NULL, true) == 0) {
        obj = regionref_get_ref(self);
    }
    if (regionref_check_access(_PyWeakref_CAST(other), NULL, true) == 0) {
        other_obj = regionref_get_ref(other);
    }
    if (PyErr_Occurred()) {
        Py_XDECREF(obj);
        Py_XDECREF(other_obj);
        return NULL;
    }

    if (obj == NULL || other_obj == NULL) {
        Py_XDECREF(obj);
        Py_XDECREF(other_obj);
        int res = (self == other);
        if (op == Py_NE) {
            res = !res;
        }
        return PyBool_FromLong(res);
    }

    PyObject *res = PyObject_RichCompare(obj, other_obj, op);
    Py_DECREF(obj);
    Py_DECREF(other_obj);
    return res;
}

static PyObject *
regionref___new__(PyTypeObject *type, PyObject *args, PyObject *kwargs)
{
    // FIXME(regions): Region references do not support callbacks yet. Adding
    // them means deciding which interpreter runs the callback and how.
    if (!_PyArg_NoKeywords("RegionRef", kwargs)) {
        return NULL;
    }
    PyObject *ob;
    if (!PyArg_UnpackTuple(args, "__new__", 1, 1, &ob)) {
        return NULL;
    }

    PyWeakReference *ref = get_or_create_weakref(type, ob, NULL);
    if (ref == NULL) {
        return NULL;
    }

    // FIXME(region): Freezing a region reference needs special handling like weak
    // references. We also need to handle a case, where a freeze would propagate into
    // a closed region. The solution is probably a pre-freeze hook that calls freeze
    // on the target object.
    if (_PyImmutability_SetFreezable(
            (PyObject *)ref, _Py_FREEZABLE_NO) < 0) {
        Py_DECREF(ref);
        return NULL;
    }

    // An immutable target is reachable from everywhere, no meta is set.
    if (_Py_IsImmutable(ob)) {
        return (PyObject *)ref;
    }

    // Until a close re-homes it, the target is local to this interpreter.
    LOCK_REGION_REF_META();
    _PyRegionRefMetadata *meta = meta_new_local_lock_held();
    if (meta != NULL) {
        set_region_ref_lock_held(ref, meta);
        meta_decref_lock_held(meta);
    }
    UNLOCK_REGION_REF_META();
    if (meta == NULL) {
        Py_DECREF(ref);
        return PyErr_NoMemory();
    }

    return (PyObject *)ref;
}

static int
regionref___init__(PyObject *self, PyObject *args, PyObject *kwargs)
{
    if (!_PyArg_NoKeywords("RegionRef", kwargs)) {
        return -1;
    }
    PyObject *tmp;
    return PyArg_UnpackTuple(args, "__init__", 1, 1, &tmp) ? 0 : -1;
}

PyDoc_STRVAR(regionref_doc,
"RegionRef(object)\n\
--\n\
\n\
A weak reference into a region that does not keep the region open.\n\
Calling it returns the referenced object if th object may be reached,\n\
or raises a RuntimeError otherwise. Returns None once the referenced \n\
object is gone.");

PyTypeObject
_PyRegionref_RefType = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0)
    .tp_name = "immutable.RegionRef",
    .tp_basicsize = sizeof(PyWeakReference),
    .tp_dealloc = weakref_dealloc,
    .tp_vectorcall_offset = offsetof(PyWeakReference, vectorcall),
    .tp_call = PyVectorcall_Call,
    .tp_repr = regionref_repr,
    .tp_hash = regionref_hash,
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC |
                Py_TPFLAGS_HAVE_VECTORCALL,
    .tp_doc = regionref_doc,
    .tp_traverse = gc_traverse,
    // tp_reachable explicitly doesn't visit the weak reference to reflect the
    // actual RC of referenced objects. Changes to this will require adjustments
    // in freezing and region traversal code.
    .tp_reachable = _PyObject_ReachableVisitTypeAndTraverse,
    .tp_clear = gc_clear,
    .tp_richcompare = regionref_richcompare,
    .tp_methods = weakref_methods,
    .tp_init = regionref___init__,
    .tp_alloc = PyType_GenericAlloc,
    .tp_new = regionref___new__,
    .tp_free = PyObject_GC_Del,
};


static bool
proxy_check_ref(PyObject *obj)
{
    if (obj == NULL) {
        PyErr_SetString(PyExc_ReferenceError,
                        "weakly-referenced object no longer exists");
        return false;
    }
    return true;
}


/* If a parameter is a proxy, check that it is still "live" and wrap it,
 * replacing the original value with the raw object.  Raises ReferenceError
 * if the param is a dead proxy.
 */
#define UNWRAP(o) \
        if (PyWeakref_CheckProxy(o)) { \
            o = _PyWeakref_GET_REF(o); \
            if (!proxy_check_ref(o)) { \
                return NULL; \
            } \
        } \
        else { \
            Py_INCREF(o); \
        }

#define WRAP_UNARY(method, generic) \
    static PyObject * \
    method(PyObject *proxy) { \
        UNWRAP(proxy); \
        PyObject* res = generic(proxy); \
        Py_DECREF(proxy); \
        return res; \
    }

#define WRAP_BINARY(method, generic) \
    static PyObject * \
    method(PyObject *x, PyObject *y) { \
        UNWRAP(x); \
        UNWRAP(y); \
        PyObject* res = generic(x, y); \
        Py_DECREF(x); \
        Py_DECREF(y); \
        return res; \
    }

/* Note that the third arg needs to be checked for NULL since the tp_call
 * slot can receive NULL for this arg.
 */
#define WRAP_TERNARY(method, generic) \
    static PyObject * \
    method(PyObject *proxy, PyObject *v, PyObject *w) { \
        UNWRAP(proxy); \
        UNWRAP(v); \
        if (w != NULL) { \
            UNWRAP(w); \
        } \
        PyObject* res = generic(proxy, v, w); \
        Py_DECREF(proxy); \
        Py_DECREF(v); \
        Py_XDECREF(w); \
        return res; \
    }

#define WRAP_METHOD(method, SPECIAL) \
    static PyObject * \
    method(PyObject *proxy, PyObject *Py_UNUSED(ignored)) { \
            UNWRAP(proxy); \
            PyObject* res = PyObject_CallMethodNoArgs(proxy, &_Py_ID(SPECIAL)); \
            Py_DECREF(proxy); \
            return res; \
        }


/* direct slots */

WRAP_BINARY(proxy_getattr, PyObject_GetAttr)
WRAP_UNARY(proxy_str, PyObject_Str)
WRAP_TERNARY(proxy_call, PyObject_Call)

static PyObject *
proxy_repr(PyObject *proxy)
{
    PyObject *obj = _PyWeakref_GET_REF(proxy);
    PyObject *repr;
    if (obj != NULL) {
        repr = PyUnicode_FromFormat(
            "<weakproxy at %p; to '%T' at %p>",
            proxy, obj, obj);
        Py_DECREF(obj);
    }
    else {
        repr = PyUnicode_FromFormat(
            "<weakproxy at %p; dead>",
            proxy);
    }
    return repr;
}


static int
proxy_setattr(PyObject *proxy, PyObject *name, PyObject *value)
{
    PyObject *obj = _PyWeakref_GET_REF(proxy);
    if (!proxy_check_ref(obj)) {
        return -1;
    }
    int res = PyObject_SetAttr(obj, name, value);
    Py_DECREF(obj);
    return res;
}

static PyObject *
proxy_richcompare(PyObject *proxy, PyObject *v, int op)
{
    UNWRAP(proxy);
    UNWRAP(v);
    PyObject* res = PyObject_RichCompare(proxy, v, op);
    Py_DECREF(proxy);
    Py_DECREF(v);
    return res;
}

/* number slots */
WRAP_BINARY(proxy_add, PyNumber_Add)
WRAP_BINARY(proxy_sub, PyNumber_Subtract)
WRAP_BINARY(proxy_mul, PyNumber_Multiply)
WRAP_BINARY(proxy_floor_div, PyNumber_FloorDivide)
WRAP_BINARY(proxy_true_div, PyNumber_TrueDivide)
WRAP_BINARY(proxy_mod, PyNumber_Remainder)
WRAP_BINARY(proxy_divmod, PyNumber_Divmod)
WRAP_TERNARY(proxy_pow, PyNumber_Power)
WRAP_UNARY(proxy_neg, PyNumber_Negative)
WRAP_UNARY(proxy_pos, PyNumber_Positive)
WRAP_UNARY(proxy_abs, PyNumber_Absolute)
WRAP_UNARY(proxy_invert, PyNumber_Invert)
WRAP_BINARY(proxy_lshift, PyNumber_Lshift)
WRAP_BINARY(proxy_rshift, PyNumber_Rshift)
WRAP_BINARY(proxy_and, PyNumber_And)
WRAP_BINARY(proxy_xor, PyNumber_Xor)
WRAP_BINARY(proxy_or, PyNumber_Or)
WRAP_UNARY(proxy_int, PyNumber_Long)
WRAP_UNARY(proxy_float, PyNumber_Float)
WRAP_BINARY(proxy_iadd, PyNumber_InPlaceAdd)
WRAP_BINARY(proxy_isub, PyNumber_InPlaceSubtract)
WRAP_BINARY(proxy_imul, PyNumber_InPlaceMultiply)
WRAP_BINARY(proxy_ifloor_div, PyNumber_InPlaceFloorDivide)
WRAP_BINARY(proxy_itrue_div, PyNumber_InPlaceTrueDivide)
WRAP_BINARY(proxy_imod, PyNumber_InPlaceRemainder)
WRAP_TERNARY(proxy_ipow, PyNumber_InPlacePower)
WRAP_BINARY(proxy_ilshift, PyNumber_InPlaceLshift)
WRAP_BINARY(proxy_irshift, PyNumber_InPlaceRshift)
WRAP_BINARY(proxy_iand, PyNumber_InPlaceAnd)
WRAP_BINARY(proxy_ixor, PyNumber_InPlaceXor)
WRAP_BINARY(proxy_ior, PyNumber_InPlaceOr)
WRAP_UNARY(proxy_index, PyNumber_Index)
WRAP_BINARY(proxy_matmul, PyNumber_MatrixMultiply)
WRAP_BINARY(proxy_imatmul, PyNumber_InPlaceMatrixMultiply)

static int
proxy_bool(PyObject *proxy)
{
    PyObject *o = _PyWeakref_GET_REF(proxy);
    if (!proxy_check_ref(o)) {
        return -1;
    }
    int res = PyObject_IsTrue(o);
    Py_DECREF(o);
    return res;
}

static void
proxy_dealloc(PyObject *self)
{
    PyObject_GC_UnTrack(self);
    clear_weakref(self);
    PyObject_GC_Del(self);
}

/* sequence slots */

static int
proxy_contains(PyObject *proxy, PyObject *value)
{
    PyObject *obj = _PyWeakref_GET_REF(proxy);
    if (!proxy_check_ref(obj)) {
        return -1;
    }
    int res = PySequence_Contains(obj, value);
    Py_DECREF(obj);
    return res;
}

/* mapping slots */

static Py_ssize_t
proxy_length(PyObject *proxy)
{
    PyObject *obj = _PyWeakref_GET_REF(proxy);
    if (!proxy_check_ref(obj)) {
        return -1;
    }
    Py_ssize_t res = PyObject_Length(obj);
    Py_DECREF(obj);
    return res;
}

WRAP_BINARY(proxy_getitem, PyObject_GetItem)

static int
proxy_setitem(PyObject *proxy, PyObject *key, PyObject *value)
{
    PyObject *obj = _PyWeakref_GET_REF(proxy);
    if (!proxy_check_ref(obj)) {
        return -1;
    }
    int res;
    if (value == NULL) {
        res = PyObject_DelItem(obj, key);
    } else {
        res = PyObject_SetItem(obj, key, value);
    }
    Py_DECREF(obj);
    return res;
}

/* iterator slots */

static PyObject *
proxy_iter(PyObject *proxy)
{
    PyObject *obj = _PyWeakref_GET_REF(proxy);
    if (!proxy_check_ref(obj)) {
        return NULL;
    }
    PyObject* res = PyObject_GetIter(obj);
    Py_DECREF(obj);
    return res;
}

static PyObject *
proxy_iternext(PyObject *proxy)
{
    PyObject *obj = _PyWeakref_GET_REF(proxy);
    if (!proxy_check_ref(obj)) {
        return NULL;
    }
    if (!PyIter_Check(obj)) {
        PyErr_Format(PyExc_TypeError,
            "Weakref proxy referenced a non-iterator '%.200s' object",
            Py_TYPE(obj)->tp_name);
        Py_DECREF(obj);
        return NULL;
    }
    PyObject* res = PyIter_Next(obj);
    Py_DECREF(obj);
    return res;
}


WRAP_METHOD(proxy_bytes, __bytes__)
WRAP_METHOD(proxy_reversed, __reversed__)


static PyMethodDef proxy_methods[] = {
        {"__bytes__", proxy_bytes, METH_NOARGS},
        {"__reversed__", proxy_reversed, METH_NOARGS},
        {NULL, NULL}
};


static PyNumberMethods proxy_as_number = {
    proxy_add,              /*nb_add*/
    proxy_sub,              /*nb_subtract*/
    proxy_mul,              /*nb_multiply*/
    proxy_mod,              /*nb_remainder*/
    proxy_divmod,           /*nb_divmod*/
    proxy_pow,              /*nb_power*/
    proxy_neg,              /*nb_negative*/
    proxy_pos,              /*nb_positive*/
    proxy_abs,              /*nb_absolute*/
    proxy_bool,             /*nb_bool*/
    proxy_invert,           /*nb_invert*/
    proxy_lshift,           /*nb_lshift*/
    proxy_rshift,           /*nb_rshift*/
    proxy_and,              /*nb_and*/
    proxy_xor,              /*nb_xor*/
    proxy_or,               /*nb_or*/
    proxy_int,              /*nb_int*/
    0,                      /*nb_reserved*/
    proxy_float,            /*nb_float*/
    proxy_iadd,             /*nb_inplace_add*/
    proxy_isub,             /*nb_inplace_subtract*/
    proxy_imul,             /*nb_inplace_multiply*/
    proxy_imod,             /*nb_inplace_remainder*/
    proxy_ipow,             /*nb_inplace_power*/
    proxy_ilshift,          /*nb_inplace_lshift*/
    proxy_irshift,          /*nb_inplace_rshift*/
    proxy_iand,             /*nb_inplace_and*/
    proxy_ixor,             /*nb_inplace_xor*/
    proxy_ior,              /*nb_inplace_or*/
    proxy_floor_div,        /*nb_floor_divide*/
    proxy_true_div,         /*nb_true_divide*/
    proxy_ifloor_div,       /*nb_inplace_floor_divide*/
    proxy_itrue_div,        /*nb_inplace_true_divide*/
    proxy_index,            /*nb_index*/
    proxy_matmul,           /*nb_matrix_multiply*/
    proxy_imatmul,          /*nb_inplace_matrix_multiply*/
};

static PySequenceMethods proxy_as_sequence = {
    proxy_length,               /*sq_length*/
    0,                          /*sq_concat*/
    0,                          /*sq_repeat*/
    0,                          /*sq_item*/
    0,                          /*sq_slice*/
    0,                          /*sq_ass_item*/
    0,                          /*sq_ass_slice*/
    proxy_contains,             /* sq_contains */
};

static PyMappingMethods proxy_as_mapping = {
    proxy_length,                 /*mp_length*/
    proxy_getitem,                /*mp_subscript*/
    proxy_setitem,                /*mp_ass_subscript*/
};


PyTypeObject
_PyWeakref_ProxyType = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0)
    "weakref.ProxyType",
    sizeof(PyWeakReference),
    0,
    /* methods */
    proxy_dealloc,                      /* tp_dealloc */
    0,                                  /* tp_vectorcall_offset */
    0,                                  /* tp_getattr */
    0,                                  /* tp_setattr */
    0,                                  /* tp_as_async */
    proxy_repr,                         /* tp_repr */
    &proxy_as_number,                   /* tp_as_number */
    &proxy_as_sequence,                 /* tp_as_sequence */
    &proxy_as_mapping,                  /* tp_as_mapping */
// Notice that tp_hash is intentionally omitted as proxies are "mutable" (when the reference dies).
    0,                                  /* tp_hash */
    0,                                  /* tp_call */
    proxy_str,                          /* tp_str */
    proxy_getattr,                      /* tp_getattro */
    proxy_setattr,                      /* tp_setattro */
    0,                                  /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC, /* tp_flags */
    0,                                  /* tp_doc */
    gc_traverse,                        /* tp_traverse */
    gc_clear,                           /* tp_clear */
    proxy_richcompare,                  /* tp_richcompare */
    0,                                  /* tp_weaklistoffset */
    proxy_iter,                         /* tp_iter */
    proxy_iternext,                     /* tp_iternext */
    proxy_methods,                      /* tp_methods */
    .tp_reachable = _PyObject_ReachableVisitTypeAndTraverse,
};


PyTypeObject
_PyWeakref_CallableProxyType = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0)
    "weakref.CallableProxyType",
    sizeof(PyWeakReference),
    0,
    /* methods */
    proxy_dealloc,                      /* tp_dealloc */
    0,                                  /* tp_vectorcall_offset */
    0,                                  /* tp_getattr */
    0,                                  /* tp_setattr */
    0,                                  /* tp_as_async */
    proxy_repr,                         /* tp_repr */
    &proxy_as_number,                   /* tp_as_number */
    &proxy_as_sequence,                 /* tp_as_sequence */
    &proxy_as_mapping,                  /* tp_as_mapping */
    0,                                  /* tp_hash */
    proxy_call,                         /* tp_call */
    proxy_str,                          /* tp_str */
    proxy_getattr,                      /* tp_getattro */
    proxy_setattr,                      /* tp_setattro */
    0,                                  /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC, /* tp_flags */
    0,                                  /* tp_doc */
    gc_traverse,                        /* tp_traverse */
    gc_clear,                           /* tp_clear */
    proxy_richcompare,                  /* tp_richcompare */
    0,                                  /* tp_weaklistoffset */
    proxy_iter,                         /* tp_iter */
    proxy_iternext,                     /* tp_iternext */
    .tp_reachable = _PyObject_ReachableVisitTypeAndTraverse,
};

PyObject *
PyWeakref_NewRef(PyObject *ob, PyObject *callback)
{
    return (PyObject *)get_or_create_weakref(&_PyWeakref_RefType, ob,
                                             callback);
}

PyObject *
PyWeakref_NewProxy(PyObject *ob, PyObject *callback)
{
    PyTypeObject *type = &_PyWeakref_ProxyType;
    if (PyCallable_Check(ob)) {
        type = &_PyWeakref_CallableProxyType;
    }
    return (PyObject *)get_or_create_weakref(type, ob, callback);
}

int
PyWeakref_IsDead(PyObject *ref)
{
    if (ref == NULL) {
        PyErr_BadInternalCall();
        return -1;
    }
    if (!PyWeakref_Check(ref)) {
        PyErr_Format(PyExc_TypeError, "expected a weakref, got %T", ref);
        return -1;
    }
    return _PyWeakref_IS_DEAD(ref);
}

int
PyWeakref_GetRef(PyObject *ref, PyObject **pobj)
{
    if (ref == NULL) {
        *pobj = NULL;
        PyErr_BadInternalCall();
        return -1;
    }
    if (!PyWeakref_Check(ref)) {
        *pobj = NULL;
        PyErr_SetString(PyExc_TypeError, "expected a weakref");
        return -1;
    }
    *pobj = _PyWeakref_GET_REF(ref);
    return (*pobj != NULL);
}


/* removed in 3.15, but kept for stable ABI compatibility */
PyAPI_FUNC(PyObject *)
PyWeakref_GetObject(PyObject *ref)
{
    if (ref == NULL || !PyWeakref_Check(ref)) {
        PyErr_BadInternalCall();
        return NULL;
    }
    PyObject *obj = _PyWeakref_GET_REF(ref);
    if (obj == NULL) {
        return Py_None;
    }
    Py_DECREF(obj);
    return obj;  // borrowed reference
}

/* Note that there's an inlined copy-paste of handle_callback() in gcmodule.c's
 * handle_weakrefs().
 * There is also a copy-paste in immutability.c.
 */
static void
handle_callback(PyWeakReference *ref, PyObject *callback)
{
    PyObject *cbresult = PyObject_CallOneArg(callback, (PyObject *)ref);

    if (cbresult == NULL) {
        PyErr_FormatUnraisable("Exception ignored while "
                               "calling weakref callback %R", callback);
    }
    else {
        Py_DECREF(cbresult);
    }
}

/* This function is called by the tp_dealloc handler to clear weak references.
 *
 * This iterates through the weak references for 'object' and calls callbacks
 * for those references which have one.  It returns when all callbacks have
 * been attempted.
 */
void
PyObject_ClearWeakRefs(PyObject *object)
{
    PyWeakReference **list;

    if (object == NULL
        || !_PyType_SUPPORTS_WEAKREFS(Py_TYPE(object))
        || _Py_IsImmutable(object)
        || Py_REFCNT(object) != 0)
    {
        PyErr_BadInternalCall();
        return;
    }

    list = GET_WEAKREFS_LISTPTR(object);
    if (FT_ATOMIC_LOAD_PTR(*list) == NULL) {
        // Fast path for the common case
        return;
    }

    /* Remove the callback-less basic and proxy references, which always appear
       at the head of the list.
    */
    for (int done = 0; !done;) {
        LOCK_WEAKREFS(object);
        if (*list != NULL && is_basic_ref_or_proxy(*list)) {
            PyObject *callback;
            clear_weakref_lock_held(*list, &callback);
            assert(callback == NULL);
        }
        done = (*list == NULL) || !is_basic_ref_or_proxy(*list);
        UNLOCK_WEAKREFS(object);
    }

    /* Deal with non-canonical (subtypes or refs with callbacks) references. */
    Py_ssize_t num_weakrefs = _PyWeakref_GetWeakrefCount(object);
    if (num_weakrefs == 0) {
        return;
    }

    PyObject *exc = PyErr_GetRaisedException();
    PyObject *tuple = PyTuple_New(num_weakrefs * 2);
    if (tuple == NULL) {
        _PyWeakref_ClearWeakRefsNoCallbacks(object);
        PyErr_FormatUnraisable("Exception ignored while "
                               "clearing object weakrefs");
        PyErr_SetRaisedException(exc);
        return;
    }

    Py_ssize_t num_items = 0;
    for (int done = 0; !done;) {
        PyObject *callback = NULL;
        LOCK_WEAKREFS(object);
        PyWeakReference *cur = *list;
        if (cur != NULL) {
            clear_weakref_lock_held(cur, &callback);
            if (_Py_TryIncref((PyObject *) cur)) {
                assert(num_items / 2 < num_weakrefs);
                PyTuple_SET_ITEM(tuple, num_items, (PyObject *) cur);
                PyTuple_SET_ITEM(tuple, num_items + 1, callback);
                num_items += 2;
                callback = NULL;
            }
        }
        done = (*list == NULL);
        UNLOCK_WEAKREFS(object);

        Py_XDECREF(callback);
    }

    for (Py_ssize_t i = 0; i < num_items; i += 2) {
        PyObject *callback = PyTuple_GET_ITEM(tuple, i + 1);
        if (callback != NULL) {
            PyObject *weakref = PyTuple_GET_ITEM(tuple, i);
            handle_callback((PyWeakReference *)weakref, callback);
        }
    }

    Py_DECREF(tuple);

    assert(!PyErr_Occurred());
    PyErr_SetRaisedException(exc);
}

/* Clear weak references with callbacks of an immutable object.
 * Store them in a list to be able to call their callbacks later.
 */
void
_PyImmutability_ClearWeakRefsWithCallback(PyObject *object, PyWeakReference **callbacks)
{
    if (object == NULL
        || !_PyType_SUPPORTS_WEAKREFS(Py_TYPE(object))
        || !_Py_IsImmutable(object))
    {
        PyErr_BadInternalCall();
        return;
    }

    PyWeakReference **list = GET_WEAKREFS_LISTPTR(object);
    if (_Py_atomic_load_ptr(list) == NULL) {
        // Fast path for the common case
        return;
    }

    LOCK_WEAKREFS(object);
    PyWeakReference *next = *list;
    while (next != NULL) {
        PyWeakReference *current = next;
        next = next->wr_next;
        if (current->wr_callback != NULL) {
            clear_weakref_lock_held(current, NULL); // keeps the callback
            insert_head(current, callbacks);
        }
    }
    UNLOCK_WEAKREFS(object);
}

void
PyUnstable_Object_ClearWeakRefsNoCallbacks(PyObject *obj)
{
    if (_PyType_SUPPORTS_WEAKREFS(Py_TYPE(obj))) {
        _PyWeakref_ClearWeakRefsNoCallbacks(obj);
    }
}

/* This function is called by _PyStaticType_Dealloc() to clear weak references.
 *
 * This is called at the end of runtime finalization, so we can just
 * wipe out the type's weaklist.  We don't bother with callbacks
 * or anything else.
 */
void
_PyStaticType_ClearWeakRefs(PyInterpreterState *interp, PyTypeObject *type)
{
    managed_static_type_state *state = _PyStaticType_GetState(interp, type);
    PyObject **list = _PyStaticType_GET_WEAKREFS_LISTPTR(state);
    // This is safe to do without holding the lock in free-threaded builds;
    // there is only one thread running and no new threads can be created.
    while (*list) {
        _PyWeakref_ClearRef((PyWeakReference *)*list);
    }
}

void
_PyWeakref_ClearWeakRefsNoCallbacks(PyObject *obj)
{
    _PyWeakref_ClearWeakRefsExcept(obj, NULL);
}

void
_PyWeakref_ClearWeakRefsExcept(PyObject *obj, _Py_hashtable_t *keep)
{
    /* Modeled after GET_WEAKREFS_LISTPTR().

       This is never triggered for static types so we can avoid the
       (slightly) more costly _PyObject_GET_WEAKREFS_LISTPTR(). */
    PyWeakReference **list = _PyObject_GET_WEAKREFS_LISTPTR_FROM_OFFSET(obj);
    LOCK_WEAKREFS(obj);
    while (*list) {
        if (keep != NULL && _Py_hashtable_get_entry(keep, *list)) {
            list = &((*list)->wr_next);
        } else {
            clear_weakref_lock_held(*list, NULL);
        }
    }
    UNLOCK_WEAKREFS(obj);
}

int
_PyWeakref_IsDead(PyObject *weakref)
{
    return _PyWeakref_IS_DEAD(weakref);
}
