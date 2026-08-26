# Explicit-I/O Abstraction Tax (E1 / G0 Core Cost Baseline)

Status: **Active** — the #221 E1 first-layer experiment: a reproducible,
provenance-bound measurement of what the Sluice explicit-I/O control plane
costs per request compared with direct I/O and a simple thread pool.
Governing methodology:
[`performance-engineering.md`](performance-engineering.md); the schema-2
evidence machinery and tooling live in
[`scripts/bench/perf-attribution.py`](../../scripts/bench/perf-attribution.py)
(`e1` subcommand) and
[`scripts/bench/perf-evidence-validate.py`](../../scripts/bench/perf-evidence-validate.py)
(artifact kind `e1tax`).

> **This benchmark measures software/control-plane cost under the recorded
> environment. It does not establish universal storage performance.**

This document is deliberately narrow: it defines the G0 experiment only.
No optimization is authorized by anything here (methodology §6: PMU
counters and profiles are diagnostic evidence, not optimization
authorization).

## Question

> Under the same operation stream, the same byte count, the same request
> size, and the same concurrency/depth conditions, how much fixed
> control-plane cost does Sluice + ThreadPoolBackend add compared with a
> simple baseline?

## Ladder

Each ladder executes the identical deterministic positional op stream; the
difference between adjacent rungs isolates one layer (the grep ladder in
[`performance-attribution.md`](performance-attribution.md) applies the same
shape to one application).

| Stage | Contents | Isolates |
|-------|----------|----------|
| `L0_raw` | raw blocking `pread`/`pwrite` at parallelism D (D = 1: inline serial loop; D > 1: D strided raw threads, no queue) | the OS syscall stream at that parallelism |
| `L1_pool` | minimal competent fixed `std::thread` pool: W persistent workers, one mutex + two condvars, bounded job ring of capacity D, no per-request heap allocation, proper join | OS + thread-pool execution machinery |
| `L2_sluice` | the real public path: `ApplicationRuntime` + `ThreadPoolBackend(request_capacity = D, worker_count = W)`, one task driving a depth-D `submit_*` / `await_completion` pipeline over process-lifetime caller-owned buffers + Completions (the `sluice-copy` Version B pipeline shape) | the Sluice explicit-I/O control plane |

Tax definitions (naming is normative for claims):

```text
ThreadPool direct tax  = T_L1 - T_L0
Sluice incremental tax = T_L2 - T_L1
Sluice overhead ratio  = (T_L2 - T_L1) / T_L1
```

`T_L2 - T_L0` is **not** reported as "Sluice overhead": L1 already contains
concurrent execution machinery, so that difference mixes the thread-pool tax
into the Sluice number.

`L3 raw-liburing` / `L4 Sluice + UringAsyncBackend` are **deferred** for G0:
they require a real-liburing host validation
([`io-uring-liburing-validation.md`](io-uring-liburing-validation.md)), not
a simulated or fake-backend comparison. No io_uring claim is made by this
experiment.

Scheduler workers in L2 are fixed at 1 (a DISTINCT resource from backend
blocking-I/O workers per `AGENTS.md` §1); the G0 pipeline is one task,
matching the applications' `run_task_to_result(workers = 1)` shape. Sweeping
scheduler workers is a separate experiment.

## Workload

Two primitive workloads only (this is a Core microbenchmark, not an
application benchmark; copy/hash/grep/tail composites come later in the
campaign):

- **READ**: fixed-size positional `pread` from a pre-generated
  deterministic file;
- **WRITE**: fixed-size positional `pwrite` into a pre-sized output file.

Dataset: one 4 KiB master block of splitmix64 words, tiled to
`total_bytes`. The generator, the read word-sum expectation, and the write
fill all derive from the same master block (closed-form expected sum), so
every ladder provably moved the same bytes.

Recorded per cell: operation (read/write), request size, total bytes,
operation count, in-flight depth, worker count, access pattern
(sequential positional), file size, filesystem, cache assumption
(warm / steady-state).

## Matrix

```text
request size : 4 KiB, 16 KiB, 64 KiB, 256 KiB, 1 MiB, 4 MiB
depth        : 1, 2, 4, 8, 16, 32, 64, 128
workers      : 1, 2, 4, 8
ladders      : L0_raw, L1_pool, L2_sluice
ops          : read, write
```

