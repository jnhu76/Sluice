# #595 Flow Audit — 下一步实验状态（C-A 撤回为 HOLD_DESIGN_CONFOUNDED）

**状态（2026-11-07 复核二后）：C-A 未实施；本轮未获执行授权。** 状态令牌
HOLD_DESIGN_CONFOUNDED 沿用，含义按复核二精确化：混杂指其**归因主张**的
对照（§1），不是判其净效应不可测。默认产品策略维持 A0；原候选 B 维持
不放行。本文件取代 v1 版"唯一下一项 = C-A"的表述。

## 1. C-A 的比较对象与它能回答的问题（R7 二轮修正）

C-A 草案（9d1bf648 §1）自身的 A/B 对比是：同分支、同 bench/fixture/线程
拓扑/同锁；A0 臂 = `SLUICE_URING_FLUSH_Q=1`（逐语句等价现行为）；两臂统一
T0 = submit 前；"无 adapter、无 seam、无决策表参与任何臂"，唯一自由度 =
flush 触发。因此：

- **C-A vs A0 是一个可测净效应的实验**：它回答"这套延迟 flush 策略（量子
  满内联 flush + residue 唤醒 + pass 头 flush）在预注册形状/门限下的净
  效果"。上一轮把该对比写成"同时改变 ready 构造、入口/锁路径……多项
  条件"是**错误的**：ready 构造与入口/锁路径在两臂中相同；跨调用持有
  SQE 与浅队列下的 residue/driver 依赖是干预本身的机制内容，不是混杂
  变量。此处按复核二修正（reviewer 亦更正其上轮表述）。
- **C-A 不能单独回答本档案的核心归因问题**——"R_cleanup 的退化是否来自
  研究层"。该归因需要的对照是 **C-A vs R_cleanup**，而这一对比同时改变
  多项条件：bench/呈现（产品逐 op vs 研究组呈现+adapter 循环）、入口/锁
  （逐 op 两级锁 vs seam+整批临界区+DecisionRecord/计数器）、T0 语义、
  flush 实现与触发。多项条件一次变化，识别不成立。草案 §7 的二选一结论
  （"p99 FAIL 且浅队列同时变好 ⇒ 浅队列税确证属研究层"）据此撤回：C-A
  结果只能约束"无研究层时该 flush 设计的行为"，不能把 R_cleanup 的退化
  归因到某一层。

HOLD 维持；理由 = 本节识别边界 + §2 与 #625 的重叠 + §3-§5 的协议缺陷。

## 2. 与历史 defer 设计的对照（R8：指认确认为 PR #625 causal-r2）

复核二澄清："R2 的历史 defer 设计"指 **PR #625 `causal-r2`**
（`docs/research/595-causal-r2/ROOT_CAUSE.md`，提交 `0a22a791`，已在
master；A·IMMEDIATE vs B·BOUNDED_PROGRESS），非 r5-phase-a 的候选
B/C-variant1——后者的对照表保留于 `461cb267`，其 T0 起点缺陷等结论不受
本轮指认修正影响。

| 维度 | #625 B·BOUNDED_PROGRESS（已执行） | C-A 草案（未执行） |
|---|---|---|
| 基线/路径 | 产品 master `271b81bf` + 隔离 worktree 补丁 | 同（自 master 切研究分支） |
| 延迟什么 | 五门准入（`defer_transport_flush_locked_`）通过后不 submit，SQE 留 `transport_ledger_` | 无条件：`submit_transport_locked` 不在每次 `dispatch_after_accept` 内调用 |
| flush 通道 | 下一次 `poll()` 无条件 flush（`uring_backend.cpp:652`）+ ledger **cap=8** 强制 immediate（D16 6.4% dispatch，一次 flush 8 SQE） | (a) burst ≥Q=8 内联 flush；(b) residue 广告唤醒；(c) pass 头 residue flush |
| residue 通知 | dispatch 尾 `unsubmitted_after_dispatch` → `signal_ready_progress()`（eventfd write） | 同一路径（草案 (b)"现有路径不变"） |
| 已裁决代价（W02-D2） | CPU +3.70%/+189.15 ns 由**通知**承担（CF-1 sigdrop 消除、消失 ≥95.2%、restore 复现）；p50/p95/p99 +6.91/+4.96/+7.07% 由 **defer 时序**承担（CF-2 deferoff 归零）；门链本身 ≈0（CF-0 CI 含 0；in-situ +6.6 ns/call） | 未测；通道 (b) 即 #625 已定位的 CPU 源 |
| 已裁决收益（W02） | D16 CPU −26.36%/p99 −28.68%、D3 −6.69%；deferoff 使之归零（+0.26%/+1.07%，CI 含 0）⇒ 改善在 defer 链内；无 restore 块；机制分解 PARTIAL（io-wq/用户分量未闭合） | 未测 |
| 形状/口径 | W02 D1/D2/D3/D16；直接测量（计数器/TSC/syscall/PMU）+ 单因素反事实 | W03-d16 主 + 保护格；R5 门限与 A/A 纪律 |

