# #595 R4 — 预注册（PREREGISTERED，正式性能数据产生之前冻结）

- 文档角色：#595 R4 轮（READY_REUSE / STATE_AWARE 感知提交策略）的**唯一**性能预注册。
  本文件一经冻结（§15 回执），其中全部门槛、随机化、样本量、排除规则与判定树
  **不得修改**；任何偏离只能以新开附录（addendum）方式记录并降级相应结论。
- frozenBeforeTiming = **true**（冻结时点见 §15；正式性能计时尚未开始——本轮已完成
  的全部运行均为机制冒烟/构建/正确性验证，见 §10.6）。
- 冻结基线协议：本文沿用 R3 完成轮冻结预注册
  `/home/jnhu/sluice-595-e5/r3-completion-20261105T083818Z/evidence/PREREGISTERED_R3_COMPLETION.md`
  （sha256 `1a9c1e12…` 前缀，1007 行）与续跑附录
  `/home/jnhu/sluice-595-e5/r3-flip-20261105T1428Z/evidence/PREREG_ADDENDUM_CONTINUATION.md`
  的计时/CPU/统计口径（引用处标 §R3-x.y）；差异均在本文明示。
- 常设方法：`docs/performance/methodology.md`（53 行；Freeze→Baseline→Profile→
  Attribute→Optimize→Decide，跨机不可比、profiling 与 clean run 分离、无归因测得的
  改动直接拒绝、workload-local 测量不得声称 library-wide 改进）。
- 伴生文档：`BASELINE.md`（环境与冻结对象清单）、`DESIGN.md`（信息审计、状态-动作表、
  参考规则映射）。

---

## 1. 对象冻结（本 ask 项 1）

### 1.1 源码冻结

| 项 | 值（实测） |
|---|---|
| 冻结基线提交 | `d9d140eea180f29f629992aa293109acdeff1e03`（`research-595-r4`，R4 frozen baseline = #627 三补丁重建） |
| 实现提交（本轮对象） | `f3b25329bc8a64a201961c426a58adacfb8fdf08`（HEAD，`research-595-r4`，worktree porcelain=0，2026-11-06T04:53:25+08:00 提交） |
| 基线→实现补丁 sha256 | `d66f3bcf52d01c5ee19ecd6285d67586d4467a44ecc8d608b1b5a8e9b267cf49`（`git diff d9d140ee..f3b25329` 全量） |
| 补丁内容 | 5 文件：`bench/r3_ready_batch.hpp`（489 行变更，主体）、`bench/bench_w03_external_loop.cpp`、`bench/bench_w02_pipeline.cpp`、`bench/bench_common.hpp`、`bench/r4_arm_smoke.cpp`（新增）；累计 +1126/−24 |
| 关键源文件内容 sha256 | `bench/r3_ready_batch.hpp` = `363f346ac3b26076322cbe0126a9d276aedbcb1d2ecd7607c108f8c32621dd81`（= 正确性轮登记值，tree_proof 复核一致） |
| git blob（f3b25329） | `r3_ready_batch.hpp=0284d098…`、`bench_w02_pipeline.cpp=95cc4b79…`、`bench_w03_external_loop.cpp=8a01af62…`、`bench_common.hpp=16588098…`、`r4_arm_smoke.cpp=b4494f83…` |
| 上游补丁链（重建基线的输入） | r3-completion.patch `2596f204…`、delta_w02.patch `4aa7b306…`、delta_w03.patch `57f21e02…`（双源核验见 `evidence/BASELINE-FREEZE.md` §2） |
| 产品源码改动 | **零**（补丁只触碰 `bench/`；`include/`、`src/` 与冻结基线逐位一致） |

### 1.2 bench 二进制冻结（sha256 实测，2026-11-06）

构建：worktree @f3b25329 提交后全量重建（持锁；`logs/final_rebuild_smoke.log`，
`logs/LOCK_RECEIPT.md` 第 8 条），`xmake f -m release`，builddir
`/home/jnhu/sluice-595-e5/r4/build`。参考副本与清单：
`evidence/frozen-binaries/MANIFEST.txt`。

| 二进制 | sha256 | 用途 |
|---|---|---|
| `build/linux/x86_64/release/bench_w03_external_loop` | `f029607fc1a657efe0ba96203115f27a90d598f12c274e9360b6029a5e019009` | W-03 全部计时/功能块 |
| `build/linux/x86_64/release/bench_w02_pipeline` | `daee6b6c1ab618a29d8e1afc120f8da5e478756b2def7c77c712f54a6a4e1931` | W-02 全部计时/功能块 |
| `build/linux/x86_64/release/r4_arm_smoke` | `07dc3176a1d66bacc91eb2c217fb9a66f292cfc9a11afbb6eae5a0b5ef77bf3b` | 仅机制冒烟（非计时证据） |

绑定规则：每个计时 invocation 启动前由 runner 重算二进制 sha256 并与上表比对；
不符 → invocation 无效（§10.2）。禁止重建、替换或触碰 `r4/build` 中的上述对象；
若意外失配，整块作废（BLOCKED_OBJECT）而非换用新二进制。

### 1.3 臂集合与选择机制（冻结）

臂由环境变量 `SLUICE_595_R3_ARM` 的**精确 token** 选择（`bench::r3::arm_from_env()`，
`bench/r3_ready_batch.hpp:42-74`；非法/缺省 → A0），在 `main` 读一次，以
`bench::r3::Arm` 枚举传入 Adapter（W03：`bench_w03_external_loop.cpp:862`；W02 沿用
`effective_arm` 强制规则 `bench_w02_pipeline.cpp:756-758`：仅 uring 非饱和 sliding
window phase 允许研究臂，否则 NOT_RUN_WITH_REASON）。同一二进制、同一执行路径；
W02/W03/r4_arm_smoke/既有确定性测试共用同一 Adapter。

