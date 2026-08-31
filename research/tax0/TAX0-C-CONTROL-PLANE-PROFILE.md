# TAX-0C — Control-plane topology profile

Issues: #250 · #259 · PR #260 · Profile session: `results/tax0c-profile-wsl2/`

# 1 Verdict

**TAX-0C COMPLETE — HOT SPOTS ATTRIBUTED TO SYMBOLS; NO NEW OBSERVABILITY INFRA.**

工具：`perf record -F 4999 -g --call-graph dwarf` 只。剖析对象是
symbolized 孪生二进制（`tax0_z_ladder_bench_sym`，同源同依赖，仅加
symbols；与 canonical stripped binary 等价性验证 3121 vs 3112 instr/op）。
原始 perf.data（4 × 0.2–1.4 GB）按 repo bloat 规则提取 report 文本后删除，
session 保留 report-*.txt + profiles.json + notes.md。

# 2 Regime profiles（分开采样，不合并叙事）

## z2 @ 4K d32 read（L1 税分解目标）

userspace 样本 top（完整表见 report-z2-4k-d32.txt）：

```text
35.1%  bench 驱动循环（run_rep 内联：workload + inline consume/ready/reset）
 8.3%  detail::submit_transaction（arena 准入阶梯）
 6.9%  pthread_mutex_lock          ┐ access_mtx_ + dispatch_mtx_ + arena leaf
 4.8%  pthread_mutex_unlock        ┘ 三权威域聚合
 4.5%  UringAsyncBackend::handle_one_cqe
 4.3%  dispatch_one_locked
 3.6%  enqueue_after_commit
 3.3%  finalize_operation_terminal_
 2.6%  RequestArena::reap
 2.4%  TransportLedger::append
 2.0%  record_terminal   1.6% reserve   1.6% free_slot_locked_
 1.4%  publish_size_ready  1.1% retire_router_entry_  0.9% enqueue
 0.9%  release_completed_binding  0.7% mark_running
 0.64% UringAsyncBackend::outstanding   ← F01 站点可见但样本占比小
```

Sluice 控制面符号合计 ≈ 55% userspace 样本。样本份额 ≠ 指令份额
（stall 使锁偏重）；量化归因以 TAX-0D A/B 与 ladder 差值为准。

## z3w1 @ 4K d32 read（continuation 层）

在 z2 集合之上增加：`register_waiter`（backend 6.7% + ctx 1.1%）、
`await_completion_size` 1.7%、`retire_wait_record_locked` 1.3%；
mutex 样本升到 ~24%。即 Z3−Z2 的 +787 instr/op 主要落在 waiter
注册/退record 与 scheduler 域——与 census Path B 逐箭头恢复一致。

## z1b @ 4K d32 read（floor 对照）

85.9% 在 bench 自身驱动循环（workload 本体），无 Sluice 符号——floor
的用户态工作就是 workload，干净成立。

## z2 @ 1M d8 read（大块 regime）

97.9% 在驱动循环（memcpy/word_sum 占绝对主导）；+2015/op 控制面差值
仍在但相对 <1%——与 §"regime structure" 一致。

# 3 结论与边界

- F01 站点（`outstanding()`）样本 0.64%——**可见但不大**；其因果量级
  （−75 instr/op）由 TAX-0D A/B 给出，不靠样本份额断言。
- mutex 聚合样本 ~11.7%（z3w1 ~24%）——F03 结构性存在的证据，但
  **样本热度 ≠ 因果可省**；其预注册消融（同 ownership epoch 的转移合并）
  属结构变更/ADR 领域，本轮不做。
- 无 per-request 时间戳、无全局 tracing、无 eBPF、无生产日志——全部
  外部 perf，production 未动。
