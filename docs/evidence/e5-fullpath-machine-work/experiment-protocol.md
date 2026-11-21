# 单因素消融实验预注册协议（草案，未执行）

- 撰写：候选裁决官，2026-11-18。性质：预注册协议**草案**——本会话未执行任何构建、
  测试或测量；执行须在协议冻结（BASE/CANDIDATE SHA 落定）后按本文件逐条进行。
- 选定候选：**#1（M-28 syscall_count_ 生产侧宏门控剔除）**，裁决见 `candidates.md` §1/§3。
- 单因素声明：本实验只改一个机制（一个计数器的门控归属）；不改任何锁、提交时序、
  批量拓扑、发布/交付协议；不含 #11（M-42，另行排队）。
- **修订 r2（独立挑战者缺陷修复）**：F1——§2 负载集改用真实 case_id（`W02-threadpool-…`，
  make_meta 拼装核实；原 `W02-tp-…` 在 '=' 精确匹配下零匹配）；F4——§5 判词补 uring
  隔离格数值判据、64KiB 判据与回执违例映射（并入判词 1 中止通道）；F5——§5 补双 MME
  管辖声明与 σ 功效先验（NOT_POWERED 预声明为现实结局）；F7——§2/§3.2 写明 mutant
  载体（休眠 test-only defines），消除与"仅门控 diff"冻结描述的矛盾。

## 1. 机制定义与机制正当性门作答

- 生产改动 = 把 `syscall_count_` 写侧（`src/async/threadpool_backend.cpp:434`）与成员
  （`include/sluice/async/detail/threadpool_backend.hpp:258`）移入
  `#if defined(SLUICE_ASYNC_INTERNAL_TESTING)`。访问器声明（hpp:66-76 内）与定义
  （`src/async/threadpool_test_seams.hpp:108-110`，文件宏 :4）已在宏内，无需改动。
- 机制门七问：① 代表的事实 = 每 backend 物理 syscall 计数，所有权是测试 oracle；
  ② 依据 = 测试断言（16 条计数断言）；③ 可从既有权威派生（测试宏内读）；
  ④ 无须在生产物化——生产无读者；⑤ 分类 = **test-only evidence**（生产侧错位）；
  ⑥ 替代机制 = None；⑦ 移除条件 = 生产读者出现则重新物化为生产权威（届时宏门控不成立）。
- 先例：#635 DrainObservation（test-only 观测不得在生产运行；#635 的 test-only 面
  即按此形态落地，history-digest §4.2）。

## 2. 冻结项

