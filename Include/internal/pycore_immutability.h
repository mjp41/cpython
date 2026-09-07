#ifndef Py_INTERNAL_IMMUTABILITY_H
#define Py_INTERNAL_IMMUTABILITY_H
#ifdef __cplusplus
extern "C" {
#endif

#ifndef Py_BUILD_CORE
#  error "Py_BUILD_CORE must be defined to include this header"
#endif

struct _Py_immutability_state {
    int late_init_done;
    struct _Py_hashtable_t *shallow_immutable_types;
    struct _Py_hashtable_t *warned_types;
    // With the pre-freeze hook it can happen that freeze calls are
    // nested. This is stack of the enclosing freeze states.
    struct FreezeState *freeze_stack;
#ifdef Py_DEBUG
    PyObject *traceback_func;  // For debugging purposes, can be NULL
#endif
};

#ifdef __cplusplus
}
#endif
#endif /* !Py_INTERNAL_IMMUTABILITY_H */