`--matrix smoke` runs sizes {4 KiB, 64 KiB, 1 MiB} × depths {1, 8, 64} ×
workers {1, 4}; `--matrix full` runs the whole sweep; `--matrix custom`
takes explicit axis lists. The runner refuses `depth * request_size`
combinations above a 1 GiB buffer budget (the bench enforces the same
bound). Axes are CLI-overridable.

## Metrics

M0 (mandatory, every cell): wall time (median/min/max/p25/p75 over
repetitions with raw per-rep samples preserved), throughput bytes/s, IOPS,
process user CPU, process system CPU, max RSS, operation count, byte
count, error count. M1 (best effort, `--perf`): `perf stat` cycles /
instructions / context-switches / cpu-migrations / page-faults from a
separate 1-rep invocation so counters never perturb the timed cells. When
a counter source is unavailable (permissions, virtualization), the artifact
records `available=false` with a reason — never zeros.

Latency (`--latency`): per-op submit→completion p50/p95/p99 from
preallocated stamps (no per-op allocation). L0 measures the syscall as the
issuing thread sees it; L1/L2 measure submit→completion as observed by the
submitting code (queue wait included — the latency the driving thread
experiences). Latency mode is opt-in so its instrumentation never silently
sits inside throughput numbers.

Steady-state vs lifecycle: per-rep wall time covers the op stream with
persistent workers / persistent Runtime (created before the first rep);
one-time construction and teardown are recorded separately
(`lifecycle_setup_ns`, `lifecycle_teardown_ns`) and excluded from wall
time. Both are in the artifact; neither is hidden.

## Same-work guarantee (fail-closed)

Every repetition of every ladder must process exactly
`ops = total_bytes / request_size` requests of `request_size` bytes. READ
repetitions must produce the exact expected word sum; WRITE output is read
back and verified once after the last repetition. Short reads, short
writes, zero-progress writes, unexpected EOF, I/O errors, and count
mismatches exit non-zero (exit 3) — the runner refuses to build an
artifact from a failed cell. The validator independently re-checks
`completed == expected` for ops and bytes on every committed cell.

## Environment discipline

Canonical G0 condition: **warm cache / steady state** on tmpfs, Release
build, clang — the goal is control-plane fixed tax, not storage-device
latency. tmpfs numbers must not be generalized to real SSD performance
(`PLATFORM = tmpfs`, goal: suppress device variance / emphasize software
overhead). Cold-cache runs are not claimed; dropping caches is not assumed
available. The environment fingerprint (WSL classification, CPU, kernel,
glibc, compiler, filesystem mounts, tool versions, perf_event_paranoid,
bpftrace availability) is bound into every artifact.

## How to run

```sh
xmake f -m release --toolchain=clang -y
xmake build e1_abstraction_tax_bench
# smoke matrix (READ):
scripts/bench/perf-attribution.py e1 --matrix smoke --op read \
    --total-bytes 1073741824 --reps 7 --output out.json
# representative points, >= 20 reps, perf counters:
scripts/bench/perf-attribution.py e1 --matrix custom --op read \
    --sizes 4096,65536,1048576 --depths 1,8,32 --workers 4 \
    --total-bytes 1073741824 --reps 21 --perf --output out.json
# diagnostics (never canonical evidence):
scripts/bench/perf-attribution.py flame -- .../e1_abstraction_tax_bench ...
scripts/bench/perf-attribution.py e1bpf -- .../e1_abstraction_tax_bench ...
```

The bench binary itself is intentionally dumb: it executes one (ladder, op,
size, depth, workers, reps) cell per process, enforces exact accounting,
and emits one JSON object; the runner owns matrix generation, process
isolation, repetitions, provenance, and aggregation.

## How to validate

```sh
python3 scripts/bench/perf-attribution.py self-test
python3 scripts/bench/perf-evidence-validate.py --self-test
python3 scripts/bench/perf-evidence-validate.py
```

The validator (runs in the pre-push gate and CI) fails an `e1tax` artifact
closed on: missing executable provenance / commit; non-Release build;
wrong operation or byte counts; zero repetitions; non-numeric timing
samples; medians inconsistent with raw samples; recorded errors; read
cells without word-sum verification; derived tax rows contradicting the
cell medians; unavailable diagnostics without a recorded reason.

## Known limitations

- WSL2 host: cross-thread wake latency inflates all queue-based rungs
  (L1 and L2 share that inflation; the *increment* L2−L1 is the Sluice
  part). Numbers are environment-bound, as recorded.
