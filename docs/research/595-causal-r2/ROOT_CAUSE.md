# ROOT_CAUSE — Sluice #595 causal-r2 最终根因裁决

状态：FINAL ADJUDICATION。撰写：独立裁决撰写员（2026-11-05，E5）。
范围：D2（`W02-uring-4k-d2`，4 KiB positional read，warm tmpfs，depth=2）A·IMMEDIATE
vs B·BOUNDED_PROGRESS 的 CPU 与延迟退化；边界扩展到 D1/D3/D16 的深度机制。
证据根：`/home/jnhu/sluice-595-e5/causal-r2/evidence/`（下文的相对路径均以此为根）。
基线：主工作树 `/home/jnhu/Source/Sluice` `271b81bf`（裁决时 `git status` 空）；研究改动
只在 `causal-r2/worktree{,-timing,-trace}` 独立副本；R1 预注册门槛只读。

结论纪律：只归因、不修复；不修改公开 API / v2 契约；区分**直接测量 / 反事实支持 / 未知**；
不把估计值冒充直接观测；不把双仪器关联 run 的时延数字用于性能归因；不把某单一层
aggregate 账本没有对应行当作“已解释”。

---

## 0. 裁决摘要

| 域 | 裁决 | 命题（裁决所断言的准确范围） | 主依据 |
|---|---|---|---|
| **CPU（D2 退化）** | **ESTABLISHED（源级）** | B 的 **+3.70% CPU/op（+189.15 ns/op）** 由 defer 决策链的 **residue 通知（`signal_ready_progress()` → eventfd `write`）** 承担；defer 五门准入判断本身、io-wq、enter 路径、缺页/设备路径、调度等待均不构成可测 CPU 成本。**份额分解的 82% 是上限口径，不是 100% 精确分解**（残差 ≈27–35 ns/op UNOBSERVED）。 | 直接测量（计数器/TSC/syscall/线程 CPU/PMU）+ 单因素反事实 CF-1、CF-2 + 撤销复验；门槛逐条见 §3.2 |
| **延迟/尾（D2 p50/p95/p99 位移）** | **ESTABLISHED（源级）** | 延迟分布的整体位移（p50 +6.91%、p95 +4.96%、p99 +7.07%）由 **deferral 时序（flush 从 dispatch 推迟到下一次 poll，L1）** 承担，**不是 residue 通知主导**（L0 主导被排除；CF-1 的 p50/p99 点估计仅回落约 1pp 且 CI 重叠，signal 的残余贡献未单独量化），**不是** CPU 均值平移。 | 直接测量（延迟分布、逐请求结构）+ CF-1（去通知：CPU 归零而延迟保留）+ CF-2（去 deferral：延迟归零）+ 撤销复验（同向/同域） |
| **尾部分量 L2（p99 超出 p95 的 ~+320 ns）与段级分解** | **PARTIAL** | p99 的附加尾部**归属在 defer 链内**（CF-2 使总 p99 差归零，保守消失 ≥96.2%——该界是 **p99 总效应**，不是 p99−p95 附加尾部的界；后者 CI 未计算），但其**段级机制**（defer→flush / 完成投递 / reap 分段）与 p99 比 p95 多出的 ~+320 ns 的构成**未直接观测**（G12 未运行）。 | 反事实支持（归属）+ UNOBSERVED（段级机制） |
| **深度边界（D3/D16 的改善）** | **PARTIAL** | D3 的 **−6.69%** 与 D16 的 **−26.36%** CPU/op 改善在 **defer 决策链内**（同一 `deferoff` 变异二进制 8 对/深度使其归零到 +1.07%/+0.26%，CI 含 0）；但**没有恢复生产二进制并复现的 restore 块**（phaseA 干预前、不同会话/二进制的测量不构成 restore），且“全部改善由 enter 摊销承担”**超出证据**：直接测得的提交线程内核分量约 54.8%/50.3%，io-wq 分量约 29.1%/21.0% 机制 UNOBSERVED，用户分量约 16.8%/28.8% 只是与更少 submit 准备一致的间接解释。 | 直接测量（计数器/trace/strace/线程 CPU）+ 同二进制单因素反事实（无 restore）；机制分解 PARTIAL |

三条裁决边界（先写在前面，防止被误读）：

1. **工作负载边界**：所有因果结论限定于 `W02-uring-4k` warm-tmpfs 形状（D1/D2/D3/D16，
   以 D2 为主结论对象）。CF-2 的 `deferoff` 变异体按设计**不满足 arm-B held-batch 进度
   契约**（595 oracle rc=134，见 `counterfactuals/raw/correctness/mutant-receipts-cf2.log`），
   因此“defer 决策链承担全部效应”不能外推到存在 held-batch 停滞的形状。
2. **粒度边界**：CPU 侧的“因果源”是 ESTABLISHED；CPU 的精确份额分解（C5 微架构停顿
   68–71 ns/op、eventfd write 内部阶段、残差 ≈27–35 ns/op）仍是 CORRELATION_PARTIAL /
   UNOBSERVED，**82% 是上限口径而非精确分解**。延迟侧的“因果源”是 ESTABLISHED（L1 主导）；
   段级分解（G12）与 L2 尾部机制是 PARTIAL。深度改善是 PARTIAL：在 defer 链内成立，
   但缺恢复复验，机制分解（enter 摊销 vs io-wq/用户分量）未闭合。
3. **不是修复建议**：本裁决不主张任何优化方向。D3/D16 的摊销收益与 D2 的代价是同一
   机制在不同深度下的两面；是否改变提交策略属于后续决策，不在本裁决内。

---

## 1. 基线与问题

### 1.1 工作负载与协议

- `bench_w02_pipeline`：64 MiB pattern file（warm tmpfs `/tmp/sluice-595-bench` 或
  `/tmp/sluice-595-cost`），20,000 次 4 KiB positional read/rep；窗口 depth=2；
  每 measured rep 由 bench 内部字节 oracle 校验；`exit 0` 硬门；`taskset -c 2`。
- 每 invocation `WARMUP=1 MEASURED=5`（D2/D3/D16 主块）= **100,000 measured op/invocation**；
  延迟 pass 另有 20,000 op/rep（`worktree/bench/bench_w02_pipeline.cpp:468` 记录
  `take` 时刻 − `submitted_ns`，即 submit 返回到 take 的逐 op 延迟）。
- 协议与偏差记录：`PROTOCOL.md`（phaseA 五轮 A/A→D1→D2→D3→D16；A/A 24 对门 PASS：
  cpu 半宽 0.65 pp ≤5、tput 0.64 pp ≤3、p99 1.66 pp ≤3；loadavg 守卫 1.5→4.0 的偏差
  已如实记录；48/48 invocation exit 0，单二进制 SHA `61c6d579…`）。
- 统计：块内 pair 相对差 (B−A)/A，bootstrap 10,000、seed 5950、percentile 95% CI；
  偶对 A→B、奇对 B→A（ABBA 平衡）；块内 A/B 同二进制同布局。

### 1.2 R1 基线、causal-r2 phaseA 复现与差异

- **R1 实测（与 causal-r2 phaseA 是不同会话/二进制，不可混用）**：R1 的 D2 为
  CPU/op **+4.17%** [+3.53,+4.83]、p95 **+5.69%** [+4.90,+6.50]、p99 **+7.45%**
  [+5.77,+9.38]、吞吐 −3.99%；复跑为 +3.05%/+4.98%/+5.81/−2.95%
  （`/home/jnhu/sluice-595-e5/evidence/measurement/summary-tables.md:11-12`；
  `/home/jnhu/sluice-595-e5/evidence/report-595-e5.md:11-14`）。R1 的 performance-protection
  判定为 **FAIL**（首跑 p95 +5.69、p99 +7.45、throughput +3.99 超 5/3% 界；复跑 p99 +5.81
  仍超界；`/home/jnhu/sluice-595-e5/evidence/report-595-e5.md:126-144`、`:196`）。
  `PREREGISTERED.md`（140 行）只含门槛/校准，**不含这些 D2 实测结果**。
- **causal-r2 phaseA（本轮复现，独立会话/二进制 `61c6d579…`）**：CPU/op **+3.73%**
  [+3.04,+4.42]、p50 +6.91%、p95 +4.96%、p99 +7.07%
  （`raw/phaseA/ab-d2-w02-4k/`；`PROTOCOL.md` §Results；`counters/analysis/latency-percentiles.md`）。
  裁决撰写时从 `results.jsonl` 重算得 +3.725% [+3.036,+4.423]，与 phaseA 文档一致（§附录 B）。
  **它是 phaseA 的结果，不是 R1 冻结基线**；R1 与 phaseA 在方向/量级上同域
  （CPU 4.17% vs 3.73%，p95 5.69 vs 4.96，p99 7.45 vs 7.07），但数值不可互换引用。
- D1 精确对照：`sqes_deferred=0`、`wakeups=0`、`flush=enter=1.0000/op`，CPU/op −0.07%
  [−0.73,+0.63]、p95/p99 中性（`counters/cost_table.md` §3.4；`raw/phaseA/ab-d1-w02-4k/`）。
  ⇒ 机制在 depth=1 精确关闭，排除“计数器/arm 选择器本身带来系统偏差”。
- A/A：phaseA 24 对门 PASS（cpu 0.65 pp、tput 0.64 pp、p99 1.66 pp）；CF-0 的 A/A 12 对
  cpu hw 1.47pp、tput 1.51pp、p99 2.95pp，全部 ≤ 5/3/3 pp 门 PASS
  （`CF0_THREE_ARM.md` §7.4）。

### 1.3 退化与改善的符号地图（phaseA，无仪器，24 对/深度）

| 深度 | CPU/op Δ（B−A） | p50 Δ | p95 Δ | p99 Δ | 机制 |
|---|---|---|---|---|---|
| D1 | −0.07% [−0.73,+0.63] | −0.72% | −0.06% | −0.14% | 机制关闭（deferred=0） |
| **D2** | **+3.73% [3.04,4.42]** | **+6.91%** | **+4.96%** | **+7.07%** | defer 但零 enter 摊销 |
| D3 | −6.69% [−7.25,−6.18] | −3.07% | −11.38% | −9.57% | defer 跨 poll 合并（SQE/enter 1.448） |
| D16 | −26.36% [−26.66,−26.08] | −32.31% | −31.00% | −28.68% | 批量 5.07 SQE/enter |

来源：`raw/phaseA/{ab-d1-w02-4k,ab-d2-w02-4k,ab-d3-w02-4k,ab-main-w02-4k-d16}/results.jsonl`；
`PROTOCOL.md` §Results；`analysis/depth-analysis/DEPTH_MECHANISM.md` §2。裁决撰写时重算 D2/D3/D16
的 CPU 为 +3.725/−6.687/−26.358%、p50/p95/p99 与上表逐项一致（§附录 B）。