| 臂 | token | 语义（冻结） |
|---|---|---|
| A0 | `A0` | 产品逐条路径（无 adapter；每次 `submit_read` 一条）——一切对照的锚 |
| A1 | `A1` | R3 adapter 单条路径（adapter 存在但每次呈现 1 条；仅归因用，§11.4） |
| B | `B` | R3_FIXED_PREFIX：整跨度一次呈现，入口内 cap=8，untouched 后缀走 singles（R3 轮候选，冻结未动） |
| R | `R` | READY_REUSE：整跨度呈现；`stop=cap 且 accepted>0` 时 untouched 后缀经同一批量入口重入 |
| S | `S` | STATE_AWARE：表驱动 quantum（§1.5） |
| SNI | `SNI` | S_without_inflight_hint：不读 `outstanding()` |
| SNP | `SNP` | SNP=S_without_pressure_hint：不读 pass 事实 |
| Q2 | `Q2` | 固定 quantum=2，无 hint 读取 |
| Q8 | `Q8` | 固定 quantum=8，无 hint 读取（与 R 在 16-op refill 冒烟中呈现/SQE 计数等价） |
| SHADOW | `SHADOW` | 读并记录两种 hint（counterfactual 表求值），但按 R 动作呈现 |

策略隔离（防 case/fixture 感知，冻结声明）：S 的决策函数是纯表求值
`budget::evaluate(Input)`（`r3_ready_batch.hpp:252-289`），输入仅含
remaining/eligible_prefix/active_data/pressure/prior-stop 标志/pinned_quantum；
Adapter 内无 getenv、无 case/depth/文件名引用；harness 读环境变量仅用于输出与
case 过滤，不进入决策。诊断通道 `SLUICE_595_R4_DIAG_LINES=<path>` 只在独立诊断
运行设置（`bench_w03_external_loop.cpp:341-358`）；正式计时块设置该变量 →
invocation 无效（§10.2）。

### 1.4 信息定义（审计绑定，冻结）

| 决策输入 | 绑定 | 读者 |
|---|---|---|
| `remaining` | caller 本次呈现调用的 ready 跨度长度 | 全部 budgeted 臂 |
| `eligible_prefix` | 对 caller 自己的 op 内容按冻结形态镜像谓词计算：`len==4096 && dst!=nullptr` 的前缀长度（`batch_shaped_prefix`，`r3_ready_batch.hpp:187-196`；镜像 `UringAsyncBackend::batch_eligible_read` 的冻结形态，入口内重新权威求值） | 全部 budgeted 臂 |
| `active_data` | `AsyncIoContext::outstanding()`（公共权威 API；`include/sluice/async/async_io_context.hpp:273`，实现 `src/async/async_io_context.cpp:837-840` = 持锁转发 backend 的 accepted-未收集计数） | S、SNP、SHADOW |
| `pressure` | 调用方作为参数传入的最近一次 owner pass 快照：`pressure_known && (last_pass_completed>0 ∨ last_pass_immediate_work)`；由 driver 经既有 `shared.mutex` 通道发布（字段 `bench_w03_external_loop.cpp:151-158`、发布 `:244-258`、submitter 持锁读取 `:415-422`），**不为 hint 发起新 poll** | S、SNI、SHADOW |
| `caller_window` | caller 侧未收集 window 大小的无锁镜像；仅观察（shadow 记录），永不作决策权威 | SHADOW |
| prior stop/residue 标志 | 上一次呈现返回的 `stop∈{control,recovery}` 与 `residue_advertised`（authority 边界） | 全部 budgeted 臂 |
| `pinned_quantum` | 臂策略常量（Q2=2、Q8=8，其余 0） | Q2、Q8 |

禁止读取（判据：源码审计 + 决策输入枚举）：case id、depth、request_count、
fixture/env、文件名、SQ 环剩余空间（ring 为 backend 私有；调用方读 SQ space 被审计
明令禁止——诊断只记录 `sq_budget` stop 投影）。『实际 enter 提交尺寸』同理以
presented/accepted/flush 报告字段为代理。

shadow 防编译器消除（冻结）：shadow 读取并入已导出的
`r4_shadow_outstanding_sum` / `r4_shadow_window_sum` 计数器（流经 bench Record 的
printf 与 JSONL＝不可优化 sink）。运行时证据：W03 SHADOW 冒烟
`shadow_outstanding_sum=76`、`shadow_window_sum=43`（`logs/w03_smoke_SHADOW.log`，
非零只能来自决策点读取）。**冻结判据**：凡 SHADOW 计时 invocation 报
`shadow_outstanding_sum==0` → 该 invocation 无效（oracle 失败类，§10.2）。

### 1.5 S 最终选择规则（冻结；与源码逐条对应）

表（`r3_ready_batch.hpp:222-244`，行序即求值序；ACTION 首个非 batch 匹配行获胜，
quantum 效果叠加为 min）：

| # | 行 | 谓词 | 动作 | quantum 效果 |
|---|---|---|---|---|
| 1 | `remaining_single` | `remaining==1` | immediate（呈现 1） | 1 |
| 2 | `front_ineligible` | `eligible_prefix==0` | immediate（呈现 1，走入口权威判定） | 1 |
| 3 | `authority_signal` | prior stop∈{control,recovery} ∨ residue_advertised | authority（退出呈现循环，余量走产品 singles） | 0 |
| 4 | `quantum_base` | 恒真 | batch | **置 8（默认/静态回退）** |
| 5 | `inflight_zero` | inflight hint active ∧ `active_data==0` | batch | **min(·,2)** |
| 6 | `completion_pressure` | pressure hint active ∧ `pressure==true` | batch | **min(·,2)** |

呈现上限：`present_len = min(quantum, eligible_prefix)`（为 0 时取 1；
`r3_ready_batch.hpp:497-499`）；**SQ 容量界由入口权威执行**（`stop=cap` + untouched
后缀；S 因 `reuse_on_cap=true` 对 untouched 后缀经同一入口重入，`input_exhausted`
且有余量时 chunk 继续，无进展/authority 信号时退出——零进展续环被 M2 变异证明承载）。

即冻结口径：**表驱动 quantum，默认 8；`outstanding()==0` → 2；合法 completion
pressure → min(·,2)；batch_limit = min(quantum, SQ 容量, eligible prefix)；静态回退
8**。行 1/2/3 为非 batch 优先行。SNP/Q2/Q8/SHADOW 的差异只在于 hint 激活位与
pinned 覆盖（§1.6）。

