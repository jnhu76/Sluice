## #595 E5 causal-r2（R2）— 因果归因结论与证据归档

**轮次说明**：本轮为 **R2**，在独立会话与独立研究副本中重跑/补测；**不覆盖 R1 的判定**。
R1 的 `KEEP_IMMEDIATE` 与 `/home/jnhu/sluice-595-e5/evidence/` 历史证据保持只读、未改动。
本轮只归因、不修复：`PRODUCT_CHANGE: NONE`、`PRODUCT_PR: NONE`、`MERGED: NO`。

### 结论

| 域 | 裁决 | 一句话 |
|---|---|---|
| CPU（D2 退化） | **ESTABLISHED（源级）** | +3.70% CPU/op 由 defer 决策链的 residue signal → eventfd write 承担；82% 是上限口径，非精确分解 |
| 延迟（D2 p50/p95/p99） | **ESTABLISHED（源级）** | L1 主导：deferred SQE 的 flush 从 dispatch 推迟到下一次 poll；L0（通知）主导被排除 |
| 尾部分量 L2（p99−p95 ~+320 ns） | **PARTIAL** | 归属在 defer 链内，但段级机制与附加尾部构成未观测；≥96.2% 是 p99 总效应的界 |
| 深度边界（D3/D16 改善） | **PARTIAL** | 改善在 defer 链内（干预后 +1.07%/+0.26%），但无 restore，机制分解未闭合 |

### 关键数字（块内配对，正 = B 更差；bootstrap 10k / seed 5950）

- **D2 phaseA**（24 对，无仪器）：CPU/op **+3.73%** [+3.04,+4.42]；p50 **+6.91%**、
  p95 **+4.96%**、p99 **+7.07%**；吞吐 +3.58%。
- **成本分解**（split-d2）：用户 +95.24、提交线程内核 +79.37、io-wq +7.94 ns/op；
  计数器 `sqes_deferred=0.5589/op`、`flushes=actual_enters=1.0000/op`；
  signal 277.5 ns/call、eventfd write 592.5 ns/call、syscw +86,912/inv（8.005 B/次）。
- **CF-1 sigdrop**（24 对合并）：frozen +3.34% → **−0.26% [−0.67,+0.16]**（CPU 归零）；
  延迟仍 **+4.30% / +4.87%**（p50/p99）⇒ 延迟不是通知主导；restore +3.34% 精确重现。
- **CF-2 deferoff**（24 对合并）：frozen +3.90% → **−0.76% [−1.61,+0.06]**；
  p50/p95/p99 **−0.71/−0.87/−0.83%**（归零）；restore +3.94% 同域重现。
- **深度**：D3 −6.69%、D16 −26.36%（phaseA）；同一 deferoff 二进制使改善归零到
  **+1.07% [−0.47,+2.80] / +0.26% [−0.69,+1.04]**；SQE/enter 1.448 / 5.066；
  每避免一次 enter 约省 628 / 623 ns 内核。

### 因果链（D2）

```
O1 B 的 defer 决策（五门全过，0.5589 SQE/op 被持有）
 ├─ C1-m 用户态 signal 277.5 ns/call（含 mutex+epoch 35.3）
 ├─ C3-m eventfd write 592.5 ns/call；syscw +86,912/inv；+138.25 ns/op raw
 └─ O2 flush 时点迁移：dispatch → 下一次 poll（B 29.93% submit 后 <250 ns 即 peek；A 0/120,000）
        ↓
 C5-m 用户停顿 ≈68–71 ns/op（cycles +5.71% vs instructions +1.06%，IPC −4.36%）
        ↓
 提交线程 CPU +189.15 ns/op（+3.70%）；io-wq +7.94（n.s.，排除）
        ↓
 延迟：p50 +532 ns / p95 +634 / p99 +955（L1；L2 附加 ~+320 ns 未分解）
```

### 证据目录（§13 结构，根 = `/home/jnhu/sluice-595-e5/causal-r2/evidence`）

```
README.md  CAPABILITY_MATRIX.md  PROTOCOL.md  CAUSAL_DAG.md  COST_CLASSIFICATION.md
EVIDENCE_GAPS.md  OBSERVABILITY_LIMITS.md  RESULTS.md  ROOT_CAUSE.md  CF0_THREE_ARM.md
raw/            phaseA 五轮原始 results.jsonl
traces/         C1 调用栈、C2 syscall、C3 内核、C4 调度、xlayer 11 节点关联
counters/       cost_table、C1/C2/C3/C4、PMU/split/schedstat/sysio raw、analysis、bin
counterfactuals/ CF-0/CF-1/CF-2 文档、raw、bin、tools、frozen 子清单
analysis/       depth-analysis、ARCHIVE_MOVES.md、final-report-block.md、issue-comment-r2.md
MANIFEST.sha256 全量清单（相对路径，4739 个证据文件；排除解释器缓存）
```

### 清单校验

```
cd /home/jnhu/sluice-595-e5/causal-r2/evidence && sha256sum -c MANIFEST.sha256
# → 4739/4739 OK
```

### 限制（如实登记）

- 非 root 会话：`:k` PMU、tracefs、root bpftrace 不可用；CF-1/CF-2 的
  `K.submit_req` 标记 `UNOBSERVED_ADMIN_UNAVAILABLE`，由用户环前驱 + shim 时点等价覆盖。
- L2 段级直方图（G12）未运行；p99−p95 附加尾部机制 `UNOBSERVED`。
- depth 反事实**无 restore 块**，且未在 depth 二进制上重跑 595 oracle；结论限定于
  无 held-batch 停滞的 `W02-uring-4k` 形状。
- 详细边界、复现命令与逐项证据见 `ROOT_CAUSE.md`、`RESULTS.md`、`EVIDENCE_GAPS.md`、
  `OBSERVABILITY_LIMITS.md`。
