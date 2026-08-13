# Phase F1 Compliance Gate — Scheduler consumes identity-bearing reap

**Phase:** F1 (Issue #98, re-baselined after the 2026-08-13 corrective pass)
**Design:** `docs/design/phase-f1-scheduler-ready-sink.md`
**Status:** COMPLETE — Gate 4 evidence filled from executed runs (2026-08-13).
**Authority:** ADR-explicit-io-request-contract (Accepted) Decisions 9/10;
AGENTS.md §4.1/§4.3/§4.4, §10, §13.1/§13.2; Constitution AC-13/AC-14/AC-15.

---

## Gate 0 — Architecture Classification

| Field | Decision |
|---|---|
| Affected capability | Scheduler wait registration + wake routing; production ReadySink; poll/wait_one sink wiring; production waiter cancellation |
| Affected layer | L1 AsyncIoContext (sink attachment + waiter registration plumbing); E7-E13 Scheduler (wait registry, routing); backend sink call sites (plumbing only — no authority change) |
| Classification | Corrective (completes the Accepted contract's production Scheduler-consumption path; no new divergence) |
| Governing ADR | ADR-explicit-io-request-contract Decisions 9/10 (identity-bearing reap; waiter cardinality and routing-record lifetime) |
| Conformance map change | Row "Batch + batchAwait" / "Future" evidence pointers update; no row reclassification (Scheduler remains sole Fiber-routing authority; wake bridge stays Phase G) |
| Constitution rules | AC-2, AC-5, AC-6, AC-7, AC-10, AC-11, AC-13, AC-14, AC-15 |

---

## Gate 1 — Ownership and State Machine

### 1.1 WaitRecord state machine (new, Scheduler wait registry — leaf R)

```text
free
  | registration (G -> R): generation++, fiber/owner/completion stored
  v
registered
  | sink delivery (A -> R)                | waiter cancel (G -> R)
  v                                        v
delivered                                cancelled
  | drain consume (G -> R): fiber+owner extracted; record freed
  v
free (generation preserved until next registration-reuse bumps it)

delivered -> (cancel path) : impossible (cancel_waiter returns not_found;
                              the delivery already won — no record access)
cancelled -> (sink)        : impossible (no delivery exists)
```

| Transition | Authority | Lock domain | Alloc | Failure | Wake | Shutdown |
|---|---|---|---|---|---|---|
| `free -> registered` | Scheduler registration (await_completion) | G -> R (record), G -> A -> L (arena register_waiter) | none (all records preallocated at construction) | pool exhausted (nullptr) -> await returns `no_space` synchronously; no record installed; register_waiter `invalid_state` + `c.ready()` -> inline success; `invalid_state` otherwise -> record retired, await returns it; `not_found` -> record retired, await returns `invalid_state` | none (fiber about to suspend) | record retired on failed register; live registrations must resolve before quiescent destruction |
| `registered -> delivered` | ReadyRoutingSink (reap) | A -> R | none | stale token/generation or cancelled state -> lease dropped, no route (loser) | drain routes the fiber in the same or next pass | delivery still routed during stop; control wake only interrupts backend park |
| `delivered -> free` | drain (worker_loop) | G -> R | none | none (extraction is unconditional after pop) | fiber made runnable exactly once (E7-T2) then `route_runnable_locked` | drained before termination; records must be free at `~Scheduler` (assert) |
| `registered -> cancelled` | `Scheduler::cancel_waiter` (winner) | G -> R (record), G -> A -> L (arena cancel_waiter) | none | loser (`not_found`): no record access, return false | fiber resumed exactly once with `canceled` outcome | legal during stop; I/O/borrow untouched |
| record reuse | registration | G -> R | none | generation bump before the new occupant is visible (I6) | none | n/a |

### 1.2 Waiter registration state (arena, unchanged — Decision 10)

```text
registration_open(no_waiter)
  | register(token, RequestKey)                    (Scheduler production path now)
  v
registration_open(registered(token, lease))
  | reap closes + takes delivery    | cancel_waiter takes delivery
  v                                  v
registration_closed(no_waiter)     registration_open(no_waiter)
```

Duplicate register on the Scheduler path returns synchronous `invalid_state`
without overwriting the first token (I13) — the second fiber's `await_completion`
surfaces it.

### 1.3 Lock-order table (AGENTS.md §13.1)

```text
G -> A        classify_locked (ctx_.outstanding())            [unchanged]
G -> A -> L   drain poll / register_waiter / cancel_waiter     [unchanged order; new callers]
G -> R        registration, delivered-pop, cancel retirement   [new]
G -> I        route_runnable_locked / classify_locked          [unchanged]
G -> W        signal_wake_locked                               [unchanged]
A -> R        ReadyRoutingSink::on_ready                       [new; leaf only]
A -> L        backend reap / register / cancel                 [unchanged]
L -> (release) -> sink.on_ready                                [arena contract]
R is a leaf: never acquires G, A, I, W, or L while held.
```

Cycle proof: G and A are the only upper domains; R and L are leaves with
inbound edges only (`{G,A} -> {R,L}`); the sink's R access during `poll`
(`A -> R`) cannot deadlock against the drain's `G -> A` (no `R -> G`, no
`A -> G`). See design §5.

---

## Gate 2 — Resource and Failure Model

| Resource | Capacity / allocation | Full behavior | Reclamation |
|---|---|---|---|
| WaitRecord registry (`wait_records_` + free list, R) | **fixed capacity** at `wait_capacity` (default 256), preallocated at `Scheduler` construction; never grows beyond configured bound (AC-7); address-stable | **zero allocation** after construction — `acquire_wait_record_locked` pops from free list or returns `nullptr` (synchronous `no_space`); **never** on the accepted -> route path | free-list reuse with generation bump; registry empty at `~Scheduler` (assert) |
| Delivered list (intrusive through records) | bounded by live records; each record delivered at most once | impossible to overflow (single lease per record; drained every pass) | popped + freed every drain pass |
| Token/lease | one per registration; moved slot -> event -> sink (or cancel) exactly once | loser consumes at the by-value boundary | lease destruction = acknowledgement |
| Sink / drain / route path | **zero allocation** after acceptance | n/a | n/a |
| Backend `routing_sink_` pointer | one pointer per backend | no-op fallback when unset | detached at `~Scheduler` |

OOM at each stage: WaitRecord pool exhaustion (all `wait_capacity` records
live) propagates as a synchronous `no_space`-class `Result` from
`await_completion` (record never installed; no token/lease created — nothing
to leak). No accepted request's
terminal path depends on an allocation (I9).

---

## Gate 3 — Progress and Wake Model

| Question | Answer |
|---|---|
| Who may suspend? | Fibers (unchanged). Waiters suspend only after registration committed under G and the arena accepted the token/lease. |
| What makes them continue? | (a) identity route: sink marks delivered -> drain routes under G; (b) waiter cancel: cancel path routes with `canceled`; (c) backend progress via MW-S2 `wait_one` reaps. |
| Backend -> Scheduler progress | unchanged: drains poll under G (`G -> A`); MW-S2 participant parks in split-phase `wait_one` (D4-RM13/14, no `access_mtx_` across the park). The sink is invoked by the arena with no slot/backend lock held and takes only the R leaf — it never blocks on G, so the existing `G -> A` edges are preserved. |
| Commit-to-sleep closure | arena leaf serializes registration vs reap extraction (Race A); R state machine serializes sink vs cancel (Race B); E7-T2 `make_runnable` gates runnable publication; split-phase epoch protocol closes the MW-S2 park (unchanged). |
| Polling dependency | the 2ms MIXED-WAKE backstop remains the observation-return path for backend progress (unchanged; removal is Phase G). No new polling interval is introduced. |
| Single-worker liveness | unchanged (route_runnable_locked target selection; pending_spawn_ for zero participants). |
| Lost-wake risk | none added: every registered waiter's delivery is either routed (drain) or cancelled (cancel path); the sink marks before the lease is destroyed; the drain polls before popping in the same G scope. |

---

## Gate 4 — Evidence (filled after execution, 2026-08-13)

### 4.1 Deterministic causal tests (new, RED-first)

`tests/scheduler_identity_wake_test.cpp` — **PASS, ALL TESTS PASSED** (repeated
3 consecutive full-suite runs plus 60 standalone runs; no sleeps as proof):

| Test | Proves | Result |
|---|---|---|
| `f1_scheduler_routes_via_ready_sink` (T1) | production Scheduler consumes ReadyEvent-routed wakes; sink delivery counter increments; legacy scan path absent (`legacy_completion_wait_count == 0`) | PASS |
| `f1_completion_before_waiter_registration_no_lost_wake` (T2) | Race A inline return (Live external-wake run; completion strictly precedes registration) | PASS |
| `f1_waiter_before_completion_wake_exactly_once` (T3) | exactly-once routing (repeat reap delivers nothing) | PASS |
| `f1_cancel_waiter_keeps_io` (T4) | I5: wait-cancel removes only the waiter; I/O terminals + reaps; Completion publishes | PASS |
| `f1_cancel_waiter_vs_reap_race` (T5) | Race B, 200 iterations, exactly-one legal outcome + protocol consistency per iteration | PASS |
| `f1_stale_record_generation_no_wake` (T6) | Race C (forged stale token cannot route the N+1 occupant) | PASS |
| `f1_duplicate_waiter_invalid_state` (T7) | I13 on the Scheduler path (synchronous `invalid_state`, first waiter untouched) | PASS |
| `f1_shutdown_convergence_registry_empty` (T9) | Race D / shutdown (registry empty at `~Scheduler`) | PASS |
| `f1_routing_sync_backend` / `f1_routing_threadpool_backend` (T10) | unified contract on SyncBackend (in-process) and ThreadPoolBackend (real syscall) | PASS |

Death test `tests/scheduler_identity_wake_death_test.cpp` — **PASS**:
`~Scheduler` with a live registered waiter exits 86 (fail-fast, Debug+Release
paths); quiescent control exits 0.

Real-liburing `tests/uring_f1_scheduler_routing_test.cpp` — **PASS
(`[evidence-meta] evidence=uring_f1_scheduler_routing_integration mode=real`)**
on the production `sluice_async` library (real ring, no seams); stub builds
classify mode=stub → INCOMPLETE by design (`required_modes=("real",)`).

### 4.2 Existing gates (re-run, all executed)

- [x] Clang Debug full suite: 3 consecutive `xmake test -v` runs — 160
  binaries, **0 failures** (baseline was 157/157; +F1 suite +death target).
- [x] Backend conformance manifest self-test: `python3
  scripts/tests/test_backend_conformance_manifest.py` — 209 tests OK (pin
  extended with the new `uring_f1_scheduler_routing_integration` record).
- [x] Pre-push gate: `bash scripts/gates/pre-push.sh` — **ALL CHECKS PASSED**
  (doc links, architecture docs, manifest, whitespace).
- [x] Negative compile: `verify-completion-authority-negative-compile.sh`
  (12/12 PASS), `verify-request-arena-negative-compile.sh` (6/6 PASS).
- [x] Docs: `check-doc-links.py --self-test` + `check-doc-links.py` (PASS),
  `verify-architecture-docs.py` (PASS).
- [x] TSan (AGENTS.md §16.3): full `xmake run -g test` — **0 ThreadSanitizer
  warnings, ALL TESTS PASSED** (race classes covered: registration vs reap
  delivery, cancel vs delivery, delivered-drain, wake signal vs park,
  reuse-after-cancel, shutdown registry check).
- [x] ASan/UBSan (AGENTS.md §16.2): full `xmake run -g test` — **0 sanitizer
  errors, ALL TESTS PASSED**.
- [x] Release (AGENTS.md §16.1, public headers changed): full `xmake test -v`
  — **149 binaries, 0 failures**.
- [x] Real liburing suite: stub/off build validated first (full suite green),
  then `xmake f -c -m debug --toolchain=clang --with-liburing=true -y`;
  forced rebuild (`xmake build -r -g test` + explicit
  `xmake build sluice_experimental_uring` — the experimental lib is NOT in
  the test group and must be rebuilt explicitly after a mode reconfig) —
  **152 binaries, 0 failures**, all 7 uring evidence records
  `[evidence-meta] ... mode=real` (c2b, c2c, c2d, c2e, quiescent destruction,
  backend contract, and the new F1 routing record).

---

## Gate Completion Checklist

- [x] Gate 0 classification complete and accurate
- [x] Gate 1 state machines + lock-order table published (design §5)
- [x] Gate 2 no unbounded growth without ADR approval; no post-accept allocation
- [x] Gate 3 wake model: no undocumented polling dependency; race closures listed
- [x] Gate 4 evidence filled with actual results (executed 2026-08-13)
- [x] Conformance map evidence pointers updated (as-built §9.1, roadmap Phase F)
- [x] Divergence registry: rows 12b/14b Scheduler half closes as Phase F scope
  (registry updated); no new divergence introduced
- [x] AGENTS.md §6 / §16 change-class gates run (Debug, Release, TSan,
  ASan/UBSan, negative compile, docs, real liburing)
