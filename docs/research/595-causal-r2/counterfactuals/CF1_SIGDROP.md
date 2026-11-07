# CF-1 反事实 `sigdrop`：O1 residue 通知的因果裁决（A→干预→A）

状态：ACTIVE EVIDENCE。轮次：#595 causal-r2 反事实阶段（2026-11-04）。
冻结假设、机制预测、性能预测**全部取自 Phase C 冻结文本、零改动**
（`../CAUSAL_DAG.md` §6 CF-1 + 任务冻结块，包括【D】关联级扩展）。本文档
§1–§4 在任何计时/机制 run 之前定稿；§5+ 为运行后结果，只追加不回改。

## 1. 冻结假设（原样引用，未改）

- **H（CF-1）**：O1 的 residue 通知（`advertise_residue` 因 deferral 成立 →
  `signal_ready_progress()`）是 D2 CPU 增量的主因（≥60% 消失）；
  deferral 本身与 flush 时点不变。
- **机制预测**：`wakeups` 0.5589→0、`wakeup_writes` 0.2353→0；bench 线程
  syscw 的 B−A 差（+86,912/inv）→0；内核 stime 增量 +79.37 ns/op→≈0；
  `sqes_deferred` 仍 0.5589；`flushes`/`actual_enters` 仍 1.0000/op；
  perf `__GI___libc_write` 符号份额回落为 0。
- **性能预测**：CPU/op +3.70%→≤+1.0%；p50/p99 随之等比例回落至 ≈+2% ⇒
  延迟是 CPU 均值平移（L0 主导）；仍 +5~7% ⇒ 延迟与 signal 无关，
  **L1 升级为延迟主因候选**。
- 裁决门槛（`../CAUSAL_DAG.md` §7，预注册引用）：CF-1 使 ≥60% CPU 增量
  消失 → O1→C1/C3/C5 因果链闭合（G10 停止条件）；残量 >2.5% → 启 CF-2。
  延迟按上述分叉裁决 L0 vs L1。

## 2. 干预定义（单因素）与实现

- 唯一变化：编译期定义既有 mutant 宏
  `SLUICE_595_MUTANT_DEFER_RESIDUE_SIGNAL_DROPPED`（源码已存在于研究补丁：
  `worktree/src/async/uring_transport.cpp:115-122`）——从 `advertise_residue`
  的或项中移除 `unsubmitted_after_dispatch`；错误/毒化路径
  （`injected_dispatch_failure || newly_poisoned || post_poison_push_retired`）
  的通知保留；deferral 决策（五门链）、flush 时点、其余一切不变。
- 构建门控（本轮研究副本新增，仅 xmake 层，无 C++ 改动）：
  `SLUICE_595_MUTANT_SIGDROP_BUILD=1` → 上述 define
  （`worktree-timing/xmake.lua`、`worktree/xmake.lua`）；并按 R1 模式新增
  mutant oracle target `uring_595_mut_defer_residue_signal_dropped`
  （两 worktree `xmake/tests.lua`）。
- 二进制成对（frozen / sigdrop），臂仍由 env `SLUICE_595_ARM` 运行时选择，
  每个计时块内 A/B 同二进制同布局（无布局混淆）；跨块的 mutant-A 与
  frozen-A 绝对值对比作为布局等价性检查（§5.4）。

## 3. 构建与 provenance（`raw/meta/`）

