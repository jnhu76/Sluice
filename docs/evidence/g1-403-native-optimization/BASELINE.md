# G1-R0 — frozen native Linux baseline

The clean native baseline for #403 Phase G1: no `perf`, no `strace`, no eBPF, no
profiler of any kind. This is the B0 reference every later A/B comparison uses.

## Provenance

```text
BASE_SHA        = 10aaf58b19092d44bfdc31c2761b90636d3a1518 (master, #533 + #534 merged)
HOST            = E5, Intel Xeon E5-2666 v3, 20 logical CPUs, 62 GiB (see HOST.md)
KERNEL          = 7.2.9-201.fc44.x86_64
PROTOCOL        = 2 warmup + 7 measured repetitions per case (the G0 v2 protocol)
G0_TMP_DIR      = /tmp/sluice-g1-bench      (tmpfs, page-cache-resident cases)
G0_DISK_DIR     = /var/tmp/sluice-g1-bench  (btrfs on the sda SSD, storage-backed cases)
```

Build (both profiles, with the harness's own profile assertions):

```bash
bash bench/build_profiles.sh release
```

```text
noring bench_w01_direct        9cb468d3080420e939bf3fe11cea45b62f589007f4d2693f38c8b49bbd60d6f2
noring bench_w02_pipeline      b41b6ec058e2954db0186fd9e3940d4073fd10c0e82e99d7f4ef50ffd2dcac44
noring bench_w03_external_loop 69218c3c6f926e405c55f04f755a1e576fafc3d5f8f16c1f8bbdd9dffc0282ff
ring   bench_w01_direct        9cb468d3080420e939bf3fe11cea45b62f589007f4d2693f38c8b49bbd60d6f2
ring   bench_w02_pipeline      29b2eb93b4c6104e7b0ce18daa10025e28a3027a9de73fd29a4a70969e503cc5
ring   bench_w03_external_loop 1c5533f94a645ab006a59751254bab3fd2c1e7b428524a7008bc510c19f95e32
```

`bench_w01_direct` is byte-identical across profiles (core-only by contract);
the two async binaries differ and only the ring build links `liburing.so`. Both
assertions are enforced by `build_profiles.sh`, not assumed.

Harness self-checks before the run:

```text
python3 bench/verify_hashes.py        -> 14 entries match, 0 problem(s)
bash bench/runner_failure_control.sh  -> all 11 scenarios behaved as required
```

Capture:

```bash
G0_TMP_DIR=/tmp/sluice-g1-bench G0_DISK_DIR=/var/tmp/sluice-g1-bench \
  bash bench/run_matrix.sh docs/evidence/g1-403-native-optimization/baseline 2 7 all -g1r0
```

Every run in the matrix is validated by `bench/check_run.py` against the frozen
G0 case set before the next one starts:

```text
check_run: OK w01-g1r0.jsonl stage=measured cases=15 records=145 not_run=0 exit=0
check_run: OK w02-g1r0.jsonl stage=measured cases=16 records=145 not_run=0 exit=0
check_run: OK w03-g1r0.jsonl stage=measured cases=6  records=55  not_run=0 exit=0
check_run: OK w02-noring-notrun.jsonl stage=notrun cases=16 records=17 not_run=6 exit=1
check_run: OK w03-noring-notrun.jsonl stage=notrun cases=6  records=7  not_run=3 exit=1
```

The no-ring controls are the required NOT_RUN evidence: the io_uring cases fail
with `NOT_RUN_WITH_REASON: io_uring backend not present in this build profile`
and exit non-zero, rather than silently falling back.

Raw records: `baseline/raw/*.jsonl`, logs: `baseline/logs/*.log`, matrix log:
`baseline/matrix-run.log`. Reproduce the tables with
`python3 bench/analyze.py docs/evidence/g1-403-native-optimization/baseline`.

## Results

Medians over the 7 measured repetitions, exactly as the frozen G0 analyzer
(`bench/analyze.py`) reports them. `CPU/op` is `1 / median(ops per CPU second)`
over the measured repetitions — process CPU seconds per operation across all
threads. `CV` is the sample standard deviation over the repetitions of
`ops_per_sec`.

### W-01 direct execution (tmpfs, page-cache-resident)

| case | ops/s | CPU/op | p50 | p99 | CV |
|---|---|---|---|---|---|
| `W01-read_at-4k-seq` | 1,143,219 | 0.87 µs | 882 ns | 1245 ns | 0.5% |
| `W01-read_at-4k-rand` | 1,146,541 | 0.87 µs | 889 ns | 1258 ns | 0.5% |
| `W01-read_at-64k-seq` | 138,164 | 7.22 µs | 7230 ns | 9268 ns | 0.1% |
| `W01-read_at-1m-seq` | 7,912 | 125.94 µs | 124980 ns | 145008 ns | 2.2% |
| `W01-write_at-4k-seq` | 924,709 | 1.08 µs | 1077 ns | 1338 ns | 4.6% |
| `W01-sync_data-4k` | 934,676 | 1.08 µs | 1092 ns | 1114 ns | 0.1% |
| `W01-sync_all-4k` | 932,344 | 1.09 µs | 1092 ns | 1117 ns | 0.0% |
| `W01-open_close` | 499,725 | 2.00 µs | 2006 ns | 2060 ns | 1.4% |
| `W01-file_info` | 1,927,254 | 0.52 µs | 534 ns | 571 ns | 0.8% |
| `W01-read_exact_at-4k` | 1,417,357 | 0.70 µs | 720 ns | 762 ns | 1.1% |
| `W01-write_all_at-4k` | 1,111,441 | 0.90 µs | 896 ns | 961 ns | 3.3% |

### W-01 storage-backed (btrfs on the sda SSD)

| case | ops/s | CPU/op | p50 | p99 | CPU util | CV |
|---|---|---|---|---|---|---|
| `W01-read_at-64k-seq-cold` | 9,375 | 28.07 µs | 7894 ns | 11808 ns | 26.5% | 1.7% |
| `W01-read_at-1m-seq-cold` | 337 | 693.00 µs | 133680 ns | 144693 ns | 23.3% | 2.0% |
| `W01-sync_data-4k-disk` | 452 | 288.18 µs | 1956672 ns | 3849750 ns | 13.2% | 8.7% |
| `W01-sync_all-4k-disk` | 524 | 210.61 µs | 1831066 ns | 3688639 ns | 11.3% | 4.9% |

These are device-bound (11–27% CPU utilisation) and are reported separately from
the tmpfs cases; they are never compared with each other or with WSL2's VHDX.

### W-02 bounded pipeline (4 KiB unless noted)

| case | ops/s | CPU/op | p50 | p99 | CPU util | CV |
|---|---|---|---|---|---|---|
| `W02-threadpool-4k-d1-w1` | 106,226 | 11.89 µs | 8812 ns | 13105 ns | 126% | 8.9% |
| `W02-threadpool-4k-d4-w4` | 201,394 | 14.67 µs | 16242 ns | 29129 ns | 300% | 7.7% |
| `W02-threadpool-4k-d16-w4` | 272,430 | 12.34 µs | 57466 ns | 85186 ns | 337% | 6.2% |
| `W02-threadpool-4k-d64-w4` | 243,431 | 13.50 µs | 247673 ns | 313987 ns | 331% | 7.0% |
| `W02-threadpool-4k-d16-w1` | 240,495 | 7.34 µs | 63505 ns | 108587 ns | 178% | 4.2% |
| `W02-threadpool-4k-d16-w8` | 255,181 | 15.64 µs | 61997 ns | 87548 ns | 406% | 4.7% |
| `W02-threadpool-64k-d16-w4` | 205,887 | 22.17 µs | 72925 ns | 100466 ns | 456% | 4.4% |
| `W02-uring-4k-d1` | 98,548 | 11.94 µs | 8947 ns | 12633 ns | 119% | **31.6%** |
| `W02-uring-4k-d4` | 335,191 | 5.15 µs | 10253 ns | 20516 ns | 172% | 7.1% |
| `W02-uring-4k-d16` | 307,979 | 5.43 µs | 51389 ns | 62006 ns | 170% | 6.5% |
| `W02-uring-4k-d64` | 290,972 | 5.49 µs | 207127 ns | 239372 ns | 166% | 6.5% |
| `W02-uring-64k-d16` | 91,831 | 18.67 µs | 158110 ns | 183059 ns | 173% | 2.4% |
| `W02-threadpool-4k-d16-w4-saturate` | 275,293 | 12.33 µs | — | — | 336% | 6.2% |
| `W02-uring-4k-d16-saturate` | 329,986 | 5.01 µs | — | — | 166% | 5.8% |
| `W02-threadpool-4k-d4-w4-scope64` | 218,368 | 14.03 µs | 15690 ns | 27690 ns | 306% | 6.8% |
| `W02-threadpool-4k-d16-w4-scope64` | 277,538 | 12.11 µs | 55183 ns | 81773 ns | 335% | 2.8% |

### W-03 external event loop (4 KiB)

| case | ops/s | CPU/op | p50 | p99 | CPU util | CV |
|---|---|---|---|---|---|---|
| `W03-threadpool-4k-d4-w4` | 137,950 | 19.14 µs | 28226 ns | 50735 ns | 265% | 13.5% |
| `W03-threadpool-4k-d16-w4` | 181,326 | 18.24 µs | 56512 ns | 98710 ns | 328% | 13.9% |
| `W03-threadpool-4k-d64-w4` | 201,454 | 17.83 µs | 197059 ns | 356195 ns | 359% | 9.4% |
| `W03-uring-4k-d4` | 136,743 | 12.65 µs | 14589 ns | 26266 ns | 182% | 21.3% |
| `W03-uring-4k-d16` | 191,367 | 10.27 µs | 54615 ns | 88135 ns | 196% | 4.5% |
| `W03-uring-4k-d64` | 187,821 | 10.78 µs | 191882 ns | 337913 ns | 203% | 10.6% |

## Worker-count scaling on native X99

| workers | ops/s | CPU/op | CPU util |
|---|---|---|---|
| 1 | 240,495 | 7.34 µs | 178% |
| 4 | 272,430 | 12.34 µs | 337% |
| 8 | 255,181 | 15.64 µs | 406% |

Throughput peaks at 4 workers and CPU/op rises monotonically with the worker
count. This is **not** the WSL2 shape: the G0 v2 baseline on WSL2 fell
monotonically (220,366 → 111,757 → 108,043 ops/s for w1 → w4 → w8). The extra
synchronization cost per operation is real on both hosts; on X99 the extra
parallelism still buys throughput up to 4 workers. A candidate justified by
WSL2's monotonic inversion therefore has to be re-justified against this shape,
and no absolute figure from the two hosts may be compared.

## Noise assessment and the pre-recorded repetition decision

CV is the sample standard deviation over the 7 measured repetitions of
`ops_per_sec`. Almost all cases are at or below 8.7%. Five cases exceed 10%:

| case | CV | decision |
|---|---|---|
| `W02-uring-4k-d1` | 31.6% | 15 measured repetitions in A/B stages |
| `W03-uring-4k-d4` | 21.3% | 15 measured repetitions in A/B stages |
| `W03-threadpool-4k-d16-w4` | 13.9% | 15 measured repetitions in A/B stages |
| `W03-threadpool-4k-d4-w4` | 13.5% | 15 measured repetitions in A/B stages |
| `W03-uring-4k-d64` | 10.6% | 15 measured repetitions in A/B stages |

The threshold and the reason are recorded here, before any candidate is
measured: these cases have depth 1 or a driver thread whose park latency is a
large fraction of the per-operation time, so a single repetition can differ from
the median by tens of percent. All other cases keep the 7-repetition protocol.
Samples are never selected after the fact; every repetition the harness records
stays in the raw JSONL.

## G0 adoption (four baselines that must not be mixed)

```text
Parallel G0-v1                     = HISTORICAL_NON_AUTHORITATIVE
Degenerate-oracle v1               = SUPERSEDED_RETAINED
PR #534 revised v2 (WSL2)          = CANONICAL_G0_RESEARCH_BASELINE
X99 baseline (this file)           = G1_NATIVE_LINUX_BASELINE
```

The G0 v2 evidence under `docs/evidence/g0-403-performance-baseline-20261015/`
is untouched: this baseline was written to its own directory, and
`bench/verify_hashes.py` still reports all 14 frozen G0 entries matching.

## What this baseline does not establish

* It is not a contract or conformance claim; `docs/roadmap/v1-conformance.md`
  remains the ledger, and this file is not part of it.
* It says nothing about why the costs are what they are. The attribution stage
  (`PMU.md`, `EBPF.md`, `SCHEDULING.md`) answers that, and it runs separately
  from these clean measurements.
* W-04 remains UNSUPPORTED and is not measured here.
