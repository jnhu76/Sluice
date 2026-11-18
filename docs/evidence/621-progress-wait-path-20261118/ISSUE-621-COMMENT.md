# #621 Progress/Wait 路径战役 — 终稿回写（A–K 精简版）

**基线**：`5d94f233967c3aef993eb19ba9e3252c532b0951`（HEAD = origin/master，单 worktree，tracked tree 干净）
**日期**：2026-11-18
**完整报告**：`FINAL-REPORT.md`（随本评论一并提交/归档于同一路径；本评论只保留结论、数字与 owner gate，不替代报告）
**判词**：**`ADMIT_TO_AB = 0`；无 GO；无产品 PR；无新 A/B。** 建议 `NO_FURTHER_OPTIMIZATION_JUSTIFIED`（当前没有可进入 A/B 的候选；W3/M-53 设计门仍开放，见末尾）。

---

## 1. 一句话结论

本战役查清了 Progress/Wait 路径的机器工作在哪里、候选为什么不能进 A/B、还缺什么，但**没有产生任何新的产品收益或 A/B 收益**。真正减少机器工作的是 BASE 之前已合并的 `#634` 和 BASE 本身的 `#635`；本轮 0 个新候选达到 A/B 准入。W1 的 material 方向（blocking read / 第二等待 fd）是**语义阻断**，不是“还没测”；唯一契约允许的 material 替代（C2 condvar 双通道）需要 owner 授权。W3 有候选级 owner-total 点估计（`offer_terminal` 1.80%、`claim_execution` 1.56% 等），但 P4R 判定其为方向性证据、单候选可删量仍 `NOT_OBSERVABLE`；M-53 的 O1/O2 已证、O3 未在 HEAD 实现；**若** d64 效应接近 d16 的 0.4% 量级（乐观代理假设），172/336 对不可行，P4 的 1.06%/1.13% 点估计不显著、尚未证明大于 0.4%；M-42 低于噪声；`#637` 的 `NO_GO`/`NO_GO_ABORT`/`SEMANTIC_MISMATCH` 终态未被翻案（M-16/M-42 的分类性调整为本轮裁决）。

---

## 2. 做了什么（范围与证据等级）

| 项 | 等级 | 结果 |
|---|---|---|
| 环境门禁 | R | 频率门禁 PASS（单核 ≈3.48 GHz、4 核 ≈3.20 GHz，无掉频/节流）；PMU/eBPF/介质可用；paranoid=2；BTF 7,023,892 B；临时目录为 tmpfs；根 btrfs `compress=zstd:1` |
| 成本地图（profile） | R + V | 5 个形态：W-02 uring d16/d1、W-03 d16 split-owner、W-02 ThreadPool d16 w4、`sluice-copy` 4k/d4/w1；PMU 198/198 行无缩放，record Lost 15/15=0（本会话复算 P2 R1 关键数） |
| A/A 校准 | R + V | 11 格（P1 6 + P2 5），352 arm，**9 格有效 / 2 格无效（C2/C5 filter 未命中）**，**144 个有效配对 / 288 个有效 arm**（本会话复算 P1/P2 A/A） |
| A/B | U | **0 个**；没有候选 build、候选 SHA、预注册 A/B 协议 |
| 语义审计 | R | W1 的 O1–O10、D1/D2/D3/C1/C2/A1–A8/B/C3 逐项裁决；M-53 O1/O2/O3；W3 的 `publication_inflight`/slot reuse/observer 门（本轮未重跑 oracle/mutant/TLC） |
| 行级归因 | R | P4 在 TP d16 w4 / W-03 d16 / copy / uring d64 上做 DWARF 行级 profile；结果只作方向性证据（P4R 限制了定量结论） |
| 独立复算 | V | 本会话重跑 P1/P2 A/A 与 P2 R1 复算；另用自写 t 分位实现复核 required-n；M-53 三个既有测试 7/7、20/20、21/21，均 exit 0 |

**本轮未运行**：`xmake` 构建/测试、完整矩阵（debug/noring/ASAN/TSAN）、TLC、任何新的 perf/bench/A-A/A-B、语义 oracle/mutant 重跑。**没有把“未运行”写成“通过”。**

---

## 3. 关键数字（带口径）