---

## 2. 因果结构：originating / mediating / observed symptom

### 2.1 因果图（最终形态）

```
[O1] B·BOUNDED_PROGRESS 的 defer 决策 + residue 通知            [originating]
  defer_transport_flush_locked_ 五门(owner/payload/witness/cap) 全过
  → 0.5589 SQE/op 被持有（sqes_deferred 0→0.5589；确定性计数）
  → advertise_residue(unsubmitted_after_dispatch) → signal_ready_progress()
  （源码：worktree/src/async/uring_transport.cpp:45-126, :94, :111-125,
          defer 函数 :246-294, cap :284；kDeferCap=8 在
          include/sluice/async/detail/uring_backend.hpp:305）
        │
        ├─────────────────────────────┬──────────────────────────────┐
        ↓                             ↓                              ↓
 [C1-m] signal 用户态执行      [C3-m] write(eventfd) 内核路径    [O2] flush 时点迁移
 277.5 ns/call（含 write）      592.5/562.7 ns/call；0.2353/op   dispatch → 下一次 poll
 signal 155.8 ns/op；           +=138.25 ns/op raw；syscw        （C2 shim：B 29.93% submit 后
 mutex+epoch 35.3；defer +6.6   +86,912/inv；8.005 B/次          <250 ns 即 peek；A 0/120,000）
 [mediating，直接测量]          [mediating，直接测量]             [originating-时序，直接观测]
        │                             │                              │
        ↓                             ↓                              ↓
 [C5-m] 用户微架构停顿          （并入提交线程 stime             [L1] deferred op 的内核工作起点
 cycles+5.71% / instructions+1.06%  +79.37 ns/op [37.8,124.6]）      推迟一个 poll 周期
 IPC−4.36%、cache-ref+23.31%、                                        [结构：每请求级直接观测；
 branch-miss+14.94%；≈68–71 ns/op                                   延迟后果：反事实裁决]
 [mediating，CORRELATION_PARTIAL]                                      │
        │                                                              ↓
        └──────────→ [S-CPU] 提交线程 CPU/op +189.15 ns（+3.70%）←──────┘
                        ├ 用户态 +95.24（+7.39%）
                        ├ 内核态 +79.37（+3.99%）
                        └ io-wq +7.94（+0.46%，n.s.，排除）
                                   │
                                   ↓
                        [S-THR] 吞吐 −3.55% [−4.34,−2.84]（observed symptom）
                                   │
                        [S-LAT] p50 +6.91%（+532 ns）/ p95 +4.96%（+634）/ p99 +7.07%（+955）
                                   ↑
                        [L1] deferral 时序（CF-1/CF-2 裁决）
                        [L2] p99 附加 ~+320 ns（UNOBSERVED 机制）
```

### 2.2 originating：只有一个 B 特有决策

- **O1（defer 决策 + residue 通知）**：B 臂在 `defer_transport_flush_locked_()` 五门全过后
  **不 submit**，把 SQE 留在 `transport_ledger_`，并在 `dispatch_after_accept()` 尾部因
  `unsubmitted_after_dispatch` 为真调用 `signal_ready_progress()`。
  证据等级：**直接测量 + 源码路径**。计数 `sqes_deferred`/`wakeups` 0→0.5589/op、
  `wakeup_writes` 0.2353/op、`wakeup_coalesced` 0.3236/op（`counters/raw/cnt-on-full-d2/*B.err`；
  裁决时从原始 stderr 重算为 0.5589/1.0/1.0/0.5589/0.2353/1.2354/0.7942，与
  `counters/cost_table.md` §3.2 一致）。
- **O2（flush 时点迁移）**：同一 SQE 的 `io_uring_submit` 从 dispatch 现场移到下一次
  `poll()` 的无条件 flush（`src/async/uring_backend.cpp:634-658`，flush 在 :652、
  reap 在 :657）。证据等级：**直接观测**（C2 shim：B 35,921/120,000 = 29.93% 的 submit
  后 <250 ns 即 `peek_batch_cqe`，A 0/120,000；`traces/c2-syscall/README.md` §0.3；
  xlayer 每请求 join：defer 1045/1035/1038，flush 2000/phase 不变，enter to_submit≡1，
  `traces/xlayer/README.md` §5.3）。
- **口径分离**：O1 的 gate 链本身不是 originating。CF-0 的 C 臂（执行完整门链、但强制
  return false）与 A 的 CPU 差为 **−0.52% [−1.28,+0.22]**（D2）、**−0.13% [−1.45,+0.82]**
  （D16），CI 均含 0；C1 的 in-situ 门增量 +6.6 ns/call（`CF0_THREE_ARM.md` §7、
  `counters/C1_COUNTERS.md` §3）。⇒ 成本来自 **defer 动作及其通知**，不是准入判断。

### 2.3 mediating：成本承载层

1. **C1-m（用户态 signal 执行）**：`ProgressSource::signal()` 277.5 ns/call（clean，
   12 invocation CI [275.3,280.0]），其中 mutex+epoch 35.3、eventfd `write` 592.5；
   `signal` 155.8 ns/op；新增**用户态指令执行**仅 ≈24–27 ns/op（mutex+epoch 14–17、
   defer 6.6、write 用户 stub ≈3.7）。**直接测量**（in-situ TSC，`counters/C1_COUNTERS.md` §3、§5.1）。
2. **C3-m（eventfd write 内核路径）**：B 唯一新增内核 syscall 是 8 字节 eventfd write：
   syscw B−A **+86,912 [86,541,87,263]/invocation**、wchar/syscw = **8.005 B/次**；
   measured 密度 0.24078/op；单次 562.7–592.5 ns/call；per-op **+138.25 ns/op raw**
   [+136.8,+139.8]，**超过**提交线程内核净增量上限 +124.6。read/poll/drain 面为 **负**
   （−0.00718/op、−13.03 ns/op [−17.61,−8.45]）；io-wq 12/12 invocation 零 read/write
   syscall；futex 2/进程常量。**直接测量**（`counters/C3_COUNTERS.md` §0、§2–§5；
   `counters/raw/sysio-d2/`、`raw/c3-interleaved-d2/`）。
3. **C5-m（用户态微架构停顿）**：`instructions:u` +1.06% vs `cycles:u` +5.71%（指令只解释
   18.6% 的 cycles 增量）；IPC −4.36%、cache-references +23.31%、branch-misses +14.94%；
   cache-misses +0.65%、page-faults +0.15%（均 n.s.）。用户 +95.24 ns/op 中约 3/4 不是新增
   指令执行，而是新增 signal/eventfd 路径的停顿/缓存/分支成本，≈68–71 ns/op。
   **CORRELATION_PARTIAL**（PMU 事件级归因，非指令级；`counters/cost_table.md` §3.1 #5–11；
   `EVIDENCE_GAPS.md` G7）。
4. **C4-m（通知/调度路径，已降级）**：提交线程 runnable delay +36.4 ns/op（+0.94%
   [+0.38,+1.50]），仅为同窗口 on-CPU 增量 +474.8 的 **7.7%**；`progress_handoffs=0`；
   写 eventfd 的线程就是唯一可能的 park 者，写时 waitqueue 为空，不产生 `sched_wakeup`；
   timeslices +0.12% n.s.、迁移 0。**直接测量**（/proc schedstat 1 ms 采样 + 源码论证；
   `counters/C4_SCHED.md` §0、§5；`counters/raw/schedstat-d2/`）。
5. **O2 的 CPU 份额**：flush 单次 702.3→714.4 ns（+12.1，同物挪位），flush 总数 1.0/op
   不变；submit_attempts 1.7893→1.2354（少掉的是空 ledger 调用）。**直接测量**
   （`counters/C1_COUNTERS.md` §3；`counters/cost_table.md` §3.2 #21–#23）。

### 2.4 延迟机制：L1 / L2

- **L0（CPU 均值平移）**：**排除的是“L0 主导”，不是信号对延迟的零贡献**。CF-1 `sigdrop`
  （只去掉 residue 通知）后 CPU/op B−A 已 **−0.26% [−0.67,+0.16]**（CI 含 0），而 p50
  **+4.30% [+2.64,+6.61]**、p99 **+4.87% [+3.96,+5.75]** 仍显著 >0
  （`counterfactuals/CF1_SIGDROP.md` §6–§7）。相对 frozen，CF-1 的 p50 点估计从 +5.38%
  降到 +4.30%、p99 从 +5.85% 降到 +4.87%（约 1pp，CI 重叠）——所以 signal 的残余贡献
  可能非零但远小于 deferral；本裁决不宣称 signal 对延迟为零。
  ⇒ 延迟位移不是 CPU 均值平移**主导**（L0 主导排除），signal 的残余贡献未单独量化。
- **L1（deferral 时序）**：deferred SQE 的内核工作起点被推迟到下一次 poll；A 臂在
  dispatch 现场 submit。结构前提是**每请求级直接观测**：xlayer 的 defer 52% 请求、
  flush 2000/phase 不变、enter to_submit≡1、deferred SQE 由下一 poll 的无条件
  `submit_transport` 吸收；warm tmpfs 4k read 100% 经 io-wq 排队且完成由 io-wq worker
  投递（两臂同构，`traces/xlayer/README.md` §5.1–§5.3）。**延迟后果的因果裁决**由
  CF-2 `deferoff` 承担：p50 −0.71% [−2.57,+1.90]、p95 −0.87% [−1.71,−0.10]、
  p99 −0.83% [−2.12,+0.23] 全部归零；消失比例点估计 111.4/116.7/113.8%，保守口径
  （干预 CI 上界）≥69.7/≥101.9/≥96.2%（`counterfactuals/CF2_DEFEROFF.md` §7.2、§8.2）。
  两反事实联合：**延迟位移随 deferral 消失而消失、不随通知消失而消失 ⇒ L1 是源**。
- **L2（p99 附加尾部）**：p99 绝对位移 +955 ns vs p95 +634 ns，p99 比 p95 多 ~+320 ns
  （`counters/analysis/latency-percentiles.md` §3）。其**归属**在 defer 链内（CF-2 使
  p99 总差归零，保守消失 ≥96.2%）；但要注意这个 ≥96.2% 是 **p99 总效应**
  （1 − 0.23/6.03，`counterfactuals/CF2_DEFEROFF.md` §7.2）的界，**不是 p99−p95 附加尾部的界**，
  后者的 CI 未计算。**机制**也未观测：没有 latency-pass 的逐请求分段直方图
  （dispatch→defer→flush→完成→reap→take × deferred 标志），也没有逐事件
  wakeup→run 分布。**UNOBSERVED / PARTIAL**（`EVIDENCE_GAPS.md` G12；§8 本文）。