### 1.6 消融臂定义（冻结；`arm_policy`，`r3_ready_batch.hpp:160-182`）

| 臂 | reuse_on_cap | budgeted | read_inflight | read_pressure | pinned | shadow |
|---|---|---|---|---|---|---|
| S | true | true | true | true | 0 | false |
| SNI | true | true | **false** | true | 0 | false |
| SNP | true | true | true | **false** | 0 | false |
| Q2 | true | true | false | false | **2** | false |
| Q8 | true | true | false | false | **8** | false |
| SHADOW | true | true | (读，仅记录) | (读，仅记录) | 0 | **true**（按 R 动作呈现） |
| R | true | false | — | — | — | —（整跨度呈现+cap 重入） |
| B | false | false | — | — | — | —（整跨度一次呈现+suffix singles） |
| A0/A1 | false | false | — | — | — | —（产品逐条 / adapter 单条） |

消融读法（冻结）：S−SNP = inflight hint 的动作价值；S−SNI = pressure hint 的动作
价值；S−Q2 / S−Q8 = 表选择相对固定 quantum 的价值；SHADOW−R = **纯读取成本**
（动作与 R 相同，读与记录为唯一差异）；Q8−R = 量子化呈现 vs 整跨度+重入。

### 1.7 正确性前置判定（冻结，均已通过；证据 `evidence/correctness/RECEIPT.md`）

逐臂判定采用任务书冻结文本（A0/A1/B/Q2/Q8/R/S/SHADOW/SNI/SNP 十臂全绿：18 场景、
约 3×10⁴ 断言位点的确定性 oracle 套件 BASE GREEN，M1 重复接受/M2 零进展续环/M3
authority 边界三变异全部被杀死，恢复 GREEN 后 header sha256 与 git blob 逐位一致）。
正确性结论在本文件冻结后**不再重开**；计时轮中正确性以 §10.2 的 invocation 硬门
（exit 0 + oracle + 计数对账）持续核验。

---

## 2. case 清单与主次问题（本 ask 项 2）

### 2.1 核心矩阵（冻结）

全部 4KiB、warm tmpfs 64MiB fixture、原生 E5、`taskset -c 2`、WARMUP=1/MEASURED=5
（每 rep 固定 20,000 有效读；例外：r3d16cap 64 读/rep、r3d2pos 2 读/rep——均为冻结
case 参数）、`SLUICE_BENCH_STATS=0`、精确 case filter。

| 格 | case id（精确 filter） | 所需 env | 跨度/状态域特征 | 臂组 |
|---|---|---|---|---|
| W03-d16（**主形状**） | `=W03-uring-4k-d16` | — | 完成驱动 refill，成组突发+持续多 ready 混合 | **8 臂**（A0,R,S,SNI,SNP,Q2,Q8,SHADOW） |
| W03-d2serial | `=W03-uring-4k-d2-r3d2serial` | `SLUICE_595_R3_CASES=1` | 稀疏单条（每 refill 1 条；outstanding 在 0..2 振荡） | 4 臂（A0,R,S,Q8） |
| W03-d4 | `=W03-uring-4k-d4` | — | 浅流水持续 | 4 臂 |
| W03-d64 | `=W03-uring-4k-d64` | — | 深流水持续；整跨度呈现受 SQ 容量截断 | 4 臂 |
| W02-d1 | `=W02-uring-4k-d1` | — | 产品逐条路径的极限（跨度恒 1；adapter 单条开销暴露） | 4 臂 |
| W02-d2 | `=W02-uring-4k-d2` | `SLUICE_BENCH_595_DEPTHS=1` | 最小合发机会（跨度 ≤2） | 4 臂 |
| W02-d16 | `=W02-uring-4k-d16` | — | W-02 sliding window 稳态（冒烟显示 refill 多为单条/偶发小跨度） | 4 臂 |

诚实声明（冻结）：任务书原文核心矩阵为 "W02 d1/2/3/16"。冻结对象集内 **W02 无 d3
case**（默认深度 1/4/16/64，opt-in 只加 d2；`bench_w02_pipeline.cpp:916-936`），
W02-d3 记 `NOT_RUN_WITH_REASON: no frozen W02 d3 case`；最近的冻结深度 3 形状是
W03 `r3d3`（本矩阵未列入门槛，如跑只能作描述性补充）。W02-d64 存在于冻结对象集
但不在本预注册门槛内（如跑同样只作描述性补充）。不得为补 d3 修改任何源码或重建
二进制（§1.2 绑定规则）。

### 2.2 主问题（冻结）

**W03-uring-4k-d16 上 `R−A0` 与 `S−A0` 的联合门**：CPU/op、吞吐、p95/p99 四类
指标按 §7.1 全条件同时满足（两臂各自全过；一臂不过则该臂 FAIL，另一臂独立判定）。

### 2.3 次问题（冻结）

其余核心格（2.1 表除 W03-d16 外）为**适用范围刻画**：每臂对 A0 只做保护判定
（§7.2）+ 机制计数描述（presentations/reentries/quantum 直方图/enter 数），不设
获益门；结论措辞上限为 "在该形状无退化/有描述性差异"，不得外推为 library-wide
（methodology.md:45-47）。

---

## 3. 状态变化负载轨迹（本 ask 项 3，冻结具体参数）

### 3.1 TRAJECTORY 块定义

一个 trajectory 槽位 = 同一块内对同一臂**背靠背顺序执行**的三段，每段一次完整
invocation（固定顺序 T1→T2→T3，同一外部轨迹施于所有臂）：

| 段 | case | 冻结参数 | 轨迹语义 |
|---|---|---|---|
| T1 稀疏单条 | `W03-uring-4k-d2-r3d2serial` | depth=2, refill=1, 20000 读/rep | 每次决策跨度=1；`outstanding()` 0..2 振荡（S 冒烟：active_zero=20000, immediate=20000） |
| T2 成组突发→completion pressure→排空→重启 | `W03-uring-4k-d16-r3d16cap` | depth=16, refill=16, 64 读/rep | 每 rep：首 refill 16（突发）→ 完成压力下 2 条级 chunk（diag 样例：`completion_pressure` 行 quantum=2, active 2,4,6…生长）→ 全窗口排空 → 下一 rep 从空管重启 |
| T3 持续多 ready | `W03-uring-4k-d16` | 产品参数 | 完成驱动持续 refill，稳态多 ready |

