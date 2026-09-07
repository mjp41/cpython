/*
 * _test_reachable - Test module for tp_reachable freeze warnings.
 *
 * Provides types that deliberately lack tp_reachable so we can test
 * the freeze-time warnings in traverse_freeze().
 *
 *   HasTraverseNoReachable  - has tp_traverse, no tp_reachable
 *   HasTraverseNoReachableHeap - heap type, has tp_traverse, no tp_reachable
 *   IncorrectTraverseNoReachableHeap - heap type, forgets to visit its type
 *   NoTraverseNoReachable   - no  tp_traverse, no tp_reachable
 *   HasReachable            - has both tp_traverse and tp_reachable (no warning)
 */

#ifndef Py_BUILD_CORE_BUILTIN
#  define Py_BUILD_CORE_MODULE 1
#endif

#include "Python.h"
#include "pycore_object.h"      // _PyObject_ReachableVisitTypeAndTraverse()

/* ---- HasTraverseNoReachable ------------------------------------------- */

typedef struct {
    PyObject_HEAD
    PyObject *value;
} HasTraverseNoReachableObject;

static int
htnr_traverse(PyObject *self, visitproc visit, void *arg)
{
    HasTraverseNoReachableObject *obj = (HasTraverseNoReachableObject *)self;
    Py_VISIT(obj->value);
    return 0;
}

static int
htnr_clear(PyObject *self)
{
    HasTraverseNoReachableObject *obj = (HasTraverseNoReachableObject *)self;
    Py_CLEAR(obj->value);
    return 0;
}

static void
htnr_dealloc(PyObject *self)
{
    PyObject_GC_UnTrack(self);
    htnr_clear(self);
    Py_TYPE(self)->tp_free(self);
}

static int
htnr_init(PyObject *self, PyObject *args, PyObject *kwds)
{
    HasTraverseNoReachableObject *obj = (HasTraverseNoReachableObject *)self;
    PyObject *value = Py_None;
    if (!PyArg_ParseTuple(args, "|O", &value))
        return -1;
    Py_XSETREF(obj->value, Py_NewRef(value));
    return 0;
}

static PyTypeObject HasTraverseNoReachable_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "_test_reachable.HasTraverseNoReachable",
    .tp_basicsize = sizeof(HasTraverseNoReachableObject),
    .tp_dealloc = htnr_dealloc,
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,
    .tp_doc = "Type with tp_traverse but no tp_reachable.",
    .tp_traverse = htnr_traverse,
    .tp_clear = htnr_clear,
    .tp_init = htnr_init,
    .tp_alloc = PyType_GenericAlloc,
    .tp_new = PyType_GenericNew,
    .tp_free = PyObject_GC_Del,
    /* tp_reachable deliberately left NULL */
};

/* ---- HasTraverseNoReachableHeap --------------------------------------- */

typedef struct {
    PyObject_HEAD
    PyObject *value;
} HasTraverseNoReachableHeapObject;

static int
htnrh_traverse(PyObject *self, visitproc visit, void *arg)
{
    HasTraverseNoReachableHeapObject *obj = (HasTraverseNoReachableHeapObject *)self;
    Py_VISIT(Py_TYPE(self));
    Py_VISIT(obj->value);
    return 0;
}

static int
htnrh_clear(PyObject *self)
{
    HasTraverseNoReachableHeapObject *obj = (HasTraverseNoReachableHeapObject *)self;
    Py_CLEAR(obj->value);
    return 0;
}

static void
htnrh_dealloc(PyObject *self)
{
    PyObject_GC_UnTrack(self);
    htnrh_clear(self);
    Py_TYPE(self)->tp_free(self);
}

static int
htnrh_init(PyObject *self, PyObject *args, PyObject *kwds)
{
    HasTraverseNoReachableHeapObject *obj = (HasTraverseNoReachableHeapObject *)self;
    PyObject *value = Py_None;
    if (!PyArg_ParseTuple(args, "|O", &value))
        return -1;
    Py_XSETREF(obj->value, Py_NewRef(value));
    return 0;
}

