# CF-0 基础反事实三臂（A=IMMEDIATE / B=BOUNDED_PROGRESS / C=GATE_ONLY_IMMEDIATE）

状态：ACTIVE EVIDENCE。轮次：#595 causal-r2 反事实阶段（2026-11-04）。
本文档 §1–§5 在任何计时/机制 run 之前冻结（写完后才开始构建与运行）；
§6+ 为运行后结果，只追加不回改。冻结输入：

- Phase A 协议与结果：`evidence/raw/phaseA/`（`PROTOCOL.md` 同目录）。
- Phase B 机制计数器与 D2 账本：`counters/cost_table.md` §3.2（sqes_deferred
  0→0.5589/op、submit_attempts 1.7893→1.2354、flushes/actual_enters 1.0000 恒定）。
- C1 in-situ 门成本：`counters/C1_COUNTERS.md`（`defer_transport_flush_locked_`
  A 10.2 ns/call ≈ 空 TSC pair 9.9（实际早退≈0）；B 16.8 ns/call；增量 +6.6 ns/call，
  1.0 call/op）。
- R1 预注册门槛（只读）：`/home/jnhu/sluice-595-e5/evidence/measurement/PREREGISTERED.md`。

## 1. 研究问题与冻结假设

问题：**B（BOUNDED_PROGRESS）的准入判断（`defer_transport_flush_locked_` 五门链：
bounded→owner→payload(read,4096)→witness→cap）本身是否构成显著 CPU/尾延迟成本？**

- H0（待否证）：准入判断链本身的成本 ≤ 测量噪声（D2 上按 C1 直接测量 ≈ +6.6 ns/op
  ≈ +0.14%，远低于 phaseA A/A CPU 半宽 0.65 pp）。
- H1：准入判断链贡献了 B·D2 退化（+3.70% CPU/op）的可观份额（≥1 pp）。

判定变量：C−A（同一 trio 内）的 cpu/op、tput、p95、p99 配对相对差。

## 2. 干预定义（单因素）

- A = IMMEDIATE：冻结基线（`bounded_progress_=false`，门在第一检查早退）。
- B = BOUNDED_PROGRESS：现行研究臂（门链全跑，通过则 defer flush）。
- C = GATE_ONLY_IMMEDIATE（本轮新增，仅研究副本）：`bounded_progress_=true` 使门链
  与 B 完全相同地执行（owner 比较、`prepared_ops_` 载入、`live_cookies_` 载入、
  ledger size、cap 比较），但在全部门之后强制 `return false`（在 `sqes_deferred`
  计数之前），提交路径与 A 完全一致（dispatch 点立即 `submit_transport_locked()`）。
- 单因素纯度：C 与 B 的唯一差异 = defer 判定的最终动作（true→false）；C 与 A 的
  唯一差异 = 门链执行（A 在 `!bounded_progress_` 早退）。C 不改任何其他状态：
  `bounded_progress_` 仅被 `defer_transport_flush_locked_` 读取（源码核验，
  `src/async/uring_transport.cpp:248` 唯一引用）；`note_progress_driver_locked_()`
  在两臂均无条件执行。
- 实现位置：仅研究副本（`causal-r2/worktree` 与本次新建 timing worktree），
  不改公开 API / v2 契约 / 主工作树。
- 三臂共用同一二进制，`SLUICE_595_ARM={A,B,C}` 运行时选择 —— 无二进制布局混淆。

## 3. 机制预测（冻结，Phase B 计数器，counters build + SLUICE_595_COUNTERS=1）

D2（`W02-uring-4k-d2`；A/B 预测值直接取 Phase B `cnt-on-full-d2` 实测）：

| 计数器 /op | A（Phase B 实测） | B（Phase B 实测） | C（本轮预测） |
|---|---|---|---|
| `sqes_deferred` | 0 | 0.5589 | **0**（=A） |
| `wakeups` / `wakeup_writes` / `wakeup_coalesced` | 0/0/0 | 0.5589/0.2353/0.3236 | **0/0/0**（=A） |
| `flushes` / `actual_enters` / `cqes_reaped` / `sqe_preparations` / `dispatch_passes` | 1.0000 | 1.0000 | **1.0000**（=A=B） |
| `submit_attempts` | 1.7893 | 1.2354 | **≈A（1.79）**，绝不取 B 形态 |
| `wait_attempts` / `poll_passes` | 0.5513 / 0.7893 | 0.5590 / 0.7942 | **≈A** |