### 3.1 成本地图（P2 R1；份额 ≠ 可删 cycles；阶段容器 ≠ 候选上界）

| 形态 | cycles/op | 内核占比 | `progress/wait` | `poll boundary` | publication |
|---|---:|---:|---:|---:|---:|
| P-A W-02 uring d16 | 15,090.0 | 82.3% | 5.27% | — | 1.46% |
| P-B W-02 uring d1 | 28,703.2 | 81.7% | **25.09%** | **9.16%** | 0.07% |
| P-C W-03 d16 | 20,324.9 | 81.8% | 16.29% | — | 2.82% |
| P-D W-02 TP d16 w4 | 20,386.1 | 71.9% | 3.51% | 0.06% | 4.13% |
| P-E sluice-copy /chunk | 44,481.5 | 75.6% | **24.36%** | **4.38%** | 3.30% |

- **W1**：P-B `__poll` inclusive **24.93%**（6,129/24,585）、`__x64_sys_poll` 15.77% 是其内核子集、poll boundary 9.16%；P-E `__poll` **15.83%**（5,500/34,736）、x64 11.46%、poll boundary 4.38%。**`__poll` inclusive 15.83% 是 poll 相关份额；x64 11.46% 是它的内核子集；两者都不是可删量或上界。** `progress/wait` 是混合容器（P-B 含 2,057 个 worker-eventfd；P-E 含 3,537 个 progress-pass-only）。
- **W3**：族级 self P-A 6.00% / P-B 0.24% / P-C 15.82% / P-D 22.43% / P-E 13.07%，是阶段容器；P4 另有候选级 owner-total 点估计（`offer_terminal` 1.80% 等），但 P4R 判为方向性证据、单候选可删量 `NOT_OBSERVABLE`；P-B 因 90.67% app 样本无 bench 帧而 `NOT_OBSERVABLE`；M-62 的 4.13% 是 publication 容器，不是候选上界；M-61 有 r6 R3 0.45% < 1% MME 的反例。
- **M-53**：d16 leaf self P-A 0.406%（34/8,369）、P-C 0.285%（47/16,472），≈61.3/58.0 cyc/op；d64 P4 点估计 self 1.06% / owner 1.13% app（168/179 样本），但 d64 vs d16 不显著（self p=0.22、owner p=0.30）；app owner 1.13% **低于 MDE80 1.416%、高于 t-hw 1.0066%**，all-comm period owner 0.63% 才低于两者；因此 16 对 A/A 在保守 t 判据下仍是边缘/差一对。
- **M-42**：`submit_flushes_.fetch_add` 编译为 `lock addq $0x1,0x120(%rdi)`；按 19–20 cycles 为 P-A 0.126–0.133%、P-B 0.066–0.070%、P-C 0.093–0.098% cycles；1 instr/submit 0.0041–0.0094%；TP 形态不执行 → `NOT_POWERED`。

### 3.2 A/A 噪声带（n=16；hw 是 bootstrap 95% CI 半宽；MDE80 是 80% 功效最小效应）

| 格 | 判词 | CPU SD/hw/MDE% | cycles SD/hw/MDE% | 可用性 |
|---|---|---|---|---|
| C1 copy 4k/d4/w1 | **QUIET** | 0.786 / 0.365 / 0.589 | 2.346 / 1.121 / 1.758 | P-E 匹配格 |
| C3 uring d16 | **ENVIRONMENT_BLOCKED** | 7.027 / 3.312 / 5.266 | 1.991 / 0.949 / 1.492 | CPU/wall 不可用；cycles 未认证 |
| C4 W-03 d16 | **QUALIFIED** | 3.019 / 1.402 / 2.262 | 8.123 / 3.843 / 6.088 | P-C 匹配格；只支持 ≥~2.3% CPU |
| C6 uring d1 | **ORDER_SENSITIVE**（wall） | 0.997 / 0.471 / 0.747 | 2.876 / 1.368 / 2.155 | P-B 匹配格；wall 第二臂 +1.132 pp（13/16，p=0.0049） |
| C2b TP d16 w4 | **NOISY** | 3.414 / 1.604 / 2.558 | 5.144 / 2.401 / 3.855 | P-D 匹配格 = 无可用带 |
| C3b uring d16 | **ENVIRONMENT_BLOCKED** | 5.526 / 2.597 / 4.141 | 1.319 / 0.621 / 0.988 | cycles 条件带，未认证 |
| C5b TP d1 w1 | **QUIET（条件）** | 0.964 / 0.458 / 0.723 | 3.107 / 1.481 / 2.328 | 背景负载警告；cycles 不作窄带；不是 W3 匹配格 |
| C7 uring d64 | **ENVIRONMENT_BLOCKED** | 9.782 / 4.573 / 7.331 | 1.889 / 0.893 / 1.416（t-hw 1.0066） | CPU/wall 均值不正；cycles 条件带；M-53 唯一可能指标 |
| C8 TP d64 w4 | **NOISY** | 4.938 / 2.323 / 3.700 | 6.804 / 3.259 / 5.099 | 不能替代 C7 |

