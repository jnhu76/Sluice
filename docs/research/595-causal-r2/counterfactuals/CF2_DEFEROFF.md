# CF-2 反事实 `deferoff`：defer 决策链整体关闭的因果裁决（A→干预→A）

状态：ACTIVE EVIDENCE。轮次：#595 causal-r2 反事实阶段（2026-11-04）。
冻结假设、机制预测、性能预测**全部取自 Phase C 冻结文本（`../CAUSAL_DAG.md` §6
CF-2）与任务冻结块，零改动**（含【D】关联级扩展）。本文档 §1–§4 在任何
计时/机制 run 之前定稿；§5+ 为运行后结果，只追加不回改。

前置：CF-0 三臂（`../CF0_THREE_ARM.md`）与 CF-1 sigdrop（`./CF1_SIGDROP.md`）
已完成——CF-1 已使 CPU 增量全部消失（−0.26% [−0.67,+0.16]），G10 停止条件的
"残量 >2.5% → 启 CF-2" 数值上未触发；CF-2 依预注册定义作为独立确认数据运行，
其在本轮的核心裁决价值转为：**延迟侧**（CF-1 后 p50/p99 仍 +4.3/+4.9%，
L1 升级为延迟主因候选——defer 决策链整体关闭后延迟 Δ 是否归零）与
CPU 侧的重复确认（sigdrop 与 deferoff 是两个不同干预点，同一批数据不得
既当探索又当确认；CF-2 构成对 O1→C1/C3 结论的独立第二干预点验证）。

## 1. 冻结假设（原样引用，未改）

- **H（CF-2）**：D2 的全部效应（CPU 与延迟）来自 defer 决策链及其连带通知；
  B 其余代码路径（bounded_progress 状态、其余门、进度语义）不构成可测成本。

## 2. 干预定义（单因素）

- 唯一变化：新增编译期 mutant 宏
  `SLUICE_595_MUTANT_DEFER_DECISION_DROPPED`——`defer_transport_flush_locked_`
  函数体第一可执行语句处直接 `return false`（单点改动；在 `!bounded_progress_`
  早退之前）。deferral、门链内段（owner/payload/witness/cap）、residue 通知的
  成因全部不再触发；flush 回到 dispatch 现场立即 `submit_transport_locked()`。
- 提交语义：B 臂（`SLUICE_595_ARM=B`，`bounded_progress_=true`）构造不变；
  干预后 B 的提交面与 A 逐字节同构（A 臂的等价路径是
  `if (!bounded_progress_) return false;` 入口早退——同一函数、同型早退、
  不同判定源）。`note_progress_driver_locked_()` 等其余状态机不动。
- 构建门控（本轮研究副本新增，仅 xmake 层）：
  `SLUICE_595_MUTANT_DEFEROFF_BUILD=1` → 上述 define
  （`worktree-timing/xmake.lua`、`worktree/xmake.lua`，随 CF-1 sigdrop 门控
  同型追加）；并按 R1 模式新增 mutant oracle target
  `uring_595_mut_defer_decision_dropped`（两 worktree `xmake/tests.lua`）。
- 与 CF-1 sigdrop 的关系：互斥构建宏；sigdrop 砍通知项（保留 deferral），
  deferoff 砍决策成因（deferral 与通知一并消失）。两者不叠加。
- 二进制成对（frozen / deferoff），臂仍由 env `SLUICE_595_ARM` 运行时选择，
  每个计时块内 A/B 同二进制同布局；跨块 mutant-A 与 frozen-A 绝对值对比
  作为布局等价性检查（§7）。mutant 布局风险已在 G10 登记（预注册引用）。

## 3. 机制预测（冻结，Phase B 计数器口径 + CAUSAL_DAG §6 CF-2 原文）

counters build（`SLUICE_595_COUNTERS=1`，D2，非计时 run）：

| 计数器 /op | frozen A | frozen B | deferoff A | deferoff B（预测） |
|---|---|---|---|---|
| `sqes_deferred` | 0 | ≈0.55 | 0（=frozen A） | **0** |
| `wakeups` / `wakeup_writes` / `wakeup_coalesced` | 0/0/0 | ≈0.55/0.25/0.30 | 0/0/0 | **0/0/0**（成因消失） |
| `flushes` / `actual_enters` | 1.0000 | 1.0000 | 1.0000 | **1.0000**（dispatch 现场） |
| `submit_attempts` | ≈1.79 | ≈1.25 | ≈1.79 | **≈1.79（A 形态）** |
| `dispatch_passes` / `sqe_preparations` / `cqes_reaped` | 1.0000 | 1.0000 | ≡frozen A | **1.0000** |
| `wait_attempts` / `poll_passes` | ≈0.54/0.79 | ≈0.55/0.80 | ≡frozen A | **≈A** |
| `progress_handoffs` | 0 | 0 | 0 | **0** |

