# K3 — Progress pass → wakeup → retry

Verdict: **`NO_CHANGE_JUSTIFIED`** for both candidates. The progress machinery
is either dormant (W-02 parks zero times in 540,000 ops) or amortized by 1/D
(passes/op 1.34 → 0.030 from D=1 to D=64). The largest-signal fusion arm
measured −1.00 % CPU (9/15 negative), with its **median** inside the
same-session A/A band but its range extending below it, at a per-fusion lock
round-trip of ≈55 cycles. The effect is also below the campaign 4.79 %
materiality band.

## 1. Question

What does a progress pass actually advance, and is there scanning, lock
handover, state query, notification or driving with no pending obligation?

## 2. Mechanism inventory

| # | Mechanism | Location | Authority / obligation |
|---|---|---|---|
| M1 | `run_progress_pass_` — composes backend pass + core/progress health + exhaustion gate | `src/async/async_io_context.cpp:417-427` | PROG-01 post-pass facts |
| M2 | `drive_entry_admitted_` / `drive_exit_` / `DriveGuard` | `async_io_context.cpp:179-196,328-334` | THREAD-01 owner exclusivity; 2 `access_mtx_` per public drive call |
| M3 | `UringAsyncBackend::poll_progress` — pass summary | `src/async/uring_backend.cpp:304-328` | PROG-01 |
| M4 | `UringAsyncBackend::poll` — pass body | `uring_backend.cpp:616-689` | backend-private |
| M5 | `service_pending_controls_locked_` | `src/async/uring_transport.cpp:695-711` | CANCEL-01; O(1) gate on `pending_control_count_` |
| M6 | `dispatch_retry_remains_locked_` | `uring_transport.cpp:211-216` | PROG-01 honest residue |
| M7 | 10 ms bounded retry + `wait_one` nap | `async_io_context.cpp:22,632-652` | PROG-02 |
| M8 | `has_immediate_physical_work` — unlocked CQ ring probe | `uring_backend.cpp:285-293` | PROG-02 physical readiness |
| M9 | `outstanding_summary` (derived, non-authoritative) | `src/async/detail/request_core.cpp:93-96` | PROG-01 `accepted_work_remains` |
| M10 | `ProgressSource::snapshot` — epoch token | `include/sluice/async/detail/progress_source.hpp:91-94` | PROG-02 |
| M11 | `wait_if_unchanged` — health → epoch revalidate + `read(eventfd)` → probe → recheck → `poll(2)` | `progress_source.hpp:108-232` | PROG-02 park preconditions |
| M12 | `signal` / `wake_notification_nolock_` — suppression via `notification_pending_` | `progress_source.hpp:252-264,426-449` | PROG-02 write suppression; does **not** track kernel CQE writes |
| M13 | `signal_ready_progress` call sites | `uring_transport.cpp:98-101`; `uring_backend.cpp:574,700-703`; `uring_transport.cpp:601-615,523` | only for transitions with no kernel event |
| M14 | kernel eventfd (`io_uring_register_eventfd`) | `uring_backend.cpp:259-283` | one write per CQE, counter aggregation |
| M15 | exhaustion gate | `async_io_context.cpp:437-441` | progress epoch saturation; physically unreachable |
| M16 | `reap_cqes` pass boundary | `src/async/uring_completion.cpp:200-278` | PROG-01 boundedness |
| M17 | per-CQE `dispatch_mtx_` in `handle_one_cqe` | `uring_completion.cpp:99-144` | K1/K4 boundary, counted here only |
| M18 | `deliver_event` claim/retire | `uring_backend.cpp:610-614` | K1 boundary, counted here only |
| M19 | `wait_one` result classification / quiescent report | `async_io_context.cpp:596-683` | PROG-04 |
| M20 | scope wait loop | `include/sluice/async/request_scope.hpp:146-185`; `src/async/request_scope.cpp:82-150` | W-02 drive loop |
| M21 | W-03 driver loop (bench harness) | `bench/bench_w03_external_loop.cpp:189-244` | epoll park, 1 ms timeout only when retry pending, drain until `!immediate_work_remains` |

## 3. Per-pass fixed cost (source-derived, then measured)

An **empty pass** takes eight non-contended acquisitions and **zero syscalls**:

- `access_mtx_` ×2 (drive gate),
- `dispatch_mtx_` ×2 (pass body head + summary),
- core `mutex_` ×2 (`outstanding_summary`, `health_failed`),
- progress `mtx_` ×2 (`wait_health_failed`, `exhausted`).

Ring reads: `cq_ready` (bound), `peek_batch_cqe`, `cq_has_overflow`, and
`has_immediate_physical_work` — approximately five acquire loads. No syscall
unless overflow is flagged.

A **productive pass** adds one `dispatch_mtx_` per CQE, one per publication
pop, ~6 core acquisitions per event, and one pass-level owed check.

A **park** costs progress mutex ×4 + `read(eventfd)` + `poll(2)`: two syscalls.