### 2.5 observed symptom（不是机制）

- CPU/op +3.70% [2.93,4.55]（split-d2）；吞吐 −3.55% [−4.34,−2.84]；
  p50 +6.91% [+5.03,+9.23]（+532 ns）、p95 +4.96% [+4.23,+5.74]（+634 ns）、
  p99 +7.07% [+4.24,+11.78]（+955 ns）。
- `cycles:u +5.71%`、`IPC −4.36%`、`cache-references +23.31%`、`branch-misses +14.94%`
  是 PMU 观测行，不是独立机制（`counters/cost_table.md` §3.1）。
- 吞吐 −3.55% 与 CPU +3.70% 量级一致（同一 pipelined throughput pass），不构成需要
  独立机制的 symptom；不用吞吐反推延迟已解释（不同 pass、不同测量）。

---

## 3. CPU 裁决：ESTABLISHED

### 3.1 裁决

D2 的 **CPU/op +3.70%（+189.15 ns/op）** 由 **defer 决策触发的 residue 通知路径**
（`signal_ready_progress()` → `ProgressSource::signal()` → eventfd `write`）承担；
defer 五门准入判断本身、deferral 持有动作本身（在无通知时）、io-wq、`io_uring_enter`、
缺页/设备、调度等待均不构成可测 CPU 成本。证据等级：**反事实支持（单因素、机制闭合、
oracle 通过、撤销复验）→ 严格因果（源级）**。这里的“源级”指因果源已由两个干预点
锁定；份额分解（82% 上限口径）不是 100% 精确账。

### 3.2 ESTABLISHED 门槛逐条核验

| 门槛 | 是否满足 | 证据 |
|---|---|---|
| **复现** | ✅ | phaseA +3.73% [3.04,4.42]；CF-0 +3.21% [+2.43,+4.02]；CF-1 base +3.34% [+2.30,+4.35]；CF-2 base +3.86% [+2.38,+5.40]；split-d2 +3.70% [2.93,4.55]；C1 OFF +4.24%；C3 OFF +3.18%；C4 +3.11/+3.88/+3.88%。跨会话、跨构建、同向且 CI 均排除 0。 |
| **精确定位** | ✅ | CF-1 只从 `advertise_residue` 的或项移除 `unsubmitted_after_dispatch`（`worktree/src/async/uring_transport.cpp:111-125`，宏 `SLUICE_595_MUTANT_DEFER_RESIDUE_SIGNAL_DROPPED`），deferral/门链/flush 时点不变；CPU 增量消失。机制读数：`wakeups`/`wakeup_writes`/`wakeup_coalesced` 0.5463/0.2472/0.2991→**0**；`sqes_deferred` **0.5448**（同轮 frozen B 0.5463，保持不变）；`flushes`/`actual_enters` 仍 1.0000/op；syscw B−A **+86,344→0**（`counterfactuals/CF1_SIGDROP.md` §5.1–5.2）。⇒ 定位到 signal/eventfd write 单路径。 |
| **机制与成本一致** | ✅（上限口径，非闭合账） | C1 直接测得 signal 155.8 ns/op + defer 增量 6.6 = 162.4 raw / ≈154.6（扣 TSC pair）≈ **82%** 的 Phase B 中央 +189.15（三块比例 67–94%）；C3 的 write on-CPU +138.25 ns/op 已超过内核净增量上限 +124.6；C5 的 ≈68–71 ns/op 是 signal 路径的用户态停顿。剩余 ≈27–35 ns/op 为 TSC/tick 口径差 + 未逐项归因（UNOBSERVED）。**82% 不得读作 100% 精确分解**；但 CF-1 证明**总增量整体消失**，残差不构成替代源。 |
| **单因素干预消除/显著减轻 + 撤销复验** | ✅ | CF-1 合并 24 对：frozen +3.34% [+2.62,+4.07] → sigdrop **−0.26% [−0.67,+0.16]**；消失比例点估计 107.8%、保守 ≥95.2%（≥60% 门槛）；restore **+3.34% [+2.40,+4.38]** 精确重现 base。CF-2 独立第二干预点：frozen +3.90% [+2.91,+4.91] → deferoff **−0.76% [−1.61,+0.06]**；restore **+3.94% [+2.60,+5.26]** 重现 base（`counterfactuals/CF2_DEFEROFF.md` §7.1）。 |
| **正确性不变** | ✅（有边界） | sigdrop 构建：13 个 uring B-mode target **13/13 rc=0**（含 595 oracle “all checks passed”，`counterfactuals/raw/correctness/bmode-mutant-suite.log`）；全部**计时** invocation exit 0、每 invocation 恰 100,000 measured op（逐块核验，§附录 B）、bench 字节 oracle 通过；机制 companion 块的运行记录见 `counterfactuals/CF1_SIGDROP.md` §5。CF-2 deferoff 为**设计内拒绝**：595 oracle 22 项 FAIL + rc=134、B-mode 12/13 rc=0 + 595 rc=134（`counterfactuals/raw/correctness/mutant-receipts-cf2.log`、`bmode-mutant-suite-cf2.log`）——证明干预真实到达 defer 语义层，同时把结论限定在无 held-batch 停滞的形状（§11）。 |
| **替代解释排除** | ✅ | 见 §7 表：io-wq、enter、batching、缺页/内存、设备、调度等待、sched_wakeup、用户指令执行、频率/漂移、探针开销、门链本身、信号以外的 defer 分支/微架构 layout 均有负结果或独立干预排除。 |

### 3.3 成本对账（CPU，D2，直接测量）

| 层 | 分量（ns/op） | 证据等级 |
|---|---|---|
| C3 eventfd write on-CPU | +132~138（raw +138.25；clean +135.2） | 直接测量（TSC × 密度；syscw/wchar 内核记账） |
| C1 mutex+epoch + write 用户 stub + defer 门 | +26~33（执行 ≈24–27 + defer 6.6） | 直接测量（in-situ TSC） |
| C1 flush 单次变贵 | +12.1（702.3→714.4 ns/call，1.0/op） | 直接测量 |
| C5 用户停顿（cycles−指令差） | ≈68–71 | CORRELATION_PARTIAL（PMU 差值，G7） |
| U 残差（TSC/tick 口径差 + 未归因） | ≈27–35（中央） | UNOBSERVED |
| **合计（对 +189.15）** | | write 单项已超内核净增量上限；中心解释比例 ≈80%，范围 67–94% |

口径声明（必须随数字引用）：C1 完整探针集的 B 开销为 +2.72%，与效应同量级，
因此 **ON 块不作为定量效应**；定量效应取无仪器的 OFF 块与 Phase B split-d2。
write 的 132–138 是 on-CPU 墙钟（含内核），不是 `:k` 指令级分解。
`counters/cost_table.md` §3.1、§4、§5；`counters/C1_COUNTERS.md` §5–§6。

### 3.4 为什么不是别的（CPU 侧竞争解释的排除摘要）

详见 §7。最关键的三条：

1. **不是 io-wq**：io-wq 内核 CPU +0.46% [−0.44,+1.36] n.s.；io-wq 线程 syscr/syscw
   在 12/12 invocation 恒 0；io-wq 的 complete 投递在两臂同构（xlayer 6004/6004 由
   worker 投递；`traces/xlayer/README.md` §5.3）。
2. **不是 `io_uring_enter`**：enter 100% submit-only `(to_submit=1,min_complete=0,flags=0)`
   ret≡1；strace B−A 时长 +67.7 ns [−154.1,+287.5]、shim +37.9 ns [−33.7,+110.4]，
   CI 均含 0（`traces/c2-syscall/README.md` §0.1–0.2）。
3. **不是门链/微架构 layout**：CF-0 C−A ≈0；CF-1 残量 CI 上界 +0.16% <2.5%；
   CF-2 残量 CI 上界 +0.06% <1%——G10 假设 (b)(c) 不触发。

---

## 4. 延迟/尾裁决：ESTABLISHED（源级）+ PARTIAL（L2/段级）

### 4.1 直接测量：分布整体抬升

phaseA 24 对（`raw/phaseA/ab-d2-w02-4k/results.jsonl`，重算一致）：

| 分位 | 相对差 | 绝对位移 | 备注 |
|---|---|---|---|
| p50 | **+6.91% [+5.03,+9.23]** | **+532 ns** | 相对位移 ≥ p95 ⇒ 不是纯尾部加宽 |
| p95 | **+4.96% [+4.23,+5.74]** | **+634 ns** | phaseA 同值（非 R1：R1 p95 为 +5.69%） |
| p99 | **+7.07% [+4.24,+11.78]** | **+955 ns** | p99 比 p95 多 ~+320 ns（L2） |

- CPU 位移只有 +189.15 ns/op（split-d2）/ +177 ns（phaseA 口径），延迟位移的
  超额 **+343~766 ns/op** 是 wall/等待分量；C4 的 runnable delay +36.4 ns/op 比超额
  小一个量级（`counters/analysis/latency-percentiles.md` §2–§3；`counters/C4_SCHED.md` §6）。
- 三个独立冻结块复现同一形态：phaseA（+6.91/+4.96/+7.07）、CF-1 frozen 24 对
  （+5.38/+4.73/+5.85）、CF-2 frozen 24 对（+6.26/+5.19/+6.03）。

### 4.2 反事实分叉：L0 主导被排除、L1 被确认

| 反事实 | 单因素 | CPU/op B−A | p50 B−A | p95 B−A | p99 B−A | 机制读数 | 裁决 |
|---|---|---|---|---|---|---|---|
| **CF-1 sigdrop** | 只去 residue 通知；deferral/门链/flush 时点不变 | **−0.26% [−0.67,+0.16]** | **+4.30% [+2.64,+6.61]** | +4.28% [+3.58,+4.92] | **+4.87% [+3.96,+5.75]** | wakeups/writes→0；sqes_deferred 0.5448；flushes/enters 1.0；syscw 差→0 | CPU 源=通知；**延迟≠通知主导**（L0 主导排除） |
| **CF-2 deferoff** | 入口 `return false`；deferral 与通知一并消失 | **−0.76% [−1.61,+0.06]** | **−0.71% [−2.57,+1.90]** | **−0.87% [−1.71,−0.10]** | **−0.83% [−2.12,+0.23]** | sqes_deferred/wakeups→0；submit_attempts 1.7901（A 形态）；syscw 差 +88,768→+0.3 | **延迟源=deferral（L1）** |

