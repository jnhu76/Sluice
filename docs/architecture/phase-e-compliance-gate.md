# Phase E Compliance Gate — Bounded Blocking-I/O Backend (ThreadPoolBackend → RequestArena)

**Design:** [`docs/design/phase-e-bounded-threadpool-backend.md`](../design/phase-e-bounded-threadpool-backend.md)
**Governing ADR:** [`ADR-explicit-io-request-contract`](../adr/ADR-explicit-io-request-contract.md) (Accepted)
**Generic gate:** [`design-compliance-gate.md`](design-compliance-gate.md)
**Branch:** `feat/phase-e-bounded-threadpool-explicit-io`
**Status:** Gate 0–3 complete at design time; Gate 4 evidence is **PENDING** and is filled as
each vertical slice lands. **No row is pre-marked PASS** (AGENTS.md §8/§22: pre-filling PASS
before execution is forbidden; "PENDING — slice N" is the honest pre-execution state).

This document is the PR-level evidence ledger, complementing the ADR's own Gate 0–4 (which gives
the contract-level classification) with the Phase E backend-migration evidence.

---

## Gate 0 — Architecture Classification

```text
Affected capability:    AsyncBackend / ThreadPoolBackend / AsyncIoContext
Affected layer:         L0 portable blocking-I/O backend; L1 context-facing backend contract
Classification:         Corrective + explicit-I/O backend migration
Governing ADR:          ADR-explicit-io-request-contract (Accepted); ADR-explicit-io-completion-authority;
                        ADR-async-io-model; ADR-execution-model
Conformance map change: yes — DIV-03 Resolved; DIV-14 partially closed for ThreadPool only;
                        zig-io-conformance-map Pending.Userdata / Resource bounds /
                        Threaded-layer rows advance for the blocking backend
Divergence change:      DIV-03 -> Resolved; DIV-12 -> Resolved; DIV-14 partially closed
                        (ThreadPool implements prepare()-time validation; reference backends +
                        Uring retain their existing status); DIV-10 unchanged (running-cancel
                        best-effort stays)
Constitution rules:     AC-1, AC-2, AC-3, AC-4, AC-5, AC-6, AC-7, AC-8, AC-9, AC-10, AC-11,
                        AC-12, AC-13
Public API effect:      ThreadPoolConfig + explicit ThreadPoolBackend(ThreadPoolConfig) ctor are
                        ADDED (public header + contract tests + api-reference); submit_*/poll/
                        wait_one/cancel signatures unchanged; the unguarded public
                        shutting_down_for_test() is REMOVED (replaced by real close_admission)
```

No field is "unknown" or "TBD". Coding may proceed under the vertical-slice order in the design.

---

## Gate 1 — Ownership and State Machine

Full state machine, transition authorities, lock domains, allocation rules, failure semantics,
wake obligations, and shutdown behavior: **design §4 / §5**. Summary of authorities:

| Transition | Authority | Lock domain | Alloc | Failure | Wake |
|---|---|---|---|---|---|
| `free -> reserved` | backend admission | arena leaf | none | `would_block`/`invalid_state`, Completion idle | none |
| `reserved -> prepared` | backend prepare | arena leaf | none | malformed -> `invalid_argument`, rollback | none |
| `prepared -> pending` (commit) | backend claim CAS `idle->binding->outstanding` | arena leaf + context | none | pre-commit rollback; LP is the release-store | none |
| `pending -> enqueued` | enqueue arbitration | arena leaf | none (noexcept) | only `terminal_noop`; stale/illegal fail-fast | notify worker if enqueued |
| `pending -> backend_ready(canceled)` | Scheme-B cancel | arena leaf | none | terminal winner; enqueue later no-ops | ready epoch |
| `enqueued -> running` | worker pop + `mark_running` | **work_mtx_ + arena** | none | backoff `false` if already backend_ready; stale fail-fast | none |
| `running/enqueued -> backend_ready` | worker `record_terminal` (verbatim) | arena leaf | none | first winner only | ready epoch |
| `backend_ready -> completion_ready` | reap | arena leaf | none | ineligible while pinned (I19) | sink callback after unlock |
| `completion_ready -> free` | caller reset/destroy | arena leaf | none | live pin / open reg -> fail-fast | capacity waiter (none in Phase E) |

