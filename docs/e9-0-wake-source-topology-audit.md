# E9-0 — Wake-Source Topology Audit

Narrow audit of every current source that can make progress or executable
work appear, and every place a Scheduler Worker can stop actively observing
state. This is the input to the E9 architecture decision
(`docs/adr/ADR-execution-model.md` §9.4) and the TLA+ park/wake models
(`docs/spec/e9_park_wake/`).

All evidence below is read from actual production source (HEAD
`1ca5d73`). Nothing is inferred from the roadmap.

---

## 0. Pre-gate

```
HEAD:    1ca5d73db0dce1dc557b381e350b77d2212eb882
command: scripts/verify-e8-stability.sh release e7_coord_test "" 500
result:  500/500 PASS
```

E9 proceeds.

---

## 1. Wake-source topology (E9 spec §3)

For each source: who writes the persistent state, whether the producer
knows `Scheduler`, whether/which Worker it notifies today, and whether the
signal can occur before/while parked / after `run()` returns.

Key cross-cutting facts established from source:
- `Scheduler` has **no public wake API** today. The only Worker
  notification primitive is `WorkerState::inbox_cv` (`scheduler.hpp:48`),
  notified only by `route_runnable` / `route_runnable_locked` / `spawn` /
  `spawn_on` / `try_steal` — all of which run **inside a Scheduler Worker
  thread** (or are called by the test before `run()`).
- Persistent readiness is observed **only by polling** inside
  `wake_ready_completions_locked` (`scheduler.cpp:454`) and
  `wake_ready_flags_locked` (`scheduler.cpp:492`). Nothing wakes the loop
  when readiness is published from outside a Worker.
- `Future::complete_with` (`future.hpp:66`) sets `ready_` and notifies its
  **own** `cv_` — the Future's internal Threaded cv, not any Scheduler
  primitive. Under the Evented policy the Future's cv is irrelevant
  (`evented_wait_policy.hpp:54` ignores `mtx`/`cv`).

| Source ID | producer context | persistent state written | current notification | can occur while all Scheduler Workers idle? |
| --------- | ---------------- | ------------------------ | -------------------- | ------------------------------------------- |
| **W1** | Scheduler Worker, via `spawn`/`spawn_on` | `Fiber` created→runnable; push to `local_runnable`/`pending_spawn_`; `fiber_owner_` set | `owner->inbox_cv.notify_one()` (`scheduler.cpp:75,101,149`) | **No** — runs on a Worker thread; a Worker is by definition not idle while executing it |
| **W2** | Scheduler Worker, via `route_runnable(_locked)` after a poll/wake drain | waiting→runnable; push to `owner->local_runnable` | `owner->inbox_cv.notify_one()` (`scheduler.cpp:448,529`) | **No** — runs inside `wake_ready_*_locked` under `global_mtx_`, i.e. a Worker is mid-loop |
| **W3** | `Scheduler::spawn` while `!workers_.empty()` (during a coordinated run) | created→runnable; push to owner's `local_runnable` | `inbox_cv.notify_one()` (`scheduler.cpp:75`) | **No** in practice — `spawn` from a Worker thread. (If an *external* thread called `spawn` mid-run it would race `global_mtx_`/`next_spawn_worker_`/`fiber_owner_` — but that is an E7-external-caller violation, ADR §9.2.8, explicitly out of E7 scope. E9 §18 must decide.) |
| **W4** | `FakeAsyncBackend` worker? **None.** `poll()`/`wait_one()` apply staged results synchronously on the caller thread. | `Completion` outstanding→ready inside `poll()` (`fake_backend.hpp:114`) | **None** — the test stages results; the Scheduler Worker itself polls. No producer thread exists. | N/A — no producer thread; readiness is observed only when a Worker polls. If all Workers park without polling, Fake readiness stays unobserved. |
| **W5** | `ThreadPoolBackend` OS worker thread (one per op, `threadpool_backend.cpp:74`) | `Completion` outstanding→ready inside `poll()` (after the worker pushes a `ReadySize`/`ReadyVoid` onto the ready deque) | Backend-internal `cv_.notify_one()` (`threadpool_backend.cpp:80`). **Does NOT notify any Scheduler Worker.** | **Yes** — a ThreadPool worker thread completes an op while all Scheduler Workers are idle/parked. The ready deque gains an entry but nothing wakes a parked Scheduler Worker. |
| **W6** | Linux kernel → io_uring CQE | `Completion` outstanding→ready inside `reap_ready` (`uring_backend.cpp:226`) | **None** to the Scheduler. `io_uring_submit_and_wait` blocks the calling Worker in the kernel (`uring_backend.cpp:414`); a CQE only unblocks that *one* Worker. | **Yes** — a CQE may become available while other Scheduler Workers are idle/parked. But it is only observable from `wait_one`/`poll`, which requires a Worker to call into the backend. |
| **W7** | Scheduler Worker, via `Future::complete_with` (Evented policy) | `Future::ready_ = true` (`future.hpp:71`); also backend-ready if composed | Future's own `cv_.notify_all()` (`future.hpp:73`) — ignored by Evented policy. No Scheduler notification. | **No** — runs on a Worker thread (same as W1). The Scheduler observes `ready_` on its next `wake_ready_flags_locked` poll of the same loop. |
| **W8** | **External OS thread**, via `Future::complete_with` | `Future::ready_ = true` (`future.hpp:71`) | Future's own `cv_.notify_all()` (`future.hpp:73`) — **ignored by Evented policy**. **No Scheduler notification whatsoever.** | **Yes** — and this is the load-bearing E9 hazard. If the registered wait is `waiting_ready_`, the external thread sets the flag but no Scheduler Worker is told to re-poll. |
| **W9** | Coordinated run termination (`global_terminate_` set in `worker_loop`, `scheduler.cpp:357,399`) | `global_terminate_ = true` | `inbox_cv.notify_all()` on every Worker (`scheduler.cpp:359,401`) | Yes, by design — but only from inside a Worker loop. An *external* shutdown signal does not exist. |