| 项 | 冻结值 |
|---|---|
| BASE_SHA | `5d94f233967c3aef993eb19ba9e3252c532b0951`（master 冻结基线） |
| CANDIDATE_SHA | 新 research worktree **`/home/jnhu/sluice-e5-elim/`** 内单提交 = BASE + §1 的门控 diff **+ 2 个休眠 test-only mutant defines**（#634 先例载体：mutant 代码全部位于测试宏内侧或 seam 文件——写侧抑制 mutant 包在门控后的测试宏块内、访问器错报 mutant 位于 `threadpool_test_seams.hpp`——任何常规构建（含生产库）行为零差异，单因素声明不变）；mutant 构建二进制 sha256 另行登记。SHA 在任何计时前登记进证据根。**绝不复用 `/home/jnhu/sluice-621-r2/`**（12 个在制 worktree，repo-reality §2.2 保护清单）；主仓工作树不改任何被跟踪文件 |
| 编译器/工具链 | gcc 16.2.1、xmake 3.1.2、liburing 2.13（history-digest §0 口径）；构建入口 `bash bench/build_profiles.sh release`（独立 `build-bench-ring/`/`build-bench-noring/` 目录，#520 混链禁令，repo-reality §4）；readelf profile 身份断言照跑 |
| 二进制 sha256 | 两臂 `bench_w02_pipeline` + `libsluice_async.a` sha256 全登记（P1-A 参考基线 ring bench sha `03dbef09…`，p1a §0）；构建日志入证据根 |
| CPU placement | **不 pin**（保留 tp-w8 竞争现象与 P1-A 口径可比性——pin 会改变被测的竞争形态）；每 invocation 记录 `Cpus_allowed_list`；AB/BA 交错 + A/A 宽度吸收放置噪声；中半程禁止改 placement |
| governor | performance 三列存证恢复：逐 CPU 存原值 → performance（读回 20/20）→ 全部计时 → 恢复（读回 match=yes）；**每轮独立唯一文件名**（measurement-audit §2-8 教训：固定文件名被覆盖） |
| fixture | bench 自建 64 MiB（`kFixtureBytes`，bench_w02_pipeline.cpp:50）；scratch = 新 btrfs 目录（chattr +C，lsattr 存证；挂载 compress=zstd:1 属既知装置偏差，如实披露）；warm 判据 = 每个 timing 窗口 `/proc/diskstats` sda Δreads=0 且 Δsectors=0（P0-C 判据） |
| 负载集 | 主格 **W02-threadpool-4k-d16-w8** @btrfs warm（M-28 实测格，p1a §4-1）；支持格 W02-threadpool-4k-d16-w4（无竞争形态，非裁决）；隔离对照 W02-uring-4k-d16（判据见 §5 判词 1）；payload 对照 W02-threadpool-64k-d16-w4。**case_id 为 make_meta 真实拼装值**：`"W02-" + backend->name("threadpool"/"uring") + "-{4k,64k}-d<depth>" + ("-w<workers>" 仅 threadpool)`（bench_w02_pipeline.cpp:462-467，backend 名 ：621-622；本会话核实）——P0-C §4.1 表内 "W02-tp-…" 是表格缩写、不是 case_id，'=' 精确匹配（bench_common.hpp:492-500，`case_id == filter+1`）下缩写零匹配，禁止使用。M-28 仅 threadpool 路径（uring TU 不触碰）→ uring 无计时裁决格，仅隔离对照 + 正确性矩阵。`SLUICE_BENCH_WARMUP=2 SLUICE_BENCH_MEASURED=7` |
| perf 口径 | 每 observation = 1 次 bench invocation 包 1 层 perf stat，两种 pass 固定分工：**P2（裁决主）**= sudo `-e cycles,instructions,task-clock,context-switches -x,`（事件集 A，0 复用，u+k 两列分开）；**P1（计数回执）**= 无 sudo `-e instructions:u,cycles:u -x,`。每对每臂各跑 2 次 invocation（P1/P2 各一），pass 类型内配对 |
| ops 分母 | 非饱和格 9 rounds × 3 phases × 20000 = 540000 ops/invocation（P1-A §0 口径）；tp-w8 非饱和（P1-A §1） |

## 3. 正确性 Oracle

1. **语义 oracle**：三个消费套件的 16 条计数断言门控后必须全绿（tests/request_scope_test.cpp
   :885,990,1133,1189；tests/threadpool_request_core_integration_test.cpp :265,289,380,
   422,463,487,493,522,569,943；tests/public_request_test.cpp :313,414）。构建前提已核实
   （r2 更正为完整清单）：`grep -n 'threadpool_backend.cpp' xmake/tests.lua` 全文 **12 处**
   重编译点（:406,448,501,559,745,818,891,959,1015,1074,1139,1205）；三个消费套件的目标
   均在其中且 `add_defines("SLUICE_ASYNC_INTERNAL_TESTING")`——`request_scope_target`
   （helper :1114、define :1122、重编译 :1139）、`public_request_target`（:731 起、重编译
   :745）、`threadpool_request_core_integration_test`（define :1202、重编译
   :1205）→ 门控后测试二进制仍持计数器，生产库（libraries.lua 无宏）失去它。任何链接
   失败 = 构建回执，不是静默通过。