消失比例（CF-2，frozen 合并 → deferoff 合并）：p50 点估计 111.4% / 保守 ≥69.7%；
p95 116.7% / ≥101.9%；p99 113.8% / ≥96.2%（`counterfactuals/CF2_DEFEROFF.md` §7.2）。
撤销复验：restore 块 p50 +5.05%、p95 +4.95%、p99 +5.25%（CF-2）与 base +7.46/+5.42/+6.81
同向/同域重现（p50 差 2.4pp）；CF-1 的 restore 为 p50 +8.21%、p95 +5.04%、p99 +6.68%
vs base +2.56/+4.42/+5.03（p50 差 5.7pp，`counterfactuals/CF1_SIGDROP.md` §6）。分位数块间波动大，
故使用 24 对合并口径（`counterfactuals/CF2_DEFEROFF.md` §7.1、§9）。**“restore 复现”在延迟侧只成立为
“同向/同域”，不是逐分位点的精确重现**；CPU/总效应的 restore 精度见 §3.2。

### 4.3 机制—成本一致性（源级成立，段级未闭合）

- **机制**：deferred SQE 直到下一次 `poll()` 才 flush；A 臂在 dispatch 现场 submit。
  该结构是**每请求级直接观测**（xlayer：defer 52%、flush 2000/phase 不变、to_submit≡1；
  C2 shim：29.93% 的 B submit 后 <250 ns 即 peek，A 0/120,000）。
- **成本量级**：D2 的 defer 占比 0.5589（确定性计数）；trace 构建（单仪器）测得
  defer→flush delay p50/p90/p99 = **617/1234/2777 ns**、mean 820 ns（3 phase 一致，
  20,000 dispatch/phase；裁决时用 `analysis/depth-analysis/tools/parse_trace.py` 重跑复核，§附录 B）。
  以“deferred op 至少多等一次 poll flush”作零阶估计：0.5589 × 820 ≈ **458 ns/op**，
  与实测 p50 位移 **+532 ns** 同量级、同方向；p99 的 2777 ns 长尾与 L2 的 ~+320 ns
  附加尾部方向一致。**但这不是闭合账**：trace 构建对时序有 ~0.7 µs/op 量级扰动，
  latency pass 的逐 op 延迟又包含应用侧窗口排队，无法据此把每一纳秒分摊到段。
- **段级缺失**：G12 的停止条件是“分层直方图把 ≥60% 的 p50/p95 超额归到具体段，或
  CF-1 已裁决 L0/L0-排除且上层决策不再需要段级分解”（`EVIDENCE_GAPS.md` G12）。
  CF-1 已裁决 **L0 主导**排除、L1 升级，预注册的替代停止条件满足；但**段级直方图未运行**，
  因此“机制与成本一致”在**决策链层**成立、在**段层**不成立。**L0 的残余贡献（约 1pp）
  没有单独量化**，所以这里的“一致”不宣称 signal 对延迟为零。
- 结论：**LATENCY_CAUSE ESTABLISHED（源级：L1 主导）**；
  **L2/段级机制 PARTIAL**。若未来需要段级分解，现成仪器是已验证的用户态 7 点环
  （`traces/xlayer/tool_research_595_trace.hpp`，单仪器、dropped=0），不需要新的跨层构建。

### 4.4 延迟侧替代解释排除

- **通知（L0）**：CF-1 去掉通知后延迟仍 +4.30/+4.87%；排除的是“主导”，不是“零贡献”
  （相对 frozen，p50/p99 点估计各回落约 1pp，CI 重叠）。
- **CPU 均值平移**：CF-1 的 CPU 差已 ≈0 而延迟仍显著；排除。
- **门链本身**：CF-0 C−A ≈0；排除。
- **调度等待**：runnable +36.4 ns/op（7.7% of on-CPU 增量），且 off-CPU 不计 CPU；
  eventfd write 不产生 sched_wakeup（写者=唯一 park 者）；排除为根因。
- **io-wq**：两臂同构（qasync/complete 6004/6004；worker 投递），CPU +0.46% n.s.；排除。
- **p99 罕见调度/收割长尾作为独立源**：CF-2 使 p99 总差归零（保守 ≥96.2%，该界是
  p99 总效应，不是 p99−p95 附加尾部的界），说明尾部位移也在 defer 链内；但尾部
  **形状**机制仍未分解（L2 PARTIAL）。

---

## 5. 深度边界裁决：D3/D16 的改善在 defer 链内，机制分解 PARTIAL

### 5.1 直接测量

| 指标（per op） | D2 | D3 | D16 |
|---|---|---|---|
| `sqes_deferred` | 0.5589 | 0.6904 | 0.8762 |
| `flushes` = `actual_enters` | 1.0000 | **0.6905** | **0.1974** |
| **SQE/enter** | 1.000 | **1.448** | **5.066** |
| trace flush-size 直方图（20k dispatch，phase 1） | `{1:20000}` | `{1:7128, 2:6436}` | `{1:1343, 7:1166, 8:1311}`（+2 个 3/4 的相位边界小 flush） |
| dispatch 时 ledger 非空 | **0/20000** | **6436/20000（32.2%）** | **16178/20000（80.9%）** |
| strace `to_submit` 直方图（单 run） | A 全 1 | B `{2:39991, 1:40018}` | B `{8:7494, 7:7494, 1:7590}` |
| phaseA CPU/op Δ | **+3.73%** | **−6.69%** | **−26.36%** |
| phaseA p95 / p99 Δ | +4.96 / +7.07% | −11.38 / −9.57% | −31.00 / −28.68% |
| 每避免一次 enter 的内核节省 | — | 194.4 ns / 0.3095 = **628 ns** | 500.0 ns / 0.8026 = **623 ns** |

来源：`analysis/depth-analysis/DEPTH_MECHANISM.md` §2、§4、§5；`analysis/depth-analysis/raw/cnt-d{3,16}-ab/`；
`analysis/depth-analysis/raw/trace-d{2,3,16}/`；`analysis/depth-analysis/raw/strace-d{3,16}/`；
`analysis/depth-analysis/raw/enterargs-d{3,16}/`；`analysis/depth-analysis/depth_ledger_summary.json`。

**strace 口径限定（必须随表引用）**：strace 块是受 ptrace 扰动的块，其 enter 密度低于
计数器块——D3 B **80,017/120,000 = 0.667/op** vs 计数器 **0.6905/op**；D16 B
**22,585/120,000 = 0.188/op** vs 计数器 **0.1974/op**（低 3.4–4.7%）。
strace 只用于 syscall 类别与 `to_submit` 参数直方图，**不用于 enter 率或密度的定量**
（`analysis/depth-analysis/DEPTH_MECHANISM.md:357-358` 同款声明）。表中 strace 行与计数器行不是同一口径。

### 5.2 机制：为什么 D2 是精确零、D3/D16 才有摊销

- D2 的窗口=2：每次 consume 后补一个 submit，队首未 ready 就 wait→poll；`poll()` **先无条件
  flush 整个 ledger，再 reap**（`worktree/src/async/uring_backend.cpp:652-657`）。warm tmpfs
  4k read 由 io-wq 极速完成并在同一 pass 被 reap，`live_cookies_` 归零；下一次 dispatch 的
  witness 门 `live(0) > ledger(0)` 不成立（`uring_transport.cpp:277`），只能 immediate。
  trace 直接看到 `immediate→defer→poll(flush 1)→immediate→…`，flush 之间 defer 数直方图
  `{0:9386, 1:10614}`，**没有 ≥2 的桶**。⇒ D2 的 defer 只改变 flush 位置，不减少 enter。
- D3：一次 poll flush 2 个并 published 2 个完成，bench 可跳过 wait 连续 dispatch，两个
  defer 落入同一 flush（32.2% 的 dispatch 看到 ledger 非空，全部 =1）。
- D16：窗口 16 让 ledger 单调填到 7/8；cap=8 在 **6.4%** 的 dispatch 强制 immediate
  （flush 8 个 SQE）；witness pass 94.1% vs 最终 defer 87.7%。
- 通知成本随合并率下降：eventfd write 0.2353→0.1862→0.0540/op；write/wakeup
  42.1%→27.0%→6.2%（`analysis/depth-analysis/DEPTH_MECHANISM.md` §2、§5）。
- **口径提醒**：以上 enter 摊销是直接测到的**最大单项**，不是全部改善的完整分解；
  io-wq 与用户分量的机制未闭合，且深度反事实没有 restore（§5.3）。

### 5.3 反事实与裁决

- 同一 `deferoff` 变异构建（sha `64189aa8…`）在 D3/D16 各跑 8 对同二进制 ABBA
  （400k measured op/invocation；PMU-only + reuse fixture，非 phaseA 的 full 协议）：
  D3 B−A **+1.07% [−0.47,+2.80]**、D16 **+0.26% [−0.69,+1.04]**（CPU CI 均含 0）；
  对照 phaseA D3 −6.69%、D16 −26.36%——**改善在 defer 链被整体关闭后消失**
  （`analysis/depth-analysis/DEPTH_MECHANISM.md` §0、§4–§5；`analysis/depth-analysis/raw/deferoff-d{3,16}/`；
  裁决撰写时从逐 invocation jsonl 重算 +1.066% [−0.475,+2.804] / +0.259% [−0.688,+1.041]，一致）。
- **没有 restore 块**：`analysis/depth-analysis/tools/run_deferoff.sh` 只跑 A/B 两臂；
  `raw/deferoff-d{3,16}/` 只有 16 invocation/深度（8 对×2 臂），没有“撤回到生产二进制
  并复现”的块。phaseA 的 −6.69%/−26.36% 是**干预前、不同会话、不同二进制**
  （`61c6d579…`）的测量，**不能充当 restore**。⇒ 深度裁决不满足“单因素干预消除 +
  撤销复验”的完整门槛。
- **机制分解不是单因素 enter 摊销**：按 420k op 窗口重算 split-d{3,16} 的线程分解
  （与 `analysis/depth-analysis/DEPTH_MECHANISM.md` §2/§4/§5 一致）：D3 提交线程内核 −194.4 / bench 总差 −354.8
  = **54.8%**、io-wq −103.2 = **29.1%**（`analysis/depth-analysis/DEPTH_MECHANISM.md:62-63,348` 标 UNOBSERVED）、
  用户 −59.5 = **16.8%**（只与更少 submit 准备一致，间接）；D16 内核 −500.0 / −993.1
  = **50.3%**、io-wq −208.3 = **21.0%**（UNOBSERVED）、用户 −285.7 = **28.8%**（间接）。
  `deferoff` 砍的是整条 defer 链（决策 + 通知 + flush 时点），不是“enter 摊销”单因素；
  因此“全部改善由 enter 摊销承担”**超出证据**。enter 摊销是已直接测到的最大单项
  （D3/D16 SQE/enter 1.448/5.066；每避免一次 enter ≈628/623 ns 内核），但不是全部。