Lock order (design §5): `access_mtx_ -> work_mtx_ -> arena leaf`; `ready_mtx_` is separate and
does not nest with `work_mtx_` or the arena. The leaf domain never calls Scheduler/user/sink.

The load-bearing Phase-E concurrency — **pop + mark_running as one coordinated transfer under
`work_mtx_`**, and **`ring.remove_exact` + `arena.cancel` under one `work_mtx_`** — eliminates
the pop-before-running gap and the "off-ring AND being-dispatched" window (design §4.2/§4.3;
ADR §10.3/§10.4).

---

## Gate 2 — Resource and Failure Model

```text
Construction-time resources:
  - RequestSlot[request_capacity] + per-slot PreparedBlockingOp scratch: capacity=fixed,
    allocation=preallocated at construction, failure=no_space (never a submit error)
  - BoundedDispatchQueue storage[request_capacity]: preallocated ring, fixed head/size/high_water
  - worker_count std::thread: created in the constructor; failure -> set stop, notify, join
    started workers, rethrow

Submit-time resources:
  - RequestSlot (reserve): capacity=request_capacity, allocation=none (claims free slot),
    failure=would_block (full) / invalid_argument (malformed) / invalid_state (admission closed)
  - submit allocation-free after acceptance? YES (steps 10-12 are noexcept; I9)

Dispatch/terminal resources:
  - terminal Result: 1/slot preallocated, cannot be lost (I4/I9)
  - dispatch ring entry: preallocated; post-commit push failure = fail-fast invariant

Capacity and backpressure:
  - Maximum outstanding: request_capacity (bounded; AC-7)
  - Queue-full behavior: synchronous would_block, Completion idle, no borrow (pre-commit only)
  - OOM at each stage: construction -> no_space; submit pre-commit -> rolled back idle;
    post-commit -> NONE (the accepted terminal path depends on no new allocation, I9)

Reclamation:
  - Shrink under load? NO (fixed arena + fixed ring + fixed workers; free-list reuse)
  - Bounded by: outstanding (slot_in_use: reserve -> release), NOT by historical total
```

Distinct counters (ADR Decision 13): `slot_in_use` (reserve → release) vs `accepted_outstanding`
(commit → completion-ready). The legacy duplicate `outstanding_` is DELETED; `outstanding()`
returns `arena_.accepted_outstanding()`. `active_workers` is a separate, named metric.

---

## Gate 3 — Progress and Wake Model

```text
Blocking/suspension:
  - Who may block?       caller thread via wait_one(); poll() is non-blocking
  - Who may suspend?     no Fiber in Phase E (Scheduler integration is Phase F)
  - What makes them continue? caller-driven poll()/wait_one(); backend_ready observable on reap

Backend -> Scheduler progress:
  - How does backend-ready reach? observation via poll()/wait_one (Phase E does NOT bridge to
                          Scheduler wake; that is Phase G, DIV-04/05 unchanged)
  - Signal or observation? a persistent ready EPOCH (ready_epoch_) under ready_mtx_ +
                          ready_cv_; signal_ready_progress() is called by worker/cancel/ack/stop
  - Worst-case latency?   caller-defined (wait_one blocks on the epoch); NO internal poll interval

External wake coexistence:
  - Commit-to-sleep race closed by? the ready_epoch_ sequence: snapshot epoch -> reap -> if 0,
                          re-lock and re-check epoch before wait (design §4.5). A signal that
                          arrives between snapshot and wait bumps the epoch and the wait re-checks.
  - Pinned backend-ready (I19): the enqueue pin-ack path re-arms signal_ready_progress so a
                          ready-but-ineligible slot does not lose its wake once acked.

Polling dependency:
  - Periodic timeout?     NO. No polling interval is introduced. wait_one has NO deadline.

Single-worker liveness:
  - With one worker, can an accepted op progress while another is in a blocking syscall?
                          YES — worker_count >= 1 serves the ring; a long syscall occupies one
                          worker but others (if any) keep draining. With worker_count == 1 a long
                          syscall serializes dispatch (documented, not a lost-wake).
```

No answer is "periodic poll". AC-6 satisfied.

---

## Gate 4 — Evidence Plan (PENDING — filled as each slice lands)

Each row lists the property, the test that proves it, and (once the slice lands) the actual
command + result. **No row is pre-marked PASS.** Until a slice lands its tests, the Result column
reads "PENDING — slice N". A test that cannot fail on the pre-fix code is not proof (AGENTS.md §7).

