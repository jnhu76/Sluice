# Phase G — backend-ready progress wake integration (focused design)

> **Archived 2026-08-25 (issue #167 Step 5).** Moved from `docs/design/`;
> classification at move: PINNED-EVIDENCE → CLOSED-HISTORY (IMPLEMENTED /
> COMPLETE 2026-08-15, foundation-freeze closeout). The current park/wake
> invariants this design governs — R1–R4, the split-wait bridge, the
> MIXED-WAKE backstop, and the wake-bridge lost-wake closure — remain
> documented in CURRENT authority: `docs/adr/ADR-execution-model.md`
> §9.4/§9.4.7.2, `docs/architecture/foundation-freeze.md`,
> `docs/history/closeout/phase-g-compliance-gate.md`, and the `e9_park_wake` TLA
> model (`spec/tla/e9_park_wake/`). Body preserved as-written; see
> `docs/history/README.md`.

**Status:** IMPLEMENTED / COMPLETE (2026-08-15, closeout branch
closeout-phase-g-foundation-freeze). Evidence of record:
`docs/history/closeout/phase-g-compliance-gate.md` (Gate 4 executed rows),
`tests/phase_g_closeout_test.cpp` (deterministic causal proofs Cases A–D +
TP-G1..G7), `tests/phase_g_closeout_uring_test.cpp` (UR-G1..G7, real
liburing, `mode=real`), and `spec/tla/e9_park_wake/` (R1–R4 + bridge model,
4 positive + 4 negative TLC gates). The ADR amendment is
ADR-execution-model §9.4.7.2.
**Date:** 2026-08-14 (design), 2026-08-15 (implemented + closeout)
**Methodology note (2026-08-18, issue #123):** the closeout test's
observations were migrated from yield-spin + wall-clock deadlines to blocking
handshakes (zero-CPU atomic::wait / controller-cv / ReadyWaitSource epoch
observer) with a single fail-closed case-level watchdog; the TP-G5/D1 progress
baselines are read before the gate release (a baseline-after-release inversion
caused the documented parallel-Debug false failures). Design semantics are
unchanged — see
`docs/history/issues/issue-123-phase-g-closeout-parallel-flake.md`.
**Authority:** ADR-execution-model §9.4.7 / §9.4.7.1 (MIXED-WAKE bounded
observation park; the P5 seam explicitly reserved: "P5 is reserved if P3 proves
insufficient under the formal gate or the load-bearing tests"); AGENTS.md §4.4
(Scheduler authority), §10, §13.1 (lock order), §13.2 (wake obligation),
§14 (shutdown); Architecture Constitution AC-6 (polling must be explicitly
justified).
**Scope:** the backend-ready -> Scheduler wake bridge; the 2ms MIXED-WAKE
observation interval verdict; the wake-domain park timeout policy. F2/F3 and
all other phases are NOT re-opened.

---

## 1. G0 — as-built adversarial audit (source-traced)

### 1.1 ThreadPool progress path (as-built)

```text
worker syscall completes
  -> arena_.record_terminal(h, terminal)          threadpool_backend.cpp:646
       (arena leaf lock; backend-ready + ready-ring push; allocation-free)
  -> signal_ready_progress()                      threadpool_backend.cpp:649
       -> ReadyWaitSource::signal_progress()      ready_wait_source.hpp:134
            (ready_epoch_++ under the ready mutex, then notify_all)
  -> [participant parked in ctx_.wait_one()]      async_io_context.cpp:207
       -> ReadyWaitSource::wait_for_change()      returns progress promptly
  -> participant reaps: arena_.reap(sink)         threadpool_backend.cpp:718
  -> ReadyRoutingSink / ReferenceReadySink        (Phase F1)
  -> Scheduler drain routes the Fiber             scheduler.cpp (drain)
```

- **backend-ready linearization point:** `record_terminal` (arena leaf).
- **who notifies:** the worker itself, via the ready epoch (`signal_progress`).
- **who waits:** a wait_one() observer parked in the BACKEND domain.
- **notification loss / coalescing:** epoch-based; coalescing-safe; no loss.
- **the gap:** when the MW-S2 participant parks on the SCHEDULER domain
  (MIXED-WAKE, `external_wake_possible_locked()` true — a ready-flag, WaitQueue,
  deadline, or select wait is registered), nothing waits on the ready epoch and
  the worker's signal does NOT reach the Scheduler wake source. Backend
  progress is then observed ONLY when the 2ms bounded park expires
  (park_on_wake_source, scheduler.cpp:433-456). **Up to ~2ms latency + a
  periodic CPU tax while the MIXED park is active.**

### 1.2 io_uring progress path (as-built)

```text
kernel CQE
  -> ring fd readable
  -> [participant parked in ctx_.wait_one()]      UringWaitSource::wait_for_change
       poll(ring_fd, control_fd, -1)              uring_wait_source.hpp:270
  -> poll returns POLLIN -> re-poll -> reap_cqes() -> record_terminal
  -> arena_.reap(sink)                            uring_backend.cpp:1434-1443
  -> signal_ready_progress() (n>0)
  -> ReadyRoutingSink -> Scheduler drain routes the Fiber
```

- **backend-ready linearization point:** `record_terminal` inside the CQE
  handler / reap (arena leaf).
- **who notifies:** the KERNEL (ring fd readability); there is no userspace
  thread that runs when a CQE lands.
- **the gap:** identical MIXED-WAKE gap. When the participant parks on the
  SCHEDULER domain, nobody polls the ring fd; a CQE is observed only when the
  2ms bounded park expires. The kernel cannot call a C++ callback — the only
  prompt transport is the ring fd, which only a parked poll observes.

### 1.3 Fake / Sync (reference backends) progress path (as-built)

```text
submit -> staged in the arena ready-ring
  -> poll()/wait_one() (NON-BLOCKING) -> dispatch_and_reap -> ready
```

- Readiness is produced ONLY inside poll/wait_one (ADR A3/O1 "completions only
  inside poll/wait_one"). No wait source, no progress epoch, no event source.
- In MIXED-WAKE the participant parks on the SCHEDULER domain; the 2ms bounded
  observation interval is the ONLY poll driver for reference-backend
  readiness. This is the structural reason the reference backends are
  classified "immediate/reference" (G4) and keep a bounded observation park.

### 1.4 The 2ms MIXED-WAKE verdict (as-built)

```text
CURRENT:
  2ms = correctness authority for MIXED-WAKE backend observation
        (ADR §9.4.7.1 normative: "protocol authority ... NOT defense in depth
        only"; DIV-05 Approved; P2-04 open)
```

- It exists in ONE site: `park_on_wake_source` (`kParkBackstop`,
  scheduler.cpp:433), used by every SCHEDULER-domain park.
- It is the observation-return path ONLY for backend progress; the wake epoch
  is the authority for Scheduler wake publications.
- Removing it today would expose: ThreadPool MIXED backend-ready latency
  (bounded by nothing), Uring MIXED CQE latency (bounded by nothing), and
  Fake/Sync MIXED readiness (never observed) — lost progress, not just
  latency.
- No periodic wake exists when no worker is parked on the wake domain; the
  tax is paid only while a MIXED park is active (backend outstanding + an
  external-wake-capable wait registered).

---

## 2. Design decision — P5-CORRECTIVE (unified split-wait progress participant)

The ADR (§9.4.1) evaluated six wake models. P3 (decoupled wake domains) was
selected for E9, with two explicitly reserved upgrades:

```text
P2 — backend signals the Scheduler wake epoch:
     "ThreadPoolBackend *could* signal, but io_uring cannot" -> REJECTED as
     primary (not uniform).
P5 — unified interruptible backend wait seam:
     "would be sound, but costs a backend-facing seam change" -> RESERVED.
     "io_uring would need an eventfd registered in the ring;
      ThreadPoolBackend would need its cv tied to the Scheduler wake source."
P6 — OS multiplexed wake source (eventfd in io_uring): deferred.
```

Phase G is the roadmap-sanctioned revisit (roadmap: "This is the only phase
that may reclassify DIV-04/DIV-05"; findings P2-04 -> Phase G). The E9-baseline
objection to P5 was that the backend wait seam did not exist; Phase D4
implemented the split-phase `BackendWaitSource` (snapshot / wait_for_change /
interrupt_all / arm_committed_wait) — the seam now exists on both production
backends. **Phase G selects P5-CORRECTIVE: unify the MW-S2 progress park on
the backend wait domain, and bridge the Scheduler wake domain INTO it.**

```text
backend-ready (worker / kernel CQE)
  -> backend wait source epoch / ring fd      (prompt, per-backend transport)
  -> wait_one() returns progress
  -> reap -> ReadyRoutingSink -> Scheduler drain -> Fiber

Scheduler wake publication (routing / flag / select / waitqueue / deadline /
wake handle / control)
  -> signal_wake_locked()
  -> interrupt_backend_waiters()              (THE BRIDGE, one choke point)
  -> parked wait_one() returns interrupted
  -> participant re-drains under G -> routes -> reclassifies -> re-parks
```

No backend knows the Scheduler. No second routing bridge exists: the backend
wait source remains the ONLY backend-progress transport, and the Scheduler
drain remains the ONLY Fiber-routing authority (G-I1, G-I2).

### 2.1 Why not P2 (backend -> Scheduler callback)

- ThreadPool could call a notifier, but io_uring cannot (the kernel produces
  CQEs; no userspace thread runs). A P2 bridge would cover only one production
  backend and leave Uring on the 2ms path — failing UR-G1.
- P5 handles both with the EXISTING split-wait seam (ReadyWaitSource epoch /
  UringWaitSource ring-fd poll), no new notification abstraction.

### 2.2 Why not P6 (fd-multiplexed wake domain)

- P6 converts the Scheduler wake domain (wake_cv_ + epoch) to an eventfd-based
  park. That is a transport change to the E9 park/wake protocol, its TLA+
  models (e9_park_wake), and its load-bearing tests, for no semantic gain over
  P5 — the E9 protocol's epoch/predicate semantics are exactly what P5 reuses.
- P5 confines the change to: the MW-S2 park-domain decision, one bridge call
  in `signal_wake_locked`, and a bounded-park variant of the existing wait
  source API.

### 2.3 MW-S2 park domain selection (new rule)

```text
Let W be the elected MW-S2 participant (E7 two-phase admission COMMITTED).

bounded_park_needed = ready-flag waits registered OR an active deadline
bounded_park_ok     = (no bound needed) OR wait source bounds its parks

IF the backend exposes a split wait source (wait_source() != nullptr)
   AND bounded_park_ok:
    park domain = BACKEND  (ctx_.wait_one()), for BOTH backend-only and
                           MIXED-WAKE. External wake publications reach the
                           park through the interrupt bridge (§3).
ELSE IF the backend has NO split wait source AND no external-wake-capable
     wait is registered:
    park domain = BACKEND  (legacy serialized wait_one — unchanged)
ELSE:
    park domain = SCHEDULER (bounded observation park: MIXED-WAKE for
                           reference/legacy backends, E9 behavior unchanged;
                           and the bounded-capability fallback below).
```

Rationale: with a split wait source, `wait_one()` blocks on the backend's own
progress transport (ThreadPool ready epoch / Uring ring fd) — prompt for
backend progress by construction — and the interrupt bridge makes external
publications observable (the E9 GAP-2 closure, now on the backend domain).
Reference backends have no progress transport; their readiness is poll-driven,
so the bounded Scheduler-domain observation park remains their progress path
(G4 classification).

**Bounded-wait capability (PR #108 review P1b).** The Phase G bounded
overload originally had a base implementation that silently discarded
`max_park` — a third-party wait source implementing only the one-argument
`wait_for_change` was classified split-wait capable, parked in the backend
domain under a deadline expectation, and actually parked indefinitely (an
E11 deadline-liveness hole in the interface contract). The contract is now
explicit:

- `BackendWaitSource::supports_bounded_wait()` (default false) truthfully
  reports whether the bounded overload bounds the physical park; the in-tree
  sources (ReadyWaitSource: `cv.wait_for`; UringWaitSource: poll timeout)
  override it to true;
- `AsyncIoContext::has_bounded_split_wait_capability()` composes split wait
  AND bounded transport; the MW-S2 Phase-B commit uses it in the domain
  decision above — a FINITE cap (active deadline / ready-flag observation) is
  only committed to the backend domain when the capability holds, otherwise
  the park uses the Scheduler wake domain whose cv timeout the Scheduler
  itself bounds;
- `AsyncIoContext::wait_one(max_park)` fails fast (`not_supported`,
  synchronously, no park) when handed a finite cap by a wait source without
  the capability — never a silent unbounded fallback;
- a deadline that becomes active between the Phase-B commit and the max_park
  computation is handled by a defensive clamp (park unbounded; the
  registration's own wake publication crosses the bridge and re-drains into a
  fresh domain decision) so the fail-fast contract is never hit from the
  Scheduler path.

### 2.4 The interrupt bridge

`Scheduler::signal_wake_locked()` is the SINGLE choke point for every
Scheduler wake-domain publication (routing, flag/select/waitqueue resolution,
deadline pump, external wake handle, termination). Phase G appends the
backend-waiter interrupt:

```cpp
void Scheduler::signal_wake_locked() {
    { LockGuard lk(wake_mtx_); ++wake_epoch_; }
    wake_cv_.notify_all();
    if (backend_wait_active_.load(std::memory_order_acquire)) {   // NEW
        ctx_.interrupt_backend_waiters();                          // NEW
    }
}
```

- `interrupt_backend_waiters()` already exists (async_io_context.cpp:331) and
  is the D4 control seam (`interrupt_all` on the backend wait source: control
  epoch bump + wake; no fabricated readiness, no Completion publication, no
  I/O cancellation).
- A parked wait_one() returns `interrupted`; the Scheduler participant
  re-drains under global_mtx_ (routes the external work), reclassifies, and
  re-parks. The D4-RM13 invocation-level control baseline makes the interrupt
  one-shot per wait_one invocation — no shutdown busy-spin.
- `backend_wait_active_` is an optimization gate (an atomic bool set at the
  MW-S2 commit, cleared when wait_one() returns) so the bridge costs one
  atomic load on wake publications that occur while NO backend participant is
  parked. The commit-to-wait_one window is closed by the EXISTING
  `arm_backend_wait_commit()` handshake (D4-RM14), which now also covers the
  MIXED case (the arm already runs whenever the park domain is BACKEND).

### 2.5 Deadline pumping (E11) in the unified park

The MIXED condition includes active deadlines
(`any_active_deadline_locked()` in `external_wake_possible_locked()`), so a
deadline-driven run with outstanding backend I/O parks in wait_one(). The
wait must return in time for the deadline pump:

- `AsyncIoContext::wait_one()` gains a bounded-park overload
  `wait_one(std::chrono::nanoseconds max_park)` (additive; the no-arg form is
  unchanged and unbounded).
- `BackendWaitSource::wait_for_change` gains an additive bounded overload
  `wait_for_change(observed, max_park)`. The base implementation does NOT
  honor the bound — bounded parking is the SEPARATE capability
  `supports_bounded_wait()` (§2.3, PR #108 review P1b): ReadyWaitSource
  (`cv.wait_for`) and UringWaitSource (poll timeout, clamped to poll's int ms
  range) override both; a wait source without the capability receives finite
  caps only as a synchronous `not_supported` from `wait_one(max_park)`, never
  as a silently discarded bound.
- The Scheduler computes `max_park` from the existing lock-free
  `earliest_active_deadline_` cache: no deadline -> unbounded; deadline due ->
  immediate re-drain; test clock mode -> the short test poll bound (E11
  deterministic clock).

### 2.6 Interrupted-no-progress semantics (MW-S2 Phase D)

The backend-domain participant that returns `interrupted` with no reaped
completion must NOT terminate the coordinated run when external-wake-capable
waits remain registered (an interrupt is a re-evaluation signal, not a
no-progress boundary):

```cpp
if (!made_progress) {
    LockGuard lk(global_mtx_);
    if (classify_locked(run_workers) == MwState::mw_s1) continue;
    if (external_wake_possible_locked()) continue;   // NEW: stay resident
    global_terminate_.store(true, ...);              // unchanged: pure control
    ...
}
```

Pure control interrupts (backend-only MW-S2: stop / close) keep the terminate
semantics (driver re-entry on the runtime control epoch). Shutdown convergence
is preserved: request_stop -> interrupt -> re-drain -> re-park; when the
outstanding I/O reaches backend-ready the participant reaps it, reaches MW-S3,
and the stop predicate terminates the run.

### 2.7 Scheduler-domain park timeout policy (the 2ms verdict)

After the MW-S2 MIXED participant moves to the backend domain for split-wait
backends, the SCHEDULER-domain park is used only by:

1. MW-S2 MIXED with a NON-split-wait (reference/legacy) backend — the bounded
   observation interval is their progress path; **the 2ms interval is retained
   here, unchanged** (G4 reference classification);
2. MW-S3 Live parks and idle non-participants — no backend outstanding, so NO
   backend observation is needed; the park timeout becomes deadline-driven
   only (unbounded when no deadline is active) — **the 2ms cap is removed**.

```text
2ms verdict:
  REMOVED as production correctness authority.
  REMOVED as a fixed periodic wake for wake-domain parks.
  RETAINED ONLY as the bounded observation interval for poll-driven
  reference backends (Fake/Sync) in MIXED-WAKE, whose readiness cannot
  self-notify (G4 classification: immediate/reference backend).
```

---

## 3. Wake obligations (AGENTS.md §13.2)

| Persistent state | Producer | Sleeping consumer | Predicate | Commit-to-sleep closure | Worst-case latency |
|---|---|---|---|---|---|
| backend ready epoch / ring fd | terminal winner (worker / kernel CQE) | MW-S2 participant in wait_one (backend domain) | epoch delta / POLLIN | snapshot -> poll -> wait_for_change; epoch predicate; level-triggered fd | one backend progress event |
| backend control epoch (interrupt) | signal_wake_locked bridge / request_stop / close_admission | MW-S2 participant in wait_one | control epoch delta | D4-RM14 arm_committed_wait (commit-to-park window); invocation-level control baseline (D4-RM13) | one wake publication |
| wake epoch | signal_wake_locked | Scheduler-domain parkers | epoch delta / terminate / local_runnable | E9 epoch validation at commit; predicate under wake_mtx_ | one publication |
| runnable ticket | drain / routing | owning worker | local_runnable non-empty | route_runnable_locked push + wake | one routing |

Every producer publishes persistent state BEFORE the wake (I4: state first,
then notify — unchanged in all sites).

---

## 4. Lock protocol (AGENTS.md §13.1)

### 4.1 Lock inventory (additions in bold)

| Lock | Domain | Guards |
|---|---|---|
| `global_mtx_` (G) | Scheduler coordination | runnable routing, admission, classification |
| `wake_mtx_` (W) | wake epoch | `wake_epoch_` + `wake_cv_` |
| **backend wait source mutex (B)** | **split-phase wait** | **ready/control epochs (ReadyWaitSource) or progress/control epochs + eventfd counters (UringWaitSource)** |
| `access_mtx_` (A) | AsyncIoContext | backend serialization |
| arena leaf (L) | RequestArena | slot lifecycle |

### 4.2 Allowed edges (new edges in bold)

```text
G -> A        classify_locked / drain poll (unchanged)
G -> B        arm_backend_wait_commit at the MW-S2 commit (unchanged, now also MIXED)
G -> W        signal_wake_locked under G (unchanged)
G -> W -> B   signal_wake_locked -> interrupt_backend_waiters (NEW; W and B are leaves,
              no reverse edge exists: no wait-source path ever acquires wake_mtx_)
G -> I        routing (unchanged)
A -> B        context wait_one snapshot/poll/wait_for_change (unchanged)
A -> L        poll/reap (unchanged)
L -> (release) -> sink.on_ready (unchanged)
lifecycle -> B  request_stop -> interrupt_backend_waiters (unchanged)
```

**Cycle argument:** W and B are leaves with inbound edges only from G/A; no
path acquires G, A, or W while holding B. The new G -> W -> B edge is
one-directional; the wait-source interrupt is a bounded mutex + epoch bump +
non-blocking notify/eventfd write (no Scheduler call, no user code, no join,
no blocking syscall).

### 4.3 What the bridge MUST NOT do

- Never call the Scheduler from a backend wait source (B -> G forbidden).
- Never publish a Completion, touch a RequestSlot, or route a Fiber from the
  bridge (notification != completion, G-I2).
- Never allocate (the interrupt path is allocation-free; the epoch/eventfd
  protocol has no queue node).

---

## 5. Race analyses (deterministic test plan)

| # | Race | Closure | Test |
|---|---|---|---|
| R1 | backend-ready between participant snapshot and park | epoch predicate (existing) | TP-G1 / UR-G1 |
| R2 | external publication between MW-S2 commit and wait_one entry | D4-RM14 arm_committed_wait (now also MIXED) | TP-G3 / UR-G4 |
| R3 | external publication while parked | interrupt bridge (control epoch) | TP-G3 / UR-G3 |
| R4 | multiple backend-ready coalesce | epoch + drain (reap all) | TP-G2 / UR-G2 |
| R5 | backend-ready vs shutdown interrupt | wait_one final poll (D4-RM13) | TP-G4 / UR-G5 |
| R6 | close_admission while parked | control interrupt one-shot | TP-G5 / UR-G6 |
| R7 | backend-ready notification publishes Completion? | negative: sink/arena untouched | TP-G6 |
| R8 | notification allocation on the accepted terminal path | no-alloc probe | TP-G7 |
| R9 | deadline expiry while parked in wait_one | bounded park (deadline hint) | E11 existing + new |
| R10 | cancel_waiter routes while participant parked in wait_one (single worker) | interrupt bridge | TP-G3 variant |

---

## 6. Change surface

| File | Change |
|---|---|
| `include/sluice/async/async_io_context.hpp` | `wait_one(max_park)` overload; `has_split_wait_capability()` accessor; `BackendWaitSource::wait_for_change(observed, max_park)` additive overload |
| `src/async/async_io_context.cpp` | split-phase wait_one bounded-park plumbing |
| `include/sluice/async/detail/ready_wait_source.hpp` | bounded wait_for_change (`cv.wait_for`) |
| `include/sluice/async/detail/uring_wait_source.hpp` | bounded wait_for_change (poll timeout, INT_MAX clamp) |
| `src/async/scheduler.cpp` / `include/sluice/async/scheduler.hpp` | MW-S2 park-domain rule; `backend_wait_active_` gate + bridge in `signal_wake_locked`; interrupted-no-progress re-park; deadline-driven wake-domain park timeout; wait_one(max_park) call with deadline hint |
| `docs/` | ADR-execution-model §9.4.7 revision (P5-CORRECTIVE), as-built, findings (P2-04), divergence (DIV-04/DIV-05), roadmap, api-reference |
| tests | new `phase_g_backend_progress_wake_test` (+ Uring real-mode variants), mutation evidence, benchmark probe |

## 7. Out of scope

- No new backend->Scheduler coupling, no new notification abstraction, no
  wake-domain transport change (P6), no socket/timer/network API, no
  completion-authority change, no public submit/Completion API change.
- Fake/Sync keep their poll-driven classification; their MIXED observation
  interval is documented, not removed.

---

## 8. G1 forensic finding — park-window invariant violation (G1 BLOCKED)

Status date: 2026-08-14. The clean-rebuild full parallel stress (163 debug
binaries, xargs -P 16, per-binary timeout 300) returned 160 PASS with two
Phase-G-relevant failures:

- `sluice_copy_pipeline_integration_test` — TIMEOUT (live gdb: ALL 4 scheduler
  workers unguarded-parked in `park_on_wake_source` with
  `bounded_backend_observation=false`, NO worker in `ctx_.wait_one`, driver
  joining, app main thread waiting forever);
- `application_runtime_drain_starvation_test` — SIGABRT "terminate called
  without an active exception" (coredump: the abort is a downstream CONSEQUENCE
  — the test destroyed a still-joinable `std::thread` after its bounded wait
  expired on a stalled drain; one worker was unguarded-parked mid-drain).

(`sluice_copy_integration_test` FAIL(1) is a known pre-existing environment
failure, reproduced on the HEAD baseline worktree.)

### 8.1 Instrumentation (this change — test-only, no behavioral fix)

Per the park-window audit directive, `SLUICE_ASYNC_INTERNAL_TESTING`-guarded
instrumentation was added (production layout unchanged). The PR #108 review
(P2a/P2b) hardened the facility itself: every cross-thread diagnostic field is
now an atomic (a "best-effort" racy read is still UB and would be worthless
under sanitizers), and classify evidence is per-worker (a Scheduler-global
last-classify value mis-attributes when another worker classifies between the
parking worker's decision and its park commit):

- a bounded park ledger (ring, 64) recorded at every wake-domain park commit
  in `park_on_wake_source`: park sequence, worker id, wake-epoch baseline,
  backend wait-source (ready, control) generations, backend outstanding,
  registered-wait count, idle/terminate/backend-wait-active state, the
  bounded flag, and the PARKING worker's own classify pair
  (`WorkerState::last_classify` + `classify_seq`);
- per-worker atomic diagnostics: `loop_exit_reason` (enum, written at every
  `worker_loop` break path), `loop_exited` (written at worker_loop RETURN —
  the causal worker-is-dead point), `last_classify`/`classify_seq`, plus
  production `park_domain`/`current` made atomic (read cross-thread by the
  dump);
- `AsyncIoContext::backend_wait_token_for_test()` (wait-source snapshot) and
  `Scheduler::AsyncTestAccess::backend_wait_token` (test-side observation of
  the backend ready publication);
- a `worker_park_returned` causal seam (PhaseTag, controller-driven):
  pauses a worker immediately AFTER its wake-domain park returns, BEFORE the
  loop-top re-drain/classify. Deliberately excluded from
  `release_all_phases` — a terminating sibling's release must not destroy a
  reproducer's hold (the arming test's own watchdog is the escape hatch);
- `Scheduler::dump_park_forensics_for_test()` — watchdog-callable dump of
  live state (wake epoch, token, outstanding, admission, waiting sets,
  running-fiber count, per-worker domain/baseline/inbox/current-fiber/exit
  reason/loop-exited/last-classify) plus the ledger ring;
- `phase_g_g1_stranded_runnable_park_stall_reproducer` — the DETERMINISTIC
  (causal-seam) reproducer that replaced the yield-ordered high-probability
  canary. It first pins the parked configuration (participant worker 0
  `park_domain == Backend` AND survivor worker 1
  `park_domain == Scheduler`, both stable while the read is gated), THEN
  arms the post-park recheck seam and fires request_stop: the survivor's
  park is guaranteed to return through the stop wake and be held at the
  seam while worker 0 dies and the backend completes. Releasing it reaps
  the request and routes the continuation onto the dead worker; a bounded
  drain watchdog proves the stall and exits fail-closed (rc 70). Because
  the watchdog exits BEFORE any teardown (and the caller resets the ready
  Completion before join, ADR Decision 15), the old rc 134 teardown abort
  (`request_arena_invalid_terminal_fail_fast`) and the rc 124
  cleanup-hang no longer compete as exit modes — 10/10 runs exit rc 70
  through the primary stranded-runnable construction (the no-participant
  manifestation, when the task fiber lands on worker 1 via spawn/steal
  racing, fails closed EARLY with its own dump tag, also rc 70).

The deterministic reproducer (2026-08-14, pre-fix; identical structure on
every repetition — see the compliance gate's determinism row) reproduces the
exact §8.2 end state:

```text
FORENSICS: stranded-runnable observed=1 (ready_count=0 w0_runnable=1)
[park-forensics] wake_epoch=5 token=(ready=1,ctrl=2) outstanding=0 admission=none
                idle_workers=0 terminate=0 backend_wait_active=0
[park-forensics] worker id=0 domain=None observed_epoch=0 local_runnable=1
                current_fiber=(nil) fiber_state=- exit_reason=mw_s2_no_progress_terminate
                loop_exited=1 last_classify=1
[park-forensics] worker id=1 domain=SCHEDULER observed_epoch=5 local_runnable=0
                current_fiber=(nil) fiber_state=- exit_reason=(live)
                loop_exited=0 last_classify=0
[park-forensics] ledger seq=2 worker=1 epoch_at_commit=5 ready=1 ctrl=2
                outstanding=0 waiting=0 classify=mw_s1 classify_seq=4 bounded=0
                idle=0 term=0 extwake=0 bwait=0
```

Every §8.2 timeline step is now a causally OBSERVED transition in the test
(backend gate flag -> participant wait-phase flag -> request_stop ->
`loop_exited[0]` -> ready-publication token -> seam release -> reap count 0 +
stranded runnable depth 1 -> drain watchdog), not a statistical outcome: no
`yield()`-as-ordering, no sleep as proof (production-test-plan §1).

gdb on the original stress stall (6 threads): main (watchdog), drainer
(`ApplicationRuntime::drain()` cv wait), driver (`run_impl` joining),
TWO idle ThreadPoolBackend workers, and exactly ONE scheduler worker thread
(worker 1, parked). Worker 0's OS thread no longer exists. The deterministic
reproducer constructs this thread picture by causality: worker 0's thread
exits through the observed `mw_s2_no_progress_terminate` break, the driver
remains inside `run_live`'s join, and the sole survivor parks in the wake
domain.

### 8.2 Reconstructed timeline (each step evidence-backed)

1. The task fiber submits one read and awaits (outstanding=1). Worker 0 —
   the only worker allowed to elect (`ws->id == 0`, scheduler.cpp MW-S2
   Phase A) — is the participant parked in `ctx_.wait_one()` (backend
   domain; `backend_wait_active=1`).
2. `rt.request_stop()` interrupts the participant (control generation 2).
   The read is still gated (test seam), so `wait_one` reaps 0:
   the `!made_progress` path stores `global_terminate_=true`, notifies all
   inboxes, signals the wake domain (wake epoch -> 5), and worker 0 exits
   (`exit_reason=mw_s2_no_progress_terminate`).
3. The gate is released; the read completes (`ready=1`);
   `signal_ready_progress()` publishes ONLY to the backend wait source —
   no participant remains in `wait_one`, so the publication has no observer.
4. Worker 1's loop-top drain reaps the completed request and routes the
   task fiber's continuation to its OWNER (worker 0) inbox
   (`local_runnable=1` on worker 0). `route_runnable_locked` signals the
   wake domain — the epoch advance is absorbed by worker 1's OWN park
   baseline (ledger seq=1: `epoch_at_commit=5` == current `wake_epoch=5`).
5. Worker 1 classifies `mw_s1` ("runnable work exists — someone will run
   it"), parks UNBOUNDED in the wake domain delegating progress to worker 0.
   Worker 0's thread is gone. The parked predicate checks (wake epoch,
   terminate, OWN inbox) — it cannot observe a runnable queued on ANOTHER
   worker's inbox, and no further wake publication ever occurs.
6. `ApplicationRuntime::drain()` waits forever; the driver joins forever.

### 8.3 Violated park-window invariants (the audit's four conditions)

Let a worker entering an unguarded wake-domain park satisfy:

1. *scheduler ready queue empty* — VIOLATED for the parked worker: a
   runnable continuation exists (worker 0's inbox). The classify-side
   premise "mw_s1 ⇒ some worker will run it" is a stale delegation: it
   never verifies the owner worker is alive. The E9-CORRECTIVE predicate
   backstop checks only the parking worker's OWN inbox.
2. *backend outstanding == 0* — held at park time (the request had been
   reaped), but only because the reap already happened on worker 1; the
   stranded continuation is the residue of that reap.
3. *backend ready publication re-checked* — the ready publication
   (ready=1) HAD no remaining observer (the sole `wait_one` participant
   exited at step 2); worker 1's drain consumed it, but the product of the
   drain (the routed continuation) landed on a dead worker's queue.
4. *no wake_epoch publication between snapshot and final check* — VIOLATED:
   the route's epoch advance (to 5) landed between worker 1's final
   classify and its `observed_epoch` baseline record, and was CONSUMED by
   that baseline (`epoch_at_commit == wake_epoch == 5`). The predicate has
   no persistent-state re-check for "runnable exists anywhere" — the
   commit-to-sleep window is not closed (AGENTS.md §13.2).

Structural contributors (design-level, to be addressed in the fix design —
NOT fixed in this change):

- MW-S2 participation is restricted to worker 0 (`ws->id == 0`) while
  non-electing workers observing `mw_s2` still park unguarded in the wake
  domain (the fall-through comment assumes "another worker is the
  participant" — false when none exists or it has exited).
- Backend completion (`signal_ready_progress`) publishes only to the
  backend wait source; it never crosses into the wake domain, so a parked
  wake-domain worker cannot be woken by backend progress at all.
- The `mw_s2_no_progress_terminate` exit path strands queued runnables: it
  notifies but neither drains nor transfers inbox entries, and run
  re-entry (run_impl resets `global_terminate_` at line ~871) reuses
  WorkerState without a re-seeding guarantee for per-worker inbox residue.
- The park predicate's persistent-state backstop covers only
   route-to-SELF runnables (own inbox), not route-to-OWNER runnables.
- External references confirming the closure discipline the fix must
  follow: Tokio's `Notify::enable` (arm the wake registration BEFORE
  checking the condition — register, then check, then await; our order is
  check-then-arm, which is the consumed window), and Tokio's
  `enable_eager_driver_handoff` (the driver/poll role must be transferable
  between workers with an explicit wake, vs. our fixed worker-0-only
  participant with no handoff obligation).

### 8.4 Verdict

**G1 BLOCKED (superseded by §8.5 — the repair landed and is verified).** The
Phase G unbounded wake-domain park was NOT sound with the pre-repair
classify/predicate/election protocol: the system could reach and permanently
hold a state where work exists (a routed continuation on a worker queue), no
worker is in the backend domain, every worker is parked unguarded in the wake
domain, and no wake publication is pending. The E9 2ms periodic
re-classification previously masked these windows; removing it (Phase G's
goal) exposed them.

Residual open question (resolved by the repair's retire + live-count): worker
0's WorkerState carried the stranded runnable across the run boundary because
the terminate path neither transferred inbox entries nor let the survivors'
idle-dance converge against the shrunken participant set; both are now closed
mechanically (below), so the run boundary no longer strands tickets.

### 8.5 The G1 repair (PR #108 follow-up; addresses §8.3's four conditions together)

The repair is four cooperating mechanisms, each mapped to the violated
invariant it closes. No timeout re-arm is used anywhere.

**R1 — Park-commit arm→recheck closure (conditions 1+4; the Tokio
`Notify::enable` discipline).** `park_on_wake_source` records its
`observed_epoch` baseline under a NESTED `global_mtx_ → wake_mtx_`
acquisition (the accepted order), AFTER rechecking under `global_mtx_` — the
domain every runnable/route publication serializes under — that no
`unguarded_progress_pending_locked()`: a runnable ticket on ANY active
participant's queue or in `pending_spawn_`, or accepted backend work, with NO
active observer (no running fiber, no backend-domain participant, no
admission in flight). A refusing worker does not park: it SIGNALS the wake
domain (a non-electable refuser must wake the sleeping electable sibling —
the progress-observer invariant) and re-loops, becoming the observer itself
(steal — including from a terminated worker's queue — or MW-S2 election).
Because publishers signal under `global_mtx_`, a publication before the
recheck is SEEN by it and one after it advances the epoch past the
just-recorded baseline: the check-then-arm window is closed.

**R2 — Transferable MW-S2 election (the "worker-0-only election"
contributor).** Phase A elects the LOWEST-ID worker whose loop is still live
(`WorkerState::active`), not hard-coded worker 0: with all workers alive the
behavior is unchanged (worker 0), but after the participant's thread exits
through the no-progress terminate a survivor can elect — the no-participant
manifestation heals instead of parking unguarded forever.

**R3 — Terminate-path retire (the "terminate path strands queued runnables"
contributor).** Every worker_loop exit funnels through a retire block that
moves the worker's `local_runnable` to `pending_spawn_` (live workers'
loop-top drains it; the next invocation's setup re-seeds and re-records
owners) and decrements `live_loop_workers_` — the idle-dance convergence
threshold is now the LIVE loop count, not the invocation snapshot size (the
"participant disappearance" residual: a survivor could otherwise remain one
short of last-idle forever, `run_live` never returns, and the driver never
reaches its drain-complete evaluation). The retire signals the wake domain
unconditionally, with the inbox lock released first (lock-order discipline).

**R4 — Persistent-state park backstop (condition 4's general closure;
final form after the adversarial review).** The idle-dance condition is
checked at the park COMMIT recheck — the same `global_mtx_` critical
section as R1 — compared AGAINST the worker's own dance contribution
(`WorkerState::idle_dance_contributed_`), never as a bare count and never
in the cv predicate. Two rejected drafts show why: (1) a predicate term
observes the dancer's own count, so the dancer's park becomes a no-op and
it re-dances immediately; (2) a bare commit-time `idle_workers_ > 0`
refusal self-triggers the same way, and each re-dance's last-idle
`store(0)` erases the count, so a woken sleeper always starts its dance
from zero, emits its own not-last signal, and wakes its peer — the
wake-park-wake chain never damps (a 100%-CPU livelock in the Live-mw_s3
resident state; the shape of the CI stall). Exempting the worker's own
contribution restores the pre-G1 damping: a counted dancer SLEEPS holding
its count, so the woken sleeper's `fetch_add` reaches the last-idle
threshold immediately and its cycle ends in either termination or a
SILENT park. Meanwhile a worker that has not danced (contribution 0)
refuses behind ANY live count: the recheck and the dance serialize under
`global_mtx_` (the dancer's fetch_add and not-last signal both hold it),
so the dance is either visible at the recheck (refuse; re-loop and
converge the dance) or its signal advances the epoch past the baseline
being recorded (predicate wake) — the E9-LIFE-8 absorbed-baseline window
stays closed with persistent state. The contribution flag is cleared
conservatively at each loop-iteration top; an under-clear can only cause
an extra refuse-and-redance cycle (convergence), never a missed refusal.

**Why no backend→wake bridge is needed:** with R1+R2, any worker that finds
unguarded backend progress refuses to park and becomes the observer (it
elects into `wait_one`, whose snapshot→poll→park loop then reaps a
ready-but-unobserved publication on entry); the observer invariant is
self-restoring at the park boundary rather than requiring a reverse bridge.

**Evidence (all on the repaired head):**
- the deterministic reproducer (§8.1): pre-repair RED, post-repair GREEN on
  an UNCHANGED scenario. After the initial 20+8 GREEN runs, a Release-build
  sweep exposed a construction (not a repair) defect: a submit-after-start
  placement races the workers' first idle convergence — in Release the
  workers can go idle, one exits `last_idle_terminate`, and the late submit
  leaves the fiber for the single survivor, so the parked pair the
  reproducer pins can never form (fail-closed `no-participant-parked-pair`).
  The construction now holds run_impl at the run-entry seam
  (`worker_topology_ready_before_start`: topology published, no worker
  thread started), submits while held, and releases — the fiber is queued on
  worker 0's inbox before any thread loops, eliminating the convergence race
  in every build mode. On this construction: pre-repair 5/5 rc 70 (5/5 the
  PRIMARY `stranded-runnable` dump, Release build), post-repair 5/5 GREEN
  (Release) and 30/30 GREEN (Debug), including the role-swapped
  interleavings the transferable election admits;
- `sluice_copy_pipeline_integration_test` ×13, `sluice_copy_pipeline_stress_test`
  ×3+3, `application_runtime_drain_starvation_test` ×3+3 and 6/6 under TSan:
  all PASS (pre-repair: recurring timeouts / SIGABRT / the §8 stall); the
  four binaries also PASS a Release sweep;
- full suite: 165/165 debug binaries PASS (parallel ×16, per-binary
  timeout 300) — the three pre-repair G1-family failures are gone;
- TSan: the full gate clean after the retire's inbox-scope fix (the one
  finding TSan produced — an inbox_mtx→wake_mtx_ inversion introduced by an
  early retire draft — was repaired; see the compliance gate's race rows).

**Formal-model status (AGENTS §17):** the repair changes the modeled park
commit rule, election, and termination convergence of `spec/tla/e9_park_wake`.
Updating that model to R1–R4 and re-running TLC is recorded as a REQUIRED
G2-entry obligation in the compliance gate (a justified coverage gap for
THIS change: the implementation evidence is the deterministic reproducer —
pre-repair RED, post-repair GREEN on an unchanged construction — plus the
full-suite and TSan gates; the model update must land before G2).