**关键更正**：C3/C3b/C7 的 `ENVIRONMENT_BLOCKED` 是预注册判据（CPU CV > 2× cycles CV 且 instr CV <0.5%）的机械触发；P2 直接频率序列稳定在 3.43–3.44 GHz，**没有复现“持续频率 excursion”**；主导成分是 sys time（C3b sys CV 5.97%、C7 sys CV 10.50%，配对 ΔCPU/Δsys 相关 0.983/0.992）。正确表述是“CPU/wall 不稳定，频率 excursion 未被直接观测复现”，不是“已证实 5–7% 频率下降”。

---

## 4. 准入矩阵（终态）

### 4.1 W1–W6

| 包 | 阶段 | 终判 | 说明 |
|---|---|---|---|
| **W1** | owner 点开放 | `SEMANTIC_MISMATCH`（D1/D2/D3/C1）/ `NEEDS_OWNER`（C2）/ `NO_CHANGE_JUSTIFIED`（A1/A2/A3/B/A4'） | D/C1 语义阻断；C2 契约允许 condvar 但需 owner 授权第二通道；A1/A2 不 material；`#604` G0 未过；等 O-1/O-2/O-3 |
| **W2** | 终判 | `NOT_APPLICABLE` | `#635` 已合并；无新增候选；M-67 路由到 W3 |
| **W3** | 语义验证中 | `ADMIT_TO_DESIGN`（成员 `INCONCLUSIVE + design`） | 有候选级 owner-total 点估计（0.3–1.8%），但 P4R 判为方向性证据；单候选可删量 `NOT_OBSERVABLE`；P-D 的 C2b NOISY；M-61 R3 反例未推翻；预算 M |
| **W4** | 语义验证中 | `ADMIT_TO_DESIGN` | M-53 先证 O3 + d64 cycles-only A/A；M-40/M-63/M-65 设计门；预算 M/L |
| **W5** | 终判 | `NOT_APPLICABLE` | `#634` 已合并；parity 8 + 4 mutants 是回归义务 |
| **W6** | 终判 | M-42 `NOT_POWERED`；M-28 `NO_GO_ABORT`（不重做）；M-40 转 W4 | `GO` 只是排队；隔离格/动态回执带/机制门/mutant/release 无 seam 前不启动；M-28 不是 `NOT_APPLICABLE` |

### 4.2 `#637` 全量（22 候选 + 3 非候选）

**`NO_GO_ABORT`**：M-28（`syscall_count_` 仍在生产；四要素才重开）。
**`NO_GO`**：M-61 payload；H1；io-wq punt；tp-w8。
**`SEMANTIC_MISMATCH`**：M-09、M-30、M-57、M-12、M-16、M-80 hygiene（先 owner 裁决，不做性能实验）。
**`NOT_POWERED`**：M-42（原 `GO` 只是排队）。
**`INCONCLUSIVE`**：M-61 begin/complete（r6 R3 0.45% <1% MME；需推翻 R3 + 确定性注入 + 族 A/A 先行）；M-80 write_all_at；M-37（`#595`）；M-01（owner S2）。
**`INCONCLUSIVE` + design work**：M-62、M-65（W4 owed）、M-67、M-10、M-33、M-74、M-40、M-63。
**`ADMIT_TO_DESIGN`**：M-53（O1/O2 PROVEN；O3 INCONCLUSIVE）。
**`GO`（排队，未启动）**：M-42（`#637` 原终态）；本战役记 `NOT_POWERED`。