Measured per-op: passes 1.34 / 0.42 / 0.127 / 0.030 at D=1/4/16/64; empty
passes 0.34 / 0.137 / 0.042 / 0.010; parks equal to empty passes in W-02
(observe), but the full binary shows **0 parks in 540,000 ops** — the observe
instrumentation perturbs park incidence and is not used for it.

**Terminology caveat.** The census's `productive passes` counter is
`completed > 0`, i.e. a return-value criterion. Brief §6 requires productive
work to be defined by obligations actually advanced, not by a nonzero return.
The counter therefore **over-counts** productive passes and under-counts empty
ones: a pass that serves a control, retry, publication-only or owed-only
obligation but completes no operation is counted as empty. In these census arms
there were no control/owed/retry events, so `completed > 0` happens to coincide
with the only progress source; that is a property of the measured load, not a
valid general definition. The K3 conclusions rely on the pass/empty counts and
the dormancy evidence, not on the label.

## 4. K3-A — empty progress work

Existing gate inventory (all O(1) and already authority-backed; no new counter
is needed):

| Fact | Gate | Location |
|---|---|---|
| No pending cancellation | `pending_control_count_ == 0` | `uring_transport.cpp:695-711` |
| No owed observer event | `owed_count_ != 0` (H2b derived count) | `uring_backend.cpp:658-662` |
| No transport to flush | `transport_ledger_->empty()` | `uring_transport.cpp:218-235` |
| No dispatch to drain | `dispatch_->empty()` | `uring_backend.cpp:620-634` |
| No poison | `fatal_error_` check per sub-stage | `uring_transport.cpp:86,93` |

Residual "work with no obligation":

1. Two `access_mtx_` per public drive call, even for an empty pass.
2. Four cross-domain acquisitions at the pass tail to read sticky health bits
   and a derived summary; notably `exhausted()` is checked every pass although
   epoch saturation is physically unreachable.
3. In W-03 the harness runs at least one pass per wake, and an `empty_wake`
   (epoll timeout) also triggers a pass. Harness idle work and library idle
   work must be separated; the census does not attribute them.

Measured effect of removing (2): 4.2 acquisitions/op at D=1 → **−1.00 % CPU**
(9/15). (1) is a THREAD-01 obligation and was not proposed.

## 5. K3-B — redundant progress summary

`poll_progress` assembles `ProgressPass` in three lock domains at three
instants: backend facts under `dispatch_mtx_`, then an unlocked ring read, then
`accepted_work_remains` under the core mutex. PROG-01 only requires post-pass
facts, so the offsets are legal; anything arriving later is covered by PROG-02
(token + eventfd readiness + probe).

Three back-to-back same-lock pairs are fusable:

| Pair | Location |
|---|---|
| pass body's tail `dispatch_mtx_` (owed check) + summary's `dispatch_mtx_` | `uring_backend.cpp:658-662` then `:310-322` |
| core health + `outstanding_summary` | `async_io_context.cpp:421` then `uring_backend.cpp:706` |
| progress `wait_health_failed` + `exhausted` | `async_io_context.cpp:437-441` |

Not removable: `immediate_work_remains` must be read at the pass tail (a
producer may push during the pass), `accepted_work_remains` is a distinct
PROG-01 fact (it prevents the owner spinning on non-progressable work), and
`dispatch_retry_remains` is the explicit honest residue.

## 6. K3-C — wakeup and retry cadence

- One kernel eventfd write per CQE; the counter aggregates, so ≈12 operations
  per wake in W-03 d16 (0.084 notifications/op). `empty_wakes = 0.0000` in every
  uring arm.
- User-space signals are 0/op in all uring steady states; only residue, poison,
  zero-op, cancel and admission transitions signal. The threadpool backend, by
  contrast, signals exactly once per op — a different wake economy, not a
  defect.
- Probe is read twice (pass summary and park handshake); both are necessary
  because state can change between them.
- The park's drain read cannot be skipped on `notification_pending_ == false`:
  kernel eventfd writes do not go through that bit, and PROG-02 requires a
  stale-readiness acknowledge before the final check.
- Retry cadence is 10 ms (owner) / 1 ms (W-03 epoll timeout), never busy-spins.
  `SLUICE_URING_PROGRESS_MUTANT_TRANSPORT_RETRY_UNADVERTISED` shows unadvertised
  retry was a real defect class and remains mutation-guarded.

## 7. Required state distinctions

| Form | passes/op | empty passes/op | parks | user signals | notes |
|---|---:|---:|---:|---:|---|
| Busy steady state W-02 d16 | 0.127 | 0.042 | 0 (full binary) | 0 | the measured domain |
| No-work polling (W-02 d1) | 1.34 | 0.34 | 0 (full binary) | 0 | largest idle share, still small in absolute cycles |
| Producer/consumer idle W-03 | 0.104 | ≈0 | 0.063 epoll/op | 0 | kernel eventfd aggregation |
| Saturation/backpressure (W-02 sat) | 0.185 | 0.061 | 0.061 (observe) | 0 | latency not instrumented |
| W-03 split owner | 0.104–0.314 | ≈0 | epoll | 0 | K4 channel lives here |
| Cancel pending | — | — | — | — | **not exercised**: `control_cqe = 0`, control scans 0 |
| Shutdown / health failure | — | — | — | — | mutation- and test-covered, not load-measured |