A 臂 15 计数器在两构建间逐项相同（mutant 对 A 臂零行为差，单因素纯度）。

syscall / 事件面：

- bench 线程 syscw 的 B−A 差（CF-1 frozen 复测 +86,344/inv）→ **0**
  （deferoff-B 与 A 同为 dispatch 现场提交，无 residue 通知成因）。
- C2 shim（LD_PRELOAD，单仪器）：submit 后 <250 ns 即 peek 的比例
  （deferred-flush fraction）→ **0**（poll-pass flush 群体消失）。
- 【D】用户态 7 点环（单仪器）：U.defer → **0**、U.immediate = **100%**、
  U.flush / phase 不变（2000）、FLUSH 直接前驱 = immediate（不再是 defer）。
- 【D】K.submit_req 时间位置回到 submit 现场：root bpftrace 不可达
  （sudo 需密码，与 CF-1 §5.4 同因）→ 预先标记
  UNOBSERVED_ADMIN_UNAVAILABLE；用户环前驱读数承担同一结构问题的等价覆盖。

## 4. 性能预测与判定规则（冻结）

- **P1（CPU）**：deferoff 块内 B−A cpu/op Δ→0（|Δ|<1%，CI95 含 0）。
  与 CF-1 独立互证：两干预点均归零 ⇒ O1 defer 决策链 → C1/C3 因果链的
  CPU 结论闭合于两个不同干预点。若 CF-1 后 CPU 仍 >2.5% 而 CF-2 归零 ⇒
  defer 分支本身/微架构 layout（G10 假设 (b)(c)）承担残差——CF-1 实测
  残量 CI 上界 +0.16% < 2.5%，此分支数值上未触发，本块只做记录核对。
- **P2（延迟，本轮核心裁决）**：deferoff 块内 B−A 的 p50/p95/p99 Δ→0
  （|Δ|<1%）。CF-1 后延迟仍 +4.3/+4.9% 且 L1（deferral 时序：内核工作
  起点推迟一个 poll 周期）升级为延迟主因候选——deferoff 把 deferral 本身
  去掉：(i) 若 p50/p99 随之归零 ⇒ **延迟位移定位于 defer 决策链
  （含 deferral 时序 L1 与连带通知）**，L1 确认为延迟主因；(ii) 若延迟
  仍 >2% ⇒ 延迟位移与 defer 决策链无关，回到 UNOBSERVED 并登记新候选。
- **P3（恢复复验）**：restore 块（frozen）B−A 与 base 块同域重现已冻结
  效应（≈+3.3%），无漂移、无滞后。
- 有效性前提（逐条核验后才允许因果表述）：每 invocation exit 0；ops 恒
  20,000×5 rep/臂；两臂同二进制 SHA；请求语义等价（同一 bench、同一 case、
  同一字节 oracle）。mutant 构建的 595 deferral oracle 预期**设计内拒绝**
  （rc≠0，mutant receipt：证明干预真实到达 defer 语义层——与 R1 门链
  mutant receipts 同型）；其余 12 个 uring target 在 mutant B 臂必须 rc=0。

## 5. 协议（同 CF-1 / phaseA 修复版）

- Timing build：`causal-r2/worktree-timing`（分支 `research-595-r2-timing`，
  = CF-1 后状态 + 本轮 deferoff 门控；无任何 SLUICE_595_* 仪器宏）。
  frozen 二进制 = CF-1 已存档 `bin/bin_timing_frozen`（sha `0091d259…`）。
- 机制 build：`causal-r2/worktree`（Phase B/C 状态 + C 臂 + sigdrop 门控 +
  本轮 deferoff 门控），分别以 `SLUICE_595_COUNTER_BUILD=1` /
  `SLUICE_595_TRACE_BUILD=1`（可叠 `SLUICE_595_MUTANT_DEFEROFF_BUILD=1`）
  构建；frozen 机制二进制复用 CF-1 存档。机制 run 不与计时 run 混块。
