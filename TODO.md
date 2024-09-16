# TODO

> [!IMPORTANT]
> - The bytecode interpreter is a source of potential problems for us. Once the first few phases are in we need to really understand what it does and to what extent we need to enlighten it.

# Phase 1

## Notes

- Add a field to the object header to store the immutable state (used in later phase, so make ptr size)
- Make immortal
- Freeze performs object graph traversal and freezes all reachable objects
  * Use tp_traverse to look at the fields of the object
  * Look at type, which may or may not be returned by tp_traverse
  * Look at mapping and list items if they have them
- Numpy etc will require recompiling against the new runtime.
- How do we use the flag in the object header to determine if a write is allowed?
  * We need to intercept all setattr calls and check the flag
  * Slice types in numpy will need to know about the underlying objects immutability
- Exceptions can be raised as late as possible.  *i.e.,* to the actual point of mutation.
- Places where Py_SET_TYPE() is called are worth keeping in mind when we add in regions. They are always places where weird things are happening.
- ADD_TYPE() calls in various places designate places where special types are being created on the fly.
  We need to ensure we handle dependent type generation properly wrt regions



## Questions
- Do we need a new type flag to specify that the type supports deep immutability? tp_flags is a bitfield, so we could add a new flag.


## Milestones 
- [x] Add field
- [x] DFS to set flag and immortal
    - [x] [makeimmutable]
    - [x] [isimmutable]
    - [x] [PyObject_Type]
    - [x] [PyObject_Dir/PyObject_GetAttr]
    - [x] [PySequence_Check/PySequence_GetItem]
    - [x] [PySequence_Fast/PySequence_Fast_ITEMS] (where appropriate)
    - [x] [PyMapping_Check/PyMapping_Items]
- [x] Add NotWriteableError
- [ ] Checking flag on anything that could mutate.
    - [x] [PyObject_SetItem](Objects/abstract.c#L212) (`mp_ass_subscript`, `sq_ass_item`)
    - [x] [PySequence_SetItem](Objects/abstract.c#1913) (`sq_ass_item`)
    - [x] [PySequence_SetSlice](Objects/abstract.c#L1981) (`mp_ass_subscript`)
    - [x] [PySequence_DelItem](Objects/abstract.c#L1948) (`sq_ass_item`)
    - [x] [PySequence_DelSlice](Objects/abstract.c#L2004) (`mp_ass_subscript`)
    - [x] [PyObject_DelItem](Objects/abstract.c#L246) (`mp_ass_subscript`, `sq_ass_item`)
    - [x] [PyNumber_InPlaceAdd](Objects/abstract.c#L1248) (`nb_inplace_add`, `sq_inplace_concat`)
    - [x] [PySequence_InPlaceConcat](Objects/abstract.c#L1800) (`sq_inplace_concat`, `nb_inplace_add`)
    - [x] [PyNumber_InPlaceMultiply](Objects/abstract.c#L1273) (`nb_inplace_multiply`, `sq_inplace_repeat`)
    - [x] [PySequence_InPlaceRepeat](Objects/abstract.c#L1829) (`sq_inplace_repeat`, `nb_inplace_multiply`)
    - [x] [PyNumber_InPlacePower](Objects/abstract.c#L1302) (`nb_inplace_power`)
    - [x] [INPLACE_BINOP Macro](Objects/abstract.c#L1233) (`nb_inplace_or`, `nb_inplace_xor`, `nb_inplace_and`, `nb_inplace_lshift`, `nb_inplace_rshift`, `nb_inplace_subtract`, `nb_inplace_matrix_multiply`, `nb_inplace_floor_divide`, `nb_inplace_true_divide`, `nb_inplace_remainder`)
    - [x] [PyObject_SetAttr](Objects/object.c#L1162) (`tp_setattro`, `tp_setattr`)
    - [x] [PyObject_GenericSetAttr](Objects/object.c#L1619)
    - [x] [PyObject_GenericSetDict](Objects/object.c#L1625)
    - [x] [PyObject_GetBuffer](Objects/abstract.c#L382) (`tp_as_buffer`, need to set `readonly` flag)
    - [x] [PyObject_AsWriteBuffer](Objects/abstract.c#L355) (see above)
    - [x] [PyDict_SetItem](Objects/dictobject.c#L1877)
    - [x] [PyDict_SetDefault]
    - [x] [PyDict_DelItem](Objects/dictobject.c#L1972)
    - [x] [PyDict_Clear](Objects/dictobject.c#L2063)
    - [x] [PyList_SetItem](Objects/listobject.c#L272)
    - [x] [PyList_SetSlice](Objects/listobject.c#L730)
    - [x] [PyList_Sort](Objects/listobject.c#L2516)
    - [x] [list_sort_impl]
    - [x] [PyList_Reverse](Objects/listobject.c#L2545)
    - [x] [list_reverse_impl]
    - [x] [PyList_Append](Objects/listobject.c#L340)
    - [x] [list_append_impl]
    - [x] [PyList_Insert](Objects/listobject.c#316)
    - [x] [list_insert_impl]
    - [x] [list_clear_impl]
    - [x] [PySet_Add](Objects/setobject.c#L2319)
    - [x] [PySet_Pop](Objects/setobject.c#L2346)
    - [x] [PySet_Clear](Objects/setobject.c#L2289)
    - [x] [PySet_Discard](Objects/setobject.c#L2309)
    - [x] [set_add]
    - [x] [set_remove]
    - [x] [set_discard]
    - [x] [set_clear]
    - [x] [set_pop]
    - [x] [set_update]
    - [x] [PyTuple_SetItem](Objects/tupleobject.c#L113)
    - [ ] [PyException_SetTraceback](Objects/exceptions.c#L387)
    - [ ] [PyException_SetCause](Objects/exceptions.c#L401)
    - [ ] [PyException_SetContext](Objects/exceptions.c#L417)
    - [ ] [PyException_SetArgs](Objects/exceptions.c#L430)
    - [x] [PyCell_Set](Objects/cellobject.c#L63)
    - [x] [DELETE_DEREF](Python/bytecodes.c)
    - [x] [STORE_DEREF](Python/bytecodes.c)

# Phase 2

## Notes
- Make all the keys of the globals dictionary immutable
- Module dictionaries are made immutable (`PyModule_GetDict()` should return an immutable dict)
- Implement `_PyBehaviorRuntime_CheckInit` which returns whether behaviors can run.
- Make all types immutable (except for subtype checks, see below). This includes static/built-in types.
- Altered behavior if `_PyBehaviorRuntime_CheckInit` is true:
  * `Py_NewInterpreterFromConfig()` uses the immutable globals dictionary for new thread state
  * `PyState_AddModule()/PyState_RemoveModule()` acquire a global lock. The resulting module is made immutable once it is loaded.
  * `PyObject_GenericGetDict()` acquires the type lock if a dictionary is not already present. Dictionary is made immutable if created.
  * `PyType_IsSubtype()/add_subclass()` acquire the type lock.
  * `PyType_FromMetaclass()/PyType_FromModuleAndSpec()/PyType_FromSpecWithBases()/PyType_FromSpec()` create immutable types