D16（`W02-uring-4k-d16`；Phase B 未跑过计数器，预测为组内相对）：C 的全部 15 个
计数器与同 run 的 A 一致（不变量为确定性相等；统计性计数器在 A/A 噪声内），
B 的 `sqes_deferred`>0。

C1 build（`SLUICE_595_C1=1`，D2）直接测量（追加验证，若运行）：
`defer_owner_pass`：A 0 → C ≈1.0/op（=B）；`defer_witness_pass`：A 0 → C >0；
`Agg::defer` 每次 TSC：A ≈10.2、B ≈16.8、**C ≈16–18 ns/call**（链执行、动作取消）。

## 4. 性能预测与判定规则（冻结）

计时构建 = 无仪器 timing build（见 §5）。配对单位 = trio（A、B、C 各一次新鲜
invocation，背靠背，顺序轮转）。

- **P1（核心）**：D2 的 C−A cpu/op 点估计 ≈ +0.1~+0.2%（来自 C1 +6.6 ns/op），
  CI95 预期含 0，且 |mean| < 0.65 pp（phaseA A/A CPU 半宽）。
  - 判定“准入判断显著有成本” ⟺ C−A cpu mean ≥ +0.65 pp **且** CI95 下界 > 0。
  - 否则：H0 不被否证 —— 准入判断成本在噪声内（不显著）。
- **P2**：D2 的 B−C cpu/op ≈ +3.5%（B 退化的主体随 defer 动作留在 B 侧）。
- **P3**：D16 的 C−A 在噪声内（|mean| < 2 pp 或 CI 含 0），B−A ≈ −26%（复现
  phaseA）；若 C 未复现 B 的收益 → 收益来自 defer 动作而非门判断。
- **P4（恢复复验）**：三臂轮结束后 A/A 12 对（D2）：cpu/tput/p99 CI 半宽 ≤
  phaseA 门（5/3/3 pp），mean 与 0 一致（含 A/A 二位偏置 ~+0.7%）。
- 有效性前提（逐条核验后才允许因果表述）：每 invocation exit 0；ops 恒 20,000×5
  rep/臂；三臂同一二进制 SHA；请求语义等价（同一 bench、同一 case、同一 oracle）。

## 5. 协议（同 Phase A，仅配对单位由 pair 扩展为 trio）

- Timing build：新 worktree `causal-r2/worktree-timing`（`271b81bf` + R1
  `patch.diff` 原样 + 本轮 C 臂补丁；无任何 SLUICE_595_* 仪器宏）。
  A/B 行为与 phaseA 冻结二进制同源（仅差 C 臂三分支）。
- 机制 build：`causal-r2/worktree`（现有 Phase B 状态 + C 臂补丁）分别以
  `SLUICE_595_COUNTER_BUILD=1` / `SLUICE_595_C1_BUILD=1` 构建。
  机制 run 与计时 run 不混块；机制 run 不进入性能归因。
- 每 invocation：`taskset -c 2`，`SLUICE_BENCH_WARMUP=1 SLUICE_BENCH_MEASURED=5
  SLUICE_BENCH_CASE_FILTER=<exact> SLUICE_BENCH_STATS=1`，tmpfs
  `/tmp/sluice-595-bench`，exit 0 硬门。D2/D2rep 需 `SLUICE_BENCH_595_DEPTHS=1`。
- Trio 顺序轮转：i%3=0→A,B,C；1→B,C,A；2→C,A,B（24 trio = 每臂每位置 8 次）。
- 块序（全部串行、独占机器）：D2 24 trio → D16 24 trio → D2rep 12 trio（重复确认）
  → AA 12 对（恢复复验，arm 顺序 A,A）。
