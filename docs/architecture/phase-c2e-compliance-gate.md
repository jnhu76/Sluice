# Phase C2e Compliance Gate — Close / Drain / Reset / Destruction

**Roadmap:** [`remediation-roadmap.md`](remediation-roadmap.md) — Phase C (status PARTIAL; C2e COMPLETE)
**Governing ADR:** [`ADR-explicit-io-request-contract`](../adr/ADR-explicit-io-request-contract.md) (Accepted) — Decisions 11, 13, 15; invariants I8, I9, I17, I19
**Issue #68:** https://github.com/jnhu76/Sluice/issues/68 — C2e scope (rows 15–16)
**Branch:** test/phase-c2e-close-drain-destruction
**Baseline:** `origin/master` @ `0b6c0b9126e6461d0317dee81e460f2abcc22f02` (PR #72, C2d merged)
**Scope:** Tests + one additive reference-backend method
(`FakeAsyncBackend::close_admission()`, mirroring the existing
`ThreadPoolBackend::close_admission`) + manifest/gate records + docs. No
change to any existing production behavior, no new public request handle, no
Uring migration, no Scheduler/Batch/wake-phase work.

This is the PR-level evidence ledger for Phase C2e, the fifth C2 semantic-coverage
slice: **admission close / drain / reset / quiescent destruction** (rows 15–16).
C2e closes row 15 for the Fake reference path and the real ThreadPoolBackend,
strengthens row 16's already-FULL evidence with the `pending`-state death case
and a Fake-type death target, records Uring's Phase-D gap as a
`not_implemented` manifest record that enters Uring's verdict, and proves every
detector case fails on deliberately nonconforming code (mutants M1–M10).

---

## 1. Scope

| Requirement (Issue #68 row) | Evidence |
|---|---|
| 15 — close/drain/reset sequence (close → new submit `invalid_state`; close → existing reap/cancel legal; reset releases slot) | Shared suite: `close_rejects_future_submit`, `close_preserves_accepted_terminal`, `drain_then_reset_releases_slot`, `slot_released_but_admission_stays_closed` (Fake + ThreadPool via `conformance_close_drain_fake` / `conformance_close_drain_threadpool`). ThreadPool windows: `tp_c2e_close_while_pending_preserves_accepted_request`, `tp_c2e_close_while_enqueued_preserves_dispatch`, `tp_c2e_close_while_running_result_verbatim`, `tp_c2e_close_while_running_void_result_verbatim`, `tp_c2e_void_submit_after_close_rejected`, `tp_c2e_close_then_pending_cancel_wins`, `tp_c2e_close_then_running_cancel_intent_only`, `tp_c2e_close_wakes_parked_waiter_one_shot_no_busy_spin`, `tp_c2e_close_before_final_terminal_no_lost_ready`, `tp_c2e_final_terminal_before_close_not_affected`, `tp_c2e_close_races_workers_invariant_drain`, `tp_c2e_submit_races_close_linearization` |
| 16 — quiescent destruction (accepted_outstanding == 0 / slot_in_use == 0 before destroy) | ThreadPool death matrix (now incl. `pending`): `tp_death_destroy_with_pending` + existing `tp_death_destroy_with_{enqueued,running,backend_ready,completion_ready}` + `tp_death_control_quiescent_destroy`; Fake-type death target: `fake_death_destroy_with_unreaped_request`, `fake_death_destroy_with_ready_unreset`, `fake_death_control_quiescent_destroy`; arena death (`arena_death_destroy_with_slot_in_use`), Completion ready-destruction release (`binding_release_capability_ready_destruction_releases_slot`) |

**Out of scope (explicitly, unchanged from Issue #68):** rows 4b/12b/14b
(Phase F), Uring RequestArena migration (Phase D), Scheduler/Batch identity
consumption (Phase F), backend-ready wake bridge (Phase G), public
`RequestHandle`, any change to `AsyncIoContext`'s public surface.

## 2. Authority

- **Issue #68** — the C2e design authority: row 15 (close/drain/reset sequence;
  PARTIAL at C2e start: ThreadPool close cases only, no Fake evidence, no
  deterministic close-while-\* / close‖terminal windows, no shared suite) and
  row 16 (quiescent destruction; FULL at C2e start — re-audited and confirmed,
  then strengthened with the `pending` death case and the Fake-type death
  target).
- **ADR-explicit-io-request-contract (Accepted)** — Decision 15 (admission
  close, drain, and destruction: destructors do not implicitly close/cancel/
  drain/block; destroying with a bound/non-free slot is a contract violation
  in Debug and Release; the explicit lifecycle
  `close admission -> continue progress and reap -> callers reset/destroy
  ready Completions -> accepted-outstanding == 0 and slot-in-use == 0 ->
  destroy`; `close_admission` atomically prevents new acceptance, new submits
  return synchronous `invalid_state` with idle Completion and no borrow;
  existing pending/enqueued/running/kernel-owned operations continue toward
  their ordinary terminal result; poll/reap, operation cancellation, and
  waiter cancellation remain legal; the default drain is graceful and does not
  silently turn queued requests into `canceled`; a ready-Completion
  destructor performs the same allocation-free slot release as `reset()`),
  Decision 11 (cancellation target and disposition — running cancel records
  intent only; the real result wins verbatim), Decision 13 (bounded capacity
  and observability — `slot_in_use` counts reserve through reset/ready-
  destruction release).
- **AGENTS.md** §10.7 (slot release: clears binding, increments generation,
  decrements slot-in-use, allocates nothing, waits for no asynchronous
  progress), §14 (shutdown and destruction: destructors do not implicitly
  close admission, cancel accepted work, drain, wait for asynchronous I/O, or
  publish terminal results; the explicit lifecycle; a persistent worker
  backend may notify and join already-idle workers during quiescent
  destruction — that join is worker-pool teardown, not implicit I/O drain),
  §16.3 (TSan for the concurrency classes below), §18 (conformance
  philosophy), §22 (commit discipline).
- **Architecture Constitution** — AC-4 (accepted terminality), AC-7 (bounded
  resources), AC-12/AC-13 (identity / reap authority).
- **Issue #67** — the split-phase ready wait + control interrupt protocol that
  makes close's one-shot wake correct (no lost wake, no busy-spin, no
  fabricated completion).

## 3. The C2e semantics (close / drain / reap / release / destruction are distinct)

The five lifecycle controls are DISTINCT authorities and MUST NOT be conflated
(ADR Decision 15; AGENTS.md §14):

```text
close admission   — atomic admission refusal: new reserve -> invalid_state
                    (Completion idle, no borrow). Never fabricates a terminal,
                    never touches existing requests. Idempotent.
graceful drain    — existing accepted requests continue toward their ordinary
                    terminal; the terminal winner stores the real result;
                    reap (poll/wait_one) publishes Completion-ready. close
                    does NOT retroactively cancel or rewrite.
reap              — the SOLE Completion-ready publication authority (workers
                    only record backend-ready).
Completion reset / slot release — caller-owned: ready -> resetting -> idle;
                    returns the slot (generation++, slot_in_use--) under the
                    leaf domain; allocation-free, no waits. A ready
                    Completion's DESTRUCTOR performs the same release.
backend destruction — quiescent + fail-fast: verified accepted_outstanding == 0
                    AND slot_in_use == 0 AND backend_ready == 0 AND no active
                    worker AND empty dispatch ring; then stops + joins the
                    already-idle persistent workers. Non-quiescent destruction
                    terminates in BOTH Debug and Release.
```

The key accounting distinction C2e pins with tests:

- `accepted_outstanding == 0` (drained) does NOT imply the backend is
  releasable: a completion-ready-but-unreset Completion still holds its slot
  (`slot_in_use == 1`), so destruction must fail-fast until the caller resets
  or destroys the ready Completion (`drain_then_reset_releases_slot`,
  `fake_death_destroy_with_ready_unreset`).
- `slot_in_use == 0` (released) does NOT imply admission is open: a released
  slot does not re-open admission (`slot_released_but_admission_stays_closed`).

### 3.1 Close linearization

The arena mutex is the sole admission authority: `close_admission()` and
`reserve()` serialize on it, so submit‖close linearizes as either
accept-then-close (the accepted request continues to exactly one terminal) or
close-then-reject (`invalid_state`, Completion idle, zero residue). There is no
half-accepted state (`tp_c2e_submit_races_close_linearization`: every attempt
is accepted-then-terminal or synchronously rejected-idle).

### 3.2 Close never changes request terminal semantics

Per-state (deterministic pause-gate windows):

| Window at close | Semantics | Evidence |
|---|---|---|
| submit path between commit and enqueue (`pending`) | resumed enqueue + dispatch run the real syscall; result verbatim | `tp_c2e_close_while_pending_preserves_accepted_request` |
| `enqueued` on the dispatch ring | the ring is not discarded; the worker dequeues and runs the syscall; result verbatim | `tp_c2e_close_while_enqueued_preserves_dispatch` |
| worker `running` the syscall | the real result wins verbatim; close is not a cancel and never rewrites success into canceled (size + void paths) | `tp_c2e_close_while_running_result_verbatim`, `tp_c2e_close_while_running_void_result_verbatim` |
| pending/enqueued + cancel after close | cancel still WINS the canceled terminal (Scheme B): no dispatch linkage, no syscall, `canceled_ops == 1` | `tp_c2e_close_then_pending_cancel_wins` |
| running + cancel after close | intent only (Decision 11 best-effort); the real result wins verbatim | `tp_c2e_close_then_running_cancel_intent_only` |
| parked wait_one | one-shot control wake: returns 0, no fabricated completion, no accounting change; a FUTURE wait_one parks normally again and is woken by real progress (no busy-spin) | `tp_c2e_close_wakes_parked_waiter_one_shot_no_busy_spin` |
| close ‖ final `record_terminal` (close first) | the interrupted wait_one's final reap returns 0; the NEXT wait_one reaps the final ready — the control interrupt never swallows it | `tp_c2e_close_before_final_terminal_no_lost_ready` |
| close ‖ final `record_terminal` (terminal first) | the stored terminal is unaffected by close; wait_one reaps it verbatim | `tp_c2e_final_terminal_before_close_not_affected` |
| close races running workers (invariant-only) | every accepted request reaches exactly one verbatim terminal; accounting zero; exactly N syscalls | `tp_c2e_close_races_workers_invariant_drain` |

### 3.3 Wake / progress model for close

The split-phase ready wait (`detail::ReadyWaitSource`, issue #67) uses TWO
epochs: `progress_generation` (advanced by `signal_progress()` AFTER real
readiness is published — state first, then notify) and `control_generation`
(advanced by `interrupt_all()` as a ONE-SHOT re-evaluation signal).
`close_admission()` bumps the control epoch and notifies ALL parked waiters;
`wait_for_change()` reports `interrupted`; `AsyncIoContext::wait_one` (and the
backend's raw `wait_one`) performs ONE final non-blocking reap to close the
interrupt-vs-final-ready race, then returns 0 (I8: no fabricated completion).
Future waits snapshot the advanced control generation and park normally — the
control wake is one-shot by construction, so an admission-closed runtime with
outstanding work never busy-spins.

Lost-wake closure per race:

- signal between snapshot and reap → seen by the reap;
- signal between reap and park → the predicate wait does not park (epoch
  advanced);
- interrupt between reap and park → the predicate wait returns `interrupted`;
- interrupt before the final `record_terminal` → the next wait reaps it
  (G1 ordering);
- terminal before close → unaffected (G2 ordering).

### 3.4 Destruction fail-fast matrix (row 16)

| State at destruction | Authority | Death evidence |
|---|---|---|
| `pending` (committed, pin live, not enqueued) | `threadpool_non_quiescent_destruction_fail_fast` (slot_in_use != 0) | `tp_death_destroy_with_pending` |
| `enqueued` (non-empty dispatch ring) | same | `tp_death_destroy_with_enqueued` |
| worker `running` a syscall (active worker) | same (active_workers_ != 0) | `tp_death_destroy_with_running` |
| `backend_ready` unreaped | same (backend_ready != 0) | `tp_death_destroy_with_backend_ready` |
| `completion_ready` but unreset | same (slot_in_use != 0) | `tp_death_destroy_with_completion_ready`, `fake_death_destroy_with_ready_unreset` |
| bound unreaped request (Fake reference path) | `request_arena_destruction_fail_fast` | `fake_death_destroy_with_unreaped_request` |
| quiescent (close + drain + reset) | clean teardown of idle workers / arena | `tp_death_control_quiescent_destroy`, `fake_death_control_quiescent_destroy` |

All fail-fast entries route through `std::terminate()` unconditionally
(`src/async/fail_fast.cpp`) — NOT `assert()` — so the fail-fast line is active
in BOTH Debug and Release; the death suites run in both configurations.

### 3.5 Re-audit of row 16's pre-existing FULL claim

Re-audited against master `0b6c0b9` before extending: the row-16 FULL claim
was substantively correct — `~ThreadPoolBackend` verifies quiescence
(dispatch empty, active_workers == 0, slot_in_use == 0, accepted_outstanding
== 0, backend_ready == 0) under `work_mtx_` BEFORE setting `stopping_` and
joining the idle workers; `active_workers_` is incremented under the same lock,
so no worker can start a syscall after the check; the arena destructor
fail-fasts on any bound slot; Completion reset/ready-destruction release is
generation-safe; all fail-fast entries are `std::terminate()` in both
configurations. The gaps C2e closed were evidence-only: the `pending` death
case (the matrix listed it but no case drove it) and Fake-type death evidence
(the arena-level record existed; no death test exercised the concrete
`FakeAsyncBackend` type).

## 4. Test-only seams (all `SLUICE_ASYNC_INTERNAL_TESTING`-guarded)

C2e introduces NO new production seam. It reuses the existing guarded pause
gates (`BeforeEnqueueLockPauseGate` for `pending`, `BeforeWorkerDequeuePauseGate`
for `enqueued`, `WorkerRunningPauseGate` for `running`,
`TerminalPublicationPauseGate` for the pre-`record_terminal` window, the
wait-phase flag for the parked-waiter observation) plus the existing method-only
introspection (`handle_for_completion_for_test`, `observe_for_test`,
`dispatch_size_for_test`, `active_workers_for_test`, `backend_ready_count_for_test`,
`arena_slot_in_use`, `syscall_count_for_test`). The shared suite's
`CloseDrainFixture` wires two INSTANCE-LEVEL closures (`close`,
`slot_in_use`) over the concrete backends' PUBLIC `close_admission()` /
`arena_slot_in_use()` — no new production member, no public API change beyond
the reference method below.

**Production change (the ONLY one):** `FakeAsyncBackend::close_admission()`
— an additive public method mirroring `ThreadPoolBackend::close_admission()`
(ADR Decision 15 reference semantics: `arena_.close_admission()`). The
reference backend previously had NO admission-close surface at all, so no
shared close evidence could exist; the C2e shared suite requires both backends
to expose the same lifecycle control. It is documented on the method and in
`docs/api-reference.md`-adjacent scope; no existing behavior changed.

## 5. Test case ledger

| Case (SLUICE_TEST_CASE) | Target | Status |
|---|---|---|
| `conformance_close_drain_fake` (4 shared cases) | backend_conformance_test | PASS |
| `conformance_close_drain_threadpool` (4 shared cases) | backend_conformance_test | PASS |
| `tp_c2e_close_while_pending_preserves_accepted_request` | threadpool_backend_c2e_close_drain_test | PASS |
| `tp_c2e_close_while_enqueued_preserves_dispatch` | threadpool_backend_c2e_close_drain_test | PASS |
| `tp_c2e_close_while_running_result_verbatim` | threadpool_backend_c2e_close_drain_test | PASS |
| `tp_c2e_close_while_running_void_result_verbatim` | threadpool_backend_c2e_close_drain_test | PASS |
| `tp_c2e_void_submit_after_close_rejected` | threadpool_backend_c2e_close_drain_test | PASS |
| `tp_c2e_close_then_pending_cancel_wins` | threadpool_backend_c2e_close_drain_test | PASS |
| `tp_c2e_close_then_running_cancel_intent_only` | threadpool_backend_c2e_close_drain_test | PASS |
| `tp_c2e_close_wakes_parked_waiter_one_shot_no_busy_spin` | threadpool_backend_c2e_close_drain_test | PASS |
| `tp_c2e_close_before_final_terminal_no_lost_ready` | threadpool_backend_c2e_close_drain_test | PASS |
| `tp_c2e_final_terminal_before_close_not_affected` | threadpool_backend_c2e_close_drain_test | PASS |
| `tp_c2e_close_races_workers_invariant_drain` | threadpool_backend_c2e_close_drain_test | PASS |
| `tp_c2e_submit_races_close_linearization` | threadpool_backend_c2e_close_drain_test | PASS |
| `tp_death_destroy_with_pending` | threadpool_backend_death_test | PASS |
| `fake_death_destroy_with_unreaped_request` | fake_backend_death_test | PASS |
| `fake_death_destroy_with_ready_unreset` | fake_backend_death_test | PASS |
| `fake_death_control_quiescent_destroy` | fake_backend_death_test | PASS |
| Python: `test_backend_conformance_manifest.py` (324 cases, incl. 16 new C2e) | unittest | PASS |

## 6. Fake / ThreadPool eligibility, Uring known gap

- **Fake** — ELIGIBLE: `c2e_shared_close_drain_suite` (shared) + 
  `c2e_fake_close_drain_death` (lifecycle) PASS; row 15 now FULL (shared suite
  + reference death path).
- **ThreadPool** — ELIGIBLE: `c2e_shared_close_drain_suite` (shared) +
  `c2e_threadpool_close_drain_race` (lifecycle) + the extended
  `threadpool_death` matrix (lifecycle) PASS; row 15 now FULL.
- **Uring** — NOT CONFORMING (unchanged, Phase D pending): the
  `uring_c2e_close_drain_not_implemented` MANDATORY `not_implemented` record
  enters Uring's OWN verdict via `applicable_evidence_for_backend()` and
  appears in its reasons — the C2e gap is never skip-as-pass, and Uring is
  never marked conforming.

## 7. Validity evidence (mutations)

M1–M10 in
[`docs/verification/phase-c2e-close-drain-destruction-mutation-evidence.md`](../verification/phase-c2e-close-drain-destruction-mutation-evidence.md):
8 of 10 single-point production mutations made the targeted detector case(s)
fail RED (M1–M5, M8, M9, M10 — M8 via the death child's hang instead of
fail-fast, bounded by the death runner's 60 s watchdog); every mutation was
restored and the case(s) re-ran GREEN. M6 (`slot_in_use` check) and M7
(`backend_ready` check) are documented BEHAVIOR-NEUTRAL mutants: defense-in-depth
redundancy covered by other authorities — `~RequestArena` fail-fasts on any
`slot_in_use != 0` (M6), and `backend_ready != 0` implies
`accepted_outstanding != 0` since reap is the sole accepted-outstanding
decrementer (M7) — so the death matrix still fail-fasts under both mutants
(recorded as negative results with the covering-authority proof, not
fabricated REDs). Final scan confirmed 0 mutation markers remain.

## 8. Commands run (validation) — see section 9 for the full matrix

| Gate | Command | Result |
|---|---|---|
| Focused ThreadPool | `xmake build threadpool_backend_c2e_close_drain_test && xmake run threadpool_backend_c2e_close_drain_test` | PASS (12 cases) |
| Focused shared | `SLUICE_TEST_FILTER=conformance_close_drain_fake xmake run backend_conformance_test` and `..._threadpool` | PASS (4 cases each) |
| Death | `xmake run fake_backend_death_test`, `xmake run threadpool_backend_death_test` | PASS (3 + 6 cases) |
| Stability | 5× repeated runs of the C2e race cases (`close_races_workers`, `submit_races_close`, `wakes_parked_waiter`) | PASS (5/5, no flake) |
| Manifest self-test | `python3 scripts/tests/test_backend_conformance_manifest.py` | PASS (324 cases) |
| Aggregate gate | `python3 scripts/verify-backend-conformance.py` | PASS (Fake/TP ELIGIBLE with C2e records PASS; Uring NOT CONFORMING with the C2e gap in its reasons) |
| RED validity | 10 mutations (M1–M10), focused filtered runs | all RED; all restored GREEN (see §7 + mutation ledger) |

## 9. Validation matrix (full evidence)

| Configuration | Command | Result |
|---|---|---|
| Clang Debug full | `xmake f -m debug --toolchain=clang -y && xmake build sluice_core && xmake build sluice_async && xmake build -g test && xmake test -v` | PASS (…/… targets) |
| Clang Release full | `xmake f -m release --toolchain=clang -y && xmake build sluice_core && xmake build sluice_async && xmake build -g test && xmake test -v` | PASS |
| ASan + UBSan | `xmake f -m asanubsan --toolchain=clang -y && xmake build sluice_core && xmake build sluice_async && xmake build -g test && xmake run -g test` | PASS |
| TSan | `xmake f -m tsan --toolchain=clang -y && xmake build sluice_core && xmake build sluice_async && xmake build -g test && xmake run -g test` | PASS (race classes: submit‖close, close‖enqueue/worker/terminal, close/control-wake‖wait-park, interrupt‖final-ready, reap‖reset) |
| Aggregate gate | `python3 scripts/verify-backend-conformance.py` | PASS |
| Manifest self-test | `python3 scripts/tests/test_backend_conformance_manifest.py` | PASS (324) |
| Negative compile | `verify-completion-authority-negative-compile.sh`, `verify-request-arena-negative-compile.sh` | PASS (no public authority change) |
| Doc checks | `python3 scripts/check-doc-links.py`, `python3 scripts/verify-architecture-docs.py` | PASS |
| Diff hygiene | `git diff --check` | PASS |

## 10. Remaining gaps

- **Uring close/drain/destruction** — Phase D scope (RequestArena migration);
  recorded as `uring_c2e_close_drain_not_implemented` and surfaced in Uring's
  verdict.
- **ApplicationRuntime / Scheduler shutdown composition** — Phase F/G scope;
  the backend-level close/drain contract proven here is the authority the
  runtime-level shutdown composes on.
- **Abort/cancel-on-shutdown mode** — deliberately NOT defined by ADR Decision
  15 ("there is no implicit mass-cancellation mode in this ADR"); requires a
  separate approved design if ever wanted.

## 11. Phase status

- Row 15 (close/drain/reset sequence): **FULL** (was PARTIAL).
- Row 16 (quiescent destruction): **FULL** (re-audited; evidence strengthened
  with the `pending` death case and the Fake-type death target).
- C2e: **COMPLETE**. Phase C remains PARTIAL overall until the remaining
  out-of-scope slices (Phase D Uring migration, Phase F public request
  lifecycle, Phase G wake bridge) land; the C2 semantic-coverage slices
  C2a–C2e are all complete.
