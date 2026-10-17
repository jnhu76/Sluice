# G0 (#403) performance baseline — preregistration

```text
[SLUICE-G0-403]
REPORT_VERSION = g0-1
BASE_SHA = dcc46266709e98d760b3be2957d18c09e1e55952
EVIDENCE_SHA = see COMMIT.txt in this directory (harness + matrix freeze)
STAGE = G0 admission and preregistration
```

This document freezes the measurement design *before* any measured repetition was
executed. The harness, this matrix and the runner script were committed together
as the freeze commit; the measured runs follow it. No case parameter was adjusted
after a measured result was seen.

## 1. Baseline and authority boundary

| Line | Authority |
|---|---|
| Normative contract | `docs/explicit-io-v1-final-decision.md` (v1-r4 root) |
| Implementation under measurement | `BASE_SHA` above (tree equals the recorded post-merge baseline `9a501d1c8839c2f1375d48937c9bf4df18b93b26`) |
| Correctness | The established test suite, mutants and formal models already in the tree; G0 re-runs none of them as performance evidence and adds no new semantic authority |
| Performance | The G0 measurements recorded in this directory |
| Optimization | G1, not started; G0 changes no production code |

Baseline note: `origin/master` had advanced from the recorded post-merge baseline
`9a501d1c` to `dcc46266` when this work started. The two commits have *identical
trees* (`7c3de3ab1fe66af043243758d29e3bfabf895321`); the three intervening commits
are merges only. The head SHA is therefore recorded as `dcc46266` rather than
silently substituting the older one.

W-04 (`StackfulIoHost`) is **UNSUPPORTED** for this baseline: it is not measured,
nothing in G0 depends on it, and G0 does not touch its error semantics or release
status.

## 2. Measurement host (measured, not assumed)

Full machine-readable capture: `host-manifest.txt` (script: `bench/host_manifest.sh`).

| Item | Measured value |
|---|---|
| Host | WSL2 on Windows 10.0.26200.9711; WSL 3.0.3.0; kernel `6.18.54.1-microsoft-standard-WSL2` |
| Distribution | Ubuntu 26.04.1 LTS |
| CPU | **AMD Ryzen 7 5800H with Radeon Graphics** (Zen 3), 8 logical CPUs = 4 cores x 2 SMT, 3193 MHz reported, hypervisor Microsoft |
| CPU limits | `processors=8` in `.wslconfig`; no cpufreq driver (governor NOT_AVAILABLE); `nproc`=8 |
| Memory | 16 GiB configured (`memory=16GB`); 13 GiB available at capture; 8 GiB swap (`/dev/sdc`), 0 used |
| cgroup | v2, `memory.max=max`, `cpu.max=max 100000` (no quota) |
| Page-cache reclaim | `autoMemoryReclaim=gradual` in `.wslconfig` |
| Toolchain | gcc/g++ 15.2.0 (Ubuntu 26.04); xmake v2.9.7; C++20; `-m release` = `-O3 -DNDEBUG`, stripped |
| io_uring | liburing 2.14-1 (`/usr/include/liburing.h`, `liburing.so.2`); runtime probe passes: `io_uring_queue_init` OK, opcodes READ/WRITE/FSYNC/STATX/OPENAT supported, real submit/reap OK |
| Filesystem (hot) | `/tmp` = **tmpfs** (`size=8189792k`), RAM-backed by construction |
| Filesystem (storage attempt) | `/` and `$HOME` = ext4 on `/dev/sdd` (1 TB "Virtual Disk", `ROTA=1`) inside the WSL2 VHDX |
| Page-cache control | `drop_caches` **not writable**, no passwordless `sudo`; only per-file `posix_fadvise(DONTNEED)` is available |
| Profiling | `perf` present, `perf_event_paranoid=2` (user-space counters only); `strace` present; `pidstat` NOT_INSTALLED |

The issue text described the execution host as a 12th-generation Intel Core i7
with roughly 10 GB available memory. The measured host is an **AMD Ryzen 7
5800H** with 13 GiB available. This preregistration records the measured host;
every throughput and latency number in the report belongs to this machine and is
not transferable.

## 3. Measurement rules applied to every case

* Warmup: **2 repetitions**; measured: **7 repetitions**. Rationale: seven
  independent repetitions give a distribution (min/max/spread) over the whole
  case rather than a single average, and two warmup repetitions let page-cache
  residency, thread creation and first-touch effects settle. Every repetition is
  retained in the raw JSONL; nothing is discarded.
