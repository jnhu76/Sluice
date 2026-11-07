# #595 Flow Audit — 下一步实验状态（C-A 撤回为 HOLD_DESIGN_CONFOUNDED）

**状态（2026-11-07 复核后）：C-A 未实施；本轮未获执行授权；其协议原稿存在
识别混杂，在解决下列问题前不得称为可执行协议。** 默认产品策略维持 A0；
原候选 B 维持不放行。本文件取代 v1 版"唯一下一项 = C-A"的表述。

## 1. 复核发现的识别问题（R1）

v1 协议声称 C-A（产品 transport 层 Q=8 延迟 flush，对 A0 产品臂 A/B）检验
"去除研究呈现层的效果"。该主张不成立：C-A 与 A0 之差同时包含多项变化——

- ready 构造（研究臂按组构建/呈现 vs 产品逐 op 内联构建）；
- 入口与锁路径（研究 seam+整批临界区 vs 产品逐 op 两级锁）；
- 跨调用持有 SQE（C-A 将已接受工作的 enter 推迟到量子满/residue/下一
  pass，改变了"accept 后何时到达内核"这一自由度本身）；
- flush 时机（1 enter/op → 1 enter/≤8 op）；
- residue/driver 依赖（浅队列下每 op 依赖 signal→wake→pass 才进内核）。

因此 C-A 的任一结果都不能单独识别"研究呈现层"的因果贡献：C-A p99 良好
不能证明 p99 差来自呈现层（其余协变因素同样不同）；C-A p99 失败也不能
证明"batching 内在代价"（见 §5）。同理，浅队列格上 C-A 的表现只能约束
"无研究层时该 flush 设计的行为"，不能把 d2serial 税归因到某一层。

## 2. 与历史 defer 设计的对照（归档：595-r5-phase-a NEXT_EXPERIMENT.md）

| 维度 | 原候选 B（residue 感知预算） | C-variant1（流式准备） | C-A（本撤回稿） |
|---|---|---|---|
| 延迟什么 | adapter 层延迟**接受/呈现**（预算耗尽即返回，后缀留给下一轮重建） | 延迟**构建**（ReadOp/T0 后移） | transport 层延迟**enter**（SQE 照常准备） |
| 依赖的残余机制 | 进度路径最终驱动（cv wait + driver poll） | 无（只是换测量起点，被归档判为"改指标不改管线"） | PROG-01 residue + pass 开头 flush |
| T0/测量起点 | **致命缺陷**：后缀 T0 后移 = 改测量起点（F3 禁止；归档已加 WARNING） | 同类缺陷（明示不推荐） | 每 op 构建时照常打点，起点不动（这是其相对 B 的真实差异） |
| 服务机会 | 创造真 park/发布机会（其机制主张） | 无 | 不创造 park；只改 enter 粒度 |
| 新状态 | adapter 预算计数器 | 无 | backend flush 量子（SQ 占用的派生摘要） |

相同点：三者都以"让后到达的工作更晚进内核"换取提交侧经济性，都依赖
进度路径兜底，都在后位成员上有尾延迟风险——这是整个 defer 家族的共性，
**"移到 transport 层"本身不构成新的实验价值**。不同点主要是测量起点
是否被移动与是否创造服务机会。相对 B/C-variant1 的唯一新增证据是
"起点不动的 defer 在产品路径上尚未测过"——一个未测事实，不是一个
已识别的因果问题。也不能仅因与 B 相似就断言 C-A 必然失败：B 的致命
缺陷（T0 后移）C-A 并不共享。

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
保护格 d4/d2serial/W02-d1/d2）作为形状先例可引用，但门限与口径需随
§3/§4 的修正重写后方可使用。
