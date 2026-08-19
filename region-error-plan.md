## Plan: restore region close diagnostics

### Current state

The tree-region rewrite moved the close algorithm into `try_close_region_tree()` and `_try_close_region()` in `Objects/tracingregionobject.c`. The good news is that the important accounting still exists locally:

- `region_trace_state_t.visited` still maps each moved object to its local refcount delta.
- `region_trace_state_t.external_rc` still carries the total outstanding incoming references for the region being traced.
- `region_trace_state_t.src` still identifies the source object during traversal, which is enough to rebuild graph edges.
- `_try_close_region()` still has the exact failure point where diagnostics should be produced, after dissolving the tentative GC list and after ignoring restart traces.

The regression is mostly that `_try_close_region()` now formats only:

```text
Failed to close region %p, there are %zd incoming references
```

and then destroys `state.visited`, so the object-level detail and Mermaid graph are lost. The old implementation had these pieces before the rewrite:

- `error_ref_filter` / `_filter_visited()` to select the first `ERROR_OBJECT_REPORT_COUNT` objects with positive incoming references.
- `build_close_error_message()` to emit:
  - `The region could not be closed due to:`
  - `- N incoming reference(s) to 'obj'`
  - `- N reference(s) to other objects`
- `mermaid_builder_t`, `mermaid_visit()`, and `dump_mermaid_diagram()` to write `region-graph.md` with red-highlighted leaking objects and cyan immutable objects.

### Desired behavior

When closing a region tree fails because a particular open region has lingering references into it, the exception should again identify the problematic objects instead of only reporting a total count. For small graphs, the failed close should also regenerate `region-graph.md` so the reference path can be inspected visually.

The diagnostics should be scoped to the region that actually failed during `try_close_region_tree()`, not to the whole tree unless a later failure aggregation is explicitly added. That preserves the current close algorithm: child regions are closed first; the parent is retried; whichever region still has external refs reports its own graph.

### Implementation steps

1. Reintroduce a diagnostic result type.

	Add a small struct near the trace state types, for example:

	```c
	typedef struct {
		 _Py_hashtable_t *obj_table;
		 Py_ssize_t incoming_refs;
	} close_error_info_t;
	```

	Keep it separate from `region_trace_state_t` so the tracing state can remain reusable and the caller owns the filtered error table lifetime.

2. Re-add the filtering helpers, adjusted for bridge semantics.

	Restore the old `error_ref_filter` idea, but make it explicit that the bridge object has one expected external owning reference. In the old code this was handled by subtracting one from the root region object; in the new code the bridge is `state.bridge`.

	Rules for `_filter_visited()`:

	- Start with the stored ref delta from `state.visited`.
	- If `key == state.bridge`, subtract the expected owning reference.
	- Keep only entries with `refs > 0`.
	- Cap the table at `ERROR_OBJECT_REPORT_COUNT` entries.
	- Treat `_Py_hashtable_foreach()` return `1` as intentional early stop, not an error.

	This preserves the old message shape while matching the current close model, where references to the bridge from inside the region are tracked separately as `bridge_rc` and should not be reported as external leaks.

3. Build diagnostics inside `_try_close_region()` before destroying `state`.

	In the `state.external_rc != 0` failure branch, after `gc_list_dissolve(&region->gc_list)` and after the `state.restart` check:

	- Allocate the filtered `close_error_info_t.obj_table` from `state.visited`.
	- Store `close_error_info_t.incoming_refs = state.external_rc`.
	- Build the Python exception with the restored `build_close_error_message()`.
	- Fall back to the existing summary string only if message construction fails without a more specific exception.
	- Destroy the filtered table on all exits.

	Important: do not build the nice error on restart traces. Restart traces are intentionally incomplete because freezing or open child-region discovery invalidated the current accounting.

4. Restore `build_close_error_message()`.

	Port the old `incoming_ref_report`, `_report_incoming_ref()`, and `build_close_error_message()` almost directly. The main adjustment is replacing `trace_info_t` with `close_error_info_t` and making the expected-reference subtraction happen during filtering, not during final summarization.

	The summary calculation should therefore be:

	```c
	Py_ssize_t problem_refs = error_info->incoming_refs;
	```

	not `incoming_refs - 1`, because the bridge's expected reference has already been removed from the filtered object counts and should also be excluded from `external_rc` if needed. If `external_rc` still includes the expected bridge reference for the region currently being closed, subtract it once at diagnostic collection time and document that invariant next to the code.

	Cheap check: the existing `test_release_error` expectations in `Lib/test/test_freeze/test_tracing_region.py` should pass with the old exact message lines.