### Per-source answers (E9 spec §3 questions)

```
W1 (runnable publication, Worker):
    Who writes persistent state?  the Worker thread (spawn).
    Does producer know Scheduler? yes (it IS a Scheduler Worker).
    Currently notify a Worker?   yes — inbox_cv.notify_one on the owner.
    Which primitive?             WorkerState::inbox_cv.
    Before park / while parked / after run()?
        before park: YES (Worker is active).
        while parked: NO (a Worker cannot spawn while parked).
        after run(): NO (run() returned; no Workers exist).

W2 (cross-worker route, Worker):
    same as W1 — runs inside wake_ready_*_locked under global_mtx_.
    before park: YES. while parked: NO. after run(): NO.

W3 (spawn mid-run):
    Producer = whoever calls Scheduler::spawn. In-well-formed-use that is a
    Worker or the caller before run(); notification = inbox_cv.notify_one.
    External-thread spawn is an E7 §9.2.8 out-of-scope case; E9 §18 decides
    whether to support it. Either way the notification primitive is
    inbox_cv, which a parked Worker DOES wait on (see §2 boundary B5).
    before park: YES. while parked: YES (inbox_cv can wake it). after run(): NO.

W4 (Fake backend):
    No producer thread. Persistent state (Completion ready) appears only when
    a Worker calls poll()/wait_one(). There is no signal to "happen".
    N/A for the before/while/after question — readiness is observed, not
    signalled.

W5 (ThreadPool backend):
    Who writes persistent state? ThreadPool worker thread, into the backend's
    own ready deque; the Completion is marked ready later by poll().
    Does producer know Scheduler? NO — ThreadPoolBackend has no Scheduler ref.
    Currently notify a Worker?   NO — only its own backend cv_.
    Which primitive?             ThreadPoolBackend::cv_ (unrelated to inbox_cv).
    Before park / while parked / after run()?
        before park: observable on next poll.
        while parked: the ready deque grows; no Scheduler Worker is woken. The
                      ONLY path a parked Worker revisits poll is the 1ms
                      inbox_cv.wait_for timeout (see §2 B5) — i.e. periodic
                      polling, not a signal. THIS IS THE MIXED-WAKE / W5 GAP.
        after run(): the entry sits in the deque until the next run(); the
                     Completion is not yet ready, so ctx_.outstanding()>0.

W6 (io_uring CQE):
    Who writes persistent state? the kernel; observed via reap_ready.
    Does producer know Scheduler? NO.
    Currently notify a Worker?   Only the single Worker blocked in
                                 io_uring_submit_and_wait (kernel wakes it).
                                 Other parked Workers: NO.
    Before park / while parked / after run()?
        before park: YES (next poll/wait_one).
        while parked: only the wait_one participant. Other parked Workers: NO.
        after run(): CQEs remain in the ring until the next run().

W7 (Future::complete_with from a Worker):
    Producer = Scheduler Worker. Sets Future::ready_. Notifies Future cv_
    (ignored by Evented). Scheduler observes ready_ on next
    wake_ready_flags_locked poll of the SAME loop. No cross-loop signal.
    before park: YES. while parked: NO (no signal; needs poll). after run(): NO.

W8 (Future::complete_with from external thread):
    Producer = external OS thread. Sets Future::ready_. Notifies Future cv_
    (ignored by Evented). NO Scheduler notification of any kind.
    before park: the flag may be set before any Worker observes it; the
                 double-check in await_ready_flag (scheduler.cpp:599) catches
                 the pre-switch race, but NOT the post-park case.
    while parked: YES — and this is the load-bearing lost-wake case. Flag is
                  set, nothing tells a parked Worker to re-poll.
    after run(): YES — flag is set but run() has returned; caller-driven
                 re-entry is the only recovery (forbidden by E9-T1).

W9 (shutdown):
    Only internal termination exists today. External shutdown signal is not
    modelled. A parked Worker is woken by inbox_cv.notify_all on termination.
    before park: YES. while parked: YES (notify_all). after run(): N/A.
```

