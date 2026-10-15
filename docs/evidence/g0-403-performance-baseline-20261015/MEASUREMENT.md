# G0 (#403) measured baseline and bottleneck evidence

```text
[SLUICE-G0-403]
REPORT_VERSION = g0-1
BASE_SHA = dcc46266709e98d760b3be2957d18c09e1e55952
EVIDENCE_SHA = 2a5b85f4691a3d3189b273893f02432368ec5542
STAGE = B — measurement and bottleneck evidence
```

Preregistration (frozen before any measured repetition): `PREREGISTRATION.md`.
All numbers below are medians over **7 measured repetitions** per case, with 2
warmup repetitions, on the host recorded in `host-manifest.txt`
(AMD Ryzen 7 5800H, 8 logical CPUs = 4 cores x 2 SMT, WSL2 kernel
6.18.54.1-microsoft-standard-WSL2). Hot cases use tmpfs with explicit
`FileInitialContents::truncate`; nothing here is a bare-metal storage figure.

## 1. What executed, and the integrity check

| Run | Binary profile | Cases | Records | Measured reps/case | Failures |
|---|---|---|---|---|---|
| `raw/w01.jsonl` | no-ring (core only) | 16 (incl. 1 calibration) | 144 | 7 | 0 |
| `raw/w02.jsonl` | ring | 16 (14 frozen + 2 diagnostics) | 144 | 7 | 0 |
| `raw/w03.jsonl` | ring | 6 | 54 | 7 | 0 |
| `raw/w02-noring-notrun.jsonl` | no-ring control | 16 | 16 | 1 | 6 NOT_RUN (expected) |
| `raw/w03-noring-notrun.jsonl` | no-ring control | 6 | 6 | 1 | 3 NOT_RUN (expected) |

Integrity check over the primary runs (recomputed from the raw JSONL):
**266 measured records, 76 warmup records, 7 measured repetitions for every case,
zero failures, and `AsyncStats::max_outstanding` never exceeded the declared
`queue_depth` in any record.** Failing or skipped samples were not omitted
because there were none in the primary runs; the only `ok=false` records are the
9 deliberate `NOT_RUN_WITH_REASON` io_uring records in the no-ring control.

Both backends ran in ONE process for W-02/W-03, so ThreadPool and io_uring cases
alternate inside every repetition round (`order[i] = (i + round) mod n`). The
no-ring control proves the io_uring cases report `NOT_RUN_WITH_REASON: io_uring
backend not present in this build profile` instead of passing or silently
falling back to ThreadPool.

Reproduce: `bench/build_profiles.sh release && bench/run_matrix.sh <outdir> 2 7`
(the build script asserts each binary's profile from its dynamic dependencies:
only the ring build depends on `liburing.so.2`, and `bench_w01_direct` is
byte-identical across profiles because W-01 has no async dependency).

Raw evidence hashes (also in `logs/`):

```text
1704e4b86f90ef5bb71dbfa3023f8cc7fbfb32e2941d18f94fb14ea2729c2dfa  raw/w01.jsonl
81882e21b8227e94ba7711273f871665e357bdcf3135c6f9ddc4700d153755d1  raw/w02.jsonl
c2feb808c86c66bd072b84c36ce005ae3d887b1ba78619f80ff8fbbe8ea9213a  raw/w03.jsonl
db57bdf61583367322253149ad8db70d305ed2945c47692fbe62d6c5ef61bc1e  raw/w02-noring-notrun.jsonl
9e60c65d61213f1678a940258f88e877942415a236a957ab5df4dddc0ad3c006  raw/w03-noring-notrun.jsonl
```

## 2. W-01 — direct execution (root W-01, INV-02)

One uninstrumented timed phase per repetition; every measured repetition also
passed a verification pass that re-read **every** exercised offset and compared
the bytes against the file pattern (2 passed readbacks for the two write cases,
including the two `-disk` cases).

