# #595 Flow Audit — 路线裁决（≤500 字，2026-11-07 复核修订版）

**已确认的结构事实**：研究分支确有额外入口工作——seam 三重校验+整批
`access_mtx_`/`dispatch_mtx_` 临界区、每呈现 DecisionRecord+计数器、
adapter 呈现循环；产品 master 仅逐 op `submit_read`（正常非零读形状
每 op 一次 enter）。**归因限定**：这些工作对 singleton 退化与 d16
p99 差（运行级均值差 C−A0 9.52µs / R−A0 10.71µs，机器复算）的贡献
**尚未由单因素反事实独立识别**；现有证据既不能证明产品整体架构有问题，
也不能排除产品共享路径、完成服务或调用者反馈的作用。F5 位置条件尾风险
（0-7≈1.2% vs 8-15≈9.0%，cw8≈26%）保持 PERTURBED lead，非 p99 分解。

**净效应已知**：R5 指定形状上 R/C−A0 过程 CPU −22%/吞吐 +28%、p99 +18%
FAIL、浅队列格 FAIL、d4 全改善（过程 CPU 口径，非全系统）。批量减少
提交调用的机制有证据；成本变化尚未完全归因到 enter 本身。

**裁决**：
1. **C-A（transport 层 Q=8 延迟 flush）= HOLD_DESIGN_CONFOUNDED**：其
   A/B 同时改变 ready 构造、入口/锁路径、跨调用 SQE 持有、flush 时机与
   residue/driver 依赖，不能识别研究层因果贡献；未实施、未获授权。任何
   产品化批量入口仍须承担准入、生命周期与并发责任（"不在 master"≠
   "无产品价值"）。
2. **候选 B 维持不放行**（T0 起点缺陷未修复前提不变）。
3. **观察解耦（C-variant2）维持暂缓**。

**下一步**：完成本轮离线修正即止。新性能矩阵的前置 = 一个语义等价、
可识别的单一干预（现无：NO_COUNTERFACTUAL_SELECTED，见
NEXT_EXPERIMENT §7）。**默认产品策略维持 A0。**
