# B.2 step-ledger recomputation

Source: `raw-r2/record/agg-attrib.tsv` (5 `perf record` runs of the BASE
`sluice-copy` workload, exclusive-self sample attribution). The file's header
labels are transposed relative to its data: column 1 is the percentage and
column 2 is the leaf class. This recomputation reads column 1 as the value and
column 2 as the class.

`caller-recompute.tsv` is the exact, recomputable caller self-sample sum for
each B.2 step. It is authoritative for the **profile share** column only.
B.2's frequency and per-request counts come from separate archived counters
(`mech-counters.tsv`, tracepoint counts, `strace`) and are not recomputed here.

Why the report's B.2 numbers differ from these sums:

- B.2's `submit_read 4.83%` is an inclusive figure that covers validation,
  reserve, accept and dispatch; the exclusive caller rows sum to 4.685%
  (`AsyncIoContext::submit_read` 4.402 + backend 0.257 + scope 0.026).
- B.2's `poll_progress 2.00%` is inclusive; the exclusive caller row is
  1.492%.
- B.2's `publish_one 0.43%` covered the publication step; the exclusive caller
  row is 0.225%.
- B.2's `wait_one 31.63%` / 32.2% are the two overloads summed; the exact sum
  is 32.197%.
- B.2's `run_syscall 19.95%` is from an earlier raw aggregation; the exact
  sum here is 19.752%.
- B.2's application row `0.93%` is not a caller self-sample sum; the caller
  row for `run_pipelined_copy_with_context` is 0.010%. The 0.93% figure
  cannot be reproduced from `agg-attrib.tsv` and must not be used
  quantitatively.

Conclusion recorded in the report: B.2 is a location map, not a quantitative
ledger. Quantitative claims use B.1 and the dedicated mechanism counters.
