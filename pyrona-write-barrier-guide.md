# Lungfish Write Barriers — Migration Guide

This document explains how to insert region write barriers into CPython C
code to make a type region-aware. It is written to be supplied to an LLM that
migrates a whole type autonomously, walking through it function by function.
The "Running a migration" section below is the top-level procedure; the rest of
the document is the reference it draws on.

It assumes the **Lungfish Region Model — Conceptual Reference** is provided
alongside it. Terms like *local region*, *borrow*, *LRC*, *OSC*, *dirty*,
*bridge object*, *immutable*, and *closed* are defined there and are not
re-explained here.

This document is the operational reference. For anything not covered here,
ask rather than infer — do not invent runtime APIs, exception types, or
barrier behavior that is not stated.

---

## Running a migration

This section is the top-level procedure. Given a Python type name (e.g.
`"set"`), migrate the entire type autonomously, then stop. Do not pause for
approval between functions; the human reviews the finished type.

**Authoritative sources — only these, nothing else.** This guide and `pyrona.md`
are the complete reference for this migration. Do not read other type
implementations (e.g. `dictobject.c`) for context or examples. If you find
yourself reaching for an external source because something is not covered here,
that is a guide gap — stop, report what is missing and why you needed it, and
wait for the human to update the guide. Filling the gap yourself wastes tokens,
produces a one-off fix the next migration will not have, and obscures the
deficiency.

### The loop
1. **Find the spec** (`PyTypeObject` or `PyType_Spec`) for the type, as in
   "The type-level workflow" below.
2. **Request broad file-edit permission upfront.** Before making any edits,
   tell the user you will be modifying the migration target file throughout
   and ask for a single approval that covers all edits. Do **not** ask again
   for each individual change — repeated per-edit prompts slow the migration.
3. **Movability check.** Inspect the object struct named in `sizeof(...)`. If
   it holds non-movable pointers (`PyThreadState*`, `PyInterpreterState*`,
   frame pointers, or other non-`PyObject*` pointers into runtime/stack
   state), **stop and report it for human review** with the specific fields,
   then wait. Otherwise continue.
4. **Set the flag now.** Add `.tp_flags2 = Py_TPFLAGS2_REGION_AWARE` at the
   start, not the end — otherwise dispatching to the type during testing marks
   regions dirty and the tests can't observe correct behavior. Setting it early
   does not change runtime correctness.
5. **Walk the spec with the moving `// TODO`**, top to bottom, descending into
   sub-structs. For each function the TODO lands on, **complete all five steps
   of the per-function cycle below — including writing the test and waiting for
   all three review agents to finish — before writing any code for the next
   function.** Only then advance the TODO. Continue until the TODO reaches the
   end of the spec and all sub-structs.
6. **Run the tests once, at the end** (see "Tests"). The type is not complete
   until **all** of the following hold:
   - The full `test_regions` package passes with **zero failures and zero
     expected failures introduced by this migration**. An `@expectedFailure`
     that covers unimplemented barrier behavior is a migration defect, not a
     valid outcome. Fix the barrier, or stop and flag for human review (see
     "When to stop and ask the human") — do not hand the human an incomplete
     implementation disguised as an expected failure.
   - Every operation slot in the type spec has a corresponding test (see
     "Test coverage requirement" in the Tests section).
   If a test fails: (a) re-read the migrated function against the Definition
   of Done checklist — if any item is violated, fix the migration and re-run.
   Repeat step (a) until the migration passes every checklist item or two full
   fix-and-rerun cycles have produced the same failure. (b) Only then inspect
   the test against the "Tests" section — if the test violates a stated
   requirement (wrong object type, absolute instead of relative counter,
   missing region object), fix the test and re-run. If after one read of the
   test no stated requirement is violated, treat (b) as inconclusive and stop
   — flag for human review. Do not change the test to make a broken migration
   pass.
7. **Produce the final output** (below).

### Per-function cycle
For the function currently under the TODO:
1. **Migrate** the function, following "Migrating a single function".
2. **Write the test** for this function before doing anything else. The test
   must be written while the function's logic is fresh, and before review —
   the review agents check both the migration and the test together, so the
   test must exist first. See "Tests" for what to write. If the function has
   no direct Python API surface (e.g. a shared internal helper), write the
   test against its shallowest public caller and note the linkage explicitly.
   Tests are written now but only *run* at the end once the build is stable —
   see step 6 of "The loop".
3. **Review both** with independent sub-agents — not yourself. Self-review
   shares the blind spots that produced any error, so it is not trusted. Hand
   the *original* function, the *migrated* function, the new test, and this
   guide to separate review agents with the instruction in "The review step"
   below. The only permitted exception: if migrating this function required
   descending into one or more callees for context, declare the group upfront
   ("migrating `foo` and its callees `bar`, `baz`"), complete all of them and
   write all their tests, then review the entire group together. Groups may not
   be formed retrospectively to justify deferred reviews.
4. If the review reports violations, fix them and re-review. Repeat until the
   review is clean.
5. Advance the TODO to the next function. **Do not advance until step 4 is
   satisfied.** Deferring all reviews to after the full migration is prohibited
   — a batch review cannot prevent errors from propagating into subsequently
   migrated functions that call the reviewed one.

### The review step
**Spawn three independent review sub-agents using the Agent tool** — not
yourself, not the same agent twice. Self-review shares the blind spots that
produced any error, so it is not trusted. Call the Agent tool three times (in
parallel if the tool supports it) with the instructions below. Each agent
receives the *original* function(s), the *migrated* function(s), the *new
test(s)*, and this guide. The migration proceeds to "Final output" only when
**all three reviews report no violations**. If any review finds a violation,
fix it and re-run all three reviews for the functions that changed. "Re-run
from scratch" is scoped to the changed function(s): functions that were not
touched by the fix do not need re-review unless the fix altered a shared helper
they also call.

**Model for review agents**: Spawn each review agent with `model:
"claude-sonnet-4-6"` and `effort: "medium"`. The review task is structured and
checklist-driven — the agent does not need to discover what to look for, only
apply the criteria in its prompt systematically. If a review
comes back clean but the function meets any stop condition listed
in “When to stop and ask the human”, flag for human review regardless of the
clean review result.

Each agent has a different lens:

**Agent 1 — Barrier correctness and behavior preservation.** Give it this instruction:

> You did not write this code. Review the migrated functions against the
> attached Migration Guide. Check specifically: every `Py_DECREF`/`Py_XDECREF`
> on a stack-held value has a preceding `RemoveLocalRef`/`CLEARLOCAL`; every
> exit path (`return`, `goto`) is symmetric — borrows added are removed on
> every path; every **owning-reference** barrier failure (`AddRef`, `AddRefs`,
> `AddRefsArray`, `TakeRef`, `TakeRefs`, `XSETREF`, `XSETNEWREF`) is handled in
> the function's own error-handling style; local/borrow barriers never fail, so
> flag any code that treats a local/borrow barrier's *return* as a failure
> signal (e.g. `if (PyRegion_AddLocalRef(...)) { error }`) as a defect to remove
> — but do NOT flag a NULL guard on the *value* passed to a barrier (e.g.
> `if (key != NULL) PyRegion_AddLocalRef(key);`), nor a legitimate inspection of
> the strong reference that `NewRef`/`XNewRef` return; if the function is inside
> a critical section, every failure path exits it; every skipped barrier is
> justified with an `assert`;
> `TakeRef` is only used when the caller genuinely owns the stack reference
> (never on a borrowed argument); and `AddRef` vs `TakeRef` is chosen
> correctly per the guide's classification table. Also verify that the
> migration did not change observable behavior: return values, error codes,
> exception types, and side effects must be identical to the original for all
> inputs — the only permitted additions are barrier calls and `assert`s.
> Report every violation specifically with the line number and the rule it
> breaks.

**Agent 2 — Test completeness.** Give it this instruction:

> You did not write these tests. Review them against the attached Migration
> Guide's "Tests" section and "Test coverage requirement". Check specifically:
> every operation slot in the type spec has at least one test; in-place
> operators (`|=`, `&=`, `-=`, `^=`) are each covered separately from their
> non-in-place counterparts; the three operation shapes (neutral,
> returning-borrow, transfer) are each tested; tests assert *relative* counter
> changes (record before/after), not absolute values; returned references are
> kept alive to observe the borrow and cleared to prove symmetry; failure paths
> are tested, not just the happy path; and no test is marked `@expectedFailure`
> to hide an unimplemented barrier. For failure paths specifically: every
> mutating operation that accepts a caller-supplied element has an isolation
> enforcement test — a sibling-region element is rejected with `RuntimeError`,
> the container is unchanged (length/membership matches pre-call state), the
> element's ownership is unchanged, and the region's LRC is neutral; batch
> operations (`extend`, `update`, `__ior__`, etc.) have an atomicity test using
> a `[local, sibling-owned, local]` layout to confirm all-or-nothing rollback.
> Report every gap specifically.

**Agent 3 — Edge cases and corner shapes.** Give it this instruction:

> You did not write this code or tests. Review for edge cases the other
> reviewers might miss: frozenset vs mutable-set branches that diverge in
> barrier handling; self-referential operations (e.g. `s & s`, `s.isdisjoint(s)`);
> empty-collection fast paths; iterator exhaustion vs early abandonment;
> operations that call `__eq__` or `__hash__` on foreign objects (arbitrary
> Python can run, region state can change mid-call); and in-place operators
> that return `self` (must use `PyRegion_NewRef`, not `Py_NewRef`). For each
> edge case, check whether the migration and tests cover it. Report every gap.

Note: three independent reviews reduce how many errors reach the test suite,
but they do not eliminate the possibility — they still share model-level blind
spots with the migrating agent. Compilation and the `test_regions` run at the
end are the checks that do not depend on model judgment, and are the real gate.

### When to stop and ask the human (only these)
Proceed autonomously except in these cases, where you stop and ask:
- The movability check is positive (non-movable pointers found).
- A staged-reference shape is required (see "Staged references") — flag, do not
  implement.
- A public function's signature would need to change to migrate it — flag, do
  not implement.
- The guide does not resolve the situation — specifically: you cannot determine
  the source region after reading the function, cannot determine new-ref vs
  transfer after reading the function and its direct callees, or cannot
  establish the RC/region behavior of a callee after reading it.
- You find yourself needing context that is not in this guide or `pyrona.md`
  (e.g. you want to read another type's implementation for examples or
  patterns). Do not read it. Report what is missing from the guide and why,
  so the guide can be fixed permanently.
- A barrier cannot be implemented because the required API does not exist or
  the behavior is genuinely undefined. Flag for human review. **Do not** mark
  the test `@expectedFailure` and hand over an incomplete implementation — that
  hides the defect and forces the human to rediscover it.

### Final output
When the whole type is migrated:
- The migrated type: all functions, plus `.tp_flags2 = Py_TPFLAGS2_REGION_AWARE`.
- The tests added.
- The `test_regions` run result, **plus the run result for the type's own test
  module** (e.g. `test_set` when migrating `set`) — both must pass.
- A list of everything flagged for human review (movability, staged refs,
  public-signature changes, unresolved questions).
- **Disputed findings**: for each review finding the agent evaluated and chose
  not to act on, record the finding text, the agent's counter-reasoning, and
  the specific code location. This allows the human reviewer to adjudicate
  without re-running the review.

---

## What a write barrier is, in one paragraph

CPython tracks object lifetimes with reference counts (`Py_INCREF` /
`Py_DECREF`). Lungfish additionally tracks *which region owns each object* and
*how many borrows point into each region*. A write barrier is the extra
bookkeeping call placed next to a reference-count change so the region
system's counters (LRC, OSC, ownership) stay correct. Migrating a function
means: for every reference-count change, add the matching region barrier — or
add an `assert` documenting why it can be safely skipped.

---

## Three facts that drive every decision

1. **Stack-held values are borrows (local references).** A value in a C local
   variable — including a value held in a `_PyStackRef` slot — is a borrow
   into the local region. A borrow can be transferred into an owning
   reference stored on a heap object using `TakeRef(src, tgt)`, which adds the
   owning edge from `src` and then removes the local borrow.

2. **References stored into a heap object are owning references and are not
   reversible.** Once an owning reference is added (`AddRef` / `TakeRef`), it
   cannot be cheaply undone. This is why the owning-reference barriers —
   `AddRef`, `AddRefs`, `AddRefsArray`, `TakeRef`, `TakeRefs`, `XSETREF`,
   `XSETNEWREF` — can *fail* and must be checked: the failure is reported
   *before* the irreversible store, so you can still bail out cleanly.

3. **Local (borrow) operations always succeed.** Adding or removing a borrow —
   `AddLocalRef`, `AddLocalRefs`, `NewRef`, `XNewRef`, `RemoveLocalRef`,
   `CLEARLOCAL`, `XSETLOCALREF`, `XSETLOCALNEWREF` — never fails, and each (except `NewRef`) is
   also NULL-safe (a no-op on `NULL`). The reason: to take a local ref you must
   already hold a borrow that reaches the object, which guarantees its region
   is open. Only creating an *owning* reference into a heap object can fail, so
   a local operation never needs a failure-handling path.

**Use proper barriers with full error handling.** Skipping a barrier in favour
of an `assert` is permitted only when one of these specific structural
conditions holds:

1. The object was created in the current frame with no path for it to enter a
   region before this point (e.g., return value of `PyList_New`, `PyDict_New`,
   `PySet_New` — functions that always return a freshly allocated object).
2. The object is known immutable — `PyRegion_NeedsReadBarrier` would return
   false in all cases (e.g., `Py_None`, `Py_True`, `Py_False`, small integers,
   interned strings).
3. The branch has already verified that the referenced object does not need a
   barrier, using `PyRegion_NeedsReadBarrier`, `PyRegion_IsLocal`, or
   `_Py_IsImmutable`.

The condition must be stated explicitly in a comment adjacent to the assert.
An assert that provides no structural justification must be replaced with a
proper barrier. Note that asserts are stripped in optimized builds; a barrier
replaced by an assert is absent at production runtime.

If none of the three named structural conditions above is met and stated in a
comment, use a proper barrier with error handling.

---

## The type-level workflow

You migrate a type by walking its specification top to bottom, migrating one
referenced function at a time.

### Step 1 — Find the type specification
This is usually a constant `PyTypeObject`:
```c
PyTypeObject PySet_Type = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0)
    "set",                              /* tp_name */
    sizeof(PySetObject),                /* tp_basicsize */
    ...
    set_dealloc,                        /* tp_dealloc */
    ...
};
```
It may instead be a `PyType_Spec` with a `.slots` array:
```c
PyType_Spec Dialect_Type_spec = {
    .name = "_csv.Dialect",
    .basicsize = sizeof(DialectObj),
    .flags = (...),
    .slots = Dialect_Type_slots,
};
```

### Step 2 — Movability check (flag for human review)
Look at the object struct named in `sizeof(...)` (the `tp_basicsize` /
`.basicsize` argument — e.g. `PySetObject`). If that struct contains pointers
that cannot be moved between sub-interpreters — for example
`_PyInterpreterFrame *`, `PyFrameObject *`, `PyThreadState *`,
`PyInterpreterState *`, or any pointer type whose declared name contains
`Frame`, `Thread`, `Interpreter`, or `State` — flag the type for human review
as a non-movable candidate. For any other non-`PyObject *` pointer field: flag
for human review if the field is not a C primitive pointer
(e.g. `char *`, `void *`, `int *`).

Marking a type non-movable (done by adding it to `_PyRegion_GetMoveability`)
is **always a human decision**. As an LLM, **flag the type for review with the
specific fields that prompted the concern**, and continue with the migration
regardless of the outcome — the movability decision does not block migrating
the functions.

### Step 3 — Walk the spec with a moving `// TODO`
Place a `// TODO` comment at the top of the spec, just before the first
function slot. Migrate the function under the TODO; once it reaches the
Definition of Done (below), move the TODO down to the next function slot.
Repeat to the end of the struct.