5. Re-add Mermaid generation as a read-only diagnostic trace.

	Restore `mermaid_builder_t` and `mermaid_visit()`, but adapt it to `region_trace_state_t`:

	- Add `mermaid_builder_t *mermaid;` to `region_trace_state_t`, initialized to `NULL` in `region_trace_state_reset()`.
	- At the start of `_trace_visit()`, call `mermaid_visit(obj, state)` when `state->mermaid != NULL`.
	- In `mermaid_visit()`, keep the old node format: pointer, refcount, and type name.
	- Preserve the old special node shapes for ownership objects:
	  - Regions use Mermaid's subroutine shape: `id[[Region 0x...]]`.
	  - Cowns use Mermaid's stadium shape: `id([Cown 0x...])`.
	- Continue hiding immutable nodes behind `ERROR_MERMAID_HIDE_IMMUTABLE`.
	- Highlight objects present in the filtered error table with `:::error`.

	For the diagnostic trace, initialize `region_trace_state_t` with `gc_list == NULL` so no objects are moved. Use the same `tree_trace_state_t` shape only if required by `_trace_visit()` for region references; otherwise, split a read-only Mermaid visitor path from closing behavior so dumping the graph cannot enqueue or close subregions.

6. Decide how Mermaid handles sub-regions.

	The tree rewrite adds a case the old graph did not have: references to region bridge objects can represent nested ownership rather than ordinary objects.

	Recommended first version:

	- Show closed sub-region bridge objects as region-shaped boundary nodes and do not traverse into them, matching `_move_obj()`'s current `if (!Region_Check(obj))` behavior.
	- Treat open sub-regions as boundary nodes in the graph and label them as `[TracingRegion open]` or `[TracingRegion closed]` if that can be done without allocating risky strings.
	- Do not let Mermaid dumping trigger `_enqueue_region_for_closing()` or `region_trace_state_set_restart()`.
	- For now, dump only the graph for the single region that failed. Do not attempt to show the whole region tree yet.

	This keeps the diagnostic graph side-effect-free and aligned with the current failure point. A later enhancement can add dashed edges from parent to child region graphs if whole-tree visualization becomes useful.

7. Write `region-graph.md` only when the graph is small.

	Reuse the old limit:

	```c
	if (_Py_hashtable_len(state.visited) < ERROR_MERMAID_REPORT_LIMIT) {
		 dump_mermaid_diagram(region_obj, error_info.obj_table);
	}
	```

	Keep the graph dump strictly best-effort: failure to open `region-graph.md` must not replace the close error. Actual Python exceptions from building the diagram should either be cleared and ignored, or avoided by making the dump path best-effort all the way through. For diagnostics, losing the graph is less important than preserving the close failure message.

8. Add focused tests.

	Update or add tests in `Lib/test/test_freeze/test_tracing_region.py`:

	- Keep the existing simple leak test for exact message shape.
	- Add a capped-output test with more than `ERROR_OBJECT_REPORT_COUNT` leaked objects and an `other objects` summary.
	- Add a tree-region case where a child region fails to close and the error names an object inside the child, not just the parent total.
	- Add a tree-region case where the child closes successfully but the parent fails due to a reference into the parent.
	- For Mermaid, either assert that `region-graph.md` exists and contains `flowchart TD` plus `:::error`, or add a small C-visible/private Python hook if file-system assertions are too brittle.

	Also fix the duplicate Python test method name currently present in `TestTraceRefs`; the second `test_release_error` overrides the first.

9. Validation commands.

	After implementation, run the narrow test file first:

	```sh
	./python.exe -m test test_freeze.test_tracing_region
	```

	Then run a build if C changes were made:

	```sh
	make -j
	```

### Decisions for this pass

- `region-graph.md` should show only the single failing region for now.
- Graph dumping is strictly best-effort; diagnostic file failures should not mask the ownership violation.
- Structured exception attributes like `source` and `target` are a follow-up, not part of this restoration pass.
