# Sluice flame-graph profiling (#403 G1-R1 input)

```text
[SLUICE-403-FLAMEGRAPH]
REPORT_VERSION = 2
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

## 0. Corrections in version 2

Version 1 was reviewed for how it normalised and attributed its numbers. Seven
defects were found and fixed; the superseded tables are kept beside the
corrected ones (`data/*.superseded-v1.md`) rather than overwritten. Nothing in
§2 below rests on a superseded figure.

1. **Syscall denominator was ~10% short.** v1 divided by `measured ops x 3`,
   dropping the warmup repetition, so every per-operation figure was about 10%
   high. The denominator is now the full execution count read from each run's
   own JSONL — `(warmup + measured) reps x 3 passes x ops` — and the fixed
   startup/fixture cost is measured separately
   (`tools/measure_init_cost.sh`, 382 syscalls for W-02/W-03, 2,048 for W-01).
   Data-path syscalls now land exactly on 1.00/op where v1 showed 1.10.
2. **`count_syscalls.sh` had a line-continuation bug.** Two invocations were
   merged into `10count_case`, so re-running the committed script would have
   dropped the W-03 case and passed `reps=10count_case` to the io_uring case.
   The committed raw data predates the bug (all five cases are present with
   1 warmup + 10 measured repetitions); the script is fixed.
3. **Capture verification was missing.** `count_syscalls.sh`, `capture_perf.sh`
   and the new measurement scripts now record exit codes, the case the harness
   actually ran, repetition counts, probe attach status, sample counts and an
   integrity verdict, and exit non-zero on failure
   (`tools/verify_capture.py`). The bench exit codes of the original runs were
   never recorded; their verification records are reconstructed from the
   committed artifacts and labelled as reconstructed.
4. **Probe attach status was not committed.** The five `*.syscalls.err` files
   (each `Attached 1 probe`) existed but were left out of the evidence tree;
   they are committed now.
5. **The mutex/futex attribution mislabelled a bucket.** Symbol lookup did not
   normalise glibc names, so `__GI___libc_write` was unknown and the unwind
   walked past it to the `clone3` frame at the bottom of every worker stack:
   the "clone (thread create) 5.42%" row was in fact the **completion
   notification write** to the progress eventfd. Kernel cycles with no syscall
   entering frame are no longer charged to a Sluice function at all.
6. **"one enter per operation, therefore not batched" was an inference.** The
   number of SQEs inside an `io_uring_enter` cannot be read off a syscall count.
   `tools/measure_lock_events.sh` now observes the argument directly: all 60,004
   enters of the W-02 io_uring case carried `to_submit=1`.
7. **§2's medians came from an uncommitted scratch file.** They were computed
   over a JSONL that had accumulated two capture runs; the committed records
   hold one. The table is recomputed from the committed records by
   `tools/report_case_medians.py`, and the statistic is stated.

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

Directly observed events and the fixed setup cost need no root and are measured
separately:

```bash
tools/measure_init_cost.sh     # setup path only, strace syscall counts
tools/measure_lock_events.sh   # futex operations and io_uring_enter arguments
tools/verify_perf_captures.sh  # sample counts and manifest check for perf.data
```

Report generation (no root needed):

```bash
git clone https://github.com/brendangregg/FlameGraph \
    docs/evidence/403-flamegraph-profiling-20261015/tools/flamegraph
cd docs/evidence/403-flamegraph-profiling-20261015
python3 tools/report_self_cost.py     # svg/*.svg, data/SUMMARY.md
python3 tools/report_attribution.py   # data/ATTRIBUTION.md
python3 tools/report_syscalls.py      # data/SYSCALLS.md
python3 tools/report_case_medians.py  # data/CASE-MEDIANS.md
python3 tools/report_lock_events.py   # data/LOCK-EVENTS.md
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
  execution, with the denominator read from the run's own records.

### 1.2 Three kinds of evidence, kept apart

| class | what it is | where |
|---|---|---|
| directly sampled | the leaf frame is what the CPU was executing at the sample | `SUMMARY.md`, all shares |
| call-path attribution | a kernel leaf charged to the syscall that entered the kernel, found by walking the stack for a glibc wrapper | `ATTRIBUTION.md`, with a per-case confidence line |
| directly observed events | syscall counts (bpftrace) and operation/argument values (strace) | `SYSCALLS.md`, `LOCK-EVENTS.md` |

Call-path attribution is an inference and assumes the frame-pointer unwind is
complete. Each case therefore reports, as a share of the cycles that have a
kernel leaf, how often the wrapper sat directly above the kernel frames
(`direct`), how often frames had to be skipped (`gap`, naming them), how often
no wrapper was found, and how much kernel time came from an interrupt rather
than a syscall. For `W02-threadpool-4k-d16-w4` that is direct 99.12%, no wrapper
0.50%, gap 0.34%, interrupt 0.04%; for `W02-uring-4k-d16` it is direct 91.19%,
interrupt 8.19%, no wrapper 0.62%. Kernel-only io-wq threads count as `direct`
because the thread itself, not a syscall, is what the stack identifies.

## 2. Measured result

`data/CASE-MEDIANS.md` holds the per-case medians (throughput, CPU/op, p50, p99,
syscalls/op), recomputed from the committed single-run records. They come from
sampled runs and are attribution evidence, not a baseline.

| Case | Throughput | CPU/op | p50 | p99 | syscalls/op |
|---|---|---|---|---|---|
| W-01 direct `read_at` 4 KiB | 1019k ops/s | 0.98 µs | 0.97 µs | 1.40 µs | 1.00 |
| W-02 io_uring 4 KiB depth=16 | 340k ops/s | 7.63 µs | 45.2 µs | 62.1 µs | 1.00 |
| W-02 ThreadPool 4 KiB depth=16 workers=4 | 246k ops/s | 13.66 µs | 62.5 µs | 102.4 µs | 8.79 |
| W-02 ThreadPool 4 KiB depth=16 workers=1 | 238k ops/s | 7.49 µs | 64.0 µs | 105.5 µs | 7.28 |
| W-02 ThreadPool 4 KiB depth=16 workers=8 | 217k ops/s | 20.79 µs | 71.9 µs | 121.9 µs | — |
| W-02 ThreadPool 64 KiB depth=16 workers=4 | 197k ops/s | 22.94 µs | 79.6 µs | 122.4 µs | — |
| W-03 external loop, ThreadPool 4 KiB depth=16 | 116k ops/s | 26.75 µs | 93.2 µs | 182.7 µs | 10.61 |
| W-03 external loop, io_uring 4 KiB depth=16 | 232k ops/s | 11.37 µs | 49.9 µs | 83.7 µs | — |

### 2.1 ThreadPool 4 KiB: the cost is mutex/futex handoff, not the data copy

`W02-threadpool-4k-d16-w4`, cycle shares (`data/ATTRIBUTION.md`):

| bucket | share |
|---|---|
| kernel (including syscall machinery) | 71.8% |
| libc pthread / futex (user space) | 17.2% |
| Sluice user space | 6.5% |
| other (harness verification/timing included) | 1.9% |
| libc mem* | 1.1% |
| c++ runtime | 0.6% |
| bench harness | 0.4% |
| kernel (syscall, wrapper unresolved) | 0.4% |
| libc wrapper self | 0.2% |
| kernel (interrupt/exception) | 0.0% |

Top detail rows (all directly sampled leaf cost; the syscall charge is by call
path, 99.4% of it from a wrapper found directly above the kernel frames):

| bucket | detail | share |
|---|---|---|
| kernel | `futex_wake` (mutex unlock slow path) | 21.52% |
| kernel | `futex_wait` (mutex lock slow path) | 19.05% |
| kernel | `pread` (the actual 4 KiB read) | 11.79% |
| kernel | `futex_wait` (condition variable, bounded park) | 7.97% |
| libc pthread | `pthread_mutex_unlock` (user space) | 6.17% |
| libc pthread | `pthread_mutex_lock` (user space) | 5.77% |
| kernel | `write` (completion notification eventfd) | 5.06% |
| kernel | `futex_wake` (condition variable) | 4.78% |
| libc pthread | `__lll_lock_wait` (glibc mutex slow path) | 2.05% |
| sluice | `RequestCore::occupancy` | 1.13% |
| libc mem* | `__memcmp_avx2_movbe` | 1.02% |

Reading this correctly: the shares are sampled cost, the labels are inference.
`futex_wake`/`futex_wait` are identified as the mutex path because the wrapper
frame found in the same stack is glibc's `__lll_lock_wait`/`__lll_lock_wake`.
Mutex acquisition and release — user-space fast path plus kernel slow path plus
the glibc `__lll_*` frames — account for **55.2%** of cycles (v1 said ~52% for
the same mechanism without the `__lll_*` frames). Moving the 4 KiB payload
(`rep_movs_alternative`) is about 4%.

The syscall census agrees on the mechanism: 8.79 syscalls/op in total
(worker 6.64, main 2.15), of which 6.78 are `futex`, plus exactly one `pread64`
and one `write` per operation. The write is the completion notification: the
stack is `worker_loop → signal_ready_progress → ProgressSource::signal →
wake_notification_nolock_ → write(2)`, and it costs 5.06% of all cycles.
`data/LOCK-EVENTS.md` shows the same traffic as events: 21 distinct
(operation, address) pairs over one repetition — the whole workload's
synchronization concentrates on about ten futex words.

This is also the mechanism behind the worker-count behaviour: CPU/op rises from
7.49 µs (workers=1) to 13.66 µs (workers=4) to 20.79 µs (workers=8), and
throughput peaks at workers=4 (246k) before falling back at workers=8 (217k).

### 2.2 io_uring: 36% of cycles inside `io_uring_enter`, one SQE per enter

`W02-uring-4k-d16`: the application issues 1.00 syscalls/op (essentially all
`io_uring_enter`), yet that syscall's kernel path holds 36.14% of all cycles.
The kernel io-wq worker threads (`iou-wrk-*`) hold another 34.6%
(`rep_movs_alternative` 6.1%, `shmem_file_read_iter` 2.2%, spin locks above 7%).
A further 6.5% is kernel time entered by interrupt rather than syscall
(reschedule IPIs and irqentry), which v1 charged to Sluice functions.

Submission is not batched, and that is now directly observed rather than
inferred from the syscall count: `data/LOCK-EVENTS.md` records all 60,004
`io_uring_enter` calls of a repetition with `to_submit=1`. CPU/op is still about
half the ThreadPool figure (7.63 µs vs 13.66 µs), consistent with the
futex/mutex work that the io_uring path does not pay.

### 2.3 W-03 external loop: two extra thread handoffs

`W03-threadpool-4k-d16-w4`: 10.61 syscalls/op (worker 5.88 `futex`, main 2.36
`futex`, plus `epoll_wait` 0.12 and a notification `read` 0.24) and 26.75 µs
CPU/op — about twice the W-02 ThreadPool figure. The extra cost is the driver
thread parked in `epoll_wait` plus the submitter parked on its own condition
variable.

### 2.4 The direct path is an order of magnitude cheaper

W-01 `read_at` costs 0.98 µs CPU/op at p50 0.97 µs, with 86.5% of cycles in the
kernel (`pread` alone 80.0%) and under 3% in Sluice user-space code. The
async/pipeline machinery raises the CPU cost of the same 4 KiB read to about
14 µs (ThreadPool) or 7.6 µs (io_uring).

### 2.5 Native X99 and WSL2 scale differently — do not mix the baselines

The G0 v2 baseline was captured on WSL2 (AMD Ryzen 7 5800H, virtual disk); these
measurements are from the native X99 host. The worker-count behaviour is not the
same on the two hosts:

| workers | WSL2 (G0 v2, canonical baseline) | X99 (this campaign, sampled runs) |
|---|---|---|
| 1 | 220k ops/s, 6.18 µs CPU/op | 238k ops/s, 7.49 µs CPU/op |
| 4 | 112k ops/s, 16.17 µs CPU/op | 246k ops/s, 13.66 µs CPU/op |
| 8 | 108k ops/s, 16.91 µs CPU/op | 217k ops/s, 20.79 µs CPU/op |

On WSL2 throughput falls monotonically as workers are added (the inversion G0
reported); on X99 it rises to workers=4 and only then falls back. CPU/op rises
with worker count on both hosts, so the added synchronization cost is real on
both — but only on X99 does the extra parallelism still buy throughput. A
candidate experiment tuned to WSL2's monotonic inversion must therefore be
re-justified against the X99 shape, and absolute figures from the two hosts must
not be compared.

The G0 lock-probe counts (18.6–45.0 mutex acquisitions/op, B-G0-06) come from an
`LD_PRELOAD` interposition probe on WSL2 and measure acquisitions, not syscalls.
The X99 census here counts `futex` syscalls (6.78/op for w4, one syscall per slow
path). The two quantities are different and must not be substituted for each
other.

## 3. Limitations

1. **Sampled runs, not a baseline.** `perf record` at 4999 Hz adds overhead; the
   throughput and latency columns are therefore not comparable with the G0 v2 or
   any G1 clean run. Syscall counts (bpftrace, in-kernel aggregation) have much
   lower overhead but are still instrumented runs.
2. **Three passes per repetition** — see §1.1. Harness verification and latency
   instrumentation are inside every flame graph.
3. **Call-path attribution is inference** — see §1.2. Per-case confidence is
   reported as a share of kernel-leaf cycles; the unresolved part is 0.8% or less
   for the ThreadPool cases (0.62% for io_uring) and the affected stacks are named
   in `ATTRIBUTION.md`. Interrupt-entered kernel time, which no syscall explains,
   is up to 8.2% of kernel-leaf cycles for io_uring and is reported separately
   rather than folded into a syscall.
4. **Symbolisation.** At `-O3`, identical-code folding and inlining occasionally
   attribute a frame to a neighbouring symbol (a wide frame may read
   `bench::CaseMeta::~CaseMeta`); shares are unaffected. Kernel frames need root
   to resolve through `/proc/kallsyms`.
5. **Per-process capture.** ThreadPool workers belong to the profiled process and
   are captured with it; io_uring's io-wq threads are kernel threads and are
   reported separately as `kernel (io-wq worker)` rather than being folded into
   the submitting process.
6. **`perf trace` was rejected as an instrument.** Its ring buffer reported
   thousands of lost events at this syscall rate, so its counts were discarded
   and bpftrace counting was used instead. The discarded output is not committed.
7. **The strace event census perturbs the workload.** `measure_lock_events.sh`
   serialises threads, so the operation mix and batch sizes it reports are
   trustworthy but its counts are not throughput evidence.
8. **The raw `perf.data` files are not committed** (about 371 MB uncompressed).
   `MANIFEST.sha256` records their names, sizes and SHA-256, and
   `data/perf/SAMPLE-COUNTS.md` re-verifies all eleven against it and reports the
   recorded sample count of each. `data/folded/` holds the gzipped folded stacks,
   which are sufficient to redraw every flame graph.
9. **SVG regeneration is not byte-identical.** `flamegraph.pl` iterates a Perl
   hash internally, so the order of same-level frames can vary between runs even
   for identical input; frame content and widths do not. The derived tables are
   deterministic and byte-identical across runs.

## 4. Contents

```text
REPORT.md            this report
MANIFEST.sha256      SHA-256 of every committed artifact + the uncommitted perf.data
data/perf/           records, stdout and verification of the 11 perf-record runs
data/syscalls/       bpftrace counts, records, stdout and verification of the runs
data/syscalls/init/  setup-path-only strace runs (fixed cost, no pipeline pass)
data/lock-events/    strace futex/io_uring_enter traces and their verification
data/folded/         gzipped folded stacks (FlameGraph input; regenerates every SVG)
data/CASE-MEDIANS.md per-case medians of the sampled runs
data/SUMMARY.md      per-case leaf self-cost ranking
data/ATTRIBUTION.md  bucket attribution, evidence class and per-thread-role split
data/SYSCALLS.md     syscalls per operation, split by thread role
data/LOCK-EVENTS.md  futex operations and io_uring_enter batch sizes (events)
data/INIT-COST.md    fixed startup/fixture cost per case
svg/                 full interactive flame graphs: <case>.svg and <case>.threads.svg
svg-compact/         narrower copies used by the inline single-file view
tools/               capture, measure, verify, fold and report scripts (see §1)
REPORT.superseded-v1.md   the version-1 report, kept for comparison
data/*.superseded-v1.md  the version-1 tables, kept for comparison
```
