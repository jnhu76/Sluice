# E1 Native-Linux Validation — W-03 Real External-Loop A/B (Gate R4)

Campaign: 15 valid interleaved AB/BA pairs per config over a REAL kernel
event loop — the driver thread parks in `epoll_wait(-1)` on the context's
real eventfd notification fd (`ctx.progress_notification_fd()`), on wake it
acknowledges and drives `poll_progress()` passes; the submitter runs the
depth window and parks on a condvar fed by delivered completions. No mocked
CQEs, no memory queues, no fabricated notifications anywhere in the perf
path. Raw: `~/sluice-e1-e5/results-w03/` (180 perf rows + 10 verify rows;
`w03-combined.jsonl` is the reducer input, sha256 recorded).

Reducer status: **w03 = PASS**.

## Paired E1/Base ratios (timed cpu_ns_per_op)

Correction (Review B P2-1): the frozen reducer computes W-03 medians by
rank-pairing (`zip(sorted(e1), sorted(base))`, `e1_verdict.py`), not by the
protocol's preregistered per-rep pairing; the two methods are reported side
by side below. Both satisfy the ≤1.03 gate; no status flips.

| Config | rep-paired median (preregistered) | rank-paired median (reducer/verdict.json) | pairs < 1 |
|---|---:|---:|---:|
| 4096-C1024-D1 | **0.8508** | 0.8587 | 15/15 |
| 4096-C1024-D8 | **0.6146** | 0.6171 | 15/15 |
| 65536-C1024-D8 | **0.9118** | 0.9206 | 14/15 |

The rank-paired values are what `verdict.json` records (the frozen reducer is
byte-reproducible and was not edited); the rep-paired column is the
preregistered statistic recomputed from `w03-combined.jsonl` at review.

All ≤ 1.03; the D8 gain carries over into the external-loop shape. Notification
coalescing is visible in every run (e.g. smoke: 20000 ops, ~4774 notifications
≈ 4.2 completions per eventfd write), so E1's scan savings convert under
K>0 delivery too.

## Verify phase (deterministic interleave obligations, both arms)

All 10 rows (5 scenarios × 2 arms) pass with `no_lost_wake=true`:

1. `idle_park_wake` — driver stays parked with no work (pass count flat),
   one submitted op wakes it exactly once and settles.
2. `zero_length` — a zero-length read settles with result 0 through the
   loop (this scenario settles synchronously on the submit path:
   notifications=0 is an observation, not a failure).
3. `burst_coalescing` — 64 instant completions deliver via 2 notifications
   (K>0 coalescing) with no lost completion.
4. `cancel_pending_pipe` — a REAL kernel cancel of a pending pipe read while
   the driver loops; settles; no lost wake; both arms.
5. `detach_shutdown` — EPOLL_CTL_DEL, detach, drain shutdown all converge
   with terminal responsibility intact.

Synchronization uses context events and bounded 10s watchdogs only; no
proof-by-sleep (the only dwell, 150ms in `idle_park_wake`, is a negative
check that the pass counter stays flat, not a synchronization proof).

## Status

`W03_AB = PASS` (real eventfd/epoll loop; 15 pairs × 3 configs; no-lost-wake
verified; performance non-regression with real gains). The stock
`uring_external_loop_test` remains green as the deterministic protocol layer
beneath this.