| Case | ops/s (median) | spread | CV | p50 ns | p95 ns | p99 ns | max ns | CPU s per op |
|---|---|---|---|---|---|---|---|---|
| `CAL-clock-overhead` | 21 ns per `steady_clock` read | — | — | — | — | — | — | — |
| `W01-file_info` | 2,076,194 | 1.2% | 0.4% | 501 | 511 | 561 | 39,707 | 0.48 µs |
| `W01-read_exact_at-4k` | 1,554,720 | 3.5% | 1.3% | 641 | 761 | 1,212 | 38,044 | 0.64 µs |
| `W01-read_at-4k-seq` | 1,288,004 | 2.3% | 1.0% | 711 | 1,202 | 1,573 | 47,031 | 0.78 µs |
| `W01-write_all_at-4k` | 1,224,152 | 3.0% | 1.1% | 802 | 1,012 | 1,463 | 45,347 | 0.82 µs |
| `W01-sync_all-4k` (tmpfs) | 1,086,868 | 4.9% | 1.7% | 952 | 952 | 962 | 1,252 | 0.94 µs |
| `W01-sync_data-4k` (tmpfs) | 1,069,341 | 25.9% | 9.4% | 962 | 962 | 982 | 1,323 | 0.95 µs |
| `W01-read_at-4k-rand` | 1,036,247 | 2.6% | 1.0% | 932 | 1,463 | 2,054 | 64,084 | 0.97 µs |
| `W01-write_at-4k-seq` | 979,494 | 12.2% | 4.2% | 941 | 1,453 | 1,963 | 69,444 | 1.02 µs |
| `W01-open_close` | 372,737 | 1.5% | 0.6% | 2,675 | 2,685 | 2,775 | 17,373 | 2.68 µs |
| `W01-read_at-64k-seq` | 143,574 | 7.9% | 2.8% | 7,034 | 9,058 | 15,029 | 49,956 | 6.97 µs |
| `W01-read_at-1m-seq` | 11,365 | 4.2% | 1.5% | 82,739 | 113,550 | 131,655 | 177,923 | 88.0 µs |
| `W01-read_at-64k-seq-cold` | 68,624 | 40.6% | 13.3% | 4,669 | 7,214 | 11,453 | 48,664 | 14.3 µs |
| `W01-read_at-1m-seq-cold` | 3,600 | 7.9% | 3.1% | 61,889 | 99,972 | 133,698 | 181,190 | 267 µs |
| `W01-sync_data-4k-disk` | 1,367 | 5.7% | 1.9% | 709,069 | 860,151 | 1,152,993 | 1,344,705 | 31 µs (3.0% util) |
| `W01-sync_all-4k-disk` | 1,058 | 5.3% | 2.0% | 724,814 | 1,708,164 | 1,988,564 | 2,408,523 | 40 µs (3.9% util) |

Facts established by W-01:

* **Direct execution is a genuine single-syscall path.** `strace -c` on
  `read_at-4k-seq` counts **1.0002 `pread64` per operation and no other syscall**
  above noise. The p50 of 711 ns for a 4 KiB tmpfs read is consistent with that.
* **Durability is measured separately and is not a read/write cost.** On tmpfs
  the barrier is ~0.95 µs because tmpfs has no backing store; on ext4 the same
  barrier is **709 µs p50**, i.e. ~740x more, while process CPU utilisation
  collapses to ~4% (the caller blocks in the kernel). The tmpfs barrier numbers
  must not be read as durability performance.
* **The storage path was reached but is virtualized.** `fadvise(DONTNEED)`
  changed the 1 MiB read from 11,365 to 3,600 ops/s (3.2x) and the 64 KiB read
  from 143,574 to 68,624 ops/s (2.1x, spread 40.6%). Something real was evicted
  and re-read, but the device is a WSL2 `Virtual Disk` (`ROTA=1`) inside a VHDX:
  the physical medium is NOT_OBSERVABLE from inside the distribution and these
  are not bare-metal device numbers.
  **Recorded-ordering defect (disclosed):** in the run that produced these
  numbers, the per-repetition fixture probe read 64 blocks (4 MiB for the 64 KiB
  case, 64 MiB for the 1 MiB case) *after* eviction, so part of the file was warm
  again before the timed phase. The harness now evicts after probing; the
  recorded cold figures were **not** re-measured and are therefore conservative
  lower bounds on the cold penalty. The defect weakens the magnitude, not the
  direction, and no conclusion in this report depends on the cold cases.