**Topological conclusion:** two classes of source can publish readiness
while **all** Scheduler Workers are idle:

1. **External-thread persistent-flag publication (W8)** — sets
   `Future::ready_` with zero Scheduler notification. This is the core E9
   external-wake hazard.
2. **Backend completion from a ThreadPool worker thread (W5)** — appends to
   the backend's ready deque with zero Scheduler notification. This is the
   MIXED-WAKE hazard (§5).

Both reduce, today, to "a parked Worker only notices via the 1ms
`inbox_cv.wait_for` timeout" — i.e. **periodic polling**, which E9 §6
explicitly forbids choosing as a primary strategy.

---

## 2. Idle / blocking boundary audit (E9 spec §4)

Every place a Scheduler Worker can stop actively observing state:

| Boundary | location | blocks OS thread? | wake source | predicate | may return run()? |
| -------- | -------- | ----------------- | ----------- | --------- | ----------------- |
| **B1** | `ctx_.wait_one()` (MW-S2 committed participant, `scheduler.cpp:332`) | **Yes** — ThreadPool: `cv_.wait` (`threadpool_backend.cpp:159`); io_uring: `io_uring_submit_and_wait` kernel block (`uring_backend.cpp:414`); Fake: returns immediately | backend `cv_` / kernel CQE / none | (ThreadPool) `!ready_size_.empty() \|\| !ready_void_.empty()` | Yes — on `!made_progress` sets `global_terminate_` (`scheduler.cpp:357`) |
| **B2** | `WorkerState::inbox_cv.wait_for(..., 1ms)` (idle park, `scheduler.cpp:423`) | **Yes** — up to 1ms | `inbox_cv` (notify_one from route/spawn/steal) **+ 1ms timeout** | `!local_runnable.empty() \|\| global_terminate_` | Yes — `global_terminate_` predicate |
| **B3** | MW-S2 admission seam (`admission_seam_cv_.wait`, `scheduler.cpp:301`) | **Yes** — test-only | `admission_seam_cv_` (test releases it) | `!admission_seam_armed_` | No (test seam) |
| **B4** | MW-S3 / quiescent idle barrier — the all-idle recheck (`scheduler.cpp:380-416`) | No (atomic counter + global_mtx_) | re-check under `global_mtx_`; on all-idle sets `global_terminate_` and `inbox_cv.notify_all` | `idle_workers_+1 >= workers_.size()` and `classify_locked()` not MW-S1/S2 | Yes — `global_terminate_` set, then `break` |
| **B5** | `worker_loop` fall-through park (same physical wait as B2) | **Yes** | as B2 | as B2 | as B2 |
| **B6** | `run()` termination (`scheduler.cpp:191` — threads joined) | n/a | n/a | n/a | n/a (this *is* the return) |