```c
PyTypeObject PySet_Type = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0)
    "set",                              /* tp_name */
    sizeof(PySetObject),                /* tp_basicsize */
    0,                                  /* tp_itemsize */
    // TODO   <-- migrate set_dealloc, then move this line down
    set_dealloc,                        /* tp_dealloc */
    0,                                  /* tp_vectorcall_offset */
    ...
};
```

Some slots point to sub-structs (`tp_as_number`, `tp_as_sequence`, etc.).
When the TODO reaches one, descend into that struct and apply the same
moving-TODO walk through its function pointers before returning to the parent
spec:
```c
static PyNumberMethods set_as_number = {
    0,                                  /*nb_add*/
    // TODO   <-- migrate set_sub, then move down
    set_sub,                            /*nb_subtract*/
    ...
};
```

`tp_methods` and `tp_getset` are **arrays**, not sub-structs, but must be
walked all the same. For `tp_methods`, every non-NULL `.ml_meth` entry is a
function to migrate. For `tp_getset`, every non-NULL `.get` and `.set` entry
is a function to migrate. Do not skip them because they are array entries
rather than named struct fields.

**Before you start migrating any function**, produce an explicit numbered list
of every `.ml_meth` entry in `tp_methods` (and every `.get`/`.set` entry in
`tp_getset`). Record this list as your working inventory. Cross each entry off
only after it has been migrated and its per-function review has passed. This
list is the ground truth — if a function is not on the list, it was not
enumerated and may be silently skipped during the walk.

This walk defines the order in which functions are migrated, and guarantees
every function reachable from the type's spec is covered.

