# #595 Flow Audit — CORRECTIONS（外审发现 R1–R6 处置）

审阅锚点：`9d1bf648f7382393f5c45ae6076aebe324207337`（PR #631 原头，
远端未前进，无第三方改动需要保留）。旧文本由 git 历史保存；本文件
记录每项的原主张、核验结果、改动、验证与仍然存在的边界。本修订只
改 `docs/research/595-flow-audit/` 下的归档、离线脚本与针对这些脚本的
确定性测试；未动产品代码、公开 API、正式 benchmark、已计时二进制、
历史原始数据与 R5 冻结统计方法；无任何新计时/trace/性能矩阵。

## 处置总表

| 发现 | 核验结果 | 改动文件 | 验证 |
|---|---|---|---|
| R1 实验识别 | 属实（复核二 R7 精确化：混杂在 C-A vs R_cleanup 归因对照；C-A vs A0 可测净效应） | NEXT_EXPERIMENT.md 重写；DECISION.md；README.md；PR 正文 | 结构论证 + 冻结源码引用 |
| R2 过强归因/数值 | 属实：CPU→耗时换算无效；≈20µs 错误；H3 反证无效；多处以结构为根因 | BASELINE/FLOW_AUDIT/HYPOTHESES/DECISION/README/账本 | 机器复算 9.52/10.71µs（原始精度） |
| R3 信息来源 | 属实：策略实现位置≠输入来源 | FLOW_AUDIT §1 新表 | 逐项冻结源码行号核验 |
| R4 CPU 口径 | 属实：v1 脚本伪造 ns/op；rusage 是 wait4 整次子进程；ticks 是 invocation 窗口 | cpu_scope_recalc.py 重写 + BASELINE.md | 合成测试 + 实数据 exit 0 |
| R5 流程图/映射 | 属实：图A未分边族/缺 size==1 回退；图C单链冒充全生命周期；cv.wait/通知/1:1 enter 表述过强 | FLOW_AUDIT 图A/C/B/D/E + §7 + 账本 | 冻结源码逐边核验 + mermaid 结构检查 |
| R6 证据校验 | 属实：CSV 11/32 行错位；v1 校验器可接受不完整证据 | CSV 重建 + ledger_csv.py + verify_inherited_evidence.py 重写 + test_offline_checks.py | 旧缺陷被反例暴露；新检查 30/30+60/60 PASS |

## R1 — C-A 唯一优先地位撤回（HOLD_DESIGN_CONFOUNDED）

- 原主张：NEXT_EXPERIMENT.md 称 C-A 检验"去除研究呈现层的效果"，
  `enters=ceil(n/Q)+residue` 可精确断言，"mutant 删除 residue 广播后
  oracle 必须挂起"，失败可二选一归因；DECISION/README/PR 称其为
  "唯一下一项"。
- 核验：识别混杂属实——C-A vs A0 同时改变 ready 构造、入口/锁路径、
  跨调用 SQE 持有、flush 时机、residue/driver 依赖。计数断言忽略提前
  flush/partial/retry/control/等待型 enter。负控断言只在"driver 已睡、
  无后续 submit/主动 poll/其他唤醒、该信号为唯一推进来源"时成立。
  条件 (b) 有歧义：冻结源码中 `signal_ready_progress()`=
  `progress_port_.signal()` **只唤醒不 flush**（`M:uring_backend.hpp:297`），
  flush 在下一 pass（`M:uring_backend.cpp:620-634`）——"内联 flush"与
  "唤醒后 pass flush"是不同实验，未预注册不得称可执行。
- "R2 的历史 defer 设计"当轮按 r5-phase-a 归档 defer 家族（原候选 B 与
  C-variant1）理解并注明可重指；复核二已确认为 PR #625 causal-r2，对照
  表按其重写（见 R8）。
- 改动：NEXT_EXPERIMENT.md 全文重写（HOLD_DESIGN_CONFOUNDED、协议
  缺陷清单、对照表、尚未批准的研究问题、**NO_COUNTERFACTUAL_SELECTED**
  ——C1/C2 之后研究路径剩余步骤无语义等价证据，seam 移除曾被 phase-A
  Candidate A 明确拒绝）；DECISION/README/PR 同步降级。
- 边界：C-A 的形状/门限先例仍可被未来协议引用，但须按修正重写；
  本轮不以新候选顶替。

## R2 — 过强归因收缩与数值修正

