
#include "Python.h"
#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include "pycore_dict.h"
#include "pycore_object.h"
#include "pycore_regions.h"


typedef struct node_s {
    PyObject* object;
    struct node_s* next;
} node;

typedef struct stack_s {
    node* head;
} stack;

stack* stack_new(void){
    stack* s = (stack*)malloc(sizeof(stack));
    if(s == NULL){
        return NULL;
    }

    s->head = NULL;

    return s;
}

bool stack_push(stack* s, PyObject* object){
    node* n = (node*)malloc(sizeof(node));
    if(n == NULL){
        return true;
    }

    n->object = object;
    n->next = s->head;
    s->head = n;
    return false;
}

PyObject* stack_pop(stack* s){
    if(s->head == NULL){
        return NULL;
    }

    node* n = s->head;
    PyObject* object = n->object;
    s->head = n->next;
    free(n);

    return object;
}

void stack_free(stack* s){
    while(s->head != NULL){
        PyObject* op = stack_pop(s);
        Py_DECREF(op);
    }

    free(s);
}

bool stack_empty(stack* s){
    return s->head == NULL;
}

void stack_print(stack* s){
    _Py_VPYDBG("stack: ");
    node* n = s->head;
    while(n != NULL){
        _Py_VPYDBGPRINT(n->object);
        _Py_VPYDBG("[rc=%ld]\n", n->object->ob_refcnt);
        n = n->next;
    }
}

bool is_leaf(PyObject* obj){
    return Py_IsNone(obj) || Py_IsTrue(obj) || Py_IsFalse(obj) || PyUnicode_Check(obj) || PyLong_Check(obj) || PyFloat_Check(obj) || PyBytes_Check(obj) || PyFrozenSet_Check(obj);
}

bool is_c_wrapper(PyObject* obj){
    return PyCFunction_Check(obj) || Py_IS_TYPE(obj, &_PyMethodWrapper_Type) || Py_IS_TYPE(obj, &PyWrapperDescr_Type);
}

bool is_leaf_type(PyTypeObject* obj){
    return PyType_HasFeature(obj, Py_TPFLAGS_IMMUTABLETYPE) || !PyType_HasFeature(obj, Py_TPFLAGS_HEAPTYPE);
}

PyObject* walk_sequence(PyObject* seq, stack* frontier) {
    if(!PySequence_Check(seq)){
        Py_RETURN_NONE;
    }

    Py_ssize_t size;
    _PyObject_ASSERT(seq, PySequence_Check(seq));
    size = PySequence_Size(seq);
    _Py_VPYDBG("list/tuple: pushing %ld [i: element] elements\n", size);
    for(Py_ssize_t i = 0; i < size; i++){
        PyObject* element = PySequence_GetItem(seq, i); // element.rc = x + 1
        _Py_VPYDBG("[%ld: ", i);
        _Py_VPYDBGPRINT(element);
        _Py_VPYDBG("]\n");
        if(!_Py_IsImmutable(element)){
            if(stack_push(frontier, element)){
                Py_DECREF(element); // element.rc = x
                return PyErr_NoMemory();
            }
        }else{
            Py_DECREF(element); // element.rc = x
        }
    }
    _Py_VPYDBG("\n");
    Py_RETURN_NONE;
}

PyObject* walk_mapping(PyObject* map, stack* frontier) {
    if(!(PyMapping_Check(map) && (PyDict_CheckExact(map) || PyObject_HasAttrString(map, "keys")))){
        Py_RETURN_NONE;
    }

    PyObject* keys = PyMapping_Keys(map);
    Py_ssize_t size = PyList_Size(keys);
    _Py_VPYDBG("mapping: pushing %ld <key: value> pairs\n", size);
    for(Py_ssize_t i = 0; i < size; i++){
        PyObject* key = PyList_GET_ITEM(keys, i); // key.rc = x
        _Py_VPYDBG("<");
        _Py_VPYDBGPRINT(key);
        if(!_Py_IsImmutable(key)){
            Py_INCREF(key); // key.rc = x + 1
            if(stack_push(frontier, key)){
                Py_DECREF(key); // key.rc = x
                return PyErr_NoMemory();
            }
        }

        PyObject* value = PyObject_GetItem(map, key); // value.rc = x + 1
        _Py_VPYDBG(": ");
        _Py_VPYDBGPRINT(value);
        if(!_Py_IsImmutable(value)){
            if(stack_push(frontier, value)){
                Py_DECREF(value); // value.rc = x
                return PyErr_NoMemory();
            }
        }

        _Py_VPYDBG(">\n");
    }

    _Py_VPYDBG("\n");
    Py_DECREF(keys);

    Py_RETURN_NONE;
}

