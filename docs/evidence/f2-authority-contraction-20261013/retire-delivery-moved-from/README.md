# F2 `AsyncIoContext::retire_delivery` moved-from dereference — resolution

Status: **CORRECTED** (residual finding, not an F2 blocker). Closes
`../known-preexisting-gaps.md` §2. Raw logs: `raw/red-null-deref.log`
(SIGSEGV at the unfixed head) and `raw/green-guarded.log` (8/8 pass).

## Classification

The unguarded `core_` dereference in `retire_delivery`
(`src/async/async_io_context.cpp:817`) is **not** merely a documented
invalid-call precondition:

- no documented precondition forbids calls on a moved-from context;
- the sibling entry points (`attach_observer`, `cancel_observer`,
  `outstanding()`) already define the class's moved-from contract as benign
  rejection — no dereference, not-found/zero result;
- a `Scheduler` holds a context reference (`src/async/scheduler.cpp:675`)
  that can reach `retire_delivery` after the context object was moved from.

`core_` is null exactly in the moved-from state (move construction and move
assignment exchange it; nothing else nulls it).

## Correction and oracle

The smallest correction — the same null guard the siblings carry, returning
`false` (nothing retired), mirroring `cancel_observer`'s not-found — with the
regression oracle in `tests/request_core_ownership_test.cpp`
(`move_transfers_the_same_core`): after the move, the moved-from context must
report nothing retired without a dereference.

- RED at the unfixed head: the oracle line faults deterministically
  (exit 139, null dereference of the moved-from `core_`).
- GREEN with the guard: 8/8 ownership scenarios pass.

No new mechanism or state: one guard on one public entry point.