- 原主张：①"问题主要位于研究叠加层，不是产品流程"；②"p99 差 ≈20µs
  与呈现 8 op 的研究路径 CPU 同量级（C≈2.5µs/op×8）"；③"observe 均值
  A0>R ⇒ H3 不能主导 p99 差"；④"1 enter/8 op ⇒ CQE 更集中""唯一机制级
  收益是 enter 摊销""任何批量形态都能拿到收益"；⑤"同 enter 数 8×1 vs
  2×8"反事实；⑥描述符构造/buffer/应用 ready/提交"四事件必然重合"；
  ⑦R 的 T0 模糊称"组级 T0"。
- 核验与修正：
  - ①改为证据限定表述（DECISION.md 开头）；"不在 master"≠"无产品价值"。
  - ②撤回（进程测量窗口 CPU ≠ 提交线程连续路径 wall time）；代之以
    机器复算绝对差：MAIN 运行级 p99 均值差 **C−A0 9.52µs / R−A0
    10.71µs**（逐块相对效应均值 +17.97%/+20.14%；合并逐请求 p99
    UNAVAILABLE）。三种口径由 verify 脚本机器生成，不再手抄。
  - ③撤回：两臂 tail set 不同，跨集合均值不能分离 H3 对差值的作用；
    改为"现有诊断未能分离观察顺序及反馈作用"。
  - ④降级为待检验假设（H4/H7）。
  - ⑤删除（同时改变调用数与工作量）；识别缺口如实登记。
  - ⑥改写为"这是 bench 的选择，不是产品语义要求"（FLOW_AUDIT §7.1）。
  - ⑦核实为组构建循环内**逐成员**打点（`C:bench_w03:413`），照实改写。
- 改动：BASELINE.md、FLOW_AUDIT.md、HYPOTHESES.md、DECISION.md、
  README.md、WORK_AND_WAIT_LEDGER.csv（W07/T2/T8 等行）。
- 边界：净效应（R5 指定形状的 CPU/吞吐/尾延迟裁决）不变、按原有效
  证据解释；本修正不改任何历史门通过与否。

## R3 — 信息来源与架构推论修正

- 原主张："感知决策表的输入全是 harness 状态，产品无落点""产品唯一
  自由度是 flush"。
- 核验：不成立。`outstanding()` 是冻结产品公共 API
  （`M:async_io_context.cpp:837-840`，语义 accepted∧未 published，非
  kernel inflight）；owner-pass facts 来自产品 `poll_progress()` 的
  `ProgressPass` 返回值（`M:async_io_context.hpp:66-78`，含
  dispatch_retry_remains），跨 pass 保存才是 bench 行为；ready span/
  caller_window 是调用者提供的集合/占用；SQ 空间/ledger/control-residue
  是 backend 内部权威。新增 FLOW_AUDIT §1 紧凑表（信息/原始生产者/
  产品是否已有/缓存转交/语义/读取成本证据/是否足以证明策略价值）。
- 改动：FLOW_AUDIT §0.4 + §1；README/DECISION 同步。
- 边界：策略接入产品的净收益仍未证明；本轮不建议任何新
  timestamp/hint/全局计数器/runtime，也不要求公开新 API。

## R4 — CPU 口径脚本修复

- 原主张（v1 脚本 + BASELINE）：把 `ru_utime+ru_stime` 除以
  measured_records 再除以 20000、把 invocation ticks 除以 20000 得
  "ns/op"；称 rusage 为"getrusage at end"；io-wq 份额当作机器工作占比。
- 核验：①主性能 CPU = measured 相位 CLOCK_PROCESS_CPUTIME_ID（冻结
  harness 定义 sum(cpu_s)/sum(ops)）；②`ru_utime/ru_stime` 来自父进程
  **wait4** 的整次子进程 rusage（measure3.py:144,161）——覆盖 warmup/
  oracle/instrumented/setup/teardown，既非 measured 相位也非"end 时
  getrusage"；③/proc ticks 覆盖整次 invocation、10ms 粒度、first-to-last
  差分，io-wq 每 0.5s 扫描得下界——任何整次量除以 op 数都不产生与主
  指标等价的 ns/op。
- 修复（cpu_scope_recalc.py v2）：三窗口分离输出；主口径沿用冻结定义
  并与 harness metrics 交叉核验；invocation 秒/ticks 只按原单位输出；
  份额只称"sampled-and-classified"；缺失字段显式 MISSING/UNAVAILABLE
  且退出非零；`--root` 参数；输出含 results.jsonl SHA 与覆盖说明。
