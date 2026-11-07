# RECEIPT — diag-r2（R2 enter/SQE/residue/eventfd 短诊断）

| 字段 | 值 |
|---|---|
| 二进制 | `/home/jnhu/sluice-595-e5/worktree/build/linux/x86_64/release/bench_w02_pipeline` sha256 `32beabcd…` |
| case | `W02-uring-4k-d2`（`SLUICE_BENCH_595_DEPTHS=1`） |
| 臂 | A = `SLUICE_595_ARM=A`（A_R2），B = `SLUICE_595_ARM=B`（B_R2） |
| 参数 | `WARMUP=0 MEASURED=1 STATS=1`、`taskset -c 2`、3 次重复/臂 |
| 工具 | `strace -f -ttt -e trace=io_uring_enter,io_uring_register,eventfd2,eventfd` |
| 锁 | `MEASUREMENT.lock`（会话锁 11:03:31Z–11:08:21Z） |
| 性质 | **仅机制/功能证据**；strace 下的 CPU/墙钟数字不进任何计时统计 |

## 解析修订（保留双记录）

- 第一次解析使用的正则不接受 strace `-f` 的**裸 pid 前缀**（`53606 1793876888.042035 io_uring_enter(...)`），
  因此 `results.jsonl` 的 trace 字段全为 0。`.strace` 文件本身完整（每臂 60,023 行）。
- `harness/reparse_diag.py` 用修正后的解析器重读同一批 `.strace`，写出
  `results_reparsed.jsonl` 与 `summary.json`；**本回执的计数以 `results_reparsed.jsonl` 为准**。
  原始零计数 `results.jsonl` 保留为失败尝试记录，不删除、不改写。

## 计数（3 次重复的均值）

| 臂 | 纯 submit enter | 总 enter | SQE | SQE/enter | max to_submit | register | eventfd | to_submit 直方图 |
|---|---|---|---|---|---|---|---|---|
| A_R2 | 60,004 | 60,004 | 60,004 | 1.000 | 1 | 12 | 4 | `{'1': 180012}` |
| B_R2 | 60,004 | 60,004 | 60,004 | 1.000 | 1 | 12 | 4 | `{'1': 180012}` |

- 每次 invocation 跑 3 个 pass（timed/oracle/instrumented）× 20,000 ops = 60,000 data SQE，
  加 4 次 setup enter，合计 60,004；3 次重复逐次相同。
- A/B 的 enter 数、SQE/enter、to_submit 直方图完全相同；**未观察到 deferred multi-SQE flush**。
- `eventfd` 只出现在 `io_uring_register_eventfd` 的注册/注销路径（`io_uring_register` 12 次/ invocation，
  `eventfd2` 4 次/ invocation），无用户态 eventfd 写入。
- residue：R2 bench 不暴露 `transport_ledger_` 计数；从 `Σto_submit = 60,004 = ΣSQE` 可判定
  本次诊断中所有 SQE 都被内核取走，退出时没有未提交 residue 残留。
- stats 对账：A/B 均为 submits=20,000、poll=10,001、wait=10,000、completed=20,000、
  queue_full_retries=0、invalid_state=0。

## 解释与边界

- D2 形状下没有可合并的提交（每 op 一次 enter），因此 B_R2 的 timed-block 退化
  （CPU +3.435%、p99 +7.052%）**不能**用“批量收益”解释。源码门链（`uring_transport.cpp:84-90`、
  `:229-263`）在每次 dispatch 上增加 `defer_transport_flush_locked_` 检查，并在命中时把 flush
  推到下一个 progress 边界；这与观察到的“enter 数不变、延迟尾部变差”一致，但本诊断只做
  syscall 计数，不能单独证明该归因。
- `uring_595_bounded_progress_test`（`SLUICE_595_ARM=B`）的 11 个 oracle 全过，证明 deferral
  在受控条件下确实工作；本诊断只说明它在 W-02 D2 benchmark 形状中没有形成多 SQE flush。
- 只有 syscall 计数；无 PMU、无 ftrace、无 perf。strace 下 CPU/延迟数字不是性能结论。