* Alternation: within one repetition round the case order is rotated
  (`order[i] = (i + round) mod n`), so no case is permanently first and
  comparable cases are separated by other work. Both backends run inside the
  same process for W-02/W-03, so ThreadPool and io_uring cases alternate within
  every round.
* Throughput phase: one uninstrumented timed region per repetition (no clock call
  inside the loop). Latency: a separate instrumented pass over the same
  operation/offset pattern; its cost is quantified by `CAL-clock-overhead`.
* Every measured repetition must pass its correctness oracle; a failing
  repetition is reported as failing and never dropped.
* Rejected/short/failed operations are counted and reported, never silently
  retried into a success.

## 4. Workload design (root W-01/W-02/W-03)

### W-01 — direct execution (`bench_w01_direct.cpp`)

Uses only `sluice::File` and `sluice::blocking::*`: no `AsyncIoContext`, no
worker, no request table, no progress source (root W-01, INV-02). `CAL-clock-overhead`
is a calibration case, not a workload case.

| CASE_ID | Operation | BLOCK_SIZE | REQUEST_COUNT | FILE_SYSTEM | CACHE_POLICY |
|---|---|---|---|---|---|
| `CAL-clock-overhead` | two steady_clock readings | n/a | 200000 pairs | n/a | n/a |
| `W01-open_close` | `File::open(open_existing)` + explicit `close` | 0 | 2000 | tmpfs | metadata only |
| `W01-file_info` | `blocking::file_info` + `size` check | 0 | 100000 | tmpfs | metadata only |
| `W01-read_at-4k-seq` | `read_at` sequential | 4 KiB | 100000 | tmpfs | page-cache resident |
| `W01-read_at-4k-rand` | `read_at` seeded random offsets | 4 KiB | 100000 | tmpfs | page-cache resident |
| `W01-read_at-64k-seq` | `read_at` sequential | 64 KiB | 4000 | tmpfs | page-cache resident |
| `W01-read_at-1m-seq` | `read_at` sequential | 1 MiB | 512 | tmpfs | page-cache resident |
| `W01-write_at-4k-seq` | `write_at` sequential | 4 KiB | 100000 | tmpfs | page-cache resident |
| `W01-sync_data-4k` | `write_at` then `sync_data` | 4 KiB | 200 | tmpfs | barrier timed alone |
| `W01-sync_all-4k` | `write_at` then `sync_all` | 4 KiB | 200 | tmpfs | barrier timed alone |
| `W01-read_exact_at-4k` | `read_exact_at` composition | 4 KiB | 50000 | tmpfs | page-cache resident |
| `W01-write_all_at-4k` | `write_all_at` composition | 4 KiB | 50000 | tmpfs | page-cache resident |
| `W01-read_at-64k-seq-cold` | `read_at` sequential | 64 KiB | 4000 | ext4 | `fadvise(DONTNEED)` each repetition |
| `W01-read_at-1m-seq-cold` | `read_at` sequential | 1 MiB | 512 | ext4 | `fadvise(DONTNEED)` each repetition |
| `W01-sync_data-4k-disk` | `write_at` then `sync_data` | 4 KiB | 200 | ext4 | barrier timed alone |
| `W01-sync_all-4k-disk` | `write_at` then `sync_all` | 4 KiB | 200 | ext4 | barrier timed alone |

**Correctness oracle (W-01).** Every file is filled with a fixed 64 KiB periodic
byte pattern. After the timed throughput phase, a separate verification pass
re-reads *every* offset used by the case and compares the returned bytes against
the pattern (`memcmp` per 64 KiB segment, first differing absolute position and
bad-segment count reported). For write cases, the same pass reads the data back
from the file. The timed phase never verifies inline, so verification cannot
inflate or deflate the throughput number.

### W-02 — bounded RequestScope pipeline (`bench_w02_pipeline.cpp`)

Bounded sliding window over `RequestScope` + `AsyncIoContext` +
`ThreadPoolBackend`/`UringAsyncBackend`. Each in-flight request owns its own
destination buffer (LIFE-01 forbids overlapping destinations). Submit while the
window has room; otherwise `wait_for` the oldest ticket, `take` its result, and
reuse that buffer. Admission rejection at the bound is counted (`would_block`),
not treated as an error.

