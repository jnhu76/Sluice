# E14 — Threaded vs Evented Semantic Parity and Runtime Closure Preparation

```text
TASK:    E14-THREADED-EVENTED-SEMANTIC-PARITY-PREPARATION-1
MODE:    AS-BUILT PRODUCTION-FIRST AUDIT + IMPLEMENTATION PREPARATION
STATUS:  ARCHIVED PREPARATION + IMPLEMENTATION CLOSEOUT (E14-IMPLEMENTATION-1)
         Implementation completed and merged via PR #29 (2026-07-26).
         Review follow-up merged (5661f19). This document is historical.
```

> **Revision banner (2026-07-26).** This is the **rev-3+1** preparation. It
> has been through four independent reviews: rev-1
> (`E14-PREPARATION-REVIEW: FAIL`, B1–B5 + one required correction), rev-2
> (`E14-PREPARATION-REV2-REVIEW: FAIL`, seven required corrections applied
> in rev-3), rev-3 (`E14-PREPARATION-REV3-REVIEW: FAIL` on artifact-
> integrity + Review-Request-staleness + T12 `g_worker` wording, applied in
> rev-3+1), and rev-3+1 (`E14-PREPARATION-REV3-REVIEW:
> PASS-WITH-OBSERVATIONS`, O1–O3 applied here). See the consolidated change
> log in §24 (Revision ledger). The original rev-1 blocking findings were:
>
> - **B1 — F2's destructor trace was materially incorrect.** The rev-1
>   document claimed the Evented destructor "releases `evented_fibers_` and
>   `evented_stacks_` without processing task Futures." Production does the
>   opposite: `~Group` (group.cpp:76-92) **unconditionally** swaps `tasks_`
>   and `futures_`, joins local threads, and `await()`s every local Future,
>   with **no `sched_` branch**. The actual reachable Evented failure is a
>   `g_worker == nullptr` invalid-context dereference inside
>   `Scheduler::await_ready_flag` (scheduler.cpp:1074-1076) before any
>   Fiber/stack release. T11, F2, RT-F2, and every dependent section were
>   rewritten (Revisions 1 and 2).
> - **B2 — D-E14-1 was framed as an unresolved current-contract question.**
>   The installed public header (`group.hpp:93-97`) and ADR §3 already fix the
>   accepted contract as logical await-all. F1-B (narrowing the contract) is
>   only available via an explicit superseding ADR. D-E14-1 is now a narrow
>   *mechanism* decision, not a *contract* decision (Revision 3).
> - **B3 — `attach_ready_wake` was misdescribed as sufficient F1 wiring.**
>   `attach_ready_wake` (scheduler.cpp:4102-4131) does not retain a wake
>   handle, store a callback, or bind the Future to later publication. F1-A's
>   required wake-publication topology (owner, lifetime, registration/
>   publication race, teardown race, exactly-once) is now specified
>   (Revision 4).
> - **B4 — F1's formal-impact classification was internally inconsistent.**
>   Rev-1 declared `EXTEND_EXISTING_MODEL` while also saying the model is
>   unchanged and the map extension is mechanical. Rev-2 then asserted an
>   unconditional `NO_FORMAL_CHANGE` and "no new state is required," which
>   over-claimed in the opposite direction because D-E14-1 (T-WAKE-1..8)
>   was still undecided. The classification is now
>   `PROVISIONAL_NO_FORMAL_CHANGE` keyed to the chosen mechanism: it stays
>   `NO_FORMAL_CHANGE` only if the mechanism introduces no distinct
>   per-Future/per-registration attachment state and refines entirely to
>   existing E9 transitions; it MUST be upgraded to `EXTEND_EXISTING_MODEL`
>   otherwise (Revision 5 / rev-3).
> - **B5 — F5 conflated missing macro spelling with missing capability
>   enforcement.** The ADR permits an *equivalent* gate; `fiber_ctx::supported`
>   is one. The real defect is that the capability is not enforced at the
>   Evented public admission boundary. F5 is now split into F5a / F5b / an
>   observation (Revision 6).
>
> The final readiness verdict changed from `READY-WITH-CONSTRAINTS` to
> `NOT-READY` (Revision 7): one confirmed finding (F2) had the wrong
>   production path, and F1's correction is not yet mechanically specified.
>   This document cannot authorize implementation until the remaining narrow
>   decisions (now §22) are resolved and an independent review of the
>   complete preparation artifact signs off (the rev-3+1 review returned
>   PASS-WITH-OBSERVATIONS; its O1–O3 fixes are applied in rev-3+1).

**Authority chain** (per `AGENTS.md` §2):
```text
1. this E14 task scope
2. AGENTS.md
3. accepted ADRs + closed/as-built subsystem docs:
   - docs/adr/ADR-execution-model.md            (E0 — execution model)
   - docs/adr/ADR-async-io-model.md             (016D — async I/O model)
   - docs/async-runtime-plan.md                 (E8–E14 roadmap)
   - docs/async-runtime-construction-method.md  (M1–M9 method lock)
   - docs/e10-e12-api-semantic-closure.md       (closed cross-primitive audit)
   - docs/history/closeout/e12-cross-primitive-terminal-audit.md (E12-G closed)
   - docs/design/e13-select-production-architecture.md (E13 master)
   - docs/design/e13-select-production-test-plan.md
   - docs/history/closeout/e13-select-p7-rollback-closeout.md
4. public headers under include/sluice/async/ + docs/api-reference.md
5. production implementation under src/async/ + include/sluice/async/
6. tests/ + scripts/verify-e* scripts
7. xmake.lua
8. .github/workflows/ci.yml
9. historical plans/comments/commit messages (supporting only)
```

A scanner report, stale comment, roadmap status, or commit message is
**evidence, not authority.** When documents and code disagree, this audit
records the disagreement as `DOC_DRIFT` rather than silently reinterpreting
production behavior to fit old prose.

---

## 1. Task status and exact audited HEAD

```text
BASELINE BRANCH:      master
BASELINE HEAD:        4ffff76 Merge pull request #28 from jnhu76/design/e12-g-terminal-audit
WORKING TREE:         clean at audit start (git status --short: no output)
OBSERVED BASELINE:    master after PR #28; E12 Queue, AsyncRwLock, E12-G audit
                      merged; E13 Select merged
PRODUCTION CHANGES:   FORBIDDEN by this task
TEST CHANGES:         FORBIDDEN by this task
FORMAL MODEL CHANGES: FORBIDDEN by this task
COMMIT / PUSH / PR:   FORBIDDEN by this task
```

This document writes only to `docs/history/implementation-plans/e14-threaded-evented-parity-preparation.md`,
`docs/history/reviews/E14-...-REVIEW-REQUEST.md`, and (only if supported by merged
as-built evidence) a minimal status/link correction to
`docs/history/implementation-plans/async-runtime-plan.md`. It does not modify `include/**`, `src/**`,
`tests/**`, `scripts/**`, `xmake.lua`, `.github/**`, `README.md`,
`docs/api-reference*.md`, or any formal model.

---

## 2. Scope and non-goals

### 2.1 In scope

- Freeze the actual observable Threaded and Evented public contracts as
  currently shipped.
- Classify every observable difference into the parity taxonomy (§5).
- Record every confirmed divergence with production `file:line` evidence and a
  reachable adversarial trace.
- Produce a phased, narrowly-scoped implementation plan for a LATER task.
- Decide formal-model impact per confirmed divergence (NO new large model).
- Update only documentation; defer production/test/formal changes.

### 2.2 Non-goals (forbidden scope expansion)

Per task §11, this preparation MUST NOT recommend or design:

```text
a generic Executor abstraction
P2300 / sender-receiver
C++ coroutine framework
actor runtime
generic Awaitable hierarchy
unified SynchronizationPrimitiveBase
generic Grant framework
new CancelToken design
Queue v1 external cancellation
new Select arm types
wait-all
networking expansion
high-level AsyncReader/AsyncWriter bridge
lock-free queue/deque optimization
NUMA / affinity / priority / RwLock upgrade-downgrade
```

### 2.3 Strategy split observed in production

The codebase contains TWO async substrates that this audit must keep distinct:

```text
EVENTED substrate (Scheduler-bound):
    Future<T> with EventedWaitPolicy
    Group(Scheduler&)
    AsyncIoContext + Completion<T> (the L1 op-execution layer)
    Scheduler + Fiber + Fiber lifecycle
    Event / Semaphore / AsyncMutex / AsyncCondition / AsyncQueue / AsyncRwLock
    Select
    All driven by Scheduler::global_mtx_ coordination

THREADED substrate (no Scheduler dependency):
    Future<T> with ThreadedWaitPolicy (the default)
    Group() (default ctor) — std::thread per task
    AsyncIoContext + AsyncBackend (Fake / ThreadPool / Uring) — std::mutex
    CancelToken / CancelState / check_cancel — strategy-neutral
```

Both substrates share the **logical Future/Group/CancelToken** types and the
**L1 Completion/AsyncIoContext** layer. They differ in the physical wait
mechanism (block-thread vs suspend-fiber).

---

## 3. Authority chain summary

Loaded in full before any source inspection (per task §4):

- `AGENTS.md`, `ADR-execution-model.md`, `ADR-async-io-model.md`,
  `async-runtime-plan.md`, `async-runtime-construction-method.md`,
  `api-reference.md`, `e10-e12-api-semantic-closure.md`,
  `e12-cross-primitive-terminal-audit.md`, `e13-select-production-architecture.md`,
  `e13-select-production-test-plan.md`, `e13-select-p7-rollback-closeout.md`.

The construction method (M1–M9) is NORMATIVE for E14 per
`async-runtime-construction-method.md`. E14 specifically must avoid repeating
the E9 lifetime-conflation defect (M2 dimension 4: invocation/lifetime).

---

## 4. Production source map

Authoritative `file:line` for the parity matrix. Headers under
`include/sluice/async/`; implementations under `src/async/`.

```text
L1 op-execution (strategy-neutral):
    include/sluice/async/completion.hpp            Completion<T>
    include/sluice/async/async_io_context.hpp      AsyncIoContext / AsyncBackend
    src/async/async_io_context.cpp                 public submit/poll/wait_one/cancel

Cooperative cancellation (strategy-neutral):
    include/sluice/async/cancel.hpp                CancelToken / CancelState
    src/async/cancel.cpp                           check_cancel

Future + wait policies:
    include/sluice/async/future.hpp                Future<T> + complete_with/await/cancel
    include/sluice/async/wait_policy.hpp           WaitPolicy / ThreadedWaitPolicy
    include/sluice/async/evented_wait_policy.hpp   EventedWaitPolicy (Evented seam)

Group:
    include/sluice/async/group.hpp                 Group (Threaded + Evented)
    src/async/group.cpp                            Group::await + ~Group

Scheduler + Fiber:
    include/sluice/async/scheduler.hpp             Scheduler (E7/E8/E9/E10/E11/E13 seams)
    src/async/scheduler.cpp                        run/run_impl/run_live/park_on_wake_source
    include/sluice/async/fiber.hpp                 Fiber state machine
    include/sluice/async/fiber_ctx.hpp             fiber_ctx::supported / context_switch
    src/async/fiber_ctx.cpp                        x86_64 asm + non-x86_64 stubs

E12 synchronization primitives (Evented only — Scheduler& bound):
    include/sluice/async/event.hpp                 Event (set/reset/wait/cancel)
    include/sluice/async/semaphore.hpp             Semaphore (acquire/release/cancel)
    include/sluice/async/async_mutex.hpp           AsyncMutex (lock/unlock/cancel)
    include/sluice/async/condition.hpp             AsyncCondition (wait/notify/cancel)
    include/sluice/async/async_queue.hpp           AsyncQueue<T> (push/pop/close)
    include/sluice/async/async_rwlock.hpp          AsyncRwLock (read/write)

E13 Select (Evented only):
    include/sluice/async/select.hpp                select() + SelectResult + cases
    include/sluice/async/select_fwd.hpp            kSelectMaxArms + SelectCaseType
    src/async/select.cpp                           select_admit / publish / rollback
    src/async/select_timer.cpp                     Select timer splice/pump/resolve
```

Search results for the prompt's symbol set are recorded as authority cites
throughout this document (e.g. `default_wait_policy` at `wait_policy.hpp:69`,
`SLUICE_HAS_EVENTED` referenced only in `ADR-execution-model.md:189` and absent
from source — see finding F5).

---

## 5. Meaning of "parity"

Frozen classification (task §6). Every observable difference falls into exactly
one bucket:

```text
PROVEN_PARITY
    Same public logical result and lifecycle, with sufficient evidence.

DOCUMENTED_PHYSICAL_DIFFERENCE
    Different physical mechanism is required and intentional:
    Threaded blocks/parks an OS thread;
    Evented suspends a Fiber and returns the worker to the Scheduler.

INTENTIONAL_SURFACE_ASYMMETRY
    A current public operation exists only in one strategy or has an explicitly
    approved strategy-specific contract. Documented and not accidental.

ACCIDENTAL_SEMANTIC_DIVERGENCE
    Same logical public operation produces a different result, completion,
    lifetime, failure, cancellation, cleanup, or repeated-call behavior without
    explicit authority.

DOC_DRIFT
    Documentation, comments, roadmap, API reference, or xmake commentary does
    not describe the as-built code.

TEST_GAP
    Contract exists and implementation appears consistent, but there is no
    sufficient deterministic evidence.

FORMAL_GAP
    A load-bearing protocol transition changed or exists without the formal
    evidence required by the project method.

DEFERRED_BY_DESIGN
    Explicitly excluded by an accepted current contract, not an accidental gap.
```

The cardinal rule (task §6): do NOT call a difference a bug merely because
Threaded uses threads and Evented uses Fibers; do NOT call a difference
intentional unless an accepted authority explicitly says so.

---

## 6. Threaded/Evented public operation matrix

Each row is classified using the §5 taxonomy. "n/a" means the operation has no
equivalent in the other substrate.

### 6.1 Future<T>

| Operation | Threaded | Evented | Classification |
|---|---|---|---|
| construction `Future()` / `Future(WaitPolicy&)` | default Threaded (`future.hpp:53`) | `Future(EventedWaitPolicy&)` (`future.hpp:54`) | PROVEN_PARITY |
| `complete_with(Result<T>)` | mtx+cv publish (`future.hpp:66-74`) | same body; ready_ set; NO Scheduler notify | DOCUMENTED_PHYSICAL_DIFFERENCE (mechanism) — but see F1 |
| `await()` blocks until ready | cv.wait (`wait_policy.hpp:62-64`) | `Scheduler::await_ready_flag` (`evented_wait_policy.hpp:56`) | DOCUMENTED_PHYSICAL_DIFFERENCE |
| repeated `await()` | cached result, no re-block (`future.hpp:91-96`) | cached result; `make_runnable` exactly-once (`scheduler.cpp wake_ready_flags_locked:950-966`) | PROVEN_PARITY |
| `cancel()` | token.request + await (`future.hpp:103-106`) | same | PROVEN_PARITY |
| repeated `cancel()` | idempotent (token + cached result) | same | PROVEN_PARITY |
| producer exception/error result | `Result<T>` channel only | same | PROVEN_PARITY |
| destruction before completion | safe (thread joined elsewhere or detached) | Fiber lifetime under Scheduler; see F2 | TEST_GAP (no Evented Future-destruct-before-completion test) |
| completion before wait | fast path `ready_` acquire (`future.hpp:92`) | level-triggered ready flag (`scheduler.cpp:950`) | PROVEN_PARITY |
| external-thread completion | `complete_with` thread-safe (`future.hpp:66-74`) | `complete_with` thread-safe BUT does NOT wake parked Scheduler (F1) | ACCIDENTAL_SEMANTIC_DIVERGENCE (F1) |
| concurrent completion vs wait | exactly-once via `ready_` (`future.hpp:69`) | exactly-once via `resolve_` CAS only for WaitQueue-backed waits; Future-backed waits have no resolve_ CAS | DOCUMENTED_PHYSICAL_DIFFERENCE |
| unsupported calling context | n/a (always OS thread) | calling EventedWaitPolicy outside Fiber is a contract violation (`evented_wait_policy.hpp:27-31`) | INTENTIONAL_SURFACE_ASYMMETRY |

### 6.2 Group

| Operation | Threaded | Evented | Classification |
|---|---|---|---|
| construction `Group()` | default (`group.hpp:59`) | `Group(Scheduler&)` (`group.hpp:65`, `group.cpp:14-19`) | DOCUMENTED_PHYSICAL_DIFFERENCE |
| async task admission | `std::thread` per task (`group.hpp:117-131`) | `init_fiber` + `spawn` (`group.hpp:165-206`) | DOCUMENTED_PHYSICAL_DIFFERENCE |
| task execution | OS thread | Scheduler Fiber (`group.hpp:165-206`) | DOCUMENTED_PHYSICAL_DIFFERENCE |
| `await()` waits for ALL tasks | joins threads, awaits each Future (`group.cpp:60-73`) | drives `run_until_idle()` loop (`group.cpp:21-57`) | ACCIDENTAL_SEMANTIC_DIVERGENCE (F1: returns with pending externally-produced Futures) |
| repeated `await()` | idempotent (tasks swapped out, size 0) | idempotent only after all Futures ready; size() reflects un-reaped Futures (F1) | ACCIDENTAL_SEMANTIC_DIVERGENCE (F1) |
| `cancel()` | token.request + await (`group.hpp:102-105`) | same | PROVEN_PARITY |
| task exception handling | swallow (try/catch in worker) (`group.hpp:120-126`) | swallow (try/catch in Fiber entry) (`group.hpp:186-193`) | PROVEN_PARITY |
| task cancellation boundary | cancel-propagation boundary (Io.zig:1240) | same | PROVEN_PARITY |
| task cleanup/reaping | swap+join (`group.cpp:60-73`) | reaped via `run_until_idle` progress | ACCIDENTAL_SEMANTIC_DIVERGENCE (F1: no-progress break leaves pending) |
| `size()` before await | `futures_.size()` under mtx (`group.hpp:109-112`) | same | PROVEN_PARITY |
| `size()` after await | 0 (futures swapped out) | reflects residual pending Futures (F1) | ACCIDENTAL_SEMANTIC_DIVERGENCE (F1) |
| destruction with live tasks | swap+join all threads (`group.cpp:76-89`); Future::await safe on caller thread (ThreadedWaitPolicy blocks on cv) | `~Group` has NO `sched_` branch: it unconditionally awaits every task Future (`group.cpp:89`); a pending Evented task Future enters `EventedWaitPolicy` -> `await_ready_flag`, which dereferences `g_worker` (== nullptr on the caller thread) — invalid-context UB before any Fiber/stack release (F2a) | ACCIDENTAL_SEMANTIC_DIVERGENCE (F2a — the as-built first failure; F2b is a separate residual-lifetime question) |
| Scheduler lifetime | n/a | borrowed Scheduler must outlive Group | DOCUMENTED_PHYSICAL_DIFFERENCE |
| fiber stack lifetime | n/a | unique_ptr per stack; address-stable across vector realloc (`group.hpp:175-177`) | DOCUMENTED_PHYSICAL_DIFFERENCE |
| `init_fiber` failure | n/a | return value silently discarded `(void)ok;` (`group.hpp:196-197`) | ACCIDENTAL_SEMANTIC_DIVERGENCE (F3) |
| external producer progress | n/a (no shared scheduler) | `run_until_idle` returns STALLED; caller must re-stage (F1) | ACCIDENTAL_SEMANTIC_DIVERGENCE (F1) |
| no-progress/STALLED behavior | n/a (always joins) | `after_pending == pending` break (`group.cpp:54`) | ACCIDENTAL_SEMANTIC_DIVERGENCE (F1) |

### 6.3 I/O (AsyncIoContext + Completion + backends)

| Operation | Threaded-equivalent | Evented | Classification |
|---|---|---|---|
| submission result (`submit_*`) | `Result<void>` synchronous (`async_io_context.hpp:116-128`) | same surface, called from Fiber | PROVEN_PARITY |
| Completion state machine | idle→outstanding→ready (`completion.hpp:21-23`) | same | PROVEN_PARITY |
| short I/O | bytes reported; derived helpers loop | same | PROVEN_PARITY |
| zero progress on non-empty write | invalid backend state (AGENTS §7) | same | PROVEN_PARITY |
| error propagation | Result<T>/IoError (no exceptions) | same | PROVEN_PARITY |
| cancellation | `cancel(Completion&)` best-effort (X3) | same | PROVEN_PARITY |
| completion before await | caller polls/reaps | Completion.ready flag observed by Scheduler | DOCUMENTED_PHYSICAL_DIFFERENCE |
| completion after suspension | n/a (caller blocks) | `wake_ready_completions_locked` (`scheduler.cpp:912-948`) | DOCUMENTED_PHYSICAL_DIFFERENCE |
| backend-specific mechanism vs logical result | Fake/ThreadPool/Uring | same; all serialized via `access_mtx_` (`async_io_context.cpp:4-6`) | DOCUMENTED_PHYSICAL_DIFFERENCE |
| ThreadPool backend | blocking-pool offload (P2, ADR §9.1) | same | PROVEN_PARITY |
| Fake backend | deterministic test vehicle | same | PROVEN_PARITY |
| io_uring stub vs real-path | gated by `SLUICE_HAS_LIBURING` | same | PROVEN_PARITY |

The L1 op-execution layer is uniformly strategy-neutral. Its consumers differ
(plain OS thread vs Scheduler-driven Fiber), but the public L1 surface and its
contracts are unchanged across strategies. PROVEN_PARITY for the L1 surface.

### 6.4 Runtime waiting (Scheduler)

| Operation | Drain (Threaded-equivalent compat) | Live | Classification |
|---|---|---|---|
| drain invocation lifetime | `run(N)` MW-S3 returns STALLED (`scheduler.cpp:455-459, 821-829`) | `run_live(N)` may park (E9-CORRECTIVE) | DOCUMENTED_PHYSICAL_DIFFERENCE |
| live invocation lifetime | n/a | MW-S3 + external wake parks (`scheduler.cpp:821-829`) | DOCUMENTED_PHYSICAL_DIFFERENCE |
| external wake | n/a (Drain) | `SchedulerWakeHandle::notify` (`scheduler.cpp:195-281`) | DOCUMENTED_PHYSICAL_DIFFERENCE |
| deadline | MW-S2 backend wait; MW-S3 STALLED | bounded park by earliest deadline (`scheduler.cpp:343-379`) | DOCUMENTED_PHYSICAL_DIFFERENCE |
| cancellation | `cancel_wait` (E10) | same | PROVEN_PARITY |
| exactly-once runnable publication | `make_runnable` CAS gate (`scheduler.cpp:925,939,957`) | same | PROVEN_PARITY |
| ownership after stealing | fiber_owner_ record (`scheduler.cpp:421,447`) | same | PROVEN_PARITY |
| shutdown/termination | `global_terminate_` wakes all (`scheduler.cpp:842-848`) | same | PROVEN_PARITY |
| repeated scheduler entry | re-arm sched_ctx per run (`scheduler.cpp:485-496`) | same | PROVEN_PARITY |