轨迹块臂组 = 8 臂（同主形状），24 matched blocks（§5 随机化）。段内即状态变化：
"阶段"由负载到达与完成时序自然形成；**算法不得读阶段标签**——结构保证：Adapter
决策输入不含 case/段信息（§1.3/§1.4），段顺序对每臂相同。访问模式确定性
（offset = sequence % blocks，`bench_w03_external_loop.cpp:411`）：同一轨迹对
所有臂逐位同构，无随机到达成分需要配对。

### 3.2 closed-loop 与外部定时到达（分开，冻结）

- 冻结对象集内全部 case 均为 **closed-loop**（submitter 在 completion 驱动下
  refill，提交循环 `bench_w03_external_loop.cpp:395-530`）；核心格与轨迹块的
  **一切门槛**只基于 closed-loop 数据。
- 外部定时到达（open-loop / 定时器到达）在冻结二进制中**没有安全 harness**
  （无 pacing 机制；不得新增——新增即改冻结对象）。记
  `OPEN_LOOP=NOT_RUN_WITH_REASON: no frozen open-loop harness`；若未来出现，须另写
  预注册附录且不追溯修改本轮门槛。

### 3.3 负载生成器成本单列（冻结）

W-03 的负载生成器 = submitter 线程（构造 ReadOp+臂逻辑）+ driver 线程（progress
owner）。主指标 CPU 已含全部执行者（§9）。单列方式：CPU 分桶诊断轮（§9.4）对
W03-d16 全部 8 臂报告 main(submitter)/driver/io-wq/未归因残差份额（10ms tick 粒度，
残差如实披露）；driver 侧纯 submit enter 期望 =0（提交全在 submitter，R3 轮
RESULTS.md:131-132 口径沿用并复核）。臂逻辑本身的开销**不从**候选对 A0 的账中
扣除（§8）。

---

## 4. 消融计划（本 ask 项 4，冻结）

在 **W03-d16（主形状）** 与 **TRAJECTORY（§3.1）** 两个形状上，同一 8 臂 matched
块内跑全 `S/SNI/SNP/Q2/Q8/SHADOW`（+A0/R 锚）：

- 对比集合（均在 tuple 内计算）：S−SNP、S−SNI、S−Q2、S−Q8（hint/表的**动作**价值）
  与 SHADOW−R（**纯读取成本**：动作与 R 相同）。这些对比**不设获益门**，产出：
  (a) 点估计与双侧 95% CI 的描述表；(b) 若 S−Q8 或 S−Q2 的 CPU/op 出现 >+3% 的
  单侧 95% 上界（表选择自身有可测成本），记 `TABLE_OVERHEAD_FLAG` 并在结论中
  作为限制披露——不翻转主门。
- SHADOW 的 counterfactual 记录（r4_shadow_* 计数器与诊断行）只作机制描述，不进
  任何门槛。
- 消融对比的统计单位与 bootstrap 与 §5 相同（tuple 重采样）。

---

## 5. 随机化与统计（本 ask 项 6，冻结）

### 5.1 样本量与区组

- 每个核心格/轨迹块：**24 个 matched blocks**；块内每臂恰一次 invocation
  （WARMUP=1/MEASURED=5）。不足 24 个有效块 → `INCOMPLETE`，不得 PASS（§R3-6.5
  规则沿用）。
- 统计单位：**block（matched tuple）/ invocation**。quad（4 臂格）与 octet（8 臂格）
  内的差值只在 tuple 内计算；bootstrap 对 tuple 索引重采样；**禁止**把 5 个
  measured rep 当独立样本跨臂 bootstrap。

### 5.2 顺序随机化（预固定 seed，冻结）

- **4 臂格 {A0,R,S,Q8}**：24 种平衡排列（4!=24 恰好各用一次）。生成：`perms =
  list(itertools.permutations(['A0','R','S','Q8']));
  random.Random(5954).shuffle(perms)`；块 j（0 起）按 `perms[j]` 顺序执行。平衡
  性：每臂每位置恰 6 次、每有序相邻对恰 6 次。生成代码与 sha256 记入 header.json。
- **8 臂格 {A0,R,S,SNI,SNP,Q2,Q8,SHADOW}**（W03-d16 与 TRAJECTORY 每段）：确定性
  循环移位——base = `['A0','R','S','SNI','SNP','Q2','Q8','SHADOW']`；块 j（0..23）
  顺序 = base 左移 `j mod 8` 位，且当 `(j // 8) % 2 == 1`（中间 8 块）时整体反转。
  平衡性（如实声明）：每臂每位置**恰 3 次**；方向为正-反-正三轮，只有 base 环的
  8 个有序相邻对及其反向被覆盖（非全序对平衡）。该残余顺序偏差由 tuple 内差值 +
  A/A 噪声校准承载；不得据此在事后改用其他排列。无 RNG，完全确定。
- **A/A 块**：同臂两次独立 invocation（m1/m2）；块偶 → m1 先，奇 → m2 先。
- 块内臂间不插任何休眠/调参；invocation 间只做 §10.3 的记录动作。

### 5.3 Bootstrap 与区间（冻结）

- 每 tuple 的 pair-relative 差：CPU/尾延迟 `d=(X−A0)/A0`（正=更差）；吞吐
  `d=(tput_A0−tput_X)/tput_A0`（正=吞吐下降）。S−R 路线同式以 R 为分母。
- 汇总：24 个 tuple 的均值；bootstrap **10,000 次、seed 5950、percentile 95% CI**
  （numpy 或等价实现；实现与 sha256 随结果记录）。
- 单侧界：`CI95_upper`（恶化方向）；one-sided p = bootstrap 均值 ≥ 界限的比例
  （与 R1 `harness/analyze.py:9-10` 口径一致）。
- 功效规则（§R3-9.7 沿用）：若某 metric 的 CI 半宽超过其门槛对应界（CPU 5pp、
  吞吐 3pp、p95/p99 5pp；主门 CPU 上界用 5pp）→ 该 metric `NOT_ESTABLISHED`，
  点估计在界内也不得写 PASS。

