# Sluice flame-graph profiling (#403 G1-R1 input)

```text
[SLUICE-403-FLAMEGRAPH]
REPORT_VERSION = 1
STATUS = evidence only — no production change, no contract change
AUTHORITY = non-authoritative for v1 conformance; input evidence for #403 Phase G1-R1
BASE_SHA = 10aaf58b19092d44bfdc31c2761b90636d3a1518 (master)
HOST = Intel Xeon E5-2666 v3, 20 logical CPUs (10 cores x 2 SMT), 62 GiB, 1 NUMA node
KERNEL = 7.2.9-201.fc44.x86_64
SCOPE = attribution of CPU time for representative W-01/W-02/W-03 invocations
NOT_A_BASELINE = throughput/latency here come from sampled runs; use the G0/G1 clean
  runs for throughput or latency claims
```

This directory records a one-off profiling investigation: where the CPU cycles of
representative Sluice invocations actually go. It changes no product code and
asserts nothing about the v1 conformance ledger. The measurements are the input
that #403 Phase G1-R1 needs; they are not a substitute for the frozen native
baseline that G1-R0 must still capture.

## 1. Method

Build a release-equivalent profile with symbols and frame pointers (the shipped
release mode strips symbols, so a separate build directory is used):

```bash
xmake f -m releasedbg -o build-bench-prof --liburing=y \
    --cxflags="-O3 -DNDEBUG -g -fno-omit-frame-pointer" -y
xmake build -g bench -y
```

Capture (root is required for kernel stacks; `perf_event_paranoid=2` limits an
unprivileged perf to user-space samples and `/proc/kallsyms` is zeroed for
unprivileged readers):

```bash
sudo SLUICE_ROOT=$PWD docs/evidence/403-flamegraph-profiling-20261015/tools/capture_perf.sh
sudo docs/evidence/403-flamegraph-profiling-20261015/tools/fold_stacks.sh
sudo docs/evidence/403-flamegraph-profiling-20261015/tools/count_syscalls.sh
```

`capture_perf.sh` runs `perf record -F 4999 -g --call-graph fp` per case with
`G0_CASE_FILTER=<case>` so exactly one case runs per process. `fold_stacks.sh`
pipes `perf script` through FlameGraph's `stackcollapse-perf.pl --kernel`
(`--kernel` annotates kernel frames with `_[k]`); `count_syscalls.sh` counts
syscalls with bpftrace's in-kernel aggregation, which — unlike `perf trace`, whose
ring buffer lost events under this syscall rate — cannot lose samples.

Report generation (no root needed):

```bash
git clone https://github.com/brendangregg/FlameGraph \
    docs/evidence/403-flamegraph-profiling-20261015/tools/flamegraph
cd docs/evidence/403-flamegraph-profiling-20261015
python3 tools/report_self_cost.py     # svg/*.svg, data/SUMMARY.md
python3 tools/report_attribution.py   # data/ATTRIBUTION.md
python3 tools/report_syscalls.py      # data/SYSCALLS.md
```

The FlameGraph scripts are CDDL-1.0 and are deliberately not vendored into this
MIT repository. On a host whose perl lacks the core `open` pragma, replace
`use open qw(:std :utf8);` in `flamegraph.pl` with explicit `binmode` calls.

Sampling is **cycle-weighted**: a flame graph's width is the share of CPU cycles,
not the share of samples.

### 1.1 What the profile actually contains

`bench_w02_pipeline.cpp:run_one_repetition` runs each recorded repetition three
times: the timed throughput pass, the correctness-oracle pass (`verify=true`) and
the instrumented latency pass. One recorded `ops=20000` repetition therefore
executes 60000 operations, which `strace -c -f` confirms directly (60006
`pread64`, 60003 `write` for one W-02 repetition). Consequences:

* the flame graphs cover all three passes, so they include harness cost
  (`verify_pattern` memcmp, latency-pass timestamps) alongside the pipeline;
* every syscall figure in `data/SYSCALLS.md` is normalised to one pipeline
  execution (`measured_ops x 3`), so `per op` is comparable with the reported
  throughput.

## 2. Measured result

Medians over the measured repetitions of each sampled run (2 warmup + 20/40
measured, one case per process). `CPU/op` is process CPU seconds per operation
across all threads.

| Case | Throughput | CPU/op | p50 | p99 | syscalls/op |
|---|---|---|---|---|---|
| W-01 direct `read_at` 4 KiB | 931k ops/s | 1.08 µs | 1.1 µs | 1.5 µs | 1.10 |
| W-02 io_uring 4 KiB depth=16 | 338k ops/s | 7.63 µs | 45 µs | 63 µs | 1.10 |
| W-02 ThreadPool 4 KiB depth=16 workers=4 | 243k ops/s | 14.16 µs | 64 µs | 103 µs | 9.66 |
| W-02 ThreadPool 4 KiB depth=16 workers=1 | 200k ops/s | 8.99 µs | 75 µs | 123 µs | 8.01 |
| W-02 ThreadPool 4 KiB depth=16 workers=8 | 217k ops/s | 20.79 µs | 72 µs | 122 µs | — |
| W-02 ThreadPool 64 KiB depth=16 workers=4 | 197k ops/s | 22.94 µs | 80 µs | 122 µs | — |
| W-03 external loop, ThreadPool 4 KiB depth=16 | 128k ops/s | 26.10 µs | 86 µs | 172 µs | 11.67 |
| W-03 external loop, io_uring 4 KiB depth=16 | 232k ops/s | 11.37 µs | 50 µs | 84 µs | — |

### 2.1 ThreadPool 4 KiB: the cost is mutex/futex handoff, not the data copy

