# #595 R3 续跑轮预注册附录（翻转 + 补测）

冻结时间：2026-11-05T14:28Z（任何计时开始之前；此后不得按结果修改）。
本文件是 `r3-completion-20261105T083818Z/evidence/PREREGISTERED_R3_COMPLETION.md`
（sha256 前缀 `1a9c1e12`，下称"主预注册"）的**附录**，不修改主预注册任何门槛数值；
它登记两类经所有者授权的增量，并声明本轮计时计划。

## 0. 授权与动机

所有者 2026-11-05 消息（原意转述，四点修正）：

1. 归档 patch 中尾延迟起点两臂不一致：B 在 `adapter->submit(...)` 整组返回后才记
   `submitted_ns`，A0 每条提交后即记；该口径不足以证明完整请求尾延迟非回归。
   须统一"应用已 ready → 应用观察到完成"口径（含准备、扫描与提交成本）。
2. 补翻转回执不会自动回答 W02 D2：主预注册 §14.2 翻转后 W02 新 B 仍因缺臂不激活；
   必须实际运行新 B 的 W02 D2（W03 d2serial 只是合成形状，不能替代）。
3. 不能由单条 enter 推断 ready 组很小：suffix 逐条提交是算法拆批，须区分进入算法前
   的组大小与拆批后提交大小，才可谈 census 差距归因。
4. 删除"物理收益上限不到 30%"类表述（cycles 差÷enter 是相关平均成本变化，不是
   enter 的独立因果成本；诊断 −14~−18% 也不能证明门余量）。

本轮优先交付两行结果：(a) W-03 d16 B−A0（同时报 B−A1、A1−A0）；
(b) W02 D2 新 B−A0（CPU、吞吐、完整请求尾延迟）。算法与调用者固定；cap 调整、
suffix 再成组、refill 粒度变化均为后续独立候选，不得用于补救本轮主实验。

## 1. 增量 A：统一完整请求延迟口径（bench-only，两基准面）

- 变更：`bench/bench_w03_external_loop.cpp` 与 `bench/bench_w02_pipeline.cpp` 的
  Inflight 时间戳改为 `ready_ns`，在**ReadOp 构造完成、任何臂特定提交工作之前**记录
  （两臂同一语义点）；延迟 = 结算观察时刻 − ready_ns。
  效果：A0 的每条 submit 成本、A1/B 的组准备/扫描/前缀批提交/suffix 逐条提交成本
  全部计入该请求的完整延迟；旧 `submitted_ns` 口径作废（新旧二进制不同 SHA，
  PROV-8 意义下不可跨二进制比较）。
- 证明为 bench-only：`libsluice_async.a` 与全部正确性/回归二进制在隔离 builddir
  `/tmp/r3c2-build` 重建后与主预注册 §3.2 冻结值**逐位相同**（见 §3 清单）。
  `bench/r3_ready_batch.hpp`（adapter 语义）零改动。
- delta patch：`evidence/delta_w03.patch`（sha256 `57f21e02…`）、
  `evidence/delta_w02.patch`（sha256 `4aa7b306…`）；
  含 delta 的全 worktree diff：`evidence/r3-worktree-full-diff.patch`
  （sha256 `73fa116f…`；主预注册冻结 diff 为 `c1a31a30…`，差异即本附录增量）。
- 新二进制（`/tmp/r3c2-build/linux/x86_64/release/`）：
  `bench_w03_external_loop` = `bb142d69788a…`，
  `bench_w02_pipeline` = `5104ce250e07…`。
- 门槛含义：§9.2 的 p95/p99 门改以统一口径判定（阈值不变：point 与 CI95 上界
  ≤ +5%）。统一口径同样适用于 A/A 功效检查。

## 2. 增量 B：W02 D2 新 B 臂（新 bench 面，主预注册明确为"超出本协议"）

- 案例冻结：`W02-uring-4k-d2`（4k、20,000 读、tmpfs、page-cache-resident、
  depth=2、uring），经 `SLUICE_BENCH_595_DEPTHS=1` opt-in 加入，默认重建不改变
  冻结 G0 案例表。
- 臂定义（同一 bench 二进制，`SLUICE_595_R3_ARM` 选择）：
  - **A0**：主预注册冻结的 RequestScope 逐条路径，源码零改动（PROV-6 口径）。
  - **B（新）**：同一滑动窗调用者形状（free_slots、最旧优先结算），提交策略替换为
    `bench::r3::Adapter`（context 级 ready-batch 入口；prefix=min(ready,cap=8,SQ,
    budget)，suffix 逐条，无再成组、无等待制造）。B 不经 RequestScope（batch 入口
    为 context 级）；最旧请求的等待用与 scope 内部相同的 `ctx.wait_one(deadline)`
    原语与 20s 期限，超时/中断/健康失败均为相位失败。非 uring 或 saturation 案例下
    臂环境即拒绝（NOT_RUN_WITH_REASON）。
- 计划：翻转后 fresh A/A（A0/A0，24 对）→ `W02R3-D2` 块（A0/B 交替，24 对）。
  报告 B−A0 的 CPU/op、吞吐、p50/p95/p99（统一口径）+ 机制计数（描述性）。
- 机制预期登记（**非门**）：该调用者稳定态每次 refill 只呈现 1 条 ready op
  （初始填窗 1 组×2 条），故 B 的 `entry_calls≈submits`、`group_calls≈1` 属预期；
  主预注册 §9.4 的 "B enter ≤ 0.5×A0" 机制门属于 W-03 批量调用者，**不继承**到
  本块。本块不设主门/ Holm 家族；结论只回答"新 B 在 D2 形状是否守住 IMMEDIATE"
  （对照 LEGACY-W02-D2 旧 B 的已证退化）。
- 调用者公平性说明：A0 与 B 共用同一滑动窗循环、同一结算顺序与同一等待原语；
  差异仅在提交策略（这正是被测对象）。refill 粒度不改（见 §0 授权边界）。

## 3. 本轮 §14.3 验证与翻转计划（计时前完成）

1. 独立复跑（持 `MEASUREMENT.lock`）：26 场景确定性套件、research/fault 回归、
   `shutdown_lifecycle_test`（全部使用冻结 SHA 二进制）。
2. 3 个变异体 RED → 干净重建逐位相同 → RESTORED GREEN 复跑。
3. 出具 `A0_AND_SUFFIX_RECEIPT.md`（A0 源码形状 vs baseSha 原始循环、suffix 规则、
   d16 机制计数对账）与 §14.3 翻转回执（7 项最低内容），
   声明 `semanticsReady=true`，适用范围：主预注册 §14.2 全部 B/A1 块
   （以增量 A 的新 bench 二进制执行）+ 本附录增量 B 的 W02R3-D2 块。
4. 翻转后、任何 B 计时前：fresh A/A（W03 d16 24 对、W02 d2 24 对，新二进制）。

## 4. 冻结声明

本附录之后，不得因测量结果调整：臂定义、案例参数、对数（24）、统计方法
（主预注册 §6：bootstrap 10,000、seed 5950、triplet 重采样、Holm）、门槛阈值
（§9.2/§9.3/§9.7）、口径（统一延迟）。若某块被阻断，按主预注册 §15 词汇登记
（BLOCKED_*/NOT_RUN/INCONCLUSIVE），不得以诊断数字替代。

## 5. 指纹

- 主预注册：sha256 前缀 `1a9c1e12`（未改动）。
- 冻结 patch：`2596f204…`（未改动，reverse-check 通过）。
- 本附录 + delta patch 清单哈希：见 `FLIP_MANIFEST.sha256`（翻转回执一并绑定）。