PyObject* walk_object(PyObject* obj, stack* frontier)
{
    PyObject* attrs;
    Py_ssize_t size;

    if(!PyObject_HasAttrString(obj, "__dir__")){
        // object does not provide __dir__
        Py_RETURN_NONE;
    }

    _Py_VPYDBG("object: pushing (attr: value)\n");

    attrs = PyObject_Dir(obj);
    size = PyList_Size(attrs);
    for(Py_ssize_t i = 0; i < size; i++){
        PyObject* attr = PyList_GET_ITEM(attrs, i); // attr.rc = x
        PyObject* value = PyObject_GetAttr(obj, attr); // value.rc = x + 1
        if(value == NULL){
            // attribute error
            return NULL;
        }

        if(_Py_IsImmutable(value) || is_c_wrapper(value)){
            // C functions are not mutable by construction, so we can skip them.
            // Wrappers are ephemeral (created on request)
            Py_DECREF(value); // value.rc = x
            continue;
        }

        _Py_VPYDBG("(");
        _Py_VPYDBGPRINT(attr);
        _Py_VPYDBG(": ");
        _Py_VPYDBGPRINT(value);
        _Py_VPYDBG(")\n");

        if(stack_push(frontier, value)){
            Py_DECREF(value); // value.rc = x
            return PyErr_NoMemory();
        }
    }

    Py_DECREF(attrs);
    _Py_VPYDBG("\n");
    Py_RETURN_NONE;
}

#define _Py_VISIT_FUNC_ATTR(attr, frontier) do { \
    if(attr != NULL && !_Py_IsImmutable(attr)){ \
        Py_INCREF(attr); \
        if(stack_push(frontier, attr)){ \
            Py_DECREF(attr); \
            return PyErr_NoMemory(); \
        } \
    } \
} while(0)

PyObject* make_global_immutable(PyObject* globals, PyObject* name)
{
    PyObject* value = PyDict_GetItem(globals, name); // value.rc = x
    _Py_VPYDBG("value(");
    _Py_VPYDBGPRINT(value);
    _Py_VPYDBG(") -> ");

    _PyDict_SetKeyImmutable((PyDictObject*)globals, name);

    if(!_Py_IsImmutable(value)){
        _Py_VPYDBG("pushed\n");
        _Py_SetImmutable(value);
        Py_INCREF(value);
        return value;
    }else{
        _Py_VPYDBG("immutable\n");
        Py_RETURN_NONE;
    }
}

