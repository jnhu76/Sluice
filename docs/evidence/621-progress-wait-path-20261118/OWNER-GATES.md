# Owner Gate 裁决记录 — #621 Progress/Wait 路径战役

| 字段 | 值 |
|---|---|
| 日期 | 2026-11-18 |
| 裁决者 | Repository Owner（会话内明示裁决；非代理、非自动） |
| 输入 | `FINAL-REPORT.md` §J.4（O-1–O-12 与建议列）、`p3/P3-ADMISSION-R1.md`、`p4/W3-ROWLEVEL-REVIEW.md`、`p4/M53-OBLIGATIONS.md`、`p2/P2-COST-MAP-R1.md` |
| 性质 | Owner 治理裁决记录。不改变任何技术证据与测量结论；`FINAL-REPORT.md` 中“#621 保持 OPEN，停在 Owner Gate”的表述以本记录为准收口 |
| 上游 | 终稿回写评论：#621 issuecomment-6981978538 |

## 1. 总裁决

接受 0 A/B 的研究结果，停止本轮实验预算。判词维持 `NO_FURTHER_OPTIMIZATION_JUSTIFIED`，范围限定为：**在当前已准入的候选与证据范围内停止新增 A/B**；不解释为“#621 不再存在任何潜在可优化的机器工作”。

区分两种“完成”：

- **本轮战役收口**：38 条准入记录、A/A 噪声带、全链路 profile、行级归因与三项迁移义务审计已形成明确结果——没有候选同时满足五项 A/B 准入条件。
- **尚未穷尽**：W3、M-53 与 W1-C2 仍有明确的前置设计或 Owner 授权条件（§3）。本轮的负结果是“这些方向目前不具 A/B 资格”，不是“这些方向已被永久否定”。

Issue 终态：**`RESEARCH_COMPLETED / ZERO_NEW_AB / NO_PRODUCT_CHANGE / NO_ADMITTED_CANDIDATES`**，随后以 Completed 关闭 #621；不宣称所有可能的优化均已被否定。

## 2. O-1–O-12 逐项裁决

| Gate | 裁决 | 原因 |
|---|---|---|
| O-1 | `WAIT_G0` | W1 的应用收益需要 #604 真实消费者准入 |
| O-2 | `REJECT_IN_621` | 不为减少一次 poll 而引入第二通知通道及新权威 |
| O-3 | `NOT_ACTIVATED` | 本轮无合法 W1 A/B，未来重开时另行冻结指标 |
| O-4 | `DEFER_W3` | 行级热点尚不是可删除成本；P-D 匹配 A/A 仍有噪声 |
| O-5 | `DEFER_M53` | O3 编码证明未完成，d64 收益不足以支持当前投入 |
| O-6 | `KEEP_QUEUED` | M-42 预计收益过小，不批准大型实验 |
| O-7 | `KEEP_PENDING_CONTRACT` | 六项语义冲突不作为性能优化；未完成契约审查前不擅自删除 |
| O-8 | `NO_REOPEN_NOW` | M-01 未获得推翻历史 free-list 负结果的证据 |
| O-9 | `ROUTE_TO_OWNER` | 提交策略归 #595，实际 workload 归 #604 |
| O-10 | `NO_GO_ABORT` | 保持 M-28 的 #637 裁决 |
| O-11 | `NO_GO` | M-61 payload 不存在新的生产收益证据 |
| O-12 | `NOT_APPLICABLE` | #634、#635 已产品化，本轮无新的候选 |

语义注释：`DEFER` 是**当前不资助**，而不是技术上永远不能优化。W3 的 `offer_terminal` 1.80% / `claim_execution` 1.56%（`p4/W3-ROWLEVEL.md` §6）是函数所有权总成本，不是可删除的净收益；M-53 的 d64 点估计（1.06%/1.13%）不显著且低于匹配 C7 MDE80（`p4/W3-ROWLEVEL-REVIEW.md:207`），不足以证明优化收益。

## 3. 重开条件（保留，不因关闭而失效）

- **W3（O-4）**：#604 G0 真实 workload 提供应用级 Request/数据搬运/I/O 成本，或新的机制证明（步间语义分离 + P-D 匹配 A/A 替代格，当前 C2b NOISY、无可用带）。
- **M-53（O-5）**：O3 packed 编码先在 HEAD 实现并验证（8 项改动 + envelope 控制 + `uring_*` 套件重跑，见 `p4/M53-OBLIGATIONS.md` §5）；此后 d64 cycles-only A/A 预注册——δ=2% 需 17–20 对；0.4% 量级需约 346/178 对（A/A+A/B 合计 ≥524 对），当前不资助该规模。
- **W1-C2（O-2）**：在 #621 内否决；若未来重启，属 `#617` G1 独立设计项目（第二通知通道 + 新权威），不作为 #621 收尾实验。
- **M-42（O-6）**：保持排队；隔离格 + mutant/release-seam 证明前不启动。
- **M-28（O-10）/ M-61（O-11）/ #637 其余终态**：维持，翻案条件见 `CAMPAIGN-LEDGER.md` §4.1 四要素，均未满足。

## 4. E5 频率排查收口

依据 `CAMPAIGN-LEDGER.md:343-348`（P0 门禁与 20×1s 采样）与 `FINAL-REPORT.md:201`：

- 单核 Busy MHz ≈3.48 GHz；四个物理核持续负载恒为 3200 MHz（Bzy_MHz 20/20=3200，CoreTmp 50.1–52.8°C）。
- 未观察到热节流（core/package throttle_count 前后均 0；`msr/cpu_thermal_margin` 37°C；dmesg 无 thermal throttle）。
- C3b/C7 的 CPU 时间不稳定主要与 system time 波动相关（`p1/AA-CALIBRATION-2.md`），**不能归因为已证实的 CPU 降频**。

因此不追求全核 3.5 GHz，不做 CPU 频率改造；功耗计数不可信，不得据此声明能效收益。频率排查就此结束。

## 5. 证据归档

- 证据按 #637 先例提交至**独立 evidence 分支**（分支名与固定 SHA 见 #621 Owner Gate 回写评论；master 不动）。
- 不保存可重建的构建日志：清单口径已排除 `*.log`（33 个、198 KB，本地标注保留，见 `EVIDENCE-MANIFEST-VERIFY.md` §3–§4）。
- 7.3 GB 本地原始件（`*.data`/`script-*.txt`/`report-*.txt`/`*.pyc` 等 162 项）不入 Git，按 `EVIDENCE-MANIFEST-VERIFY.md` §4 的“本地保留”标注与路径清单留验。
- 下一阶段方向：让 #604 提供真实应用的 Request、数据搬运与 I/O 成本，再由新证据决定是否重新打开 W3 或 M-53——而不是继续微调这套抽象的每一把锁。