### 5.4 多重比较家族与 Holm（冻结）

| 家族 | 成员枚举 | 校正 |
|---|---|---|
| F-MAIN | W03-d16 上 {R−A0, S−A0} × {CPU 点估计, CPU 上界, 吞吐, p95, p99} 的**联合全条件门**（§7.1） | 合取门，无 Holm（本身即最保守的联立形式） |
| F-PROT | 全部 (格, 臂, metric) 保护比较：格 ∈ 核心格∪扩展保护格（该格实测臂集），臂 ≠ A0，metric ∈ {CPU, 吞吐, p95, p99} | **Holm–Bonferroni 单侧 α=0.05**；每成员须同时过单侧界与 Holm 调整 |
| F-VALUE | 感知额外价值路线（§7.3）：路线 ∈ {A, B} × 形状 ∈ {W03-d16, TRAJECTORY-T2}，共 4 个成员（路线内条件合取） | Holm–Bonferroni 单侧 α=0.05；**至少一个**成员过 Holm 才可声明"感知额外价值" |

非劣检验原假设（冻结）：`H0: mean(d) ≥ 容忍界`（**退化达到或超过容忍界**）；
单侧 p < 0.05（Holm 后）才拒绝 H0（即通过该保护/非劣判定）。

### 5.5 样本追加规则（只按本条，冻结）

- 每格恰好 24 块，**没有**中途加样；无效 invocation 替换上限 2 次/槽位（共 3 次
  尝试，全部保留于 `attempts.jsonl`）。
- 唯一允许的"追加"：整会话因 §10.1 环境失效判据在完成前中止 → 该**格整体重跑**
  （非续跑），原数据全保留并标注；每格至多重跑 1 次。仍不足 24 → `INCOMPLETE`。
- 任何"某臂差几对就再跑几块凑显著"的行为被禁止且数据不得进入分析。

---

## 6. A/A 校准（冻结；§R3-8 规则沿用）

| 块 | case | 成员 | 门（半宽） |
|---|---|---|---|
| AA-W03-D16 | `=W03-uring-4k-d16` | A0 vs A0（同二进制/同 env） | CPU ≤5pp；吞吐 ≤3pp；p99 ≤3pp |
| AA-W03-D2SERIAL | `=W03-uring-4k-d2-r3d2serial` | A0 vs A0 | 同上 |
| AA-W02-D2 | `=W02-uring-4k-d2`（`SLUICE_BENCH_595_DEPTHS=1`） | A0 vs A0 | 同上 |

- 三块**全部 PASS** 才允许任何 A/B、负控、正控、保护与 VALUE 对比；A/A 均值偏移
  不构成失败，只有半宽门。
- 预声明修复（本轮额度 **1 次**、仅作用于一个失败块、修复前写
  `REPAIR_RECEIPT.md`）：选项 (a) `taskset -c 2,12`（两成员同改）或 (b)
  `WARMUP=2`（两成员同改），二选一不混用；修复后整块 24 对重跑，双回执保留。
- 修复后仍失败 → 该 metric 记 `INCONCLUSIVE_NOISE`；依赖该 metric 的门全部
  `NOT_ESTABLISHED`（不得用第二次修复；R3 轮额度已用尽不消耗本轮额度，也不得
  援引）。背景：续跑轮 AA-W02-D2 p99 半宽 3.431pp 曾 FAIL（面功效），本轮以三块
  A/A + 该规则承接。
- 已知继承：R3 完成轮 AA-W03-D16 0.797/0.821/2.039pp、AA-W02-D2 0.862/0.853/
  1.420pp PASS；统一口径 fresh A/A AA-W03-D16 1.245/1.215/2.628 PASS——以上为
  **r3 二进制**的数字，仅作噪声背景，不得替代本轮 A/A。

---

## 7. 冻结门槛（不得改；本 ask 项 7）

### 7.1 主门：W03-uring-4k-d16，对 A0（R 与 S 各自全条件合取）

| 条件 | 门槛 |
|---|---|
| CPU/op 点估计 | `mean(d_cpu) ≤ −10%` |
| CPU/op 单侧 95% 上界 | `CI95_upper(d_cpu) ≤ −5%` |
| 吞吐单侧下界 | `CI95_upper(d_tput) ≤ +3%`（吞吐下降不超过 3%） |
| p95 单侧上界 | `CI95_upper(d_p95) ≤ +5%` |
| p99 单侧上界 | `CI95_upper(d_p99) ≤ +5%` |
| 正确性 | §10.2 全部通过（块内全部 invocation） |
| CPU 双口径符号一致 | primary 与 secondary 同号，否则 NOT_ESTABLISHED（§9.2） |
| 资源门 | §8 全过（RSS/线程/分配/队列） |
| 功效 | 各 metric CI 半宽不超 §5.3 界 |

### 7.2 其他核心形状保护门（每臂对 A0，进 F-PROT/Holm）

CPU/op 单侧上界 ≤+5%；吞吐下降上界 ≤+3%；p95、p99 单侧上界 ≤+5%。点估计与
单侧界都须在界内；超界 → 该保护成员 FAIL。d4/d64 型裁决沿用 §R3-9.6 精神：
enter 大减而 CPU 未改善是**允许的发现**，不构成 FAIL。

### 7.3 感知额外价值（预注册路线，至少一条，在指定形状上）

指定形状 = **W03-uring-4k-d16** 与 **TRAJECTORY-T2（r3d16cap）**。

| 路线 | 条件（合取） |
|---|---|
| A | S−R CPU/op 单侧 95% 上界 ≤ **−2%**，且 S−R 吞吐上界 ≤+3%、p95/p99 上界 ≤+5%（吞吐/尾延迟保护门过） |
| B | S−R p99 单侧 95% 上界 ≤ **−5%**，且 S−R CPU/op 上界 ≤ **+2%**、吞吐上界 ≤+3% |

F-VALUE 家族 Holm 后至少一个 (路线 × 形状) 成员通过 → 声明"感知额外价值成立"
（并写明在哪条路线/哪个形状）；全部不过 → 如实写"未建立"，不得用描述性数字
变通。

### 7.4 A/A 精度半宽（冻结）