| 项 | 值 |
|---|---|
| timing worktree | `causal-r2/worktree-timing`，分支 `research-595-r2-timing`，基 `271b81bf` + R1 `patch.diff` + CF-0 C 臂补丁 + 本轮 sigdrop 门控；全 diff sha256 `4e6dd643…`（`raw/meta/timing-worktree-full.diff`） |
| 仪器 worktree | `causal-r2/worktree`（Phase B/C 状态 + C 臂补丁 + 本轮 sigdrop 门控）；全 diff sha256 `80baecf5…`（`raw/meta/instrumented-worktree-full.diff`） |
| `bin_timing_frozen` | sha256 `0091d2592242f605b5c64b11f22faa8e8b9e9b729146f93f263ba1288118f81c`（= CF-0 timing 二进制，无 SLUICE_595_* 仪器宏） |
| `bin_timing_sigdrop` | sha256 `4de2329923b6f889dd269edfb3657ff7d7f54b6553c1226f7b778b8051b778e8`（同上 + sigdrop define；构建命令 `SLUICE_595_MUTANT_SIGDROP_BUILD=1 xmake f -c -y --liburing=y -m release && xmake build bench_w02_pipeline`） |
| `bin_counters_frozen` / `bin_counters_sigdrop` | `601dad6a…`（= CF-0 counters 二进制）/ `23640643…`（+sigdrop；`SLUICE_595_COUNTER_BUILD=1 [SLUICE_595_MUTANT_SIGDROP_BUILD=1]`） |
| `bin_trace_frozen` / `bin_trace_sigdrop` | `1b131f4f…` / `58c49d26…`（`SLUICE_595_TRACE_BUILD=1 [ + sigdrop]`） |
| 工具链/内核 | g++ 16.2.1、xmake 3.1.2+HEAD、liburing-devel 2.13-2、Python 3.14.8、kernel 7.2.11-200.fc44（`raw/meta/toolchain-env.txt`） |
| CPU/环境 | E5 Xeon E5-2666 v3 20 核；cpu2 `taskset -c 2`，schedutil，事后频率 3.5 GHz；perf_event_paranoid=2；计时块内 1-min loadavg 峰值 1.56（base 块）/ ≤1.30（其余块），phaseA 偏差 3 同域 |
| 主工作树 | `/home/jnhu/Source/Sluice` clean 未触碰；R1 evidence 未触碰 |

## 4. 正确性 oracle（sigdrop mutant timing 构建，2026-11-04）

- 13 个 uring target B-mode lane（`SLUICE_595_ARM=B`，mutant 配置构建）：
  **13/13 rc=0**（`raw/correctness/bmode-mutant-suite.log`），含
  `uring_595_bounded_progress_test` "all checks passed"。frozen BASE GREEN
  复用 CF-0 `three_arm/correctness/bmode-suite.log`（同一 frozen 二进制
  `0091d259…`，13/13）。
- 595 oracle 臂 A 显式：rc=2 "SLUICE_595_ARM=B is required"——设计内拒绝，
  与 CF-0 amode-explicit lane 同口径。
- bench 字节 oracle：全部计时/机制 invocation exit 0（硬门）。
- 门链 mutant 先例（R1 `mutant-receipts.log`）证明该 oracle 能杀死 defer
  语义 mutant；sigdrop 通过 ⇒ 干预在 oracle 覆盖范围内保持 arm-B 请求语义。

## 5. 机制读数（companion run，全部独立于计时块）

### 5.1 研究计数器（`raw/cnt-companion/`，counters build，D2，每 cell 3 invocation × 3 rep × 20k op）

| 计数器 /op | frozen A | frozen B | sigdrop A | sigdrop B | 冻结预测（sigdrop B） | 判定 |
|---|---|---|---|---|---|---|
| `wakeups` | 0 | 0.5463 | 0 | **0.0000** | →0 | ✓ |
| `wakeup_writes` | 0 | 0.2472 | 0 | **0.0000** | →0 | ✓ |
| `wakeup_coalesced` | 0 | 0.2991 | 0 | **0.0000** | （连带→0） | ✓ |
| `sqes_deferred` | 0 | 0.5463 | 0 | **0.5448** | 仍 0.5589（同轮对照 0.5463） | ✓ deferral 不变 |
| `flushes` / `actual_enters` | 1.0000/1.0000 | 1.0000/1.0000 | 1.0000/1.0000 | **1.0000/1.0000** | 仍 1.0000 | ✓ |
| `submit_attempts` | 1.7877 | 1.2473 | 1.7865 | 1.2578 | B 形态保持 | ✓ |
| `dispatch_passes`/`sqe_preparations`/`cqes_reaped`/`wait_attempts`/`poll_passes` | — | — | ≡frozen A | ≈frozen B | 形态不变 | ✓ |
| `progress_handoffs` | 0 | 0 | 0 | 0 | — | ✓ |

A 臂 15 计数器在两构建间逐项相同 ⇒ mutant 对 A 臂零行为差（单因素纯度）。

### 5.2 syscall 面（`raw/sysio-frozen/`、`raw/sysio-sigdrop/`，timing 二进制、
`/proc/<tid>/io` 每 invocation 差分，6 对 × 2 臂 × 2 构建，非计时用途）