### Boundary notes

- **The only true indefinite block is B1** (`ctx_.wait_one`). Its wake
  source is **the backend only**. An external Future completion (W8) cannot
  wake it. A Fake-backend `wait_one` doesn't block at all (returns
  `poll()` immediately), so Fake never strands; **ThreadPool and io_uring
  do strand**.
- **B2/B5** is a *timed* park (1ms). It is the de-facto periodic-poll loop
  today: every 1ms a Worker re-loops, re-polls `wake_ready_*_locked`, and
  thus eventually notices W4/W5/W7/W8 readiness. This is exactly the
  "poll every ~1ms" strategy E9 §6 P4 warns against. It is the current
  *workaround* hiding the E9 gap; E9 must replace it with a real wake
  protocol, not bless it.
- **B4** terminates the run on all-idle + MW-S3/quiescent. Note: an
  external-thread flag publication (W8) that happens *after* B4 fires and
  `run()` returns is unrecoverable without caller re-entry — E9-T1 forbids
  caller re-entry.

### P0-Q1 .. P0-Q5

```
P0-Q1: Can a Worker currently block in ctx_.wait_one() while an external
       Future may become ready?
  YES. B1 (ThreadPool wait_one) and B1 (io_uring wait_one) both block the
  single MW-S2 participant in a backend-only wait. If an external thread
  completes a Future registered in waiting_ready_ (W8), the wait_one
  participant is not woken (the backend cv/kernel CQE is the only wake
  source). The other Workers, if any, are in B2/B5 (timed park, 1ms).

P0-Q2: If yes, can that external Future wake ctx_.wait_one()?
  NO. Future::complete_with notifies only the Future's own cv_, which the
  Evented policy ignores (evented_wait_policy.hpp:54). There is no path
  from Future::complete_with to either ThreadPoolBackend::cv_ or the
  io_uring ring. The wake set of B1 excludes external-ready.

P0-Q3: Can all Workers currently return/terminate while a persistent
       ready-flag wait remains registered?
  YES. classify_locked (scheduler.cpp:535) returns MW-S3 when
  any_wait is true and ctx_.outstanding()==0. The all-idle recheck (B4)
  terminates the run on MW-S3/quiescent (scheduler.cpp:395). So if an
  Evented Future await registered a flag and no backend op is outstanding,
  the run terminates with the wait still registered. (This is the accepted
  E7 MW-S3 semantics — "E7 is not required to make progress from MW-S3."
  E9 must change this for the external-wake case.)

P0-Q4: Can an external producer subsequently set that flag ready without
       caller-driven Scheduler re-entry?
  NO. After run() returns, no Worker is polling. The external thread sets
  Future::ready_ = true, but nothing observes it until the caller re-enters
  run() and a Worker polls wake_ready_flags_locked. E9-T1 requires this to
  work WITHOUT caller re-entry — so E9 must keep at least one Worker
  polling/parked-and-wakeable while an external-wake-capable wait is
  registered.

P0-Q5: Does any existing condition variable / worker notification close
       this path?
  NO. inbox_cv is notified only by route/spawn/steal (all Worker-internal).
  The Future cv_ is ignored by Evented. The ThreadPool cv_ and io_uring
  ring are backend-only. Nothing connects an external-thread flag
  publication to a Scheduler Worker wake.
```

**P0 verdict:** the current system has a real E9 external-wake liveness
gap. It is masked only by (a) the 1ms timed park (B2/B5 periodic polling)
and (b) caller-driven run() re-entry. Both are forbidden as primary
strategies by the E9 brief.

---