**计数（38 行）**：`ADMIT_TO_AB 0`；`ADMIT_TO_DESIGN 11`；`SEMANTIC_MISMATCH 11`；`NEEDS_OWNER 1`；`NO_CHANGE_JUSTIFIED 2`；`NOT_APPLICABLE 2`；`NOT_POWERED 2`；`INCONCLUSIVE 4`；`NO_GO 4`；`NO_GO_ABORT 1`。

---

## 5. 哪些真正减少了机器工作？

**本战役新增减量 = 0。** 只有两个已合并历史事实：

| 工作 | 减量（历史回执） | 口径/限制 |
|---|---|---|
| `#634` ThreadPool owed-count gate | `sluice-copy` 256 MiB、4 KiB/d4/1 worker、btrfs warm、24 对：CPU/task **−5.58%** [−6.45,−4.71]、wall **−6.53%** [−7.55,−5.53]、user instr **−22.1%** | `PARTIAL_CPU_ACCOUNTING`；负控制 64 KiB/d1 +0.10% [−1.56,+1.77]；**未预注册非劣效界值**，CI 上界不能排除小幅 CPU 回归 |
| `#635` ProgressSource 单读 drain | 36 对：instructions **−5.0180%** [−5.7603,−4.2987]、cycles **−5.1111%**、task-clock **−5.4653%**、rusage CPU **−5.3199%**；A/A +0.7713%、宽度 2.4090 | 只 `progress_source.hpp` +47/−3；证据目录不在 master，只在未合并归档 `0e50612c`；TP 证据不外推到 io_uring |

`#609` ELIDE 是 `E5_MECHANISM_VERIFIED / END_TO_END_WIN_NOT_DEMONSTRATED`（四个 native W02 格在噪声内），也不是本轮新增。**不能把 `#634`/`#635` 的数字写成“本轮成果”。**

---

## 6. 哪些不能优化？

| 类别 | 项 | 原因 |
|---|---|---|
| 当前契约下语义不能动 | W1-D1/D2/D3、C1、C3、A4（全局）、A5–A8 | `dup` 共享文件状态标志（`after clear on dup: original False dup False`）；同 fd 改 blocking 破坏非阻塞 ack/drain、有界 timeout、PROG-02 第 2 条；io_uring 只注册一个 eventfd；第二 fd/relay 产生第二通知权威；删重验/drain/P3/后检查丢唤醒或破坏 control sticky |
| 契约允许但需 owner | W1-C2（TP condvar + protected predicate） | PROG-02 允许 condvar，但需要第二通知通道/新模型/新 oracle；`NEEDS_OWNER` |
| 没有候选级可删量 | W3 M-61/M-62/M-67/M-10；W4 M-65（owed） | W3 有候选级 owner-total 点估计（约 0.3–1.8%），但单候选可删量 `NOT_OBSERVABLE`；publication 4.13% 是容器；M-61 R3 0.45% <1% MME |
| 低于噪声/不可测 | M-42 | `lock addq` 19–20 cycles → ≤0.14% cycles；1 instr 0.0041–0.0094%；W-02 16 对 MDE 远大于信号；需隔离格 |
| 已否决/不重做 | M-28、M-61 payload、H1、io-wq punt、tp-w8 | `NO_GO_ABORT`/`NO_GO`；无新证据不复活 |
| owner 裁决面 | M-09/M-30/M-57/M-12/M-16/M-80 hygiene | 与 root/契约语义或 seam 义务冲突；性能实验无意义 |
| 经济上不可行（条件） | M-53 d64（若效应接近 d16 的 0.4% 乐观假设） | 0.4% 不是 d64 实测效应；若效应为 0.4%，`hw≤效应/2` 需 336/346 对，A/A+A/B ≥524 对；P4 点估计 1.06%/1.13% 不显著、尚未证明大于 0.4%；CPU 指标 2% 需约 370–371 对 |