CPU 5pp、吞吐 3pp、p99 3pp；不足记 `NOT_ESTABLISHED`（§6）。

---

## 8. 资源门（本 ask 项 8，冻结；§R3-10.1/10.2 规则沿用）

1. **零新增线程/timer/后台队列**：三路证据——(a) 静态审计（adapter 热路径无
   new/malloc/容器增长/新线程/timer；记录到 `logs/audit.txt`）；(b) 运行时
   `/proc/<pid>/status Threads` 100ms 采样，候选臂用户态最大线程数 ≤ A0；strace
   诊断轮 `clone/clone3/timer_create/epoll_create*/eventfd/eventfd2` 新增 =0
   （kernel `iou-*` io-wq worker 是物理线程，计数记录，不算用户态新增）；
   (c) 分配斜率：`valgrind --tool=memcheck --xtree-memory=full`（或等价 massif）
   在 W03-d16 跑 512/1024 读诊断 pass，slope=(allocs₁₀₂₄−allocs₅₁₂)/512 ≤
   0.01 allocs/read 且 ≤ A0 slope+0.01。任一子门 FAIL → 资源门 FAIL；NOT_RUN →
   NOT_ESTABLISHED。
2. **内存峰值**：每 invocation `/usr/bin/time -v` Maximum RSS + `wait4 ru_maxrss`
   交叉；绝对与相对都报告。同 case/depth/fixture 内 `(RSS_X−RSS_A0)/RSS_A0` 点估计
   与 95% 上界均 ≤ **+2%**，超界 → 该臂 `RESOURCE_RSS_FAIL`（**单独判资源门**，
   使整体不得 GO）。跨 case 不比。
3. **adapter 一次性开销不从候选对 A0 账中扣除**：一切 R/S/消融臂对 A0 的 CPU/吞吐/
   延迟比较用总实测值（含 adapter 构造与 DecisionFacts 通道）；A1 仅单列归因
   （§11.4）。
4. **enter 减少作为机制指标独立报告**：strace 诊断轮 pure_submit_enters（§R3-9.3
   口径）+ invocation 内 r4 计数器（presentations/cap_reentries/chunk_reentries/
   singles/q1/q2/q8/active_zero/pressure）——描述性，不进门。

---

## 9. 计时与 CPU 口径（本 ask 项 9，冻结；沿用往轮并写明边界）

1. **延迟边界**：统一 descriptor/ready → observed completion。ready_ns = 该 ReadOp
   完全构造、任何臂特定提交工作**之前**打点（W03 `bench_w03_external_loop.cpp:370-373`
   的 Inflight::ready_ns；W02 同语义 delta_w02）；完成 = 应用观察到并 take_result。
   两臂同语义点，含 prep/scan/submit。**不得整组提交返回后才计时**。旧 submitted_ns
   口径数字不与本轮比较（跨二进制/口径不可比）。
2. **CPU 口径**：primary = 每 measured rep 的 `CLOCK_PROCESS_CPUTIME_ID` 进程 CPU
   差值（该内核含 iou-* io-wq worker；分桶证明 §R3-6.4:457-459）；`cpu/op =
   Σcpu/Σops`。secondary = whole-invocation `wait4` rusage（utime+stime）；主门
   要求两口径符号一致，否则 NOT_ESTABLISHED（§R3-9.2:584-586）。CPU = 完成相同
   useful task 的总可归因 CPU（submitter/driver/io-wq/其他线程），invocation 间
   fixture 与 case 完全一致以防漏计/重复归因。
3. **p95/p99**：per-invocation = 5 个 measured rep 分位数的均值（rep 内固定读数；
   非 pooled histogram；无逐请求 ECDF——R3 已披露限制沿用）。
4. **主性能与诊断计时分开**：clean 块无 strace/perf/valgrind/diag env
   （`SLUICE_595_R4_DIAG_LINES`、`SLUICE_BENCH_STATS=1` 只出现在诊断轮）；CPU 分桶
   诊断轮单独跑 W03-d16 全 8 臂（main/driver/io-wq/残差，10ms tick，残差如实披露）；
   IRQ 只记整机上界（cpu2 计数器增量），不进臂间差异。
5. **perf/eBPF/PMU**：独立诊断轮；观测成本单独核算；PMU 采样须记录并核查
   inherit/offspring 覆盖（含 io-wq worker）、`time_running/time_enabled`
   （multiplexing 区间标记），未覆盖区间不得用于归因结论。
6. **吞吐**：`Σops/Σwall`（rep 间 wall 含 context setup/shutdown，两臂同口径）。

---

## 10. 有效性规则（本 ask 项 10，冻结）

### 10.1 环境失效判据

| 判据 | 动作 |
|---|---|
| MEASUREMENT.lock 1800s 内不可获取 | 会话 `BLOCKED_LOCK`，无部分数据可用 |
| 块开始前 cpu2/cpu12 有 CPU-bearing 外来进程（`ps -eo pid,psr,pcpu,comm`） | 等待其空闲（≤30min，记录等待）；仍占用 → `BLOCKED_BACKGROUND` |
| 块开始前 loadavg(1min) > 4.0 | 同上推迟；块**开始后**背景负载不使 invocation 无效，只记录为 limitation（§R3-6.5） |
| governor ≠ schedutil 或 cpu2 频率策略变化 | 该会话自该点作废（环境失效），按 §5.5 整格重跑规则处理 |
| `/tmp` 可用 < 1GiB（`df -h /tmp`） | `BLOCKED_DISK` |
| fixture sha256/大小变化 | 该块作废 |

每 invocation 前后记录（§R3-6.3 表沿用）：case id、臂 token、env、taskset mask、
二进制 sha256、HEAD/patch sha256、governor+`scaling_cur_freq`（cpu2）、cpu2/cpu12
占用（100ms `/proc/stat` + pin 进程清单）、`df -h /tmp`、fixture 状态、起止 UTC、
exit code、oracle、ops、峰值 RSS、线程数峰值、锁获取/释放时间。

### 10.2 invocation 无效判据（穷尽；无事后离群点剔除）

