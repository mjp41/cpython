# TODO

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


## Questions
- Do we need a new type flag to specify that the type supports deep immutability? tp_flags is a bitfield, so we could add a new flag.


## Milestones 
- [ ] Add field
- [ ] DFS to set flag and immortal
- [ ] Checking flag on anything that could mutate.
    - [ ] [PyObject_SetItem](Objects/abstract.c#L212) (`mp_ass_subscript`, `sq_ass_item`)
    - [ ] [PySequence_SetItem](Objects/abstract.c#1913) (`sq_ass_item`)
    - [ ] [PySequence_SetSlice](Objects/abstract.c#L1981) (`mp_ass_subscript`)
    - [ ] [PySequence_DelItem](Objects/abstract.c#L1948) (`sq_ass_item`)
    - [ ] [PySequence_DelSlice](Objects/abstract.c#L2004) (`mp_ass_subscript`)
    - [ ] [PyObject_DelItem](Objects/abstract.c#L246) (`mp_ass_subscript`, `sq_ass_item`)
    - [ ] [PyNumber_InPlaceAdd](Objects/abstract.c#L1248) (`nb_inplace_add`, `sq_inplace_concat`)
    - [ ] [PySequence_InPlaceConcat](Objects/abstract.c#L1800) (`sq_inplace_concat`, `nb_inplace_add`)
    - [ ] [PyNumber_InPlaceMultiply](Objects/abstract.c#L1273) (`nb_inplace_multiply`, `sq_inplace_repeat`)
    - [ ] [PySequence_InPlaceRepeat](Objects/abstract.c#L1829) (`sq_inplace_repeat`, `nb_inplace_multiply`)
    - [ ] [PyNumber_InPlacePower](Objects/abstract.c#L1302) (`nb_inplace_power`)
    - [ ] [INPLACE_BINOP Macro](Objects/abstract.c#L1233) (`nb_inplace_or`, `nb_inplace_xor`, `nb_inplace_and`, `nb_inplace_lshift`, `nb_inplace_rshift`, `nb_inplace_subtract`, `nb_inplace_matrix_multiply`, `nb_inplace_floor_divide`, `nb_inplace_true_divide`, `nb_inplace_remainder`)
    - [ ] [PyObject_SetAttr](Objects/object.c#L1162) (`tp_setattro`, `tp_setattr`)
    - [ ] [PyObject_GenericSetAttr](Objects/object.c#L1619)
    - [ ] [PyObject_GenericSetDict](Objects/object.c#L1625)
    - [ ] [PyObject_GetBuffer](Objects/abstract.c#L382) (`tp_as_buffer`, need to set `readonly` flag)
    - [ ] [PyObject_AsWriteBuffer](Objects/abstract.c#L355) (see above)
    - [ ] [PyDict_SetItem](Objects/dictobject.c#L1877)
    - [ ] [PyDict_DelItem](Objects/dictobject.c#L1972)
    - [ ] [PyDict_Clear](Objects/dictobject.c#L2063)
    - [ ] [PyList_SetItem](Objects/listobject.c#L272)
    - [ ] [PyList_SetSlice](Objects/listobject.c#L730)
    - [ ] [PySet_Add](Objects/setobject.c#L2319)
    - [ ] [PySet_Pop](Objects/setobject.c#L2346)
    - [ ] [PySet_Clear](Objects/setobject.c#L2289)
    - [ ] [PySet_Discard](Objects/setobject.c#L2309)
    - [ ] [PyTuple_SetItem](Objects/tupleobject.c#L113)