static PyType_Slot HasTraverseNoReachableHeap_slots[] = {
    {Py_tp_doc, "Heap type with tp_traverse but no tp_reachable."},
    {Py_tp_dealloc, htnrh_dealloc},
    {Py_tp_traverse, htnrh_traverse},
    {Py_tp_clear, htnrh_clear},
    {Py_tp_init, htnrh_init},
    {Py_tp_alloc, PyType_GenericAlloc},
    {Py_tp_new, PyType_GenericNew},
    {Py_tp_free, PyObject_GC_Del},
    {0, NULL},
};

static PyType_Spec HasTraverseNoReachableHeap_spec = {
    .name = "_test_reachable.HasTraverseNoReachableHeap",
    .basicsize = sizeof(HasTraverseNoReachableHeapObject),
    .itemsize = 0,
    .flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,
    .slots = HasTraverseNoReachableHeap_slots,
};

/* ---- IncorrectTraverseNoReachableHeap -------------------------------- */

typedef struct {
    PyObject_HEAD
    PyObject *value;
} IncorrectTraverseNoReachableHeapObject;

static int
itnrh_traverse(PyObject *self, visitproc visit, void *arg)
{
    IncorrectTraverseNoReachableHeapObject *obj = (IncorrectTraverseNoReachableHeapObject *)self;
    // This is intentionally not visiting the heap type for testing
    // Py_VISIT(Py_TYPE(self));
    Py_VISIT(obj->value);
    return 0;
}

static int
itnrh_clear(PyObject *self)
{
    IncorrectTraverseNoReachableHeapObject *obj = (IncorrectTraverseNoReachableHeapObject *)self;
    Py_CLEAR(obj->value);
    return 0;
}

static void
itnrh_dealloc(PyObject *self)
{
    PyObject_GC_UnTrack(self);
    itnrh_clear(self);
    Py_TYPE(self)->tp_free(self);
}

static int
itnrh_init(PyObject *self, PyObject *args, PyObject *kwds)
{
    IncorrectTraverseNoReachableHeapObject *obj = (IncorrectTraverseNoReachableHeapObject *)self;
    PyObject *value = Py_None;
    if (!PyArg_ParseTuple(args, "|O", &value))
        return -1;
    Py_XSETREF(obj->value, Py_NewRef(value));
    return 0;
}

static PyType_Slot IncorrectTraverseNoReachableHeap_slots[] = {
    {Py_tp_doc, "Heap type with tp_traverse but no tp_reachable and an incorrect traverse."},
    {Py_tp_dealloc, itnrh_dealloc},
    {Py_tp_traverse, itnrh_traverse},
    {Py_tp_clear, itnrh_clear},
    {Py_tp_init, itnrh_init},
    {Py_tp_alloc, PyType_GenericAlloc},
    {Py_tp_new, PyType_GenericNew},
    {Py_tp_free, PyObject_GC_Del},
    {0, NULL},
};

static PyType_Spec IncorrectTraverseNoReachableHeap_spec = {
    .name = "_test_reachable.IncorrectTraverseNoReachableHeap",
    .basicsize = sizeof(IncorrectTraverseNoReachableHeapObject),
    .itemsize = 0,
    .flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,
    .slots = IncorrectTraverseNoReachableHeap_slots,
};

/* ---- NoTraverseNoReachable ------------------------------------------- */

typedef struct {
    PyObject_HEAD
    int dummy;
} NoTraverseNoReachableObject;

static int
ntnr_init(PyObject *self, PyObject *args, PyObject *kwds)
{
    return 0;
}

static PyTypeObject NoTraverseNoReachable_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "_test_reachable.NoTraverseNoReachable",
    .tp_basicsize = sizeof(NoTraverseNoReachableObject),
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_doc = "Type with no tp_traverse and no tp_reachable.",
    .tp_init = ntnr_init,
    .tp_alloc = PyType_GenericAlloc,
    .tp_new = PyType_GenericNew,
    /* tp_traverse deliberately left NULL */
    /* tp_reachable deliberately left NULL */
};