bench oracle 失败 / stats gate 失败 / exit ≠0 / ops·request_count·case id 不符 /
二进制 sha 不符 / 臂 selector 读回不符 / SHADOW 计时 invocation
`shadow_outstanding_sum==0` / 计时 invocation 检出 `SLUICE_595_R4_DIAG_LINES` 或
诊断期 diag 文件在计时块期间增长。无效 → 同槽位重试 ≤2 次；全记录于
`attempts.jsonl`。

### 10.3 测量预算（冻结）

- clean 计时会话总墙钟 ≤ **4 小时**（估算 ~1.5–2h：核心+轨迹+扩展+放置 ≈1.7k
  invocations，每 invocation ~1.5–2.5s 含 setup）；诊断轮（strace/valgrind/CPU
  分桶/PMU）另 ≤1 小时。超预算 → 剩余格记 `NOT_RUN`，不得截短某格的 24 块冒充完成。
- ≥15 分钟的计时串行且独占整机，期间禁止并行编译/压缩/归档/其他实验；一切计时/
  构建/tracing 持 `/home/jnhu/sluice-595-e5/r3/MEASUREMENT.lock`（flock 独占；
  LOCK_RECEIPT 记录获取/释放 UTC、`fuser -v`、第二 fd 排他性检查；运行前后记录
  load、cpu2/cpu12 占用、`df -h /tmp`）。

### 10.4 失败保留策略

失败 block 保留在原始表并按本文件规则标注（`FAIL`/`NOT_ESTABLISHED`/`NOT_RUN`/
`BLOCKED_*`/`INCOMPLETE`）；空表不得写 PASS；每个 PASS 可由 `results.jsonl` +
`summary.json` + SHA + 24 tuple 完整复算（§R3-7/11.3 沿用）。

### 10.5 正控与机制对账（冻结）

- 正控（功能，非计时）：`r3d2pos` 每臂一次 WARMUP=0/MEASURED=1：R/S 必须一次呈现
  2 条（presentations=1、singles=0、accepted=2）；A0 走 2 次单条。不过 → 该臂
  机制不存在，主门不得 PASS。
- enter/CQE 对账（诊断轮，strace）：W03-d16 每臂 3 次（WARMUP=0/MEASURED=1，
  `SLUICE_BENCH_STATS=1`）：`data_sqes`=`cqes`=20,000；A0 `pure_submit_enters`
  =20,000；R/S 显著 <A0（合发存在性）且 `to_submit>1` 行 >0；A0 的 retry 偏差必须
  记录不得洗掉。对账数字不是性能证据。

### 10.6 smoke/pilot/holdout 声明（冻结）

- 已完成运行全部为**机制 smoke / 构建 / 正确性验证**（`logs/30-build-smoke…`、
  `r4_arm_smoke.log`、`w03_smoke_*.log`、`w02_smoke_*.log`；清单见 §12.2）。
  smoke 数字**不是** pilot 性能数据，不得在任何结果中当作性能基线引用或外推。
- 冻结后如出现任何非本协议的试验性计时（pilot），必须标 `PILOT` 且不进 §7 门槛；
  受影响格的门槛结论降级，除非另开一个此前未跑的 holdout 格（在附录中事前指定）
  并按本协议完整执行。

---

## 11. 块执行计划与目录（冻结）

### 11.1 块清单与顺序

1. AA-W03-D16 → AA-W03-D2SERIAL → AA-W02-D2（门禁，先跑）
2. MAIN-W03-D16（8 臂 × 24）
3. TRAJECTORY（8 臂 × 24 × T1/T2/T3）
4. CORE：W03-D2SERIAL → W03-D4 → W03-D64 → W02-D1 → W02-D2 → W02-D16（各 4 臂 × 24）
5. PROT-64K-D16（`W03-uring-64k-d16-r3d16-64k`，4 臂 × 24，2,000 读/rep；eligibility
   保持：len≠4096 不合批形态。冻结判据：rc=0 + byte oracle 全过；budgeted 臂
   （S/Q8）`r4_immediate_decisions` = 全部决策（S 冒烟已证：immediate=2000、
   q1=2000、presentations=2000）；任一臂经 strace 诊断出现 `to_submit>1` 的 enter
   或 pure_submit_enters < A0 → 该臂 PROT-64K FAIL（与 §R3-9.5 的 64k 判定同口径））
6. PROT-THREADPOOL（`W03-threadpool-4k-{d4,d16,d64}-w4` × {A0,R,S,Q8}，WARMUP=0/
   MEASURED=1 功能门：rc=0、byte oracle、`io_uring_enter=0`（adapter 非 uring 后端
   回落产品路径）、不新增线程/队列/timer）
7. PROT-NONTMPFS（W03-uring-4k-d16，`SLUICE_BENCH_TMP_DIR=/home/jnhu/sluice-595-e5/r4/evidence/ext4-tmp`（root fs ext4），fixture 预热读一遍后同缓存条件内配对；4 臂 × 24；同 §7.2 界；不与 tmpfs 数字跨条件比较）
8. PROT-SMT-2-12（`taskset -c 2,12`）与 PROT-XPHYS-2-8（`taskset -c 2,8`）：W03-d16
   4 臂 × 8 tuples，safety-only，不进门不用于 PASS；两块都超界 → `PLACEMENT_RISK`
   （主形状结论不得外推到该放置）；一块超界 → limitation
9. PROT-SATURATION（`W02-uring-4k-d16-saturate`）：冻结 `effective_arm` 规则使一切
   研究臂在该 case NOT_RUN_WITH_REASON → 候选臂部分记 `NOT_ESTABLISHED`（结构不可
   测，诚实保留）；A0-only 描述性 12 invocations 供背景
10. PROT-ODIRECT-COLD：条件性。冻结对象集无 O_DIRECT 配置入口、无安全 cold-cache
    判据（tmpfs 常驻、无 drop_caches 权限假设）→ 记
    `NOT_RUN_WITH_REASON: no safe frozen O_DIRECT/cold harness`。只有在出现
    事前附录指定的安全 harness（显式 O_DIRECT 配置 + 可复现缓存态判定 + 独立 A/A）
    时才可执行，且不追溯修改本轮门槛。
11. 诊断轮（clean 块全部完成后，各自持锁独立会话）：strace 对账、CPU 分桶、
    valgrind 斜率、（可选）PMU；r3d2pos 正控

