# Per-case medians of the sampled runs

Median over the measured repetitions of each case's committed run:
`ops_per_sec`, `cpu_s / ops`, and the instrumented pass's `lat_p50_ns` /
`lat_p99_ns`. `syscalls/op` is the all-thread figure from the corrected
`SYSCALLS.md`. Sampled runs are attribution evidence, not a throughput
baseline: `perf record` at 4999 Hz and bpftrace both perturb the workload.

| case | measured reps | throughput | CPU/op | p50 | p99 | syscalls/op |
|---|---|---|---|---|---|---|
| w01-read-4k-seq | 20 | 1019k ops/s | 0.98 µs | 0.97 µs | 1.40 µs | 1.00 |
| w02-uring-4k-d16 | 40 | 340k ops/s | 7.63 µs | 45.21 µs | 62.13 µs | 1.00 |
| w02-tp-4k-d16-w4 | 40 | 246k ops/s | 13.66 µs | 62.45 µs | 102.38 µs | 8.79 |
| w02-tp-4k-d16-w1 | 20 | 238k ops/s | 7.49 µs | 64.01 µs | 105.46 µs | 7.28 |
| w02-tp-4k-d16-w8 | 20 | 217k ops/s | 20.79 µs | 71.86 µs | 121.89 µs | — |
| w02-tp-64k-d16-w4 | 20 | 197k ops/s | 22.94 µs | 79.55 µs | 122.37 µs | — |
| w02-uring-4k-d64 | 20 | 296k ops/s | 8.47 µs | 216.71 µs | 246.31 µs | — |
| w03-tp-4k-d16-w4 | 20 | 116k ops/s | 26.75 µs | 93.20 µs | 182.73 µs | 10.61 |
| w03-uring-4k-d16 | 20 | 232k ops/s | 11.37 µs | 49.87 µs | 83.71 µs | — |
| w01-write-4k-seq | 20 | 916k ops/s | 1.09 µs | 1.07 µs | 1.39 µs | — |
| w01-sync-data-4k | 20 | 700k ops/s | 1.43 µs | 1.38 µs | 1.94 µs | — |