- 聚合（与 phaseA 相同）：成员 = Σcpu/Σops、Σops/Σwall、五 rep 分位数算术均；
  trio 内相对差 (C−A)/A、(B−A)/A、(B−C)/C（tput 反向）；bootstrap 10,000、
  seed 5950、percentile CI。
- 元数据：源 SHA、diff sha256、二进制 sha256、内核/工具版本、affinity、频率与
  loadavg（每 invocation 前后）、运行命令、样本量、块序、原始输出路径。
- 环境纪律：schedutil 1.2–3.5 GHz；run 前 loadavg 守卫（起始 1.5，若因自身残荷
  被拒则按 phaseA 先例调 4.0 并记录）；计时期间无其他负载。

---

## 6. 执行记录（全部先于结果解读完成）

### 6.1 构建与 provenance

| 项 | 值 | 证据 |
|---|---|---|
| timing worktree | `causal-r2/worktree-timing`，分支 `research-595-r2-timing`，基 `271b81bf` + R1 `patch.diff`（sha `7be9d014…` 原样）+ C 臂补丁 | `git worktree list`；roundtrip 验证：R1 状态 + `raw/c-arm-timing.patch` = worktree 三文件字节一致 |
| C 臂补丁 | 3 hunk / 3 文件（构造器 arm 解析、成员 `gate_only_immediate_`、门尾强制 return false），timing 版 `raw/c-arm-timing.patch` sha `d0848840…`；counters 版（多 SLUICE595_BUMP 行上下文）`raw/c-arm-counters.patch` | `sha256sum` |
| timing 二进制 | `bench_w02_pipeline` sha `0091d2592242f605b5c64b11f22faa8e8b9e9b729146f93f263ba1288118f81c`（无 SLUICE_595_* 仪器宏）；worktree diff sha `2ee6ea0d…` | header.json；`binary-sha256.txt` |
| counters 二进制 | `evidence/counterfactuals/three_arm/bin_bench_counters` sha `601dad6a…`（主 worktree diff sha `11ab3a98…`，`SLUICE_595_COUNTER_BUILD=1`） | 同上 |
| C1 二进制 | `evidence/counterfactuals/three_arm/bin_bench_c1` sha `a5c437f0…`（`SLUICE_595_C1_BUILD=1`） | 同上 |
| 工具链/内核/频率 | kernel 7.2.11-200.fc44、g++ 16.2.1、xmake 3.1.2+HEAD、liburing-devel 2.13-2、python 3.14.8；cpu2 schedui，频率快照 `cpu-freq-during.txt` / `cpu-freq-after.txt` | `toolchain.txt` |
| 主工作树 | `/home/jnhu/Source/Sluice` 未改动（clean）；R1 evidence 未触碰 | git status |

### 6.2 正确性 oracle（R1 复用套件，timing build，2026-11-04）

| lane | 结果 | 备注 |
|---|---|---|
| A-mode（env 未设，80 targets） | **80/80 rc=0** | 595 测试在 env 缺省时自选 arm B 并通过（R1 的 amode 是显式 arm A → rc=2 拒绝；口径差异记录于此） |
| A-explicit（13 uring targets，`SLUICE_595_ARM=A`） | 12/13 rc=0 + 595 rc=2（设计内拒绝） | `counterfactuals/three_arm/correctness/amode-explicit-suite.log` |
| B-mode（13 uring，`=B`） | **13/13 rc=0** | 与 R1 bmode-suite 一致 |
| C-mode（13 uring，`=C`） | **12/13 rc=0 + 595 rc=2（设计内拒绝）** | `counterfactuals/three_arm/correctness/cmode-suite.log`；与 A-explicit lane 逐行等价（唯一 diff 是 `uring_request_core_integration_test` 首行 INFO 措辞，arm A 复跑 4 次两种措辞都出现 → 测试自身日志时序非确定性，非臂差异；该测试 A/C 全量输出 diff 为空） |
| counters build 595 oracle | rc=0 all checks passed | `SLUICE_595_ARM=B` |
| R1 的 sanitizer-lane 与 mutant-receipts | **本轮未运行**（R1 存档；C 臂不改任何 595 oracle 覆盖的 arm-B 语义） | 如实标注 |

