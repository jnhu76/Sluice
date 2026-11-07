# A0 / suffix 独立回执（#595 R3 续跑轮）

verifier：ZCode（独立于实现者会话的验证轮）；日期 `2026-11-05T14:31Z`（`date -u`）。
对象：`/home/jnhu/sluice-595-e5/r3/worktree`（HEAD `0a22a791`，porcelain 16 行，
`git diff HEAD` sha256 `73fa116ff668ea58143ab6666d24684f0eea7f39fcb9e6c03572ae00a7df5560`）
= 冻结 patch `2596f204…` + 本轮 bench-only 增量（`delta_w03.patch` `57f21e02…`、
`delta_w02.patch` `4aa7b306…`；见 `PREREG_ADDENDUM_CONTINUATION.md` §1–2）。
adapter 语义文件 `bench/r3_ready_batch.hpp` 与全部库/正确性二进制**零改动、逐位相同**
（`libsluice_async.a` = `c7eed8da…`；26 场景确定性测试 = `c54a9222…`）。

## 1. A0 源码形状（vs baseSha 原始循环）

方法：`git show 0a22a791:bench/bench_w03_external_loop.cpp`（baseSha 原版）与当前
worktree 版逐段 diff（提交主循环区域）。结论：

1. A0 路径（无 adapter 的 `else` 分支）是原版循环**逐条语义保持**地提升为
   `for k < want` refill：per-item 的 slot/offset 推导、ReadOp 构造顺序、
   `++phase.submits` 位置、错误处理与 `window.push_back` 结构一一对应。
2. `max_refill == 0 → depth`，故 `want = min(depth−|window|, 剩余, depth)` 恰等于
   原版内层 while 在同一外层迭代里会执行的逐条次数；A0 行为不变。
3. PROV-6 锚点复核（当前行号）：adapter 仅在 `arm != a0` 时构造（构造点在
   phase 入口）；`ready/accepted` 向量仅在 adapter 存在时 reserve；A0 内联构造 +
   `ctx.submit_read`。arm 由 `SLUICE_595_R3_ARM` 环境选择，默认 a0。
4. bench_w02_pipeline：A0 = 冻结 RequestScope 逐条路径，源码未动（W02 原本不在
   冻结 patch 内；本轮 delta 新增的 R3 臂走独立函数 `run_pipeline_phase_r3`，
   A0 分派仍指向原 `run_pipeline_phase`）。

## 2. suffix 规则（静态审阅，`bench/r3_ready_batch.hpp:110-136`）

1. 前缀经 `TestAccess::context_submit_ready_reads(ctx, ready)` 一次成组提交；
2. suffix 从 `batch.untouched_begin` 起对剩余成员**逐条**调用同一 Context 入口
   （span=1），`++suffix_single_calls` 记账；
3. 无再成组、无等待制造、无跨调用缓存；`untouched` 计数与 suffix 提交数严格对应；
4. 非 uring / backend 缺失时回退 `submit_context_singles`（逐条公共入口），
   不改变 A0（A0 不经 adapter）。
本轮 delta 未触碰该文件（delta patch 仅两 bench .cpp）。

## 3. d16 机制计数对账（哈希绑定冻结证据）

来源（未改动，属完成轮归档）：
`r3-completion-20261105T083818Z/raw/mechanism/analysis/enter-d16-B.json`
（sha256 前缀 `86e08bbd`）、`enter-d16-A0.json`、`blocks.json`（前缀 `2872c9a1`）。

- A0：纯提交 enter 60,000（3 相位×20,000），全部 `to_submit=1`；driver 0。
- B（同一 strace 窗口）：纯提交 enter 29,225（−51.29%）；`to_submit` 直方图
  {1:24826, 2:1, 3:2, 6:1, 8:4395}；max=8；Σk·counts = 60,000 = 数据 SQE 数 ✓。
- 计时相位（bench_record）：`entry_calls=10515 == flush_attempts=10515`；
  `group_calls=1355`；`single_calls=9160`；`suffix_single_calls=9160 == untouched=9160`；
  `prefix_max=8`；恒等式 `1355×8 + 9160 = 20000`（=相位 ops）闭合；
  `accepted=20000`、`stats_submitted_ops=20000`、`stats_submit_calls=20000` ✓。
- 正控 d2pos：B 3×`to_submit=2`（每组 2 条一次 enter）；负控 d2serial：三臂
  60,000×`to_submit=1` 逐项相同（`enter-d2pos-*.json`、`enter-d2serial-*.json`）。

注（所有者修正 #3 已采纳）：以上 hist 是**进入 kernel 的 submit 尺寸**
（拆批后），不等于进入算法前的 ready 组大小；本回执只登记计数闭合，
不做 ready 组大小推断。

## 4. 结论

A0 形状与冻结基线一致、suffix 规则与预注册描述一致、机制计数恒等式全部闭合。
本回执可供 §14.3 翻转回执（`FLIP_RECEIPT_14_3.md`）哈希绑定。