2. **注册 mutant 回执**：候选提交注册 2 个 mutant（#634 回执格式，手工执行、记录
   exit 码，永不注册为常规测试）。**载体（r2 明确）**：随 CANDIDATE 提交的休眠
   test-only defines——写侧抑制 mutant 的守卫包在门控后的测试宏块内、访问器错报 mutant
   位于 `threadpool_test_seams.hpp`；mutant 宏在任何常规构建（含生产库）未定义 → 生产
   TU 行为零差异，CANDIDATE 的生产 diff 仍仅 §1 的门控（与 §2 冻结一致）；mutant 构建
   的二进制 sha256 单独登记：
   - `SLUICE_THREADPOOL_MUTANT_SYSCALL_COUNT_STICKY_ZERO`：测试宏下写侧被抑制 → 被
     `==N` 计数断言杀死（如 request_scope_test.cpp:1133 `==2`、integration :943 `==4`）；
   - `SLUICE_THREADPOOL_MUTANT_SYSCALL_COUNT_ACCESSOR_SKEW`：访问器错报（±1）→ 被
     任一精确断言杀死。
3. **conformance 矩阵**：release/debug × liburing y/n 四配置全注册测试套件，逐配置
   计数登记（#634/#635 口径 81/81、81/81、60/60、60/60 量级，以实际计数为准）。
4. **TSAN/ASAN 目标集**：request_scope_test、threadpool_request_core_integration_test、
   public_request_test、threadpool_owed_counter_parity_test、progress_source_ownership_test、
   progress_source_drain_semantic_oracle_test（asan/tsan 模式各一）。

## 4. 计数层证据

1. **宏门控验证（三列存证）**：release 库 `nm libsluice_async.a` → 无 `syscall_count_`
   符号；`objdump -d` worker_loop → 无对 `syscall_count_` 偏移的 `lock addq` 写指令；
   测试构建二进制 → 符号与写指令均在。
2. **P1-A 口径 instr/op 复测**：主格 P1 pass，预期 Δ(B−A) = **−1.000 instr/op**（`lock
   addq` 是唯一指令差；类布局偏移可能扰动编码，接受带 [−2, 0]）。该端点是机制回执
   （写指令离开指令流），**不是性能声明**——其量级 −0.025% 远小于 cycles 端点。
3. **mutant 回执 exit 码**：逐 mutant 记录（#634 `mutant-receipts.txt` 格式）。
4. **违例映射（r2，F4c）**：以上任一回执违例——nm/objdump 三列存证不符（生产库仍含
   符号或写指令、测试构建缺失）、或 instr/op Δ 落在 [−2, 0] 带外——映射到 **§5 判词 1
   中止通道**（机制未按声明落地，实验中止、候选 NO_GO），不进入任何性能判读。

## 5. 性能端点与统计（正视小效应）

- **张力陈述**：预期效应 ≈170 cyc/op（p1a §4-1 算术链 0.554% 全局 cycles @tp-w8，
  30653.5 cyc/op 口径；uncontended 指令本体与 w8 竞争放大未分解，总效应只会更大）。
  #635 先例：instructions 主口径 + 36 对 + A/A 宽度 2.4090≤3.0pp 才分辨 5.018%
  （history-digest §1.2）——0.5% 量级在该分辨力下不可分辨，必须加对数或降级为
  机制门路径。本协议两者都做：加对数到可分辨为性能 GO 的唯一通道；不可分辨则
  性能端点判 NOT_POWERED，裁决落机制门，测量数字如实报小。
- **主指标**：cycles/op @W02-threadpool-4k-d16-w8（P2 pass，u+k 主列、u/k 辅列分开报告）。
- **MME（最小有意义效应）**：155 cyc/op（≈0.50pp）；**分辨力要求：A/A 半宽 ≤77 cyc/op
  （MME/2）**。门不得松于 A/A 半宽——半宽达不到 MME/2 时不存在性能 GO。
