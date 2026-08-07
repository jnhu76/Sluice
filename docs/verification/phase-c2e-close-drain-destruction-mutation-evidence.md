# Phase C2e Close / Drain / Destruction Mutation Evidence

This document records the RED-validity evidence (Issue #68 §"Mutation evidence")
for the Phase C2e test suite (rows 15–16: close / drain / reset / quiescent
destruction). Every C2e detector case must be proven able to FAIL on
deliberately nonconforming code — green-only tests are not proof.

Method chosen: **local uncommitted single-point mutation** (the
C2b/C2c/C2d-precedented alternative; a test-only nonconforming fixture would
require duplicating the backend internals). Each defect class was proven by a
temporary mutation — of the real production logic in
`src/async/threadpool_backend.cpp`, `include/sluice/async/detail/request_arena.hpp`,
or `include/sluice/async/detail/ready_wait_source.hpp` — a focused filtered test
run, and an immediate restore, applied by a local one-off harness
(`/tmp/c2e_mutate.py`, kept out of the tree, matching the C2b/C2c/C2d
precedent): snapshot → apply one exact replacement → build → run exactly the
detector case(s) → record RED → restore → re-run GREEN.

All commands ran on the test/phase-c2e-close-drain-destruction branch (master
base `0b6c0b9126e6461d0317dee81e460f2abcc22f02`). Toolchain: **Clang**, xmake,
`xmake f -m debug --toolchain=clang -y`.

## Method

For each mutant M1–M10:

1. apply ONE single-point mutation to the production logic (exact string
   replacement via the harness);
2. rebuild the affected test target (`xmake build <target>`);
3. run exactly the detector case
   (`SLUICE_TEST_FILTER=<case> xmake run <target>`);
4. record the expected case, the actual failing case, the command, and the
   exit code;
5. restore the file from a pre-mutation snapshot of the CURRENT working tree
   (never from git, so uncommitted work is never lost);
6. re-run the case GREEN on the restored tree;
7. after all mutants, confirm no mutation marker remains
   (`grep -r C2E-MUTANT src/ include/ tests/ scripts/` → no matches) and
   `git status --short` shows only the intended C2e changes.

## Mutation matrix

| Mutant | Deliberate defect (§13 class) | Mutation applied | Detector case(s) | Result |
|---|---|---|---|---|
| M1 | close no longer rejects new submit | `RequestArena::reserve()` drops the `admission_closed_` check | `close_rejects_future_submit` (Fake + ThreadPool drivers), `tp_c2e_void_submit_after_close_rejected` | RED → GREEN |
| M2 | close turns accepted request into canceled | `ThreadPoolBackend::close_admission()` mass-cancels every bound slot | `tp_c2e_close_while_pending_preserves_accepted_request` | RED → GREEN |
| M3 | close does not wake a parked waiter | `ThreadPoolBackend::close_admission()` drops `ready_wait_.interrupt_all()` | `tp_c2e_close_wakes_parked_waiter_one_shot_no_busy_spin` | RED → GREEN |
| M4 | wait_one interrupt path drops the final reap | `ThreadPoolBackend::wait_one()` drops the final reap in the interrupted branch | `tp_c2e_interrupt_final_reap_closes_ready_race` | RED → GREEN |
| M5 | close's control wake becomes sticky (wait never parks again / busy-spin) | `ReadyWaitSource::wait_for_change()` always returns `interrupted` (never parks) | `tp_c2e_close_wakes_parked_waiter_one_shot_no_busy_spin` | RED → GREEN |
| M6 | destructor drops the `slot_in_use` check | `~ThreadPoolBackend()` removes `q.slot_in_use != 0` from the quiescence condition | `tp_death_destroy_with_completion_ready` | **behavior-neutral** (see note) |
| M7 | destructor drops the `backend_ready` check | `~ThreadPoolBackend()` removes `q.backend_ready != 0` | `tp_death_destroy_with_backend_ready`, `tp_death_destroy_with_pending` | **behavior-neutral** (see note) |
| M8 | destructor silently drains accepted work instead of fail-fast | `~ThreadPoolBackend()` cancels every slot instead of fail-fast | `tp_death_destroy_with_enqueued`, `tp_death_destroy_with_pending` | RED → GREEN (enqueued case: the destructor hangs joining the paused worker instead of fail-fast; the death runner's 60 s watchdog kills the child and the case fails — not exit 86. The pending case stays GREEN: the mass-cancel wins the canceled terminal but the slot stays bound, so the ARENA destructor fail-fasts — the covering authority; see the M6 note) |
| M9 | reset does not release the slot | `RequestArena::free_slot_locked_()` stops decrementing `slot_in_use_` | `drain_then_reset_releases_slot` (Fake + ThreadPool drivers) | RED → GREEN |
| M10 | close_admission is a no-op (admission never closes) | `RequestArena::close_admission()` no longer sets `admission_closed_` | `close_rejects_future_submit` (Fake + ThreadPool drivers) | RED → GREEN |

### M6 / M7 note (documented negative results — defense-in-depth redundancy)

The mutation study proves the ThreadPool destructor's `slot_in_use` and
`backend_ready` quiescence checks are **defense-in-depth redundancy**, not
independent contract lines:

- **M6 (`slot_in_use`):** every `slot_in_use != 0` state also trips the ARENA
  destructor's own fail-fast (`~RequestArena` checks `slot_in_use_ != 0` and
  routes to `request_arena_destruction_fail_fast`; the arena is the last
  member destroyed, after the worker join). Removing the ThreadPool-level
  check therefore changes no observable behavior — the death matrix still
  exits 86, via the covering authority, in Debug AND Release. The check is
  kept because it fail-fasts BEFORE the worker-pool join (fail-fast site
  locality: a bound slot is detected before any teardown work), but the
  destruction contract does not depend on it.
- **M7 (`backend_ready`):** structurally, `backend_ready != 0` implies
  `accepted_outstanding != 0` — reap is the SOLE accepted-outstanding
  decrementer (AGENTS.md §10.6) and it transitions the slot OUT of
  `backend_ready` in the same critical section, so no reachable state has a
  backend-ready slot with `accepted_outstanding == 0`. Removing the check is
  therefore masked by the `accepted_outstanding` check — the death matrix
  still fail-fasts.

Both are recorded as behavior-neutral mutants with the covering-authority
proof rather than fabricated REDs: the C2e death matrix demonstrates the
contract (non-quiescent destruction fail-fasts in both configurations) under
the mutants, and the invariant analysis explains why the removed check cannot
be independently observed.

## RED exit-code note

`xmake run` reports 255 when the child fails. M8 fails via the death-test
child's hang → 60 s watchdog SIGKILL → the parent asserts not-exit-86 → RED.
M1/M2/M9/M10 fail on the case's own assertion text (the shared-suite CONF_CHECK
lines and the `tp_c2e_*` fail messages). M3/M5 fail on the bounded wait-phase
observation ("participant A never entered the backend ready wait" / "a FUTURE
wait_one after close must park normally"). M4 fails on the "wait_one must
return the reaped count (1), not 0" assertion.

## Summary

- 8 of 10 mutants were proven RED → restored → GREEN (M1–M5, M8, M9, M10).
- M6 and M7 are documented behavior-neutral mutants (defense-in-depth
  redundancy; covering-authority proof above).
- The harness force-rebuilds each target (`xmake build -r`) so the run binary
  always matches the applied mutation (an incremental-build hash skip in the
  first harness draft produced a false STILL_RED; fixed by forcing rebuilds).
- Final scan: `grep -r C2E-MUTANT` → no matches (0 marker residue).
- `git status --short` after the final restore shows only the intended C2e
  changes (no mutation residue).