- 延迟侧：D3 的 p50 +0.49% [−0.30,+1.42] n.s.、**p95 +1.56% [+0.16,+3.23] 显著**、
  p99 **+4.60% [+0.83,+8.98] 显著**；D16 p50/p95/p99 全部 n.s.（+0.37% [−0.95,+1.49] /
  +0.81% [−0.76,+2.43] / +0.61% [−1.15,+2.28]）。**D3 的 p95/p99 残差如实登记**：
  `analysis/depth-analysis/DEPTH_MECHANISM.md:51` 与本文旧版写“p50/p95 n.s.”，但 p95 的 CI 下界 +0.16% >0；
  正确表述是 **p50 n.s.、p95/p99 显著**。它不改变 CPU 归因，但说明 D3 的延迟改善
  没有被 deferoff 完全“归零”，成因（mutant 布局 / 残余 poll 形态）未分解。
- 正确性：**同一 mutant 宏/源码，不同构建**——`counterfactuals/raw/correctness/` 的
  mutant receipt / B-mode lane 是 frozen-config 构建的收据；depth 的
  `bench_w02_pipeline_deferoff`（sha `64189aa8…`）**没有在该二进制上重跑 595 oracle**。
  depth 运行的正确性面只有：全部 exit 0、每 invocation 400k measured op、bench 字节
  oracle 通过（`raw/deferoff-d{3,16}/*.jsonl` 的 ok=true）。不能把 frozen-config 的
  oracle 收据当作 depth timing 二进制上的 oracle 通过。
- 裁决：**DEPTH_CAUSE PARTIAL**——改善在 defer 链内（单因素干预消除），但缺恢复复验，
  且机制分解（enter 摊销 vs io-wq/用户分量）未闭合；限制见 D3 p95/p99 残差与
  held-batch 契约边界。

---

## 6. 反事实证据汇总

| 反事实 | 单因素干预 | 机制预测（关键） | 性能结果（B−A） | 撤销复验 | 正确性 | 证据等级 |
|---|---|---|---|---|---|---|
| **CF-0 三臂**（A/IMMEDIATE、B/BOUNDED_PROGRESS、C/GATE_ONLY_IMMEDIATE） | 同一二进制运行时选臂；C 执行完整门链但强制 immediate | C 与 A 的提交面完全一致（sqes_deferred=0、wakeups=0、flushes/enters=1.0、submit_attempts≈1.79） | D2 C−A **−0.52% [−1.28,+0.22]**；D16 **−0.13% [−1.45,+0.82]**；D2 B−A +3.21%、D16 −26.80% | A/A 12 对门 PASS（1.47/1.51/2.95 pp ≤5/3/3） | A-mode 80/80、A-explicit 12/13+rc=2、B 13/13、C 12/13+rc=2（设计内） | 反事实支持（单因素、机制闭合、oracle、恢复复验） |
| **CF-1 sigdrop** | 只移除 `advertise_residue` 的 `unsubmitted_after_dispatch` 或项 | wakeups/writes→0；sqes_deferred 仍 0.5589；syscw 差→0 | CPU **−0.26% [−0.67,+0.16]**；p50 **+4.30%**、p99 **+4.87%**（延迟保留） | CPU restore +3.34% 重现 base；延迟分位数同向/同域（p50 +8.21 vs base +2.56） | 13/13 B-mode；字节 oracle；96 invocations exit 0/100k op | 反事实支持 → 严格因果（CPU）；L0 主导排除 |
| **CF-2 deferoff** | `defer_transport_flush_locked_` 入口 `return false`（单点） | sqes_deferred/wakeups→0；submit_attempts→1.79；poll-pass flush 群体→0；syscw 差→0 | CPU **−0.76% [−1.61,+0.06]**；p50/p95/p99 **−0.71/−0.87/−0.83%**（归零） | CPU restore +3.94% 同域；延迟分位数同向/同域（p50 +5.05 vs base +7.46） | BASE GREEN rc=0；mutant receipt 22 FAIL rc=134（设计内）；12/13 B-mode + 595 rc=134 | 反事实支持 → 严格因果（CPU+延迟）；第二干预点 |
| **CF-2 复用 D3/D16** | 同上，D3/D16 各 8 对 | 同上（D3/D16 形态）；但整条 defer 链关闭，不能分解到 enter 摊销单项 | D3 **+1.07% [−0.47,+2.80]**、D16 **+0.26% [−0.69,+1.04]**；改善消失 | 同二进制块内 ABBA；**无独立 restore 块**（8 对/深度；phaseA 干预前测量不构成 restore） | **不同构建**：同一 mutant 宏/源码，未在 depth 二进制上重跑 595 oracle；仅 exit 0 + 字节 oracle + 400k op/inv | 反事实支持（改善在 defer 链内）；机制分解与恢复复验 PARTIAL |

CF-1/CF-2 的 96+96 invocation 全部 exit 0、每 invocation 恰 100,000 measured op、每块内
A/B 同二进制 SHA（CF-1：`0091d259`/`4de23299`；CF-2：`0091d259`/`64189aa8`），
裁决撰写时逐块核验通过（§附录 B）。CF-0 的 204/204 invocation exit 0、每臂每 invocation
恰 100,000 op（`CF0_THREE_ARM.md` §6.4）。

**过度消失说明**：CF-1 点估计 107.8%、CF-2 点估计 119.4% 的“消失比例”>100%，意味着
干预后 B 臂净露出 ≈0.3–0.8% 的微小优势（poll 合并形态的 submit_attempts 1.25 vs 1.79），
量级在噪声半宽边缘，CI 含 0；不单独归因，仅记录（`counterfactuals/CF1_SIGDROP.md` §7.4、`counterfactuals/CF2_DEFEROFF.md` §8.5）。

---

## 7. 已排除的假设（及排除依据）

| # | 竞争解释 | 排除依据 | 证据等级 |
|---|---|---|---|
| 1 | **io-wq CPU 增加是根因/收益源** | io-wq 内核 CPU +7.94 ns/op（+0.46% [−0.44,+1.36]）n.s.；io-wq 线程 syscr/syscw 12/12 恒 0；worker 完成投递两臂同构（xlayer 6004/6004）；D3/D16 的 io-wq 下降是伴随量（−6.4%/−17.2%），机制 UNOBSERVED，不作为归因主项 | 直接测量 |
| 2 | **`io_uring_enter` 参数/路径变化** | enter 100% `(1,0,0)` ret≡1；时长 B−A strace +67.7 ns [−154.1,+287.5]、shim +37.9 ns [−33.7,+110.4]，CI 含 0；`actual_enters` 两臂 1.0000/op（D2） | 直接测量（双仪器） |
| 3 | **batching 掩盖了 D2 的成本/收益** | D2 的 240,000 次 submit 全部 to_submit=1、ret=1、零部分提交；flush size 全 1；D3/D16 的 batching 才是**收益**（SQE/enter 1.448/5.066），不是 D2 的退化源 | 直接测量（strace/shim/trace） |
| 4 | **缺页 / 内存足迹** | page-faults +0.15% [−0.35,+0.69] n.s.；cache-misses +0.65% [−3.74,+5.11] n.s.；工作集 64 MiB warm tmpfs | 直接测量 |
| 5 | **设备 I/O（C6）** | warm tmpfs、`page_cache_resident`；无 block 请求；C6 不适用 | 直接测量（bench meta）+ 范围排除 |
| 6 | **调度等待构成 CPU 根因** | runnable +36.4 ns/op = on-CPU 增量 +474.8 的 7.7%；timeslices/sleep/park 推导 n.s.；off-CPU 不计 CPU；migrations 0 | 直接测量（schedstat ns） |
| 7 | **eventfd 写产生真实 sched_wakeup** | 写者=提交线程=唯一可能的 park 者；写时 waitqueue 为空；`progress_handoffs=0`；sleep 推导不变 | 源码论证 + 直接计数 |
| 8 | **用户态新增指令执行解释全部用户 CPU** | `instructions:u` 仅 +1.06%（+50/op）而用户 CPU +95.24；直接测得新增执行仅 ≈24–27 ns/op（25–28%）；其余是 C5 停顿 | 直接测量（PMU+TSC） |
| 9 | **频率/块间漂移解释了符号** | 所有定量对比都是块内 A/B 同二进制 ABBA 配对；A/A 门 PASS；CF-1/CF-2 的 **CPU** restore 精确重现 base，**延迟分位数**同向/同域（p50 差 2.4–5.7pp，见 §4.2）；块间绝对 CPU 漂移 2.67–4.52% 只影响绝对值 | 方法控制 + 直接测量 |
| 10 | **探针本身造成效应** | 计数器 ON/OFF 开销 ~1–2% CPU/op（vs 效应 3.7%）；ON 块校正后效应与 OFF 一致（4.241 vs 4.243；3.515 vs 3.518）；OFF 与冻结无差异（+0.79% CI 含 0） | 直接测量（同二进制 ON/OFF） |
| 11 | **门链准入判断是 CPU 成本** | CF-0 C−A −0.52% [−1.28,+0.22] / −0.13% [−1.45,+0.82]；C1 门增量 +6.6 ns/call（≈0.14%） | 反事实支持 + 直接测量 |
| 12 | **defer 分支本身/微架构 layout 承担残差** | CF-1 残量 CI 上界 +0.16% <2.5%；CF-2 残量 CI 上界 +0.06% <1%；G10 假设 (b)(c) 不触发 | 反事实支持 |
| 13 | **通知是延迟位移的主因（L0 主导）** | CF-1 去通知后 CPU 归零而 p50 +4.30/p99 +4.87 仍显著；CF-2 去 deferral 后延迟归零；但 CF-1 相对 frozen 的 p50/p99 点估计仅回落约 1pp 且 CI 重叠，**L0 残余未单独量化**——排除的是主导，不是零贡献 | 反事实支持 |
| 14 | **CPU 均值平移是延迟位移的原因** | 同 13：CPU 差 ≈0 时延迟仍显著；且延迟位移是 CPU 位移的 2.8 倍 | 反事实支持 |
| 15 | **p99 罕见调度/收割长尾是独立根因** | CF-2 使 p99 **总差**归零（−0.83%，保守 ≥96.2%）；该界不是 p99−p95 附加尾部（~+320 ns）的界，后者 CI 未计算；L2 的“形状机制”未分解，但“归属”在 defer 链内 | 反事实支持 + PARTIAL |
| 16 | **内核 `io_uring_defer`/task_add/local_work_run 参与** | xlayer 三轮 0 次行使；用户臂的 defer 是**用户态 transport flush defer**，与内核 io_uring_defer 不同层不同机制 | 直接测量（tracepoint 全量） |
| 17 | **用户态线程交接/跨 CPU 迁移** | 7 类用户事件 tid 全部=bench 主线程；worker 同 cpumask 同 cpu=2；migrations 0；progress_handoffs=0 | 直接测量（xlayer + schedstat） |
| 18 | **D1 的机制泄漏/arm 选择器偏差** | D1 `sqes_deferred=0`、`wakeups=0`、flush=enter=1.0/op；CPU/延迟中性 | 直接测量（精确过滤 `=W02-uring-4k-d1` 的 `*-d1x` 块） |