| CASE_ID | BACKEND | BLOCK_SIZE | REQUEST_COUNT | CONCURRENCY = QUEUE_DEPTH | WORKER_COUNT | SCOPE_CAPACITY |
|---|---|---|---|---|---|---|
| `W02-threadpool-4k-d1-w1` | threadpool | 4 KiB | 20000 | 1 | 1 | 1 |
| `W02-uring-4k-d1` | uring | 4 KiB | 20000 | 1 | n/a | 1 |
| `W02-threadpool-4k-d4-w4` | threadpool | 4 KiB | 20000 | 4 | 4 | 4 |
| `W02-uring-4k-d4` | uring | 4 KiB | 20000 | 4 | n/a | 4 |
| `W02-threadpool-4k-d16-w4` | threadpool | 4 KiB | 20000 | 16 | 4 | 16 |
| `W02-uring-4k-d16` | uring | 4 KiB | 20000 | 16 | n/a | 16 |
| `W02-threadpool-4k-d64-w4` | threadpool | 4 KiB | 20000 | 64 | 4 | 64 |
| `W02-uring-4k-d64` | uring | 4 KiB | 20000 | 64 | n/a | 64 |
| `W02-threadpool-4k-d16-w1` | threadpool | 4 KiB | 20000 | 16 | 1 | 16 |
| `W02-threadpool-4k-d16-w8` | threadpool | 4 KiB | 20000 | 16 | 8 | 16 |
| `W02-threadpool-64k-d16-w4` | threadpool | 64 KiB | 2000 | 16 | 4 | 16 |
| `W02-uring-64k-d16` | uring | 64 KiB | 2000 | 16 | n/a | 16 |
| `W02-threadpool-4k-d16-w4-saturate` | threadpool | 4 KiB | 200 rounds x 16 | 16 | 4 | 16 |
| `W02-uring-4k-d16-saturate` | uring | 4 KiB | 200 rounds x 16 | 16 | n/a | 16 |
| `W02-threadpool-4k-d4-w4-scope64` | threadpool | 4 KiB | 20000 | 4 | 4 | **64** |
| `W02-threadpool-4k-d16-w4-scope64` | threadpool | 4 KiB | 20000 | 16 | 4 | **64** |

The two `-scope64` cases are pre-declared diagnostics: they hold the pipeline
depth fixed and raise only the scope tracking capacity, which isolates the
scope's linear per-`take` slot scan (`RequestScope::resolve_`) from the
concurrency bound.

All W-02 cases use a 64 MiB tmpfs file with a 4 KiB or 64 KiB read block.

**Correctness oracle (W-02).** A separate pipeline pass with byte verification
enabled checks every consumed buffer against the file pattern; the short-read,
non-ready and rejected-with-error paths fail the repetition. The timed pass
checks result success, exact `block_size` length, and that the bound was never
exceeded (`AsyncStats::max_outstanding`).

### W-03 — external event loop (`bench_w03_external_loop.cpp`)

Application-owned loop. A **driver thread** claims the progress owner, registers
the borrowed `progress_notification_fd()` with its own `epoll`, parks in
`epoll_wait`, acknowledges notifications, and drives bounded `poll_progress()`
passes until no immediate work remains; a pass that owes a transport retry makes
the next `epoll_wait` use a 1 ms bound instead of parking (no fd-only stranding,
no busy loop). The **submitter thread** keeps the same bounded sliding window and
parks on its own condition variable, re-checking a completion counter under the
mutex before parking. Teardown follows the documented order: `request_stop`,
`EPOLL_CTL_DEL`, `detach_progress_host()`, `shutdown(drain)`.

| CASE_ID | BACKEND | BLOCK_SIZE | REQUEST_COUNT | CONCURRENCY = QUEUE_DEPTH | WORKER_COUNT |
|---|---|---|---|---|---|
| `W03-threadpool-4k-d4-w4` | threadpool | 4 KiB | 20000 | 4 | 4 |
| `W03-uring-4k-d4` | uring | 4 KiB | 20000 | 4 | n/a |
| `W03-threadpool-4k-d16-w4` | threadpool | 4 KiB | 20000 | 16 | 4 |
| `W03-uring-4k-d16` | uring | 4 KiB | 20000 | 16 | n/a |
| `W03-threadpool-4k-d64-w4` | threadpool | 4 KiB | 20000 | 64 | 4 |
| `W03-uring-4k-d64` | uring | 4 KiB | 20000 | 64 | n/a |

**Correctness oracle (W-03).** Same two-phase scheme as W-02: a separate
byte-verifying external-loop pass over every offset.

## 5. Metrics

Collected per measured repetition (all in the raw JSONL, one record per case and
repetition):

* Throughput: `ops_per_sec`, `mib_per_sec` over the uninstrumented phase.
* Latency: `lat_min/p50/p95/p99/max/mean/stddev` over the instrumented pass
  (`lat_n` = number of samples; for W-02/W-03 this is submit-to-consume latency).