### Slice 1 — config + construction bounds

| Property | Test | Result |
|---|---|---|
| default + explicit ThreadPoolConfig ctor; worker_count/request_capacity > 0; hardware_concurrency()==0 handled | `threadpool_*_config*` | PENDING — slice 1 |
| persistent + bounded workers: total threads started == worker_count; no thread created after construction; worker storage fixed | `threadpool_workers_are_persistent_and_bounded` | PENDING — slice 1 |
| (semantic) accepted requests still terminate; no loss; no hang | (same) | PENDING — slice 1 |

### Slice 2 — capacity / would_block

| Property | Test | Result |
|---|---|---|
| full arena -> would_block; Completion idle; no queue entry; no borrow; no worker execution | `threadpool_request_capacity_returns_would_block` | PENDING — slice 2 |

### Slice 3 — descriptor validation (real syscall backend; DIV-14 does NOT apply)

| Property | Test | Result |
|---|---|---|
| negative fd read/write/sync_data/sync_all -> invalid_argument | `threadpool_descriptor_validation*` | PENDING — slice 3 |
| null buffer with nonzero len -> invalid_argument; offset > off_t -> invalid_argument; len > SSIZE_MAX -> invalid_argument | (same) | PENDING — slice 3 |
| zero-length null buffer chosen behavior (allowed) | (same) | PENDING — slice 3 |
| closed nonnegative fd -> submit accepted -> terminal EBADF mapping | `threadpool_closed_fd_terminal_error` | PENDING — slice 3 |
| every rejection: Completion idle, outstanding unchanged, slot released | (same) | PENDING — slice 3 |

### Slice 4 — 5-stage admission + reap (write/read round-trip through the arena)

| Property | Test | Result |
|---|---|---|
| write/read round-trip; worker records backend-ready; reap publishes; exactly-one terminal | `threadpool_*round_trip*` | PENDING — slice 4 |
| worker does NOT hold Completion*; worker only record_terminal; reap is sole publication | (inspection + behavior) | PENDING — slice 4 |
| outstanding() == arena_.accepted_outstanding(); legacy outstanding_ deleted | (inspection + test) | PENDING — slice 4 |
| disjoint offsets; EOF returns 0; fdatasync/fsync; many concurrent writes reaped via wait_one loop | (existing + extended) | PENDING — slice 4 |

### Slice 5 — persistent workers + bounded ring (high-frequency small I/O)

| Property | Test | Result |
|---|---|---|
| N=high small I/O over fixed workers + bounded capacity; no hang; no loss; bounded workers/ring | `threadpool_high_frequency_small_io*` (default suite smaller; final report runs N=100003, buf=1) | PENDING — slice 5 |
| dispatch ring: push/pop/remove allocate nothing after construction | `threadpool_dispatch_queue_no_alloc*` | PENDING — slice 5 |
| post-commit ring-full is fail-fast (Debug AND Release), not would_block | (death / inspection) | PENDING — slice 5 |

### Slice 6 — pending cancel before enqueue (Scheme B)

| Property | Test | Result |
|---|---|---|
| cancel wins before enqueue; enqueue terminal_noop; syscall never runs; reap canceled once; reset/reuse safe | `threadpool_pending_cancel_wins_before_enqueue` | PENDING — slice 6 |

### Slice 7 — enqueued cancel wins

| Property | Test | Result |
|---|---|---|
| remove_exact + cancel; worker does not execute; syscall counter unchanged; queue has no stale entry | `threadpool_enqueued_cancel_wins` | PENDING — slice 7 |

### Slice 8 — running cancel preserves real result

| Property | Test | Result |
|---|---|---|
| running cancel -> intent_recorded only; ordinary success/error wins verbatim; NOT rewritten to canceled; canceled_ops unchanged | `threadpool_running_cancel_preserves_real_result` | PENDING — slice 8 |
| confirmed-interruption path records err(canceled) explicitly and wins (design hook) | (same, where feasible) | PENDING — slice 8 |

### Slice 9 — dequeue/cancel/reuse stress (TSan)

| Property | Test | Result |
|---|---|---|
| submit/enqueue/dequeue/cancel/terminal/reap/reset/reuse; no stale mark_running; no double terminal; no queue duplication; no stale generation; 0 data races | `threadpool_dequeue_cancel_reuse_stress` | PENDING — slice 9 |
| no pop-before-running gap (deterministic seam) | (same) | PENDING — slice 9 |