- 新观察（登记，不推翻历史）：io-wq worker 以任务形式出现在子进程
  `/proc/<tgid>/task`（24/24 次调用各 1 个，procfs 结构证据）；采样器
  自身 residual 估计 72/72 超其 bound（均值 ≈26 vs ≈11 ticks，窗口
  错配主导）。**io-wq CPU 是否计入子进程 rusage/PROCESS_CPUTIME 在本
  内核上 UNRESOLVED**——r5-repair RESULTS.md 的"io-wq outside rusage"
  警告在本主机按未验证处理，不复述为事实；主口径点估计本身不变。
- 验证：合成测试（measured/非 measured/整次量三源输入：主口径只吃
  measured、无伪造 ns/op、缺字段 exit 2）+ 实数据 5 格 exit 0。
- 边界：PMU/IRQ 不补测；"−22%"维持过程口径表述，不称全系统。

## R5 — Mermaid 与源码映射修正

- 原主张：图A 把批量入口画成调用 `submit_request` 顶层、无 size==1
  回退、锁范围不清；图C 用一条成功链覆盖全部 Request 生命周期；正文
  把 cv.wait_for 当必然 park、把库内通知与内核 CQ 通知混同、把"每 op
  一次 enter"当无条件事实、把"允许发生"暗示为"实际发生"。
- 核验与修复：
  - 图A 重画为三条边族：产品普通路径（`submit_request`→zero-op 分叉→
    `dispatch_after_accept`→每 op 1 enter）、研究批量路径（`admit_`
    循环+`dispatch_one_locked` 循环+**每呈现一次** `submit_transport_locked`，
    全部标注在整批 `dispatch_mtx_` 范围内）、**size==1/首员不合格回退**
    （`C:uring_backend.cpp:479-504`，admit_+per-op enter——singleton
    成本判断的关键边）；批量路径不再画成调用普通顶层入口。
  - 图C 拆为正常非零读主图 + 合法分支图（zero-op 在 accept 路径内
    terminal、pre-dispatch cancel 先于 execution claim、terminal 仲裁、
    observer armed/queued/delivering/retired、reclaim 条件）。
  - borrow 契约端点按 REQ-04 核写：内部 `release_execution` 退休是
    必要前置；调用者可复用缓冲区的可观察条件是**获得 publication**
    （acquire 语义），"物理完成或 cancel 响应单独不足"。
  - cv.wait_for/ctx.wait_one 全部改"potential park"表述；通知拆为
    内核 CQE 对 ring 注册 eventfd 的写 + 库 `progress_port_.signal()`
    非 CQE 路径写（同一 fd、两类写者、事件数≠请求数）；"每 op 1
    enter"限定正常非零读形状并注明 liburing 调用数≠系统调用数；
    publication 在 `dispatch_mtx_` 外保持"源码允许"而非"实测频率"。
- 验证：六块 mermaid 结构检查通过（类型头/subgraph/note/围栏）；
  **本机无离线渲染器，渲染未验证**（如实标注，不把结构检查称渲染）。
- 边界：图中"允许发生"的并发边不构成 trace 证据。

## R6 — 校验脚本拒绝不完整/错位证据

- CSV：原 13 列中 11/32 行错位（v1 手工拼接丢空字段）；另有非法证据
  等级（PUBLISHED_VERIFIED 等非冻结词表）与笼统 Required 归类。
  修复：`ledger_csv.py` 以结构化数据 + `csv.writer` 确定性重建；校验
  列数/唯一 ID/必填/kind/词表；Required 拆 REQUIRED_SEMANTIC（语义
  责任）与 REQUIRED_IMPL（此锁/扫描/通知实现）；未测字段
  UNMEASURED/UNAVAILABLE。反例（截短行、多列行、非法等级）非零退出。
- 主指标核验：v1 glob 全部 jsonl、按文件名猜臂、扁平平均、"round 后
  容差"却称 bit-match、F5 只查存在位置（双位置摘要可通过）。修复
  （verify_inherited_evidence.py v2）：results.jsonl 准入（valid/seg/
  block/position/arm/attempt/二进制 SHA[取自 measure3 BINS 并对在盘
  二进制重哈希——本修复过程自身踩过手抄 SHA 丢字符的坑，遂改为程序
  读取]/readback/measured 计数）；唯一合法 invocation（重复即失败，
  冻结 analyze_round 的 load_seg 会静默覆盖，已登记为差异）；按冻结
  定义逐 invocation 复算再按块聚合；扁平平均与逐 invocation 平均的
  恒等条件给出（ops 恒定）并实证；精度主张如实：**与 round-analysis.json
  原始浮点值一致（max rel diff 0.0，75 值）**，CIs/Holm/门限不复算；
  只验证绝对均值就明说。