* **Composition is nearly free when the primitive completes in one step.**
  `read_exact_at` (641 ns p50) is within noise of the raw `read_at` (711 ns), so
  the SEM-05 composition rule adds no visible per-call penalty for a
  fully-satisfiable request.
* **Timer instrumentation is quantified**: 21 ns per timestamp (42 ns per
  instrumented operation). For the 4 KiB cases that is ~5-6% of the measured
  per-op latency, which is why the throughput phases never call the clock.

## 3. W-02 — bounded RequestScope pipeline

Bounded sliding window; each in-flight request owns its own destination buffer
(LIFE-01). Correctness was verified by a separate byte-verifying pass over every
offset; the bound was enforced (`max_outstanding` never exceeded `queue_depth`);
`would_block` occurred only in the deliberate saturation cases.

| Case | ops/s | spread | CV | p50 ns | p95 ns | p99 ns | CPU util | waits/op | polls/op |
|---|---|---|---|---|---|---|---|---|---|
| `W02-threadpool-4k-d1-w1` | 26,805 | 5.9% | 1.9% | 24,718 | 42,481 | 65,310 | 89.4% | 1.000 | 1.998 |
| `W02-uring-4k-d1` | 27,490 | 8.1% | 3.2% | 23,425 | 41,743 | 64,808 | 92.6% | 1.000 | 1.997 |
| `W02-threadpool-4k-d4-w4` | 106,391 | 4.6% | 1.6% | 27,433 | 45,287 | 67,149 | 174.5% | 0.278 | 0.287 |
| `W02-uring-4k-d4` | 166,682 | 18.6% | 7.8% | 16,542 | 41,019 | 69,614 | 131.7% | — | — |
| `W02-threadpool-4k-d16-w1` | **276,170** | 14.7% | 5.1% | 52,000 | 95,685 | 125,421 | 140.7% | 0.319 | 0.398 |
| `W02-threadpool-4k-d16-w4` | **112,027** | 2.8% | 1.0% | 135,550 | 188,424 | 234,307 | 177.5% | 0.072 | 0.076 |
| `W02-threadpool-4k-d16-w8` | **107,206** | 2.7% | 1.1% | 135,961 | 188,153 | 221,536 | 179.0% | 0.064 | 0.064 |
| `W02-threadpool-4k-d64-w4` | 141,999 | 21.6% | 6.8% | 558,559 | 676,131 | 849,132 | 189.0% | 0.031 | 0.035 |
| `W02-uring-4k-d16` | 303,295 | 6.8% | 2.7% | 49,185 | 67,466 | 101,877 | 172.6% | — | — |
| `W02-uring-4k-d64` | 288,424 | 34.2% | 13.4% | 350,284 | 428,113 | 499,219 | 170.3% | — | — |
| `W02-threadpool-64k-d16-w4` | 72,503 | 30.3% | 12.1% | 145,159 | 443,522 | 467,178 | 185.7% | — | — |
| `W02-uring-64k-d16` | 111,666 | 5.1% | 1.8% | 125,201 | 209,683 | 240,042 | 165.7% | — | — |
| `W02-threadpool-4k-d16-w4-saturate` | 109,039 | 9.8% | 3.4% | — | — | — | 177.5% | — | — |
| `W02-uring-4k-d16-saturate` | 272,224 | 17.5% | 6.3% | — | — | — | 171.6% | — | — |
| `W02-threadpool-4k-d4-w4` vs `-scope64` | 106,391 vs 101,727 (−4.4%) | | | | | | | | |
| `W02-threadpool-4k-d16-w4` vs `-scope64` | 112,027 vs 108,471 (−3.2%) | | | | | | | | |

Facts established by W-02:

* **The request path costs ~35x the operation it wraps at single outstanding.**
  4 KiB read p50: 711 ns (direct, W-01) vs 24,718 ns (ThreadPool, d1) —
  1,288,004 vs 26,805 ops/s. Nothing about the file access changed; 4 KiB reads
  from the same tmpfs file.