### Slice 10 — wake epoch / no lost wake

| Property | Test | Result |
|---|---|---|
| ready before snapshot / between snapshot and reap / between reap and wait; pinned backend-ready; pin-ack rearm; spurious notify; multiple terminals before one reap | `threadpool_no_lost_wake*` | PENDING — slice 10 |
| wait_one never hangs; never busy-loops; never returns 0 as success | (same) | PENDING — slice 10 |

### Slice 11 — shutdown

| Property | Test | Result |
|---|---|---|
| close_admission -> new submit invalid_state; existing continue; cancel/poll/wait legal | `threadpool_close_admission*` | PENDING — slice 11 |
| quiescent destructor succeeds; idle workers joined cleanly | `threadpool_quiescent_destroy*` | PENDING — slice 11 |
| enqueued/running/backend-ready-unreaped/completion-ready-but-not-reset during destroy -> fail-fast (Debug AND Release) | (death harness) | PENDING — slice 11 |
| partial worker-construction cleanup (set stop, notify, join started, rethrow) | (same) | PENDING — slice 11 |
| unguarded shutting_down_for_test() removed; no unguarded *_for_test in production | (negative-compile / inspection) | PENDING — slice 11 |

### Slice 12 — allocation-free hot path

| Property | Test | Result |
|---|---|---|
| post-commit enqueue / record_terminal / cancel-terminal / reap-publish allocate nothing (counting + always-throw operator new) | `threadpool_no_post_accept_alloc*` | PENDING — slice 12 |
| TSan allocation-probe limitation recorded honestly (runtime owns new/delete under TSan) | (note in evidence) | PENDING — slice 12 |

### Sanitizers / modes (AGENTS.md §16)

| Gate | Command | Result |
|---|---|---|
| Clang Debug | `xmake f -m debug --toolchain=clang -y && xmake build sluice_core && xmake build sluice_async && xmake build -g test && xmake test -v` | PENDING |
| Clang Release (§16.1) | `xmake f -m release --toolchain=clang -y && ...` | PENDING |
| ASan + UBSan (§16.2) | `xmake f -m asanubsan --toolchain=clang -y && ...` | PENDING |
| TSan (§16.3) — target 0 data races | `xmake f -m tsan --toolchain=clang -y && ...` | PENDING |
| negative-compile | `scripts/verify-completion-authority-negative-compile.sh` + `scripts/verify-request-arena-negative-compile.sh` (+ any new ThreadPool authority probe) | PENDING |
| doc-check | `python3 scripts/check-doc-links.py --self-test && python3 scripts/check-doc-links.py && python3 scripts/verify-architecture-docs.py` | PENDING |
| formal | `python3 scripts/formal/verify.py` (and a new `phase_e_blocking_dispatch` suite if added) | PENDING |
| stress | N=100003, buffer=1, fixed workers, bounded capacity (recorded command + result) | PENDING |
| diff | `git diff --check && git status --short && git diff --stat` | PENDING |

TSan coverage MUST include (§16.3): submit vs dequeue; enqueued cancel vs dequeue; running cancel
vs terminal; worker terminal vs reap; ready-epoch signal vs wait; reset/reuse after reap;
shutdown worker wake.

---

## Gate Completion Checklist

(Boxes are ticked only when the ACTUAL command has run and passed.)

- [ ] Gate 0 classification complete and accurate (above)
- [ ] Gate 1 state machine covers all new/modified lifecycles (design §4)
- [ ] Gate 2 resource model has no unbounded growth without ADR approval (above; AC-7)
- [ ] Gate 3 wake model has no undocumented polling dependency (above; AC-6)
- [ ] Gate 4 evidence filled with ACTUAL results (every row PASS)
- [ ] Conformance map updated (DIV-03 Resolved, DIV-12 Resolved, DIV-14 partial, zig-map rows)
- [ ] Divergence registry updated (DIV-03 / DIV-12 / DIV-14 / DIV-10 status)
- [ ] Constitution rules satisfied (design §0/§1 map AC-1..AC-13)
- [ ] AGENTS.md change-class gates run (Debug / Release / ASan+UBSan / TSan / negative-compile / docs / formal / stress)
- [ ] as-built / findings / api-reference updated to reflect the migrated ThreadPool