### 6.3 机制计数器（Phase B counters build，`SLUICE_595_COUNTERS=1`，非计时 run）

> 记录透明度：`cnt-d2` 首次尝试因 runner 未捕获 stderr（计数器行仅出现在会话终端）
> 在分析前整目录删除并重跑（runner 随即补上 fd2 捕获，计时 run 均带 .err 捕获）；
> 被删块未进入任何分析。计时块无此类重跑。

D2（`cnt-d2`，4 trio × 3 臂 × 5 rep × 20k op = 每臂 20 个 rep 样本）：

| 计数器 /op | A | B | C | §3 预测 C |
|---|---|---|---|---|
| `sqes_deferred` | 0.0000 | 0.5484 | **0.0000** | 0 ✓ |
| `wakeups`/`wakeup_writes`/`wakeup_coalesced` | 0/0/0 | 0.5484/0.2520/0.2964 | **0/0/0** | 0 ✓ |
| `flushes`/`actual_enters`/`cqes_reaped`/`sqe_preparations`/`dispatch_passes` | 1.0000 | 1.0000 | **1.0000** | ✓ |
| `submit_attempts` | 1.7882 | 1.2521 | **1.7896** | ≈A(1.79) ✓ |
| `wait_attempts` / `poll_passes` | 0.5397/0.7882 | 0.5485/0.8005 | **0.5416/0.7896** | ≈A ✓ |

D16（`cnt-d16`，同结构）：A `flushes`/`actual_enters`=1.0000/op；**B `sqes_deferred`=0.8767/op、
`flushes`/`actual_enters`=0.1997/op（≈5 SQE/enter 批量）、`submit_attempts`=0.2549**；
**C = A（0 deferred，1.0000/1.0000，submit_attempts 1.1476 vs A 1.1487）**。
→ 首次取得的 D16 计数器基线；B 的 D16 收益机制 = enter 批量化（0.2 vs 1.0 enter/op）。

C1 直接测量（`c1-d2`，2 trio × 3 臂，每臂 200,000 次门调用，`SLUICE_595_C1=1`）：
`defer` scope clean 均值 A 29.20 / B 53.61 / **C 52.96 ns/call**；
`defer_owner_pass` A 0 / B 0.9999 / **C 0.9999**/call；`defer_witness_pass` A 0 / B 0.5602 /
**C 0.5545**/call → **C 执行了与 B 相同的完整门链，且门调用成本与 B 无差**；
（本轮探针集为全 6 scope，绝对值高于 C1_COUNTERS 的最小探针集 10.2/16.8 ns，
同构建内对比有效，跨构建绝对值不可比。）

### 6.4 计时运行（timing build `0091d259…`，机器独占，全部 exit 0）

块序：`abc-d2`(24 trio) → `abc-d16`(24 trio) → `abc-d2rep`(12 trio) → `aa-d2`(12 对)。
首块后 `abc-d16` 一次被 loadavg 1.50>1.5 起始守卫拒绝（前块 72 invocation 残荷，
phaseA 偏差 3 同例）；按先例 `--max-loadavg 4.0` 续跑，实测每 invocation 1-min
loadavg 峰值 1.74（20 核）。trio 顺序 ABC/BCA/CAB 轮转（每臂每位置 8 次）；
每 invocation warmup=1 measured=5 × 20,000 op；三臂 workload 等价性核验：
每臂每 invocation 恰 100,000 measured op、单二进制 SHA、全部 exit 0。

## 7. 结果（bootstrap 10,000 / seed 5950 / percentile；正 = 前者更差，tput 正 = 前者更慢）

### 7.1 D2 主块（24 trio，`abc-d2`）