/* ---- HasReachable ---------------------------------------------------- */

typedef struct {
    PyObject_HEAD
    PyObject *value;
} HasReachableObject;

static int
hr_traverse(PyObject *self, visitproc visit, void *arg)
{
    HasReachableObject *obj = (HasReachableObject *)self;
    Py_VISIT(obj->value);
    return 0;
}

static int
hr_clear(PyObject *self)
{
    HasReachableObject *obj = (HasReachableObject *)self;
    Py_CLEAR(obj->value);
    return 0;
}

static void
hr_dealloc(PyObject *self)
{
    PyObject_GC_UnTrack(self);
    hr_clear(self);
    Py_TYPE(self)->tp_free(self);
}

static int
hr_init(PyObject *self, PyObject *args, PyObject *kwds)
{
    HasReachableObject *obj = (HasReachableObject *)self;
    PyObject *value = Py_None;
    if (!PyArg_ParseTuple(args, "|O", &value))
        return -1;
    Py_XSETREF(obj->value, Py_NewRef(value));
    return 0;
}

static PyTypeObject HasReachable_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "_test_reachable.HasReachable",
    .tp_basicsize = sizeof(HasReachableObject),
    .tp_dealloc = hr_dealloc,
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,
    .tp_doc = "Type with both tp_traverse and tp_reachable.",
    .tp_traverse = hr_traverse,
    .tp_clear = hr_clear,
    .tp_init = hr_init,
    .tp_alloc = PyType_GenericAlloc,
    .tp_new = PyType_GenericNew,
    .tp_free = PyObject_GC_Del,
    .tp_reachable = _PyObject_ReachableVisitTypeAndTraverse,
};

/* ---- Module ---------------------------------------------------------- */

/* ---- ShallowImmutable ------------------------------------------------ */
/*
 * A C-level type registered as shallow immutable via
 * _PyImmutability_RegisterShallowImmutable.  Instances hold a single
 * PyObject* but are declared shallow-immutable, meaning the implicit
 * check trusts that the instance itself won't be mutated.
 */

typedef struct {
    PyObject_HEAD
    PyObject *value;
} ShallowImmutableObject;

static int
si_traverse(PyObject *self, visitproc visit, void *arg)
{
    ShallowImmutableObject *obj = (ShallowImmutableObject *)self;
    Py_VISIT(obj->value);
    return 0;
}

static int
si_clear(PyObject *self)
{
    ShallowImmutableObject *obj = (ShallowImmutableObject *)self;
    Py_CLEAR(obj->value);
    return 0;
}

static void
si_dealloc(PyObject *self)
{
    PyObject_GC_UnTrack(self);
    si_clear(self);
    Py_TYPE(self)->tp_free(self);
}

static int
si_init(PyObject *self, PyObject *args, PyObject *kwds)
{
    ShallowImmutableObject *obj = (ShallowImmutableObject *)self;
    PyObject *value = Py_None;
    if (!PyArg_ParseTuple(args, "|O", &value))
        return -1;
    Py_XSETREF(obj->value, Py_NewRef(value));
    return 0;
}

static PyTypeObject ShallowImmutable_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "_test_reachable.ShallowImmutable",
    .tp_basicsize = sizeof(ShallowImmutableObject),
    .tp_dealloc = si_dealloc,
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC | Py_TPFLAGS_IMMUTABLETYPE,
    .tp_doc = "C-level type registered as shallow immutable.",
    .tp_traverse = si_traverse,
    .tp_clear = si_clear,
    .tp_init = si_init,
    .tp_alloc = PyType_GenericAlloc,
    .tp_new = PyType_GenericNew,
    .tp_free = PyObject_GC_Del,
    .tp_reachable = _PyObject_ReachableVisitTypeAndTraverse,
};

