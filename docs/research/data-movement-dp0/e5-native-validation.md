# E5 — native-Linux pre-productization validation of the DP-0 arms

Status: RESEARCH EVIDENCE (PR #570 stays draft; no product action taken).
Date: 2026-10-23. Code baseline: master `be28f275`.

The second DP-0 review (independent-review.md, "Second review round")
accepted DP-1/DP-2B/DP-3A as productization candidates conditioned on a
native-Linux validation stage: real release build, GCC 16, W-02/W-03,
ThreadPool backend, non-spinning result consumption, and the
DP-1+DP-2B+DP-3A three-arm combo, with recorded `poll_calls/op`,
completion visibility and CPU/wall at equal request counts. This document
is that stage's record. All raw JSONL, perf outputs, runner scripts,
analyzers and instruments are committed under `results/raw/e5/` and
`results/receipts/e5/`.

## Trees and provenance

Same bare-metal server as M2 (Fedora 44, kernel 7.2.9, Xeon E5-2666 v3,
20 threads, `/tmp` tmpfs 32 GiB, RLIMIT_MEMLOCK 8192 KiB,
`perf_event_paranoid=2`, no sudo; see
`docs/research/native-linux-m2/environment.md`). Five trees, all at
branch `research/dp0-data-movement-audit` commit `2a51827c` (docs-only on
top of code baseline `be28f275`), each built
`xmake f -m release -o build-e5 --liburing=y`, each 76/76 `xmake test`:

| tree | content | patch sha256 (receipts/e5/) |
|---|---|---|
| `~/Source/Sluice` | base | — |
| `wt-dp1` | + dp1 | `02f352e9…` |
| `wt-dp2b` | + dp2b | `c114bc2a…` |
| `wt-dp1dp2b` | + dp1+dp2b | `30b76d99…` |
| `wt-threearm` | + dp1+dp2b+dp3a | `54bd46b5…` |

The zop instrument was compiled per tree as
`g++ -std=c++20 -O3 -DNDEBUG -DSLUICE_HAS_LIBURING` against the tree's
static `libsluice_async`/`libsluice_core` and `liburing`.

Harness defect found by this campaign: `bench_w02_pipeline` and
`bench_w03_external_loop` declared a dead local `AsyncStats` and never
passed it to the context factory, so their `stats_delta` fields were
identically zero (upstream bench defect, independent of the DP arms).
The pollmetrics stage ran locally patched harness copies that pass a live
`AsyncStats` through `make_context`; the delta computation is untouched.

## Method

- **zop instruction instrument**: interleaved AB/BA, 15 pairs per arm
  (dp1dp2b, threearm), 5 pairs `w01ctl` negative control, `taskset -c 2`,
  `perf stat -x, -e instructions:u -e cycles:u` (user-space events only).
  Delta convention: base − arm per op; positive = instructions removed.
- **W-02**: `bench_w02_pipeline`, 4 configs × 15 interleaved pairs,
  threearm vs base, pinned `taskset -c 2`; per-op wall/cpu paired ratios
  with bootstrap 95% CI (`analyze_e5_w23.py`). Consumption is blocking
  `wait_for` — non-spinning.
- **W-03**: `bench_w03_external_loop`, 3 configs × 15 interleaved pairs,
  **unpinned** — a recorded deviation from the M2 pinning convention,
  forced by the pinned-run stall investigated in §6.
- **Attribution**: W02-uring-4k-d1 re-run against dp1-only and dp2b-only
  trees (`run_e5_attr2.sh`, 15 pairs each), alongside the earlier
  dp1dp2b pair run (`results/raw/e5/attr-d1`).
- **pollmetrics**: stats-wired harness, 5 pairs × 6 cases (W-02 pinned,
  W-03 unpinned), recording `stats_poll_calls`, driver notifications,
  empty wakes and passes per op (`analyze_e5_poll.py`).

## Results

### 1. Instruction deltas reproduce on native GCC 16 (zop)

| arm | pairs | instr/op removed (median) | pairs>0 | cycles/op Δ |
|---|---|---|---|---|
| dp1dp2b | 15 | 41.80 | 15/15 | +29.51 |
| threearm | 15 | 62.70 | 15/15 | +39.04 |
| w01ctl (negative) | 5 | −2.20 | 0/5 | +0.12 |

`THREE_ARM_COMBINED_MEASURED = 62.7 instr/op` (this host) — the
projection (52.8, WSL2/GCC 15) is superseded by a measured figure;
the negative control is clean.

### 2. W-02 wall at equal request counts (arm/base, pinned)

| case | wall ratio | 95% CI |
|---|---|---|
| W02-threadpool-4k-d16-w4 | 0.9867 | [0.9825, 0.9905] |
| W02-uring-4k-d1 | 1.0292 | [1.0139, 1.0329] |
| W02-uring-4k-d16 | 1.0024 | [0.9973, 1.0123] |
| W02-uring-64k-d16 | 0.9969 | [0.9957, 0.9985] |

### 3. The d1 regression is a dp1×dp2b interaction, not a single arm

| arm vs base, W02-uring-4k-d1 | wall ratio | 95% CI | reading |
|---|---|---|---|
| dp1 alone | 0.9821 | [0.9754, 0.9896] | improvement |
| dp2b alone | 0.9919 | [0.9841, 1.0038] | neutral |
| dp1dp2b | 1.0244 | [1.0117, 1.0306] | regression |
| threearm (+dp3a) | 1.0292 | [1.0139, 1.0329] | regression |

Neither arm regresses alone; +2.4 % appears only in combination (dp3a's
incremental ≈ +0.5 % is within noise). Productization consequence: the
dp2b PR must carry final-HEAD-on-dp1 validation as evidence, not just
same-tree validation — the stage plan already requires this; it is now
an evidence-backed requirement, and W02-uring-4k-d1 is the shape that
arbitrates it.

### 4. W-03 wall (unpinned)

| case | wall ratio | 95% CI |
|---|---|---|
| W03-uring-4k-d4 | 0.9725 | [0.9531, 0.9965] |
| W03-uring-4k-d16 | 1.0063 | [0.8969, 1.0162] |
| W03-threadpool-4k-d16-w4 | 1.0140 | [0.9209, 1.0344] |

### 5. poll_calls/op and completion visibility (stats-wired)

| case | poll/op arm | base | Δ | notif/op arm/base | empty wakes |
|---|---|---|---|---|---|
| W02-threadpool-d16-w4 | 0.1030 | 0.1024 | +0.0006 | 0 / 0 | 0 |
| W02-uring-d1 | 1.3893 | 1.3439 | +0.0454 | 0 / 0 | 0 |
| W02-uring-d16 | 0.1499 | 0.1507 | −0.0007 | 0 / 0 | 0 |
| W03-threadpool-d16-w4 | 0.1535 | 0.1525 | +0.0010 | 0.1165 / 0.1158 | 0 |
| W03-uring-d16 | 0.1704 | 0.1722 | −0.0019 | 0.1118 / 0.1124 | 0 |
| W03-uring-d4 | 0.4631 | 0.5118 | −0.0487 | 0.3955 / 0.4052 | 0 |

The claimed second-order benefit ("completion visible earlier → fewer
polls") does **not** appear on W-02 shapes; it is real only on
W03-uring-4k-d4 (−9.5 % poll passes). `empty_wakes = 0` everywhere: the
park protocol never observed a stale wake, and notifications equal
passes (every wake ran at least one pass) as designed.

### 6. The W-03 pinned-run stall — investigation record

**Symptom.** W-03 campaigns under `taskset -c 2` hung (`timeout` kills,
rc=124), on both base and threearm trees, with and without perf stat;
W-02 pinned runs were unaffected. M2 had never driven W-03 on this host
(W03_AB = NOT_RUN), so this was the first exposure.

**Reproducibility (flaky, single-CPU).** `probe_flaky.log`: cpu2 3/8
hangs, cpu3 1/8, cpu{2,3} 0/8. A later instrumented harness hung 8/15
runs on cpu2. Unpinned: 0 hangs across all campaigns. So: a race whose
window single-CPU affinity opens frequently, not a deterministic path
gap.

**Live capture at hang** (`capture_hang2.log`). Threads: main in futex,
driver thread in `epoll_wait(-1)`, an idle `iou-wrk` worker. io_uring
fdinfo: `SqHead=SqTail=CqHead=CqTail=20001` (= 1 construction probe +
20000 phase ops), no CQ overflow, `eventfd-count: 0`. Every submitted op
was completed by the kernel, reaped and published by the backend; the
kernel side was idle and is exonerated. A Sluice-free minimal repro
(liburing + registered eventfd + epoll, `receipts/e5/ring_repro.c`)
ran 30/30 clean pinned — the raw kernel path does not stall.

**Instrumented run** (`receipts/e5/bench_w03_diag2.cpp` predecessor
`bench_w03_diag.cpp`). Every hang followed a **successful** phase:
`DIAG phase-exit: failed=0 … settled=20000 completions=20000`. No
20-second completion-wait timeout ever fired. The observed main-thread
futex was `driver.join()`.

**Root cause (bench stop-protocol race, not a library defect).** The
bench calls `ctx.request_stop(drain)` **before** setting its
`shared.stop` flag. `request_stop` unconditionally wakes the progress
source (`async_io_context.cpp`: `core_->close_admission()` →
`progress_->signal()`, then `progress_->interrupt()`). The driver can
consume that wake — drain the eventfd, run its (empty) passes, loop —
re-check `shared.stop` while it is still false, and re-park in
`epoll_wait(-1)` forever. `stop = true` and the following `notify_all`
then reach nobody, and `driver.join()` blocks permanently. Single-CPU
amplification: the wake's `write(2)` is a preemption point; on one CPU
the driver runs its entire wake-drain-repark cycle while the submitter
is descheduled between `request_stop` and `stop = true`.

**Causal confirmation.** Reordering the harness to set `shared.stop`
before `request_stop` → 0/15 pinned hangs (vs 8/15 with the original
order). The in-tree `bench/bench_w03_external_loop.cpp` is fixed by this
branch with the same reorder.

**Path-coverage audit.** The library's notification paths were checked
one by one and are closed: submit-side retry advertisement
(`dispatch_after_accept` signals whenever work remains unsubmitted or a
transport retry is owed), zero-op inline publication signals, publications
enqueued by a reap are drained in the same poll pass, CQE → terminal →
publish → deliver is single-pass, the kernel eventfd is registered at
context construction (the ring is constructed eagerly before the port
attach), and `request_stop` always produces a wake. The uncovered path
was the bench's stop channel: its flag is only examined between
`epoll_wait` returns, and `epoll_wait(-1)` has no timeout. The earlier
working interpretation ("20 s timeout never fired → true deadlock inside
the wait") is corrected: phases succeeded; the timeout never needed to
fire; the hang is at `join`. The irrecoverability conclusion stands.

**Stage disposition.** Bench-only defect; W-03 numbers in §4 are
unpinned (deviation recorded in the method section). No library change
is derived from this investigation.

## Limitations

- Single host, single filesystem (tmpfs), no O_DIRECT shapes, no PMU
  cache/TLB counters (paranoid=2). Instruction deltas remain the
  verdicts; wall/cycles are context.
- W-02-uring-4k-d1's regression is real on this host and attributed to
  the dp1×dp2b interaction; it does not by itself veto productization —
  the stage gate for dp2b is final-HEAD validation, which this stage's
  attribution data now specifies precisely.
- The three-arm zop figure (62.7) is a zero-op instrument number;
  W-02/W-03 deltas are the workload-level evidence.