PyObject* walk_function(PyObject* op, stack* frontier)
{
    PyObject* builtins;
    PyObject* globals;
    PyObject* module;
    PyObject* module_dict;
    PyFunctionObject* f;
    PyObject* f_ptr;
    PyCodeObject* f_code;
    Py_ssize_t size;
    stack* f_stack;
    bool check_globals = false;
    _PyObject_ASSERT(op, PyFunction_Check(op));

    _Py_VPYDBG("function: ");
    _Py_VPYDBGPRINT(op);
    _Py_VPYDBG("[rc=%ld]\n", Py_REFCNT(op));

    _Py_SetImmutable(op);


    f = (PyFunctionObject*)op;

    // TODO find a way to use traverse to avoid having to manually walk
    // the function's members
    // f->func_code needs special treatment (see below)
    // func_globals, func_builtins, and func_module can stay mutable, but depending on code we may need to make some keys immutable
    globals = f->func_globals;
    builtins = f->func_builtins;
    module = PyImport_Import(f->func_module);
    if(PyModule_Check(module)){
        module_dict = PyModule_GetDict(module);
    }else{
        module_dict = NULL;
    }

    _Py_VISIT_FUNC_ATTR(f->func_defaults, frontier);
    _Py_VISIT_FUNC_ATTR(f->func_kwdefaults, frontier);
    _Py_VISIT_FUNC_ATTR(f->func_doc, frontier);
    _Py_VISIT_FUNC_ATTR(f->func_name, frontier);
    _Py_VISIT_FUNC_ATTR(f->func_dict, frontier);
    _Py_VISIT_FUNC_ATTR(f->func_closure, frontier);
    _Py_VISIT_FUNC_ATTR(f->func_annotations, frontier);
    _Py_VISIT_FUNC_ATTR(f->func_typeparams, frontier);
    _Py_VISIT_FUNC_ATTR(f->func_qualname, frontier);

    f_stack = stack_new();
    if(f_stack == NULL){
        return PyErr_NoMemory();
    }

    f_ptr = f->func_code;
    if(stack_push(f_stack, f_ptr)){
        stack_free(f_stack);
        return PyErr_NoMemory();
    }

    Py_INCREF(f_ptr); // fp.rc = x + 1
    _Py_VPYDBG("function: adding captured vars/funcs/builtins\n");
    while(!stack_empty(f_stack)){
        f_ptr = stack_pop(f_stack); // fp.rc = x + 1
        _PyObject_ASSERT(f_ptr, PyCode_Check(f_ptr));
        f_code = (PyCodeObject*)f_ptr;

        _Py_VPYDBG("analysing code: ");
        _Py_VPYDBGPRINT(f_code->co_name);
        _Py_VPYDBG("\n");

        size = 0;
        if (f_code->co_names != NULL)
          size = PySequence_Fast_GET_SIZE(f_code->co_names);
        _Py_VPYDBG("Enumerating %ld names\n", size);
        for(Py_ssize_t i = 0; i < size; i++){
            PyObject* name = PySequence_Fast_GET_ITEM(f_code->co_names, i); // name.rc = x
            _Py_VPYDBG("name ");
            _Py_VPYDBGPRINT(name);
            _Py_VPYDBG(": ");

            if(PyUnicode_CompareWithASCIIString(name, "globals") == 0){
                // if the code calls the globals() builtin, then any
                // cellvar or const in the function could, potentially, refer to
                // a global variable. As such, we need to check if the globals
                // dictionary contains that key and then make it immutable
                // from this point forwards.
                check_globals = true;
            }

            if(PyDict_Contains(globals, name)){
                PyObject* value = make_global_immutable(globals, name);
                if(!Py_IsNone(value)){
                    if(stack_push(frontier, value)){
                        Py_DECREF(value); // value.rc = x
                        stack_free(f_stack);
                        // frontier freed by the caller
                        return PyErr_NoMemory();
                    }
                }
            }else if(PyDict_Contains(builtins, name)){
                _Py_VPYDBG("builtin\n");

                _PyDict_SetKeyImmutable((PyDictObject*)builtins, name);

                PyObject* value = PyDict_GetItem(builtins, name); // value.rc = x
                if(!_Py_IsImmutable(value)){
                    _Py_SetImmutable(value);
                }
            }else if(PyDict_Contains(module_dict, name)){
                PyObject* value = PyDict_GetItem(module_dict, name); // value.rc = x
                _Py_VPYDBG("module(");
                _Py_VPYDBGPRINT(value);
                _Py_VPYDBG(") -> ");

                _PyDict_SetKeyImmutable((PyDictObject*)module_dict, name);

                if(!_Py_IsImmutable(value)){
                    Py_INCREF(value); // value.rc = x + 1
                    if(stack_push(frontier, value)){
                        Py_DECREF(value); // value.rc = x
                        stack_free(f_stack);
                        // frontier freed by the caller
                        return PyErr_NoMemory();
                    }
                }else{
                    _Py_VPYDBG("immutable\n");
                }
            }else{
                _Py_VPYDBG("instance\n");
                // TODO assert that it is an instance variable
            }
        }

        size = PySequence_Fast_GET_SIZE(f_code->co_consts);
        _Py_VPYDBG("Enumerating %ld consts\n", size);
        for(Py_ssize_t i = 0; i < size; i++){
            PyObject* value = PySequence_Fast_GET_ITEM(f_code->co_consts, i); // value.rc = x
            _Py_VPYDBG("const ");
            _Py_VPYDBGPRINT(value);
            _Py_VPYDBG(": ");
            if(!_Py_IsImmutable(value)){
                Py_INCREF(value); // value.rc = x + 1
                if(PyCode_Check(value)){
                    _Py_VPYDBG("nested_func\n");

                    _Py_SetImmutable(value);

                    if(stack_push(f_stack, value)){
                        Py_DECREF(value); // value.rc = x
                        stack_free(f_stack);
                        // frontier freed by the caller
                        return PyErr_NoMemory();
                    }
                }else{
                    _Py_VPYDBG("pushed\n");

                    if(stack_push(frontier, value)){
                        Py_DECREF(value); // value.rc = x
                        stack_free(f_stack);
                        // frontier freed by the caller
                        return PyErr_NoMemory();
                    }
                }
            }else{
                _Py_VPYDBG("immutable\n");
            }

            if(check_globals && PyUnicode_Check(value)){
                _Py_VPYDBG("checking if");
                _Py_VPYDBGPRINT(value);
                _Py_VPYDBG(" is a global: ");
                PyObject* name = value;
                if(PyDict_Contains(globals, name)){
                    _Py_VPYDBG(" true ");
                    value = make_global_immutable(globals, name);
                    if(!Py_IsNone(value)){
                        if(stack_push(frontier, value)){
                            Py_DECREF(value); // value.rc = x
                            stack_free(f_stack);
                            // frontier freed by the caller
                            return PyErr_NoMemory();
                        }
                    }
                }else{
                    _Py_VPYDBG("false\n");

                }
            }
        }

        Py_DECREF(f_ptr); // fp.rc = x
    }

    stack_free(f_stack);

    if(check_globals){
        size = 0;
        if(f->func_closure != NULL)
            size = PySequence_Fast_GET_SIZE(f->func_closure);
        _Py_VPYDBG("Enumerating %ld closure vars to check for global names\n", size);
        for(Py_ssize_t i=0; i < size; ++i){
            PyObject* cellvar = PySequence_Fast_GET_ITEM(f->func_closure, i); // cellvar.rc = x
            PyObject* value = PyCell_GET(cellvar); // value.rc = x
            _Py_VPYDBG("cellvar(");
            _Py_VPYDBGPRINT(value);
            _Py_VPYDBG(") is ");

            if(PyUnicode_Check(value)){
                PyObject* name = value;
                if(PyDict_Contains(globals, name)){
                    _Py_VPYDBG("a global ");
                    value = make_global_immutable(globals, name);
                    if(!Py_IsNone(value)){
                        if(stack_push(frontier, value)){
                            Py_DECREF(value); // value.rc = x
                            stack_free(f_stack);
                            // frontier freed by the caller
                            return PyErr_NoMemory();
                        }
                    }
                }else{
                    _Py_VPYDBG("not a global\n");
                }
            }else{
                _Py_VPYDBG("not a global\n");
            }
        }
    }

    Py_RETURN_NONE;
}