### 11.2 目录布局（冻结）

`/home/jnhu/sluice-595-e5/r4/`：`raw/<block>/`（header.json、results.jsonl、
summary.json、logs/、attempts.jsonl、RECEIPT.md）；`analysis/`（bootstrap/Holm 表、
COMPLETION_GATE.json）；`evidence/`（冻结对象、smoke、正确性、本预注册副本）；
`logs/`；`figures/`。块内目录命名 = 块清单名。runner/analyze 脚本在计时前写入
`raw/` 并记录 sha256（实现本协议，不得偏离）。

### 11.3 会话与锁

每 ≥15min 计时会话一次锁持有；块间可释放重取（每次都有 LOCK_RECEIPT 记录）。
会话边界写 `logs/SESSION_*.md`。

### 11.4 A1 与 B 的角色（冻结）

A1 与 B 不进任何核心格/门槛。A1 仅可选归因块（W03-d16，A1−A0，24 pairs，预算
允许时），标 `ATTRIBUTION`，产出单列 `d_cpu/d_tput/d_lat/enter 数`，不得用于修正
任何 R/S 对 A0 的账。B 的对照需求由 R3 完成轮满足（同形状、旧二进制）；本轮若需
B 数据只能以事前附录开描述性补充块，不进门槛。

---

## 12. 冻结对象侧的既有证据索引（写盘时状态）

### 12.1 冻结链

d9d140ee（基线，`evidence/BASELINE-FREEZE.md`）→ 6a13d2d5（臂+冒烟）→ f3b25329
（pass-fact 通道/记录导出/决策日志）→ 持锁全量重建 → 本文件冻结。worktree
porcelain=0；主工作树与他人 worktree 未动；未推送、未写 GitHub。

### 12.2 冒烟证据（机制 smoke，非性能数据）

`logs/r4_arm_smoke.log`（57 PASS/0 FAIL，correctness×9 臂、perception、ablation、
shadow、counters）；`logs/w03_smoke_{B,R,S,SNI,SNP,Q2,Q8,SHADOW,S_64k,S_d2serial,
S_stats,S_diag}.log`、`logs/w02_smoke_{R,S}.log`；diag 样例 64 行
`logs/r4_diag_lines_sample.log`。SHADOW `shadow_outstanding_sum=76`/
`shadow_window_sum=43`（`w03_smoke_SHADOW.log`）。

### 12.3 已知限制（冻结时如实声明）

- r3d16cap/r3d2pos 每 rep 读数小（64/2）：T2 段 p95/p99 分位粗、10ms tick 下 CPU
  分桶残差大——只影响 T2 段尾延迟功效（可能 NOT_ESTABLISHED），不影响主形状。
- 诊断通道限制：『实际可用 SQ 容量』调用方无合法读取（只记录 sq_budget stop
  投影）；『实际 enter 提交尺寸』以 presented/accepted/flush 为代理。
- 评审遗留 4 个 minor（防御守卫/q8 计数失真/diag 无机制防误设/authority 决策不进
  trace sink）未修（`evidence/correctness/RECEIPT.md` §6），不影响任何判定；本轮
  以 §10.2 的协议侧检查缓解 diag 误设。

---

## 13. 判定树（写死）

1. 三 A/A 全 PASS（含至多一次预声明修复）→ 才看后续；否则相应 metric
   INCONCLUSIVE_NOISE，依赖门 NOT_ESTABLISHED。
2. 正控 + enter/CQE 对账 PASS → 才看主门。
3. 主门（§7.1）全条件 + 资源门（§8）PASS → `GO_MECHANISM`（仅 W03-d16 代表形状）。
4. S 若主门 PASS 且 F-VALUE 至少一成员过 → 另记 `STATE_AWARE_VALUE_ESTABLISHED`
   （路线/形状写明）；S 主门 PASS 而 VALUE 全不过 → `GO_MECHANISM_NO_PERCEIVED_VALUE`。
5. R 主门 PASS、S 任一条件不过 → R 独立 `GO_MECHANISM`；S 记 FAIL 项，不得被 R 的
   收益抵消（逐臂独立裁决）。
6. 保护族任一成员 Holm 后 FAIL → 不得 GO（整体），按块记 FAIL；保护成员
   NOT_ESTABLISHED 不阻塞 GO，但阻塞"该格无退化"的表述（NOT_ESTABLISHED 不产生
   正/负结论）。
7. 主门（§7.1）或资源门（§8）任一条件 NOT_ESTABLISHED/INCOMPLETE → 不得
   GO_MECHANISM，只能 INCONCLUSIVE/PARTIAL；F-VALUE 家族 NOT_ESTABLISHED 只使
   "感知额外价值"未建立，不单独阻塞 GO。
8. 产品层：本轮 research-only；`PRODUCT=HOLD`，不开产品候选 PR，不改 KEEP_IMMEDIATE
   （真实应用须独立预注册，#604 handoff 不存在）。

---

## 14. 锁协议（冻结）

- 锁文件 `/home/jnhu/sluice-595-e5/r3/MEASUREMENT.lock`（inode 6941075，dev 0:53）。
- 每次 ≥15min 计时/构建/tracing 会话：`flock` 独占（fd 9，`-w 1800`）；LOCK_RECEIPT
  记录获取/释放 UTC、等待时长、`fuser -v`、第二 open file description 的
  `flock -n` 排他性检查（必须 blocked）、会话内命令清单；释放后复核立即可取。
- 会话前后记录：loadavg、cpu2/cpu12 占用、`df -h /tmp`；不向锁文件写内容。

---

## 15. 冻结回执

- 本文件由 R4 预注册撰写员在正式性能数据产生之前写盘；sha256 由外部 manifest
  记录（`evidence/PREREG_MANIFEST.txt`），不自引用。
- 冻结时刻之后的任何性能运行均以本文件为门槛唯一来源；此前没有任何 r4 正式
  性能计时（已有运行清单见 §12.2，全部为 smoke）。
- 伴生文档：`BASELINE.md`、`DESIGN.md`（初稿，随本文件同刻冻结；其内容是描述性
  的，不承载门槛）。