| 量（bench 线程） | frozen B−A | sigdrop B−A | 冻结预测 | 判定 |
|---|---|---|---|---|
| syscw /inv | **+86,344**（B 85,184–87,699 vs A 70–72；复现 C3 +86,912 [86,541,87,263]） | **+0**（B 70–72 vs A 70–72，恒为 fixture 背景） | →0 | ✓ |
| syscr /op | −0.016 | +0.018 | 无新增 read | ✓ |
| utime/stime（tick） | 0.00±0.00 | 0.00±0.00 | stime 增量→≈0 | **OBSERVABILITY_LIMIT**：10 ms tick 在 6 对下无分辨率；由 syscw→0 + C3 已确立"write 是唯一 B 专属内核 syscall"间接支持 |

- 首次 sysio 两块因归档二进制改名（comm 前缀失配）被整目录废弃
  （`raw/DISCARDED-sysio-wrongcomm-*`，数据本身有效但违反工具归桶协议），
  以规范名 `bench_w02_pipeline_*` 重跑——重跑块为分析所用。

### 5.3 用户态 7 点环（`raw/trace-companion/`，单仪器：仅用户环，无 bpftrace；
E2 同参数 2000 op × 3 phase，两构建各 1 次 B 臂 run，dropped=0）

| 量 | frozen B | sigdrop B | 冻结预测【D】(a) | 判定 |
|---|---|---|---|---|
| U.defer 比例（3 phase） | 0.5305/0.5235/0.5235 | **0.5285/0.5195/0.5205** | 仍 ≈52% | ✓ |
| U.flush / phase | 2000/2000/2000 | 2000/2000/2000 | 不变 | ✓ |
| FLUSH 直接前驱=defer | 1061/1047/1047（=当 phase defer 数） | 1057/1039/1041（同构） | 比例不变 | ✓ |
| 用户态 write 事件 | —（环无 write 事件类型） | — | 消失 | 由 5.1 `wakeup_writes`=0 + 5.2 syscw=背景值共同证实 ✓ |

### 5.4 root bpftrace `syscalls:sys_exit_write` 计数（【D】(b)）：**未运行**——
本会话 sudo 需密码，root 不可达；attach 路径本身已由 xlayer 验证。用户态
write 消失由 5.1/5.2 等价覆盖，此扩展标记 UNOBSERVED_ADMIN_UNAVAILABLE。

## 6. 计时（frozen 协议臂；仪器 run 与计时 run 不混块；串行独占）

- 协议 = phaseA 修复版：`pair_runner.py`，D2 case `W02-uring-4k-d2`
  （`SLUICE_BENCH_595_DEPTHS=1`），`taskset -c 2`，WARMUP=1 MEASURED=5
  ×20,000 op，`SLUICE_BENCH_STATS=1`，tmpfs `/tmp/sluice-595-bench`，
  even 对 A→B / odd 对 B→A，exit 0 硬门；统计=配对相对差、bootstrap
  10,000、seed 5950、percentile。每块 12 对（CAUSAL_DAG §6 冻结样本量）。
- 块序（A→干预→干预重复→A 恢复）：`ab-d2-base`(frozen) →
  `ab-d2-sigdrop`(mutant) → `ab-d2-sigdroprep`(mutant) →
  `ab-d2-restore`(frozen)；块间 ≥45 s 冷却，96 invocation 全部 exit 0、
  全部恰 100,000 measured op。原始数据 `raw/ab-d2-*/`（results.jsonl/
  header.json/logs/jsonl），分析 `raw/timing-analysis.json`。

| 块 | 二进制 | CPU/op B−A | p50 | p95 | p99 | tput |
|---|---|---|---|---|---|---|
| base | frozen `0091d259` | **+3.34% [+2.30,+4.35]** | +2.56 [+1.91,+3.27] | +4.42 [+3.49,+5.33] | +5.03 [+3.50,+6.64] | +3.21 |
| sigdrop | mutant `4de23299` | **−0.58% [−1.11,−0.10]** | +3.87 [+2.26,+5.60] | +3.90 [+2.85,+4.80] | +5.29 [+3.91,+6.55] | −0.59 |
| sigdroprep | mutant | **+0.06% [−0.54,+0.69]** | +4.73 [+2.11,+8.95] | +4.67 [+3.82,+5.45] | +4.45 [+3.32,+5.59] | +0.05 |
| restore | frozen | **+3.34% [+2.40,+4.38]** | +8.21 [+3.64,+13.82] | +5.04 [+3.87,+6.20] | +6.68 [+4.13,+10.53] | +3.21 |
| 合并 frozen（24 对） | | +3.34 [+2.62,+4.07] | +5.38 [+2.94,+8.54] | +4.73 [+3.97,+5.45] | +5.85 [+4.28,+7.99] | |
| 合并 sigdrop（24 对） | | **−0.26 [−0.67,+0.16]** | **+4.30 [+2.64,+6.61]** | +4.28 [+3.58,+4.92] | **+4.87 [+3.96,+5.75]** | |