### 6.5 Synchronization primitives — Evented-only surface decision

Per task §7: for Evented-only synchronization APIs, do NOT invent a Threaded
equivalent. Each E12 primitive is **Evented-only by accepted design** (borrows
`Scheduler&` at construction):

| Primitive | Surface decision | Authority |
|---|---|---|
| Event | Evented-only | `event.hpp:80`; ADR §3 (logical wait contract is Fiber/Scheduler) |
| Semaphore | Evented-only | `semaphore.hpp:92` |
| AsyncMutex | Evented-only | `async_mutex.hpp:91` |
| AsyncCondition | Evented-only | `condition.hpp:111` |
| AsyncQueue<T> | Evented-only | `async_queue.hpp:229` |
| AsyncRwLock | Evented-only | `async_rwlock.hpp:67` |

These are NOT "unintentionally presented as a dual-strategy operation." Their
constructors take `Scheduler&` unconditionally; there is no Threaded-mode ctor.
The cross-primitive authority (`e10-e12-api-semantic-closure.md` D1–D10, CLOSED
2026-07-19) records the intentional layering. Classification:
**INTENTIONAL_SURFACE_ASYMMETRY** for the existence of each primitive.

The shared logical vocabulary they reuse (`WaitOutcome`, `WaitNode`, deadline
precedence rules) is also Evented-only; it is not surfaced in any Threaded
public type. No accidental dual-strategy presentation found.

For each primitive's per-operation contract, the cross-primitive audit
(`e10-e12-api-semantic-closure.md` §3) is CLOSED PASS; this E14 audit does not
re-litigate those contracts. E14 scope for these primitives is limited to:
(a) confirming they have no Threaded dual-strategy surface (confirmed:
INTENTIONAL_SURFACE_ASYMMETRY), and (b) confirming their unsupported-target
gating (F5 below).

### 6.6 Select — Evented-only surface decision

Select is Evented-only by accepted design (`select.hpp:197-207`,
`e13-select-production-architecture.md` §0.2). Classification:
**INTENTIONAL_SURFACE_ASYMMETRY** for its existence. Per-operation contracts:

| Select observable | Behavior | Authority |
|---|---|---|
| calling context | Fiber only; external-thread throws `std::logic_error` (`select.cpp:805-815`) | `select.hpp:197-207` |
| terminal result vocabulary | `SelectResult` (winner index + kind + timer outcome) (`select.hpp:53-96`) | `select.hpp` |
| deadline precedence | resource-first (lowest-index admission) (`select.cpp:1093-1098`) | E13 spec |
| cancellation availability | registration-failure rollback only (Building phase) | `e13-select-production-architecture.md` §6 |
| close semantics | n/a (no close on Select) | DEFERRED_BY_DESIGN |
| ownership | caller-frame SelectGroup + arms | `select.cpp:856,865` |
| destruction | all arm authority closed before caller resume | `select.cpp:783-786, 393-412` |
| external-thread operations | none (Fiber-only) | INTENTIONAL_SURFACE_ASYMMETRY |
| exactly-once publication | single `select_publish_locked` write | `select.cpp:340-412` |

PROVEN_PARITY is N/A: Select has no Threaded equivalent and is not presented as
dual-strategy. Its contracts are CLOSED per E13.

---

## 7. Four-dimensional state topology

Per task §8 and the project method M2. Each operation audited across:
(1) resource/readiness; (2) execution/ownership; (3) coordination/admission;
(4) invocation/lifetime.

### 7.1 Future::await (Threaded)

```text
resource:      result_ optional; ready_ atomic flag
execution:     caller OS thread blocks on mtx_/cv_
coordination:  cv.wait predicate on ready_
invocation:    caller thread; idempotent after first ready
```

### 7.2 Future::await (Evented)

```text
resource:      result_ optional; ready_ atomic flag (Future-owned)
execution:     calling Fiber suspends; worker returns to Scheduler
coordination:  Scheduler::await_ready_flag registers &ready_ in waiting_ready_
               (scheduler.cpp:1074+); level-triggered scan in
               wake_ready_flags_locked (scheduler.cpp:950-966)
invocation:    Fiber wait-epoch; node is &ready_ identity; one
               make_runnable CAS on resolution
```

The Future has NO WaitNode and NO resolve_ CAS. Resolution authority is the
Scheduler's `make_runnable` gate on the Fiber. This is a load-bearing
observation for F1: `Future::complete_with` only flips `ready_` and notifies
`cv_`; it does NOT call `signal_wake_locked` or any Scheduler wake handle.

### 7.3 Group::await (Threaded)

```text
resource:      pending vs terminal task Futures (each Future<void>)
execution:     std::thread per task; awaiter OS thread joins + awaits
coordination:  t.join() (synchronizes worker exit); Future::await (cv)
invocation:    Group::await call; idempotent after first call (vectors swapped)
```

### 7.4 Group::await (Evented) — THE LOAD-BEARING AUDIT

```text
resource:      pending vs terminal task Futures (each Future<void>)
               NOTE: task Futures use EventedWaitPolicy, so a Future
               suspended-inside-fn awaits a producer via the Scheduler.

execution:     Fibers on Scheduler; Fiber body wraps fn
               Group::await runs on the CALLER thread, NOT a Fiber
               Group::await calls sched_->run_until_idle() (=> run(1) Drain)

coordination:  sched_->run_until_idle() drives runnable Fibers
               per loop: count pending Futures; run; re-count
               BREAK if after_pending == pending (no progress)
               (group.cpp:21-57)

invocation:    Group::await call on caller thread
               run(1) is Drain mode (E9-CORRECTIVE)
               run_until_idle returns when Scheduler classifies MW-S3/QUIESCENT
               a pending externally-produced Future (no Scheduler wake source)
               leaves run() returning STALLED with the registration still live
```

The four-dimensional audit reveals the F1 root cause precisely:

```text
coordination mismatch:
    A task Fiber awaiting an external-thread Future registers &ready_ in
    waiting_ready_. The Scheduler classification is then MW-S3 (waiting
    registration exists, no backend op, no runnable Fiber). Drain mode returns
    STALLED. Group::await sees after_pending == pending and breaks.
invocation/lifetime mismatch:
    Group::await returns to the caller while task Futures remain pending. The
    Threaded contract is "await waits for ALL tasks" (group.hpp:93-97). This is
    the E9 lifetime-conflation defect re-appearing in the Group layer: a Drain
    run + an external-wake-capable wait without an effective wake source
    returns STALLED, and the Group treats STALLED as completion.
```

### 7.5 Select (inline vs suspended winner)

```text
resource:      per-arm readiness snapshot at admission
execution:     caller Fiber (select() runs inside a Fiber)
coordination:  single global_mtx_ CS for admit; one CandidateReady scan;
               lowest-index admission tie-break
invocation:    INLINE: phase=Completed in one CS, no make_runnable
               SUSPENDED: caller->make_waiting, raise suspend_switch_pending,
                          context_switch; on resume validate phase=Completed
                          + completion_mode_=suspended + winner; phase=Consumed
```

---

## 8. Blocking/wake/publication topology

### 8.1 Threaded blocking topology

```text
Future::await         -> ThreadedWaitPolicy::wait_until_ready
                       -> std::condition_variable::wait on ready_
Group::await          -> std::thread::join + Future::await
AsyncIoContext        -> backend.wait_one (cv-backed on ThreadPool)
```

### 8.2 Evented wake topology (post-E9-CORRECTIVE)

```text
Scheduler wake sources:
    W1/W2/W3  runnable publication (spawn / route_runnable / steal)
    W4/W5/W6  backend progress (Fake / ThreadPool / Uring poll or wait_one)
    W7        Scheduler-internal worker notification (inbox_cv)
    W8        external-ready publication via SchedulerWakeHandle::notify
    W9        shutdown/termination (global_terminate_)

Park domains (P3 model, ADR §9.4.3):
    BACKEND   exactly-one MW-S2 participant in ctx_.wait_one()
              (only when NO external-wake-capable wait is registered)
    SCHEDULER any other idle Worker on wake_cv_ + wake_epoch_
              (the MIXED-WAKE fix)

Wake epoch authority (ADR §9.4.5):
    observed_epoch recorded under wake_mtx_ at COMMIT
    predicate: wake_epoch != observed OR terminate OR runnable
    bounded timeout (2ms prod / 1ms test clock) is LOAD-BEARING in MIXED-WAKE
```

### 8.3 Future-backed Evented wait gap (F1)

```text
Future::complete_with (future.hpp:66-74):
    locks mtx_; sets result_; ready_.store(release); unlock; cv_.notify_all
    DOES NOT call SchedulerWakeHandle::notify
    DOES NOT call signal_wake_locked
    DOES NOT call attach_ready_wake

A Fiber that did:
    fut.await()        // EventedWaitPolicy -> await_ready_flag registers &ready_
is NOT woken by an external-thread complete_with unless that Fiber had ALSO
called Scheduler::attach_ready_wake(fut.ready_, wh) AND the producer called
wh.notify().

attach_ready_wake (scheduler.cpp:4102+) exists but is NOT invoked by Future or
EventedWaitPolicy. The seam exists; the wiring does not.

Consequence (F1): an Evented Future completed by an external thread while a
task Fiber is suspended on it inside an Evented Group has NO path to wake a
parked Scheduler Worker except the 2ms bounded poll (and only if the Worker is
parked on the SCHEDULER domain, which it is NOT in pure-Drain Group::await).
```

This is the precise operational shape of F1. See §16 for the formal finding.

---

## 9. Ownership and lifetime matrix

| Object | Owner | Address-stable | Lifetime closure |
|---|---|---|---|
| `Future<T>` (Threaded) | caller | yes (non-movable) | caller destroys after ready |
| `Future<T>` (Evented, task) | Group shared_ptr (`group.hpp:171`) | yes | Group destroys; task Fiber must have reached done |
| `Group` | caller | yes | caller; `~Group` joins threads (Threaded) / releases Fibers+stacks (Evented) |
| `Fiber` | Group unique_ptr (`group.hpp:152,180-181`) | yes (unique_ptr indirection) | vector destructor; F2b residual hazard if still Registered in `waiting_ready_` at `~Group` (separate from F2a, the as-built first failure) |
| stack | Group unique_ptr<byte[]> (`group.hpp:153,176-177`) | yes (unique_ptr base) | vector destructor |
| `Completion<T>` | caller | yes (L7) | caller; ready before destroy |
| `WaitNode` | caller | yes (deleted move) | terminal or Detached before destroy |
| `Scheduler` | caller | yes | caller; outlives all borrowed Groups/Fibers by contract |
| SelectGroup + SelectArmSlot | caller frame (`select.cpp:856,865`) | yes (fixed array) | all arm authority closed before frame return |
| SelectTimerRegistration | Scheduler pool (`select_timer.cpp:52`) | yes (list splice) | ACTIVE→terminal before SelectGroup destroy |

### 9.1 Lifetime hazards observed

- **F2a — Evented destructor invalid waiting context (the as-built first
  failure)**. `~Group` (group.cpp:76-92) has **no `sched_` branch**: it
  unconditionally swaps `tasks_` and `futures_`, joins any local threads, and
  calls `f->await()` on every local Future. When an Evented task Future is
  still pending (reachable whenever Group::await returned early due to F1, or
  whenever await() was never called), `Future::await` dispatches to
  `EventedWaitPolicy::wait_until_ready` -> `Scheduler::await_ready_flag`
  (scheduler.cpp:1074-1076), which dereferences `g_worker->current` without
  validating `g_worker`. The Scheduler has already cleared `g_worker` on the
  caller thread before `run_until_idle()` returned (scheduler.cpp:530-547),
  so this is an invalid-context null dereference / UB on the ordinary caller
  thread, BEFORE any Fiber/stack release. Rev-1 described the wrong failure
  (silent Fiber/stack release); the real as-built first failure is this
  invalid-context dereference.