**A/A 预算（关键）**：M-53 δ=2% 的 A/A 17–20 对、A/B 17–20 对；δ=1% 的 A/A 与 A/B 各 58 对；若 δ=0.4%（乐观代理假设，不是 d64 实测）A/A 346 对 + A/B 178 对（合计 ≥524 对）。W1 若用 C6 cycles 的 1.2% 过程级效应，A/B 需 48 对（80% 功效）/91 对（hw≤0.6%），36 对不够；C1 cycles 需 32/62。W3 按形态：P-D 无带；P-C C4 CPU 2% 20/38；P-E C1 CPU 4/5；P-A C3b cycles 6/10。**A/A 只回答“若效应是 X 需要多少对”，不证明效应是 X。**

---

## 7. 为什么没有 A/B

五条件逐项检查，没有候选同时满足：

1. **合法机制**：W1 D/C1/A4–A8 硬冲突；C2 需 owner；W3 未证步间语义；M-53 合法但 packed 编码未实现。
2. **候选级 measured hotspot**：W1 只有阶段/边界级；W3 只有族级/阶段容器；M-53 d16 0.406/0.285% 低于分辨率，d64 点估计不显著；M-42 只有算术上界。
3. **语义可证**：W1 D/C1 不可证；C2 需新模型；W3 缺 `publication_inflight`/slot reuse/observer 证明；M-53 O3 未证。
4. **A/A 分辨率**：P-D 的 C2b NOISY；C4 的 hw≤1% 需 38 对；M-53 δ=2% 才 17–20 对，δ=1% 需 58，0.4% 需 172/336。
5. **无历史反例**：M-61 R3 0.45% <1% MME；`#637` 的 `NO_GO`/`NO_GO_ABORT`/`SEMANTIC_MISMATCH` 均未翻案。

**结论**：不是“没有时间做 A/B”，而是**没有候选有资格进入 A/B**。`#604` G0 真实 workload 未准入，`sluice-copy` 是参考应用，不能声明 application-level 收益。

---

## 8. Owner Gate（O-1–O-12 修订后）

| # | 裁决点 | 建议 |
|---:|---|---|
| **O-1** | W1 profile 门：接受 bench/reference 代理，还是等 `#604` G0？ | 等 G0 或只做设计/A-A；接受代理只能作 mechanism-level/bench-proxy |
| **O-2** | W1 通知拓扑：是否授权第二通知通道/blocking-read 重构？ | **拒绝在 #621 W1 内做**；若追，另立 `#617` G1/kernel-boundary，带新模型/新 oracle/机制门 |
| **O-3** | W1 的“5%”分母：process cycles / measured-loop cycles / measured-loop CPU？ | 必须先定义；未定义则任何代理 A/B 只能记 `INCONCLUSIVE` |
| **O-4** | W3 是否接受 δ=2% 作为计划值，并资助行级归因？ | 接受 δ=2%，先做行级归因（P-D/P-C/P-E 主形态）；P-D 先重跑 C2b A/A；不直接 A/B |
| **O-5** | M-53 是否接受 cycles 为决定性指标，并做 d64 A/A？ | 接受 cycles + δ=2% 的 17–20 对；**17–20 不能用于 δ=1%**；δ=1% 需 58；0.4% 需 172/336（合计 ≥524） |
| **O-6** | M-42 是否现在资助隔离格？ | 保持排队；`GO` 排队 ≠ A/B 准入；资助前不启动 |
| **O-7** | 语义冲突批的 owner 裁决 | M-09/M-30/M-57/M-12/M-16/M-80 hygiene 逐项 KEEP/DELETE/修 root；**W1-C2 不在此批** |
| **O-8** | M-01 的 free-list 是否重开（S2）？ | 先 `-scope64` 隔离 profile，再裁 free-list |
| **O-9** | M-37/H1/tp-w8 的归属边界 | 确认 `#595`/`#604` 归属；`#621` 不启动 |
| **O-10** | M-28 是否确认不重做？ | **确认 `NO_GO_ABORT`（不重做）**；不是 `NOT_APPLICABLE`；仅四要素同时满足才重开 |
| **O-11** | M-61 payload 是否维持 `NO_GO`？ | **维持 `NO_GO`** |
| **O-12** | W2/W5 是否确认无新候选？ | **确认**；任何改动重跑 9 用例 oracle + mutant（W2）/ parity 8 + 4 mutants（W5） |

---

## 9. 最终建议与剩余阻断