绝对 cpu/op（ns）：A 臂 frozen 块 4648.1/4675.4 vs mutant 块 4664.6/4642.4
——mutant-A 与 frozen-A 差 <0.7% 且方向不定 ⇒ 无二进制布局系统偏移。

## 7. 裁决（对照 §1 冻结门槛）

1. **CPU 主因假设成立（G10 停止条件满足，因果链闭合）**：合并干预
   B−A CPU/op = **−0.26% [−0.67,+0.16]**（CI 含 0）vs 冻结 +3.34%
   [+2.62,+4.07]。消失比例：点估计 **107.8%**，保守口径（干预 CI 上界
   +0.16%）**≥95.2%** —— 远超 ≥60% 门槛。残量 CI 上界 +0.16% < 2.5% ⇒
   **无需启 CF-2**。预测 "+3.70%→≤+1.0%" 以 −0.26% 命中（且本同轮
   frozen 基线 +3.34% 与 phaseA +3.73%/CF-0 +3.21% 同域）。
   证据等级：**反事实支持（配对干预 + 单因素机制计数器闭合 + oracle
   通过 + A 恢复复验）→ 严格因果证明**（同一 bench/case/oracle，
   workload 等价：每 invocation 恰 100,000 op、exit 0、字节 oracle）。
   O1(residue 通知)→C1(signal 用户态执行)→C3(write syscall) 链条由
   机制读数逐项对账：wakeups/wakeup_writes/coalesced→0（C1 面）、
   syscw 差 +86,344→0（C3 面）、CPU 增量 +3.34%→−0.26%。
2. **延迟 L0/L1 分叉：命中 (ii)，L1 升级为延迟主因候选**。干预块内
   CPU/op 差已 ≈0（−0.26%），而同块 p50 **+4.30% [+2.64,+6.61]**、
   p99 **+4.87% [+3.96,+5.75]** 仍显著 >0 且未回落至 ≈+2%——延迟位移
   与 signal 无关。p50 从 frozen 合并 +5.38% 到 sigdrop +4.30% 的
   ~1pp 差远小于 CPU 的全部消失量，且两 CI 重叠；按冻结分叉的判据
   （p50/p99 是否随 CPU 等比例回落至 ≈+2%）判 (ii)。
   ⇒ deferral 时序（L1：内核工作起点推迟一个 poll 周期，结构前提已是
   每请求级直接观测 X1）承接延迟主因候选；后续段级分解按 CAUSAL_DAG
   §7 走单仪器 7 点环 deferred × 延迟分层直方图（G12），本轮未运行。
3. **A→干预→A 恢复复验通过**：restore 块 +3.34% [+2.40,+4.38] 精确
   重现 base 块 +3.34% [+2.30,+4.35]；机制计数器与 7 点环结构在干预后
   亦同构恢复（§5 均含 frozen 对照）。无漂移、无滞后。
4. 过度消失（点估计 >100%）说明：去掉 signal 后 B 臂还净露出
   ≈0.3% 的微小优势（poll 合并形态 submit_attempts 1.25 vs 1.79），
   量级在本轮噪声半宽（0.5–0.6pp）边缘，不单独归因，仅记录。

## 8. 限制与如实标注

- utime/stime tick 差分在 6 对下无分辨率（§5.2）；"内核 stime 增量→≈0"
  为 syscw→0 的间接支持，非直接观测。
- root bpftrace sys_exit_write 扩展未运行（sudo 需密码，§5.4）。
- perf `__GI___libc_write` 符号份额回落（C1 可证伪条件）本轮未运行
  PMU 块——syscw 差→0 是同一事件的 syscall 面证据；C5 微架构回落
  （cache-ref/branch-miss/IPC）同理留给后续 G7 PMU 差分，本轮无数据。
- p50 分位数块间波动大（+2.6%~+8.2%），裁决使用 24 对合并口径。
- mutant 二进制布局差异：A 臂绝对 cpu/op 无系统偏移（§6），但无法从
  原理上排除 <0.3% 量级的布局效应；已在 G10 风险登记范围内。
- R1 sanitizer lane 与全 mutant-receipts lane 本轮未重跑（CF-0 同款
  标注）；13-uring B-mode + 595 oracle + 字节 oracle 为本轮正确性面。