- F5：断言精确位置集（d16/cw0=0..15、d16/cw8=0..7、d4/cw0=0..3）、
  每桶分母与整数尾数对账、24 instrument-on R tag × 20000=480,000 闭合
  （明确口径：仅 instrument-on R 相，12×d16+12×d4）、冻结恢复脚本
  离线重跑（输出重定向到临时目录）逐位复现派生摘要——标注为"派生
  摘要完整性"，非逐请求身份独立重建；PERTURBED 状态保留。
- 反例测试（test_offline_checks.py，当轮 30/30）：双位置/缺位/外来桶/总数
  不闭合/尾百分比不一致各自 FAIL；失败尝试后有效样本均值不变、重复
  invocation FAIL、缺块/rep 数/ops 不符/空输入 FAIL；合法微型样本手算
  聚合一致；CPU 三窗口合成；CSV 截短/多列；mermaid 结构。
- 旧缺陷暴露：v1 glob 逻辑对重复 invocation 双算（演示：3000 vs 真值
  1000 ns/op）、v1 F5 检查接受 {0,8} 两位置摘要——两个反例均在 v2 下
  FAIL。不再保留"13/13"旧计数；当前计数见 R10（复核二后 162 项校验
  全过 + 37/37 测试）。

## 二轮窄修（R7–R10，2026-11-07 复核二）

审阅锚点 `461cb267`（复核一修订头）。复核二确认 test_offline_checks.py
30/30 于其侧复跑通过；E5 原始数据的 60/60 与 75 值复算仍属 Flash 回执。
四项窄修，无新实验、无扩大审计。

| 发现 | 核验结果 | 改动文件 | 验证 |
|---|---|---|---|
| R7 C-A 比较对象 | 属实（reviewer 并自正其上轮表述）：C-A vs A0 为同分支产品路径、唯一自由度 flush 触发；混杂对照是 C-A vs R_cleanup | NEXT_EXPERIMENT §1/§8、DECISION | 9d1bf648 草案原文核验（"唯一自由度=flush 触发；无 adapter/seam/决策表"） |
| R8 "历史 R2"指认 | 确认为 PR #625 causal-r2（0a22a791，已在 master），非 r5-phase-a | NEXT_EXPERIMENT §2（对照表重写） | ROOT_CAUSE.md 冻结文本逐项引用 |
| R9 CPU 脚本自相矛盾 | 属实：primary_scope 仍写"io-wq not covered" | cpu_scope_recalc.py | 实数据重跑 exit 0；notes 全部 UNRESOLVED 口径一致 |
| R10 校验器假通过 | 属实：两条路径均在本地复现后才修 | verify_inherited_evidence.py + test_offline_checks.py | 两反例现在 FAIL；162 项校验全过 + 37/37 测试 |

### R7 — C-A 比较对象与可测净效应

- 原主张（本文件 R1 + NEXT_EXPERIMENT v2 §1）："C-A 与 A0 之差同时包含
  ready 构造、入口/锁路径、跨调用 SQE 持有、flush 时机、residue/driver
  依赖多项变化，任一结果都不能单独识别研究呈现层贡献。"
- 核验：对 **C-A vs A0** 不成立——草案（9d1bf648 §1）明确两臂同分支、同
  bench/fixture/线程拓扑/同锁、统一 T0、FLUSH_Q=1 逐语句等价现行为，
  "唯一自由度 = flush 触发；无 adapter、无 seam、无决策表参与任何臂"；
  ready 构造与入口/锁路径两臂相同，跨调用 SQE 持有与浅队列 residue 依赖
  是干预机制本身。reviewer 亦更正其复核一的对应表述。
- 修正后的准确边界：C-A vs A0 **可测该延迟 flush 策略的净效应**（有效实
  验）；不能回答的是"R_cleanup 退化是否来自研究层"——那需要 C-A vs
  R_cleanup，而该对比同时改变 bench/呈现、入口/锁+DecisionRecord、T0
  语义、flush 实现多项条件（识别混杂在此）。草案 §7 二选一结论撤回。
  HOLD 令牌沿用，语义在 NEXT_EXPERIMENT 头部精确化。
- 改动：NEXT_EXPERIMENT §1/§8、DECISION 裁决 1。

### R8 — "历史 R2" = PR #625 causal-r2