### Step 4 — The region-aware flag
Mark the type region-aware by adding:
```c
.tp_flags2 = Py_TPFLAGS2_REGION_AWARE
```
Set this **at the start of the migration, not the end** (see "Running a
migration"). Until the flag is set, dispatching to the type marks regions dirty
(safe, but it defeats the purpose during testing — the tests can't observe
correct borrow behavior). Setting it does not change runtime correctness; it
only stops the dirty-marking. It is set early precisely so the per-function
tests are meaningful, and the finished type is reviewed by a human before
production.

**Placement rule**: `.tp_flags2` must be added as a **trailing designated
initializer** — place it after all positional initializers in the struct
literal. Never insert it in the middle of a positional sequence. In C99, once
a designated initializer appears mid-sequence, the compiler fills subsequent
*positional* initializers starting from the slot *after* the designated field,
silently shifting every later slot and corrupting the struct. Adding the flag
at the end avoids this entirely.

---

## Migrating a single function

When the TODO lands on a function, open its definition and work through it.

> **Anti-pattern — name-based reasoning.** The worked examples in this guide
> use names from a past set migration (`set_next`, `setiterobject`, `other->table`,
> etc.). Do not use those names as evidence that a pattern applies. A function
> named `container_next` in a new migration does not inherit the properties of
> `set_next` in the example. Apply every pattern by structural analysis of the
> actual function — what it does to reference counts and struct fields — not by
> recognising a name from an example.

### Walk the call graph
If the function calls another function that is **not yet migrated**, check
whether that callee does any reference-count work:
- If the callee does **no** RC work, you do not need to migrate it to proceed.
  **Positive verification required** — run this check by actually reading the
  callee, not by inferring from its name: grep the callee for
  `Py_INCREF`, `Py_DECREF`, `Py_XDECREF`, `Py_XINCREF`, `Py_SETREF`,
  `Py_CLEAR`, `Py_NewRef`, `Py_XNewRef`, and `PyRegion_`. Also grep for any
  assignment through a struct field pointer: any occurrence of `->` on the
  same line as a `=` that is not a comparison operator (`!=`, `==`, `<=`,
  `>=`). This covers bare field writes (`entry->key = …`), array-indexed field
  writes (`->table[i].key = …`), and struct-level copies. A function that
  scores zero on RC-symbol grep but writes `PyObject*` fields directly may
  transfer ownership without a reference count change — it still requires
  migration or human review. Only claim "no RC work" if none of these appear.
  This check must be run by actually reading the callee; name similarity to an
  already-migrated function is not sufficient.
- If the callee **does** RC work, migrate it first (descend into it), then
  return.
- If the callee is a **shared internal helper** (called from more than one spec
  function): migrate it when first encountered; note it as done; skip it on
  subsequent encounters. When a second spec function reaches it, re-read the
  callee and verify: (a) every RC site has a barrier or a comment-annotated
  assert, and (b) the callee's error-exit path does not assume a lock or
  critical section that the new caller does not hold.
- If the callee is **already migrated / region-aware**: you can rely on it
  without descending. For a type slot: verified by reading the type's source
  and confirming `.tp_flags2 = Py_TPFLAGS2_REGION_AWARE` is present in its
  `PyTypeObject` or `PyType_Spec`. For a free function (not a type slot):
  verified by reading the function body and confirming every RC site has a
  `PyRegion_` call or a comment-annotated assert. A name or assumption is not
  sufficient in either case.
- If the call **dispatches through a type slot** to code that may not be
  region-aware, guard it with `PyRegion_NotifyTypeUse(type)` (see reference).

### Classify each reference-count site
For every RC operation in the function, pick the barrier:

| Situation | Barrier |
|-----------|---------|
| Returning a `PyObject*` to the caller (getter, constructed result, field) | `PyRegion_NewRef` / `PyRegion_XNewRef` |
| `Py_DECREF` / `Py_XDECREF` on a stack-held value | `PyRegion_RemoveLocalRef` then `Py_DECREF`, or `PyRegion_CLEARLOCAL` |
| Replacing a value held in a **stack-local** slot (2-arg, no `src`; not a heap field) | `PyRegion_XSETLOCALREF(dst, val)` (steal `val`) / `PyRegion_XSETLOCALNEWREF(dst, val)` (new ref to `val`) — never fails; see "Local replacement helpers" |
| Storing a value you **received or are also keeping** into a heap object (you create a new owning reference; RC is incremented) | `PyRegion_AddRef` before the store, then `Py_NewRef` |
| Storing several such new references in one logical step | `PyRegion_AddRefs` (fixed count) or `PyRegion_AddRefsArray` (runtime count) |
| Handing off a reference you **already own on the stack** into a heap object — you are not keeping it and the RC does not change (a transfer) | `PyRegion_TakeRef` before the store |
| **Stealing** a value from a heap field and returning it to the caller **without changing the RC** (the container loses it, the caller gains it) | `PyRegion_AddLocalRef(val)` *before* clearing the field, then clear the field, then `PyRegion_RemoveRef(src, val)` — see "Steal-and-return" below |
| Replacing a **heap field** (3-arg, takes `src`), stealing the value | `PyRegion_XSETREF(src, field, val)` — can fail |
| Replacing a **heap field** (3-arg, takes `src`) with a **new owning reference** to the value | `PyRegion_XSETNEWREF(src, field, val)` — can fail |
| `Py_SETREF(field, val)` — old value non-null | `PyRegion_XSETREF` (the X-prefix is harmless on a non-null old value) |
| Clearing a field on a heap object | `PyRegion_CLEAR` |
| `Py_DECREF` on a field in `dealloc` | `PyRegion_RemoveRef(self, field)` before `Py_DECREF` |
| Dispatching through a type slot | `PyRegion_NotifyTypeUse(type)` before the dispatch |
| Reusing/recycling an object as if newly allocated | `PyRegion_RecycleObject(obj)` |
| Bulk loop: storing N references into one container as an all-or-nothing step | **If `src` is local** (`PyRegion_IsLocal(src)` is true), use per-item `PyRegion_AddLocalRef` — it never fails, so no rollback and no heap allocation are needed. **If `src` is not local**, build the full array first, then `PyRegion_AddRefsArray(src, n, array)` before the loop; on failure propagate the error without having stored anything. Both branches share the copy loop that follows; the local branch needs no `RemoveLocalRef` cleanup after the copy. |
| Copying a struct that contains `PyObject*` fields (e.g. `tmp = *self`) — the copy's fields are new stack borrows | `PyRegion_AddLocalRef` / `PyRegion_AddLocalRefs` for each non-NULL field you will use; each `AddLocalRef` must be paired with a `Py_INCREF`/`Py_XINCREF` on that field; match cleanup with `PyRegion_RemoveLocalRef` / `PyRegion_CLEARLOCAL` — see "Struct-snapshot pattern" |
| Temporarily holding a value **borrowed from a heap field** across a call that may run Python code — e.g. `Py_INCREF(key)` to pin a table entry, call `PyObject_RichCompareBool`, then `Py_DECREF(key)` | `PyRegion_AddLocalRef(key)` before the `Py_INCREF`; `PyRegion_RemoveLocalRef(key)` before the `Py_DECREF`. The callee can remove the value from the container through arbitrary `__eq__`/`__hash__` code; the region tracker must know the borrow exists. This is the most common pattern in hash-table lookup and comparison functions. |

### Steal-and-return

"Steal-and-return" is the pattern for operations like `pop`: the function removes
a value from a heap field and returns it to the caller **without incrementing
the RC**. The container loses the owning reference; the caller gains a local
borrow. The RC does not change.

Order matters here for **correctness, not for failure handling.** `AddLocalRef`
never fails, but you must still call it *before* clearing the field: clearing
drops `so`'s owning reference, after which `so`'s region may close and `key` may
no longer be reachable to borrow. It is the pre-existing owning reference — not
the `AddLocalRef` call — that keeps the region open until step 2. The
never-fails guarantee assumes the region is still open at the call, so do
**not** reorder the barrier to after the clear even though the call cannot fail.

```c
// PATTERN: steal entry->key from 'so' and return it to caller
key = entry->key;

// 1. Record the local borrow WHILE so still owns it. AddLocalRef never fails.
PyRegion_AddLocalRef(key);

// 2. Clear the field (so no longer owns key).
entry->key = dummy;
entry->hash = -1;
...

// 3. Remove the owning edge from so — NO Py_DECREF follows.
//    The strong reference is not dropped; it is inherited by the caller
//    via the return value. RC stays at the same value throughout.
PyRegion_RemoveRef(so, key);

// 4. Return key — caller inherits the local borrow recorded in step 1.
return key;
```

Note: `RemoveRef`'s reference entry says "call before the matching `Py_DECREF`" —
that applies to the *drop* case (dealloc, field replace). In steal-and-return the
strong reference is not dropped; it is handed to the caller, so no `Py_DECREF`
is needed and none should be added.

**`AddRef` vs `TakeRef` — decide by what you hold:**
- You received a borrowed argument, or you are storing a value you also keep a
  reference to → you are creating a **new owning reference**: `PyRegion_AddRef`
  (or `PyRegion_XSETNEWREF` for a field replace), followed by `Py_NewRef`.
- You already own a reference on the stack (you created it, or were handed
  ownership) and you are transferring it into a heap object without keeping it
  → `PyRegion_TakeRef`. That is the *only* situation `TakeRef` is for.

**Anti-pattern — never create a local ref just to transfer it.** Do not write
`NewRef`/`AddLocalRef` to make a borrow and then immediately `TakeRef` it:
```c
// WRONG: pointless round-trip — makes a local ref, then converts it
PyObject *newkey = PyRegion_NewRef(key);
if (PyRegion_TakeRef(so, newkey)) {
    PyRegion_RemoveLocalRef(newkey);
    Py_DECREF(newkey);
    return -1;
}
```
If `key` is a value you are storing into `so` as a new owning reference, go
straight there:
```c
// RIGHT: a new owning reference, no local ref at all
if (PyRegion_AddRef(so, key)) {
    return -1;
}
so->slot = Py_NewRef(key);
```
Reach for `TakeRef` only when you genuinely already own the stack reference and
are giving it away.

### The known-local optimization (only under the three structural conditions)
When you can prove a value is local — for example a list you just created with
`PyList_New` — a reference from it is always allowed, even into an object that
lives in a region. In that case you may replace a full barrier with
`PyRegion_AddLocalRef` plus an `assert`, and replace a barrier-requiring
`Py_DECREF` of a value you *received from a callee* with an
`assert(!PyRegion_NeedsReadBarrier(x))`.

**Do not confuse "known-local" with "possibly-cached/interned."** Some C API
functions may return a shared or immortal object rather than a freshly
allocated one — for example `PyLong_FromLong` for small integers,
`PyBool_FromLong`, or `PyUnicode_InternInPlace`. These are not "local" in the
region sense; they are *immutable*. Apply the immutable exemption (condition 2
above), not the known-local exemption. The distinction matters: a known-local
object has no region; an immutable object may be in a region but requires no
barrier because it cannot transfer ownership.

The following example is from a past set migration. The names (`container_next`,
`entry->val`, `so`) are placeholders for real set-specific names; apply the
pattern by recognising the structure, not the names:
```c
// Example from a past migration — field names are illustrative
while (container_next(so, &pos, &entry)) {
    PyRegion_AddLocalRef(entry->val);   // never fails
    PyList_SET_ITEM(keys, idx++, Py_NewRef(entry->val));
}
...
listrepr = PyObject_Repr(keys);
Py_DECREF(keys);   // keys was created here and is always local; no barrier needed
...
listrepr = PyUnicode_Substring(listrepr, 1, PyUnicode_GET_LENGTH(listrepr)-1);
// listrepr came from a callee; assert it does not need region tracking
// (strings are immutable) to document why no RemoveRef is used:
assert(!PyRegion_NeedsReadBarrier(listrepr));
Py_DECREF(listrepr);
```
Use the known-local optimization only when one of the three structural
conditions listed under "Use proper barriers with full error handling" is met
and stated in an adjacent comment. In all other cases, use the fully-safe
alternative.

Conceptually, when `keys` is local, `PyRegion_AddRef(keys, entry->val)` is
*equivalent* to `PyRegion_AddLocalRef(entry->val)` — adding a reference from a
local object is exactly a local borrow, not an owning cross-region edge. But
**write it as `PyRegion_AddLocalRef` (as in the loop above), never as `AddRef`
with an ignored return.** `AddRef` is an owning barrier that *can* fail and
whose return must always be checked (fact 2); the only thing that makes it safe
to ignore here is the invisible fact that `keys` is local. An ignored-return
`AddRef` is exactly the shape that becomes a silent illegal store the moment it
is copied to a site where `src` is a real region object — so do not emit it.
When you have proven the destination is local, reach for the local form.

### Struct-snapshot pattern

When code copies an entire struct that contains `PyObject*` fields — for example
to iterate a collection safely while holding a snapshot:

```c
// IterObj and it_container are placeholders — use the real type and field names
IterObj tmp = *iter;
Py_XINCREF(tmp.it_container);
```

The copy `tmp` now has its own stack reference to `tmp.it_container`. That
reference is a new local borrow. Migrate it the same way as any stack-held
`Py_INCREF`:

```c
IterObj tmp = *iter;
PyRegion_AddLocalRef(tmp.it_container);   // null-safe, never fails
Py_XINCREF(tmp.it_container);             // RC change to match the borrow
```

And the matching cleanup:
```c
PyRegion_CLEARLOCAL(tmp.it_container);   // RemoveLocalRef + XDECREF
```

Use `PyRegion_AddLocalRefs(a, b, ...)` when the snapshot covers multiple fields
that must be borrowed as an all-or-nothing step.

### Handle failure the way the function already does
Every owning-reference barrier — `AddRef`, `AddRefs`, `AddRefsArray`,
`TakeRef`, `TakeRefs`, `XSETREF`, `XSETNEWREF` — can fail (returns nonzero). The
local/borrow operations never fail (see fact 3). On failure you must **undo what you have done so far and
propagate the error in the same style the function already uses for other
failures** — there is no single universal cleanup. The common shape is: undo
any borrow added (`PyRegion_RemoveLocalRef` / `CLEARLOCAL`), `Py_DECREF`
anything this function incremented, then `return -1` / `return NULL` /
`goto error` as the surrounding code does.

If the function is inside a `Py_BEGIN_CRITICAL_SECTION`, every failure path
must still exit the critical section — prefer `goto error` (which runs the
`Py_END_CRITICAL_SECTION`) over an early `return`.

### Definition of Done for a function
A function is done when **all** of the following hold:

- [ ] Every reference-count operation has a corresponding region barrier,
      **or** an `assert` documenting why the barrier can be skipped.
- [ ] Every exit path (every early `return` and `goto`) is symmetric: borrows
      added are removed, owning references added before a later failure are
      accounted for, and the region counters are left correct.
- [ ] Every called function is already migrated/region-aware (verified per
      "Walk the call graph"), does no RC work (verified per the positive
      verification procedure there), or — for dynamic dispatch — is guarded
      by `NotifyTypeUse`.
- [ ] If inside a critical section, every failure path exits it.
- [ ] No staged-ref pattern was needed without being flagged for human review.
- [ ] At least one test for this function has been **written** (before review,
      per the per-function cycle). The test exercises the function's barrier
      shape (neutral / returning-borrow / transfer). Tests are run at the end
      of the full migration, not per-function — "written" is the per-function
      gate; "passing with no `@expectedFailure`" is the final gate.

If you cannot satisfy this — the barrier classification table does not produce
a single unambiguous row for an RC site after reading the function and its
direct callees, a function with no error channel that needs a failing barrier,
or the function matches the staged-reference shape (multiple conditional
branches each discovering references, all of which must be validated before any
is committed, and neither `AddRefs` nor `AddRefsArray` can express the set —
see "Staged references") — **stop and flag the function for human review** with
a short explanation.

---

## Reference: the barriers

All barriers come from `region.h`. `src` is the object the reference is stored
*into*; `tgt` is the object referred *to*.

### Creating references into a heap object

**`PyRegion_AddRef(src, tgt)`** — record a **new** reference from `src` to
`tgt`. Call *before* the `Py_NewRef`/`Py_INCREF`. Returns nonzero on failure.
```c
if (PyRegion_AddRef(self, v)) {
    // cleanup as the function does elsewhere
    return -1;
}
self->value = Py_NewRef(v);
```

**`PyRegion_AddRefs(src, tgt1, tgt2, ...)`** — variadic form for a **fixed**
number of new references added together: succeeds only if a reference to all
targets can be created, otherwise nothing changes. Because owning references
are not reversible, references that must be added as one logical step should
be added with a single call so the group is all-or-nothing.

**`PyRegion_AddRefsArray(src, count, array)`** — same all-or-nothing semantics
as `AddRefs`, but for a count known only at **runtime**: it succeeds if a
reference can be created to every object in `array`, and on failure leaves
everything unchanged. Use this when you need to add a runtime-determined number
of references atomically.

**`IsLocal` fast-path:** `AddRefsArray` requires building a heap-allocated array
upfront. When `PyRegion_IsLocal(src)` is true you can avoid that allocation
entirely by using per-item `PyRegion_AddLocalRef` instead — `AddLocalRef` never
fails, so the loop needs no rollback and no array at all:

```c
// Field names (items, capacity, count, dst_slot, src_slot) are placeholders —
// use the real struct fields for the type you are migrating.
if (PyRegion_IsLocal(dst)) {
    /* local: per-item AddLocalRef — never fails, no rollback, no heap allocation */
    for (i = 0; i < other->capacity; i++) {
        key = other->items[i].val;
        if (key != NULL) {
            PyRegion_AddLocalRef(key);
        }
    }
} else {
    /* non-local: must use AddRefsArray for all-or-nothing ownership barrier */
    Py_ssize_t k = 0;
    PyObject **arr = PyMem_New(PyObject *, other->count);
    if (arr == NULL) { PyErr_NoMemory(); return -1; }
    for (i = 0; i < other->capacity; i++) {
        key = other->items[i].val;
        if (key != NULL) arr[k++] = key;
    }
    if (PyRegion_AddRefsArray(dst, (int)k, arr)) {
        PyMem_Free(arr);
        return -1;
    }
    PyMem_Free(arr);
}
for (i = 0; i < other->capacity; i++, dst_slot++, src_slot++) {
    key = src_slot->val;
    if (key != NULL) {
        dst_slot->val = Py_NewRef(key);
        dst_slot->hash = src_slot->hash;
    }
}
```

The key distinction: `AddLocalRef` never fails, whereas the owning references
recorded by `AddRefsArray` can (and are not reversible). This is why only the
non-local branch needs the all-or-nothing array and an error path; the local
branch is a plain loop.

The `IsLocal` branch does **not** need a `RemoveLocalRef` cleanup loop after the
copy — removing them would make the two branches asymmetric (the `AddRefsArray`
branch has no corresponding cleanup). The `AddLocalRef` calls record the borrows
the copy relies on; once `Py_NewRef` establishes proper ownership during the
copy, no explicit release is needed.

Apply this pattern when `PyRegion_IsLocal(dst)` returns true at the call site,
or when `dst` was returned by a call that always allocates a new object
(e.g. `PyList_New`, `PyDict_New`, `PySet_New`) and has not been stored into a
heap object since that call.

**`PyRegion_TakeRef(src, tgt)`** — transfer an **existing owned** reference
(typically a stack-held borrow) into `src`. Use when the code stores into a
field *without* changing the RC — the reference is handed off, not duplicated.
Returns nonzero on failure.
```c
if (PyRegion_TakeRef(self, newitem)) {
    PyRegion_RemoveLocalRef(newitem);
    Py_DECREF(newitem);
    return -1;
}
self->item = newitem;
```

**`PyRegion_TakeRefs(src, tgt1, ...)`** — variadic, all-or-nothing form of
`TakeRef`. On **success** it adds the owning reference to `src` for *every*
target **and** removes the corresponding local borrow for each — the success
path consumes the local borrows for all targets, so you must not remove them
again. On **failure** it leaves all local refs in place and makes no change,
so the caller must still remove/decref them in its ordinary error path.
```c
if (PyRegion_TakeRefs(self, a, b)) {
    // failure: a and b are still local borrows we own — clean them up
    PyRegion_RemoveLocalRef(a);
    PyRegion_RemoveLocalRef(b);
    Py_DECREF(a);
    Py_DECREF(b);
    return -1;
}
// success: the local borrows for a and b were consumed; do NOT remove them
self->a = a;
self->b = b;
```

### Removing references

**`PyRegion_RemoveRef(src, tgt)`** — remove a reference from `src` to `tgt`.
Call *before* the matching `Py_DECREF`. Does not fail. This is the barrier for
`dealloc` and for hand-written field replacement.
```c
PyObject *old = self->value;
self->value = NULL;
PyRegion_RemoveRef(self, old);
Py_DECREF(old);
```

### Field replacement helpers

**`PyRegion_XSETREF(src, field, val)`** — region-aware `Py_XSETREF` that
**steals** `val`; internally takes the reference to the new value and
removes + `Py_XDECREF`s the old value. Can fail. It is all-or-nothing: on
failure the field and both the old and new references are left unchanged, so
the operation either completes fully or has no side effects.
```c
if (PyRegion_XSETREF(self, self->args, seq)) {
    return -1;
}
```

**`PyRegion_XSETNEWREF(src, field, val)`** — replaces the old field with a
**new** owning reference to `val` (i.e. the RC is incremented) rather than
stealing an existing one. `val` may be `NULL` (it internally
uses `Py_XNewRef`). Can fail.
```c
if (PyRegion_XSETNEWREF(self, self->value, value)) {
    return -1;
}
```

**`PyRegion_CLEAR(src, field)`** — region-aware `Py_CLEAR`; removes the
reference and NULLs the field. Does not fail.
```c
PyRegion_CLEAR(self, self->traceback);
```

### Local (stack) reference management

**`PyRegion_NewRef(tgt)` / `PyRegion_XNewRef(tgt)`** — replacements for
`Py_NewRef` / `Py_XNewRef` that create a new **local (stack) borrow**: they add
a borrow (increment the target region's LRC) and return a new strong reference.
The return/getter case is the most common, but they may be used anywhere you
need a new stack reference (see below). **Neither fails**, so no failure check
is ever needed. As with their `Py_*` counterparts, `XNewRef` is NULL-safe
(returns `NULL` on a `NULL` target) while `NewRef` requires a non-NULL `tgt` —
use `XNewRef` when the value may be `NULL`:
```c
return PyRegion_NewRef(self->value);   // simple getter
```

`NewRef`/`XNewRef` are **not restricted to returns.** Since local barriers never
fail, they are safe wherever you need a new **stack/local** strong reference — a
return, a value pinned on the stack across a call, or an element stored into a
local container you built. They fold "add local ref + incref" into one call.

Two limits still apply:
- **They create a local borrow, not an owning reference.** Never use `NewRef` to
  populate a field of a heap object (`heapobj->field = PyRegion_NewRef(x)` is
  wrong) — storing into a heap object requires an owning barrier (`AddRef` /
  `TakeRef` / `XSETNEWREF`), which `NewRef` does not perform.
- **Never pair `NewRef` with `TakeRef`.** Creating a local ref only to transfer
  it into a heap object is the round-trip anti-pattern under the classification
  table; go straight to `AddRef` / `TakeRef`.

Which to use, in practice:
- `PyRegion_NewRef` / `PyRegion_XNewRef` — any case needing a new **stack/local**
  strong reference: returning to the caller, pinning a value on the stack, or
  storing into a local container you built (including `PyList_SET_ITEM(list, i,
  PyRegion_NewRef(x))`, which folds the borrow and the incref into one call).
- `PyRegion_AddLocalRef` + `Py_INCREF`/`Py_NewRef` — the explicit two-step form,
  equivalent to `NewRef`; use it when you want the borrow and the incref visibly
  separated, or when a macro does its own incref.
- **Storing into a heap object** is not a local-borrow case — use the owning
  barriers (`AddRef` / `TakeRef` / `XSETNEWREF`), never `NewRef`.

**`PyRegion_AddLocalRef(tgt)`** — add a borrow (increment LRC) without creating
the strong reference for you; use when you `Py_INCREF` manually or via
`Py_NewRef` in a macro like `PyList_SET_ITEM`. Does not fail; NULL-safe (a
no-op on `NULL`).

**`PyRegion_AddLocalRefs(tgt1, tgt2, ...)`** — variadic convenience form for
several local borrows established as one logical step (e.g. a key/value pair).
Does not fail; NULL-safe. It is purely a grouping convenience over repeated
`AddLocalRef` calls — since individual borrows never fail there is no
partial-failure case to guard against — so use whichever reads more clearly.

**`PyRegion_RemoveLocalRef(tgt)`** — remove a borrow (decrement LRC). Call
*before* the matching `Py_DECREF` on a stack-held value. Does not fail.
Calling it on `NULL` or on an immortal/immutable value is safe — it is a
no-op in both cases, so the barrier is always well-defined even when `tgt`
might be `NULL` or might be an immortal singleton such as `Py_None`.

**`PyRegion_CLEARLOCAL(local)`** — clears a stack-held local reference (removes
the borrow and DECREFs). Prefer it over a manual `RemoveLocalRef` + `Py_DECREF`
pair where it fits. `CLEARLOCAL` sets `local` to NULL as part of its
implementation — **do not write `local = NULL` afterward**, it is redundant.

**Anti-pattern — prefer `CLEARLOCAL` over open-coded `RemoveLocalRef` + `Py_DECREF`.**
When the variable is not used after the cleanup, collapse the pair into one call:
```c
// VERBOSE (only use when you need the variable to remain non-NULL afterward):
PyRegion_RemoveLocalRef(it);
Py_DECREF(it);

// PREFERRED when the variable is done:
PyRegion_CLEARLOCAL(it);
```
The pair form is appropriate only when you need to keep `it` pointing at the
old object after the barrier (e.g. to read a field from it before it frees).
In practice that is rare; default to `CLEARLOCAL`.

### Local replacement helpers

For replacing a value held in a **stack-local** slot (as opposed to a heap
field owned by `src`), use the local replacement helpers rather than
open-coding `RemoveLocalRef` + `Py_XDECREF`:

**`PyRegion_XSETLOCALREF(dst, val)`** — replace local slot `dst` with `val`,
**stealing** `val` (it becomes the new local borrow), and removing the old
local borrow + `Py_XDECREF`ing the old value. Does not fail; NULL-safe.

**`PyRegion_XSETLOCALNEWREF(dst, val)`** — like the above but creates a **new**
local borrow to `val` instead of stealing it. Does not fail; NULL-safe.

### Skipping a barrier safely

**`PyRegion_NeedsReadBarrier(obj)`** — true if `obj` is in a region and would
need tracking (it is `!(IsLocal || IsImmutable)`). Use it in an `assert` to
document a deliberate skip — on a value you *received*, not one you created:
```c
assert(!PyRegion_NeedsReadBarrier(listrepr));  // strings are immutable
Py_DECREF(listrepr);
```
An assert documents the assumption *and* checks it in debug builds — strictly
better than a comment that can silently rot.

**Anti-pattern — never pair `assert(!NeedsReadBarrier)` with `CLEARLOCAL`.**
`CLEARLOCAL` performs a region barrier (it calls `RemoveLocalRef`). If
`!NeedsReadBarrier` is true, no barrier was established, so calling
`RemoveLocalRef` is wrong — it decrements LRC without a matching increment.
The assert and `CLEARLOCAL` are contradictory:
```c
// WRONG: assert says no barrier needed, but CLEARLOCAL performs one
assert(!PyRegion_NeedsReadBarrier(x));
PyRegion_CLEARLOCAL(x);

// RIGHT option A: established a borrow (AddLocalRef was called) — use CLEARLOCAL, no assert
PyRegion_CLEARLOCAL(x);

// RIGHT option B: no borrow established (local or immutable) — assert + plain Py_DECREF
assert(!PyRegion_NeedsReadBarrier(x));
Py_DECREF(x);
```

**Immutable values never need a barrier.** This covers:
- Singletons returned via `Py_RETURN_NONE`, `Py_RETURN_TRUE`,
  `Py_RETURN_FALSE`, and `Py_RETURN_NOTIMPLEMENTED` — `Py_None`, `Py_True`,
  `Py_False`, `Py_NotImplemented` are immutable, so no barrier is needed on
  these return paths and they should not be flagged for review.
- Immutable fields in `dealloc`. If a field is known immutable (e.g. an
  interned string), `PyRegion_RemoveRef` is not required before its
  `Py_DECREF`, because immutable objects cannot transfer regions. Skip the
  barrier and optionally assert it: `assert(!PyRegion_NeedsReadBarrier(field))`.

### Dispatch sites

**`PyRegion_NotifyTypeUse(type)`** — call before dispatching through a type
slot to code that may not be region-aware. It internally checks the type's
region-aware flag and, if needed, marks open regions dirty. You do not check
the flag yourself; a redundant call is harmless.

It only needs to guard the dispatch itself; surrounding setup code does not
need to be inside it. Place it immediately before the slot call:
```c
PyRegion_NotifyTypeUse(Py_TYPE(obj));
result = Py_TYPE(obj)->tp_repr(obj);   // the dispatch
```
`NotifyTypeUse` is required only at **direct slot calls** — places where your
code calls a function pointer from a type object (`tp_repr`, `tp_richcompare`,
`mp_ass_subscript`, `sq_ass_item`, `tp_iternext`, etc.) or a C function
pointer obtained from a type struct (`bf_getbuffer`, slot wrappers, etc.).

**High-level `PyObject_*` helpers do not need a `NotifyTypeUse` at the call
site.** Functions such as `PyObject_Repr`, `PyObject_RichCompareBool`,
`PyObject_Hash`, `PyObject_GetAttr`, `PyObject_SetAttr`, `PyObject_GetIter`,
`PyObject_CallOneArg`, and similar already live in `abstract.c`, which has
been migrated: each one inserts `NotifyTypeUse` internally before it dispatches
through the slot. Duplicating the call in the caller is harmless but
unnecessary; the authoritative location is inside the helper.

When the dispatch target is a type you just migrated (and thus region-aware),
the `NotifyTypeUse` call is a no-op and harmless. When the target is an
unknown or user-defined type, it is required. Default to adding it; omit only
when the target type's source contains `PyRegion_` calls and
`.tp_flags2 = Py_TPFLAGS2_REGION_AWARE` in its `PyTypeObject` or
`PyType_Spec`, verified by reading that source file. If the source is not
available to read, add `NotifyTypeUse`.

### Object recycling

**`PyRegion_RecycleObject(obj)`** — call when the runtime reuses an existing
object and wants it to behave as if newly allocated (reset to local-region
state). It appears on object-reuse paths, often alongside GC re-tracking. For
example, the tuple recycle helper:
```c
static inline void
_PyTuple_Recycle(PyObject *op)
{
    _PyTuple_RESET_HASH_CACHE(op);
    if (!_PyObject_GC_IS_TRACKED(op)) {
        _PyObject_GC_TRACK(op);
    }
    PyRegion_RecycleObject(op);
}
```
which is used where a result tuple is reused in place (e.g. `enumerate`'s
fast path) rather than freshly allocated.

---

## Staged references (flag for human review)

`region.h` exposes a staged-reference API: `PyRegion_StageRef(s)` /
`PyRegion_StageRefs(...)`, `PyRegion_CommitStagedRef(...)`, and
`PyRegion_ResetStagedRef(...)`.

These exist for the rare case where several owning references must be added as
one atomic step but cannot be expressed with a single `AddRefs` /
`AddRefsArray` call — for example when the references to add are discovered
across branching control flow, and all must be validated as legal before any
is committed (because owning references are not reversible). Staging lets you
provisionally record the intended references, then either commit them all or
reset (abort) them all.

Staged references are easy to get wrong and are needed only in a small number
of places. **Do not insert them yourself.** Flag the function for human review
if you encounter this shape:
- multiple branches (if/else chains) that conditionally discover references to
  add,
- all discovered references must be validated legal before *any* is committed,
- neither `AddRefs` nor `AddRefsArray` fits because the set of references is
  conditionally determined,
- commonly: a multi-field update where each field's value is conditionally
  computed and all fields must succeed together.

```c
// shape that needs staged refs — do NOT implement, flag for review:
if (cond1) { ref1 = ...; }
if (cond2) { ref2 = ...; }
// both must be validated before either is committed
```
When you see this and a single `AddRefs` / `AddRefsArray` call cannot express
it, flag for human review with a short note explaining why.

---

## Tests

A migration is only trustworthy with tests. The goal is not "write an LRC
test" but "choose tests that correspond to each barrier shape the migration
touched." A type can pass a single success example and still be wrong for
deletes, replacements, cached iterator results, copy paths, default-return
paths, or cleanup after dirtying.

Tests use the `Region` object and its inspection API. Assert on **relative**
counter changes — record before and after the operation under test, never
absolute values. Useful inspection points: `_lrc`, `_osc`, `is_open`,
`is_dirty`, and the helpers `owns(obj)`, `is_local(obj)`, `get_region(obj)`.

### Test coverage requirement
A migration is **not complete** until every operation slot in the type spec has
a corresponding test. Walk the spec the same way the migration did, and for each
function verify that at least one test exercises it. Gaps to watch for:

- **In-place operators** (`|=`, `&=`, `-=`, `^=`) — each returns `self` via
  `PyRegion_NewRef`; they must be tested separately from their non-in-place
  counterparts (`|`, `&`, `-`, `^`) because the barrier shape differs.
- **All iteration shapes**: iterator creation (LRC cost), `next()` (per-yield
  LRC cost), iterator exhaustion, and early abandonment (deleting the iterator
  before it is exhausted).
- **Copy and repr** — neutral operations; assert LRC returns to baseline.
- **Pop** — steal-and-return; assert LRC rises while the result is held and
  drops when the result is released.
- **Clear** — assert LRC drops by one per element removed.
- **Frozenset-specific paths**: the idempotent `frozenset(f)` shortcut and the
  `frozenset.copy()` shortcut each have a distinct `PyRegion_NewRef` site.
- **`tp_methods` entries are not covered by tests of their helper functions.**
  If `method_foo` calls internal helper `_set_foo_impl`, a test of
  `_set_foo_impl` does not cover `method_foo`. Each entry in `tp_methods`
  needs its own test that calls the method through the Python API (e.g.
  `r.s.intersection_update(...)`) so that the method's own barrier calls and
  return-value handling are exercised.

A test that is marked `@expectedFailure` because the underlying barrier is not
yet implemented is **not acceptable** as a substitute for a passing test. Fix
the barrier, or stop and flag for human review (see "When to stop and ask the
human").

### Tests must use region objects
Every test must place the object under test inside a `Region` before calling
the function being tested. A test that operates only on local (non-region)
objects cannot exercise the barrier code path — barriers on local objects are
no-ops — and does not satisfy the coverage requirement.

**Test element class**: Use plain Python objects (`class A: pass`) as set
elements. Plain objects have a default identity-based `__hash__` and
`__eq__`; no custom `__hash__` is needed or desirable. Do not add a custom
`__hash__` — it is unnecessary complexity and makes the test harder to read.
Do not use `type` objects (classes themselves) as elements: `type` objects are
immutable in this runtime and the region system treats them as scalars, so LRC
tracking is a no-op for them. Instantiate the class instead. Prime the class
for region membership in `setUp` with `freeze(A())` so the first live instance
can be assigned to a region without being auto-frozen.

```python
# WRONG: operates on a local set; barriers never fire
def test_set_add_wrong(self):
    s = set()
    s.add(42)

# RIGHT: places element in a region so AddRef/RemoveRef fire
def test_set_add_right(self):
    r = Region()
    elem = MyObj()
    base = r._lrc
    r.s = set()
    r.s.add(elem)
    self.assertTrue(r.owns(elem))
```

When claiming that an **existing** test covers a function, confirm the existing
test constructs a region object on the relevant path. Pre-migration tests
almost always use only local objects and do not cover the barrier paths; extend
them rather than citing them as sufficient.

### Map tests to the barriers the function used
For each migrated function, look at what it did and test the matching
invariant:

- **`AddRef` / `TakeRef` stores** — assert the destination region owns the
  inserted object afterward (`r.owns(x)`), or that the source region's LRC
  rises when the destination stays local.
- **Replacement / delete / clear paths** — record the target region's `_lrc`,
  perform the replace/delete/clear, and assert the LRC drops by the expected
  relative amount.
- **Getters / returned references** — assert `_lrc` rises while the returned
  local reference is *still alive*, and returns to baseline after it is
  cleared.
- **Iterator / view paths** — assert iterator/view creation cost and per-yield
  cost separately; they can differ, and some iterators cache their result.

### Three operation shapes to distinguish
Final LRC neutrality is not the only thing to check. Be explicit about which
shape an operation has:

- **Neutral** — LRC returns to baseline before the operation returns (e.g.
  `repr`). Example below.
- **Returning-borrow** — LRC stays elevated until the *returned* local
  reference dies. Record a baseline, call the accessor, assert LRC rose while
  the result is held, then clear the result and assert LRC returns to baseline.
- **Transfer** — a local object stops being local and becomes owned by the
  destination region (`assert r.owns(x)` and `assert not is_local(x)`).

### Success-path example (neutral operation)
Records the LRC during `__repr__` to confirm `str(set)` borrows correctly and
leaves the region's LRC unchanged afterward:
```python
from regions import Region

class RecordLrcInRepr:
    def __init__(self, r: Region):
        self.r = r
    def __repr__(self) -> str:
        self.repr_lrc = self.r._lrc
        return "RecordLrcInRepr"

def test_set_repr1(self):
    r = Region()
    x = RecordLrcInRepr(r)
    s = {x}

    base_lrc = r._lrc
    str(s)

    # neutral: LRC back to baseline after the operation completes
    self.assertEqual(r._lrc, base_lrc)
    # but it was elevated during repr, from the borrow on the self reference
    self.assertGreater(x.repr_lrc, r._lrc)
```

### Failure-path example (state unchanged after a failed operation)
Do not test only the happy path. A failed operation must leave ownership,
locality, parent links, dirty flags, and LRC/OSC unchanged or restored:
```python
def test_unchanged_region_after_failure(self):
    r1 = Region()
    r2 = Region()
    a = self.A()
    a.b = self.A()
    a.b.c = self.A()

    r1.c = a.b.c                       # move a.b.c into r1
    self.assertTrue(is_local(a))
    self.assertTrue(is_local(a.b))
    self.assertTrue(r1.owns(a.b.c))

    with self.assertRaises(RuntimeError) as e:  # moving a into r2 fails
        r2.a = a
    self.assertEqual(e.exception.source, a.b)
    self.assertEqual(e.exception.target, a.b.c)

    self.assertTrue(is_local(a))       # failed move changed nothing
    self.assertTrue(is_local(a.b))
```
Match the failure test to the barrier's cleanup risk: a failing `AddRef` /
`TakeRef` *before* a store should leave ownership unchanged; a failure *after*
temporary local refs were created should still return LRC to baseline after the
exception; a failure inside a critical section or repr/iterator helper should
leave the public operation retryable and not leave dirty/open state behind
(unless dirtying is the expected behavior).

### Isolation enforcement tests (cross-region rejection)

Every mutating operation that accepts an element from the caller — `append`,
`insert`, `setitem`, `extend`, `__iadd__`, and their equivalents on other
container types — must be tested for the case where the element is **already
owned by a sibling region**. A sibling region is one that is neither a parent
nor a child of the container's region; passing one of its elements to the
container's mutating method must raise `RuntimeError` and leave the container
completely unchanged.

There are three properties to assert after a cross-region rejection:

1. **Container state is unchanged** — length, membership, and internal structure
   are identical to before the call. For sequence types, `len()` must equal its
   pre-call value. For set/dict types, the element must not appear in the
   collection.
2. **Ownership is unchanged** — the rejected element is still owned by its
   original region; the container's region did not acquire it.
3. **LRC is neutral** — the container's region LRC is the same before and after
   the failed call, proving the barrier rolled back any temporary borrow it
   created.

```python
def _make_sibling_elem(self):
    """Return (r2, elem) where elem is owned by r2."""
    r2 = Region()
    elem = A()
    r2.elem = elem
    return r2, elem

def test_append_rejects_sibling_region_element(self):
    r1 = Region()
    r1.lst = []
    r2, elem = self._make_sibling_elem()
    base = r1._lrc

    with self.assertRaises(RuntimeError):
        r1.lst.append(elem)

    self.assertEqual(len(r1.lst), 0)    # container unchanged
    self.assertTrue(r2.owns(elem))      # ownership unchanged
    self.assertEqual(r1._lrc, base)     # LRC neutral
```

Write one such test per mutating operation slot. The helper `_make_sibling_elem`
(or equivalent) keeps the setup concise and consistent across tests.

### Atomicity tests (batch operations with partial failure)

When a single call adds multiple elements — `extend([a, b, c])`, `update(...)`,
`__ior__(...)`, `AddRefsArray` — the operation is all-or-nothing. If the
barrier fails for *any* element, **none** of the elements must end up in the
container and all must remain in their original location. Test this explicitly
with a mixed list where a valid element appears on both sides of an invalid one:

```python
def test_extend_rollback_on_partial_failure(self):
    """[local, sibling-owned, local] — full rollback required."""
    r1 = Region()
    r1.lst = []
    before = A()
    r2, bad = self._make_sibling_elem()
    after = A()
    base = r1._lrc

    with self.assertRaises(RuntimeError):
        r1.lst.extend([before, bad, after])

    self.assertEqual(len(r1.lst), 0)
    self.assertTrue(is_local(before))   # not transferred — transfer rolled back
    self.assertTrue(r2.owns(bad))
    self.assertTrue(is_local(after))    # not transferred either
    self.assertEqual(r1._lrc, base)
```

The `[local, sibling, local]` layout is important: it distinguishes a true
all-or-nothing rollback from one that simply stopped at the first error and
left earlier elements transferred. Both `before` and `after` must still be
local after the failure.

### The resize-before-check bug (what tests expose and implementations must avoid)

Container operations often grow their internal storage *before* calling the
region barrier that validates the new elements. If the barrier fails, the
implementation must **undo the size change before returning**, otherwise the
container's `ob_size` (or equivalent length field) is left larger than the
number of initialized slots — deallocation will then call `Py_DECREF` on
garbage pointers and crash.

The canonical fix is to restore the pre-grow size in the barrier's error path:

```c
Py_ssize_t old_size = Py_SIZE(self);
if (list_resize(self, old_size + n) < 0)
    return -1;
// ... populate src array ...
if (PyRegion_AddRefsArray(self, n, src)) {
    Py_SET_SIZE(self, old_size);   // ← restore before returning
    return -1;
}
```

The isolation enforcement tests (container state unchanged, LRC neutral) will
catch this class of bug: if `len(container)` is wrong after a failed operation,
the implementation did not roll back the size. If the test crashes the
interpreter during teardown instead of producing a clean assertion failure, the
uninitialized slots are being DECREF'd — the same root cause, made visible at
dealloc time rather than at the assertion.

### Cover object shapes, not just one value
For container-like types, the barrier behavior differs by element kind. Cover
at least:
- mutable local elements (should transfer into a region),
- already region-owned elements (should become local borrows when returned),
- bridge/subregion references when the operation can nest or un-nest regions.

### Iterators and views
Iterators and views are easy places to get local refs wrong. Record the
baseline before creating the iterator/view, assert the creation-time LRC cost,
assert the per-yield LRC cost, and check what happens when the yielded value
and the iterator are released. Note some iterators intentionally do not reset
fully when the loop variable is cleared because they cache the result object
(e.g. dict item iteration caching its tuple) — assert the behavior the
implementation actually intends.

### Running tests
Use focused tests while migrating, then run the complete required suite before
marking the type done:
```bash
# required: region barriers + the type's own test module (e.g. test_set)
./python.exe -m test test_regions test_set

# a single region module while working on a type
./python.exe -m unittest Lib.test.test_regions.test_dict

# a single test while iterating
./python.exe -m unittest Lib.test.test_regions.test_core.TestInterRegionRelations.test_unchanged_region_after_failure
```

`test_regions` verifies that the new barriers are correct. The type's own test
module (e.g. `test_set` for `set`, `test_dict` for `dict`) verifies that
observable behavior is unchanged — barrier bugs can corrupt data structures in
ways that appear only there, not in `test_regions`. **Both must pass.** Running
only `test_regions` is not sufficient.

If tests fail, apply the same rule as in "The loop": check the migration
against the Definition of Done checklist first; fix the test only if the
migration passes every checklist item. A type is only done when the full
`test_regions` package passes with the type marked `Py_TPFLAGS2_REGION_AWARE`.

---

## Critical system constraints

These constraints govern what analysis is in scope and what runtime guarantees
hold. Do not infer, invent, or assume behavior beyond what is stated here or in
the accompanying Conceptual Reference.

1. **The GIL is enabled.** This runtime operates with the Global Interpreter
   Lock active for all Python threads.
2. **No-GIL (free-threaded) analysis is out of scope.** Do not reason about
   lock-free data structure access, atomic reference count operations, or
   thread-safety of barrier calls under concurrent execution. All migration
   decisions are made under the assumption that the GIL serializes Python
   threads.
3. **Per-object lock calls are no-ops in GIL builds.** Functions such as
   `Py_BEGIN_CRITICAL_SECTION` / `Py_END_CRITICAL_SECTION` exist in the
   codebase but compile to empty stubs when the GIL is enabled. Do not treat
   them as introducing real critical sections, and do not add or remove them
   as part of a migration.