- **F2b — residual `waiting_ready_` registration after Group destruction**.
  Only reachable after F2a is corrected or bypassed: if a task Fiber is still
  Registered in `waiting_ready_` when `~Group` reaches member destruction and
  frees `evented_fibers_`/`evented_stacks_` (group.cpp:90-91 comment), the
  residual registration is left with dangling pointers, and the actual
  dereference is deferred to a later wake pass (NOT the map's own destructor):
  (1) the local Evented task Futures are destroyed at the end of the `~Group`
  body, so the key `&Future::ready_` in `waiting_ready_` becomes dangling;
  (2) member destruction then frees `evented_fibers_`, making the
  `WaitReg.fiber` pointer dangling; (3) a later `wake_ready_flags_locked()`
  (scheduler.cpp:950-966) is the actual dereference site — it first reads
  `it->first->load(...)` (dangling `&ready_` key) and, if true, reads
  `it->second.fiber` (dangling Fiber) and `it->second.owner`. The
  `waiting_ready_` unordered_map's own destruction at `~Scheduler`
  (scheduler.cpp:108-190) merely destroys the stored raw POINTER VALUES; it
  does NOT dereference the key or the Fiber. Unlike Select timer/suspended-
  Select authority, `~Scheduler` does NOT assert that `waiting_ready_` is
  empty, so the residual registration is not caught at Scheduler teardown.
  This is a separate, residual lifetime question; it is NOT the as-built first
  failure.
- **T11/T12 not tested**: no test destroys an Evented Group with a pending
  Evented task Future (T11) or destroys the Scheduler before an Evented Group
  (T12). See §15.

---

## 10. Error/exception/fail-fast matrix

| Surface | Error mode | Authority |
|---|---|---|
| `Future::complete_with` second call | no-op (exactly-once) | `future.hpp:69` |
| `AsyncIoContext::submit_*` failure | `Result<void>` synchronous | `async_io_context.hpp:116-128` |
| `AsyncQueue` capacity 0 | `std::invalid_argument` thrown | `queue_port.cpp:108-113` |
| `select()` from plain OS thread | `std::logic_error` thrown | `select.cpp:805-808` |
| `select()` wrong-Scheduler case | `std::invalid_argument` thrown | `select.cpp:833-844` |
| `select()` deadline-heap reserve overflow | `std::length_error` thrown | `select.cpp:917` |
| Mutex `lock/try_lock` underlying failure | fail-fast (`std::terminate`) | `async_mutex.hpp:100` + D8 |
| Queue lease misuse / non-empty ring at dtor | `queue_lease_fail_fast()` terminate | `queue_port.cpp:33-35,130-132` |
| Select invariant violation | `select_invariant_fail_fast()` (release fail-fast) | `select.cpp` throughout |
| `Scheduler::init_fiber` returns false | silently discarded by Group | `group.hpp:196-197` (F3) |
| AsyncMutex destroyed while locked | debug assert (release UB) | `async_mutex.hpp:101-102` |
| Event destroyed with live SelectPort arms | debug assert + release fail-fast | `event.hpp:94-99` |

The classification matrix D8 (`e10-e12-api-semantic-closure.md`) is CLOSED and
not re-litigated here. E14-relevant additions are F3 (init_fiber) only.

---

## 11. Cancellation and deadline matrix

Per-primitive cancellation is CLOSED under E12-G
(`e12-cross-primitive-terminal-audit.md`). E14 additions:

| Operation | Cancellation model | E14 status |
|---|---|---|
| Threaded Future `cancel()` | cooperative token + await | PROVEN_PARITY |
| Evented Future `cancel()` | cooperative token + await (no Scheduler wake) | PROVEN_PARITY for the operation; F1 affects liveness not cancel semantics |
| Threaded Group `cancel()` | shared token + await | PROVEN_PARITY |
| Evented Group `cancel()` | shared token + await; F1 may still leave pending | ACCIDENTAL_SEMANTIC_DIVERGENCE (F1) |
| E12 primitive per-wait cancel | queue-identity-gated, exactly-once | PROVEN_PARITY (CLOSED E12-G) |
| Select cancellation | registration rollback only | DEFERRED_BY_DESIGN (E13 §6) |
| Deadline precedence (resource-first) | all primitives except AsyncCondition | PROVEN_PARITY (CLOSED) |

---

## 12. Destruction and repeated-call matrix

| Operation | Threaded | Evented | Classification |
|---|---|---|---|
| `Future::await` repeat | cached | cached | PROVEN_PARITY |
| `Future::cancel` repeat | idempotent | idempotent | PROVEN_PARITY |
| `Group::await` repeat | idempotent (vectors emptied) | idempotent ONLY if no F1 early-return; otherwise Futures remain (F1) | ACCIDENTAL_SEMANTIC_DIVERGENCE (F1) |
| `Group::cancel` repeat | idempotent | idempotent | PROVEN_PARITY |
| `~Group` with live tasks | joins threads; Future::await safe (ThreadedWaitPolicy cv) | `~Group` unconditionally awaits task Futures (`group.cpp:89`); a pending Evented task Future dereferences `g_worker` (== nullptr) in `await_ready_flag` — invalid-context UB (F2a); F2b is a residual registration-after-destruction question | ACCIDENTAL_SEMANTIC_DIVERGENCE (F2a; F2b conditional) |
| Primitive destruction with waiters | debug assert (caller violation) | same | PROVEN_PARITY (CLOSED E12-G) |

---

## 13. Unsupported-target capability audit (H5)

### 13.1 What the ADR requires

`ADR-execution-model.md:189-191` requires:

```text
Gating mechanism: a compile-time gate (SLUICE_HAS_EVENTED or equivalent)
that is OFF by default. Evented builds fail/disable cleanly on unsupported
targets (§7); they do NOT silently substitute one-thread-per-task.
```

And `ADR-execution-model.md:194-200`:

```text
On a target without Evented support, Evented must fail/disable cleanly —
not silently emulate using one OS thread per task ... The build either does
not compile the Evented sources or compiles them to an unavailable stub, and
the Threaded strategy remains fully functional.
```

### 13.2 What production actually does

| Gate aspect | ADR requirement | Production reality | Evidence |
|---|---|---|---|
| Compile-time macro | `SLUICE_HAS_EVENTED` off by default | macro does NOT EXIST anywhere in source | grep over include/src/tests/xmake returns 0 hits; only `ADR-execution-model.md:189` references it |
| Architecture gate | compile-time unavailable on non-x86_64 | `fiber_ctx::supported` constexpr (`fiber_ctx.hpp:69,71`); true on x86_64, false elsewhere | `fiber_ctx.hpp:68-71` |
| `context_switch` on unsupported arch | fail/disable cleanly | header stub returns `nullptr` (`fiber_ctx.hpp:140-144`); `.cpp` stub `std::abort()` (`fiber_ctx.cpp:311-314,316-319`) | `fiber_ctx.cpp:308-319` |
| `init_context` on unsupported arch | fail/disable cleanly | returns `false` (`fiber_ctx.cpp:316-319`) | same |
| `Group(Scheduler&)` ctor on unsupported arch | n/a (should not be callable) | compiles and runs unconditionally; builds `EventedWaitPolicy` | `group.cpp:14-19` |
| `Scheduler` ctor on unsupported arch | n/a | compiles and runs | `scheduler.cpp:98` |
| E12 primitive ctors on unsupported arch | n/a | compile and run | every primitive header |
| `Scheduler::init_fiber` failure propagation | should fail visibly | `Group::async_evented` discards: `bool ok = ...; (void)ok;` | `group.hpp:196-197` |
| Test skip on unsupported arch | should fail/disable | tests `if constexpr (!fiber_ctx::supported) return;` — silently PASS as no-op | every Evented test (see §15.1) |

### 13.3 Classification (H5) — rev-2 narrowed (Revision 6 / review B5)

Rev-1 classified "SLUICE_HAS_EVENTED does not exist" as an
ACCIDENTAL_SEMANTIC_DIVERGENCE by itself. That was too broad: the ADR's
"SLUICE_HAS_EVENTED **or equivalent**" wording (ADR-execution-model.md:189)
explicitly permits an equivalent mechanism, and `fiber_ctx::supported` is the
implemented equivalent SIGNAL. The real production defect is that this
signal is not ENFORCED as a gate at the Evented public admission boundary.
(`fiber_ctx::supported` is, at this writing, a capability PREDICATE — a
constexpr true/false — not yet an enforced admission/build gate; describing
it as "the gate" is only accurate once an admission or build mechanism
actually consults it.) The umbrella F5 is therefore split (see
§16 F5a/F5b/Observation):

```text
F5a — Evented public admission does not enforce the capability gate:
    ACCIDENTAL_SEMANTIC_DIVERGENCE (capability-contract violation).
    fiber_ctx::supported EXISTS as the equivalent capability predicate, but
    Group(Scheduler&) (group.cpp:14-19), Scheduler (scheduler.cpp:98), and
    every E12 primitive ctor construct unconditionally on unsupported
    targets. There is NO construction-time fail-fast and NO debug assert.
    The first observable failure is deferred to context_switch (nullptr
    return / abort).

fiber_ctx::supported + aborting stubs: DOCUMENTED_PHYSICAL_DIFFERENCE
    The architecture predicate and abort-on-call behavior satisfy the ADR's
    "do not silently emulate one-thread-per-task" rule AT THE FIBER LAYER.
    The defect in F5a is that this fiber-layer predicate is not lifted to
    the public admission boundary.

F5b — Unsupported-target tests return early and do not prove the
      fail/disable contract: TEST_GAP (T19).
    Every Evented test does `if constexpr (!fiber_ctx::supported) return;`
    and reports PASS on non-x86_64 having exercised nothing. A non-x86_64
    build of the test suite is a false green.

Observation — no macro named SLUICE_HAS_EVENTED exists:
    NOT a defect (the ADR permits an equivalent gate). Implementation-choice
    evidence. A future task MAY add the named macro as syntactic sugar over
    fiber_ctx::supported (optional, cosmetic); doing so does NOT fix F5a by
    itself — enforcement is the load-bearing part.
```

---

## 14. Adversarial traces

Each trace ends with: expected public observation, actual as-built observation,
classification, authority, test evidence. Per task §9.

### T1 — Future completes before Threaded await

```text
Setup:   producer thread calls fut.complete_with(result) before consumer.await()
Trace:   ready_.store(true, release); cv.notify_all; consumer enters await;
         ready_ acquire true; return *result_
Expected public observation: await returns the result without blocking.
Actual as-built observation: matches.
Classification: PROVEN_PARITY
Authority: future.hpp:91-96
Test evidence: tests/future_test.cpp future_inline_complete_then_await_returns_result:24
```

### T2 — Future completes before Evented await

```text
Setup:   Fiber A enters fut.await() (EventedWaitPolicy). Producer (in-scheduler
         Fiber B) calls fut.complete_with(result) before A's suspension reaches
         the registration recheck.
Trace:   A: ready_ acquire false -> await_ready_flag registers &ready_ in
         waiting_ready_ -> recheck under global_mtx_: ready_ true -> erase;
         make_runnable(A); route_runnable_locked(A) -> A never actually parks
Expected: A resumes with the result, no physical suspension.
Actual: matches.
Classification: PROVEN_PARITY
Authority: scheduler.cpp await_ready_flag:1074+; wake_ready_flags_locked:950-966
Test evidence: tests/evented_future_test.cpp F1:48
```

### T3 — external producer completes while Evented Fiber is suspended

```text
Setup:   Fiber A awaits fut (Evented, via Group(Scheduler&)). External OS
         thread E later calls fut.complete_with(result).
Trace:   A: registers &ready_ in waiting_ready_, suspends, worker returns to
         Scheduler. Scheduler classifies MW-S3 (wait registration, no backend op,
         no runnable Fiber). Drain mode (Group::await uses run(1) Drain):
         returns STALLED. E: complete_with flips ready_, notifies Future's cv_
         (which A is NOT waiting on — A is suspended via Scheduler). No path
         from E's publication to the Scheduler's wake_cv_/wake_epoch_.
Expected public observation: A eventually resumes with result (the logical
         await contract).
Actual as-built observation: A remains suspended indefinitely unless caller
         re-enters the Scheduler OR a 2ms SCHEDULER-domain park timeout fires
         (but Group::await does not park — it returns STALLED). Group::await
         sees after_pending == pending, breaks, returns to caller with A's
         Future still pending.
Classification: ACCIDENTAL_SEMANTIC_DIVERGENCE (F1)
Authority: future.hpp:66-74 (complete_with is Scheduler-unaware);
           evented_wait_policy.hpp:53-57 (no attach_ready_wake call);
           scheduler.cpp await_ready_flag registration only;
           group.cpp:21-57 (after_pending == pending break);
           ADR-execution-model.md §3 (logical wait contract)
Test evidence: NONE (T9 closest existing analog is
           external_wake_test.cpp:wake_external_producer_signal_only:620 which
           proves the property for await_ready_flag WITH an explicit
           SchedulerWakeHandle, NOT for Future::complete_with and NOT for
           Group::await)
```

### T4 — repeated await after terminal completion

```text
Setup:   fut is ready. consumer calls fut.await() twice.
Trace:   first await: ready_ acquire true, return result. second await: same.
Expected: both return the cached result; no re-suspend.
Actual: matches (Threaded and Evented).
Classification: PROVEN_PARITY
Authority: future.hpp:91-96; scheduler.cpp wake_ready_flags_locked registration erased after first wake
Test evidence: tests/future_test.cpp future_await_and_cancel_are_idempotent:55;
               tests/evented_future_test.cpp F5:101
```

### T5 — cancel loses to real completion

```text
Setup:   producer about to call complete_with(result). Consumer calls cancel().
Trace:   cancel(): token.request(); await(). producer: complete_with(result).
         await returns result (not canceled — producer observed the request
         late or not at all).
Expected: await returns whatever the producer published (best-effort cancel).
Actual: matches.
Classification: PROVEN_PARITY
Authority: future.hpp:103-106; ADR §7 X3
Test evidence: tests/future_test.cpp future_cancel_honored_by_cooperative_producer:78
```

### T6 — cancel wins before producer observes it

```text
Setup:   producer observes token at cancel points. Consumer cancels.
Trace:   token.request(); producer observes, publishes IoError::canceled.
         await returns canceled.
Expected: await returns IoError::canceled.
Actual: matches.
Classification: PROVEN_PARITY
Authority: future.hpp:103-106; cancel.cpp check_cancel
Test evidence: tests/future_test.cpp future_cancel_honored_by_cooperative_producer:78
```

### T7 — Group Threaded await with all tasks pending

```text
Setup:   Group g; g.async(fn1); g.async(fn2); neither task Future ready yet.
Trace:   g.await(): swap tasks/futures; join each thread (synchronizes worker
         exit, which happens after complete_with); await each Future (already
         ready).
Expected: g.await returns only after all tasks complete.
Actual: matches.
Classification: PROVEN_PARITY
Authority: group.cpp:60-73
Test evidence: tests/group_test.cpp group_await_waits_for_all_tasks:25
```

### T8 — Group Evented await with in-Scheduler producers

```text
Setup:   Group g{sched}; g.async(producer_fn) where producer_fn completes a
         Future awaited by another task in g. All producers in-scheduler.
Trace:   g.await(): count pending; run_until_idle() drives runnable Fibers;
         producer Fiber runs to completion inside run_until_idle; consumer
         Fiber's awaited Future becomes ready via in-scheduler complete_with;
         wake_ready_flags_locked observes the flag; consumer resumes; consumer
         completes; pending count drops to 0; loop breaks.
Expected: g.await returns after all in-scheduler producers complete.
Actual: matches.
Classification: PROVEN_PARITY (for the in-scheduler case)
Authority: group.cpp:21-57; scheduler.cpp wake_ready_flags_locked
Test evidence: tests/evented_group_test.cpp G1+G2+G3+G4:49; G5:103
```

### T9 — Group Evented await with an external producer (THE F1 TRACE)

```text
Setup:   Group g{sched}; g.async(fn) where fn awaits fut. fut is completed by
         an external OS thread E that does NOT call SchedulerWakeHandle::notify.
         Group::await is invoked on the caller thread (NOT a Fiber).
Trace:   fn's Fiber suspends on fut.ready_ via await_ready_flag. Scheduler
         classifies MW-S3 (no runnable, no backend op, registration exists).
         Group::await called run(1) (Drain). run_impl returns STALLED.
         Group::await loop: after_pending == pending (the Future is still not
         ready — E has not yet OR has already published, but in either case
         the Scheduler was not woken). Group::await breaks and returns to the
         caller. fn's Fiber is still Registered; its stack is still alive;
         g.size() still reports 1; the caller's code after await runs.
Expected public observation (group.hpp:93-97): "Wait until ALL tasks
         complete."
Actual as-built observation: Group::await returns with fn's task still
         pending (Fiber Registered, not done).
Classification: ACCIDENTAL_SEMANTIC_DIVERGENCE (F1)
Authority: group.hpp:93-97 (contract); group.cpp:21-57 (impl);
           scheduler.cpp:804-862 (Drain MW-S3 returns STALLED);
           future.hpp:66-74 (complete_with has no Scheduler wake);
           evented_wait_policy.hpp:53-57 (no attach_ready_wake wiring)
Test evidence: NONE. The closest existing test is
           tests/scheduler_progress_test.cpp progress_evented_group_task_awaits_real_backend:132
           which uses a ThreadPool-backend Completion (the Scheduler observes
           it via wait_one), NOT an external-thread Future. No test exercises
           external-thread Future completion during Evented Group::await.
```

### T10 — Evented Group run reaches Drain/MW-S3 STALLED while tasks remain pending

```text
Setup:   same as T9.
Trace:   see T9.
Expected: per ADR §9.4.0 Drain compatibility: Drain MW-S3 returns STALLED.
Actual: matches the Scheduler contract. The defect is in Group::await's
         treatment of STALLED as if it were completion (F1).
Classification: ACCIDENTAL_SEMANTIC_DIVERGENCE (F1, Group layer); the
         Scheduler behavior itself is DOCUMENTED_PHYSICAL_DIFFERENCE / PROVEN
         per E9-CORRECTIVE.
Authority: scheduler.cpp:821-829; group.cpp:54
Test evidence: tests/external_wake_test.cpp wake_t1_drain_mw_s3_returns_stalled:722
           (proves Scheduler behavior; does NOT exercise Group::await)
```

### T11 — Evented Group destruction while an Evented task Future is pending

```text
PRIMARY TRACE (the as-built first failure; rev-1 had this wrong):

Setup:   T9 has occurred: Group::await returned with one Evented task Future
         still pending (F1). The caller has NOT called await() again and now
         destroys g. The Scheduler is still alive. run_until_idle() has
         returned, so the Scheduler's single-worker run has already cleared
         g_worker before returning to the caller (see
         scheduler.cpp:530-547: g_worker = workers_[0].get() at run entry,
         g_worker = nullptr at run exit on the caller thread).

Trace:   ~Group (group.cpp:76-92) — there is NO sched_ branch:
           1. swap tasks_   -> local_tasks;       (group.cpp:78-83)
           2. swap futures_ -> local_futures;     (group.cpp:78-83)
           3. join each local task thread;        (group.cpp:88; none in
              Evented mode — tasks_ is empty, the loop is a no-op)
           4. for each local Future, call f->await();   (group.cpp:89)
              ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
              the pending Evented task Future lives here
           5. (group.cpp:90-91 comment) only AFTER all awaits return, allow
              evented_fibers_ and evented_stacks_ to destruct via member
              destruction at end of ~Group.
         The pending Future's await() (group.cpp:89) enters
         EventedWaitPolicy::wait_until_ready (evented_wait_policy.hpp:53-57)
         because ready_ is still false. That dispatches to
         Scheduler::await_ready_flag (scheduler.cpp:1074-1095):
              WorkerState* ws = g_worker;        // == nullptr here
              Fiber* me = ws->current;           // NULL-DEREF / UB
         (scheduler.cpp:1075-1076). The destructor therefore exhibits
         undefined behavior on the ordinary caller thread BEFORE it ever
         reaches the Fiber/stack vector release that rev-1 blamed.

EXPECTED PUBLIC OBSERVATION:
    The Threaded destructor's contract is "drain if await was never called"
    (group.cpp:77): it joins all task threads and awaits all task Futures
    safely, from a thread where ThreadedWaitPolicy::wait_until_ready can
    legitimately block on the Future's condition variable. There is no
    analogous safe destruction path for a pending Evented task Future,
    because EventedWaitPolicy's documented precondition
    (evented_wait_policy.hpp:27-31) requires a Scheduler-driven Fiber
    context, and a destructor runs on an ordinary caller thread.

ACTUAL AS-BUILT OBSERVATION:
    ~Group calls Evented Future::await on an ordinary caller thread after the
    Scheduler has cleared g_worker; the first operation of await_ready_flag is
    an invalid-context null dereference (UB). In a Debug build this is latent
    UB; in a hardened/ASan build it is reported as a null-pointer
    dereference. The rev-1 "use-after-free on Fiber/stack release" does not
    describe the as-built first failure.

CLASSIFICATION: ACCIDENTAL_SEMANTIC_DIVERGENCE (F2a — destructor waiting
                context)
AUTHORITY: group.cpp:76-92 (no sched_ branch; unconditional Future await);
           evented_wait_policy.hpp:27-31 (Fiber-context precondition);
           scheduler.cpp:1074-1076 (g_worker used without validation);
           scheduler.cpp:530-547 (g_worker cleared at single-worker run exit)
TEST EVIDENCE: NONE.

RESIDUAL LIFETIME TRACE (F2b — only reachable AFTER F2a is corrected or
bypassed; the subject of a separate ASan/lifetime test, not the primary
regression):

Setup:   Suppose F2a is corrected (e.g. the destructor no longer calls
         Evented Future::await from the caller thread) and a task Fiber is
         STILL Registered in the Scheduler's waiting_ready_ map because
         await() never drove it to terminal.
Trace:   The residual hazard is a chain of dangling pointers whose actual
         dereference is DEFERRED to a later wake pass, not performed by the
         unordered_map's own destructor:
           (1) the local Evented task Futures live in `local_futures` (swapped
               out of `futures_` at the top of ~Group, group.cpp:78-83). They
               are destroyed at the end of the ~Group body, so the
               `waiting_ready_` key `&Future::ready_` (the address captured at
               await_ready_flag registration, scheduler.cpp:1082) becomes
               DANGLING.
           (2) Member destruction then frees `evented_fibers_` and
               `evented_stacks_` (group.cpp:90-91 comment; the vectors are
               members). Now the `WaitReg.fiber` pointer in
               `waiting_ready_[&ready_]` also points at freed memory.
           (3) The actual dereference site is a LATER wake_ready_flags_locked()
               (scheduler.cpp:950-966), which reads `it->first->load(...)`
               (dangling `&ready_` key) and, when that reads true, reads
               `it->second.fiber` (dangling Fiber) and `it->second.owner`
               before calling make_runnable/route_runnable_locked.
         The `waiting_ready_` unordered_map's OWN destruction at ~Scheduler
         (scheduler.cpp:108-190) merely destroys the stored raw POINTER VALUES
         (`{Fiber*, WorkerState*}`) — it does NOT dereference the key or the
         Fiber. It is therefore NOT a dereference site by itself; the
         dereference hazard is at step (3). Note: ~Scheduler asserts
         waiting_select_count_ == 0 and active_deadline_count_ == 0, but does
         NOT assert that waiting_ready_ is empty — so generic ready-flag
         registrations are not caught at Scheduler teardown.
CLASSIFICATION (residual): ACCIDENTAL_SEMANTIC_DIVERGENCE (F2b — residual
                registration-after-destruction; deref deferred to a later
                wake_ready_flags_locked)
AUTHORITY: group.hpp:152-153 (Fiber/stack owned by Group vectors);
           scheduler.cpp:950-966 (wake_ready_flags_locked dereferences
           it->first then it->second.fiber);
           scheduler.cpp:1082 (registration keyed by &Future::ready_);
           scheduler.cpp:108-190 (~Scheduler destroys pointer values only;
           asserts Select/timer counts but NOT waiting_ready_.empty()).

F2a and F2b are SEPARATE questions (see §16 F2). F2a is the as-built first
failure and the subject of RT-F2. F2b is a residual-lifetime question to be
settled only after F2a's accepted teardown policy is chosen.
```

### T12 — Scheduler destroyed before Evented Group

```text
PRIMARY TRACE (rev-3 — corrected to the actual destructor path; rev-2's
description of "the Fiber registration in waiting_ready_ is destroyed with
the Scheduler's maps. Then ~Group runs" skipped the real first failure):

Setup:   Scheduler s constructed; Group g{s}; g.async(fn) where fn awaits an
         Evented Future on s. The task Fiber suspends via
         Scheduler::await_ready_flag and is Registered in s.waiting_ready_
         (key &Future::ready_, value {fiber, owner}). s is now destroyed
         before g (the group.hpp:64 "Scheduler must outlive the group"
         contract is violated). No Fiber/stack has been released yet.

Trace:   Step 1 — ~Scheduler (scheduler.cpp:108-190):
           - wake_control_ is invalidated (alive=false, scheduler=nullptr)
             and wake_control_.reset() drops the Scheduler's reference
             (scheduler.cpp:118-135).
           - debug asserts pass for the Select/timer invariants only:
             waiting_select_count_ == 0, active_deadline_count_ == 0, no
             ACTIVE SelectTimerRegistration (scheduler.cpp:154-189).
             IMPORTANT: ~Scheduler does NOT assert waiting_ready_.empty()
             (F2b-adjacent observation). The map values {Fiber*, WorkerState*}
             are raw pointers; their destruction at the end of ~Scheduler
             merely destroys pointer VALUES — it is NOT itself a dereference
             of fiber or of the &ready_ key.
         Step 2 — g member destruction begins. The body of ~Group
         (group.cpp:76-92) runs FIRST (before member destruction):
           - it has NO sched_ branch; it unconditionally swaps tasks_/
             futures_ and, on line 89, calls f->await() on every local
             Future. The pending Evented task Future's await() dispatches to
             EventedWaitPolicy::wait_until_ready (evented_wait_policy.hpp:
             53-57).
           - EventedWaitPolicy stores a BORROWED Scheduler& (scheduler_
             member, evented_wait_policy.hpp:53-58) captured at Group
             construction. That reference now denominates the DESTROYED
             Scheduler s. wait_until_ready dereferences it:
               scheduler_.await_ready_flag(ready)   // scheduler_ is dangling
             i.e. the FIRST reachable invalid operation is a use of the
             destroyed Scheduler (the member function call through a
             dangling Scheduler&). Two DISTINCT failure modes then chain
             inside await_ready_flag (scheduler.cpp:1074-1076); they must
             not be conflated:
               (i)  the call itself denominates destroyed Scheduler storage
                    (use-after-destroyed-object on `this`);
               (ii) SEPARATELY, the first statement of await_ready_flag reads
                    the file-scope `thread_local WorkerState* g_worker`
                    (scheduler.cpp:44 — g_worker is NOT a Scheduler member;
                    it is a translation-unit-local thread-local pointer set
                    only on a worker thread inside run()). On the ordinary
                    caller thread g_worker is null, so the next line
                    dereferences a null WorkerState*:
                      WorkerState* ws = g_worker;   // thread-local, == nullptr
                                              //   on the caller thread
                      Fiber* me = ws->current;      // null-deref / UB
             The first failure is the dangling-Scheduler member call (i);
             the second (ii) is the invalid Worker/Fiber context reached
             immediately afterward. Both occur inside await_ready_flag,
             reached from the ~Group BODY, BEFORE any Fiber/stack member of
             g is destroyed (the evented_fibers_/evented_stacks_ vectors are
             destroyed later during member destruction, group.cpp:90-91
             comment).

EXPECTED PUBLIC OBSERVATION:
    group.hpp:64 states "Scheduler must outlive the group." Destroying s
    before g is a documented caller-contract violation. The system should
    either fail cleanly at the violation boundary or document the required
    ordering such that no invalid operation is reachable.

ACTUAL AS-BUILT OBSERVATION:
    The first reachable invalid operation is NOT a use-after-free on
    Fiber/stack release. It is the call
    EventedWaitPolicy::wait_until_ready -> dangling Scheduler& ->
    Scheduler::await_ready_flag -> g_worker/current deref, reached from
    ~Group's unconditional Future::await on line 89 BEFORE member
    destruction. Under ASan/hardened this is reported as a
    use-after-destroyed-object / read of freed storage on the Scheduler;
    in plain Debug/Release it is latent UB.

CLASSIFICATION: TEST_GAP (no test) + ACCIDENTAL_SEMANTIC_DIVERGENCE (the
                caller-contract violation is reachable and produces UB on a
                documented-borrow contract; it is not fail-fast at the
                violation boundary)

AUTHORITY: group.hpp:64 (borrow contract "Scheduler must outlive the group");
           group.cpp:76-92 (~Group has no sched_ branch; unconditional
           Future::await on line 89);
           evented_wait_policy.hpp:53-58 (EventedWaitPolicy borrows
           Scheduler& and dereferences it in wait_until_ready);
           scheduler.cpp:44 (g_worker is a file-scope thread_local
           WorkerState*, NOT a Scheduler member — set only on a worker
           thread inside run());
           scheduler.cpp:1074-1076 (await_ready_flag reads g_worker then
           ws->current with no validation);
           scheduler.cpp:108-190 (~Scheduler invalidates wake_control_,
           asserts Select/timer invariants only — no waiting_ready_ check).

TEST EVIDENCE: NONE. No test destroys a Scheduler before an Evented Group
               while a task Fiber is Registered.

NOTE:   This trace is SEPARATE from F2b. F2b concerns a task Fiber still
        Registered in waiting_ready_ whose memory is later freed by the
        GROUP's member destruction (evented_fibers_/evented_stacks_), and a
        later wake_ready_flags_locked() dereferencing the dangling Fiber.
        T12 concerns the GROUP's destructor body first reaching a DANGLING
        SCHEDULER (via the borrowed EventedWaitPolicy reference) on the
        Future::await path. The two have different root objects (Group-owned
        Fiber vs Scheduler reference) and are recorded separately.
```

### T13 — init_fiber fails during Group::async

```text
Setup:   non-x86_64 target (or hypothetical init failure). g.async(fn) on
         Evented Group.
Trace:   async_evented: stack allocated; fiber_up allocated; entry set;
         bool ok = sched_->init_fiber(*fiber_raw, stack_base, kStackBytes);
         (void)ok;   <-- failure discarded
         push fiber_up, stack_up, fut; sched_->spawn(*fiber_raw);
         spawn: fiber.make_runnable() succeeds (Fiber state machine doesn't
         know init failed); fiber enqueued. Group::await drives Scheduler;
         Scheduler eventually context_switches into the uninitialized Fiber.
         Header stub context_switch returns nullptr (no switch); .cpp stub
         std::abort()s.
Expected: Group::async reports init failure (throw, Result, or debug assert)
         BEFORE spawning the Fiber.
Actual: failure silently discarded; spawn proceeds; eventual abort or
         no-op at first switch.
Classification: ACCIDENTAL_SEMANTIC_DIVERGENCE (F3)
Authority: group.hpp:196-197 (the (void)ok; comment "production code would
           propagate; E5 test asserts externally");
           fiber_ctx.hpp:163-167 (init_context returns false on unsupported)
Test evidence: NONE. No test exercises init_fiber's false return on the
           Group path.
```

### T14 — Group await followed by size()

```text
Setup:   Threaded: g.async(fn); g.await(); g.size().
Trace:   Threaded: await swaps futures out; size() returns 0.
         Evented (in-scheduler producers complete): await loop terminates
         with pending == 0; futures_ NOT swapped on Evented branch; size()
         returns the (now ready) count of futures.
Expected: size() reflects remaining tasks. After full await, 0.
Actual: Threaded 0; Evented (success path) returns the count of ready Futures
         still in the vector (NOT 0).
Classification: ACCIDENTAL_SEMANTIC_DIVERGENCE (F4)
Authority: group.cpp:21-57 (Evented branch does not clear futures_);
           group.cpp:60-73 (Threaded branch swaps futures_);
           group.hpp:109-112 (size returns futures_.size())
Test evidence: tests/evented_group_test.cpp G4 (g.size() == 1 at :94) —
           the Evented test ASSERTS the divergent behavior (size 1 after
           await), confirming F4 is observable.
```

### T15 — Group repeated await (Evented, post-F1)

```text
Setup:   T9 occurred: Group::await returned with one Future still pending
         (external producer). Caller calls g.await() again.
Trace:   Second await: count pending (1); run_until_idle (returns STALLED);
         after_pending == pending; break. Returns again with the Future
         pending. This continues until either the external producer is
         staged via a separate Scheduler invocation or the caller gives up.
Expected (group.hpp:93-97 "idempotent"): repeated await is a no-op once all
         tasks are terminal.
Actual: repeated await re-enters the Scheduler each time but cannot make
         progress on an external-wake-capable wait without an effective wake
         source.
Classification: ACCIDENTAL_SEMANTIC_DIVERGENCE (F1)
Authority: group.hpp:93-97 (idempotent contract); group.cpp:21-57
Test evidence: NONE.
```

### T16 — task throws in Threaded mode

```text
Setup:   g.async([](auto&){ throw std::runtime_error("x"); });
Trace:   worker thread: try { fn(*tok); } catch (...) { /* swallow */ };
         fut->complete_with(Result<void>{}); thread exits.
Expected: exception swallowed; Future completes void; await returns.
Actual: matches.
Classification: PROVEN_PARITY
Authority: group.hpp:120-126
Test evidence: tests/group_test.cpp group_task_exception_is_contained_and_await_drains:94
```

### T17 — task throws in Evented mode

```text
Setup:   same, Evented Group.
Trace:   Fiber entry: try { fn(*tok); } catch (...) { /* swallow */ };
         fut->complete_with(Result<void>{}).
Expected: same as Threaded.
Actual: matches.
Classification: PROVEN_PARITY
Authority: group.hpp:186-193
Test evidence: NONE directly (evented_group_test.cpp does not throw);
           inferred from code parity with T16. TEST_GAP for direct evidence.
```

### T18 — runnable task exists while another Evented task awaits I/O

```text
Setup:   Fiber A awaits a backend Completion (suspended). Fiber B is runnable.
Trace:   Scheduler classify: MW-S1 (runnable B exists); worker runs B; B
         completes; backend progress makes A's Completion ready; A resumes.
Expected: B progresses before A's pending op completes (E4 success criterion).
Actual: matches.
Classification: PROVEN_PARITY
Authority: scheduler.cpp worker_loop; ADR §9.1 (E4 success criterion)
Test evidence: tests/evented_scheduler_test.cpp sched_single_worker_scheduler_liveness:44 (E4-T1)
```

### T19 — unsupported architecture attempts to construct/use Evented mode

```text
Setup:   build on non-x86_64; construct Group(Scheduler&); g.async(fn); g.await().
Trace:   see T13. Either no-op switch or abort.
Expected: clean compile-time unavailability OR construction-time fail-fast.
Actual: silent construction; silent init_fiber failure; eventual abort/no-op.
Classification: ACCIDENTAL_SEMANTIC_DIVERGENCE (F5)
Authority: ADR §6, §7; fiber_ctx.hpp:68-71; group.cpp:14-19; group.hpp:196-197
Test evidence: NONE. Tests silently early-return on non-x86_64.
```

### T20 — Select inline winner versus suspended winner

```text
Setup (inline):    one Event arm SET at admission. select(sched, event_case).
Setup (suspended): no arm ready at admission; later set() makes one ready.
Trace (inline):    admit captures snapshot; any_ready true; lowest-index arm
                   marked CandidateReady; select_process_group_locked once;
                   select_publish_locked (phase=Completed, mode=Inline); return.
                   NO make_runnable, NO context_switch.
Trace (suspended): admit: no ready; caller->make_waiting; raise
                   suspend_switch_pending; context_switch. Later: event.set()
                   -> event_set_broadcast -> select_event_scan_locked marks
                   CandidateReady -> select_process_group_locked +
                   select_publish_locked (mode=Suspended) -> make_runnable +
                   route_runnable_locked(caller_owner) -> caller resumes;
                   validate phase=Completed + mode=Suspended + winner; phase=Consumed.
Expected: exactly one winner; one publication; losers cleaned up.
Actual: matches.
Classification: PROVEN_PARITY (Select is single-strategy; the inline vs
                suspended distinction is internal, not a Threaded/Evented split)
Authority: select.cpp:788-1282; e13-select-production-architecture.md §1
Test evidence: tests/select_inline_test.cpp ST-1..ST-8:80-441;
               tests/select_suspended_test.cpp ST-9/ST-10/ST-13:97-292
```

---

## 15. Existing test-evidence map

### 15.1 Test files inspected (per task §12)

```text
tests/future_test.cpp                 5 cases, Threaded only
tests/evented_future_test.cpp         3 cases (F1..F6), Evented + Threaded regression
tests/group_test.cpp                  5 cases, Threaded only
tests/evented_group_test.cpp          3 cases (G1..G6), Evented + Threaded regression
tests/evented_scheduler_test.cpp      4 cases (E4-T1..T4)
tests/scheduler_progress_test.cpp     4 cases (E6-T1..T4)
tests/external_wake_test.cpp         15 cases (T1..T14 + DRAIN-T1) — uses
                                     sluice_async_internal_testing for park seams
tests/multi_worker_test.cpp           5 cases (E7-T1,T2,T3,T7,T10A)
tests/runnable_steal_test.cpp        11 cases (E8-T1..T11)
tests/timer_wait_test.cpp           16 cases (E11-T0..T18) — uses
                                    sluice_async_internal_testing for clock
```

E12 primitive tests: `event_primitive_test.cpp` (42 cases),
`semaphore_primitive_test.cpp` (31), `async_mutex_primitive_test.cpp` (23),
`async_condition_primitive_test.cpp` (32), `async_queue_primitive_test.cpp` (24),
`async_rwlock_test.cpp` (22), plus authority probes and death tests.

E13 Select tests: P1–P7 staged, 13+ test binaries including death tests,
registration rollback, multi-worker publication.

Cross-primitive parity: `async_sync_cross_primitive_parity_test.cpp`
(`parity_d3_*`, `parity_d4_*`, `parity_waitoutcome_*`) — but this is E12
cross-primitive parity (Event/Semaphore/Mutex against each other), NOT
Threaded-vs-Evented parity.

### 15.2 What each existing test proves

| Test class | Proves | Does NOT prove |
|---|---|---|
| `future_test.cpp` | Threaded Future idempotency, cancel | Evented Future lifecycle |
| `evented_future_test.cpp` | Evented Future liveness (B runs while A suspended); Threaded regression | External-thread completion of Evented Future |
| `group_test.cpp` | Threaded Group await-all, cancel, drain | Evented Group |
| `evented_group_test.cpp` | In-scheduler Evented Group liveness; Threaded regression | External-producer Evented Group (F1); destruction with pending (F2) |
| `evented_scheduler_test.cpp` | E4 single-worker liveness cycle | Group integration |
| `scheduler_progress_test.cpp` | E6 real-backend progress; Evented Group with ThreadPool Completion | External-thread Future completion |
| `external_wake_test.cpp` | E9 external-wake protocol with explicit SchedulerWakeHandle | Future::complete_with as external producer (no wake handle) |
| `multi_worker_test.cpp` | E7 worker-local state, pinned resume | Group integration |
| `runnable_steal_test.cpp` | E8 steal/transfer exactly-once | Group integration |
| `timer_wait_test.cpp` | E11 deadline races, Drain STALLED, Live progress | Group integration |
| E12 primitive tests | Per-primitive contracts (CLOSED E12-G) | n/a (Evented-only primitives; no Threaded dual) |
| E13 Select tests | Select contracts (P1–P7) | n/a (Evented-only; no Threaded dual) |

### 15.3 Can any existing Evented Group test fail when Group::await returns with pending externally-produced work?

**No existing test exercises this path.** The two Evented Group tests
(`egroup_evented_group_task_suspends_and_resumes_on_fiber` G1+G2+G3+G4 and
`egroup_evented_group_runs_on_awaiting_thread` G5) both use **in-scheduler
production** — the awaited Future is completed by a peer Fiber on the same
scheduler (`evented_group_test.cpp:79-83`). No Evented Group test stages an
external-thread Future completion between `await()` calls.

Consequence: F1 is **not caught by any existing test**, and a regression test
for F1 cannot false-pass on an earlier invariant (the test must stage external
completion between Scheduler invocations).

### 15.4 Required deterministic regression tests (prepared, NOT implemented by this task)

For each, per task §12, the plan states: target/file, pre-fix expected failure,
exact causal boundary, post-fix expected result, why it cannot false-pass,
Debug/Release/TSan applicability. These are recorded in the implementation plan
(§20, P2) — NOT implemented here.

```text
RT-F1  external-thread Future completion during Evented Group::await
RT-F2  Evented Group destruction with a pending Evented task Future
RT-F3  init_fiber(false) failure on Evented Group::async, REPORTED BEFORE
       spawn/enqueue (simulated via the init seam or non-x86_64 target); the
       failure must surface (throw / fail-fast / Result) BEFORE the un-
       runnable Fiber is spawned, not silently after.
RT-F4  Evented Group size() semantics after await
RT-F5a unsupported-target Evented PUBLIC ADMISSION enforces the chosen gate
       (scoped static_assert / construction fail-fast / build exclusion):
       constructing Group(Scheduler&), Scheduler, or an E12 primitive on an
       unsupported target triggers the chosen enforcement, not a deferred
       abort at context_switch. Threaded stays compilable/functional; a
       shared-header include does NOT disable Threaded.
RT-F5b unsupported-target CI/test PROVES the fail/disable contract instead
       of silently early-returning (closes F5b TEST_GAP). The test observes
       the fail/disable behavior; it does NOT `if constexpr (!supported)
       return;` and report PASS having exercised nothing.
```

All must use phase seams / latches / controlled clock / explicit state
observation, NOT `sleep_for` as causal proof (M7).

**RT-F2 must prove the as-built first failure, not the rev-1 UAF theory.**
Rev-1 framed RT-F2 as an ASan use-after-free on Fiber/stack release. That is
the wrong proof target: the destructor's FIRST misbehavior is the invalid-
context dereference inside `Scheduler::await_ready_flag` (T11 PRIMARY TRACE).
RT-F2 is therefore reframed as two separate tests:

```text
RT-F2a (PRIMARY — proves the actual failure; required for F2a):
  Setup:   Evented Group g{sched}; g.async(fn) where fn awaits a Future that
           is NEVER completed. g.await() is either NOT called, or is called
           and returns early per F1 (pending == 1).
  Pre-fix expected behavior:
           Destroying g on the caller thread (the same thread that ran
           run_until_idle, on which g_worker is now nullptr) reaches
           group.cpp:89 f->await() on the pending Evented task Future, which
           enters Scheduler::await_ready_flag and dereferences g_worker
           (== nullptr) at scheduler.cpp:1075-1076. Under ASan/hardened this
           is reported as a null-pointer dereference; in plain Debug/Release
           it is latent UB.
  Exact causal boundary:
           The failure is reached via Future::await -> EventedWaitPolicy ->
           await_ready_flag, BEFORE ~Group reaches Fiber/stack release. The
           test must observe the failure at this exact boundary, not at the
           later vector-destruction phase.
  Post-fix expected result:
           Determined by F2a's accepted teardown policy (§16 F2): either the
           destructor debug-asserts / release fail-fasts on the pending
           Evented task Future (D5 caller-contract violation), or the bad
           state is made unreachable by F1-A so the destructor never observes
           a pending Evented task Future.
  Why it cannot false-pass:
           The test stages a pending Evented task Future with NO producer,
           so it cannot become terminal by luck of scheduling. The failure is
           reached on the destructor's existing Future::await path, not via a
           hypothetical race.
  Gates:   Debug (the assert / fail-fast path) + ASan/UBSan (the UB path).

RT-F2b (RESIDUAL — only if F2b is reachable after F2a's correction; an ASan
       /lifetime test, NOT the primary regression):
  Setup:   As for RT-F2a, but AFTER F2a's correction prevents the destructor
           from calling await() on a pending Evented task Future (e.g. the
           destructor debug-asserts). The question RT-F2b settles: if a task
           Fiber is still Registered in waiting_ready_ when ~Group frees
           evented_fibers_/evented_stacks_, can the Scheduler (or its later
           destruction) dereference the freed Fiber*?
  Pre-fix expected behavior:
           Depends on F2a's chosen policy. If the chosen policy is "fail-fast
           at destruction," RT-F2b is moot (the destructor never reaches the
           release phase in the bad state). If the chosen policy is "drain by
           contract," RT-F2b should prove under ASan that no residual
           waiting_ready_ registration outlives the Fiber.
  Why it cannot false-pass:
           It must NOT depend on sleep_for; it must observe Scheduler state
           (waiting_ready_count()) and Fiber lifetime directly. It is proposed
           ONLY for the residual path, and only after F2a is decided.
  Gates:   ASan/UBSan + TSan if the residual path is reachable.
```

RT-F2a is the mandatory regression for F2a. RT-F2b is conditional on F2a's
chosen teardown policy.

---

## 16. Confirmed findings ledger

Findings use the format required by task §15.

### F1 — Evented Group::await returns with pending externally-produced Futures

```text
ID: F1
CLASS: ACCIDENTAL_SEMANTIC_DIVERGENCE
SEVERITY: P1 (logical contract violation; potential use-after-free via F2)
STATUS: CONFIRMED — production trace in T9

PUBLIC OPERATION:
    Group::await() in Evented mode (Group(Scheduler&))

EXPECTED CONTRACT:
    "Wait until ALL tasks complete. Idempotent." (group.hpp:93-97).
    Mirrors Zig Io.Group (Io.zig:1282). ADR §3: the public task contract is
    logical control-flow waiting; the physical mechanism is strategy-
    determined but the logical contract is preserved.

AS-BUILT BEHAVIOR:
    Group::await (Evented branch, group.cpp:21-57) loops:
      count pending Futures; run_until_idle(); re-count;
      if after_pending == pending break.
    A task Fiber awaiting an external-thread-completed Future suspends via
    await_ready_flag. The Scheduler classifies MW-S3; Drain mode (run(1))
    returns STALLED. Group::await observes no progress and breaks, returning
    to the caller with one or more task Futures still pending.

PRODUCTION EVIDENCE:
    group.cpp:21-57            Evented await loop with no-progress break
    group.cpp:54              if (after_pending == pending) break;
    scheduler.cpp:821-829     Drain MW-S3 returns STALLED (E9-CORRECTIVE)
    future.hpp:66-74          complete_with sets ready_ + notifies Future's
                              cv_ only; NO Scheduler wake
    evented_wait_policy.hpp:53-57   EventedWaitPolicy calls await_ready_flag
                                    but does NOT call attach_ready_wake
    scheduler.cpp:4102+       attach_ready_wake exists but is never wired
                              from Future or EventedWaitPolicy

TEST EVIDENCE:
    NONE. No Evented Group test stages external-thread Future completion.
    Closest analog (external_wake_test.cpp:620 wake_external_producer_signal_only)
    proves the property for await_ready_flag WITH an explicit
    SchedulerWakeHandle, which Future::complete_with does not use.

DOCUMENT AUTHORITY:
    group.hpp:93-97           contract
    ADR-execution-model.md §3 logical wait contract is preserved across
                             strategies
    async-runtime-construction-method.md M2 dimension 4 (invocation/lifetime)
                             — the E9 lifetime-conflation defect class

ADVERSARIAL TRACE: T9 (this document, §14)

USER-OBSERVABLE EFFECT:
    Code after Group::await runs while task Fibers are still suspended. The
    caller observes g.size() > 0. Subsequent operations on resources the
    tasks were supposed to have produced observe incomplete state. If the
    caller destroys the Group, F2 may trigger.

ROOT CAUSE:
    Group::await treats Scheduler Drain STALLED as completion. The Future's
    external-thread completion path has no wiring to the Scheduler wake
    source (the seam exists — attach_ready_wake — but neither Future nor
    EventedWaitPolicy invokes it). This is the E9 lifetime-conflation
    defect class re-appearing at the Group layer: an external-wake-capable
    wait without an effective wake source, observed under a Drain run.

FOUR-DIMENSION ANALYSIS:
    resource:        task Future pending; ready_ flag false
    execution:       task Fiber Waiting; Group::await on caller thread (not Fiber)
    coordination:    Scheduler MW-S3; Drain returns STALLED; Group breaks
    invocation:      Group::await call returns STALLED-completion to caller;
                     Fiber remains Registered; lifetime hazard (F2)

SMALLEST CORRECTION DIRECTION (rev-2 — reframed; rev-1 incorrectly presented
the contract interpretation itself as an open human decision; see review B2):

    ACCEPTED SEMANTIC DIRECTION (frozen by existing authority, NOT a choice):
        Evented Group::await MUST wait for all admitted Group tasks to reach
        terminal completion, matching the installed public header
        ("Wait until ALL tasks complete. Idempotent." group.hpp:93-97) and
        ADR §3 (the logical wait contract is shared across strategies; only
        the physical mechanism differs). The contrary "an external producer
        must stage more work" comment at group.cpp:42-46 is an E5-era
        implementation comment of LOWER authority than the installed header
        and the ADR; per AGENTS §2 it cannot create an equal competing
        contract. F1-B (narrowing Evented Group to in-Scheduler producers
        only) is therefore NOT an available smallest correction under the
        CURRENT authority — it is a superseding-contract alternative that
        would require an explicit superseding ADR (AGENTS §2). It MAY be
        recorded as a rejected or future-superseding alternative, but it
        does not block the F1-A correction as an unresolved interpretation
        of current authority.

    REMAINING IMPLEMENTATION DECISION (a narrow mechanics decision, recorded
    for the implementation task; the contract is already decided):
        Choose the smallest lifetime-safe mechanism that realizes the
        await-all contract under external-thread producers. The mechanism
        MUST specify the actual wake-publication topology (§19.1) — calling
        attach_ready_wake is NOT sufficient by itself (review B3). Candidate
        mechanism families (recorded, not chosen here):
          (M1) Drive the Scheduler in Live mode from Group::await (run_live)
               AND realize the wake-publication topology of §19.1 so an
               external-thread Future completion routes through a
               SchedulerWakeHandle. This matches the E9 closed protocol and
               is the smallest correction that preserves the accepted
               contract.
          (M2) Some other lifetime-safe mechanism that preserves the
               await-all contract without the forbidden scope expansions
               (§2.2) and without weakening the SchedulerWakeHandle
               weak/generation contract (R-E14-3).
        F1-B (contract narrowing) remains OUT of scope for this preparation
        as a smallest correction.

REQUIRED REGRESSION TEST: RT-F1 (deterministic, no sleep_for)
FORMAL IMPACT: see §18 (F1-FORMAL: PROVISIONAL_NO_FORMAL_CHANGE — rev-3
              corrects rev-2's unconditional NO_FORMAL_CHANGE. The
              classification stays NO_FORMAL_CHANGE ONLY IF the chosen
              D-E14-1 mechanism introduces no distinct per-Future or per-
              registration attachment state and refines entirely to existing
              E9 transitions. It MUST be upgraded to EXTEND_EXISTING_MODEL
              (with model update before closure) if the mechanism introduces
              such a state. T-WAKE-1..8 are undecided; the preparation does
              not pre-judge the absence of new state before D-E14-1.)
OUT-OF-SCOPE ALTERNATIVES:
    Spinning, sleeping, retrying blindly, silently switching all Scheduler
    calls to Live, weakening the await contract, or adding producer
    provenance to Future (all forbidden by task §11 / ADR §9.2.9).
```

### F2 — Evented Group destructor cannot safely await a pending Evented task Future

```text
ID: F2  (split into F2a + F2b after the rev-1 review — see B1)
CLASS: ACCIDENTAL_SEMANTIC_DIVERGENCE
SEVERITY: P1 (invalid-context UB on a reachable destruction path)
STATUS: CONFIRMED — production trace in T11 (PRIMARY TRACE)

PUBLIC OPERATION:
    ~Group() in Evented mode with one or more task Futures still pending

EXPECTED CONTRACT:
    The Threaded destructor contract is "drain if await was never called"
    (group.cpp:77): join all task threads and await all task Futures from the
    caller thread, where ThreadedWaitPolicy::wait_until_ready may legitimately
    block on the Future's condition variable. There is no analogous SAFE
    destruction path for a pending Evented task Future: EventedWaitPolicy's
    documented precondition (evented_wait_policy.hpp:27-31) requires a
    Scheduler-driven Fiber context, and a destructor runs on an ordinary
    caller thread. Therefore the accepted contract must EITHER (i) make such
    destruction unreachable by guaranteeing await() drives all Evented tasks
    to terminal, OR (ii) make it fail cleanly (debug assert / release
    fail-fast) when a pending Evented task Future remains.

AS-BUILT BEHAVIOR (rev-2 — corrected):
    ~Group (group.cpp:76-92) has NO sched_ branch. It unconditionally:
      1. swaps tasks_   -> local_tasks;     (group.cpp:78-83)
      2. swaps futures_ -> local_futures;   (group.cpp:78-83)
      3. joins each local task thread;      (group.cpp:88; no-op in Evented
         mode — tasks_ is empty)
      4. calls f->await() on EVERY local Future;   (group.cpp:89)
      5. only afterward allows evented_fibers_ and evented_stacks_ to be
         destroyed by member destruction (group.cpp:90-91 comment).
    When an Evented task Future is still pending, step 4 calls Future::await
    -> EventedWaitPolicy::wait_until_ready -> Scheduler::await_ready_flag,
    which executes:
        WorkerState* ws = g_worker;       // == nullptr on the caller thread
        Fiber* me = ws->current;          // invalid-context null deref / UB
    (scheduler.cpp:1075-1076). g_worker is nullptr because the Scheduler
    cleared it before the single-worker run returned to the caller
    (scheduler.cpp:530-547).

    The rev-1 "releases evented_fibers_/evented_stacks_ without processing
    Futures" claim was materially incorrect: ~Group DOES process Futures, and
    that is exactly what makes the invalid-context deref reachable.

PRODUCTION EVIDENCE:
    group.cpp:76-92           destructor: no sched_ branch; unconditional
                              Future::await on line 89
    evented_wait_policy.hpp:53-57   wait_until_ready delegates to
                                    Scheduler::await_ready_flag
    scheduler.cpp:1074-1076   await_ready_flag uses g_worker->current with
                              no g_worker validation
    scheduler.cpp:530-547     g_worker set at single-worker run entry,
                              cleared at run exit (caller thread)
    group.hpp:152-153         Fiber/stack owned by Group vectors (relevant
                              to F2b only)

TEST EVIDENCE:
    NONE. tests/group_test.cpp:69 group_destructor_drains_unjoined_tasks
    covers the Threaded destructor only. No test destroys an Evented Group
    while an Evented task Future is pending.

DOCUMENT AUTHORITY:
    group.cpp:77              "Threaded: drain if await was never called"
                              (the Threaded-safe analogue)
    evented_wait_policy.hpp:27-31   Fiber-context precondition
    D5 caller-contract-violation policy (e10-e12-api-semantic-closure.md)

ADVERSARIAL TRACE: T11 (this document, §14) — PRIMARY TRACE is the
                   invalid-context deref; the residual UAF is a separate
                   trace (F2b) reachable only after F2a is corrected.

USER-OBSERVABLE EFFECT:
    On a reachable destruction path (Evented Group destroyed with a pending
    task Future — e.g. after F1's early return, or when await() was never
    called), the destructor exhibits undefined behavior on the caller thread
    (null deref under await_ready_flag). Under ASan/hardened builds this is
    reported as a null-pointer dereference. The behavior is non-deterministic
    in plain Debug/Release.

ROOT CAUSE:
    The Threaded and Evented destruction paths were written symmetrically
    (both swap+await) on the assumption that await() drives all tasks to
    terminal before destruction. That assumption holds for Threaded
    (std::thread join synchronizes worker exit before the Future is awaited)
    but does NOT hold for Evented: an Evented task Future can be left
    pending by F1's early return, or simply because await() was never
    called. The destructor then enters an Evented wait from a context that
    cannot legally host one.

FOUR-DIMENSION ANALYSIS:
    resource:        Evented task Future pending; ready_ false
    execution:       ~Group runs on an ordinary caller thread, NOT a Fiber
    coordination:    Scheduler's g_worker == nullptr on this thread;
                     await_ready_flag cannot establish a current Fiber
    invocation:      destruction bypasses the await/idempotency contract;
                     no Fiber context exists to host the Evented wait

SMALLEST CORRECTION DIRECTION (split into two accepted-behavior questions):

    F2a — destructor waiting context (the as-built first failure):
        A destructor CANNOT safely call a pending Evented Future's await()
        from outside a Scheduler Fiber. The accepted teardown policy must be
        chosen from:
          (i)  make the bad state unreachable by guaranteeing await() drives
               all Evented tasks to terminal (depends on F1's correction);
          (ii) debug-assert + release fail-fast at the top of ~Group if any
               Evented task Future is not yet terminal (matches D5 caller-
               contract-violation; surfaces F1+F2 together);
          (iii) explicit pre-destruction drain performed by the caller (a
               documented caller obligation, not a hidden destructor flush).
        The smallest correction consistent with accepted D5 is (ii) once F1
        is decided. The implementation task must not pick silently (i) by
        relying on F1-A; (ii) and (iii) are independent of the F1 decision.

    F2b — residual waiting_ready_ registration after Group destruction:
        After F2a is corrected, PROVE that no residual registration leaves a
        dangling dereference reachable. The actual dereference site is a LATER
        wake_ready_flags_locked() (scheduler.cpp:950-966), NOT the map's own
        destructor. The residual mechanics are:
          (1) the local Evented task Futures are destroyed at the end of the
              ~Group body, making the `waiting_ready_` key `&Future::ready_`
              (scheduler.cpp:1082) dangling;
          (2) Group member destruction then frees evented_fibers_, making
              WaitReg.fiber dangling;
          (3) the next wake_ready_flags_locked() reads it->first->load(...)
              (dangling ready-flag key) and then it->second.fiber (dangling
              Fiber).
        The unordered_map's OWN destruction at ~Scheduler (scheduler.cpp:108-
        190) merely destroys the stored raw pointer values {Fiber*,
        WorkerState*}; it does NOT dereference the key or the Fiber, so it is
        NOT itself the dereference hazard. Note that ~Scheduler asserts
        waiting_select_count_ == 0 and active_deadline_count_ == 0 but does
        NOT assert waiting_ready_.empty(). Either:
          - prove the F2a teardown policy prevents any residual registration
            (e.g. all task Fibers reached terminal before destruction); or
          - add the missing teardown assertion for waiting_ready_ at
            ~Scheduler (symmetric with the Select/timer assertions); or
          - document why the residual registration is safe (it currently is
            NOT — a later wake_ready_flags_locked would dereference the
            dangling key and Fiber).
        This is a SEPARATE lifetime question; it is not part of the F2a
        regression and is settled only after F2a's accepted policy is chosen.

REQUIRED REGRESSION TEST: RT-F2 (proves the invalid-context deref; see
                          §15.4 / Revision 2). A SEPARATE ASan/lifetime test
                          is proposed for the F2b residual path only.
FORMAL IMPACT: NO_FORMAL_CHANGE (lifetime invariant; no protocol transition)
OUT-OF-SCOPE ALTERNATIVES:
    Hidden destructor flushes that complete I/O (forbidden by AGENTS §7);
    implicit detach (forbidden by CP.26); weakening EventedWaitPolicy's
    Fiber-context precondition to silently no-op outside a Fiber (would mask
    the defect rather than fix it).
```

### F3 — init_fiber failure silently discarded by Group::async_evented

```text
ID: F3
CLASS: ACCIDENTAL_SEMANTIC_DIVERGENCE
SEVERITY: P2 (silent failure on unsupported target or resource exhaustion)
STATUS: CONFIRMED — production trace in T13

PUBLIC OPERATION:
    Group::async(fn) in Evented mode when Scheduler::init_fiber returns false

EXPECTED CONTRACT:
    Admission failure should be visible (throw, Result, or debug assert)
    BEFORE the Fiber is spawned. AGENTS §4: "A test that cannot fail on the
    pre-fix code is not proof of the repair" implies the failure must be
    reachable and observable.

AS-BUILT BEHAVIOR:
    group.hpp:196-197:
        bool ok = sched_->init_fiber(*fiber_raw, stack_base, kStackBytes);
        (void)ok;  // production code would propagate; E5 test asserts externally
    The Fiber is then spawn()ed unconditionally. spawn's make_runnable
    succeeds (Fiber state machine is independent of context init). The
    Scheduler eventually context_switches into an uninitialized Fiber.

PRODUCTION EVIDENCE:
    group.hpp:196-197         silent discard
    scheduler.cpp:400-403     init_fiber returns fiber_ctx::init_context result
    fiber_ctx.cpp:316-319     init_context returns false on non-x86_64
    fiber_ctx.hpp:140-144     context_switch stub returns nullptr
    fiber_ctx.cpp:311-314     context_switch_final stub std::abort()

TEST EVIDENCE: NONE.

DOCUMENT AUTHORITY:
    group.hpp:196-197 comment ("production code would propagate")
    ADR-execution-model.md §7 (fail/disable cleanly)

ADVERSARIAL TRACE: T13 (this document, §14)

USER-OBSERVABLE EFFECT:
    On a non-x86_64 target: Group::async returns normally; later
    Group::await either no-ops (header stub) or aborts (.cpp stub).
    On resource exhaustion (hypothetical): same.

ROOT CAUSE:
    E5-era "test asserts externally" deferment never replaced with production
    error propagation.

FOUR-DIMENSION ANALYSIS:
    resource:        Fiber ctx uninitialized; stack allocated
    execution:       async_evented on caller thread
    coordination:    spawn enqueues un-runnable Fiber
    invocation:      failure invisible to caller

SMALLEST CORRECTION DIRECTION:
    Propagate init_fiber failure: throw std::runtime_error (matching
    AsyncQueue's std::invalid_argument pattern) OR return Result<void> from
    async (breaking API change — less preferred). Throw is the smallest
    correction consistent with existing primitive conventions.

REQUIRED REGRESSION TEST: RT-F3
FORMAL IMPACT: NO_FORMAL_CHANGE
OUT-OF-SCOPE ALTERNATIVES:
    New Result API for async (incidental API change, forbidden by task §11).
```

### F4 — Evented Group size() diverges from Threaded after await

```text
ID: F4
CLASS: ACCIDENTAL_SEMANTIC_DIVERGENCE
SEVERITY: P3 (observational divergence; documented in test as expected)
STATUS: CONFIRMED — observable in existing test

PUBLIC OPERATION:
    Group::size() after Group::await()

EXPECTED CONTRACT:
    group.hpp:107-108: "Number of tasks currently in the group (live +
    completed, before await reaps)."
    Implied: after await reaps, size is 0 (Threaded branch swaps vectors).

AS-BUILT BEHAVIOR:
    Threaded await swaps futures_ out (group.cpp:65); size() returns 0.
    Evented await does NOT swap futures_ (group.cpp:21-57); size() returns
    the count of ready Futures still in the vector.

PRODUCTION EVIDENCE:
    group.cpp:60-73          Threaded swaps
    group.cpp:21-57          Evented does not swap
    group.hpp:109-112        size() returns futures_.size()

TEST EVIDENCE:
    tests/evented_group_test.cpp:94 asserts g.size() == 1 after await
    (Evented). The test ASSERTS the divergent behavior, treating it as
    expected. This is the divergence made normative by test.

DOCUMENT AUTHORITY:
    group.hpp:107-108        contract
    group.hpp:93-97          await "waits for ALL tasks"

ADVERSARIAL TRACE: T14 (this document, §14)

USER-OBSERVABLE EFFECT:
    Code that switches between Threaded and Evented Groups sees different
    size() semantics after await. Tests written against Threaded semantics
    fail under Evented.

ROOT CAUSE:
    Threaded and Evented await paths reap differently; the contract did not
    specify post-await size() precisely.

FOUR-DIMENSION ANALYSIS:
    resource:        futures_ vector contents differ post-await
    execution:       n/a
    coordination:    n/a
    invocation:      Threaded reaps (swap); Evented does not

SMALLEST CORRECTION DIRECTION:
    Either (a) make Evented await also reap (clear/swap futures_ when all
    are terminal), or (b) document size() as strategy-divergent explicitly.
    (a) restores parity; (b) acknowledges the divergence as
    INTENTIONAL_SURFACE_ASYMMETRY. A human decision is required.

REQUIRED REGRESSION TEST: RT-F4
FORMAL IMPACT: NO_FORMAL_CHANGE
OUT-OF-SCOPE ALTERNATIVES: none.
```

### F5 — Unsupported-target Evented admission / test behavior (H5)

```text
ID: F5  (rev-2 — split into F5a / F5b + an Observation after the rev-1
        review; see B5. Rev-1 conflated "the macro SLUICE_HAS_EVENTED does
        not exist" with "the capability gate is missing." The ADR permits
        an EQUIVALENT gate, and fiber_ctx::supported is one. The real
        production defect is that the capability is not ENFORCED at the
        Evented public admission boundary.)

CLASS (umbrella): split below.
SEVERITY: P2 (ADR-required capability enforcement is missing on the public
                admission boundary; tests silently pass on non-x86_64)
STATUS: CONFIRMED — production + doc trace in T19

This umbrella finding is split into three parts. Only F5a and F5b are
defects; the macro-name Observation is implementation-choice evidence, not
a defect by itself (the ADR explicitly permits an equivalent gate).

PUBLIC OPERATION (umbrella):
    Constructing/using Evented mode (Group(Scheduler&), Scheduler, E12
    primitives) on a target without fiber_ctx::supported

EXPECTED CONTRACT:
    ADR-execution-model.md §6, §7: a compile-time gate
    (SLUICE_HAS_EVENTED OR EQUIVALENT) that fails/disables Evented cleanly
    on unsupported targets; Evented does NOT silently substitute
    one-thread-per-task. The ADR's "or equivalent" wording is load-bearing:
    it permits fiber_ctx::supported (or any equivalent mechanism) to satisfy
    the gate. The spelling "SLUICE_HAS_EVENTED" is NOT normative.
```

#### F5a — Evented public admission does not enforce the capability gate

```text
ID: F5a
CLASS: ACCIDENTAL_SEMANTIC_DIVERGENCE (capability-contract violation)
SEVERITY: P2
STATUS: CONFIRMED

AS-BUILT BEHAVIOR:
    fiber_ctx::supported (fiber_ctx.hpp:68-71) is an existing
    source-derived CAPABILITY PREDICATE — a constexpr that is true on
    x86_64 and false elsewhere. It is the ADR-permitted equivalent of the
    gate's SIGNAL at the fiber layer, but until an admission or build
    mechanism actually consults it, it is only a predicate, not an enforced
    gate. The defect is that this capability is NOT enforced at the Evented
    PUBLIC admission boundary:
      - Group(Scheduler&) (group.cpp:14-19) constructs unconditionally;
      - Scheduler(AsyncIoContext&) (scheduler.cpp:98) constructs
        unconditionally;
      - every E12 primitive constructor (event.hpp, semaphore.hpp,
        async_mutex.hpp, condition.hpp, async_queue.hpp, async_rwlock.hpp)
        compiles and runs on unsupported targets with no arch check;
      - Group::async_evented discards init_fiber()'s false return
        (group.hpp:196-197 — see F3) and spawns the Fiber anyway.
    There is NO construction-time fail-fast and NO debug assert. The first
    observable failure is deferred to context_switch, which returns nullptr
    (header stub, fiber_ctx.hpp:140-144) or std::abort()s (.cpp stub,
    fiber_ctx.cpp:311-314).

PRODUCTION EVIDENCE:
    fiber_ctx.hpp:68-71      supported constant (the equivalent gate)
    fiber_ctx.hpp:140-144    context_switch stub returns nullptr
    fiber_ctx.cpp:308-319    stub init_context returns false; stub
                             context_switch_final aborts
    group.cpp:14-19          Group(Scheduler&) unconditional
    scheduler.cpp:98         Scheduler ctor unconditional
    group.hpp:196-197        init_fiber result discarded (F3)

DOCUMENT AUTHORITY:
    ADR-execution-model.md:189-191, 194-200   gating policy (the
    "or equivalent" wording permits fiber_ctx::supported)

ADVERSARIAL TRACE: T19 (this document, §14)

USER-OBSERVABLE EFFECT:
    A developer on a non-x86_64 target constructs Evented mode; gets no
    compile error, no construction-time fail-fast; observes eventual abort
    or a silent no-op switch.

ROOT CAUSE:
    The ADR-permitted equivalent capability predicate (fiber_ctx::supported)
    was used only in tests, not at the public admission boundary. The
    capability exists; it is simply not enforced where Evented objects enter
    the world.

SMALLEST CORRECTION DIRECTION (rev-3 — constrained so Threaded stays
portable; rev-2 recommended `static_assert(fiber_ctx::supported)` without
qualifying WHERE it may fire):
    Enforce fiber_ctx::supported at the Evented public admission boundary,
    WITHOUT making Threaded or the shared async headers fail to compile on
    unsupported targets. `fiber_ctx::supported` is, at this writing, a
    CAPABILITY PREDICATE (a constexpr true/false on architecture,
    fiber_ctx.hpp:68-71), NOT yet an admission/build gate; describing it as
    a gate is only accurate once an admission or build mechanism actually
    consults it. Options (recorded for the implementation task; not chosen
    here), ALL subject to the portability proof below:
      (a) A SCOPED static_assert(fiber_ctx::supported) placed ONLY inside
          Evented constructors / Evented-only translation units that are
          themselves excluded from non-Evented builds — NOT in shared async
          headers that Threaded code includes. Closest to the ADR's
          "compile-time gate" wording. An UNSCOPED static_assert in a
          general header would make merely including a shared async header
          fail to compile on unsupported targets and disable Threaded; that
          is FORBIDDEN.
      (b) Construction-time fail-fast: debug assert + release std::terminate
          (or a factory Result) on unsupported targets. This keeps shared
          headers compilable; the failure is at Evented object construction.
      (c) Build-level exclusion: compile Evented sources/APIs out
          (xmake/feature gate) so the Evented surface is unavailable rather
          than merely asserted-against.
      (d) A named SLUICE_HAS_EVENTED macro as syntactic sugar over
          fiber_ctx::supported (OPTIONAL cosmetic; does NOT fix F5a by
          itself — enforcement is the load-bearing part).
    (a scoped), (b), or (c) is required; (d) is optional.

    PORTABILITY PROOF REQUIRED for whichever option is chosen:
      - Threaded remains COMPILABLE and FUNCTIONAL on unsupported targets
        (Group() default ctor, ThreadedWaitPolicy, AsyncIoContext backends
        all build and run);
      - Evented sources/APIs are EXCLUDED, UNAVAILABLE, DELETED on
        unsupported targets, or FAIL CLEANLY at the admission boundary
        (per the chosen option) — never silently emulated as
        one-thread-per-task (ADR §7);
      - merely INCLUDING a shared async header (e.g. future.hpp,
        group.hpp for the Threaded surface, cancel.hpp) does NOT disable
        Threaded and does NOT trip a static_assert on an unsupported target.
    Until an admission/build gate enforces fiber_ctx::supported, treat the
    constant as a capability predicate, not as a gate.

REQUIRED REGRESSION TEST: RT-F5a
FORMAL IMPACT: NO_FORMAL_CHANGE (capability enforcement, not protocol)
OUT-OF-SCOPE ALTERNATIVES:
    Silent Threaded emulation (explicitly forbidden by ADR §7); an
    unscoped static_assert(fiber_ctx::supported) in a shared/general header
    (would disable Threaded on unsupported targets).
```

#### F5b — Unsupported-target tests return early and do not prove the fail/disable contract

```text
ID: F5b
CLASS: TEST_GAP
SEVERITY: P2
STATUS: CONFIRMED

AS-BUILT BEHAVIOR:
    Every Evented test early-returns on unsupported targets:
        if constexpr (!fiber_ctx::supported) return;
    A non-x86_64 build of the test suite therefore reports ALL TESTS PASSED
    having exercised no Evented code and having proven nothing about the
    ADR's "fail/disable cleanly" contract.

PRODUCTION EVIDENCE:
    every Evented test       `if constexpr (!fiber_ctx::supported) return;`
                             (e.g. evented_group_test.cpp:50, :104)

DOCUMENT AUTHORITY:
    ADR-execution-model.md:194-200   fail/disable contract on unsupported
    targets

ADVERSARIAL TRACE: T19 (this document, §14)

USER-OBSERVABLE EFFECT:
    CI on a non-x86_64 target reports a false green; an Evented regression
    on a supported target is not caught if the failure mode is
    target-dependent.

ROOT CAUSE:
    Tests treat unsupported targets as "skip silently" rather than "prove
    the fail/disable contract."

SMALLEST CORRECTION DIRECTION:
    Add a test that, on an unsupported target (or a simulated unsupported
    path), PROVES the chosen F5a enforcement (compile-time gate,
    construction fail-fast, or factory Result) instead of silently
    returning. The test must observe the fail/disable behavior, not skip it.

REQUIRED REGRESSION TEST: RT-F5b
FORMAL IMPACT: NO_FORMAL_CHANGE
OUT-OF-SCOPE ALTERNATIVES: none.
```

#### Observation — no `SLUICE_HAS_EVENTED` macro exists

```text
ID: F5-obs (NOT a defect — implementation-choice evidence)
CLASS: n/a (the ADR permits an equivalent gate)
SEVERITY: n/a
STATUS: NOTED (not a divergence by itself)

OBSERVATION:
    A grep over include/, src/, tests/, xmake.lua returns 0 hits for the
    literal SLUICE_HAS_EVENTED; only ADR-execution-model.md:189 references
    it. fiber_ctx::supported (fiber_ctx.hpp:68-71) is the implemented
    equivalent capability PREDICATE (a constexpr signal of Evented
    capability, not yet an enforced admission/build gate — see F5a).

CONCLUSION:
    The exact absence of a macro named SLUICE_HAS_EVENTED is NOT
    independently an accidental semantic divergence, because the ADR's
    "SLUICE_HAS_EVENTED OR EQUIVALENT" wording explicitly permits an
    equivalent mechanism (review B5). F5a (enforcement) is the load-bearing
    defect; this Observation is recorded only so a future task can decide
    whether to add the named macro as syntactic sugar (optional, cosmetic).
```

F3 remains an INDEPENDENT true finding because init_context() can also
return false on a NOMINALLY supported build — for example, when TSan
logical-Fiber creation fails (fiber_ctx.cpp). F3 is not reducible to F5a.

### F6 — Documentation and build-comment drift (H6)

```text
ID: F6
CLASS: DOC_DRIFT (multiple sub-findings)
SEVERITY: P2 (cumulative; some load-bearing in installed headers)
STATUS: CONFIRMED — direct quotes

Sub-findings (each with file:line evidence):

F6a  docs/async-runtime-plan.md:434   "E12-F RwLock [DEFERRED]"
     Reality: AsyncRwLock fully implemented (include/sluice/async/async_rwlock.hpp)
              and documented (docs/api-reference.md:728). DOC_DRIFT_STATUS.

F6b  docs/async-runtime-plan.md:538   E13 section has no closure banner
     Reality: select() shipped (select.hpp:8); P2-P7 tests exist.
              DOC_DRIFT_STATUS.

F6c  include/sluice/async/future.hpp:5,21,22,23
     "cppio has no fiber scheduler" / "no fiber runtime yet — PHASE E" /
     "await() BLOCKS THE CALLING THREAD" / "Threaded-equivalent"
     Reality: EventedWaitPolicy exists; Group(Scheduler&) exists in the
              same project. DOC_DRIFT_STATUS + DOC_DRIFT_MINOR (cppio name).

F6d  include/sluice/async/group.hpp:9,15,21,23
     "cppio shape (no fiber runtime)" / "No scheduler" /
     "Threaded-equivalent; not fiber yield"
     Reality: same header declares Group(Scheduler&) (group.hpp:65) and
              documents Evented mode (group.hpp:46-49). Self-contradictory.
              DOC_DRIFT_STATUS (high-impact).

F6e  xmake.lua:1130-1138   async_queue_primitive_test comment claims
     "P2+P3 scope ... no-Scheduler fast paths ... public AsyncQueue<T>
     wrapper lands in P8"
     Reality: AsyncQueue<T> is Scheduler-bound and shipped
              (async_queue.hpp:30,229). DOC_DRIFT_STATUS.

F6f  xmake.lua:691,778   references CPPIO_HAS_LIBURING
     Reality: defined macro is SLUICE_HAS_LIBURING (xmake.lua:731,753,787).
              DOC_DRIFT_MINOR (legacy naming).

F6g  README.md:339-345   frames async as "unblocked"/"proceeds" (future);
     no mention of Threaded/Evented, AsyncRwLock, select, or the
     Fiber/Scheduler runtime.
     Reality: async runtime largely implemented per api-reference.md.
              DOC_DRIFT_STATUS.

F6h  docs/api-reference.md   missing select() documentation
     Reality: select() is shipped public API (select.hpp:8).
              DOC_DRIFT_API.

F6i  include/sluice/async/{future,group,batch,cancel,fiber_ctx}.hpp +
     many docs/   "cppio" legacy name in installed headers and audit docs.
     DOC_DRIFT_MINOR (pervasive).

F6j  docs/zig-stdio-migration-jobs.md:431   "with no fiber runtime
     (PHASE E not started)"
     Reality: Fiber runtime exists. DOC_DRIFT_STATUS.

PRODUCTION EVIDENCE: cited inline above per sub-finding.
TEST EVIDENCE: n/a (documentation).
DOCUMENT AUTHORITY: AGENTS §12 (documentation discipline).

SMALLEST CORRECTION DIRECTION:
    Update each cited location to match as-built reality. Status banners
    (F6a, F6b, F6e) update async-runtime-plan.md and xmake.lua comments
    only. Header comments (F6c, F6d) update installed headers (permitted
    for the implementation task; NOT this preparation). README (F6g) and
    api-reference (F6h) update user-facing docs. Legacy naming (F6f, F6i)
    is optional cosmetic; defer unless the implementation task chooses to
    address it.

REQUIRED REGRESSION TEST: none (documentation).
FORMAL IMPACT: NO_FORMAL_CHANGE.
OUT-OF-SCOPE ALTERNATIVES: none.
```

---

## 17. Rejected findings ledger

### R1 — "E12 primitives should have Threaded equivalents"

```text
ID: R1
CLASS: FALSE_POSITIVE
STATUS: REJECTED
Reason: The E12 primitives (Event/Semaphore/AsyncMutex/AsyncCondition/
AsyncQueue/AsyncRwLock) are Evented-only BY ACCEPTED DESIGN (each ctor
borrows Scheduler&). The cross-primitive authority (e10-e12-api-semantic-
closure.md D1-D10, CLOSED) records this as intentional layering. Task §7
forbids inventing Threaded equivalents. Classification:
INTENTIONAL_SURFACE_ASYMMETRY (not a divergence).
```

### R2 — "Scheduler worker parking is a Threaded/Evented divergence"

```text
ID: R2
CLASS: FALSE_POSITIVE
STATUS: REJECTED
Reason: Scheduler parking is internal to the Evented substrate; it has no
Threaded equivalent and is not a public operation. The Drain/Live
distinction is invocation-lifetime policy (E9-CORRECTIVE), not a parity
axis. Classification: DOCUMENTED_PHYSICAL_DIFFERENCE / out of parity scope.
```

### R3 — "AsyncIoContext should be Fiber-aware"

```text
ID: R3
CLASS: FALSE_POSITIVE
STATUS: REJECTED
Reason: AsyncIoContext is the L1 strategy-neutral op-execution seam (ADR
016D). It is uniformly caller-thread-agnostic via std::mutex. The Fiber
integration is the Scheduler's responsibility (await_completion_*).
Classification: PROVEN_PARITY (L1 surface).
```

### R4 — "Future should use WaitNode/resolve_ CAS like the primitives"

```text
ID: R4
CLASS: FALSE_POSITIVE
STATUS: REJECTED
Reason: Future is the value-channel analogue (Zig {any_future, result}),
not a synchronization primitive. It uses ready_ + cv (Threaded) or
await_ready_flag (Evented) — both level-triggered. Adding resolve_ CAS
would be incidental API redesign (forbidden by task §11). The F1 defect
is about LIVENESS wiring (complete_with does not wake the Scheduler),
not about Future's resolution mechanism.
```

---

## 18. Formal-model impact

Per task §13. For each confirmed divergence:

### F1 — Evented Group::await external-producer divergence

```text
Decision: PROVISIONAL_NO_FORMAL_CHANGE  (rev-3 — rev-2 returned an
         unconditional NO_FORMAL_CHANGE while simultaneously asserting "no
         new attachment/lifetime state is introduced." That was premature:
         T-WAKE-1..8 are NOT yet decided, and T-WAKE-1's owner candidates
         explicitly include "a new narrow per-Future control," which WOULD
         be a distinct per-Future/per-registration attachment state. This
         rev-3 entry records the decision as PROVISIONAL and pins the
         upgrade condition concretely. It does NOT claim the absence of new
         state before D-E14-1 is resolved.)

Why PROVISIONAL_NO_FORMAL_CHANGE:
    The F1 correction restores the accepted logical await-all contract by
    routing external-thread Future completion through the EXISTING, CLOSED
    E9 external-wake protocol. The proposed topology of §19.1 has the
    producer hold a generation-validated SchedulerWakeHandle whose
    notify() -> signal_wake_locked path is ALREADY modeled by
    docs/spec/e9_park_wake/ (the ExternalReadyPublish transition) and
    docs/spec/e9_wake_handle_lifetime/ (the callback-lease lifetime). IF
    the chosen mechanism refines ENTIRELY to those existing E9 transitions
    (the wake capability is the existing SchedulerWakeHandle control
    block: alive, generation, Control::mtx callback lease; the completion-
    before-registration closure is the existing persistent ready_ flag +
    registration recheck in await_ready_flag), THEN no new modeled state is
    introduced and the formal classification stays NO_FORMAL_CHANGE.

    This is the EXPECTED outcome for the mechanism families that reuse the
    existing SchedulerWakeHandle (e.g. M1 in §19.1.1). It is NOT guaranteed
    for every candidate answer to T-WAKE-1: the candidate "a new narrow
    per-Future control" (§19.1.1) is a distinct per-Future attachment
    state. Because D-E14-1 is open, the preparation CANNOT yet assert
    unconditionally that no such state exists.

UPGRADE CONDITION (binding on the implementation task):
    Require EXTEND_EXISTING_MODEL (and update the E9 model before claiming
    closure) IF the selected D-E14-1 mechanism introduces any DISTINCT
    per-Future or per-registration ATTACHMENT STATE — for example:
      - a per-Future attachment field distinct from the wake handle's own
        generation (e.g. an explicit Attached/Published/Detached lifecycle
        on the Future or on the EventedWaitPolicy), or
      - a per-registration record beyond the existing {Fiber*, WorkerState*}
        value plus the SchedulerWakeHandle control block, or
      - any new admission/winner/teardown rule not expressible as a
        refinement of the existing ExternalReadyPublish + callback-lease
        transitions.
    In that case the implementation task MUST (a) add the new state and
    transitions to docs/spec/e9_park_wake/ (or a clearly-scoped E14
    extension), (b) preserve or add a negative/broken-model check
    demonstrating the counterexample (AGENTS §9), and (c) add a C++
    regression test connecting the modeled property to implementation
    behavior. T-WAKE-1..8 remain UNDECIDED until D-E14-1 is resolved; this
    preparation does NOT pre-judge them.

Required refinement evidence (the implementation task MUST demonstrate
these to keep the classification at NO_FORMAL_CHANGE; each must hold
WITHOUT introducing a distinct per-Future/per-registration attachment
state):
    - producer always owns a valid E9 wake capability while the Evented
      wait can remain registered (SchedulerWakeHandle outlives the wait
      registration by construction — R-E14-3 forbids weakening this);
    - completion-before-registration is covered by the persistent ready_
      flag and the registration recheck inside await_ready_flag
      (scheduler.cpp:1082-1090);
    - registration-before-completion is covered by producer notify()
      (existing ExternalReadyPublish transition);
    - completion racing teardown cannot call into a destroyed Scheduler
      (existing SchedulerWakeHandle callback lease — ADR §9.4.10);
    - notification loss cannot strand the Fiber (existing wake-epoch
      predicate + bounded timeout in MIXED-WAKE — ADR §9.4.5);
    - repeated completion does not create duplicate runnable publication
      (existing make_runnable CAS gate — scheduler.cpp wake_ready_flags_-
      locked; Future::complete_with exactly-once via ready_ — future.hpp:69).

Required (if the mechanism refines to existing transitions — NO new model;
the existing artifacts already cover the property):
    positive model: existing E9 park/wake model (unchanged)
    one-rule negative model: existing E9-CORRECTIVE negative (Drain parks
        on MW-S3 with an external-wake-capable wait — already produces the
        deterministic-hang counterexample that motivates the F1 correction)
    required invariant: external-ready publication wakes a parked Worker
        (already in E9-Inv)
    counterexample: the existing negative model already produces it
    production refinement map: Future::complete_with -> producer-held
        SchedulerWakeHandle::notify (NEW wiring to an EXISTING mechanism);
        Group::await -> run_live (NEW invocation of an EXISTING entry);
        every target transition already exists in the E9 model

    C++ regression test: RT-F1.

NOTE on rev-1's EXTEND_EXISTING_MODEL claim and rev-2's NO_FORMAL_CHANGE:
    Rev-1 suggested that wiring might introduce a new attachment/lifetime
    state (Unattached / Attached(handle-generation) / Published / Detached /
    Retired) and therefore require an E9 model EXTENSION. Rev-2 then
    declared NO_FORMAL_CHANGE unconditionally and asserted "no such state
    is required." Both over-claimed: rev-1 over-claimed EXTENSION before
    any mechanism was chosen, and rev-2 over-claimed its absence before
    D-E14-1 is resolved. Rev-3 records the decision as
    PROVISIONAL_NO_FORMAL_CHANGE and makes the upgrade to
    EXTEND_EXISTING_MODEL a binding condition keyed to the actual selected
    mechanism (above). T-WAKE-1..8 are undecided; the preparation cannot
    certify either classification until the mechanism is chosen.
```

### F2 — Evented Group destructor

```text
Decision: NO_FORMAL_CHANGE
Rationale: lifetime invariant; no protocol transition. The correction is
either a debug assert (caller-contract violation per D5) or a drain drive;
neither changes the modeled protocol.
```

### F3 — init_fiber failure propagation

```text
Decision: NO_FORMAL_CHANGE
Rationale: error reporting, not protocol.
```

### F4 — size() semantics

```text
Decision: NO_FORMAL_CHANGE
Rationale: observational semantics, not protocol.
```

### F5 — Unsupported-target gating

```text
Decision: NO_FORMAL_CHANGE
Rationale: capability gate, not protocol transition. If correction (c)
(supersede the ADR's gate description) is chosen, a superseding ADR
section is required (AGENTS §2) but no TLA+ model change.
```

### F6 — Documentation drift

```text
Decision: NO_FORMAL_CHANGE
Rationale: documentation only.
```

**No new large E14 TLA+ model is REQUIRED-IF-PROVISIONAL by this audit.**
F2–F6 reduce to (a) lifetime invariants / wiring defects against an existing
closed protocol (F2, F3), (b) observational/semantic asymmetries (F4),
(c) capability gating (F5), or (d) documentation (F6), and need no new
model. F1 is **PROVISIONAL_NO_FORMAL_CHANGE** (Rev-3): it requires no new
model ONLY IF the chosen D-E14-1 mechanism refines entirely to existing E9
transitions; if it introduces a distinct per-Future/per-registration
attachment state, F1 MUST be upgraded to EXTEND_EXISTING_MODEL with a
scoped model update before closure (see §18 F1).

---

## 19. Minimal corrective architecture

The smallest correction that restores already accepted contracts. Per task §11,
no incidental redesign.

### 19.1 The core correction (F1)

The accepted contract is frozen (Revision 3): Evented Group::await MUST wait
for all admitted Group tasks to reach terminal completion. The F1 defect is
the lifetime-conflation class: an external-wake-capable wait without an
effective wake source, observed under a Drain run, treated as completion.
The correction is therefore MECHANICAL, not a contract choice. Rev-1
presented the contract interpretation as an unresolved human decision; that
framing was incorrect (review B2).

#### 19.1.1 The required wake-publication topology (review B3)

Rev-1 repeatedly suggested the correction could be realized by "wiring
`attach_ready_wake` from `EventedWaitPolicy` or `Future::complete_with`."
That description is INCOMPLETE. `attach_ready_wake` (scheduler.cpp:4102-4131)
does NOT:

- retain a wake handle;
- store a callback;
- bind the Future to later producer publication; or
- cause a later `complete_with()` to notify the Scheduler.

It only verifies that `&ready` is currently registered and emits an
immediate `signal_wake_locked` if the flag has already become ready during
the attachment window. Meanwhile `Future::complete_with` (future.hpp:66-74)
only publishes the result/ready flag and notifies the Future's own cv; it
has no path to the Scheduler wake source. The implementation task MUST
define the actual required causal connection:

```text
Evented wait registration (await_ready_flag registers &ready_ in
waiting_ready_)
    <- lifetime-safe association ->
external producer terminal publication (Future::complete_with)
    ->
SchedulerWakeHandle::notify   (the producer holds the handle)
    ->
Scheduler wake epoch (signal_wake_locked -> wake_cv_ / wake_epoch_)
    ->
ready-flag drain (wake_ready_flags_locked observes ready_ true)
    ->
Fiber runnable publication (make_runnable + route_runnable_locked,
exactly-once via the existing CAS gate)
```

The implementation task need not select final code in this preparation, but
it MUST specify each of the following before claiming the F1 correction is
specified. These are the load-bearing design questions (recorded for the
implementation task; NOT decided here):

```text
T-WAKE-1  Wake-capability OWNER. Who owns the SchedulerWakeHandle (or
          equivalent safe control) for an Evented wait registration? The
          producer MUST hold it across the wait (R-E14-3 forbids weakening
          the wake handle's weak/generation contract). Candidates: the
          Future, the EventedWaitPolicy, the Group task Fiber, or a new
          narrow per-Future control. The choice affects Future's layout and
          must not introduce a public API change (task §11).

T-WAKE-2  Registration LIFETIME. How is the wake capability associated with
          the await_ready_flag registration, and how does that association
          outlive the wait? The registration is keyed by &ready_
          (scheduler.cpp:1084); the association must be established BEFORE
          the registration is visible to a concurrent complete_with, and
          torn down in the same critical section that resolves/erases the
          registration (so a stale handle cannot fire after teardown).

T-WAKE-3  Producer ACCESS. How does an external-thread producer reach the
          wake capability? Future::complete_with is the existing producer
          surface (future.hpp:66). The producer must either (a) call
          SchedulerWakeHandle::notify on a capability reachable from the
          Future, or (b) the wake must be routed through the existing
          complete_with body. Option (a) changes the producer contract;
          option (b) requires Future to know it was registered via
          EventedWaitPolicy.

T-WAKE-4  completion-BEFORE-attach behavior. If complete_with runs before
          the wait registration is visible, the persistent ready_ flag and
          the registration recheck inside await_ready_flag
          (scheduler.cpp:1085-1088) must let the Fiber observe readiness
          without suspending. The topology must NOT regress this closure.

T-WAKE-5  completion-AFTER-attach behavior. If complete_with runs after the
          registration is visible, the producer's notify() must reach
          signal_wake_locked (the existing ExternalReadyPublish transition).
          The bounded MIXED-WAKE timeout (ADR §9.4.5) is a safety net, NOT
          the proof of liveness (M7).

T-WAKE-6  detach/teardown behavior. When the wait resolves (Woken) or is
          retired, the wake-capability association must be torn down in the
          SAME critical section that erases the &ready_ registration, so a
          late notify() cannot strand the Fiber or fire into a destroyed
          Scheduler. The SchedulerWakeHandle callback lease (ADR §9.4.10)
          is the existing lifetime guarantee; the topology must reuse it,
          not invent a parallel mechanism.

T-WAKE-7  Scheduler DESTRUCTION race. If ~Scheduler races a producer
          notify(), the existing alive=false + Control::mtx lease
          (scheduler.cpp:108-135) must make the late notify a safe no-op.
          The topology must NOT add a raw Scheduler* callback (R-E14-3).

T-WAKE-8  EXACTLY-ONCE runnable publication. Repeated complete_with (second
          call is a no-op via ready_, future.hpp:69) and concurrent
          notify()/wake-epoch firing must not produce a second make_runnable
          for the same Fiber. The existing CAS gate at wake_ready_flags_-
          locked (scheduler.cpp) and make_runnable is the authority; the
          topology must reuse it.
```

"Call `attach_ready_wake`" is NOT by itself an implementation architecture
for F1. The implementation task MUST answer T-WAKE-1..T-WAKE-8 before the
F1 correction can be considered mechanically specified.

#### 19.1.2 Correction F1-A (the accepted-contract correction)

```text
Correction F1-A (preserve the accepted await-all contract):
    Realize the wake-publication topology of §19.1.1 by:
      - driving the Scheduler in Live mode from Group::await (run_live,
        not run_until_idle) so an unresolved external-wake-capable wait
        keeps the run resident; AND
      - establishing a lifetime-safe association (T-WAKE-1..T-WAKE-8)
        between the Evented wait registration and a producer-held
        SchedulerWakeHandle so an external-thread Future::complete_with
        wakes a parked Scheduler Worker through the existing E9 protocol.
    This matches ADR §3 (logical wait contract preserved across strategies)
    and the E9 closed external-wake protocol. It is the smallest correction
    that restores the accepted contract.
```

#### 19.1.3 Correction F1-B (NOT a smallest correction under current authority)

```text
Correction F1-B (narrow the contract — NOT available as a smallest
                 correction; recorded as a superseding-contract alternative
                 only):
    Document Evented Group::await as "in-scheduler producers only"; treat
    an external-thread Future completion during Evented Group::await as a
    caller contract violation; debug-assert (or release fail-fast) at the
    no-progress break so F1 surfaces as a detectable misuse; reclassify F1
    as INTENTIONAL_SURFACE_ASYMMETRY.
    This is a SMALLER code change but a LARGER authority change: it
    SUPPOSES the installed header and ADR §3. Under AGENTS §2 it requires
    an explicit superseding ADR. It is recorded here only so a future task
    can choose it deliberately; it does NOT block F1-A as an unresolved
    interpretation of current authority (review B2).
```

### 19.2 Dependent corrections

```text
F2 depends on F1 (but F2a is the as-built first failure and is reachable
    independently of the F1 decision):
    F2a (destructor invalid-context deref): ~Group calls Future::await on a
        pending Evented task Future, which dereferences g_worker (== nullptr)
        in await_ready_flag. The smallest F2a correction is independent of
        F1: debug-assert + release fail-fast at the top of ~Group if any
        Evented task Future is not yet terminal (matches D5), OR another
        accepted teardown policy (Revision 1 / §16 F2). This correction is
        REQUIRED regardless of F1, because the destructor must not exhibit
        UB even on the F1-early-return path.
    F2b (residual waiting_ready_ registration): if F1-A is chosen and
        Group::await drains all tasks, F2b becomes unreachable in the normal
        path (no Fiber remains Registered). If F1-B is chosen later via a
        superseding ADR, F2b must be settled by a teardown assertion or
        equivalent. Either way the implementation task must PROVE no
        residual registration can outlive the Fiber.

F3 is independent: propagate init_fiber failure (throw) regardless of F1.

F4 depends on the accepted size() contract (D-E14-3, a legitimate
    contract-clarification decision because the installed header's size()
    wording is less explicit than the await-ALL contract). Per Revision 3
    F1-A is the accepted F1 correction; if Evented await reaps like Threaded
    (swap/clear futures_), F4 is resolved as parity. D-E14-3 remains a real
    decision because the public size() wording is ambiguous (review A3).

F5 is independent: choose the gating mechanism (compile-time macro /
    construction fail-fast / ADR supersession). Per Revision 6, the real
    defect is admission enforcement, not macro spelling.

F6 is independent: documentation updates.
```

### 19.3 What is explicitly NOT in the corrective architecture

```text
- No new Executor abstraction.
- No new Awaitable hierarchy.
- No P2300 / coroutine / actor framework.
- No unified SynchronizationPrimitiveBase.
- No new CancelToken design.
- No Queue v1 external cancellation.
- No new Select arm types.
- No wait-all.
- No networking / AsyncReader/AsyncWriter bridge.
- No lock-free queue/deque optimization.
- No NUMA / affinity / priority / RwLock upgrade.
- No spinning / sleeping / retrying as causal proof.
- No silent weakening of Group::await contract.
```

---

## 20. Phased production implementation plan

RECORDED FOR A LATER TASK. (Historical: this plan was executed by
E14-IMPLEMENTATION-1 and merged via PR #29.) Per task §16, no
big-bang phase is acceptable. The actual phases must follow the evidence.

### P1 — Freeze E14 public semantic matrix and correct status/document drift

```text
authorized files:
    docs/async-runtime-plan.md             (status banners F6a, F6b)
    docs/api-reference.md                  (add select() doc F6h; OUT OF SCOPE
                                            for this preparation but in scope
                                            for P1 if the implementation task
                                            chooses; README/api-reference are
                                            excluded for THIS task only)
    README.md                              (F6g; same scope note)
    include/sluice/async/future.hpp        (header comments F6c)
    include/sluice/async/group.hpp         (header comments F6d)
    xmake.lua                              (build comments F6e, F6f)
public contract affected: none (documentation only)
production authority changed: none
tests required: none
formal impact: NO_FORMAL_CHANGE
sanitizer/platform gates: n/a
review exit condition: every F6 sub-finding corrected OR explicitly deferred
    with reason; git diff shows only doc/comment changes
```

### P2 — Add deterministic characterization / regression tests

```text
authorized files:
    tests/evented_group_test.cpp           (RT-F1, RT-F2, RT-F3, RT-F4)
    tests/future_test.cpp / new test file  (RT-F1 Future-level analog)
    tests/group_test.cpp                   (RT-F4 cross-strategy)
    tests/*death_test.cpp or new           (RT-F3 init_fiber(false) failure
                                            reported before spawn/enqueue;
                                            RT-F5a unsupported-target
                                            admission enforcement; RT-F5b
                                            unsupported-target fail/disable
                                            contract, not a silent early-
                                            return)
    xmake.lua                              (wire new test targets; possible
                                           build-level Evented exclusion for
                                           the RT-F5a/RT-F5b unsupported-
                                           target configuration)
public contract affected: none (tests only)
production authority changed: none
tests required: the tests being added
formal impact: NO_FORMAL_CHANGE
sanitizer/platform gates: Debug + TSan for RT-F1/RT-F2 (lifetime); Debug for
    RT-F3/RT-F5 (death/assert); Release for RT-F4 (observational)
review exit condition: each RT-* test FAILS on the pre-fix code for the
    intended reason and PASSES after the corresponding P3 correction; no
    sleep_for causal proof (M7); each test states why it cannot false-pass
```

Pre-fix expected behavior per test (must be reproduced before the P3 fix).
RT-F2 is split per Revision 2: the PRIMARY regression (RT-F2a) proves the
as-built first failure (invalid-context deref under await_ready_flag), NOT
the rev-1 UAF theory. RT-F2b is a conditional ASan/lifetime test for the
residual registration path only.

```text
RT-F1:  Group::await returns with size() > 0; task Fiber observed Waiting
        after await returns.
RT-F2a: ~Group with a pending Evented task Future reaches group.cpp:89
        f->await() -> EventedWaitPolicy -> Scheduler::await_ready_flag and
        dereferences g_worker (== nullptr on the caller thread) at
        scheduler.cpp:1075-1076. Under ASan/hardened: null-pointer
        dereference. In plain Debug/Release: latent UB. (This is the
        as-built first failure; rev-1's "ASan use-after-free on Fiber/stack
        release" was the wrong proof target.)
RT-F2b: (CONDITIONAL — only if F2b is reachable after F2a's correction)
        under ASan, prove no residual waiting_ready_ registration outlives
        the Fiber; or document the path as unreachable with a proof.
RT-F3:  Group::async on (simulated) init_fiber(false) failure returns
        normally pre-fix (no throw; later await aborts). The regression must
        stage init_fiber(false) via the init seam and assert the failure is
        REPORTED BEFORE spawn/enqueue — i.e. that the un-runnable Fiber is
        never enqueued. Post-fix: the chosen error surface (throw /
        fail-fast / Result) fires at admission, before spawn.
RT-F4:  Evented Group size() != 0 after await (currently ASSERTED by
        evented_group_test.cpp:94; the regression must surface this as a
        divergence, not a silently-accepted expectation).
RT-F5a: On a non-x86_64 build (or simulated via the chosen admission seam),
        constructing Evented mode (Group(Scheduler&), Scheduler, E12
        primitives) does NOT enforce fiber_ctx::supported at the public
        admission boundary pre-fix; the regression proves the chosen
        enforcement (scoped static_assert / construction fail-fast / build
        exclusion) fires post-fix. (Per Revision 6, the defect is admission
        enforcement, not macro spelling; per rev-3 the chosen gate MUST keep
        Threaded compilable/functional and MUST NOT live in a shared header
        Threaded code includes.)
RT-F5b: On a non-x86_64 build (or simulated path), an unsupported-target
        CI/test PROVES the fail/disable contract instead of silently
        early-returning (F5b TEST_GAP closure). It must observe the
        fail/disable behavior, not `if constexpr (!supported) return;`.
```

### P3 — Apply minimal Future/Group/runtime production corrections

```text
authorized files:
    include/sluice/async/future.hpp        (F1-A: realize the wake-publication
                                            topology of §19.1.1 — NOT just
                                            "call attach_ready_wake"; review B3)
    include/sluice/async/evented_wait_policy.hpp  (F1-A: wake-capability
                                            association per T-WAKE-1..8)
    include/sluice/async/group.hpp         (F3: throw on init_fiber failure;
                                            F4: reap on await per D-E14-3;
                                            F2a: destructor teardown policy)
    src/async/group.cpp                    (F1: run_live; F2a: destructor
                                            teardown policy; F3: throw;
                                            F4: reap)
    possibly include/sluice/async/scheduler.hpp / src/async/scheduler.cpp
        (only if a new narrow seam is required for the wake wiring; MUST NOT
         weaken the SchedulerWakeHandle weak/generation contract — R-E14-3)

public contract affected: Group::async exception spec (F3); Group::size()
    post-await (F4); Evented Group::await behavior is restored to the
    ACCEPTED await-all contract (F1 — Revision 3: the contract is NOT
    changed, only realized)
production authority changed: yes (Group behavior)
tests required: RT-F1, RT-F2a (+ RT-F2b if reachable), RT-F3, RT-F4 all
    pass post-fix
formal impact: see §18 F1 (PROVISIONAL_NO_FORMAL_CHANGE — rev-3. The
    classification stays NO_FORMAL_CHANGE only if the chosen D-E14-1
    mechanism introduces no distinct per-Future/per-registration attachment
    state and refines to existing E9 transitions; it MUST be upgraded to
    EXTEND_EXISTING_MODEL otherwise. T-WAKE-1..8 are undecided.)
sanitizer/platform gates: Debug + Release (public contract change, AGENTS
    §6.1) + TSan (Group/Scheduler interaction) + ASan/UBSan (lifetime F2a/F2b)
review exit condition: full Clang Debug gate green; Release green; ASan/UBSan
    green; TSan green; every RT-* test green; no test weakened; T-WAKE-1..8
    answered
```

P3 MUST be split into separate reviews if the corrections are large (one PR
per finding). Specifically:

```text
P3-F1:  Group::await external-producer correction. The CONTRACT is decided
        (await-all — Revision 3); the MECHANISM (T-WAKE-1..8) must be
        chosen first (D-E14-1). F1-B (contract narrowing) is NOT an
        available smallest correction without a superseding ADR.
P3-F2a: Group destructor teardown policy (independent of F1; small; REQUIRED
        regardless of F1 because the destructor must not exhibit UB on the
        F1-early-return path).
P3-F2b: Residual registration lifetime closure (conditional on F2a's policy;
        small if reachable).
P3-F3:  init_fiber throw (independent; small).
P3-F4:  size() semantics (depends on D-E14-3, a legitimate contract
        clarification).
```

### P4 — Close unsupported-target capability behavior (F5)

```text
authorized files:
    include/sluice/async/group.hpp         (SCOPED admission check ONLY —
    Evented ctor path; never an unscoped static_assert in a shared header;
    see F5a portability constraint)
    include/sluice/async/scheduler.hpp     (SCOPED admission check ONLY —
    same constraint)
    possibly include/sluice/async/fiber_ctx.hpp  (macro/predicate; no
        behavior change)
    possibly xmake.lua                     (build-level exclusion of Evented
        sources/APIs if option (c) is chosen)
    possibly docs/adr/ADR-execution-model.md  (superseding section if
        correction (d) macro-sugar is chosen)
public contract affected: Evented construction on unsupported targets
production authority changed: yes (capability predicate becomes an enforced
    admission/build gate)
tests required: RT-F5a, RT-F5b
formal impact: NO_FORMAL_CHANGE
sanitizer/platform gates: Debug + Release (gate behavior)
review exit condition: unsupported-target behavior matches the chosen gating
    mechanism; CI on x86_64 unaffected; non-x86_64 build (if testable)
    produces the chosen failure mode; AND the portability proof holds:
    Threaded remains compilable and functional on an unsupported target,
    Evented is excluded/unavailable/deleted or fails cleanly, and merely
    including a shared async header does NOT disable Threaded or trip a
    static_assert on an unsupported target
```

### P5 — Update public API documentation and examples

```text
authorized files:
    docs/api-reference.md                  (select(); F1 contract clarification)
    docs/api-reference-zh.md               (parallel)
    README.md                              (async runtime orientation; F6g)
    examples/ (if any)                     (Evented Group usage)
public contract affected: documentation
production authority changed: none
tests required: none
formal impact: NO_FORMAL_CHANGE
sanitizer/platform gates: n/a
review exit condition: documented contracts match as-built behavior after P3
```

### P6 — Run full Debug/Release/ASan/UBSan/TSan and stability gates

```text
authorized files: none (verification only)
commands:
    xmake f -m debug --toolchain=clang -y && xmake build sluice_core &&
        xmake build sluice_async && xmake build -g test && xmake test -v
    xmake f -m release --toolchain=clang -y && ... (AGENTS §6.1)
    xmake f -m asanubsan --toolchain=clang -y && ... (AGENTS §6.2)
    xmake f -m tsan --toolchain=clang -y && ... (AGENTS §6.3)
    scripts/verify-e{8,9}-stability.sh (if applicable to Group changes)
    scripts/verify-e12-*-formal.sh (if applicable)
review exit condition: all gates green; no test weakened; restore Debug config
```

### P7 — Independent adversarial review and E14 closeout

```text
authorized files: docs/reviews/E14-...-CLOSEOUT.md (new); docs/async-runtime-plan.md
public contract affected: none
production authority changed: none
tests required: none
formal impact: none
review exit condition: independent reviewer reads production first (M9);
    confirms every F1-F5 correction; signs off E14-CLOSED in async-runtime-plan
```

---

## 21. Verification matrix

What evidence will prove each correction. Per task §14, no claim without
command-backed evidence.

| Correction | Verification |
|---|---|
| F1 (F1-A, the accepted correction) | RT-F1 fails pre-fix (Group::await returns with size>0 and a task Fiber observed Waiting after await returns); passes post-fix; TSan clean; matches the existing E9 negative-model counterexample shape. Formal: PROVISIONAL_NO_FORMAL_CHANGE (Rev-3) — verify the chosen mechanism introduces no distinct per-Future/per-registration attachment state; otherwise EXTEND_EXISTING_MODEL with a model update is required |
| F2a (destructor invalid-context deref) | RT-F2a reaches Future::await -> await_ready_flag with g_worker == nullptr pre-fix (null-deref under ASan/hardened; latent UB otherwise); post-fix the chosen teardown policy (debug assert / release fail-fast / drain-by-contract) is observed at the top of ~Group |
| F2b (residual registration; conditional) | RT-F2b ASan-clean post-fix ONLY IF the F2b residual path is reachable after F2a's correction; otherwise documented as unreachable with a proof |
| F3 | RT-F3 stages init_fiber(false) via the init seam and asserts the failure is REPORTED BEFORE spawn/enqueue (the un-runnable Fiber is never enqueued); post-fix the chosen error surface (throw / fail-fast / Result) fires at admission, eliminating the deferred abort |
| F4 | RT-F4 size() matches the chosen D-E14-3 contract across Threaded/Evented |
| F5 | RT-F5a admission enforcement fires on unsupported target at the Evented public admission boundary (scoped static_assert / construction fail-fast / build exclusion), keeping Threaded compilable and functional; RT-F5b unsupported-target CI/test PROVES the fail/disable contract instead of silently early-returning (does NOT `if constexpr (!supported) return;`) |
| F6 | git diff shows only doc/comment changes; status banners match as-built |

Full Debug gate must remain green throughout. Release gate required for P3
(public contract change, AGENTS §6.1). ASan/UBSan required for P3 (lifetime
F2, AGENTS §6.2). TSan required for P3 (Scheduler interaction, AGENTS §6.3).

---

## 22. Residual risks and explicit deferrals

### 22.1 Decisions required (BLOCKING P3)

Per Revision 3, the F1 CONTRACT is already decided by existing authority
(await-all); what remains is a narrow MECHANICS decision plus the legitimate
contract-clarification decisions for size() and gating, plus the F2a
destructor teardown policy (promoted to a numbered decision D-E14-F2a in
rev-3+1). Rev-1 framed D-E14-1 as an unresolved contract question; that
framing was incorrect (review B2). The FOUR blocking decisions are
**D-E14-1** (F1 mechanism), **D-E14-F2a** (F2a destructor teardown policy),
**D-E14-2** (F5 admission mechanism), and **D-E14-3** (size() semantics);
all four must be resolved by accepted authority before implementation may
be authorized (§23 C1–C4).

```text
D-E14-1 (F1 MECHANISM — the contract is decided; only the mechanism is open):
    Choose the smallest lifetime-safe mechanism that realizes the accepted
    await-all contract under external-thread producers. The mechanism MUST
    answer T-WAKE-1..T-WAKE-8 (§19.1.1) and MUST NOT weaken the
    SchedulerWakeHandle weak/generation contract (R-E14-3). The accepted
    semantic direction is F1-A. F1-B is NOT available as a smallest
    correction under current authority; it requires an explicit superseding
    ADR (AGENTS §2) and is recorded only as a future alternative.
    Authority already applying: group.hpp:93-97 + ADR §3 (await-all is the
    accepted contract). The E5-era implementation comment at group.cpp:42-46
    ("an external producer must stage more work") is lower-authority evidence
    of a known-and-deferred defect, NOT an equal competing contract.
    WHAT IS REQUIRED FROM A HUMAN: pick the mechanism family (M1 run_live +
    wake topology, or another lifetime-safe mechanism) and answer T-WAKE-1..
    T-WAKE-8. NOT a YES/NO on the contract.

D-E14-F2a (F2a DESTRUCTOR TEARDOWN POLICY — promoted to a numbered decision
         in rev-3+1 so it has a stable ID for the implementation prompt and
         decision record; does NOT renumber D-E14-2 / D-E14-3):
    Choose the accepted behavior when an Evented Group reaches destruction
    with a pending task Future (the F2a as-built first failure: ~Group has
    no sched_ branch and unconditionally awaits every task Future, reaching
    await_ready_flag on a caller thread where g_worker is null — see §14 T11
    PRIMARY, §16 F2). Candidate policies (already recorded in §16 F2 F2a):
      (i)  make the bad state unreachable by guaranteeing await() drives all
           Evented tasks to terminal (depends on F1's correction);
      (ii) debug-assert in Debug AND fail-fast (release std::terminate or
           equivalent) at the top of ~Group if any Evented task Future is
           not yet terminal, BEFORE calling Evented Future::await from a
           non-Fiber context — matches D5 caller-contract-violation and
           surfaces F1+F2 together;
      (iii) explicit pre-destruction drain performed by the caller (a
           documented caller obligation, NOT a hidden destructor flush —
           AGENTS §7 forbids hidden destructor I/O flushes).
    PREFERRED SMALLEST CORRECTION UNDER D5: (ii). The implementation task
    MUST NOT silently pick (i) by relying on F1-A; (ii) and (iii) are
    independent of the F1 decision.
    F2b CLOSURE CONDITION (binding): the chosen F2a policy MUST also state
    whether F2b is PROVEN UNREACHABLE (no residual waiting_ready_
    registration can outlive the Fiber) or REQUIRES an explicit residual-
    registration teardown assertion/test. If (ii) is chosen, F2b is moot in
    the bad state (the destructor never reaches member destruction) but
    the implementation task must still state the F2b disposition.
    Authority required: D5 caller-contract-violation policy
    (e10-e12-api-semantic-closure.md); group.cpp:76-92 (the no-sched_-branch
    destructor); evented_wait_policy.hpp:27-31 (Fiber-context precondition).

D-E14-2 (F5 gating MECHANISM — Revision 6 narrows this from "macro spelling"
         to "admission enforcement"; rev-3 adds the Threaded-portability
         constraint):
    Choose how to ENFORCE fiber_ctx::supported (an ADR-permitted equivalent
    CAPABILITY PREDICATE, true on x86_64 / false elsewhere) at the Evented
    public admission boundary (Group(Scheduler&), Scheduler, E12 primitive
    ctors) AND how to make unsupported-target tests prove the fail/disable
    contract instead of silently early-returning. Options (ALL subject to
    the portability proof in §16 F5a): (a) a SCOPED
    static_assert(fiber_ctx::supported) placed ONLY inside Evented
    constructors / Evented-only TUs — never in a shared async header
    Threaded code includes (an unscoped static_assert is FORBIDDEN because
    it disables Threaded on unsupported targets); (b) construction-time
    fail-fast (debug assert + release std::terminate or factory Result);
    (c) build-level exclusion of Evented sources/APIs (xmake/feature gate);
    (d) a named SLUICE_HAS_EVENTED macro as syntactic sugar over
    fiber_ctx::supported (optional, cosmetic). (d) alone does NOT fix F5a
    — enforcement is the load-bearing part.
    REQUIRED PROOF for the chosen option: Threaded stays compilable and
    functional; Evented sources/APIs are excluded/unavailable/deleted or
    fail cleanly at the admission boundary; merely including a shared async
    header does NOT disable Threaded and does NOT trip a static_assert on
    an unsupported target.
    Authority required: ADR §6/§7 permit an equivalent gate; an explicit
    decision record for the chosen enforcement mechanism.

D-E14-3 (F4 size() semantics — a legitimate contract-clarification decision,
         per review A3, because the public size() wording is less explicit
         than the await-ALL contract):
    Reap on Evented await (parity with Threaded, which swaps futures_ out at
    group.cpp:65) or document size() as strategy-divergent
    (INTENTIONAL_SURFACE_ASYMMETRY)? The installed header
    ("live + completed, before await reaps", group.hpp:107-108) is
    ambiguous about post-await semantics; this is a real clarification, not
    a reinterpretation of settled authority.
    Authority required: group.hpp contract clarification.
```

### 22.2 Out-of-scope (deferred by design)

```text
- Threaded equivalents of E12 primitives (R1).
- AsyncReader/AsyncWriter bridge (E15+ frontier).
- Wait-all, cross-Scheduler Select, multi-epoch SelectGroup (E13 deferrals).
- Queue v1 external cancellation (D4 deferred).
- Performance optimization (E15+).
- Real io_uring path validation (separate from E14; only the stub/off path
  is in E14 scope).
```

### 22.3 Residual risks after P1–P7

```text
R-E14-1: The F1 correction (whichever chosen) changes Group::await behavior.
         Existing Evented Group tests (G1..G6) MUST be re-validated; G5
         (task_tid == awaiter_tid) is particularly sensitive.

R-E14-2: Sanitizer caveats for Fiber/stack-switching code (ADR R6). ASan/TSan
         results for Evented paths may be misleading; rely on deterministic
         causal seams (M7) for proof, sanitizer for additional coverage.

R-E14-3: The SchedulerWakeHandle weak/generation contract (ADR §9.4.10) must
         not be weakened by F1-A wiring; the implementation must use the
         existing handle mechanism, not a raw Scheduler* callback.

R-E14-4: This audit did NOT run ASan/UBSan/TSan (read-only preparation).
         The implementation task MUST run them per AGENTS §6.2/§6.3.

R-E14-5: This audit did NOT validate real io_uring (no --with-liburing).
         The implementation task is unaffected (F1-F6 are io_uring-independent).
```

---

## 23. Final readiness verdict

```text
E14-PREPARATION: NOT-READY
```

The rev-1 verdict `READY-WITH-CONSTRAINTS` is **withdrawn**. The rev-1
independent review returned `E14-PREPARATION-REVIEW: FAIL` (B1–B5); the
rev-2 independent review returned `E14-PREPARATION-REV2-REVIEW: FAIL`
(seven required corrections, applied in rev-3); the rev-3 independent
review returned `E14-PREPARATION-REV3-REVIEW: FAIL` on artifact-integrity
and Review-Request-staleness grounds (plus the T12 `g_worker` wording,
applied in rev-3+1); and the rev-3+1 independent review returned
`E14-PREPARATION-REV3-REVIEW: PASS-WITH-OBSERVATIONS` (O1–O3, applied
here). The substantive findings and traces now pass independent review,
but this document STILL does NOT authorize implementation, because:

1. The load-bearing F1 correction is not yet MECHANICALLY specified. Rev-2
   freezes the accepted contract (await-all — Revision 3) and specifies the
   required wake-publication topology (Revision 4, §19.1.1 T-WAKE-1..8), but
   the implementation task must still answer T-WAKE-1..T-WAKE-8 before the
   F1 correction can be considered specified. The preparation's own standard
   (rev-1 §23, repeated below) requires every confirmed divergence to have a
   reachable production trace AND a mechanically specified smallest
   correction; F1 currently has the trace but not the mechanism.
2. F2's as-built destructor trace was materially incorrect in rev-1. Rev-2
   corrects the trace (Revision 1) and the regression (Revision 2), but a
   rev-1-style claim that "every confirmed divergence has a reachable
   production trace" would have been FALSE for F2 as written. The corrected
   trace must be independently re-reviewed before implementation.
3. The F1 formal-impact classification was internally inconsistent in rev-1
   (`EXTEND_EXISTING_MODEL` AND "model unchanged"), and rev-2 over-corrected
   by asserting an unconditional `NO_FORMAL_CHANGE` ("no new state is
   required") while D-E14-1 (T-WAKE-1..8) was still undecided. Rev-3 records
   the classification as `PROVISIONAL_NO_FORMAL_CHANGE` keyed to the chosen
   mechanism (Revision 5 / rev-3): it stays `NO_FORMAL_CHANGE` only if the
   mechanism introduces no distinct per-Future/per-registration attachment
   state and refines entirely to existing E9 transitions; otherwise it MUST
   be upgraded to `EXTEND_EXISTING_MODEL`. This provisional classification
   must be re-reviewed, and the implementation task must apply the binding
   upgrade condition once D-E14-1 is resolved.

Justification for `NOT-READY` (what IS done vs. what is NOT):

- DONE: the parity scope is frozen (§5 taxonomy; §6 matrix; §7 topology);
  every public operation is classified (§6); F2's trace is corrected
  (Revision 1); the F1 contract is frozen to await-all (Revision 3); the
  required wake-publication topology is specified as design questions
  (Revision 4); the F1 formal-impact classification is recorded as
  PROVISIONAL_NO_FORMAL_CHANGE with a binding upgrade condition (Revision 5
  / rev-3); F5 is narrowed to F5a (enforcement) + F5b (test gap) + a
  non-defect Observation (Revision 6).
- NOT DONE: the F1 mechanism is not chosen (T-WAKE-1..8 unanswered), so the
  formal classification cannot yet be finalized; F2's corrected trace has
  not been independently re-reviewed; the provisional formal classification
  has not been independently re-reviewed.

CONSTRAINTS that must be resolved before implementation may be authorized:

```text
C1. D-E14-1 (F1 MECHANISM — the contract is decided; only the mechanism is
    open): answer T-WAKE-1..T-WAKE-8 and pick the mechanism family (M1
    run_live + wake topology, or another lifetime-safe mechanism) before
    P3-F1. NOT a YES/NO on the contract.
C2. D-E14-F2a (F2a DESTRUCTOR TEARDOWN POLICY): choose the accepted
    destructor behavior for an Evented Group with a pending task Future
    (preferred under D5: debug-assert + release fail-fast at the top of
    ~Group before any Evented Future::await from a non-Fiber context), and
    state the F2b disposition (proven unreachable vs. requires an explicit
    residual-registration teardown assertion/test), before P3-F2a.
C3. D-E14-2 (F5 gating MECHANISM — admission enforcement, not macro
    spelling): choose how to enforce fiber_ctx::supported at the Evented
    public admission boundary (F5a) and how to make unsupported-target
    tests prove the fail/disable contract (F5b) before P4.
C4. D-E14-3 (F4 size() semantics — a legitimate contract-clarification
    decision): reap on Evented await (parity) or document as
    strategy-divergent, before P3-F4.
C5. INDEPENDENT REVIEW of the complete preparation artifact is required
    before any implementation phase begins. The rev-1 and rev-2 reviews
    FAILED; rev-3 and rev-3+1 applied the mandatory revisions but do not
    self-certify. The reviewer must read production first (M9) and confirm:
    (a) F2's corrected trace matches production; (b) F1's frozen contract
    and specified topology are sound; (c) the F1 formal classification
    (PROVISIONAL_NO_FORMAL_CHANGE) is justified; (d) F5's split is correct;
    (e) T-WAKE-1..8 are answerable within the forbidden-scope constraints
    (§2.2); (f) D-E14-F2a is decidable as framed. The rev-3+1 review
    returned PASS-WITH-OBSERVATIONS (O1–O3 applied here); a fresh full
    re-review is NOT required after these mechanical observation fixes —
    only a grep/diff sanity check that O1–O3 landed.
```

This document does NOT authorize production implementation. Implementation
may begin only after C1–C4 are resolved by accepted authority AND an
independent review of the complete preparation artifact signs off (C5).

---

## 24. Revision ledger (rev-1 → rev-2 → rev-3 → rev-3+1)

This section consolidates the changes made in response to the rev-1,
rev-2, rev-3, and rev-3+1 independent reviews. Rev-1 returned
`E14-PREPARATION-REVIEW: FAIL` (B1–B5 + one required correction); rev-2
returned `E14-PREPARATION-REV2-REVIEW: FAIL` (seven required corrections,
applied as rev-3 Revisions R3-1..R3-7); rev-3 returned
`E14-PREPARATION-REV3-REVIEW: FAIL` on artifact-integrity + Review-Request-
staleness grounds (T12 `g_worker` wording also fixed), applied as rev-3+1;
rev-3+1 returned `E14-PREPARATION-REV3-REVIEW: PASS-WITH-OBSERVATIONS`
(O1–O3), applied below. This is the single source of truth for what
changed and why.

```text
REVIEW FINDING | REV-1 STATE              | REV-2 CHANGE (this document)
---------------+--------------------------+-------------------------------
B1 (BLOCKING)  | F2 / T11 / RT-F2 trace   | Revision 1: T11 PRIMARY TRACE
F2 destructor  | claimed the Evented      | rewritten — ~Group has NO sched_
trace wrong    | destructor "releases     | branch; it unconditionally awaits
               | evented_fibers_/         | every task Future (group.cpp:89);
               | evented_stacks_ without  | a pending Evented task Future
               | processing Futures".     | dereferences g_worker (== nullptr)
               | Production does the      | in await_ready_flag before any
               | OPPOSITE.                | Fiber/stack release. F2 split into
               |                          | F2a (as-built first failure) + F2b
               |                          | (residual registration). §9.1, §6.2,
               |                          | §12 matrix rows, §16 F2 all updated.
               |                          | Revision 2: RT-F2 split into
               |                          | RT-F2a (primary, proves the deref)
               |                          | + RT-F2b (conditional ASan test).
---------------+--------------------------+-------------------------------
B2 (BLOCKING)  | D-E14-1 framed as an     | Revision 3: F1 contract FROZEN to
D-E14-1        | unresolved YES/NO on     | await-all by existing authority
framed wrong   | whether Evented Group    | (group.hpp:93-97 + ADR §3). D-E14-1
               | supports external        | reframed as a narrow MECHANISM
               | producers.               | decision (answer T-WAKE-1..8). F1-B
               |                          | is NOT a smallest correction without
               |                          | a superseding ADR. §16 F1, §19.1,
               |                          | §22.1 updated.
---------------+--------------------------+-------------------------------
B3 (BLOCKING)  | "attach_ready_wake"      | Revision 4: §19.1.1 specifies the
attach_ready_  | presented as sufficient  | actual wake-publication topology
wake not       | F1 wiring. It is not —   | and the 8 load-bearing design
sufficient     | it neither retains a     | questions T-WAKE-1..8 (owner,
               | handle nor stores a      | lifetime, producer access,
               | callback.                | completion-before/after-attach,
               |                          | detach/teardown, Scheduler
               |                          | destruction race, exactly-once).
---------------+--------------------------+-------------------------------
B4 (BLOCKING)  | §18 F1 declared          | Revision 5 / rev-3: §18 F1
Formal impact  | EXTEND_EXISTING_MODEL    | classification changed to
inconsistent   | while also saying the    | PROVISIONAL_NO_FORMAL_CHANGE keyed
               | model is unchanged and   | to the chosen mechanism. Rev-2 had
               | the map extension is     | asserted unconditional
               | mechanical; rev-2 then   | NO_FORMAL_CHANGE ("no new state is
               | over-asserted an         | required"), which over-claimed while
               | unconditional             | D-E14-1 (T-WAKE-1..8) was undecided.
               | NO_FORMAL_CHANGE ("no    | The classification stays
               | new state is required"). | NO_FORMAL_CHANGE ONLY IF the chosen
               |                          | mechanism introduces no distinct
               |                          | per-Future/per-registration
               |                          | attachment state and refines to
               |                          | existing E9 transitions; otherwise
               |                          | it MUST be upgraded to
               |                          | EXTEND_EXISTING_MODEL with a model
               |                          | update before closure. T-WAKE-1..8
               |                          | remain undecided.
---------------+--------------------------+-------------------------------
B5 (REQUIRED)  | F5 classified "macro     | Revision 6: F5 split into F5a
F5 too broad   | SLUICE_HAS_EVENTED does  | (admission enforcement — the real
               | not exist" as an         | defect; ACCIDENTAL_SEMANTIC_
               | ACCIDENTAL_SEMANTIC_     | DIVERGENCE), F5b (test gap; TEST_
               | DIVERGENCE by itself.    | GAP), and a non-defect Observation
               | The ADR permits an       | (macro-name absence; the ADR
               | equivalent gate.         | permits an equivalent gate).
               |                          | §13.3, §16 F5 updated.
---------------+--------------------------+-------------------------------
(derived)      | Final verdict            | Revision 7: §23 verdict changed
               | READY-WITH-CONSTRAINTS   | from READY-WITH-CONSTRAINTS to
               | unsupported because F2   | NOT-READY. F2's corrected trace and
               | had the wrong production | the F1 mechanism (T-WAKE-1..8) must
               | path and F1's correction | be independently re-reviewed before
               | is not mechanically      | implementation.
               | specified.               |
```

### 24.1 What did NOT change in rev-2

- The parity taxonomy (§5) and the operation matrix (§6) structure are
  unchanged; only the F2 rows and the F5 classification were corrected.
- F1's CORE divergence (an externally-produced Evented Future has no producer-
  side Scheduler notification, and Evented Group::await interprets a Drain
  no-progress return as permission to return) is CONFIRMED and unchanged
  (review A1). Only the correction architecture and the contract framing
  changed.
- F3 (init_fiber failure silently discarded) is CONFIRMED and unchanged
  (review A2). The preparation no longer pre-selects std::runtime_error as
  unquestionably authoritative; the accepted error boundary (exception /
  fail-fast / compile-time exclusion / existing convention) is left to the
  implementation task.
- F4 (size() divergence) is CONFIRMED and unchanged (review A3). The public
  size() wording is acknowledged as less explicit than the await-ALL contract,
  so D-E14-3 is a legitimate contract-clarification decision (unlike the
  rev-1 framing of D-E14-1).
- F6 (documentation drift) is CONFIRMED for the sampled claims (review A4);
  the full list should still be rechecked location by location when revised.
- No forbidden scope expansion (Executor, P2300, Awaitable, actor runtime,
  new sync base, new CancelToken, wait-all, networking — review §7) was
  introduced.

### 24.2 Rev-3 ledger (rev-2 → rev-3, in response to `E14-PREPARATION-REV2-REVIEW: FAIL`)

```text
REV-2 FINDING        | REV-2 STATE              | REV-3 CHANGE (this document)
---------------------+--------------------------+------------------------------
R3-1 (document       | rev-2 document was       | Verified on disk: the rev-2
completeness /       | reported truncated at    | document was NOT truncated at
truncation)          | "The mechani" inside     | "The mechani"; §22, §23, §24,
                     | D-E14-1.                 | and Cross-links were all
                     |                          | present and the file ended with
                     |                          | a complete final newline. No
                     |                          | content restoration was needed;
                     |                          | this row records the honest
                     |                          | verification (the reported
                     |                          | truncation did not reproduce).
                     |                          | Code-fence count is balanced
                     |                          | (164 = 82 pairs) post-rev-3.
---------------------+--------------------------+------------------------------
R3-2 (T12 destructor | rev-2 T12 said "the      | T12 PRIMARY TRACE rewritten to
path wrong)          | Fiber registration in    | the actual destructor path:
                     | waiting_ready_ is        | ~Group has NO sched_ branch and
                     | destroyed with the       | unconditionally awaits every
                     | Scheduler's maps. Then   | task Future (group.cpp:89); an
                     | ~Group runs: releases    | Evented Future's await dispatches
                     | Fiber/stack".            | to EventedWaitPolicy, which holds
                     |                          | a BORROWED Scheduler& (now
                     |                          | dangling) and dereferences it in
                     |                          | wait_until_ready -> await_ready_flag
                     |                          | -> g_worker/current. This is
                     |                          | reached from the ~Group BODY, before
                     |                          | any Fiber/stack member destruction.
                     |                          | §14 T12 updated.
---------------------+--------------------------+------------------------------
R3-3 (F2b mechanics) | rev-2 claimed the        | F2b rewritten at §9.1, T11
                     | waiting_ready_ map's own | RESIDUAL TRACE, and §16 F2b: the
                     | destruction dereferences | map's own destruction destroys
                     | the stored Fiber*        | pointer VALUES only — it is NOT a
                     | ("the map destructor     | dereference site. The actual
                     | would dereference freed  | deref is a later
                     | Fiber*").                | wake_ready_flags_locked(), which
                     |                          | first reads the dangling
                     |                          | &Future::ready_ key (Futures
                     |                          | destroyed at end of ~Group body)
                     |                          | and then the dangling Fiber
                     |                          | (evented_fibers_ freed by member
                     |                          | destruction).
---------------------+--------------------------+------------------------------
R3-4 (F1 formal      | rev-2 asserted an        | §18 F1 decision changed to
decision             | unconditional            | PROVISIONAL_NO_FORMAL_CHANGE. It
over-claimed)        | NO_FORMAL_CHANGE and     | stays NO_FORMAL_CHANGE ONLY IF the
                     | "no new state is         | chosen D-E14-1 mechanism introduces
                     | required", while         | no distinct per-Future/per-
                     | D-E14-1 (T-WAKE-1..8)    | registration attachment state and
                     | was undecided.           | refines to existing E9 transitions;
                     |                          | it MUST be upgraded to
                     |                          | EXTEND_EXISTING_MODEL otherwise.
                     |                          | T-WAKE-1..8 remain undecided.
                     |                          | §16 F1, §21, §23, B4 banner, and the
                     |                          | B4 revision-ledger row updated.
---------------------+--------------------------+------------------------------
R3-5 (F5 gating /    | rev-2 recommended an     | F5a smallest-correction and
Threaded             | unconditional            | D-E14-2 rewritten to require a
portability)         | static_assert(fiber_ctx:: | portability proof: Threaded stays
                     | supported) without        | compilable/functional; Evented is
                     | qualifying WHERE it may   | excluded/unavailable/deleted or
                     | fire.                    | fails cleanly; a shared-header
                     |                          | include does NOT disable Threaded.
                     |                          | fiber_ctx::supported is described
                     |                          | as a capability PREDICATE, not a
                     |                          | gate, until an admission/build
                     |                          | mechanism enforces it. §13.3, §16
                     |                          | F5a/F5-obs, §20 P4, §22.1 D-E14-2
                     |                          | updated.
---------------------+--------------------------+------------------------------
R3-6 (test-ID        | RT-F5b was defined       | RT-F3 = init_fiber(false) reported
consistency)         | inconsistently as        | BEFORE spawn/enqueue; RT-F5a =
                     | "init_fiber failure      | unsupported-target Evented public
                     | propagation",            | admission enforces the chosen gate;
                     | conflicting with RT-F3   | RT-F5b = unsupported-target CI/test
                     | and with the F5b         | PROVES fail/disable (no silent
                     | finding.                 | early-return). §15.4, §20 P2, §21
                     |                          | matrix rows updated to use these
                     |                          | meanings consistently.
---------------------+--------------------------+------------------------------
R3-7 (consistency    | n/a (the review action)  | Ran stale-claim scans: no surviving
checks)              |                          | map-destructor-dereference claim,
                     |                          | no conflicting RT-F5b definition,
                     |                          | no unconditional F1 NO_FORMAL_CHANGE
                     |                          | claim outside the historical
                     |                          | rev-1/rev-2 narrative. Surviving
                     |                          | mentions of the rev-1/rev-2 errors
                     |                          | are explicitly marked as corrected
                     |                          | history (revision ledger, T12 rev-3
                     |                          | header, B4 narrative).
```

The final verdict remains `E14-PREPARATION: NOT-READY` until D-E14-1
mechanism, F2a teardown policy, F5 admission mechanism, and D-E14-3 size
semantics are resolved and an independent rev-3 review passes.

### 24.3 Artifact-integrity proof (rev-3+1, in response to `B-REV3-1`)

The rev-3 independent review returned `E14-PREPARATION-REV3-REVIEW: FAIL`
for two reasons; the technical revisions were accepted, but the reviewer
received a PHYSICALLY TRUNCATED copy of this document (ending mid-§19.2 at
`F3 is`, 2549 lines / 131 fences / no trailing newline). The on-disk file
was complete; the truncation occurred in the upload/transfer path. This
subsection records the on-disk measurements so any future reviewer can
verify the artifact they actually received.

On-disk measurements of this file (post rev-3+1 edits). NOTE: exact byte /
line / fence COUNTS and the sha256 are intentionally NOT hard-coded in this
subsection, because writing them into the file changes the file's own
measurements (self-reference). The trustworthy artifact credential is the
sha256 captured EXTERNALLY at upload time and recorded in the upload
completion report. What this subsection fixes is the STRUCTURAL
completeness contract, which is stable:

```text
structure    : begins with "# E14 — Threaded vs Evented Semantic Parity...",
               contains §1 .. §24 (incl. §24.2 rev-3 ledger and §24.3
               this subsection), and ends with a "## Cross-links" section
               whose final bullet references
               docs/async-runtime-construction-method.md
final byte   : 0x0a (single trailing newline)
code fences  : balanced (every opening fence line has a matching closing
               fence line; total fence-line count is even)
final verdict (§23): "E14-PREPARATION: NOT-READY"
```

Required sections that MUST be present in any copy submitted for review
(absence => truncation => FAIL on artifact-integrity grounds):

```text
§20  Phased production implementation plan   (P1..P7)
§21  Verification matrix
§22  Residual risks and explicit deferrals   (§22.1 D-E14-1, D-E14-F2a,
                                              D-E14-2, D-E14-3; §22.2, §22.3)
§23  Final readiness verdict                 (E14-PREPARATION: NOT-READY)
§24  Revision ledger                         (§24.1, §24.2 rev-3 ledger,
                                              §24.3 this subsection,
                                              §24.4 rev-3+1 ledger)
Cross-links section                         (header "## Cross-links")
```

Reviewer integrity check (per the rev-3 Review Request §2.1). (Counting
fences with a literal triple-backtick pattern inside a fenced block is
itself ambiguous in Markdown, so the check is phrased without embedding
that literal.)

```text
# 1. fence balance: count of triple-backtick lines must be EVEN
# 2. tail must show §24.3 and the Cross-links section
tail -n 80 <this-file>
# 3. the §23 verdict must be present
grep -c 'E14-PREPARATION: NOT-READY' <this-file>   # >= 1
# 4. digest: compare to the value in the upload completion report
sha256sum <this-file>
```

If the received copy is truncated, fence-imbalanced, or missing §20–§24 +
Cross-links, the correct review outcome is
`E14-PREPARATION-REVIEW: FAIL` on artifact-integrity grounds (request a
clean re-upload of the on-disk file), NOT a substantive re-review of
production code.

### 24.4 Rev-3+1 ledger (in response to `E14-PREPARATION-REV3-REVIEW: PASS-WITH-OBSERVATIONS`)

The rev-3+1 independent review returned PASS-WITH-OBSERVATIONS: all
technical content and artifact integrity PASS; three mechanical
observations (O1–O3) required fixing before the implementation prompt is
generated. No full re-review was required after these fixes (only a
grep/diff sanity check). The fixes:

```text
OBS   | REV-3+1 STATE            | REV-3+1 FIX (this subsection round)
------+--------------------------+----------------------------------------------
O1    | Review Request §5 item 7 | Review Request §5 item 7 rewritten to NOT
hard- | hard-coded "3240 lines / | hard-code line/fence counts; it now points
coded | 166 fences", which would | to §2.1 (structural check + externally-
counts| be a perpetually-failing | captured digest). No surviving hardcoded
      | acceptance criterion.    | line/fence counts in the Review Request.
------+--------------------------+----------------------------------------------
O2    | F2a teardown policy was  | New decision D-E14-F2a added to §22.1
no F2a | described in §16 F2 F2a  | (between D-E14-1 and D-E14-2; D-E14-2 and
decision| but had no formal ID in  | D-E14-3 are NOT renumbered). Threaded into
ID    | §22.1, so it could not   | the §22.1 intro (four blocking decisions),
      | be referenced by the     | §23 C-list (now C1–C5: C2 = D-E14-F2a), and
      | implementation prompt.   | the Review Request. The F2b closure
      |                          | condition is bound to D-E14-F2a.
------+--------------------------+----------------------------------------------
O3    | Several spots referenced | Stale forward-looking "rev-2 review signs
stale | a future "rev-2          | off" wording replaced with "independent
rev-2 | independent review" even | review of the complete preparation
round | though rev-3+1 is the    | artifact" (banner §0; §23 intro now records
name  | round under review.      | all four review outcomes incl. rev-3+1
      |                          | PASS-WITH-OBSERVATIONS; §24 header/intro now
      |                          | spans rev-1 → rev-2 → rev-3 → rev-3+1; §23
      |                          | C4 "INDEPENDENT REV-2 RE-REVIEW" replaced by
      |                          | C5 "INDEPENDENT REVIEW of the complete
      |                          | artifact"). Factual past-tense mentions of
      |                          | the rev-2 FAIL (history) are retained.
```

The final verdict remains `E14-PREPARATION: NOT-READY` until D-E14-1,
D-E14-F2a, D-E14-2, and D-E14-3 are resolved by accepted authority AND an
independent review of the complete preparation artifact signs off (C5).
Per the rev-3+1 review, a fresh full re-review is NOT required after these
O1–O3 fixes.

---

## Implementation Decision Resolution (frozen by E14-IMPLEMENTATION-1)

The four blocking decisions are resolved as follows (implementation authority:
E14-THREADED-EVENTED-SEMANTIC-PARITY-IMPLEMENTATION-1):

```text
D-E14-1   RESOLVED: M1 existing E9 wake protocol.
           EventedWaitPolicy owns one SchedulerWakeHandle (policy-level).
           Future::complete_with routes notification via WaitPolicy::notify_ready().
           Group::await uses run_live drive until all task Futures terminal.
           NO_FORMAL_CHANGE.

D-E14-F2a RESOLVED: fail-fast (policy ii).
           DEBUG: assert at ~Group if any Evented task Future pending.
           RELEASE: std::terminate via named helper (group_lifetime_fail_fast).
           No hidden drain, no implicit detach, no blocking await in destructor.

D-E14-2   RESOLVED: construction-time fail-fast via require_evented_supported().
           Optimized no-op on supported targets; fail-fast at Evented admission
           boundary on unsupported targets. No static_assert in shared headers.

D-E14-3   RESOLVED: parity with Threaded.
           After successful Evented Group::await, size() == 0.
           Futures reaped; Fiber/stack storage reclaimed; repeated await idempotent.
```

---

## Implementation Closeout (E14-IMPLEMENTATION-1, 2026-07-26)

All four frozen decisions are implemented and verified. Review follow-up
(5661f19) added Group-scoped Live termination, complete F5 admission, and
real RT-F3 regression tests.

| Finding | Production change | Files |
|---------|------------------|-------|
| F1 (external-producer wake) | `EventedWaitPolicy` owns `SchedulerWakeHandle`; `WaitPolicy::notify_ready()` virtual; `Future::complete_with` calls `policy_->notify_ready()` after first terminal publication; `Group::await` uses Group-scoped `run_live(1, stop_fn, ctx)` drive | `evented_wait_policy.hpp`, `wait_policy.hpp`, `future.hpp`, `group.cpp`, `scheduler.cpp` |
| F2a (destructor fail-fast) | `~Group` Evented branch: pending Future → `group_lifetime_fail_fast()` | `group.cpp`, `fail_fast.hpp`, `fail_fast.cpp` |
| F3 (init_fiber failure) | `Group::async_evented` throws `std::runtime_error` on `init_fiber` failure; internal-testing seam for real regression | `group.hpp`, `scheduler.cpp` |
| F4 (post-await reaping) | `Group::await` Evented path reaps `futures_/evented_fibers_/evented_stacks_` when all terminal | `group.cpp` |
| F5 (admission guard) | `Scheduler(AsyncIoContext&)` and `Group(Scheduler&)` call `require_evented_supported(evented_admission_check())` | `scheduler.cpp`, `group.cpp`, `fail_fast.hpp`, `fail_fast.cpp` |

Verification gates (post review follow-up):

- Clang Debug: 105/105 tests passed
- Clang Release: 105/105 tests passed
- ASan/UBSan: 105/105 tests passed
- TSan: 105/105 tests passed (lock-order inversion fixed)

---

## Cross-links

- Implementation closeout: E14-IMPLEMENTATION-1 (2026-07-26) + review
  follow-up (5661f19). All four decisions (D-E14-1, D-E14-F2a, D-E14-2,
  D-E14-3) implemented and verified. Gates: Clang Debug 105/105, Release
  105/105, ASan/UBSan 105/105, TSan 105/105.
- Authority: `AGENTS.md`, `docs/adr/ADR-execution-model.md`,
  `docs/adr/ADR-async-io-model.md`, `docs/history/implementation-plans/async-runtime-plan.md`,
  `docs/history/implementation-plans/async-runtime-construction-method.md`,
  `docs/history/closeout/e10-e12-api-semantic-closure.md`,
  `docs/history/closeout/e12-cross-primitive-terminal-audit.md`,
  `docs/history/implementation-plans/e13-select-production-architecture.md`.
- Review request: `docs/history/reviews/E14-THREADED-EVENTED-SEMANTIC-PARITY-PREPARATION-1-REVIEW-REQUEST.md`.
- Closed E12-G audit: `docs/history/closeout/e12-cross-primitive-terminal-audit.md`.
- Closed E12 semantic closure: `docs/history/closeout/e10-e12-api-semantic-closure.md`.
- E13 master: `docs/history/implementation-plans/e13-select-production-architecture.md`.
- Construction method (normative M1–M9): `docs/history/implementation-plans/async-runtime-construction-method.md`.