- 计时：`pair_runner.py`，D2 case `W02-uring-4k-d2`
  （`SLUICE_BENCH_595_DEPTHS=1`），`taskset -c 2`，WARMUP=1 MEASURED=5
  ×20,000 op，`SLUICE_BENCH_STATS=1`，tmpfs `/tmp/sluice-595-bench`，
  even 对 A→B / odd 对 B→A，exit 0 硬门；每块 12 对（CAUSAL_DAG §6 冻结
  样本量）；统计 = 配对相对差 (B−A)/A（tput 反向）、bootstrap 10,000、
  seed 5950、percentile CI。
- 块序（A→干预→干预重复→A 恢复，串行独占）：`ab-d2-base`(frozen) →
  `ab-d2-deferoff`(mutant) → `ab-d2-deferoffrep`(mutant) →
  `ab-d2-restore`(frozen)；块间 ≥45 s 冷却；loadavg 起始守卫 1.5
  （自身残荷被拒则按 phaseA/CF-0 先例调 4.0 并记录）。
- 机制 companion（独立于计时块）：counters（每 cell 3 inv × 3 rep × 20k op）、
  7 点环（E2 同参数 2000 op × 1 phase × 3，单仪器）、sysio
  （`/proc/<tid>/io` 差分，6 对 × 2 臂 × 2 构建）、C2 shim（LD_PRELOAD，
  deferoff-B 与 frozen-B 各 2 inv × 3 rep，deferred-flush fraction）。

---

## 6. 执行记录（全部先于结果解读完成）

### 6.1 构建与 provenance（`raw/meta-cf2/`）

| 项 | 值 |
|---|---|
| timing worktree | `causal-r2/worktree-timing`（分支 `research-595-r2-timing`，= CF-1 后状态 + 本轮 deferoff 门控）；全 diff sha256 `b11ede03…`（`raw/meta-cf2/timing-worktree-full-cf2.diff`）；与 CF-1 存档 diff 逐 hunk 对比：差异恰为本轮 3 处（源码 `#if` 块、`xmake.lua` 门控、`tests.lua` target），无其他漂移 |
| 仪器 worktree | `causal-r2/worktree`（+ 本轮门控）；全 diff sha256 `f939602f…`（`raw/meta-cf2/instrumented-worktree-full-cf2.diff`） |
| `bin_timing_frozen` | `0091d259…`（= CF-0/CF-1 存档，复用；计时块经 `raw/sysio-cf2/bench_w02_pipeline_frozen` 规范名副本运行，sha 核验一致） |
| `bin_timing_deferoff` | `64189aa8…`（`SLUICE_595_MUTANT_DEFEROFF_BUILD=1 xmake f -c -y --liburing=y -m release && xmake build bench_w02_pipeline`；经 `bench_w02_pipeline_deferoff` 副本运行） |
| `bin_counters_deferoff` / `bin_trace_deferoff` | `ed832d97…` / `66b2d0dd…`（worktree，叠 `SLUICE_595_COUNTER_BUILD=1` / `SLUICE_595_TRACE_BUILD=1`）；frozen 机制二进制复用 CF-1 存档（`601dad6a…` / `1b131f4f…`） |
| C2 shim | 源码自 `evidence/traces/raw-c2/c2_liburing_shim.c`（只读）复制到 `/tmp/c2-shim2/` 编译（gcc -O2 -fPIC -shared -ldl），与 C2 存档分析逻辑（`shim_fractions.py`）逐函数一致 |
| 工具链/环境 | g++ 16.2.1、xmake 3.1.2+HEAD、liburing-devel 2.13-2、Python 3.14.8、kernel 7.2.11-200.fc44、cpu2 schedutil（idle 1.2 GHz）、perf_event_paranoid=2（`raw/meta-cf2/toolchain-env.txt`） |
| **构建复现性检查（如实记录）** | stash 本轮改动后于 CF-1 状态 frozen 配置重建 timing bench = `40b8ed8a…` ≠ CF-0 存档 `0091d259…`（同大小，cmp 差 186,825 字节）——**timing worktree 的 frozen 构建在 CF-0→CF-1 之间即不可字节复现**（本轮改动前已存在），非 deferoff 引入；本轮改动自身再引入 `40b8ed8a`→`4c014266`（未激活宏块的构建面差异，cmp 40,169 字节量级同域）。布局风险按协议由块内同二进制配对 + 跨块 A 臂绝对值检查承担（§7），并按 G10 登记 |
| 主工作树 | `/home/jnhu/Source/Sluice` clean 未触碰；R1 evidence、CF-0/CF-1 evidence 与 `MANIFEST.sha256` 未触碰 |

