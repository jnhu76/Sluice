# TAX-0C profile session notes — tax0c-profile-wsl2

Low-overhead external profiling of the control-plane topology
(#250 TAX-0C). Tool: `perf record -F 4999 -g --call-graph dwarf,16384`
on a symbolized twin binary (`tax0_z_ladder_bench_sym` — identical
sources/deps built with `set_symbols("debug")`; verified equivalent to
the canonical stripped binary: 3121 vs 3112 instr/op at read 4K d32 z2,
+0.3%). Raw perf.data binaries were DELETED after report-text
extraction (repo-bloat rule; reports + profiles.json retained).

## Top userspace symbols (samples, userspace, percent-limit 0.5)

z2 (AsyncIoContext manual driver), 4K d32 read — the L1 abstraction-tax
decomposition target:

- 35.1% the bench's own driver loop inlined in `run_rep` (workload +
  inline Sluice consume/ready/reset paths)
- 8.3% `submit_transaction` (arena admission ladder)
- 6.9% + 4.8% `pthread_mutex_lock` / `pthread_mutex_unlock` (access_mtx_
  + dispatch_mtx_ + arena leaf, aggregated)
- 4.5% `UringAsyncBackend::handle_one_cqe`
- 4.3% `dispatch_one_locked`; 3.6% `enqueue_after_commit`;
  3.3% `finalize_operation_terminal_`; 2.6% `RequestArena::reap`;
  2.4% `TransportLedger::append`; 2.0% `record_terminal`;
  1.6% `reserve`; 1.6% `free_slot_locked_`; 1.4% `publish_size_ready`;
  1.1% `retire_router_entry_`; 0.9% `enqueue`; 0.9% `release_completed_binding`;
  0.7% `mark_running`; **0.64% `UringAsyncBackend::outstanding` (F01 site)**
- Sum of Sluice-attributable control-plane symbols ≈ 55% of userspace
  samples.

z3w1 (ApplicationRuntime), 4K d32 read — the continuation layer adds:
`register_waiter` (backend 6.7% + ctx 1.1%), `await_completion_size`
1.7%, `retire_wait_record_locked` 1.3%; mutex samples rise to ~24%.

z1b (semantic floor), 4K d32 read: 85.9% inside the bench's own driver
loop; no Sluice symbols. Confirms the floor's user work is the workload
itself.

z2 1M d8 read: 97.9% in-loop (memcpy/word_sum dominate; the +2015
instr/op control-plane delta persists as an absolute but is <1%
relative).

## Interpretation bounds

Sample shares ≠ instruction shares (stalls over-weight locks); they
rank WHERE control-plane work lives, they do not quantify it — the
quantified attribution is the TAX-0D A/B (F01: −75 instr/op) and the
ladder deltas (Z2−Z1b ≈ +2015/op fixed). No new observability
architecture, no production instrumentation, no eBPF: perf only.