| 对比 | CPU/op | tput | p95 | p99 |
|---|---|---|---|---|
| **C vs A** | **−0.52% [−1.28, +0.22]** hw 0.75pp | −0.55% [−1.32, +0.20] | −0.70% [−1.62, +0.19] | −0.40% [−2.79, +1.81] |
| B vs A | **+3.21% [+2.43, +4.02]** | +3.08% [+2.34, +3.83] | +4.09% [+3.27, +4.90] | +5.54% [+3.32, +7.64] |
| B vs C | **+3.78% [+2.94, +4.66]** | +3.59% [+2.81, +4.41] | +4.85% [+4.12, +5.60] | +6.09% [+4.27, +7.94] |

绝对值：A 4692.3 / C 4666.4 / B 4841.8 ns/op → C−A = −25.9 ns（CI 上界 +0.22%
≈ **+10.3 ns/op**）；B−A = +149.5 ns；B−C = +175.4 ns。

### 7.2 D16 主块（24 trio，`abc-d16`）

| 对比 | CPU/op | tput | p95 | p99 |
|---|---|---|---|---|
| **C vs A** | **−0.13% [−1.45, +0.82]** | −0.22% [−1.72, +0.81] | −0.21% [−1.37, +0.80] | −0.88% [−2.90, +1.07] |
| B vs A | **−26.80% [−27.73, −26.17]** | −36.80% | −31.97% | −30.17% |
| B vs C | −26.70% [−27.00, −26.40] | −36.51% | −31.82% | −29.53% |

绝对值：A 2895.7 / C 2889.6 / B 2118.1 ns/op。C 未复现 B 的收益（与 §3/§4 预测一致）。

### 7.3 D2 重复确认（12 trio，`abc-d2rep`）

C vs A cpu **+0.06% [−0.52, +0.64]**；B vs A **+3.36% [+2.74, +3.94]**；
B vs C +3.31% [+2.62, +4.02]。方向与主块一致。

### 7.4 A→干预→A 恢复复验（12 对，`aa-d2`）

cpu −0.22% [−1.78, +1.15] hw 1.47pp；tput −0.30% hw 1.51pp；p99 +1.09% hw 2.95pp
→ 三门（≤5/≤3/≤3 pp）**PASS**；三臂干预后 A 与 A 无漂移。

## 8. 判定（对照 §4 冻结规则）

- **准入判断本身无显著成本（H0 不被否证；直接测量支持其量级为个位数 ns/op）**：
  D2 C−A cpu mean −0.52% < +0.65 pp 且 CI 含 0；D16 C−A −0.13% CI 含 0；重复块
  +0.06% CI 含 0。判定规则要求的"mean ≥ +0.65pp 且 CI 下界 > 0"在两个形状、
  三块数据中均不成立。C−A 的 95% CI 上界 +0.22%（≈+10.3 ns/op）与 C1 直接
  测量的门链增量 +6.6 ns/call（最小探针集）同量级 —— 反事实与 in-situ 测量互证。
- **B·D2 退化全部由 defer 动作承担**：B−A +3.21% 而 C−A ≈0 → 门链份额 <0.22%
  （CI 上界）；B−C +3.78% 即动作份额。机制计数器证明 C 与 A 的提交面完全一致
  （submit_attempts 1.79 形态、0 deferral、0 wakeup）。
- **B·D16 收益全部由 defer 动作（批量提交）承担**：C−A ≈0 而 B−A −26.80%；
  计数器显示 B 0.1997 enter/op vs A/C 1.0000（≈5 SQE/enter）。准入判断不产生
  收益也不产生成本。
- 证据等级：**反事实支持（配对干预，单因素，机制计数器闭合，正确性 oracle 通过，
  恢复复验通过）**。三臂同一二进制、同 workload、逐 invocation exit 0 + 字节
  oracle —— 请求语义与工作量等价性成立，本结果可作为该问题上的严格因果证明。
- 保留限制：(i) p50/p99 的 C−A CI 较宽（hw 2.3–5.6pp），只在 CPU/op 与 p95 上有
  分辨率；(ii) 门链成本在 D1（更早失败于 witness）未单独计时——D1 上 B 本身就
  近似 C（phaseA D1 中性 −0.07% 与本结论一致）；(iii) R1 sanitizer/mutant lane
  未在本轮重跑（见 §6.2）。