* CPU: process CPU seconds over the phase (`cpu_s`) and `cpu_util = cpu_s / wall_s`.
* Memory: `rss_kb` (resident set at phase end) and `peak_rss_kb`.
* Correctness: repetition `ok` plus the failure text.
* Pressure/counters: `submits`, `would_block`, `waits`, `passes`,
  `notifications`, `empty_wakes`, `extra_a`/`extra_b` (saturate: accepted/rejected),
  and the `AsyncStats` deltas: `stats_submit_calls`, `stats_submitted_ops`,
  `stats_poll_calls`, `stats_wait_calls`, `stats_completed_ops`,
  `stats_canceled_ops`, `stats_completion_errors`, `stats_short_completions`,
  `stats_max_outstanding`, `stats_queue_full_retries`, `stats_invalid_state`.

Not observable through the public surface, listed rather than estimated:

* `NOT_OBSERVABLE` allocation count per operation — there is no public allocation
  counter; only RSS growth and `perf`-style symbol profiling can bound it.
* `NOT_OBSERVABLE` internal mutex hold time / contention from inside the process —
  no public instrumentation exists; attributed indirectly through worker-count and
  depth comparisons plus external profiling.
* `NOT_OBSERVABLE` per-operation syscall counts from inside the process — obtained
  externally with `strace -c`, which is recorded as a separate profiling pass
  (it perturbs the run, so it never produces a baseline number).
* `NOT_OBSERVABLE` page-cache/page-fault state directly — `fadvise(DONTNEED)` is
  issued and its *effect* is judged from the hot-vs-cold latency comparison, not
  assumed.

## 6. Pre-declared analysis (what would count as a bottleneck)

These comparisons are fixed before measurement:

| ID | Question | Pre-declared discriminating comparison |
|---|---|---|
| Q1 | Does the request path cost dominate the file operation? | `W01-read_at-4k-seq` (direct) vs `W02-*/4k-d1` single outstanding on the same file and block size |
| Q2 | Does concurrency bound or worker count limit ThreadPool throughput? | depth sweep 1/4/16/64 at workers=4, then worker sweep 1/4/8 at depth=16 |
| Q3 | Is the scope's per-take slot scan a material cost? | `-scope64` diagnostic vs its depth-matched control (pure capacity change) |
| Q4 | Is io_uring batching helped by deeper queues? | uring depth sweep 1/4/16/64 vs the threadpool sweep |
| Q5 | Does the external loop add cost over the owned-driver scope? | W-03 depth-matched cases vs W-02 depth-matched cases |
| Q6 | How much wake amplification exists? | notifications and passes per operation vs depth (W-03), poll/wait calls per operation (both) |
| Q7 | Is the storage path reachable at all? | `W01-*-cold` (ext4 + fadvise) vs `W01-*-seq` (tmpfs) latency and throughput |
| Q8 | What does the timer instrumentation cost? | `CAL-clock-overhead` vs the case's own p50 |

A candidate becomes `G1_CANDIDATE_READY` only with (a) a measured cost on a
representative workload, (b) a discriminating experiment that separates it from
the alternative explanations listed for it, and (c) a stated proof-preservation
obligation. A profiler hotspot without (b) stays at `MORE_EVIDENCE_REQUIRED`.

## 7. Environment limitations disclosed up front

1. **Virtualized storage.** The ext4 device is a WSL2 `Virtual Disk` with
   `ROTA=1`. The physical medium (host SSD/HDD model) is not observable from
   inside the distribution, and the 9p/VHDX layer adds an unmeasured term. Any
   ext4 number is *WSL2-virtualized storage*, never bare-metal device
   throughput.
2. **Page-cache eviction is not forceable.** `drop_caches` needs root, which is
   unavailable. Cold-path cases use per-file `fadvise(DONTNEED)` and must be read
   as *eviction attempted*; if they land on the hot numbers, eviction did not
   take effect for that file and the honest conclusion is "no storage path
   proven".
3. **tmpfs is RAM.** The hot cases measure the API and page-cache path with no
   storage involvement by construction; they are not disk numbers.
4. **`autoMemoryReclaim=gradual`** may reclaim page cache asynchronously,
   introducing variance in long runs.
5. **Shared laptop.** Other Windows/WSL2 workloads can steal CPU and memory
   bandwidth; load snapshots are captured before and after the run but are not a
   controlled environment.
6. **`perf_event_paranoid=2`.** Only user-space counters are available; no
   kernel-level counters, no CPU-wide sampling. Hardware counters are available
   for our own process only.
7. **SMT + no cpufreq control.** 8 logical CPUs over 4 physical cores; the
   governor is not exposed, so conclusions about thread scaling must allow for
   SMT sharing rather than asserting core-parallel speedup.