- **双 MME 管辖声明（r2，F5a）**：本实验的 0.50pp MME 只约束**本候选性能声明的分辨
  门槛**；#1 的准入走机制正当性门（MEASURED hotspot + 七问作答），非性能准入。r6 F.6
  的 1% MME（R2 0.90%/R3 0.45% REJECTED 先例，history-digest §1.2 项 6）管辖的是
  **新产品候选的准入**。两者不冲突但须显式划界：**≤1% 的已分辨回执不构成产品级效应
  声明、不进入 ledger 产品行、不产生任何新候选槽位**——0.5–1% 的已分辨改善仅作为
  本候选回执与文献值登记。若未来要把 M-28 的回执升格为产品级效应声明，须按 r6 F.6
  的 1% MME 重新立项，不得复用本实验数字。
- **A/A 预算、对数与功效先验（r2，F5b）**：试点 **≥24 对 AB/BA 交错**（对内 A/B 时间
  紧邻以共享漂移；同 session 内先 A/A 后 A/B，新 session 须自证 A/A，history-digest
  §5.3-4）；半宽 >77 cyc/op → 按 24 对/块扩展，**上限 120 对**；仍不达 → 在拆盲前
  声明性能端点 NOT_POWERED。**功效先验（σ 依据，冻结）**：
  - P1-A 3-rep CV（cycles u+k，tp-w8）= 1.1% → σ ≈ 337 cyc/op（p1a §1）；
  - 若配对差 σ_pair ≈ σ（AB/BA 紧邻、漂移共享）：120 对半宽 ≈ 1.98×337/√120 ≈
    61 cyc/op ≤ 77 → **勉强可达**；
  - 若两臂独立 σ_pair ≈ 477：120 对半宽 ≈ 86 cyc/op > 77 → **不可达**；
  - #635 rusage A/A 36 对半宽 ≈1.2pp → σ_pair ≈ 3.6% → 外推 120 对半宽 ≈ 200 cyc/op
    → **必 NOT_POWERED**（跨工作负载外推，仅作量级警示，history-digest §1.2）；
  - tp-w8 为 futex 等待主导形态（3.2 futex/op、均值等待 42 µs，p1a §1）→ σ 落高位
    先验大。
  **预声明：NOT_POWERED 是现实预期结局之一**；预算封顶 120 对（√N 缩放在此之上的
  边际分辨收益低于成本）；扩展决策只依试点实测 σ_pair，不以乐观外推决定。无论
  NOT_POWERED 与否，机制门通道照常进行（§7）。
- **统计方法**：逐对配对差 → 逐对符号检验（方向计数）+ bootstrap CI（percentile，
  10000 重采样）+ Holm 校正跨指标族；族 = {cycles/op, rusage CPU/op, ops/s, p99}，
  仅 cycles/op 定决，其余族控支持列。
- **失败判词（冻结，r2 扩展 F4）**：
  1. **中止通道（任一触发即实验中止、候选 NO_GO）**：
     a. 任一正确性门失败（语义 oracle/mutant/矩阵/TSAN/ASAN）；
     b. 计数层回执违例（§4-4：nm/objdump 三列不符、或 instr/op Δ 落 [−2, 0] 带外）；
     c. **uring 隔离格违例**（判据：隔离格先跑 6 对同 pass 型（P2）A/A 得 σ_uring；
        uring A/B 判据在 **24 对 AB/BA（P2 pass，与主格试点同量级，冻结）** 上执行：
        配对 Δ 的 95% bootstrap CI 不含 0，或 |point| > max(2σ_uring, 隔离格 cycles/op
        均值的 1%)（量纲 cyc/op）——候选声称"uring 路径 Δ≈0"，此判据给该自述以数值
        定义，违例 = 机制外溢）；
     d. **64KiB 回归绊线**（判据：64KiB 格 CPU/op 配对回归（B−A>0）的 CI 下界 > +2%
        → halt-and-investigate：纯计数器移除不可能真实回归，回归指示构建/配置错误；
        **处置路由：经根因核查属构建/配置 artifact → 转判词 2 的 session 作废重建通道，
        记录根因后重跑（不自动 NO_GO）；排除 artifact 的真实回归 → 中止、候选 NO_GO**；
        改善方向如实披露即可，**非零是现实可能**——#635 64KiB 对照实测 −1.4611%
        [−2.7425, −0.2340]（history-digest §1.2），不构成归因声明，格为报告性）；
  2. A/A 无效（主指标半宽 >3.0pp 或系统性漂移/趋势）→ session 作废重建；
  3. B 劣于 A 且 CI 排除 0 → NO_GO；
  4. B 优于 A、A/A 半宽 ≤77 cyc/op 达成且 CI 排除 0 → 性能 GO（量级如实报告；按
     双 MME 管辖声明，≤1% 回执不构成产品级效应声明、不产生新候选槽位）；
  5. 方向为改善但未分辨（CI 跨 0 或半宽未达）→ 机制门裁决，测量值带 CI 报小，
     禁止归因声明；
  6. NOT_POWERED → 同 5（功效先验下为现实预期结局，见 A/A 预算段）。