## 8. Competing explanations

**E-A (amortized/dormant).** Every K3 mechanism is either dormant in the
measured domain (W-02 zero parks, zero user signals; W-03 zero empty wakes) or
amortized by 1/D. The D=1 premium over D=16 comes from the kernel side (one
enter per op, no overlap, io-wq copy) and the fixed per-op chain, not from pass
round-trips. Any K3-side elision is bounded at ≈2–3 % of cycles at D=1 and
<1 % at D≥4.

**E-B (low-depth pass round-trip).** At D=1 each op serially pays at least one
full pass round (2 access locks + ~5 cross-domain summary locks + snapshot) plus
0.34 empty passes/op (8 locks each), and these lock round-trips lie on the
CQE-visible → `take` path. Removing them should produce a paired CPU drop above
the A/A band at D=1.

Both predict a null at D≥4; they differ only at D=1/D=4, which is why the arm
targets D=1.

## 9. Counterfactual arm and result

`bench/research/key-path-593/patches/k3-fusion-arm.patch` (11 files, +100/−9,
**zero existing-test modifications**) implements the three same-lock fusions:
poll summary folded into `poll()`'s tail critical section, core
health+outstanding under one lock, progress health+exhaustion under one lock.

The naive version (moving the report instant into `poll()`'s last critical
section without a guard) was immediately killed by two load-bearing oracles:
`a_zero_op_after_sweep_is_immediate_residue` and
`zero_op_from_another_thread_during_pass_is_served`. The safe version needs a
new `dispatch_state_version_` atomic: bumped release under `dispatch_mtx_` at
eight cross-thread mutation points, re-checked acquire by the consumer, which
falls back to a locked recompute on any bump. `SLUICE_K3A_MUTANT_TAIL_STALE_IGNORES_VERSION`
is killed by both `uring_owed_residue_test` and `uring_external_loop_test`
(BASE GREEN → MUTANT RED → RESTORED GREEN).

Results (15 interleaved AB/BA pairs, full binary):

| Case | Mode | CPU | wall | p50 | p95 | p99 | negative CPU pairs |
|---|---|---:|---:|---:|---:|---:|---:|
| W02-uring-4k-d1 (largest signal) | fusion vs full | **−1.00 %** [−3.78,+1.89] | −0.98 % | −1.97 % | −1.50 % | −1.43 % | 9/15 |
| W02-uring-4k-d1 | A/A | +0.40 % [−2.43,+2.49] | +0.39 % | +0.98 % | — | +0.59 % | — |
| W02-uring-4k-d4 | fusion vs full | +0.54 % [−0.92,+3.45] | +0.52 % | −2.73 % | +3.54 % | +1.04 % | 6/15 |
| W02-uring-4k-d4 | A/A | +0.19 % [−4.57,+3.28] | +0.27 % | +0.38 % | — | +0.78 % | — |

The fusion **median** (−1.00 %) is inside the A/A median (+0.40 %) and the A/A
range [−2.43,+2.49], but the fusion range extends to −3.78 %, below the A/A
lower bound; "inside the band" must therefore be read as "the median is inside",
not "the whole effect is bracketed". The effect is 4.8× below the campaign
4.79 % band. Cadence guard: d1 `poll_calls/op` 1.4053 → 1.4060 and
`wait_calls/op` 1.0 → 1.0 (d4 0.4965 → 0.4928), so the CPU difference is not a
pass-cadence artefact.

Per-fusion lock round-trip ≈55 cycles — the same order as an uncontended fast
path. E-B is refuted.

## 10. Verdict

`NO_CHANGE_JUSTIFIED`. K3-a is withdrawn: the largest-signal shape's median is
inside the A/A median/range (its own range is wider) and 4.8× below the
campaign 4.79 % band, and a safe implementation would require a new
derived-consistency mechanism (`dispatch_state_version_` + 8 bump sites) that
the measured effect cannot justify. K3-b is withdrawn: its premise (significant
cross-thread `access_mtx_` contention) is contradicted by the
futex/context-switch measurements, and the K4 W-03 results show the real cost
sits in the driver-pass mechanism, not in the submitter's lock.

The mechanism justification and the measurement conclusion close in the same
direction, which is the strongest form of a negative result.

## 11. Limitations

- Warm tmpfs only. On a real device completions take longer, parks actually
  happen, and the park fixed cost (2 syscalls + 4 locks per wake) would become
  real. The K3 accounting framework transfers; the numbers do not.
- Cancel-pending and shutdown forms were not load-measured.
- The W-03 harness's own idle passes are not separated from library idle passes.
- Saturate latency is not instrumented.