**建议：`NO_FURTHER_OPTIMIZATION_JUSTIFIED`（当前没有可进入 A/B 的候选；W3/M-53 设计门仍开放，需 owner 决定是否资助）。** 本战役没有新增减量；当前不建议启动新的 A/B。

- **所有合理路径是否已验证？** 不是 100%：W1-C2、W3 的候选级行级归因/步间语义、M-53 的 O3/d64 仍未验证；但它们在 current contract 内都需要 owner 改范围/预算或另立项目，#621 可自行执行的路径已全部裁决（W1 四方向族、W3 四成员、M-53 三义务、M-42、`#637` 22 候选 + 3 非候选）。
- **哪些真正减少了机器工作？** 本战役 0 个新增；只有已合并的 `#634`（CPU/task −5.58%、wall −6.53%、instr −22.1%，24 对，`PARTIAL_CPU_ACCOUNTING`）与 `#635`（instructions −5.0180%、cycles −5.1111%、task-clock −5.4653%、rusage CPU −5.3199%，36 对）。
- **哪些不能优化？** W1-D1/D2/D3/C1/A4–A8（语义阻断）；W1-C3（本战役范围 `NO_GO`：epoll/timerfd/relay 无净减量）；M-42（≤0.14% cycles，低于噪声）；M-28/H1/io-wq/tp-w8（`NO_GO_ABORT`/`NO_GO`）；M-09/M-30/M-57/M-12/M-16/M-80 hygiene（owner 裁决面）；W3 单候选（有候选级 owner-total 点估计，但单候选可删量 `NOT_OBSERVABLE`）；M-53 d64 若效应接近 d16 的 0.4%（乐观假设）则 172/336 对、A/A+A/B ≥524 对，经济上不可行；P4 点估计 1.06%/1.13% 不显著，尚未证明大于 0.4%。
- **剩余空间是否还有投入价值？** 在 #621 的当前契约、环境、预算内：不建议继续 A/B。唯一涉及通知拓扑语义授权的路径是 W1-C2（O-2）；它是 `#617` G1/独立设计项目，不是 #621 的收尾实验。W3/M-53 的设计门仍需 owner 通过 O-4/O-5 决定是否资助。`#604` G0 未准入，任何 application-level 声明也不可用。
- **O-2 是唯一涉及通知拓扑语义授权的 owner 点**；若拒绝，C2 不能进入设计/建模，更不能进入 A/B。但 **O-4（W3 行级归因/步间语义）、O-5（M-53 O3 + d64 cycles-only A/A）、O-6（M-42 隔离格）、O-8（M-01 `-scope64`）是设计/资格实验，不是状态确认**；O-1 是 W1 profile 范围门，O-3 是 W1 预注册分母定义；在 owner 未资助/未预注册前，相关项保持开放。
- 若 owner 拒绝 O-2 且不资助 O-4/O-5/O-6/O-8，则 #621 的当前研究范围可关闭；但这是 owner 的决定，不等于 W3/M-53 的设计门已验证。

**剩余阻断（明确）**：通知拓扑语义授权 = **O-2**；当前没有可进入 A/B 的候选；W3/M-53 设计门与 M-42/M-01 隔离格/profile 仍开放，需 owner 决定是否资助。O-2 只是通知拓扑语义授权点，不是总体上的唯一未完成项；O-4/O-5/O-6/O-8 不是状态确认。

---

## 10. 纪律与来源

- **份额 ≠ 可删 cycles；阶段容器 ≠ 候选上界；inclusive 不能相加；A/A ≠ A/B。**
- 本评论的数字全部来自随附 `FINAL-REPORT.md` 与其来源（P1/P2 A/A、P2 成本地图、P3 准入、P4 行级归因、M-53 义务审计、构建回执），并按“本会话复算 / 已记录 / 已合并历史 / 未运行”区分；**没有把未运行写成通过**。
- 本评论**无凭据、无 token、无明文密码、无本地临时路径依赖**；不依赖两个“全程不得触碰”的未跟踪工作副本。完整报告与证据目录随本评论一并提交/归档于同一路径；若尚未提交，读者应以提交后的同一路径为准。
- `#621` 停在 Owner Gate；不自动 merge、不自动关闭。

---

*回写稿结束。完整 A–K 报告见同目录 `FINAL-REPORT.md`。*