- **尾延迟 SLO（保护界）**：p99(B)/p99(A) bootstrap 上界 ≤ **1.05**（#595 族保护门
  约定；这是保护界不是效应声明，与"不得放进噪声的门"不冲突——它限制回归而非声明收益）。
- 禁止项：不得以 sleep/重试膨胀/弱化断言把任何门变绿（AGENTS 测试纪律）；strace
  永不计时（仅计数 sanity）；profiling 构建与吞吐分层报告。

## 6. 负控制家族（全列后按机制相关性剪枝）

| 控制 | 处置 | 理由 |
|---|---|---|
| uring 隔离格 W02-uring-4k-d16 | **保留**（隔离判据格，非性能裁决格；判据见 §5 判词 1c） | 候选必须对 uring 路径 Δ≈0（机制不在 uring TU）；隔离证明，违例走判词 1c 中止通道 |
| 64KiB payload 格 W02-threadpool-64k-d16-w4 | **保留**（报告性负对照） | 计数与 payload 无关，但验证门控在其他 TP 形态无回归（#634/#635 负对照惯例）；判据见 §5 判词 1d |
| D1/D2/D3 深度边界 | 剪枝 | 提交时序/批量 falsifier，属概念 B/#595 所有权；且 D2/D3 在 master bench 冻结 case 列表 NOT_CONSTRUCTIBLE（p0c §9） |
| W-03 split-owner | 剪枝 | uring 外驱形态，候选无 uring 足迹；W-03 falsifier 属概念 B |
| short/long 混合 | 剪枝 | 计数按 syscall 次数递增，与长度无关 |
| cancel before/after execution claim | 剪枝（计时）/正确性矩阵覆盖 | 候选不改 claim/cancel 逻辑；integration 套件 :487-569 的 cancel 计数断言即正确性覆盖 |
| partial submit/retry | 剪枝 | uring SQE 特有路径，候选无足迹 |
| CQ overflow/poison | 剪枝 | uring 特有，候选无足迹 |
| shutdown/drain | 剪枝（计时）/正确性矩阵覆盖 | 候选不触碰 settlement/owed 门；矩阵含 owed parity 与 drain 套件 |
| stale notification/parked owner | 剪枝 | progress 通知族，候选无足迹；矩阵含 progress 套件 |
| slot reuse/observer publication | 剪枝 | 发布族，候选无足迹；矩阵含 publication/consumer probe 套件 |

## 7. 执行纪律与产出物

- 全部原始输出入 `/home/jnhu/sluice-e5-elim/results/<session>/`（governor 三列唯一
  文件名、A/A 表、A/B 表、bootstrap 输出、mutant 回执、nm/objdump 存证、sha256 清单、
  diskstats 判据、JSONL、placement 记录）；脚本不入仓库，commit 证据产物。
- 结论按 candidates.md §1 #1 的翻案条件回写裁决矩阵；ledger 更新（若有产品化）走
  GOV-02 状态 + authority-contraction audit（本机制无权威移动：计数器本来就不是
  生产语义权威）。
- 本协议冻结后如需改动，须在证据根追加 dated 修订并说明不影响已采数字；禁止事后
  改门。