static int
_test_reachable_exec(PyObject *module)
{
    PyObject *htnrh_type = NULL;
    PyObject *itnrh_type = NULL;

    /* Ready both types and register them as freezable */
    if (PyType_Ready(&HasTraverseNoReachable_Type) < 0)
        return -1;
    /* Reset tp_reachable to NULL in case Ready inherited one */
    HasTraverseNoReachable_Type.tp_reachable = NULL;
    if (PyModule_AddType(module, &HasTraverseNoReachable_Type) != 0)
        return -1;
    if (_PyImmutability_SetFreezable((PyObject*)&HasTraverseNoReachable_Type, _Py_FREEZABLE_YES) < 0)
        return -1;

    htnrh_type = PyType_FromModuleAndSpec(module, &HasTraverseNoReachableHeap_spec, NULL);
    if (htnrh_type == NULL)
        return -1;
    /* Keep this heap type as "no tp_reachable" even if inherited. */
    ((PyTypeObject *)htnrh_type)->tp_reachable = NULL;
    if (PyModule_AddObjectRef(module, "HasTraverseNoReachableHeap", htnrh_type) != 0) {
        Py_DECREF(htnrh_type);
        return -1;
    }
    if (_PyImmutability_SetFreezable((PyObject*)htnrh_type, _Py_FREEZABLE_YES) < 0) {
        Py_DECREF(htnrh_type);
        return -1;
    }
    Py_DECREF(htnrh_type);

    itnrh_type = PyType_FromModuleAndSpec(module, &IncorrectTraverseNoReachableHeap_spec, NULL);
    if (itnrh_type == NULL)
        return -1;
    /* Keep this heap type as "no tp_reachable" even if inherited. */
    ((PyTypeObject *)itnrh_type)->tp_reachable = NULL;
    if (PyModule_AddObjectRef(module, "IncorrectTraverseNoReachableHeap", itnrh_type) != 0) {
        Py_DECREF(itnrh_type);
        return -1;
    }
    if (_PyImmutability_SetFreezable((PyObject*)itnrh_type, _Py_FREEZABLE_YES) < 0) {
        Py_DECREF(itnrh_type);
        return -1;
    }
    Py_DECREF(itnrh_type);

    if (PyType_Ready(&NoTraverseNoReachable_Type) < 0)
        return -1;
    /* Reset tp_reachable to NULL in case Ready inherited one */
    NoTraverseNoReachable_Type.tp_reachable = NULL;
    if (PyModule_AddType(module, &NoTraverseNoReachable_Type) != 0)
        return -1;
    if (_PyImmutability_SetFreezable((PyObject*)&NoTraverseNoReachable_Type, _Py_FREEZABLE_YES) < 0)
        return -1;

    if (PyType_Ready(&HasReachable_Type) < 0)
        return -1;
    if (PyModule_AddType(module, &HasReachable_Type) != 0)
        return -1;
    if (_PyImmutability_SetFreezable((PyObject*)&HasReachable_Type, _Py_FREEZABLE_YES) < 0)
        return -1;

    if (PyType_Ready(&ShallowImmutable_Type) < 0)
        return -1;
    if (PyModule_AddType(module, &ShallowImmutable_Type) != 0)
        return -1;
    if (_PyImmutability_SetFreezable((PyObject*)&ShallowImmutable_Type, _Py_FREEZABLE_YES) < 0)
        return -1;
    if (_PyImmutability_RegisterShallowImmutable(&ShallowImmutable_Type) < 0)
        return -1;

    return 0;
}

static PyModuleDef_Slot _test_reachable_slots[] = {
    {Py_mod_exec, _test_reachable_exec},
    {Py_mod_multiple_interpreters, Py_MOD_PER_INTERPRETER_GIL_SUPPORTED},
    {Py_mod_gil, Py_MOD_GIL_NOT_USED},
    {0, NULL},
};

static struct PyModuleDef _test_reachable_module = {
    PyModuleDef_HEAD_INIT,
    .m_name = "_test_reachable",
    .m_doc = "Test module for tp_reachable freeze warnings.",
    .m_size = 0,
    .m_slots = _test_reachable_slots,
};

PyMODINIT_FUNC
PyInit__test_reachable(void)
{
    return PyModuleDef_Init(&_test_reachable_module);
}