---

## 8. 未决假设与缺失观测

以下不是“待办清单”，而是本裁决**明确不宣称已解决**的部分。标签：UNOBSERVED =
本会话未观测；OBSERVABILITY_LIMIT_REACHED = 能力到头；CORRELATION_PARTIAL = 只有相关证据。

1. **CPU 精确份额的段级分解（C5 微架构，≈68–71 ns/op）**：CORRELATION_PARTIAL。
   `cycles:u` 超出 `instructions:u` 的部分（IPC −4.36%、cache-ref +23.31%、branch-miss
   +14.94%）指向新增 signal/eventfd 路径的停顿/取指/分支，但未做 PEBS/LBR 或 `:k` PMU
   的指令级归因；CF-1 后未重跑 PMU 验证这些事件回落（G7 停止条件未执行）。
2. **eventfd write 的内部阶段拆分**：UNOBSERVED。单次 562.7–592.5 ns/call 是 on-CPU
   往返；特权级切换、fd 查找/计数、waitqueue 操作的占比未测。`:k` PMU/fentry 在
   xlayer 会话 root 可用时具备能力，但 CF-1/CF-2 会话未能使用 root
   （`UNOBSERVED_ADMIN_UNAVAILABLE`，`counterfactuals/CF1_SIGDROP.md` §5.4、`counterfactuals/CF2_DEFEROFF.md` §6.5）。
3. **延迟段级分解与 L2 附加尾部（G12）**：UNOBSERVED / PARTIAL。没有 latency-pass 的
   逐请求分段直方图（dispatch→defer→flush→完成→reap→take × deferred），没有逐事件
   wakeup→run 分布；p99 比 p95 多出的 ~+320 ns 的构成未分解。CF-1 的预注册替代停止条件
   已满足（L0 主导排除），因此不阻塞源级裁决，但阻塞“段级机制一致”。
4. **D3/D16 的 io-wq 内核 CPU 下降机制**：UNOBSERVED（split 块 −6.4%/−17.2%，只记录为
   伴随收益；C3 在 D2 enter 不变时 io-wq 为 flat）。
5. **D3/D16 的 native 逐 SQE submit delay**：受限。只有 trace 构建（单仪器，扰动
   ~0.7 µs/op）的 defer→flush delay；native 绝对延迟未测（D2 有 C2 原生 shim 的
   29.93% fast-follow，D3/D16 无对应原生直测）。
6. **内核态 PMU / 内核指令级路径（G1）**：`perf_event_paranoid=2`，`:k` 事件不可用；
   内核 CPU 只有 `/proc` 10 ms tick 与 TSC on-CPU 区间两种间接量。xlayer 会话的 root
   恢复了 bpftrace tracepoint/fentry 能力（`CAPABILITY_MATRIX.md` §12），但本裁决
   没有 clean-run 的 `:k` PMU 数据；该缺口是“未运行”，不是“不可达”（在 root 可用时）。
7. **utime/stime 的 tick 分辨率**：OBSERVABILITY_LIMIT（10 ms tick）。CF-1 的
   “内核 stime 增量→≈0” 是 syscw 差→0 的间接支持，不是 stime 直接读数
   （`counterfactuals/CF1_SIGDROP.md` §5.2、§8）；CF-2 同款（`counterfactuals/CF2_DEFEROFF.md` §6.4、§9）。
8. **块间绝对 CPU 漂移与实测频率（G5/G6）**：phaseA vs counter/split 块绝对 CPU/op
   差 7–12%；`scaling_cur_freq` 是 schedutil **请求值**（1.2–1.55/3.5 GHz），不是实测
   Bzy_MHz；turbostat/msr 需 root，未在本轮 clean run 使用。所有结论只用块内配对差，
   绝对值不可跨块相减。
9. **timing 构建的字节可复现性**：CF-2 §6.1 记录，stash 本轮改动后于 CF-1 状态重建
   frozen timing bench = `40b8ed8a…` ≠ CF-0 存档 `0091d259…`（同大小、cmp 差 186,825
   字节）——**CF-0→CF-1 之间即不可字节复现**，非本轮引入；CF-2 mutant-A 绝对 CPU 比
   frozen-A 低 0.94–0.98%（两块方向一致），成因不可分（布局/漂移/少一次成员加载），
   G10 登记，**不进入配对结论**（所有对比均块内同二进制）。
10. **deferoff 的 held-batch 契约边界**：CF-2 mutant 按设计不满足 arm-B held-batch
    进度契约（595 oracle rc=134）。因此“defer 决策链承担全部效应”不能外推到存在
    held-batch 停滞的形状；D3/D16 的结论同样限定于无 held-batch 停滞的 W02-4k 形状。
11. **D3 的 p95/p99 延迟残差**：deferoff 后 D3 p95 **+1.56% [+0.16,+3.23]（显著）**、
    p99 **+4.60% [+0.83,+8.98]（显著）**，p50 +0.49% [−0.30,+1.42] n.s.；
    与 mutant-A 布局（约 −1%）或残余 poll 形态的区分未做。不影响 CPU 归因，但说明
    D3 的延迟改善在 p95/p99 上没有完全归零（`analysis/depth-analysis/DEPTH_MECHANISM.md:51` 与本文旧版的
    “p50/p95 n.s.” 低报了 p95，已更正）。
12. **R1 sanitizer lane / 全 mutant-receipts lane**：本轮未重跑（CF-0/CF-1/CF-2 同款
    标注）；本轮正确性面 = BASE GREEN + 595 oracle + 12/13 或 13/13 B-mode lane +
    字节 oracle。**depth 的 `bench_w02_pipeline_deferoff` 二进制没有重跑 595 oracle**，
    其正确性面只有 exit 0 + 字节 oracle + 400k measured op/inv。
13. **深度反事实缺 restore**：D3/D16 只有同一 `deferoff` 变异二进制的 8 对 ABBA
    （`raw/deferoff-d{3,16}/`，各 16 invocation），**没有恢复生产二进制并复现的块**；
    phaseA 的 −6.69%/−26.36% 是干预前、不同会话/二进制的测量，不构成 restore。
    这是 DEPTH_CAUSE 降为 PARTIAL 的直接原因。
14. **深度改善的机制分解**：`deferoff` 砍的是整条 defer 链，不是 enter 摊销单因素。
    直接测得的提交线程内核分量约 54.8%（D3）/50.3%（D16），io-wq 约 29.1%/21.0%
    机制 UNOBSERVED，用户约 16.8%/28.8% 只是与更少 submit 准备一致的间接解释；
    “全部改善由 enter 摊销承担”超出证据。

---

## 9. 观测能力与证据等级（直接测量 / 反事实支持 / 未知）

### 9.1 直接测量（本裁决的一等证据）

- 确定性计数器：`sqes_deferred`、`wakeups`、`wakeup_writes`、`wakeup_coalesced`、
  `flushes`、`actual_enters`、`submit_attempts`、`poll_passes`、`wait_attempts`、
  `progress_handoffs`（`counters/raw/cnt-on-full-d2/`、`analysis/depth-analysis/raw/cnt-d{3,16}-ab/`；
  裁决时从 `.err` 原始行重算，§附录 B）。
- in-situ TSC 单次成本：`signal` 277.5 ns/call、`write` 592.5、`defer` 10.2→16.8、
  `flush` 702.3→714.4（`counters/C1_COUNTERS.md` §3；`raw/c1-interleaved-d2/`）。
- syscall 面：syscw/wchar 内核记账 +86,912/inv、8.005 B/write；read/poll/drain −0.00718/op；
  io-wq 零 syscall；futex 2/进程（`counters/C3_COUNTERS.md` §3–§5；`raw/sysio-d2/`）。
- 线程 CPU：提交线程用户 +95.24、内核 +79.37、io-wq +7.94 ns/op（`counters/raw/split-d2/`；
  `counters/analysis/summary.json`）。
- 用户态 PMU（`u` only，100% enabled）：instructions +1.06%、cycles +5.71%、IPC −4.36%、
  cache-ref +23.31%、branch-miss +14.94%、cache-miss/page-fault n.s.
  （`raw/pmu-d2-ab/`、`raw/pmu-d2-aa/`；A/A 噪声见 §1.2）。
- 调度：schedstat on-CPU +474.8、runnable +36.4、timeslices +0.12% n.s.、migrations 0
  （`raw/schedstat-d2/`、`raw/schedstat-off-d2/`、`raw/schedstat-d2-br/`）。
- 逐事件结构：trace flush-size/ledger 占用/defer→flush delay（D2/D3/D16）；
  strace enter 参数直方图；C2 shim fast-follow 29.93% vs 0/120,000；
  xlayer 11 节点链 join 6000/6000、零丢失、完成由 io-wq worker 投递（两臂同构）。
- 延迟分布：phaseA 24 对 p50/p95/p99 的 pair 配对 bootstrap
  （`counters/analysis/latency-percentiles.md`；重算一致）。

### 9.2 反事实支持（本裁决的因果闭环）

- CF-0：门链准入判断本身 ≈0（C−A），B 的 D2 退化与 D16 收益全部由 defer 动作承担。
- CF-1：CPU 源 = residue signal/eventfd write；延迟源 ≠ 通知**主导**（L0 残余约 1pp 未单独量化）。
- CF-2：CPU 与延迟源 = defer 决策链（含 deferral）；第二干预点互证；延迟归零。
- CF-2 复用 D3/D16：深度改善在 defer 链内（干预消除），但**无 restore**，且机制分解
  （enter 摊销 vs io-wq/用户分量）未闭合 → DEPTH_CAUSE PARTIAL。

### 9.3 未知 / 观测边界

- 见 §8。核心未知：C5 指令级来源、eventfd 内部阶段、L2 段级机制、D3/D16 io-wq 机制、
  D3/D16 native submit delay、`:k` PMU、频率实测、D3 p99 残差、held-batch 形状外推。