- L2 fixes scheduler workers at 1; the scheduler-worker axis is a separate
  experiment.
- Latency percentiles in throughput cells are opt-in; per-rep percentiles
  plus one full final-rep sample array are preserved.
- The uring ladder is deferred (see above).
- The L1 pool is deliberately minimal (mutex + condvar); it is not a
  lock-free pool and does not try to be. Its role is to price competent
  ordinary thread-pool execution, not to win.

## Claim boundary

Supported by a valid artifact, this experiment can claim: per-request and
per-byte control-plane taxes for the measured configuration on the
recorded host, their scaling signature against request count at fixed
bytes, and lifecycle construction/teardown costs. It cannot claim:
io_uring comparisons, universal storage performance, cold-cache behavior,
or any internal decomposition of the aggregate L2−L1 increment (that
requires the Core Cost Decomposition experiment — aggregate increments
prove a cost exists, not its composition).

## First-round evidence (2026-08-26)

Canonical artifacts: `docs/results/performance-attribution/`
(`e1-round1-read-smoke.json`, `e1-round1-write-smoke.json`,
`e1-round1-read-representative.json`,
`e1-round1-write-representative.json`), produced by the `e1` runner at
commit `9404df8` on a clean tree (executable sha256 `8801638…`, rebuilt
and re-verified identical after measurement), Release clang build, 1 GiB
per cell, tmpfs, warm cache. Supplementary non-canonical diagnostics
(flame graphs, bpftrace counts): `e1-diagnostics/` in the same directory.
READ/WRITE representative points (medians of 21 repetitions, workers = 4;
per-request taxes derived by the runner):

| op | request | depth | workers | L0 raw (ms) | L1 pool (ms) | L2 Sluice (ms) | (L1−L0)/op (ns) | (L2−L1)/op (ns) | (L2−L1)/L1 |
|---|---|---|---|---:|---:|---:|---:|---:|---:|
| read | 4 KiB | 1 | 4 | 226 | 8773 | 9298 | 32603 | 2004 | 6.0% |
| read | 64 KiB | 8 | 4 | 55 | 182 | 209 | 7792 | 1615 | 14.5% |
| read | 1024 KiB | 32 | 4 | 129 | 232 | 246 | 100444 | 14003 | 6.2% |
| write | 4 KiB | 1 | 4 | 426 | 9703 | 10048 | 35390 | 1314 | 3.5% |
| write | 64 KiB | 8 | 4 | 216 | 212 | 213 | −205 | 71 | 0.6% |
| write | 1024 KiB | 32 | 4 | 252 | 311 | 317 | 57217 | 5923 | 2.0% |

First-round observations (bounded to what the artifacts carry):

- The per-request **Sluice incremental tax (L2−L1)** on this host is
  ~1.3–2.0 µs/op at 4 KiB depth 1 and stays within 0.07–14 µs/op across
  the measured matrix — small relative to the pool tax.
- The **ThreadPool direct tax (L1−L0)** at 4 KiB depth 1 is ~33–35
  µs/op: on this WSL2 host a queue round trip costs ~4 thread switches
  per op (bpftrace diagnostic: ≈1.08 M `sched_switch` for 262 k ops in
  both L1 and L2). This is environment-dominated and shared by any
  queue-based design, Sluice included.
- At moderate depth / larger requests, L2−L1 shrinks toward zero and is
  **negative at several cells** (e.g. write 64 KiB d8, read 1 MiB d8 in
  the smoke matrices) — Sluice's reap batching can outperform the naive
  minimal pool; the increment is not a constant per-request surcharge.
- Recorded anomaly (finding, not fixed here): **L2 at 4 KiB, depth ≥ 32,
  workers = 4 is markedly slower** (write d32: L2−L1 = +1.73 s, +266%;
  the smoke d64 cells show the same shape for both ops) while w = 1 at
  the same depth is unaffected. Classification UNKNOWN pending
  decomposition.
- Host noise: this session showed 5–60% (max→min) sample spread on some
  cells (medians reported; raw samples in the artifacts). Two independent
  measurement sessions produced consistent shapes.

See the artifacts for the full matrices, raw samples, CPU/RSS numbers,
and perf counters (user-space-only `:u` mode — kernel-side time is not
captured; context-switch counts read 0 for that reason). Scaling
observation and classification live in issue #221's construction comment;
this section records only what the artifacts carry.