## 3. MIXED-WAKE analysis (E9 spec §5)

### 3.1 The state

```
MIXED-WAKE state:
    runnableVisible    = FALSE
    runningVisible     = FALSE
    backendOutstanding = TRUE      (a ThreadPool/io_uring op is in flight)
    externalWaitRegistered = TRUE  (a Future await is in waiting_ready_)
```

`classify_locked` (`scheduler.cpp:535`) classifies this as **MW-S2**
because `ctx_.outstanding() > 0` is checked before the wait-registration
check. So under the current protocol the elected participant enters
`ctx_.wait_one()` (B1).

### 3.2 The causal failure

```
T0  Fiber A (on Worker W0) awaits an Evented Future.
    await_ready_flag registers {A, W0} in waiting_ready_, switches out.
    No runnable/running Fiber remains.

T1  A backend op is outstanding (e.g. a ThreadPool pread in flight, or an
    io_uring SQE submitted). backendOutstanding = TRUE.

T2  classify_locked == MW-S2. Worker W0 (single-worker run, or the lowest-id
    idle Worker) is elected, two-phase-admitted, commits, enters
    ctx_.wait_one(). It blocks in ThreadPoolBackend::cv_.wait (B1) or
    io_uring_submit_and_wait.

T3  An external OS thread completes the Future: Future::complete_with sets
    ready_ = true, notifies the Future's cv_ (ignored by Evented).
    externalReady = TRUE. No Scheduler Worker is notified.

T4  The backend op is SLOW (large pread, contested fsync, etc.) and has not
    completed.

RESULT:
    externalReady = TRUE (Fiber A could be made runnable NOW)
    Fiber A is still Waiting
    the single effective observer (W0) is blocked ONLY on backend progress
    external wake cannot interrupt the backend-only wait
    recovery is: backend eventually completes (assumed away by E9 §5), OR
                 the 1ms park timeout on another Worker (if N>=2 and that
                 Worker happens to be in B2/B5), OR caller re-entry.
```

For a **single-Worker run (N=1)** this is a hard stall until the backend
completes: the only Worker is the wait_one participant, and there is no
other Worker whose 1ms timeout would re-poll. Even for N>=2 it is a
latency bug (up to 1ms) dressed as correctness.

### 3.3 Why "backend eventually completes" does not close it

E9 §5 is explicit: do not hide MIXED-WAKE by assuming the backend
eventually completes. The property under test is **external-ready wake**.
A backend that is slow (network FS, contended fsync, a paused syscall under
a debugger, a real large read) makes the external-ready wake latency
unbounded and decouples correctness from backend timing. A correct E9 must
make external-ready wake the *authority* for that wait, independent of
backend progress.

### 3.4 Required decision

E9 must select an architecture in which the MW-S2 participant's wake set
**includes external-ready publication**, OR in which a separate parked
Worker covers external-ready wake while the MW-S2 participant covers
backend progress. This is decided in ADR §9.4 (§8.6).

---

## 4. Summary of the E9 gap (before architecture)

Two gaps, one protocol:

1. **External-wake gap (W8).** No path from an external-thread
   `Future::complete_with` to a Scheduler Worker. Closed by introducing a
   Scheduler wake source (wake epoch + cv) that the external producer
   signals, and a park admission protocol that validates the epoch before
   sleeping.

2. **MIXED-WAKE gap (W5 + W8 together).** The MW-S2 `ctx_.wait_one()`
   participant blocks on backend progress only; an external-ready
   publication cannot interrupt it. Closed either by making backend wait
   interruptible by the Scheduler wake source (P5) or by a separate
   external-ready parker (P3). The architecture decision picks one.

3. **The 1ms timed park (B2/B5) is the current workaround** for both. It
   must NOT be retained as the primary strategy (E9 §6 P4); it may remain
   only as defense-in-depth against a lost wake, never as the *authority*.

The formal models (`docs/spec/e9_park_wake/`) and the ADR
(`docs/adr/ADR-execution-model.md` §9.4) decide the protocol; production
(`src/async/scheduler.cpp`, `include/sluice/async/scheduler.hpp`) realizes
it.