- **是否触及当前内核观测能力极限**：
  - `perf_event_paranoid=2`、`unprivileged_bpf_disabled=2`、tracefs root-only 是**持续性**
    限制；非 root 下 `cycles:k`/`instructions:k` 实测 `No supported events found`
    （`CAPABILITY_MATRIX.md` §2、`OBSERVABILITY_LIMITS.md` §2.1–2.2）。
  - root bpftrace 曾在 xlayer 会话通过 sudo 口令管道恢复（`CAPABILITY_MATRIX.md` §12），
    但 CF-1/CF-2 会话不可用（`UNOBSERVED_ADMIN_UNAVAILABLE`）。因此“内核态逐事件/`:k`
    指令级”是**会话相关的未运行**，不是永久不可达；但本裁决没有这些数据。
  - `/proc/PID/delay` 不存在（per-task delay accounting 不可用）；`/proc/PID/latency`
    文件存在但内容更新未验证（`OBSERVABILITY_LIMITS.md` §1.3、§1.8）。
  - 10 ms tick 的 utime/stime、p50/p95/p99 的块间分辨率（CF-0 C−A 的 hw 2.3–5.6pp）、
    `scaling_cur_freq` 非实测频率：这些是**测量分辨率**边界，已在相应结论中降级为
    间接/上界，不用估计值冒充直接观测。
  - io-wq 执行段内部（qasync→complete）无中间事件：**能力边界**（root tracepoint 未运行），
    但两臂同构行使，不影响 A/B 归因（CORRELATION_PARTIAL，`traces/xlayer/README.md` §6.2）。

---

## 10. 复现与 provenance

### 10.1 源码 / 二进制 / 环境

| 项 | 值 | 证据 |
|---|---|---|
| 基线 commit | `271b81bfc9d64eecc0266fe7066b9040525dd710`（主工作树 clean，裁决时 `git status` 空） | `PROTOCOL.md`；`counterfactuals/raw/meta-cf2/*`；本轮 `git log -1` |
| 研究副本 | `causal-r2/worktree`（branch `research-595-r2-causal`）、`worktree-timing`（`research-595-r2-timing`）、`worktree-trace` | `PROTOCOL.md`；`counterfactuals/CF1_SIGDROP.md` §3；`counterfactuals/CF2_DEFEROFF.md` §6.1 |
| 无仪器冻结二进制 | `61c6d579…`（phaseA/timing）；`0091d259…`（CF-0/1/2 timing frozen）；变异体 `4de23299…`（sigdrop）、`64189aa8…`（deferoff） | `raw/phaseA/binary-sha256.txt`；`counterfactuals/raw/meta*/binary-sha256.txt`；`analysis/depth-analysis/DEPTH_MECHANISM.md` §9 |
| 计数器/PMU/trace 二进制 | `b9a49946…`（counters）、`6670d8de…`（PMU/split）、`2f47d5c1…`（trace）、`75534641…`（C1）、`17463539…`（C3） | `counters/cost_table.md` §1；`C1_COUNTERS.md` §7；`C3_COUNTERS.md` §1；`analysis/depth-analysis/DEPTH_MECHANISM.md` §9 |
| 工具链/内核 | g++ 16.2.1、xmake 3.1.2+HEAD、liburing-devel 2.13-2、Python 3.14.8、kernel 7.2.11-200.fc44.x86_64 | `counterfactuals/raw/meta-cf2/toolchain-env.txt`；`PROTOCOL.md` |
| 机器/亲和/频率 | E5 Xeon E5-2666 v3（10C/20T），bench `taskset -c 2`，sampler/perf cpu3/cpu5；schedutil 请求频率 1.2–1.55/3.5 GHz；无 root turbostat | `counters/cost_table.md` §1；`C4_SCHED.md` §1；`counterfactuals/CF1_SIGDROP.md` §3 |
| 运行环境 | 串行独占；loadavg 峰值：phaseA 1.94、CF-1 base 1.56/其余 ≤1.30、CF-2 ≤1.39、depth 0.26–1.35（20 核） | 各 `results.jsonl` 的 `loadavg_before/after`；各文档 §6/§7 |
| 样本量/顺序 | phaseA 五轮各 24 对（48 invocation/轮，100k op/inv）；CF-1/CF-2 各四块 12 对（96 invocation/块组）；depth D3/D16 计数器 12 对、deferoff 8 对、trace 3 phase×20k dispatch | `PROTOCOL.md`；`counterfactuals/CF1_SIGDROP.md` §6；`counterfactuals/CF2_DEFEROFF.md` §7；`analysis/depth-analysis/DEPTH_MECHANISM.md` §9 |
| 清单 | `counterfactuals/three_arm/MANIFEST.sha256`（736/736 OK）、`counterfactuals/MANIFEST-CF2.sha256`（419/419 OK） | 本轮 `sha256sum -c`（§附录 B） |

### 10.2 关键运行命令（协议原文）

- phaseA：`pair_runner.py`，`taskset -c 2`，`WARMUP=1 MEASURED=5`，`SLUICE_BENCH_STATS=1`，
  `SLUICE_BENCH_CASE_FILTER=<exact>`，tmpfs warm fixture（`PROTOCOL.md`）。
- CF-1/CF-2：同一 `pair_runner.py` 协议，`SLUICE_595_ARM={A,B}`，块序
  base→mutant→mutant rep→restore，块间 ≥45 s（`counterfactuals/CF1_SIGDROP.md` §6、`counterfactuals/CF2_DEFEROFF.md` §5）。
- depth：`run_blocks.py --pairs 12 --arms AB --case '=W02-uring-4k-d3' --counters 1`；
  trace/strace/PMU/split/deferoff 命令见 `analysis/depth-analysis/DEPTH_MECHANISM.md` §9。
- 仪器分离：计数器/trace/strace/PMU/split/deferoff 各自独立块，**未叠加进同一次
  timing run**；性能读数全部取无仪器 frozen 协议臂（`counterfactuals/CF1_SIGDROP.md` §6、`counterfactuals/CF2_DEFEROFF.md` §5）。

### 10.3 本裁决的独立复核（撰写时实际执行，非重跑 benchmark）

以下复核全部只读 archived raw data；**没有新跑任何 benchmark**。

| 复核 | 命令 | 结果 |
|---|---|---|
| CF-1/CF-2 CPU 重算 | `python3 -I /tmp/verify595/recompute.py <evidence>`（按 pair 聚合 `metrics.cpu_ns_per_op`，bootstrap 10k/seed 5950） | CF-1 merged frozen +3.342% [+2.631,+4.059] / sigdrop −0.259% [−0.666,+0.161]；CF-2 merged frozen +3.897% [+2.909,+4.908] / deferoff −0.757% [−1.608,+0.055]——与 `counterfactuals/CF1_SIGDROP.md` §6/§7、`counterfactuals/CF2_DEFEROFF.md` §7.1 一致 |
| depth phaseA 重算 | `/tmp/verify595/recompute2.py` | D2 +3.725% [+3.036,+4.423]、D3 −6.687% [−7.253,−6.176]、D16 −26.358% [−26.661,−26.085]——与 `analysis/depth-analysis/DEPTH_MECHANISM.md` §2 一致 |
| D3/D16 deferoff 重算 | `/tmp/verify595/recompute3.py`（逐 invocation jsonl 聚合） | D3 +1.066% [−0.475,+2.804]（8 对）、D16 +0.259% [−0.688,+1.041]——与 `analysis/depth-analysis/DEPTH_MECHANISM.md` §0/§4 一致 |
| CF-1/CF-2 延迟重算 | `/tmp/verify595/recompute4.py` | CF-1 frozen +5.38/+4.73/+5.85%，sigdrop +4.30/+4.28/+4.87%；CF-2 frozen +6.26/+5.19/+6.03%，deferoff −0.71/−0.87/−0.83%——与文档一致 |
| depth 延迟重算 | `/tmp/verify595/recompute5.py` | D2 +6.91/+4.96/+7.07%、D3 −3.07/−11.38/−9.57%、D16 −32.31/−31.00/−28.68%；deferoff D3 +0.49/+1.56/+4.60%、D16 +0.37/+0.81/+0.61%——与文档一致 |
| trace 重放 | `python3 -I tools/parse_trace.py raw/trace-d{2,3,16}/B/trace.bin` | D2 flush `{1:20000}`、defer 10614、delay p50/p90/p99 617/1234/2777 ns；D3 `{1:7128,2:6436}`、706/3210/5419；D16 `{1:1343,7:1166,8:1311}`（另 2 个 3/4 边界 flush）、8363/14462/18919——与 `analysis/depth-analysis/DEPTH_MECHANISM.md` §2 一致（D16 的 2/3822 个异常小 flush 已在此登记） |
| 计数器原始行重算 | 从 `cnt-on-full-d2/*B.err` 取 `SLUICE595_COUNTERS` 行 | B：sqes_deferred 0.5589、flushes 1.0、actual_enters 1.0、wakeups 0.5589、writes 0.2353、submit_attempts 1.2354、poll_passes 0.7942——与 `cost_table.md` §3.2 一致 |
| 有效性门 | 逐块检查 `exit==0` 且 `metrics.ops==100000` | CF-1 四块各 24 inv（96 total）全 exit 0/100k；CF-2 四块各 24 inv 全 exit 0/100k；phaseA D2/D3/D16 各 48 inv 全 exit 0/100k |
| 清单 | `sha256sum -c MANIFEST-CF2.sha256`、`sha256sum -c MANIFEST.sha256` | 419/419 OK、736/736 OK |
| 主工作树 | `git status --porcelain` | 空；HEAD `271b81bf` |

复核脚本保存在 `/tmp/verify595/`（一次性裁决复核，按研究纪律不入 evidence；方法为
对 archived `results.jsonl` 的 pair 级相对差 + 同参 bootstrap，未修改任何原始数据）。
另：`analysis/depth-analysis/DEPTH_MECHANISM.md` §2 表的 D16 trace flush-size 直方图未列出 2 个相位边界小 flush
（size 3/4 各 1 个，phase 1，合计 2/3822）；不改变 `SQE/enter=5.066` 的计数器口径与
strace `{8:7494,7:7494,1:7590}` 的独立复核，但在此如实登记。

**未运行 / 不可验证项（如实）**：本次复核与本裁决撰写**没有重跑任何 benchmark**；
**没有 root 权限验证 `:k` PMU**（当前 `perf_event_paranoid=2`，会话无 sudo 口令）；
**没有在 depth timing 二进制（`64189aa8…`）上重跑 595 oracle**；
**没有重算 CF-1 后 PMU 事件回落**（G7 停止条件未执行）。这些是**未验证/未运行**，
不是已通过。复核意见记录中另行核验通过的项目（CF-1/CF-2/phaseA/depth deferoff 的
pair 级 CPU 与延迟重算、depth trace 重放、xlayer join、CF-2 shim 数组核对、CF-0 trio、
CF-1/CF-2 sysio、`counterfactuals/three_arm`/CF2/R1 的 `sha256sum -c`、主工作树 clean）属于**复核意见的
核验记录**，本裁决不把它们冒充为撰写者新跑的 benchmark。

