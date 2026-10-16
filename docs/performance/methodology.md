# Performance measurement methodology

This is the standing method for every Sluice performance claim. Revise this
file in place as the method improves; do not create a new method document per
study round. Raw measurements, profiler output and experiment logs are research
artifacts: keep them in an external workspace or CI artifact store, never in
this repository. A performance PR carries only a short summary and links
sufficient to reproduce the claim.

## Procedure

1. **Freeze.** Pin the commit SHA, host, compiler and flags, workload
   parameters, the correctness oracle that decides whether a run is valid, and
   the measurement configuration (warmup, repetitions, statistical summaries).
   Record them before the first run.
2. **Baseline.** Measure the frozen baseline uninstrumented. Validate every
   run's exit status, case set and raw-result integrity before reading any
   number; a workload-oracle failure invalidates the measurement entirely.
3. **Profile.** Only after a baseline exists, use perf, PMU counters, eBPF,
   syscall traces or flamegraphs as needed. Always separate profiler runs from
   clean benchmark runs: profiling answers where cost lives, never how much a
   change improves throughput. Account for observation cost when attributing.
4. **Attribute.** State a falsifiable hypothesis naming an avoidable cost, and
   test it with single-factor experiments. A change without an attributed,
   measured cause is speculative and is rejected without further study.
5. **Optimize.** Implement the minimal change. On the same machine, run paired
   interleaved A/B against the frozen baseline and compare throughput,
   CPU per operation, latency tails, and the correctness oracle in every run.
6. **Decide.** Accept, reject, or record that the cost is not worth removing.
   Real gaps that are deferred become small tracking issues. Record negative
   conclusions so the same ground is not re-researched.

## Rules

- Correctness gates govern adoption. A measured improvement never overrides a
  correctness, liveness or resource-lifetime failure, and no test is skipped
  or weakened to keep a number.
- Results from different machines are not comparable, including nominally
  identical hardware. Only paired runs on one host decide.
- An optimization must not introduce new semantic authority, runtime state or
  public behavior; it stays inside the mechanism-justification rules.
- No merged PR may claim a library-wide improvement from workload-local
  measurements; regressions in any measured scenario are disclosed in the PR.

## Tool boundary

`bench/run_matrix.sh` and `bench/check_run.py` execute and validate the frozen
G0 preregistered matrix (#403) against the G0 baseline evidence; they are the
validated harness for that workload set. A new workload needs its own runner
configuration or an explicit runner update before results count as evidence.