### 6.2 正确性 oracle（`raw/correctness/`，2026-11-04）

| lane | 结果 | 备注 |
|---|---|---|
| BASE GREEN（frozen 配置 `uring_595_bounded_progress_test`，arm B） | **all checks passed rc=0** | 本轮源码改动未破坏 frozen 路径 arm-B 语义（`mutant-receipts-cf2.log`） |
| MUTANT receipt（frozen 配置 `uring_595_mut_defer_decision_dropped`，arm B） | **22 项 FAIL + 公共绑定协议不可达不变量 fail-fast，rc=134** | R1 门链 mutant receipts 同型且更强：oracle 杀死 mutant ⇒ 干预真实到达 defer 语义层 |
| mutant 配置 13 uring target B-mode lane | **12/13 rc=0 + 595 test rc=134（设计内拒绝）** | `bmode-mutant-suite-cf2.log`；与 §4 预期逐项一致 |
| frozen BASE 13/13 lane | 复用 CF-0 `three_arm/correctness/bmode-suite.log`（同一 frozen 二进制 `0091d259…`） | CF-1 同款复用口径 |
| bench 字节 oracle | 计时/机制全部 invocation exit 0（硬门），每 invocation 恰 100,000 measured op | §7 workload 等价核验 |
| R1 sanitizer-lane | **本轮未运行**（CF-0/CF-1 同款标注） | 如实标注 |

### 6.3 机制计数器（`raw/cnt-companion-cf2/`，counters build，D2，每 cell 3 inv × 3 rep × 20k op，全 exit 0）

| 计数器 /op | frozen A | frozen B | deferoff A | deferoff B | §3 预测 | 判定 |
|---|---|---|---|---|---|---|
| `sqes_deferred` | 0 | 0.5482 | 0 | **0.0000** | →0 | ✓ |
| `wakeups`/`wakeup_writes`/`wakeup_coalesced` | 0/0/0 | 0.5482/0.2535/0.2947 | 0/0/0 | **0/0/0** | →0 | ✓ |
| `flushes` / `actual_enters` | 1.0000 | 1.0000 | 1.0000 | **1.0000/1.0000** | 1.0000 | ✓ |
| `submit_attempts` | 1.7894 | 1.2537 | 1.7885 | **1.7901** | ≈1.79（A 形态） | ✓ |
| `wait_attempts` / `poll_passes` | 0.5409/0.7894 | 0.5483/0.8019 | 0.5366/0.7885 | **0.5389/0.7901** | ≈A | ✓ |
| `dispatch_passes`/`sqe_preparations`/`cqes_reaped`/`progress_handoffs` | 1/1/1/0 | 1/1/1/0 | 1/1/1/0 | **1/1/1/0** | 不变 | ✓ |

A 臂 parity：确定性计数器逐项差 0.0000；`submit_attempts`/`wait`/`poll` 差 ≤0.0043/op（A/A 统计噪声内）⇒ mutant 对 A 臂零行为差，单因素纯度成立。`per_op_summary.json` 为机读源。

### 6.4 syscall 面（`raw/sysio-cf2/`，timing 二进制、`/proc/<tid>/io` 差分，6 对 × 2 臂 × 2 构建；comm 截断核验 `bench_w02_pipel`/`iou-wrk-*`）

| 量（bench 线程） | frozen B−A | deferoff B−A | 预测 | 判定 |
|---|---|---|---|---|
| syscw /inv | **+88,768.5**（B 88,060–89,714 vs A 70；复现 CF-1 +86,344 同域） | **+0.3**（5 对 +0、1 对 +2） | →0 | ✓ |
| utime/stime（tick） | +24/+21 ms | +10/−22 ms | — | OBSERVABILITY_LIMIT（10 ms tick 无分辨率，CF-1 同款）；syscw→0 为直接观测 |

### 6.5 用户态 7 点环（`raw/trace-companion-cf2/`，单仪器，E2 同参数 2000 op × 3 phase，两构建各 1 次 B 臂 run，dropped=0）

| 量 | frozen B | deferoff B | 【D】预测 | 判定 |
|---|---|---|---|---|
| U.defer 比例 | 0.5325/0.5215/0.5275 | **0/0/0** | →0 | ✓ |
| U.immediate | 935/957/945 | **2000/2000/2000（100%）** | 100% | ✓ |
| U.flush / phase | 2000×3 | 2000×3 | 不变 | ✓ |
| FLUSH 直接前驱=defer / =immediate | 1065/1043/1055 / 余 | **0 / 2000×3** | 前驱不再是 defer | ✓ |

