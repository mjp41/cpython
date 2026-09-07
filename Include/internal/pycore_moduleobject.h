#ifndef Py_INTERNAL_MODULEOBJECT_H
#define Py_INTERNAL_MODULEOBJECT_H
#ifdef __cplusplus
extern "C" {
#endif

#ifndef Py_BUILD_CORE
#  error "this header requires Py_BUILD_CORE define"
#endif

extern void _PyModule_Clear(PyObject *);
extern void _PyModule_ClearDict(PyObject *);
extern int _PyModuleSpec_IsInitializing(PyObject *);
extern int _PyModuleSpec_GetFileOrigin(PyObject *, PyObject **);
extern int _PyModule_IsPossiblyShadowing(PyObject *);

extern int _PyModule_IsExtension(PyObject *obj);

typedef struct {
    PyObject_HEAD
    // For immutable modules to find the mutable state and
    // for logging purposes after md_dict is cleared
    PyObject *md_name;
    int md_frozen;

    // *******************************************************
    // Module state, only available on mutable module objects
    // *******************************************************
    PyObject *md_dict;
    PyModuleDef *md_def;
    void *md_state;
    PyObject *md_weaklist;
#ifdef Py_GIL_DISABLED
    void *md_gil;
#endif
} PyModuleObject;

PyAPI_FUNC(PyModuleObject*) _PyInterpreterState_GetModuleState(PyObject *mod);

static inline PyModuleDef* _PyModule_GetDef(PyObject *mod) {
    assert(PyModule_Check(mod));
    PyModuleObject *state = _PyInterpreterState_GetModuleState(mod);
    if (state == NULL) {
        return NULL;
    }
    return state->md_def;
}

static inline void* _PyModule_GetState(PyObject* mod) {
    assert(PyModule_Check(mod));
    PyModuleObject *state = _PyInterpreterState_GetModuleState(mod);
    if (state == NULL) {
        return NULL;
    }
    return state->md_state;
}

static inline PyObject* _PyModule_GetDict(PyObject *mod) {
    assert(PyModule_Check(mod));
    PyModuleObject *state = _PyInterpreterState_GetModuleState(mod);
    if (state == NULL) {
        return NULL;
    }
    PyObject *dict = state -> md_dict;
    // _PyModule_GetDict(mod) must not be used after calling module_clear(mod)
    assert(dict != NULL);
    return dict;  // borrowed reference
}

extern PyObject * _PyModule_GetFilenameObject(PyObject *);
extern Py_ssize_t _PyModule_GetFilenameUTF8(
        PyObject *module,
        char *buffer,
        Py_ssize_t maxlen);

PyObject* _Py_module_getattro_impl(PyModuleObject *m, PyObject *name, int suppress);
PyObject* _Py_module_getattro(PyObject *m, PyObject *name);

extern int _Py_module_freeze_hook(PyObject *m);

#ifdef __cplusplus
}
#endif
#endif /* !Py_INTERNAL_MODULEOBJECT_H */