`W02-threadpool-4k-d16-w4`, cycle shares (`data/ATTRIBUTION.md`):

| bucket | share |
|---|---|
| kernel (including syscall machinery) | 72.1% |
| libc pthread / futex (user space) | 14.2% |
| Sluice user space | 6.5% |
| other (harness verification/timing included) | 5.0% |
| libc mem* | 1.1% |

Kernel cycles charged to the operation that entered the kernel:

| detail | share |
|---|---|
| `futex_wake` (mutex unlock slow path) | 21.5% |
| `futex_wait` (mutex lock slow path) | 19.1% |
| `pread` (the actual 4 KiB read) | 11.8% |
| `futex_wait` (timed, condition variable) | 8.0% |
| `pthread_mutex_unlock` (user space) | 6.2% |
| `pthread_mutex_lock` (user space) | 5.8% |
| `clone` (thread creation, per repetition) | 5.4% |
| `futex_wake` (condition variable) | 4.8% |

Roughly half the cycles go to mutex acquisition and release (user-space fast path
plus kernel futex slow path); moving the 4 KiB payload (`rep_movs_alternative`)
is about 4%. The syscall census agrees: 9.66 syscalls/op, of which 7.46 are
`futex` (worker 5.10, main 2.36), plus one `pread64` and one `eventfd` `write`
per operation.

This is also the mechanism behind the worker-count inversion: CPU/op rises from
8.99 µs (workers=1) to 14.16 µs (workers=4) to 20.79 µs (workers=8) while
throughput peaks at workers=4 (243k) and falls back at workers=8 (217k).

### 2.2 io_uring: 36% of cycles inside `io_uring_enter`, one enter per operation

`W02-uring-4k-d16`: the application issues 1.10 syscalls/op (essentially all
`io_uring_enter`), yet that syscall's kernel path holds 36.1% of all cycles.
The kernel io-wq worker threads (`iou-wrk-*`) hold another 34.6%
(`rep_movs_alternative` 6.1%, `shmem_file_read_iter` 2.2%, spin locks above 7%).
Submission is not batched — one enter per operation — so submit/reap cost is the
same order as the data path. CPU/op is still half the ThreadPool figure
(7.63 µs vs 14.16 µs), which is consistent with the futex/mutex work that the
io_uring path does not pay.

### 2.3 W-03 external loop: two extra thread handoffs

`W03-threadpool-4k-d16-w4`: 11.67 syscalls/op (worker 6.47 futex, main 2.60
futex, plus `epoll_wait` 0.13) and 26.10 µs CPU/op — 1.8x the W-02 ThreadPool
figure. The extra cost is the driver thread parked in `epoll_wait` plus the
submitter parked on its own condition variable.

### 2.4 The direct path is an order of magnitude cheaper

W-01 `read_at` costs 1.08 µs CPU/op at p50 1.1 µs, with 86.5% of cycles in the
kernel (`pread` alone 79.99%) and under 3% in Sluice user-space code. The
async/pipeline machinery raises the CPU cost of the same 4 KiB read to about
14 µs (ThreadPool) or 7.6 µs (io_uring).

## 3. Limitations

1. **Sampled runs, not a baseline.** `perf record` at 4999 Hz adds overhead; the
   throughput and latency columns are therefore not comparable with the G0 v2 or
   any G1 clean run. Syscall counts (bpftrace, in-kernel aggregation) have much
   lower overhead but are still instrumented runs.
2. **Three passes per repetition** — see §1.1. Harness verification and latency
   instrumentation are inside every flame graph.
3. **Symbolisation.** At `-O3`, identical-code folding and inlining occasionally
   attribute a frame to a neighbouring symbol (a wide frame may read
   `bench::CaseMeta::~CaseMeta`); shares are unaffected. Kernel frames need root
   to resolve through `/proc/kallsyms`.
4. **Per-process capture.** ThreadPool workers belong to the profiled process and
   are captured with it; io_uring's io-wq threads are kernel threads and are
   reported separately as `kernel (io-wq worker)` rather than being folded into
   the submitting process.
5. **`perf trace` was rejected as an instrument.** Its ring buffer reported
   thousands of lost events at this syscall rate, so its counts were discarded
   and bpftrace counting was used instead. The discarded output is not committed.
6. **The raw `perf.data` files are not committed** (about 371 MB uncompressed).
   `MANIFEST.sha256` records their names, sizes and SHA-256 so the captures can be
   verified if reproduced or archived separately. `data/folded/` holds the
   gzipped folded stacks, which are sufficient to redraw every flame graph.
7. **SVG regeneration is not byte-identical.** `flamegraph.pl` iterates a Perl
   hash internally, so the order of same-level frames can vary between runs even
   for identical input; frame content and widths do not. The three derived tables
   (`SUMMARY.md`, `ATTRIBUTION.md`, `SYSCALLS.md`) are deterministic and
   byte-identical across runs.

## 4. Contents

```text
REPORT.md            this report
MANIFEST.sha256      SHA-256 of every committed artifact + the uncommitted perf.data
data/perf/           records and stdout of the 11 perf-record runs
data/syscalls/       bpftrace syscall counts, records and stdout of the counting runs
data/folded/         gzipped folded stacks (FlameGraph input; regenerates every SVG)
data/SUMMARY.md      per-case leaf self-cost ranking
data/ATTRIBUTION.md  bucket attribution, plus the per-thread-role split
data/SYSCALLS.md     syscalls per operation, split by thread role
svg/                 full interactive flame graphs: <case>.svg and <case>.threads.svg
svg-compact/         narrower copies used by the inline single-file view
tools/               capture, fold, count and report scripts (see §1)
```