【D】K.submit_req（root bpftrace）：**UNOBSERVED_ADMIN_UNAVAILABLE**（sudo 需密码，CF-1 §5.4 同因）；"submit 现场恢复"由用户环 FLUSH 前驱=immediate 与 §6.6 shim 时点等价覆盖。

### 6.6 C2 shim 时点（`raw/shim-companion-cf2/`，LD_PRELOAD 单仪器，每 cell 2 inv × 3 rep × 20k submit 事件）

| 量 | frozen | deferoff | 预测 |
|---|---|---|---|
| fast-follow（submit→peek <250 ns）B 臂 | 31.58% | 6.85% | — |
| A 臂背景 | 5.45% | 6.94% | — |
| **B−A 超额（poll-pass flush 群体）** | **+26.13pp** | **−0.09pp** | **→0 ✓ 精确归零** |

绝对 6.85% 与 A 臂背景（5.45%/6.94%）同域 ⇒ 剩余量为 A 形态近同步完成背景，deferred poll-pass 吸收群体消失。`analysis_arms.json` 为机读源。

### 6.7 计时（frozen 协议臂；四块串行独占，块间 ≥45 s，96 invocation 全部 exit 0、全部恰 100,000 measured op、每块 12 对 ABBA、loadavg 峰 1.39 < 1.5）

块序 `ab-d2-base-cf2`(frozen) → `ab-d2-deferoff`(mutant) → `ab-d2-deferoffrep`(mutant) → `ab-d2-restore-cf2`(frozen)。原始数据 `raw/ab-d2-*-cf2*/`（results.jsonl/header.json/logs/jsonl），分析 `raw/timing-analysis-cf2*.json`。

## 7. 结果（bootstrap 10,000 / seed 5950 / percentile；正 = B 更差，tput 正 = B 更慢）

### 7.1 分块与合并（B−A）

| 块 | 二进制 | CPU/op | p50 | p95 | p99 |
|---|---|---|---|---|---|
| base | frozen `0091d259` | **+3.86% [+2.38,+5.40]** | +7.46 [+5.35,+9.68] | +5.42 [+2.80,+7.92] | +6.81 [+1.08,+12.19] |
| deferoff | mutant `64189aa8` | **−0.47% [−1.63,+0.67]** | +0.52 [−2.45,+5.17] | −0.95 [−1.85,−0.03] | −1.04 [−2.21,+0.05] |
| deferoffrep | mutant | **−1.04% [−2.26,+0.09]** | −1.94 [−3.74,−0.37] | −0.79 [−2.17,+0.40] | −0.62 [−2.95,+1.09] |
| restore | frozen | **+3.94% [+2.60,+5.26]** | +5.05 [+3.38,+7.46] | +4.95 [+3.50,+6.38] | +5.25 [+3.16,+7.21] |
| **合并 frozen（24 对）** | | **+3.90% [+2.91,+4.91]** | **+6.26% [+4.77,+7.92]** | **+5.19% [+3.72,+6.63]** | **+6.03% [+2.99,+9.02]** |
| **合并 deferoff（24 对）** | | **−0.76% [−1.61,+0.06]** | **−0.71% [−2.57,+1.90]** | **−0.87% [−1.71,−0.10]** | **−0.83% [−2.12,+0.23]** |

绝对 cpu/op（ns）：frozen 块 A 4708.5/4687.1、B 4887.9/4869.9；mutant 块 A 4664.1/4641.0、B 4641.4/4591.0。布局等价性检查：mutant-A 比 frozen-A 低 44.4/46.1 ns/op（0.94%/0.98%，两块方向一致）——量级略超 CF-1 先例（<0.7% 方向不定），成因三选一不可分（mutant 二进制布局 cmp 差 40,169 字节 / frozen 块间自身漂移 21 ns / mutant-A 门函数少一次成员加载），按 G10 风险如实登记；**不进入任何配对结论**（所有对比均为块内同二进制配对）。

### 7.2 消失比例（frozen 合并 → deferoff 合并）

| 指标 | 点估计 | 保守口径（deferoff CI 上界） |
|---|---|---|
| CPU/op | 119.4% | **≥98.6%** |
| p50 | 111.4% | ≥69.7% |
| p95 | 116.7% | ≥101.9% |
| p99 | 113.8% | ≥96.2% |