* **Adding ThreadPool workers *reduces* throughput.** At depth 16:
  **w1 = 276,170, w4 = 112,027, w8 = 107,206 ops/s**. Ideal scaling from w1 to w4
  would be ~1.1M ops/s; the measured result is 9.8x below that, and CPU
  utilisation *rises* (140.7% → 177.5% → 179.0%) while throughput falls.
  CPU per operation: 5.10 µs (w1) → 15.84 µs (w4) → 16.70 µs (w8).
* **io_uring does not show the inversion.** uring d16 = 303,295 ops/s with no
  worker pool at all; it scales up to d16 and then saturates/destabilises
  (d64 = 288,424 with 34.2% spread, and tail latency grows 7x from d16 to d64:
  p95 67,466 → 428,113 ns). More queue depth is not uniformly better.
* **The saturation cases bounded correctly.** Both saturation cases accepted the
  full window (16) and rejected the rest with `would_block` (200 rejections over
  200 rounds each), with `max_outstanding` = 16 and no error of any other kind.
* **The scope's per-`take` slot scan is small.** Raising only the scope tracking
  capacity from 4 to 64 (−4.4%) or from 16 to 64 (−3.2%) at fixed concurrency
  costs 3-4%.

## 4. W-03 — external event loop

| Case | ops/s | spread | p50 ns | p95 ns | p99 ns | notif/op | ops per notification | passes/op | CPU util |
|---|---|---|---|---|---|---|---|---|---|
| `W03-threadpool-4k-d4-w4` | 52,571 | 3.5% | 49,084 | 80,495 | 98,530 | 0.4096 | 2.4 | 0.410 | 172.5% |
| `W03-threadpool-4k-d16-w4` | 97,284 | 10.2% | 82,858 | 230,482 | 277,183 | 0.1310 | 7.6 | 0.131 | 192.5% |
| `W03-threadpool-4k-d64-w4` | 108,781 | 11.4% | 288,845 | 900,170 | 1,047,853 | 0.0467 | 21.4 | 0.047 | 197.9% |
| `W03-uring-4k-d4` | 77,123 | 4.6% | 32,803 | 59,788 | 93,229 | 0.5978 | 1.7 | 0.598 | 123.6% |
| `W03-uring-4k-d16` | 144,144 | 17.5% | 63,222 | 162,893 | 209,783 | 0.1968 | 5.1 | 0.197 | 165.3% |
| `W03-uring-4k-d64` | 129,070 | 27.0% | 337,288 | 591,456 | 700,576 | 0.0306 | 32.7 | 0.031 | 167.1% |

Facts established by W-03:

* **Wake amplification is sublinear in depth and coalescing works.** Measured
  notifications fall from 0.41 per op (depth 4) to 0.047 (depth 64) for
  ThreadPool — 2.4 to 21.4 operations per observed wake. For io_uring, 0.60 → 0.031
  (1.7 to 32.7 ops per wake). Notifications per operation *decrease* with depth,
  so no superlinear wake storm exists at these depths.
* **Every observed notification costs exactly one progress pass** (`passes/op` ==
  `notif/op` in every W-03 case), i.e. the drive loop does not re-pass after a
  wake. `empty_wakes` are recorded per repetition in the raw JSONL.
* **The external loop is measurably more expensive than the owned-driver scope at
  the same depth**: uring d16 144,144 (W-03) vs 303,295 (W-02); threadpool d16
  97,284 vs 112,027. For io_uring that is a 2.1x gap. This is a *measured gap*,
  not yet an attributed bottleneck — see B-G0-05.
* **No busy polling anywhere.** The driver parks in `epoll_wait` (unbounded when
  no transport retry is owed, 1 ms when `dispatch_retry_remains` is set), and the
  submitter parks on its own condition variable with a mutex-protected completion
  predicate. CPU utilisation is 1.2-2.0 of 8 CPUs, consistent with blocking waits
  rather than spinning.

## 5. Profiling and causal attribution

Profiling used a symbol-preserving build (`-O3 -g -DNDEBUG`, unstripped, same
defines) because the measured binaries are stripped. **Its absolute throughput is
not comparable to section 3** — it is used only for ratios, symbol attribution
and the pinning discriminator.