- 原主张（本文件 R1 末条）：按 r5-phase-a 归档 defer 家族理解，"若指其他
  文档请指出"。
- 核验：复核二指认为 **PR #625 `causal-r2`**（`docs/research/595-causal-r2/
  ROOT_CAUSE.md`，`0a22a791`，基线同为 `271b81bf`，A·IMMEDIATE vs
  B·BOUNDED_PROGRESS）。该轮已在产品路径上研究：五门准入 defer + SQE 留
  `transport_ledger_`；residue `signal_ready_progress()` → eventfd；flush
  从 dispatch 后移到下一次 poll（cap=8 量子 immediate 通道并存）；CF-1
  去通知（CPU 增量消失）与 CF-2 去 defer（延迟归零）效果分离；D16 改善
  −26.36% CPU 在 defer 链内。
- 修正：NEXT_EXPERIMENT §2 对照表按 #625 重写；撤回"起点不动的 defer 在
  产品路径上尚未测过"；C-A 增量收窄为"无门无条件 defer + W03 形状"，
  须先写成相对 #625 的增量假设。r5-phase-a 家族对照保留于 `461cb267`，
  其结论不受影响。
- 边界：不重做 R2；本轮只补对照文本。

### R9 — cpu_scope_recalc 机器输出自相矛盾

- 原主张（v2 脚本 notes.primary_scope）："io-wq kthreads not covered by
  this metric"——与正文/`io_wq_rusage_inclusion: UNRESOLVED` 矛盾（R4
  已将该排除结论降级为未核实）。
- 修复：primary_scope 改为"是否计入在本内核 UNRESOLVED（见
  io_wq_rusage_inclusion）"；机器输出不再保留已撤回的排除结论。实数据
  5 格重跑 exit 0，主口径点估计不变。

### R10 — 校验器两条假通过路径

- 复核二反例（修复前均于本地复现）：
  1. 冻结 `round-analysis.json` 置为 `[]` → 仍输出"75 个值一致"PASS 行、
     `Report.ok=True`（实际比较 0 个值）。根因：`compare_frozen()` 末尾
     无条件记成功，成功行中的"75"是硬编码描述而非实测计数。
  2. 仓库测试自己的 F5 正例（make_f5）：位置桶样本合计 216,800，恢复
     总量 480,000，无 unpositioned/terminal 解释，仍通过。根因：
     `f5_summary_checks()` 验证位置集合与整数尾数，未对账桶分母与总量。
- 修复：
  - `compare_frozen()`：期望比较集合 = 已核验 cell × 3 臂 × 5 指标，逐项
    记账；空/截断/缺项 → coverage FAIL；成功行报告实测计数（实数据
    75/75）。
  - `f5_summary_checks()`：按冻结恢复脚本的纳入/排除规则闭合（规则读自
    其**代码**：contiguous terminal 组的行**有**定位，仅 non-contiguous
    terminal 与 anomalous 组计入 unpositioned_rows——该脚本自身的
    semantics 字符串"terminal/anomalous groups excluded"不准确，冻结
    脚本不改，差异在此登记）：positioned = rows − unpositioned；已写桶
    n ≥ MIN_N=20；逐 case 残差 = positioned − Σ桶 n ∈ [0, 19·d(d+1)/2]
    （残差只能来自 sub-MIN_N 桶丢弃，桶键数上界 d(d+1)/2）；另加逐 tag
    groups 算术与 rows>0 ⇔ groups>0。实数据：d16 残差 17/界 2584、d4
    残差 7/界 190，通过。
  - 测试：两反例 + sub-MIN_N 桶 + 无组 tag 入负例（37/37）；make_f5 正例
    闭合化（旧 216,800 形状转为负例）。
- 边界：这证明校验能力此前有缺口，不证明真实数据错误——冻结恢复脚本
  离线重跑逐位复现仍是独立证据；CIs/Holm/门限仍不复算。

## 仍未核实项（诚实登记）

- io-wq worker CPU 是否计入子进程 rusage / PROCESS_CPUTIME（本内核
  UNRESOLVED，见 R4/R9）。
- 研究 enter/CQE 集中性、锁持有/等待时长、呈现间发布频率、观察顺序
  对 p99 差的作用（UNMEASURED/PERTURBED）。
- mermaid 渲染（无离线渲染器；结构检查通过）。
- C-A 相对 #625 causal-r2 的增量假设（无门无条件 defer + W03 形状）尚未
  成文，未申请授权（NEXT_EXPERIMENT §2/§6）。