#define _Py_MAKEIMMUTABLE_CALL(f, item, frontier) do { \
    PyObject* err = f((item), (frontier));             \
    if(!Py_IsNone(err)){                               \
        Py_DECREF(item);                               \
        stack_free((frontier));                        \
        return err;                                    \
    }                                                  \
} while(0)

int _makeimmutable_visit(PyObject* obj, void* frontier)
{
    _Py_VPYDBG("visit(");
    _Py_VPYDBGPRINT(obj);
    _Py_VPYDBG(") region: %lu rc: %ld\n", Py_REGION(obj), Py_REFCNT(obj));
    if(!_Py_IsImmutable(obj)){
        if(stack_push((stack*)frontier, obj)){
            Py_DECREF(obj);
            PyErr_NoMemory();
            return -1;
        }
    }

    return 0;
}

PyObject* _Py_MakeImmutable(PyObject* obj)
{
    _Py_VPYDBG(">> makeimmutable(");
    _Py_VPYDBGPRINT(obj);
    _Py_VPYDBG(") region: %lu rc: %ld\n", Py_REGION(obj), Py_REFCNT(obj));
    if(_Py_IsImmutable(obj) && _Py_IsImmutable(Py_TYPE(obj))){
        return obj;
    }

    stack* frontier = stack_new();
    if(frontier == NULL){
        return PyErr_NoMemory();
    }

    if(stack_push(frontier, obj)){
        stack_free(frontier);
        return PyErr_NoMemory();
    }

    Py_INCREF(obj); // obj.rc = x + 1
    while(!stack_empty(frontier)){
        PyObject* item = stack_pop(frontier); // item.rc = x + 1
        PyTypeObject* type = Py_TYPE(item);
        traverseproc traverse;
        PyObject* type_op;

        _Py_VPYDBG("item: ");
        _Py_VPYDBGPRINT(item);
        _Py_VPYDBG("\n");

        if(_Py_IsImmutable(item)){
            goto handle_type;
        }

        _Py_SetImmutable(item);

        if(is_c_wrapper(item)) {
            // C functions are not mutable, so we can skip them.
            goto next;
        }

        if(PyFunction_Check(item)){
            _Py_MAKEIMMUTABLE_CALL(walk_function, item, frontier);
            goto next;
        }


        traverse = type->tp_traverse;
        if(traverse != NULL){
            _Py_VPYDBG("implements tp_traverse\n");
            if(traverse(item, (visitproc)_makeimmutable_visit, frontier)){
                Py_DECREF(item);
                stack_free(frontier);
                return NULL;
            }
        }else{
            _Py_MAKEIMMUTABLE_CALL(walk_sequence, item, frontier);
            _Py_MAKEIMMUTABLE_CALL(walk_mapping, item, frontier);
        }

handle_type:
        type_op = PyObject_Type(item); // type.rc = x + 1
        if(!_Py_IsImmutable(type_op)){
            if(is_leaf_type((PyTypeObject*)type_op)){
                _Py_SetImmutable(type_op);
                Py_DECREF(type_op);
            }else{
                if(stack_push(frontier, type_op)){
                    Py_DECREF(type_op);
                    Py_DECREF(item);
                    stack_free(frontier);
                    return PyErr_NoMemory();
                }
            }
        }

next:
        Py_DECREF(item); // item.rc = x
    }

    stack_free(frontier);

    _Py_VPYDBGPRINT(obj);
    _Py_VPYDBG(" region: %lu rc: %ld \n", Py_REGION(obj), Py_REFCNT(obj));
    _Py_VPYDBG("<< makeimmutable complete\n\n");

    return obj;
}