### 5.1 Per-operation syscall cost (strace -c, ratios only)

`strace` serialises and inflates every syscall, so only *counts per operation*
are quoted. Each measured repetition runs three passes over the same operation
pattern (uninstrumented throughput, byte-verifying oracle, instrumented
latency), so one recorded operation equals three phase-operations; all figures
below are per **phase-operation** (60,000 of them in the counted run), which is
the honest normalisation for "syscalls per 4 KiB read":

| Path | syscalls per operation |
|---|---|
| W-01 direct `read_at` | **1.00 `pread64`**, nothing else |
| W-02 io_uring d16 | **1.0001 `io_uring_enter`**, nothing else above noise |
| W-02 ThreadPool d16 (w1) | 1.00 `pread64` + 1.00 eventfd `write` + 1.99 eventfd `read` + 0.98 `poll` + **4.96 `futex`** ≈ **9.9** |
| W-02 ThreadPool d16 (w4) | 1.00 `pread64` + 1.00 eventfd `write` + 0.07 eventfd `read` + <0.02 `poll` + **3.61 `futex`** ≈ 5.7 |

The `w4` row shows the *same* single eventfd signal per completion even though
the owner called `wait_one` only 0.072 times per operation — i.e. the great
majority of signals are emitted while the owner is not parked and no wait is
outstanding. Whether each such signal was nonetheless *required* (the counter is
the persistent-readiness mechanism, so a signal written while the owner is awake
may still be consumed by the owner's next arm/acknowledge step) is not directly
observable and is exactly the obligation C1 must satisfy. The number of futex
calls is slightly lower at w4 but each blocks ~3.4x longer (28 µs → 94 µs per
call under strace), which is the contention signature used in B-G0-02.

The fd was identified with `strace -y`: `write(4<anon_inode:[eventfd]>, "\1\0…",
8)`. Thread attribution shows **3 writer threads each writing exactly 20,000
times for 20,000 operations** (one per phase/context), and a **single reader**
thread (the progress owner) doing 119,589 eventfd reads for 60,000 operations.
So: exactly one notification signal is generated per completed operation,
independent of depth, whether or not the owner is parked.

### 5.2 Hardware counters (`perf stat`, user space only)

Per operation (constant work, degrading efficiency):

| Case | cycles/op | instructions/op | cache-misses/op | IPC | branches/op |
|---|---|---|---|---|---|
| ThreadPool d16 w1 | 10,514 | 14,172 | 154 | 1.35 | 3,088 |
| ThreadPool d16 w4 | 12,811 | 14,128 | 261 | 1.10 | 3,070 |
| ThreadPool d16 w8 | 19,898 | 14,044 | 550 | 0.71 | 3,034 |

Instructions and branches per operation are **flat** across worker counts while
cycles and cache-misses grow and IPC falls — the extra cost is *stalls*, not new
code. `context-switches:u` and `cpu-migrations:u` read 0 for this process, so
that channel is **UNAVAILABLE_WITH_REASON** (not zero contention) under
`perf_event_paranoid=2`.

`perf record` DSO split: **kernel ≈ 52% (w4) / 46% (w1)** of samples,
bench+library ≈ 23% / 26%, libc ≈ 19% / 22%, vdso ≈ 3-5%. Kernel symbols are
hidden at `perf_event_paranoid=2`, but the syscall counts above show what the
kernel time is spent on: eventfd signal/drain, `poll` and futex waits.

Top resolved in-binary symbols (w4, self %, via `addr2line`):
`RequestScope::take` 2.0%, `ThreadPoolBackend::submit_read` 1.9%,
`ThreadPoolBackend::worker_loop` 1.6%, `RequestCore::occupancy()` 1.1%,
`ThreadPoolBackend::publish_one` 0.6%, `RequestCore::context()` 0.3%,
`RequestCore::accept` 0.2%. A flat profile of mutex-guarded bookkeeping — no
single hot function dominates.

### 5.3 Bottleneck records

```text
BOTTLENECK_ID = B-G0-01
OBSERVED_SYMPTOM = ThreadPool request path costs ~35x the wrapped operation at
  single outstanding (p50 24,718 ns vs 711 ns) and ~10 syscalls per operation in
  steady state at every depth.
MEASURED_COST = 1 eventfd signal write + ~2 eventfd drain reads + ~1 poll +
  ~5 futex per completed operation (d16/w1); ~1 io_uring_enter/op for the uring
  path and 1 pread/op for the direct path. 46-52% of process samples are kernel.
WORKLOAD_AND_PROFILE = W02-{threadpool,uring}-4k-*, W01-read_at-4k-seq;
  strace -c on the symbol-preserving build, perf record DSO split.
SOURCE_LOCATION = src/async/threadpool_backend.cpp (worker completion signal,
  work_cv_/work_mtx_ handshake), src/async/detail/progress_source.cpp +
  include/sluice/async/detail/progress_source.hpp (notification arm/drain),
  src/async/async_io_context.cpp (owned-driver wait path).
HYPOTHESIS = the per-operation completion->owner handshake is a fixed cost paid
  once per operation regardless of outstanding depth: the completion path
  signals the eventfd unconditionally, and every parked wait performs an
  arm/acknowledge/drain sequence that costs a poll plus ~2 reads plus futexes.
ALTERNATIVE_EXPLANATIONS = (a) the pread itself dominates -> refuted: direct
  path with the same pread costs 1 syscall and 711 ns; (b) profiler artefact ->
  refuted: the syscall counts are independent of perf; (c) slot scans -> see
  B-G0-03/B-G0-04, measured at 3-4% each.
DISCRIMINATING_EXPERIMENT = count syscalls per operation for direct vs
  ThreadPool vs io_uring at the same block size and file, at depth 16, with the
  same phase structure.
RESULT = 1 vs ~9.9 vs 1.0 syscalls per operation. The ThreadPool path pays ~9
  extra syscalls per 4 KiB read that the uring path does not pay.
CONFIDENCE = high for the cost measurement; medium for attributing the *ratio*
  of cost between the signal write and the wait handshake (the wait count depends
  on how often the owner parks).
AFFECTED_REQUIREMENTS = PROG-01, PROG-02, PROG-03, REQ-06, W-02, W-03.
PROOF_PRESERVATION_OBLIGATIONS = any change that suppresses or batches a signal
  must preserve PROG-02 persistent readiness (no-lost-wake) and must not remove a
  work obligation; requires re-validation against the progress/observer model.
```

```text
BOTTLENECK_ID = B-G0-02
OBSERVED_SYMPTOM = ThreadPool throughput inverts with worker count: w1 276,170 >
  w4 112,027 > w8 107,206 ops/s at depth 16, while CPU utilisation rises
  140.7% -> 177.5% -> 179.0% and p50 latency rises 52,000 -> 135,550 -> 135,961 ns.
MEASURED_COST = 2.5x throughput loss and 3.1x CPU per operation (5.10 -> 15.84 µs)
  from w1 to w4; ideal w4 would be ~1.1M ops/s.
WORKLOAD_AND_PROFILE = W02-threadpool-4k-d16-{w1,w4,w8}; perf stat per-op
  counters; perf record symbol attribution; strace futex per-call blocking time.
SOURCE_LOCATION = src/async/detail/request_core.cpp (single `mutex_` guarding
  accept/occupancy/publication/reclaim), src/async/threadpool_backend.cpp
  (work_mtx_ + work_cv_ on submit, dequeue and completion, core_->occupancy()
  calls at lines 587, 774, 835), src/async/async_io_context.cpp (submit wrapper
  holding the context access mutex across the backend submit).
HYPOTHESIS = the submitter and every worker share the same RequestCore mutex on
  every operation; with more workers the per-operation critical sections
  (accept, publish, occupancy, resolve) are acquired more often and from more
  threads, so the pipeline is serialised behind one lock plus contention stalls.
ALTERNATIVE_EXPLANATIONS =
  (a) CPU exhaustion -> refuted: 1.4-1.8 of 8 CPUs are used.
  (b) SMT placement / core sharing -> partially real but not causal: pinning
      w4 to 2 physical cores improves it 33% (116k -> 154k) yet it stays far
      below w1 (271k), and w1 is placement-insensitive (271k unpinned vs 255k on
      a shared core vs 256k on distinct cores).
  (c) O(capacity) slot scans under the lock -> refuted as the dominant term:
      scope capacity 4->64 costs 4.4%, core capacity 16->64 costs 4.3% at fixed
      concurrency.
  (d) The submitter is the hard ceiling -> not supported as an independent
      cause: the same submitter sustains 276,170 submissions+consumes per second
      at w1 and only 112,027 at w4.
DISCRIMINATING_EXPERIMENT = worker dose-response at fixed depth with fixed work
  per operation, plus per-op instruction/cycle/cache counters and per-call futex
  blocking time.
RESULT = instructions/op constant (14.1k) while cycles/op 10.5k -> 12.8k -> 19.9k
  and cache-misses/op 154 -> 261 -> 550 (IPC 1.35 -> 0.71); futex blocking time
  under strace rises 28 µs -> 94 µs per call; the profile's hot symbols are
  exactly the mutex-guarded core operations (take, submit_read, worker_loop,
  occupancy, publish_one, accept).
CONFIDENCE = medium-high. The inversion is unambiguous in the raw data and the
  stalls are measured; the precise split between "one lock serialises the
  pipeline" and "cache-line transfer between the lock owners" needs a
  lock-hold-time measurement that this host cannot provide (perf lock tracing is
  unavailable; no public hold-time instrumentation exists -> NOT_OBSERVABLE).
AFFECTED_REQUIREMENTS = REQ-01..REQ-06, PROG-01, THREAD-01, W-02.
PROOF_PRESERVATION_OBLIGATIONS = RequestCore's single-authority mutex is
  load-bearing for acceptance/reclaim/publication atomicity; any change to its
  granularity must preserve every REQ-02/REQ-04/REQ-05 invariant and needs the
  RequestCore/observer correspondence re-validated (#524 is the running model
  maintenance; its result is NOT evidence about any future G1 change).
```

```text
BOTTLENECK_ID = B-G0-03  (REJECTED as a bottleneck)
OBSERVED_SYMPTOM = RequestScope::resolve_ linearly scans the slot table on every
  take/wait_for, so per-operation cost was expected to grow with capacity.
MEASURED_COST = 3.2-4.4%: `-scope64` diagnostic vs its depth-matched control
  (d4: 106,391 -> 101,727; d16: 112,027 -> 108,471 ops/s).
DISCRIMINATING_EXPERIMENT = hold pipeline depth fixed, raise only the scope
  tracking capacity.
RESULT = real but small; not worth an isolated optimization on this evidence.
CONFIDENCE = high (two depth-matched controls, 7 repetitions each).
PROOF_PRESERVATION_OBLIGATIONS = n/a while rejected.
```

```text
BOTTLENECK_ID = B-G0-04  (REJECTED as a bottleneck)
OBSERVED_SYMPTOM = RequestCore::occupancy() scans every slot under the core
  mutex and is called from poll_progress, outstanding and the worker completion
  path.
MEASURED_COST = 4.3%: post-hoc diagnostic holding depth at 16 and scope at 16
  while raising the backend slot capacity 16 -> 64 (119,385 -> 114,234 ops/s over
  3 repetitions, symbol-preserving build).
DISCRIMINATING_EXPERIMENT = core capacity sweep at fixed concurrency.
RESULT = the scan length is not what makes the pipeline slow; the mutual
  exclusion around it is (see B-G0-02).
CONFIDENCE = medium-high (3 repetitions, post-hoc diagnostic, disclosed as such).
PROOF_PRESERVATION_OBLIGATIONS = n/a while rejected.
```

```text
BOTTLENECK_ID = B-G0-05  (open, MORE_EVIDENCE_REQUIRED)
OBSERVED_SYMPTOM = the external-loop integration is 1.15x (ThreadPool) to 2.1x
  (io_uring) slower than the owned-driver scope at the same depth and block size.
MEASURED_COST = uring d16: 144,144 (W-03) vs 303,295 (W-02) ops/s; threadpool
  d16: 97,284 vs 112,027.
WORKLOAD_AND_PROFILE = W03-* vs W02-* depth-matched cases; raw JSONL.
HYPOTHESIS = the loop's cost is dominated by the app-level completion handoff
  (driver wake -> mutex-protected counter -> condition variable -> submitter) and
  by one progress pass per notification, rather than by the notification itself.
ALTERNATIVE_EXPLANATIONS = (a) extra thread + condvar handoff per wake;
  (b) `poll_progress` being called once per notification where the owned-driver
  path amortises its wait; (c) the io_uring driver parking in epoll_wait rather
  than sitting in io_uring_wait_cqe, so every wake costs an extra syscall.
DISCRIMINATING_EXPERIMENT = not yet run. Needs a pass-count/notification-level
  breakdown in the loop with the app handoff removed, or a per-pass cost
  measurement. The 1 ms retry timeout path also needs its own measurement.
RESULT = not established; recorded as an open measured gap.
CONFIDENCE = low (gap measured, cause not attributed).
AFFECTED_REQUIREMENTS = PROG-01, PROG-03, PROG-04, W-03.
PROOF_PRESERVATION_OBLIGATIONS = n/a until a cause is established.
```

## 6. What is NOT_OBSERVABLE on this host, and how it could be obtained

Each row states the blocker and the non-invasive measurement that would close it;
none of these methods changes production code.

| Metric | Why not observable here | Non-invasive method that would measure it |
|---|---|---|
| Allocation count per operation | No public allocation counter exists | `LD_PRELOAD` interposition on `malloc`/`free` (test-side only), or a temporary derived counter in an instrumentation build; RSS is recorded today as a coarse substitute |
| Mutex hold time / contention directly | `perf lock` needs kernel support unavailable at `perf_event_paranoid=2`; `context-switches:u` and `cpu-migrations:u` read 0 for this process | `LD_PRELOAD` shim timing `pthread_mutex_lock`/`unlock` per thread, or a test-side instrumentation build accumulating hold time; today it is inferred from the worker dose-response, per-op stall counters and futex blocking time |
| Kernel-side symbol attribution | Kernel symbols are hidden at this `perf_event_paranoid` | `perf` with kernel symbols/`ftrace` where permitted; the kernel *share* (46-52%) is measured even though its internal split is not |
| Physical storage device behaviour | The device is a WSL2 `Virtual Disk` inside a VHDX; the host medium is not visible from inside the distribution | Re-run the same frozen matrix on a native Linux host with a known device, keeping the harness and case ids unchanged |
| Page-cache residency per page | Not exposed by the API | `mincore()` in a test-side probe, or `smaps`/`fincore` sampling between phases; today only the hot-vs-`fadvise` latency difference is evidence |
| Per-thread CPU time | `pidstat` (sysstat) is not installed | Read `/proc/<pid>/task/*/stat` from a harness-side sampler, or install sysstat |
| Segmented queueing / submission / completion latency | `AsyncStats` counts calls but attaches no timestamps, and no stage clock exists on the public surface | Test-side timestamps around the submit and consume calls in the harness, plus a derived instrumentation build for the in-between stages; the end-to-end submit-to-consume distribution is what is recorded today |
* **Segmented queueing / submission / completion latency — NOT MEASURED in G0.**
  `AsyncStats` counts calls but attaches no timestamps, and the public surface
  exposes no per-stage clock, so splitting an operation's latency into
  "queue wait / physical execution / publication" needs test-side instrumentation
  inside the producer and consumer paths. It was left unmeasured rather than
  estimated; the end-to-end submit-to-consume distribution (recorded for every
  case) is what the report uses instead.

## 7. Stability

Coefficient of variation across the 7 measured repetitions is ≤ 3% for 24 of 34
aggregated cases and ≤ 6% for 30 of 34. The four noisy cases are
`W01-read_at-64k-seq-cold` (13.3%), `W02-uring-4k-d64` (13.4%),
`W02-threadpool-64k-d16-w4` (12.1%) and `W01-sync_data-4k` on tmpfs (9.4%).
Conclusions in this report are drawn only from differences far larger than that
noise (2.5x inversion, 3.2x durability gap, 10x syscall count), never from the
noisy cases alone.