## 8. 裁决（对照 §4 冻结规则）

1. **P1（CPU）命中：H 的 CPU 部分成立**。deferoff 干预下 B−A cpu/op =
   **−0.76% [−1.61,+0.06]**（|mean|<1%，CI 含 0）vs 同会话 frozen 合并
   +3.90% [+2.91,+4.91]。与 CF-1 sigdrop（−0.26% [−0.67,+0.16]）构成
   **两个不同干预点的独立互证**：砍通知成因（sigdrop）与砍决策成因
   （deferoff）均使 CPU 差归零 ⇒ D2 CPU 效应的因果源=defer 决策链（含
   连带通知），"其余代码路径不构成可测成本"（冻结假设 B 命中：mutant-B
   与 mutant-A 提交面逐计数器相同且 CPU 差 CI 含 0）。G10 竞争假设
   (b)(c)（defer 分支本身/微架构 layout 残差）**不触发**：CF-1 残量 CI
   上界 +0.16% < 2.5%，本块残量 CI 上界 +0.06% < 1%。
2. **P2（延迟）命中分叉 (i)：延迟位移定位于 defer 决策链，L1 确认为延迟主因**。
   CF-1（保留 deferral、去通知）后 p50 +4.30/p99 +4.87 仍显著；本块（去
   deferral）p50 −0.71 [−2.57,+1.90]、p99 −0.83 [−2.12,+0.23] 全部归零。
   两反事实联合裁决：延迟位移随 deferral 消失而消失、不随通知消失而消失
   ⇒ **主因=deferral 时序（L1：内核工作起点推迟一个 poll 周期），非通知
   （L0）**。延迟侧的裁决路径就此闭合（无需再启 G12 段级直方图——效应
   已整体定位到单一决策；若后续需段内分解另议）。
3. **P3（A→干预→A 恢复复验）通过**：restore 块 +3.94% [+2.60,+5.26] 精确
   重现 base 块 +3.86% [+2.38,+5.40]；机制计数器、7 点环、shim 均含
   frozen 对照同构恢复（§6.3–6.6）。无漂移、无滞后。
4. **证据等级：反事实支持（配对干预 + 单因素机制读数四路闭合 + oracle
   通过 + 恢复复验）→ 严格因果证明**。有效性前提逐条核验：96 invocation
   exit 0；每 invocation 恰 100,000 measured op；每块内 A/B 同二进制 SHA；
   同一 bench/case/字节 oracle。请求语义等价的边界如实陈述：deferoff 不
   满足 arm-B held-batch 进度契约（595 oracle 设计内拒绝），故结论限定于
   无 held-batch 停滞场景的 W02-4k-d2 工作负载形态——该形态下 12 个非 595
   uring oracle 在 mutant B 臂全部通过。
5. 过度消失（点估计 >100%）：mutant-B 比 mutant-A 净快 ≈0.76%，同 CF-1
   §7.4 先例（poll 合并形态残留的微小优势在噪声半宽边缘），CI 含 0，
   不单独归因，仅记录。

## 9. 限制与如实标注

- K.submit_req（root bpftrace 时点观测）UNOBSERVED_ADMIN_UNAVAILABLE；
  由用户环 FLUSH 前驱 + shim 时点等价覆盖（§6.5/6.6）。
- utime/stime tick 差分无分辨率（§6.4）；"内核侧增量→0"由 syscw 差→0
  直接观测 + CPU/op 差→0 共同支撑，非 stime 直接读数。
- frozen timing 构建不可字节复现（§6.1：CF-0→CF-1 间即存在，186k 字节
  级差异）；mutant-A 绝对值低 ~1% 方向一致，成因不可分（布局/漂移/少一次
  加载），G10 登记，不进入配对结论。
- p50 块间波动（+7.46/+0.52/−1.94/+5.05）大于 CPU；裁决使用 24 对合并
  口径（CF-1 同款纪律）。
- R1 sanitizer lane 与全 mutant-receipts lane 本轮未重跑（CF-0/CF-1 同款
  标注）；本轮正确性面 = BASE GREEN + 本 mutant receipt + 12/13 B-mode
  lane + 字节 oracle。
- 机制读数为研究仪器构建（counters/trace/shim），不进入性能归因；性能
  读数全部取无仪器 frozen 协议臂（仪器 run 与计时 run 未混块）。
