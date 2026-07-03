# Sync optimization notes

**Status: SYNC-IO-COMPLETE Phase 10 (sync doc reconciliation); 023S evidence
pass added.** Maps blocking-baseline **observations to future work**. The
hypothesis template below predates the matrix; the **Measured observations**
section (added in 023S) records what the W1–W4 benches robustly show, scoped to
the dev environment. **No universal performance claim is made** — every measured
observation is workload × machine × build, with caveats explicit. Tuning actions
derived from evidence are listed; tuning *results* (before/after) are deferred to
a release-mode, real-disk run.

## Measured observations (023S evidence pass)

**Scope:** WSL2 dev box, xmake **debug** build, **tmpfs** `/tmp`. Absolute
numbers are NOT claimed — debug disables optimization and tmpfs hides disk cost.
What follows is the *robust shape* (the part that survives reruns and is not
noise). Full caveats in `docs/results/sync-w1-w4-baseline.md` and
`docs/results/sync-durability-baseline.md`.

```text
M1. Concurrency helps writes/reads/copy once streams >= 2.
    blocking_bounded_pool matches or beats blocking_sequential at streams >= 2
    on W1/W2/W3, and the gap widens with streams. The pool (job 021S) closes
    gap G5: the blocking path now has a real concurrency axis. This is robust.
    Action: none — this is the engineered baseline working as designed.

M2. Positional I/O (pread/pwrite, job 018S) is competitive with many_files.
    W1/W2 one_file_many_offsets cells are within noise of many_files at equal
    total bytes. Decision Q3 partial answer: positional I/O does NOT regress
    blocking throughput on this environment. Whether it *helps* is workload-
    dependent and deferred to a release + real-disk run.
    Action: none (no regression); keep positional as an opt-in.

M3. thread_per_stream converges to bounded_pool near hardware_concurrency.
    At streams == hw threads, the two modes are within noise (both saturate
    cores). Below saturation, bounded_pool is competitive with lower thread
    pressure. Decision Q7 answer: bounded_pool is the primary async-comparison
    row; thread_per_stream is the upper-bound reference. Locked in methodology.
    Action: none — record as the comparison policy.

M4. fsync/fdatasync cost is near-free on tmpfs (W4).
    sync_data_every_file and sync_all_every_file add small overhead over none
    in this environment. THIS IS A TMPFS ARTIFACT. Decision Q4 cannot be
    answered here — on a real disk sync is expected to dominate. Recorded as
    "tmpfs understates W4" so async W4 is not later claimed to "win" against a
    bogus near-free sync baseline.
    Action: re-run W4 on real disk before any durability conclusion. No code
    change.

M5. Debug build caps absolute MB/s.
    Absolute throughput numbers from the debug benches are pessimistic. The
    *shape* (M1-M4) is robust; the *magnitudes* are not tuning-grade.
    Action: 023S tuning sweep must run in release (-m release). No claim from
    debug numbers.
```

### Tuning actions derived (evidence-linked, no claimed results yet)

```text
T1. None of M1-M5 indicates a blocking-side code defect. The positional path
    (018S) and the pool (021S) behave as designed. No refactor triggered.
T2. The 023S optimization pass therefore REDUCES to: re-run W1-W4 in release on
    a representative disk, then fill in the decision matrix
    (docs/bench-decision-matrix.md extension) with scoped numbers. The blocking
    baseline is "engineered as far as blocking allows without semantic change"
    per the 023S abort condition — no semantic change is needed.
T3. Deferred (need p50/p95/p99 + ctx_switch columns, not yet wired): Q1 knee,
    Q2 useful worker count, Q6 context-switch cost. These are columns to add to
    the matrix CSV in a future small job, not blockers for the sync-first gate.
```

## How to use this doc

Each entry follows the shape:

```text
Observation (hypothesis):  <what we expect to see in the matrix>
Implication / future work: <what to do about it; which job/async-phase it informs>
```

When real data lands, append a measured section per workload under
`docs/results/` and reference it here. Do not state "X is faster" without a
scoped result note.

## Hypotheses to investigate (template)

### Small-block writes slow
```text
Observation: W1 with small block_size (e.g. 64B-4KB) shows low mbps / high
  sys_cpu_ms per byte.
Implication: investigate vector I/O (write_all_vec), batching, buffer size.
  Prioritize vector helper quality before async (job 019S closeout).
```

### many_write does not scale with pool_threads
```text
Observation: W1 mbps plateaus or regresses as pool_threads grows past a knee.
Implication: check storage saturation, file layout (many_files vs
  one_file_many_offsets), sync_policy, syscall count. The knee answers
  sync-bench-matrix Q2.
```

### thread_per_stream high context switches
```text
Observation: blocking_thread_per_stream shows high involuntary_ctx_switches vs
  blocking_bounded_pool at similar throughput.
Implication: prefer bounded_pool as the engineered baseline; record
  thread_per_stream as an upper-bound reference only.
```

### sync_data_every_batch dominates wall time
```text
Observation: W4 sync_policy=sync_data_every_batch makes fsync/fdatasync the
  dominant cost (total_ms dominated by sync).
Implication: this is the blocking baseline that future async W4 (overlapped
  durability) should test against. Do NOT claim async "wins" W4 without this
  baseline.
```

### one_file_many_offsets poor
```text
Observation: file_layout=one_file_many_offsets underperforms many_files at equal
  total bytes.
Implication: inspect the positional I/O implementation (read_at/write_at,
  job 018S) and filesystem behavior. May indicate pread/pwrite path needs
  tuning or that many_files is simply better for this filesystem.
```

### vector I/O improves
```text
Observation: write_all_vec reduces syscall count (VectorStats.write_vec_fallback
  stays low) and improves mbps vs scalar write_all.
Implication: prioritize vector helper quality before async; consider vector
  defaults in copy strategies.
```

### blocking_bounded_pool already sufficient
```text
Observation: for some workload cell, blocking_bounded_pool matches or exceeds the
  expected async benefit (e.g. CPU-bound or low-concurrency cells).
Implication: async may be lower priority for that workload; record the cell so
  async bench (future job 022) does not oversell async where blocking suffices.
```

## Decision questions this feeds

These are the same as `docs/sync-bench-matrix.md` §5 (Q1–Q7). When the matrix is
run, each question gets an evidence-backed answer recorded here and under
`docs/results/`, scoped per workload/machine.

## Out of scope

```text
- universal performance claims              (forbidden)
- async results                             (async bench is gated, separate)
- physical durability claims                (filesystem/hardware dependent)
- committing to optimizations before the matrix provides evidence
```

## Cross-links

- Bench methodology: `docs/sync-bench-methodology.md`.
- Bench matrix + decision questions: `docs/sync-bench-matrix.md`.
- Architecture: `docs/sync-io-architecture.md`.
- Jobs: `docs/sync-io-next-jobs.md` (022S produces evidence, 023S tunes).
- Existing optimization runbook (single-stream): `docs/bench-optimization-runbook.md`.