C-A 的机制空间几乎全部落在 #625 已测范围内：cap-8 量子内联 flush、residue
通知、pass 头 flush、深队列摊销收益、浅队列 defer 代价均已直接测量或反事实
分离。上一轮"起点不动的 defer 在产品路径上尚未测过"的说法**错误，撤回**。
C-A 的真实增量收窄为两点：(i) **去掉五门准入的无条件 defer**——门链 CPU
≈0（CF-0），但门决定浅深度下何时 defer（D2 上 witness 门使 defer 不减少
enter；无条件 defer 将把深度 2 的全部 flush 推到 pass 头，#625 的 L1 裁决
已给出其延迟代价的方向）；(ii) W03-d16 形状与 R5 门限。C-A 若申请授权，
必须先把这两点写成相对 #625 的**增量假设**并修复 §3-§5 的协议缺陷；在
此之前其新增研究价值未获证明——这是 HOLD 的对照依据，不是判其净效应
不可测。

## 3. 原协议条件 (b) 的歧义（未解决前协议不可执行）

原稿写"residue 广告（现有 `signal_ready_progress` 路径不变）"。冻结源码
（M:uring_backend.hpp:297 `progress_port_.signal()`；M:uring_backend.cpp:620-634
pass 开头的 residue flush）表明该信号**只唤醒等待者，不自行 flush**；真正
的 flush 发生在下一次 progress pass。但 C-A 新增的"量子满即 flush"通道
与 residue/pass 通道**在浅队列下产生的 batching 与等待行为不同**（前者
可能在 accept 线程内联 enter，后者必须等 wake→pass）。协议必须预注册
(b) 究竟指"立即内联 flush"还是"仅唤醒、由 pass flush"，两者是不同的
实验。原稿未区分——此为协议级缺陷，不是实现细节。

## 4. 撤回通用 enter 计数断言

删除 v1 §4 的 `enters=ceil(n/Q)+residue 次数` 断言。提前 flush、partial
submit、retry/control、等待型 enter 都会改变计数；通知次数也不等于实际
flush 次数。若未来协议需要计数示例，只能对**明确定义的确定性事件序列**
按实际提交调用逐次记账。任何未来机制测量的 kernel entries 必须独立统计
真实进入（并区分 submit、wait、control、probe enter），不得由 Q 推导。

## 5. 撤回强因果失败解释与杀死性负控的过强表述

- 失败解释撤回：新策略 p99 FAIL 只证明**该实现、该形状、该门限下失败**，
  不能自动推出"batching 内在代价"或"浅队列退化全部来自某一层"。
- 负控修正：v1 写"mutant 删除 residue 广播后 oracle 必须挂起"。该断言
  只有当测试确实构造了"driver 已睡眠、无后续 submit、无主动 poll、无
  其他唤醒、且该通知是唯一推进来源"时才成立。普通测试中 mutant 未挂起
  不能自动判 oracle 无效。

## 6. 当前唯一下一项

**完成本轮离线修正（CORRECTIONS.md R1-R6）本身。** 不以任何未经识别的
新候选替换 C-A。后续任何性能候选的前置条件：先给出一个**可删除的单一
干预步骤**与**语义等价证据**（oracle 逐字节等价 + 计数器冻结点决策），
再谈 A/B。

## 7. 尚未批准的研究问题（非施工授权）

保持相同 ready 集合、逐请求 T0（R 臂现为组构建循环内逐成员打点，
C:bench_w03:413）、呈现分组与顺序、flush 规则、完成与观察路径，仅删除
一个**已证明冗余**的研究入口步骤，观察净效应。

当前状态：**NO_COUNTERFACTUAL_SELECTED**。理由：r5-repair 已执行的
C1/C2（有界 eligible-prefix 扫描、pass-fact 信号改名）是仅有的两个带
等价证据的删除；研究路径剩余步骤中——seam precheck（phase-a Candidate A
明确拒绝：对 ineligible span 不可证 transport 等价）、DecisionRecord/
计数器（冻结 R4 join keys）、整批 dispatch 临界区（承载锁序与 residue
规则）——没有任何一个已具备语义等价证据。不为交付候选而改预算表或 cap。

## 8. 保留有效的部分

基线冻结对象、双锁纪律、A/A 半宽报告、NOT_ESTABLISHED 不补样、
"失败即停不做量子/阈值搜索"、RSS 门与无新线程/定时器约束——这些与
具体候选无关，任何未来协议可继承。C-A 原稿的形状选择（MAIN d16 +
保护格 d4/d2serial/W02-d1/d2）作为形状先例可引用，但增量假设（§2）
与门限/口径（§3/§4）须先重写后方可使用。
