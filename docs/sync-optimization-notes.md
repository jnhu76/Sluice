# Sync optimization notes

**Status: SYNC-IO-COMPLETE Phase 10 (sync doc reconciliation).** Maps blocking-
baseline **observations to future work**. **No benchmark results are claimed
here** — the W1–W4 matrix (job 022S) has not been run yet at the time of writing.
This doc is a hypothesis/decision template: each entry is an observation we
*expect* to investigate and the action it implies. When 022S/023S produce real
numbers, this doc is updated with evidence (per-workload, per-machine) — never
universal claims.

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