---

## 11. 裁决边界与明确不做的声明

1. **不修复**：本裁决只回答“成本/延迟由什么造成”，不提出、不实现任何优化。D3/D16 的
   摊销收益与 D2 的代价同源；是否改变提交策略（如 H-DEPTH-1/2）属于后续决策
   （`analysis/depth-analysis/DEPTH_MECHANISM.md` §6.2 只把它列为可检验假设，未实现、未评估）。
2. **不改公开 API / v2 契约**：所有源码改动只在 `causal-r2/worktree*` 研究副本；
   主工作树 `/home/jnhu/Source/Sluice` 裁决时 clean；R1 evidence 只读未触碰。
3. **不外推**：
   - 只覆盖 `W02-uring-4k` warm-tmpfs 4 KiB read 形状（D1/D2/D3/D16）；不覆盖真实块设备/
     cold cache/O_DIRECT/其他 block size/其他 workload（C6 不适用；`EVIDENCE_GAPS.md` G11）。
   - deferoff 的结论限定于无 held-batch 停滞的形状（595 oracle 设计内拒绝）；held-batch
     停滞、取消控制、poison 路径下的成本结构未测。
   - 不从 tmpfs 外推到 btrfs/ext4/设备；xlayer 的 E3（btrfs dirty-fsync 64 op）只是
     独立场景的次序验证，不进入 D2 归因。
4. **不宣称“完整解释”**：
   - CPU：源级 ESTABLISHED；C5 的指令级来源、eventfd 内部阶段、残差 27–35 ns/op 是
     CORRELATION_PARTIAL/UNOBSERVED；82% 是上限口径，不是精确分解。
   - 延迟：源级 ESTABLISHED（L1 主导）；L0 残余约 1pp 未单独量化；L2 段级机制 PARTIAL
     （G12 未运行）。
   - D3/D16：**PARTIAL**——改善在 defer 链内（干预消除），但**无恢复复验**、机制分解
     （enter 摊销 vs io-wq/用户分量）未闭合；D3 p95/p99 残差、io-wq 机制、native submit
     delay 已登记。
5. **不用双仪器 run 做性能归因**：xlayer 的 E1/E2/E3（用户环 + root bpftrace 同开）只用于
   结构/次序/关联验证（`traces/xlayer/README.md` 头部与 §6）；所有性能数字取无仪器
   frozen 协议臂或单仪器/同二进制配对块。

---

## 附录 A：关键证据路径（按用途）

- 成本/CPU：`counters/cost_table.md`、`counters/C1_COUNTERS.md`、`counters/C3_COUNTERS.md`、
  `counters/C4_SCHED.md`、`counters/analysis/summary.json`、`counters/raw/{pmu-d2-ab,pmu-d2-aa,
  split-d2,cnt-on-full-d2,cnt-off-full-d2,frozen-full-d2,pmu-d1x-ab,split-d1x,cnt-on-full-d1x,
  cnt-off-full-d1x,cnt-on-pmu-d2,cnt-off-pmu-d2}/`、`counters/bin/bench_w02_pipeline_counters`、
  `worktree/include/sluice/async/detail/research_595_counters.hpp`、`counters/raw/DISCARDED.md`。
- 延迟：`counters/analysis/latency-percentiles.md`、`raw/phaseA/ab-d2-w02-4k/results.jsonl`、
  `counterfactuals/CF1_SIGDROP.md`、`counterfactuals/CF2_DEFEROFF.md`。
- 提交路径：`traces/c2-syscall/README.md`、`counters/C2_PROBE_OVERHEAD.md`、`traces/raw-c2/`。
- 跨层：`traces/xlayer/README.md`、`traces/xlayer/{e1-armA-immediate,e2-armB-defer,e3-fsync-iowq}/`、
  `traces/xlayer/{tool_bpftrace_kernel.bt,tool_run_experiment.py,tool_join_analysis.py,
  tool_research_595_trace.hpp}`、`CAPABILITY_MATRIX.md` §12。
- 反事实：`CF0_THREE_ARM.md`、`counterfactuals/CF1_SIGDROP.md`、`counterfactuals/CF2_DEFEROFF.md`、
  `counterfactuals/raw/{ab-d2-base,ab-d2-sigdrop,ab-d2-sigdroprep,ab-d2-restore,ab-d2-base-cf2,
  ab-d2-deferoff,ab-d2-deferoffrep,ab-d2-restore-cf2}/results.jsonl`、
  `counterfactuals/raw/cnt-companion*/`、`counterfactuals/raw/trace-companion*/`、
  `counterfactuals/raw/sysio-*/`、`counterfactuals/raw/shim-companion-cf2/`、
  `counterfactuals/raw/correctness/`、`counterfactuals/raw/meta*/`。
- 深度：`analysis/depth-analysis/DEPTH_MECHANISM.md`、`analysis/depth-analysis/depth_ledger_summary.json`、
  `analysis/depth-analysis/raw/{cnt-d3-ab,cnt-d16-ab,trace-d2,trace-d3,trace-d16,strace-d3,strace-d16,
  enterargs-d3,enterargs-d16,pmu-d3-ab,pmu-d16-ab,split-d3,split-d16,deferoff-d3,deferoff-d16}/`、
  `analysis/depth-analysis/tools/{parse_trace.py,aggregate_depth.py,run_depth_ledger.sh,run_syscalls.sh,
  run_pmu.sh,run_split.sh,run_deferoff.sh}`、`analysis/depth-analysis/RAW_MANIFEST.sha256`。
- 协议/缺口：`PROTOCOL.md`、`EVIDENCE_GAPS.md`、`OBSERVABILITY_LIMITS.md`、
  `CAPABILITY_MATRIX.md`、`CAUSAL_DAG.md`、`COST_CLASSIFICATION.md`。

## 附录 B：本裁决的复核输出（原文摘录）

```
# CF-1/CF-2 CPU（pair 相对差，bootstrap 10k / seed 5950）
CF1 base: n=12 mean=+3.341% CI=[+2.305,+4.346]
CF1 sigdrop: n=12 mean=-0.579% CI=[-1.113,-0.095]
CF1 sigdroprep: n=12 mean=+0.061% CI=[-0.537,+0.694]
CF1 restore: n=12 mean=+3.343% CI=[+2.395,+4.380]
CF1 merged frozen: n=24 mean=+3.342% CI=[+2.631,+4.059]
CF1 merged sigdrop: n=24 mean=-0.259% CI=[-0.666,+0.161]
CF2 base: n=12 mean=+3.857% CI=[+2.379,+5.395]
CF2 deferoff: n=12 mean=-0.471% CI=[-1.626,+0.672]
CF2 deferoffrep: n=12 mean=-1.043% CI=[-2.265,+0.092]
CF2 restore: n=12 mean=+3.936% CI=[+2.603,+5.264]
CF2 merged frozen: n=24 mean=+3.897% CI=[+2.909,+4.908]
CF2 merged deferoff: n=24 mean=-0.757% CI=[-1.608,+0.055]

# depth phaseA
D2 phaseA: n=24 cpu mean=+3.725% CI=[+3.036,+4.423]
D3 phaseA: n=24 cpu mean=-6.687% CI=[-7.253,-6.176]
D16 phaseA: n=24 cpu mean=-26.358% CI=[-26.661,-26.085]
deferoff-d3: n_pairs=8 mean=+1.066% CI=[-0.475,+2.804]
deferoff-d16: n_pairs=8 mean=+0.259% CI=[-0.688,+1.041]

# 延迟（CF-1/CF-2 合并 24 对）
CF1 frozen: p50 +5.38% [+2.97,+8.51] | p95 +4.73% [+3.97,+5.48] | p99 +5.85% [+4.26,+7.93]
CF1 sigdrop: p50 +4.30% [+2.64,+6.66] | p95 +4.28% [+3.61,+4.91] | p99 +4.87% [+3.99,+5.74]
CF2 frozen: p50 +6.26% [+4.77,+7.92] | p95 +5.19% [+3.72,+6.63] | p99 +6.03% [+2.99,+9.02]
CF2 deferoff: p50 -0.71% [-2.57,+1.90] | p95 -0.87% [-1.71,-0.10] | p99 -0.83% [-2.12,+0.23]

# depth 延迟
D2: p50 +6.91% [+5.03,+9.23] | p95 +4.96% [+4.21,+5.75] | p99 +7.07% [+4.19,+11.80]
D3: p50 -3.07% [-3.51,-2.63] | p95 -11.38% [-11.94,-10.85] | p99 -9.57% [-11.48,-7.68]
D16: p50 -32.31% [-32.58,-32.03] | p95 -31.00% [-31.61,-30.37] | p99 -28.68% [-29.66,-27.48]
deferoff-d3: p50 +0.49% [-0.30,+1.42] | p95 +1.56% [+0.16,+3.23] | p99 +4.60% [+0.83,+8.98]
deferoff-d16: p50 +0.37% [-0.95,+1.49] | p95 +0.81% [-0.76,+2.43] | p99 +0.61% [-1.15,+2.28]

# trace 重放（D2/D3/D16，B 臂，phase 1）
D2: flush_size_hist {1: 20000}; defer_after_flush {0: 9386, 1: 10614};
    delay p50/p90/p99 = 617/1234/2777 ns (n=10614)
D3: flush_size_hist {1: 7128, 2: 6436}; defer_after_flush {0: 6439, 2: 6436, 1: 689};
    delay p50/p90/p99 = 706/3210/5419 ns (n=13561)
D16: flush_size_hist {1: 1343, 7: 1166, 8: 1311, 3: 1, 4: 1};
    defer_after_flush {0: 1183, 7: 2477, 1: 160, 3: 1, 4: 1};
    delay p50/p90/p99 = 8363/14462/18919 ns (n=17506)

# 清单与工作树
MANIFEST-CF2.sha256: 419/419 OK
MANIFEST.sha256:     736/736 OK
主工作树: git status --porcelain 空；HEAD 271b81bfc9d64eecc0266fe7066b9040525dd710
```

---

## 附录 C：一句话裁决

**D2 的 CPU 退化（+3.70%）是 B 的 defer 决策触发 residue 通知（signal → eventfd write）
的代价（源级 ESTABLISHED；82% 是上限口径）；D2 的延迟分布整体抬升（p50/p95/p99）是
deferral 把 SQE 的 flush 推迟到下一次 poll 的代价（L1 主导；L0 主导排除、残余约 1pp
未量化），而不是 CPU 均值平移；两者都在单因素干预下消失，撤销复验在 CPU 上精确、
在延迟分位数上同向/同域。D3/D16 的改善在 defer 链内（干预消除），但缺恢复复验、
机制分解未闭合，故为 PARTIAL。**
