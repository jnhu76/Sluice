# TAX-0D — Causal ablations

Issues: #250 · #259 · PR #260 · Ablation session: `results/tax0d-ablation-wsl2-1/`
Discipline: **one mechanism, one A/B, same semantics** — R0 = 同一二进制
seam 默认（= 生产行为），R1 = 恰好开启一个消融模式。

# Seam 实现边界（研究专用）

- `src/async/tax0_ablation_seams.hpp`（非安装、guard-only）+
  `tests/tax0_ablation_seams.cpp`（只编入 sluice_async_internal_testing）；
- `include/sluice/async/completion.hpp` / `src/async/async_io_context.cpp`
  仅在 `SLUICE_ASYNC_INTERNAL_TESTING` 下分支；production TU 编译产物
  行为逐位不变；默认模式 R0；
- F02 R1 的 seam build 不 Batch-safe（by design，research-only）——harness
  不用 Batch；生产语义候选不因此预设。

# F01 — disabled instrumentation tax

```text
CODE FACT   (census FACT, verified @ HEAD): submit_* 无条件求值
            backend_->outstanding()（虚调用 + arena leaf 锁往返），
            stats_ == nullptr 时纯浪费
A/B         R0 vs R1（stats 门控求值；stats 启用路径逐位相同）
MEASURED    −74..−77 instr/op，7 cells × {z2, z3w1} × read/write 全部一致
            wall 中性（±噪声）→ CPU/control-plane 改进，非吞吐声明
VARIANCE    生产 binary session 间离散 3112/3112/3121（read 4K d32 z2），
            效应远超离散
VERDICT     PROVEN TAX（~75 instr/op ≈ z2 每-op 成本的 2.3–2.5%）
```

# F02 — unused Batch global seq_cst RMW

```text
CODE FACT   (census FACT): 每次 publication 无条件盖章 reap_seq；
            comment 称 relaxed 足够而 ++ 为 seq_cst（分歧已在案）
A/B         F02-A：R1 跳过普通 publication 盖章（Batch 语义在 production
            build 恒同；research seam build 不 Batch-safe，仅测量用）
MEASURED    read cells −4..−2 instr/op；write cells 跨零（4K d32 z2 +23）
F02-B       未运行——预注册门：仅当 F02-A 证明该 atomic materially hot
            才值得 seq_cst vs relaxed；未证明
VERDICT     NEGLIGIBLE（measured regimes；多 worker 争用 UNKNOWN，
            z3w4 不稳定见 OBS-1）
```

# F03 — RequestArena synchronization structure

```text
MEASURED    profile：mutex 聚合 ~11.7% z2 样本（三权威域不可分），
            arena 各函数 0.7–8.3%；结构性 ~9 次 leaf 锁/op（census FACT）
CAUSAL A/B  未做——预注册路径（同 ownership epoch 的顺序保持转移合并）
            属结构变更/新 ADR 领域；分片明令范围外
VERDICT     STRUCTURAL_ONLY（结构 FACT + profile 存在性证据；无因果量级）
```

# F04 — serialization topology

```text
MEASURED    z3w4 vs z3w1：+0.6..0.8% instructions、+40..54% wall
            （4K d32/d64 read）——无 cliff、非 material；单驱动 z2 本设计
            即全串行（census 有意单驱动，无争用）
UNKNOWN     多 worker write cells 未测（OBS-1 不稳定）；split-wait 持锁
            内核 park 的跨线程影响未单独归因
VERDICT     REGIME_SPECIFIC（measured regimes 内 NOT MATERIALLY HOT）
```

# F05 — continuation capability cost

```text
MEASURED    Z3 continuation premium = +787..863 instr/op（d32–d64），
            +1969 @ d1；
            hand-written same-semantic floor（z1bw）= +210 @ d32（摊销）、
            +767 @ d1（此时 cross-thread futex wake 主导：wall 46µs/op
            @ 本机 WSL2——线程对 continuation 在浅深度的物理代价）
READING     对照单元所需语义（park/wake/lost-wake-safe、exactly-once、
            stale 保护、单 consumer、无 cancel）由 z1bw 以 ~1/4 的指令
            成本满足 → Z3 的剩余 +577（d32）是 waiter 注册/routing/
            scheduler 机器的候选 incidental 成本；未做逐组件消融
GUARD       z1bw 不是 poll loop（comparison_guard 遵守）；Z3 额外携带的
            cancel race closure、跨 worker wake、TLA+ 路由协议在对照
            cell 中未被行使——REQUIRED-if-USED，未在本环境计量
VERDICT     REGIME_SPECIFIC（部分 MEASURED，分解 STRUCTURAL_ONLY）
```

# F06+ 新候选

无新增——profile top 符号全部映射回 F01–F05 已登记机制
（submit_transaction/mtx = F03 域；handle_cqe/dispatch/ledger = 生产
必要的 accept/publication 路径；register_waiter 族 = F05）。
