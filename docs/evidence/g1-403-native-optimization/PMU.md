# G1 targeted PMU counting — results

Source: `pmu/` (25 perf-stat runs: 5 cases x 5 repetitions, one
measured bench repetition each, `G0_WARMUP=0 G0_MEASURED=1`,
provenance and per-run status in `pmu/run.log`). All counts are
**user-space only**: `perf_event_paranoid=2` forces
`exclude_kernel=1` for an unprivileged counter; kernel-side
cycle attribution is reported by the #535 flame-graph evidence and
the root-required extensions are listed at the end.

Per-operation values are medians over the 5 repetitions; ops from
each run's own JSONL record. IPC, miss rates are user-space
cycles/instructions only.

| case | reps | ops/rep | cycles/op | instr/op | IPC | branch-miss/op | cache-miss/op | LLC-miss/op | dTLB-miss/op | task-clock ms/op | faults/op |
|---|---|---|---|---|---|---|---|---|---|---|---|
| W01-read_at-4k-seq | 5 | 100000 | 4285 | 1472 | 0.343 | 5.234 | 0.307 | 0.1278 | 0.04216 | 1.03e-08 | 0.01827 |
| W02-threadpool-4k-d16-w4 | 5 | 20000 | 2.283e+04 | 1.416e+04 | 0.620 | 49.81 | 0.9955 | 0.2611 | 0.1908 | 4.765e-08 | 0.0422 |
| W02-uring-4k-d16 | 5 | 20000 | 1.072e+04 | 1.435e+04 | 1.339 | 40.61 | 0.9093 | 0.1661 | 0.2391 | 2.012e-08 | 0.0418 |
| W03-threadpool-4k-d16-w4 | 5 | 20000 | 2.679e+04 | 1.199e+04 | 0.448 | 53.77 | 1.226 | 0.2518 | 0.3297 | 6.222e-08 | 0.0427 |
| W03-uring-4k-d16 | 5 | 20000 | 1.446e+04 | 1.232e+04 | 0.852 | 48.11 | 1.097 | 0.2937 | 0.2883 | 3.073e-08 | 0.0419 |

## Reading notes

- `W01-read_at-4k-seq` is the direct path reference: nearly all
  user cycles are the read loop itself; every async case pays the
  synchronization machinery on top of the same payload.
- The async cases' user IPC and miss rates quantify the
  user-space side of the 55.2% mutex share (ThreadPool) and the
  submit/enter path (io_uring) measured in the #535 evidence;
  kernel-side cost is deliberately out of these counts' scope.
- `page-faults` counts during-run faults; the fixture and ring
  setup dominate, steady-state pipeline reads are page-resident.

## PERMISSION_REQUIRED (kernel-side attribution)

The following need root on this host (`perf_event_paranoid=2`,
bpftrace requires CAP_BPF/CAP_PERFMON). Record with the same
one-repetition-per-invocation discipline via interactive
`sudo -v`; do not embed credentials in any command, script or log:

```
# kernel-side PMU sampling per case (kernel call chains):
sudo perf record -g --call-graph fp -k 1 -o <case>.perf.data -- \
    env G0_WARMUP=0 G0_MEASURED=1 G0_CASE_FILTER==<case> \
    G0_JSONL=<scratch>/<case>.jsonl <binary>
# raw event counting including kernel half (cycles:k, or cycles with
# :ku splitting): needs root or perf_event_paranoid<=1
# futex wait/wake and io_uring_enter argument observation:
sudo bpftrace docs/evidence/403-flamegraph-profiling-20261015/tools/trace_*.bt
```

Until captured, kernel-side PMU attribution for G1 stays
`PERMISSION_REQUIRED`; the C9/C1/C2 experiments proceed on the
#535 kernel evidence (sampling shares) plus these user-space
counts, which is sufficient for mechanism identification but not
for a kernel-cycle re-attribution claim.
