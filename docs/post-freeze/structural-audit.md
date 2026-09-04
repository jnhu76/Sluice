# Post-Freeze Structural Audit (R0)

**Status:** COMPLETE (2026-08-16)
**Scope authority:** post-freeze R0/R1 structural-hygiene task (post Phase G freeze)
**Baseline SHA:** `d9184de` (master, merge of PR #109, Phase G closeout)
**Companion documents:**
- `docs/architecture/foundation-freeze.md` — the freeze policy and frozen baseline
- `docs/post-freeze/zig-std-io-alignment.md` — Zig `std.Io` concept map and divergences
- `docs/architecture/zig-io-conformance-map.md` — the pre-freeze *semantic* conformance map
  (this audit adds the *structural* axis; it does not replace that map)

R0 changed no code. All LOC numbers are `wc -l` at `d9184de`.

---

## 1. Frozen baseline reference

Phase G is closed and the foundation is frozen by
`docs/architecture/foundation-freeze.md`. This audit treats the following as
**inviolable inputs** (frozen behavioral contract, not implementation shape):

| Frozen surface | Authority |
|---|---|
| Synchronous core contract (`Reader`/`Writer`/`Result<T>`/`IoError`, buffering, copy, WAL, durability) | `docs/reference/sync-io-model.md`, AC-1..AC-7 |
| Request identity & lifecycle (`RequestKey`, RequestArena/RequestSlot, generation) | `docs/adr/ADR-explicit-io-request-contract.md`, AC-2/AC-14 |
| Completion publication authority | `docs/adr/ADR-explicit-io-completion-authority.md`, AC-5/AC-13 |
| Backend wait/park/wake + interrupt bridge (R1–R4, `backend_wait_active_` gating) | `docs/history/implementation-plans/phase-g-backend-progress-wake.md`, `docs/history/closeout/phase-g-compliance-gate.md` |
| Scheduler wake/park protocol, split-wait bridge model | Phase G closeout, `spec/tla/e9_park_wake` |
| Cancellation layers (task/wait/op/syscall-interrupt/admission/drain/abort) | `AGENTS.md` §11, ADR-cancel-request-epoch |
| Deadline/timeout semantics (`deadline_t`, `advance_clock`, `*_until` waits) | E11 deadline design |
| Resource bounds (arena capacity, worker counts, queue depths as distinct resources) | AC-7, §12 |
| Shutdown/drain semantics (quiescent destruction, explicit lifecycle) | `AGENTS.md` §14 |
| Public async API (`AsyncIoContext`, `Completion<T>`, `ApplicationRuntime`, primitives) | `include/sluice/async/`, `docs/reference/api.md` |

Any R1 refactor must be a **code motion between translation units** that leaves
every row above untouched; verification is the full test matrix plus diff
inspection (§6 of the task: no condition/ordering/lock-scope/atomic-ordering
changes).

---

## 2. LOC inventory (src/, include/, tests/)

Buckets (production vs test separated; totals at `d9184de`):

### Production > 1500 LOC — the audit set

| File | LOC | Churn¹ | Decision |
|---|---:|---:|---|
| `src/async/scheduler.cpp` | 5864 | 59 | **SPLIT** (R1) — snapshot at `d9184de` |
| `include/sluice/async/scheduler.hpp` | 2768 | 56 | **DEFER** (candidate recorded, §5.2) — snapshot at `d9184de` |
| `src/async/uring_backend.cpp` | 1823 | 24 | **KEEP** (§5.3) — snapshot at `d9184de` |
| `src/async/select.cpp` | 1529 | 18 | **KEEP** (§5.4) — snapshot at `d9184de` |

¹ `git log --oneline --follow -- <file> | wc -l` — commit count over the file's
lifetime, dominated by Phases E–G.

### Production 1000–1500 LOC

| File | LOC | Churn | Decision |
|---|---:|---:|---|
| `include/sluice/async/detail/request_arena.hpp` | 1189 | 14 | **KEEP** (§5.5) — snapshot at `d9184de` |
| `src/async/threadpool_backend.cpp` | 1037 | 24 | **KEEP** (§5.6) — snapshot at `d9184de` |

### Production 750–1000 LOC (checked, no action)

`fake_backend.hpp` 811 (test/synthetic backend fixture), `application_runtime.cpp`
801 (single concept: runtime ownership/drain), `threadpool_backend.hpp` 790,
`uring_backend.hpp` 767. All single-concept; no mixed-responsibility signal.

### Tests > 1500 LOC (explicitly out of mechanical-split scope)

`event_primitive_test.cpp` 2680, `async_condition_primitive_test.cpp` 1963,
`threadpool_backend_scheme_b_race_test.cpp` 1942, `sluice_copy_pipeline_contract_test.cpp`
1828, `uring_backend_c2e_close_drain_test.cpp` 1667, `async_rwlock_test.cpp` 1629,
`threadpool_backend_c2e_close_drain_test.cpp` 1529. Per task §5.1 these are
test fixtures, not mechanical-split targets. No >1500 production file is a
generated/table file.

---

## 3. Audit dimensions (all six production files)

| Dimension | scheduler.cpp | scheduler.hpp | uring_backend.cpp | select.cpp | request_arena.hpp | threadpool_backend.cpp |
|---|---|---|---|---|---|---|
| Responsibility count | **~10** | ~6 | 1 | 1 | 1 | 1 |
| Cohesion | low (unrelated primitive families share a TU) | low | high | high | high | high |
| Coupling (depends on) | whole async layer | whole async layer | arena + context | scheduler + select ports | detail only | arena + context |
| Fan-in (files incl. it) | 76 (header) | 76 | 12 | 14 | 8 | 24 |
| Churn hotspot | **highest in repo** (59) | 56 | 24 | 24 | 14 | 24 |
| Application exposure | **HIGH** (new primitive → edit both) | **HIGH** | low (op-kind additions only — the intended seam) | medium (new select-case kind) | low | low |
| Backend leakage | none (inverse problem: *everything* lives here) | none | none (self-contained) | none | none | none |
| Policy/mechanism mix | mixed (run-loop mechanism + per-primitive policy) | mixed | clean | clean | clean | clean |
| State ownership | clear owner (Scheduler), wrong *file* | same | clear | clear (SelectGroup via ports) | clear (arena) | clear |
| Testability | primitives testable only through full Scheduler TU | same | good | good | good (death tests) | good |
| Zig structural alignment | **HIGH divergence** | HIGH | aligned (one backend = one file) | aligned | aligned (DIV-02) | aligned (DIV-03) |

---

## 4. The one structural finding

`src/async/scheduler.cpp` (5864 LOC) is the only high-risk mixed-responsibility
production file. It implements, as `Scheduler::` methods in one TU:

1. `SchedulerWakeHandle` (external control-wake capability) + its `Control`;
2. park/wake core (Phase G R1–R4): `park_on_wake_source`, `signal_wake_locked`,
   `wake_wait_one(_locked)`, `commit_suspend_locked`,
   `publish_waiting_fiber_runnable_locked`, interrupt bridge, park forensics;
3. WaitRecord pool (acquire/retire/live-count) and completion-await paths
   (`await_completion_*`, `await_wait*`, `cancel_waiter`, `cancel_wait`);
4. run loop / workers / work stealing / routing (`run_live`, `worker_loop`,
   `try_steal`, `route_runnable_locked`, `ensure_workers_locked`, `spawn*`);
5. fiber lifecycle (`init_fiber`, `owner_for_fiber_locked`);
6. timer/deadline machinery (binary heap `heap_*`, `pump_deadlines_locked`,
   `advance_clock`, earliest-deadline recompute, `expire_wait`);
7. mutex + condition + semaphore implementations (`mutex_*`, `condition_*`,
   `sem_*` — 17 methods);
8. rwlock implementation (`rwlock_*` — 13 methods + `RwWaitCtx`/`ForgedRwWaitCtx`);
9. event implementation (`event_*`, `await_event_wait*`);
10. queue admit/grant seams (`queue_push_admit`, `queue_grant_*`, …).

Nine-plus independent responsibilities in one TU; every one of the six
synchronization primitives has its own public header
(`async_mutex.hpp`, `condition.hpp`, `semaphore.hpp`, `async_rwlock.hpp`,
`event.hpp`, `async_queue.hpp`) but no implementation TU of its own — the
implementations all funnel into `scheduler.cpp`, and each must also grow state
and friend grants in the 2768-line `scheduler.hpp`.

This is exactly the funnel the task predicts: the *next* application-motivated
primitive (e.g. an `async_once`, a typed channel, a join barrier) cannot be
added without editing both god files. Contrast the already-established repo
pattern: select (`select.cpp`, `select_event.cpp`, `select_timer.cpp`) and
queue (`queue_port.cpp`) are separate TUs implementing `Scheduler` methods by
conceptual ownership. R1 generalizes that existing pattern to the remaining
concepts; it invents no new one.

---

## 5. Per-file decisions

### 5.1 `src/async/scheduler.cpp` — SPLIT

Action: relocate whole method blocks into new sibling TUs under `src/async/`
(established `queue_port.cpp`/`select_event.cpp` pattern; the build globs
`src/async/*.cpp`, so no build-manifest change). Class, header, ABI, lock
domains, atomics, and every function body stay byte-identical; only the
containing TU changes. Detailed in §6.

### 5.2 `include/sluice/async/scheduler.hpp` — DEFER

Rationale: it is a *public* header with 76 including files; the mixed content
(interface, `WorkerState`, `SchedulerWakeHandle`, all primitive state,
`AsyncTestAccess`, friend grants) is real but any restructure
(detail-header extraction of private state, pimpl, interface split) changes
class layout composition or include topology across the whole async layer.
That is disproportionate to zero observable benefit this round and risks the
freeze. Recorded as **post-freeze issue candidate PF-1** (§8), triggered only
if application work shows compile-time or seam pain. No LOC target justifies
touching it now. (Task §5.1: KEEP rationale required for >1500 files that stay
— this file stays intact *this round* by deferral, not by acquittal.)

### 5.3 `src/async/uring_backend.cpp` (1823) — KEEP

Rationale: single responsibility — the real/stub io_uring backend. The three
internal types (`BoundedDispatchQueue`, `TransportLedger`, backend proper) are
one mechanism: the queue is the bounded submission transport, the ledger is the
kernel-ownership accounting, both exist only for this backend. Splitting them
out would separate a coherent, heavily invariant-annotated implementation
(the file's comments document SQE/CQE ownership rules that span all three
types). Churn (24) is phase-migration churn, now complete (Phase D). Backend
op-kind additions (the only expected application-driven change) already enter
this file through the designed seam and *must* — a backend implements its ops.
One-file-per-backend is also the Zig shape (`Io/IoUring.zig`).

### 5.4 `src/async/select.cpp` (1529) — KEEP

Rationale: single concept — the Select two-phase claim/publication protocol.
The TU header documents its ownership boundaries explicitly; per-kind
finalizers already live in `select_event.cpp`/`select_timer.cpp`. 1529 LOC is
the inherent size of the winner-linearization protocol plus its suspended-node
resolution paths. New select-case *kinds* (the only growth axis) would enter
via a new sibling TU per kind — the established pattern — not by growth here.

### 5.5 `include/sluice/async/detail/request_arena.hpp` (1189) — KEEP

Rationale: single authority domain — the RequestSlot lifecycle, the
terminal-winner arbitration, and the ready-ring. It is the *most* invariant-
dense file in the repository (each block cites ADR decisions); any split would
separate the state machine from its arbitration rules. It is a `detail/`
header included by backends only (fan-in 8), not by applications.

### 5.6 `src/async/threadpool_backend.cpp` (1037) — KEEP

Rationale: single responsibility — bounded persistent blocking-I/O offload
backend (Phase E complete). Coherent worker-pool + dispatch-ring + reap
implementation; below the 1500 bar; matches the Zig one-backend-one-file shape
(`Io/Threaded.zig`, with the standing conceptual caveat DIV-03: Sluice's
ThreadPoolBackend is blocking-I/O offload, *not* Zig's Threaded execution
strategy — `Group` Threaded mode is that equivalent).

---

## 6. R1 plan (executed after this audit; see §7 for evidence)

Split `src/async/scheduler.cpp` into concept-owned TUs. Everything is a
**move**: identical code, identical class, identical linkage (all methods
remain `Scheduler::` out-of-line definitions; no anonymous-namespace helper is
shared across TUs except via an existing internal header, else the helper
moves with its sole user cluster).

Target layout (names follow the existing `scheduler_`-method vocabulary; no
new concepts invented):

```text
src/async/
    scheduler.cpp              # construction, run loop, workers, steal, spawn,
                               # routing, fiber lifecycle, clock base
    scheduler_park_wake.cpp    # SchedulerWakeHandle, park/wake/interrupt bridge,
                               # WaitRecord pool, completion awaits, forensics
    scheduler_timer.cpp        # deadline heap, pump, advance_clock, expire/reconcile
    scheduler_mutex.cpp        # mutex_* implementation
    scheduler_condition.cpp    # condition_* implementation
    scheduler_semaphore.cpp    # sem_* implementation
    scheduler_rwlock.cpp       # rwlock_* + RwWaitCtx/ForgedRwWaitCtx
    scheduler_event.cpp        # event_* + await_event_wait*
    scheduler_queue.cpp        # runnable queue + fiber routing (as-built
                               # extraction from the planned scheduler.cpp
                               # remainder, on conceptual-ownership grounds)
    scheduler_internal.hpp     # as-built non-installed carrier for the two
                               # cross-TU entities (g_worker inline TLS,
                               # SchedulerWakeHandle::Control) — see final
                               # report §2 proof boundary
    (select.cpp, select_event.cpp, select_timer.cpp, queue_port.cpp unchanged)
```

As-built delta from this plan (post-freeze review reconciliation): the two
entries above were added during execution; the "no header change" stop
condition below means no INSTALLED/public header change — the non-installed
`src/async/scheduler_internal.hpp` was required so that the moved TUs share
exactly one program-wide `g_worker`/`Control` definition. The audit-time
target layout omitted both; the final report §2 inventory table is the
as-built authority and is machine-checked by
`scripts/gates/mechanical-facts.py`.

Out of scope (Stop conditions respected): no scheduler redesign, no new
abstraction, no virtual interface, no header change, no public API change, no
lock/atomic/ordering change, no primitive behavior change.

### AGENTS.md §8 compliance (gate record)

This refactor touches "Scheduler wake/progress" *files*; the §8 gate fields
are satisfied by **no-change proof rather than redesign**:

- **AC rules:** AC-6 (wake obligation), AC-7 (bounds), AC-11 (tests prove
  semantics) apply; none is altered — code is byte-identical, only TU
  placement changes.
- **State machine:** unchanged (`spec/tla/e9_park_wake` still the model; no
  transition touched).
- **Lock/atomic authority:** unchanged — every lock acquisition stays inside
  the same function body; TU boundaries never span a lock scope.
- **Resource/wake/shutdown models:** unchanged (no capacity constant, signal,
  or predicate text is edited).
- **Zig conformance/divergence:** unchanged (DIV-01..DIV-13 untouched; the
  split *narrows* the structural divergence documented in
  `zig-std-io-alignment.md`).
- **Evidence:** full Clang Debug matrix before/after; `git diff` audited as
  pure motion (§7).

---

## 7. Evidence protocol

Baseline (before R1), Clang Debug, `d9184de`:

```text
xmake f -m debug --toolchain=clang -y
xmake build sluice_core        -> ok
xmake build sluice_async       -> ok
xmake build -g test            -> ok
xmake test -v                  -> 167/167 passed, 5.4s
```

After R1 the identical matrix is re-run, plus: `git diff` reviewed line-by-line
for pure motion; `git diff --check`; docs/manifest gates
(`scripts/gates/pre-push.sh` equivalents) per §16.6 where applicable.
Release/ASan/TSan re-runs per change-class judgment (§16.1: public-header
unchanged; §16.3: no concurrency-semantics change — recorded in the final
report).

---

## 8. Post-freeze issue candidates (only real ones)

| ID | Candidate | Trigger |
|---|---|---|
| PF-1 | `scheduler.hpp` internal-state extraction / interface-mechanism split | application work showing compile-time or new-primitive-seam pain (this round: DEFER) |
| PF-2 | process I/O (pipes) — Zig has `Io` process ops; Sluice `X` by scope | first application needing subprocess I/O |
| PF-3 | public `sleep_for`/`Timeout` convenience over the existing deadline core | application evidence of ergonomic gap (semantic map already classes the clock row F-with-narrowing) |

Not filed (non-issues): "file is long" for the five KEEP files (rationales
above); renaming anything (vocabulary already canonical — see alignment doc §4);
any scheduler/cancellation/backend redesign (frozen).
