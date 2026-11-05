# #595 R3 — 证据口径基线（EVIDENCE BASELINE）

| 字段 | 值 |
|---|---|
| 状态 | **FROZEN**（2026-11-05，E5 会话） |
| 角色 | Sluice #595 R3 证据口径审阅员（只读） |
| 目的 | 冻结 R3 必须回答的 claim、R1/R2 已冻结数字、R3 建议门槛与 Issue/#604 的差异、真实 consumer 准入、本轮不可改动对象 |
| 方法 | 只读仓库文档 + `gh` 远端读取；**未运行任何构建或 benchmark**；只写入本文件 |
| 标注 | `[FROZEN]` 已在 R1/R2 冻结或远端原文；`[GATE]` 冻结门槛；`[REMOTE]` 远端原文；`[GAP]` 缺失/未报告/不一致；`[INFERENCE]` 本审阅推断（非冻结事实） |

## 0. 本轮实际执行的读取

### 0.1 仓库侧（全部只读）

- `/home/jnhu/Source/Sluice/AGENTS.md`（工作规则、权威顺序、测试/证据纪律）
- `/home/jnhu/Source/Sluice/docs/explicit-io-contract.md`（v2-r1；GOV-02、TERM-01、PROG-01/02/03、BACKEND-04、BOUND-01、VERIFY-01）
- `/home/jnhu/Source/Sluice/docs/roadmap/v2-conformance.md`（GOV-02 状态词汇、PROG 行）
- `/home/jnhu/Source/Sluice/docs/performance/methodology.md`
- `/home/jnhu/Source/Sluice/docs/research/595-causal-r2/{README.md,PROTOCOL.md,RESULTS.md,ROOT_CAUSE.md,CAUSAL_DAG.md,EVIDENCE_GAPS.md}`
- `/home/jnhu/Source/Sluice/docs/research/595-causal-r2/analysis/issue-comment-r2.md`
- `/home/jnhu/sluice-595-e5/evidence/measurement/PREREGISTERED.md`
- `/home/jnhu/sluice-595-e5/evidence/report-595-e5.md`
- 交叉核对（只读）：`/home/jnhu/sluice-595-e5/evidence/measurement/summary-tables.md`、`/home/jnhu/sluice-595-e5/evidence/measurement/patch.diff`
- 只读检查：`/home/jnhu/sluice-595-e5/r3/worktree` 当前为 `research-595-r3`，HEAD `0a22a791`，`git status --porcelain` 为空（未应用 R3 patch）。

### 0.2 远端侧（只读命令与结果）

```text
gh issue view 595 --json number,title,state,body
  → number=595, state=OPEN
gh api repos/jnhu76/Sluice/issues/comments/6624399984 --jq .body
  → R1 归档/裁决原文（KEEP_IMMEDIATE）
gh api repos/jnhu76/Sluice/issues/comments/6636745376 --jq .body
  → R2 归档/裁决原文（因果归因，不改 R1）
gh pr view 625 --json number,title,state,body
  → number=625, state=OPEN, title "[research record — do not merge]"
gh issue view 604 --json number,title,state,body
  → number=604, state=OPEN
```

补充只读核查（同一会话，非 ask 指定的最小集合）：

```text
gh api repos/jnhu76/Sluice/issues/595/comments
  → 最新 comment 6636745376（R2，2026-11-04T16:59:52Z）；没有 R3 裁决 comment
gh api repos/jnhu76/Sluice/issues/604/comments --jq '[.[] | select(.body|test("READY_FOR_595"))] | length'
  → 0
```

`[GAP]` 截至 2026-11-05：远端没有 R3 裁决；#604 的 body 与 comments 都没有 `READY_FOR_595` handoff。R1/R2 的 `PRODUCT=HOLD` 依据（`PREREGISTERED.md:139-140`）在本次核查时仍成立。

---

## 1. R3 必须回答的 claim、允许的结论词汇、明确禁止的外推

### 1.1 本轮必须回答的 claim

| # | Claim | 依据 |
|---|---|---|
| C1 | D2 主 shape（`W02-uring-4k-d2`）的 CPU/op、p50/p95/p99、吞吐的**块内配对差 + 95% CI**。必须声明符号约定，并明确区分 `phaseA`、`split-d2`、`CF-*` 是不同块，不可互相替代。 | `PROTOCOL.md:84-88`；`RESULTS.md:38`；`ROOT_CAUSE.md:20-23`；`issue-comment-r2.md:16-29` |
| C2 | D2 是否仍触发冻结的**性能保护门**：p95/p99 恶化 >5% 或吞吐下降 >3%；以及 CPU 是否仍在 5% 内。R1 的失败项是 p95/p99/throughput，**不是 CPU**。 | `PREREGISTERED.md:135`；`report-595-e5.md:126-144`；R1 comment §8（https://github.com/jnhu76/Sluice/issues/595#issuecomment-6624399984） |
| C3 | 产品净胜门：whole-executor/whole-system CPU 点估计 ≤ −10% 且 bootstrap 95% CI 上界 ≤ −5%；CPU 口径必须覆盖 submitter + kernel + io-wq（若用 SQPOLL 则含 poller），并显式披露未计入的 kernel-worker 成本。 | `PREREGISTERED.md:128-136`；issue #595（https://github.com/jnhu76/Sluice/issues/595）§Portfolio status “Required for #595 claims”；`report-595-e5.md:8-16` |
| C4 | 本轮 **fresh、同 host** 的 A/A 校准，CPU/tput/p99 半宽门 5/3/3 pp；不得用 R1/R2 的 A/A 结果替代本轮校准。 | `PREREGISTERED.md:50-54`；`PROTOCOL.md:101-116`；`methodology.md:10-18` |
| C5 | 冻结负控家族（D1/D2/D3、64 KiB d16、W-03 d4/d16/d64、threadpool、saturate）是否仍在 CPU/p95/p99 ≤5%、吞吐下降 ≤3% 内，并按 Holm–Bonferroni 单侧 α=0.05 判定。**R2 没有重跑该家族。** | `PREREGISTERED.md:55-58,135`；`PROTOCOL.md:160-163`；`report-595-e5.md:139-144` |
| C6 | 资源门：是否新增 queue/thread/timer/per-request allocation；peak RSS 增长是否 ≤ 基线 +2%。 | `PREREGISTERED.md:136`；`report-595-e5.md:184-187` |
| C7 | R2 遗留的 `PARTIAL`/`UNOBSERVED` 项是否关闭或必须维持：L2（p99−p95 ~+320 ns）段级机制（G12）、D3/D16 restore 与机制分解、io-wq 机制、C5 指令级来源、eventfd 内部阶段。没有新证据就不得升级为 `ESTABLISHED`。 | `ROOT_CAUSE.md:20-23,472-529`；`RESULTS.md:269-281`；`EVIDENCE_GAPS.md:568-612` |
| C8 | 正确性面：在 R3 **实际计时二进制**上重跑 mandatory oracles（595 oracle / deferred partial submit / cancel / poison / shutdown / prepark handoff），并给出 `BASE GREEN → NAMED MUTANT RED → RESTORED GREEN`。不得把 frozen-config 的收据当作新二进制的通过证明。 | issue #595 §4–§5；`ROOT_CAUSE.md:415-420,518-521`；`report-595-e5.md:50-68` |
| C9 | 机制账本与 enter 口径：必须区分 R1 的 **3.0002 enters/op**（submit+wait 合计）与 R2 的 **actual_enters = 1.0000/op**（每 op 的 submit enter）；报 `sqes_deferred`、`wakeups`/`writes`/`coalesced`、`submit_attempts`、`flushes`/`actual_enters`。 | R1 comment §10；`EVIDENCE_GAPS.md:616-622`；`RESULTS.md:92-106` |
| C10 | 真实 consumer 与 handoff：#604 的 G0 是否 `ADMIT`；是否 handoff `READY_FOR_595`。handoff 前 `PRODUCT=HOLD`，不得把 warm-tmpfs 微基准外推为应用加速。 | `PREREGISTERED.md:139-140`；issue #595 §6 Transferability；issue #604（https://github.com/jnhu76/Sluice/issues/604）§6 G0/G3；`report-595-e5.md:205-215` |

### 1.2 允许的结论词汇

| 词汇表 | 允许值 | 依据 |
|---|---|---|
| #595 closeout | `POLICY_PERF_SUPPORTED`, `PRODUCT_CANDIDATE_READY`, `KEEP_IMMEDIATE`, `KEEP_K4_RESEARCH_ONLY`, `BLOCKED_CORRECTNESS`, `INCONCLUSIVE` | issue #595 §7 “Closeout vocabulary”；`PRODUCT_CANDIDATE_READY` 明确“不是 merge 指令” |
| R1 预注册裁决 | `GO_MECHANISM`, `KEEP_IMMEDIATE`, `INCONCLUSIVE` | `PREREGISTERED.md:138` |
| R2 证据等级 | `ESTABLISHED（源级）`, `PARTIAL`, `UNOBSERVED`, `CORRELATION_PARTIAL`, `OBSERVABILITY_LIMIT` | `ROOT_CAUSE.md:20-23,472-529`；`README.md:34-37` |
| v2 一致性台账（仅用于生产一致性声明） | `NOT_ASSESSED`, `GAP`, `IMPLEMENTED_UNVERIFIED`, `VERIFIED`, `DEFERRED_OUT_OF_SCOPE` | `explicit-io-contract.md:62-78`（GOV-02）；`v2-conformance.md:20-31` |
| #604 workload 研究 | `GO`, `CONDITIONAL_GO`, `NO_GO`, `NO_CHANGE_JUSTIFIED`, `INCONCLUSIVE`, `NOT_RUN_WITH_REASON` | issue #604 §6 |

约束：

- 必需义务（required obligation）**不能**用 `DEFERRED_OUT_OF_SCOPE`（`v2-conformance.md:31`）。
- `PRODUCT_CANDIDATE_READY` 不等于可合并；`MERGED: NO` 仍是 R1/R2 的冻结状态（`README.md:11`；R1 comment §13）。
- #604 的 `GO`/`NO_GO` 等是 workload admission/transferability 的词汇，**不能**替代 #595 的 closeout 词汇。

### 1.3 明确禁止的外推

| # | 禁止外推 | 依据 |
|---|---|---|
| F1 | 不得超出 `W02-uring-4k` warm-tmpfs 4 KiB read 形状（D1/D2/D3/D16）；不覆盖真实块设备、cold cache、O_DIRECT、其他 block size、其他 backend/workload。 | `README.md:135-145`；`ROOT_CAUSE.md:655-661`；`RESULTS.md:299-305` |
| F2 | `deferoff` 变异体按设计**不满足** arm-B held-batch 进度契约（595 oracle rc=134）；不得把“defer 链承担全部效应”外推到 held-batch 停滞、取消控制、poison 路径。 | `ROOT_CAUSE.md:25-30,509-512`；`README.md:140-142`；`RESULTS.md:206-208` |
| F3 | 82% 是**上限口径**，不是 100% 精确分解；C5 指令级来源、eventfd 内部阶段、残差 ≈27–35 ns/op 仍是 `CORRELATION_PARTIAL`/`UNOBSERVED`。 | `ROOT_CAUSE.md:20-23,245-259`；`README.md:142-145` |
| F4 | 延迟 restore 只成立为“同向/同域”，不是逐分位点的精确重现；深度反事实**没有 restore 块**。 | `ROOT_CAUSE.md:301-307,396-400`；`README.md:146-147` |
| F5 | “全部改善由 enter 摊销承担”超出证据：D3/D16 的提交线程内核分量约 54.8%/50.3%，io-wq 约 29.1%/21.0% 机制 `UNOBSERVED`，用户约 16.8%/28.8% 只是间接解释。 | `ROOT_CAUSE.md:401-408,526-529`；`RESULTS.md:228-235` |
| F6 | L0（通知）只排除“主导”，不是“零贡献”；~1pp 残余未单独量化。 | `ROOT_CAUSE.md:185-193`；`RESULTS.md:186-189` |
| F7 | ≥96.2% 是 **p99 总效应**的界，不是 p99−p95 附加尾部（~+320 ns）的界；后者 CI 未计算、段级机制未观测。 | `ROOT_CAUSE.md:204-209`；`RESULTS.md:153-155` |
| F8 | 不得把双仪器 xlayer run 的时延数字用于性能归因；xlayer 只作结构/次序/关联验证。 | `ROOT_CAUSE.md:670-672`；`RESULTS.md:252-267` |
| F9 | R1 与 R2 是不同会话/二进制，数值不可互换引用；块间绝对 CPU/op 不可直接相减（块间漂移 2.67–4.52%）。 | `RESULTS.md:15,47-49`；`ROOT_CAUSE.md:57-72` |
| F10 | 不得从 tmpfs 外推到 btrfs/ext4/设备；xlayer 的 E3（btrfs dirty-fsync）只是独立场景的次序验证。 | `ROOT_CAUSE.md:655-661`；`README.md:137-139` |
| F11 | 不得声称任何公开 API / v2-r1 契约变更；R3 的改动只能是研究副本。 | `ROOT_CAUSE.md:648-654`；issue #595 §2；issue #604 §7；`AGENTS.md:3-5` |
| F12 | 不得把 `KEEP_IMMEDIATE` 改写为“机制被否定”：R1 的结论是“机制得到支持 ≠ 当前策略可部署”。 | R1 comment §13；`report-595-e5.md:8-16` |

---

## 2. R1/R2 已冻结的 D2 数字、门槛与旧 B 语义

### 2.0 先读：符号约定是跨文档不一致的（重要）

`[GAP]` 冻结分析输出的约定是：

- CPU/p50/p95/p99：`(B−A)/A`，**正 = B 更差**；
- 吞吐：`(A−B)/A`，**正 = B 更慢/更差**（所以 B 更快时是负数）。

依据：`/home/jnhu/sluice-595-e5/evidence/measurement/summary-tables.md:3`；`docs/research/595-causal-r2/PROTOCOL.md:84-88,130`；`docs/research/595-causal-r2/RESULTS.md:9-12`。

但叙事文档存在相反的“drop/improvement 正号”写法：

- R1 report §5.1 写 `throughput +36.07% (B faster)`（`report-595-e5.md:105`），而同一主 shape 的冻结表是 `−36.07%`（`summary-tables.md:9`）；R1 report §5.3 的 D2 又写 `+3.99%`（`report-595-e5.md:129`），与冻结表一致。
- R1 comment §6 写 `+36.07%`，§7 写 D2 `−3.99%`（drop 号），§8 写“吞吐 −3.99%（>3% 下降）”。
- R2 `README.md:42` 写 `throughput −3.58%`（drop 号）；`RESULTS.md:38` 与 `PROTOCOL.md:126` 写 `+3.58%`（B 更差）。
- R2 `ROOT_CAUSE.md:64` 写 R1 “throughput +3.99 超 5/3% 界”（更差号），`:218` 写 split-d2 “吞吐 −3.55%”（drop 号）。
- R2 `PROTOCOL.md:136` 在同一句里把 R1 `tput −3.99%` 与 R2 `+3.58%` 对比（混合符号）。

`[INFERENCE]` 这不是数值冲突，而是**同一 magnitude 的符号/表述冲突**。R3 必须在自己的表头声明唯一约定，并在引用 R1/R2 时先归一化；不得把 `−3.58%` 与 `+3.58%` 当成两个不同结果，也不得据此做跨文档比较。

### 2.1 D2 headline（R1 与 R2 分列；24 对；正 = B 更差，除特别注明）

| 块 | CPU/op | p50 | p95 | p99 | 吞吐（magnitude；符号见 §2.0） | 来源 |
|---|---|---|---|---|---|---|
| **R1 D2**（R1 二进制/会话） | **+4.17%** [+3.53,+4.83] | **未报告 / 未冻结** `[GAP]` | **+5.69%** [+4.90,+6.50] | **+7.45%** [+5.77,+9.38] | **3.99%**（冻结表 `+3.99%` [+3.40,+4.60]；R1 comment 写作 `−3.99%` drop） | `summary-tables.md:11`；`report-595-e5.md:129`；R1 comment §7–§8 |
| **R1 D2 replication** | +3.05% [+2.41,+3.71] | 未报告 | +4.98% [+4.22,+5.78] | +5.81% [+3.36,+7.83] | 2.95% [+2.34,+3.56] | `summary-tables.md:12`；`report-595-e5.md:130` |
| **R2 phaseA D2**（独立会话/二进制 `61c6d579…`） | **+3.73%** [+3.04,+4.42] | **+6.91%** [+5.03,+9.23] | **+4.96%**（CI 两版本，见 §2.7） | **+7.07%**（CI 两版本，见 §2.7） | **3.58%** [+2.94,+4.22]（B 更差；README 写作 `−3.58%` drop） | `PROTOCOL.md:126`；`RESULTS.md:38`；`README.md:41-42` |
| **R2 split-d2**（另一块，12 对） | **+3.70%** [+2.93,+4.55]，+189.15 ns/op | — | — | — | 3.55% [−4.34,−2.84]（`ROOT_CAUSE.md:218` 的 drop 号；块间不可与 phaseA 混用） | `RESULTS.md:51-76`；`ROOT_CAUSE.md:213-218,236-240` |
| **R2 CF-0 B−A D2** | +3.21% | — | — | — | — | `RESULTS.md:163-165`；`ROOT_CAUSE.md:431` |
| **R2 CF-1 base D2** | +3.34% [+2.30,+4.35] | +2.56% | +4.42% | +5.03% | — | `RESULTS.md:173-181` |
| **R2 CF-2 base D2** | +3.86% [+2.38,+5.40] | +7.46% | +5.42% | +6.81% | — | `RESULTS.md:195-201` |

冻结解释：

- `[FROZEN]` R1 与 R2 方向/量级同域（CPU 4.17 vs 3.73；p95 5.69 vs 4.96；p99 7.45 vs 7.07），但**不可互换引用**（不同会话/二进制/块）。`RESULTS.md:47-49`；`ROOT_CAUSE.md:57-72`。
- `[FROZEN]` R2 phaseA 的 D2 是**phaseA 的结果，不是 R1 冻结基线**。`ROOT_CAUSE.md:67-72`。
- `[GAP]` R1 D2 的 **p50 没有在归档报告中出现**：`report-595-e5.md:124-144` 的 §5.3 只有 CPU/throughput/p95/p99；`summary-tables.md:11` 也只有 cpu/tput/p95/p99/rusage。R3 若要 p50，只能自行测量或从 R1 原始 JSONL 重算；**本基线不得补写一个 R1 p50**。
- `[GAP]` R2 phaseA p95/p99 的 CI 在文档间有两个版本（点估计一致）：`PROTOCOL.md:126`、`RESULTS.md:38`、`ROOT_CAUSE.md:734` 为 p95 [+4.21,+5.75]、p99 [+4.19,+11.80]；`ROOT_CAUSE.md:213-215,284-286` 与 `CAUSAL_DAG.md:86-88` 为 p95 [+4.23,+5.74]、p99 [+4.24,+11.78]。未重算、未择一。

### 2.2 A/A 半宽门（冻结）

| 项 | 冻结值 | 来源 |
|---|---|---|
| 门槛 | CPU 半宽 ≤ **5 pp**、吞吐 ≤ **3 pp**、p99 ≤ **3 pp** | `PREREGISTERED.md:50-54`；`PROTOCOL.md:86-88` |
| 适用方式 | 在任何 A/B 之前，同 host 先做 24 对 A/A；失败时**只允许一次**测量条件修复（保留修复前后回执，阈值不变）；仍失败则该指标 `INCONCLUSIVE_NOISE` | `PREREGISTERED.md:50-54,101-123`；`PROTOCOL.md:60-61` |
| R1 修复前 | cpu 2.44 pp PASS / tput 2.50 pp PASS / **p99 3.71 pp FAIL** | `report-595-e5.md:80-87,118`；`summary-tables.md:50` |
| R1 修复后（`WARMUP=1 MEASURED=5`，pair-member 聚合） | **0.44 / 0.43 / 1.08 pp PASS** | `report-595-e5.md:84-87,119`；`summary-tables.md:51` |
| R1 D2 noise floor | 0.53 / 0.53 / 1.61 pp（half-width） | `report-595-e5.md:120`；`summary-tables.md:52` |
| R2 phaseA A/A（d16） | **0.65 / 0.64 / 1.66 pp PASS** | `PROTOCOL.md:105-116`；`RESULTS.md:28` |
| R2 CF-0 A/A（12 对） | **1.47 / 1.51 / 2.95 pp PASS** | `RESULTS.md:165`；`ROOT_CAUSE.md:76-78` |
| 修复使用与记录 | R1 的 `PREREGISTERED.md` 记录了一次 sanctioned repair（m1→m5，`PREREGISTERED.md:101-123`；`report-595-e5.md:80-87`）。R2 的 A/A 直接 PASS，未申请新的修复；但 `PROTOCOL.md:111-113` 写“the R1 single-repair option remains unspent”，与 R1 已执行 m1→m5 的记录矛盾（疑似把 R2 误写成 R1）`[GAP]` | `PREREGISTERED.md:101-123`；`report-595-e5.md:80-87`；`PROTOCOL.md:111-113` |

`[INFERENCE]` R2 的 A/A 是在 `W02-uring-4k-d16` 上做的，不是 d2；R1 的 D2 noise floor 0.53/0.53/1.61 pp 是 R1 自己的 A/A 块。R3 若要在 d2 上做产品声明，必须给出与声明 shape 匹配的 A/A 证据，不能只引用 d16 的 A/A 数字。

### 2.3 负控门 / 性能保护门（冻结）

`PREREGISTERED.md:128-136` 的冻结门槛（注意：`PREREGISTERED.md:128` 自述“from the issue text”，但 2026-11-05 的 live issue #595 body 没有这些数字——见 §3.1）：

| 门 | 冻结条件 |
|---|---|
| Correctness | 所有 mandatory oracles 通过；data/result/cancel/lifetime/resource semantics 等价（Step-3 receipts） |
| Mechanism | W-02 4k d16：pure submit enters per valid read 相对 A 减少 **≥50%**；data SQE/CQE ledger 对账（strace enters vs completed ops） |
| Net win | whole-executor CPU per useful task：点估计 **≤ −10%**，bootstrap 95% CI 上界 **≤ −5%**，在 unfavorable attribution end |
| Performance protection | 主 shape：吞吐下降 **≤3%**，p95/p99 恶化 **≤5%**。负控（D1/D2/D3、64 KiB d16、W-03 d4/d16/d64、threadpool、saturate）：CPU/p95/p99 各自恶化 **≤5%**，吞吐下降 **≤3%**（Holm family） |
| Resource | zero new queue/thread/timer/per-request allocation；peak RSS growth **≤2%** |

`[FROZEN]` R1 负控结果（24 对；positive = B worse）：

| shape | CPU | 吞吐（R1 report 的 `+` 号=更差） | p95 | p99 | 判定 |
|---|---|---|---|---|---|
| D1 | +0.09% | +0.08% | +0.07% | +0.48% | PASS |
| **D2** | **+4.17%** [+3.53,+4.83] | **+3.99%** [+3.40,+4.60] | **+5.69%** [+4.90,+6.50] | **+7.45%** [+5.77,+9.38] | **FAIL** |
| D2 replication | +3.05% | +2.95% | +4.98% | +5.81% | 方向确认 |
| D3 | −6.81% | −7.34% | −11.57% | −9.13% | PASS（改善） |
| 64 KiB d16 | −0.72% | −0.82% | +0.02% | −0.00% | PASS |
| W-03 d4 | +0.07% | −0.02% | +0.61%（噪声底 3.88 pp） | +0.72% | 均值在带内；p95 **未建立** |
| W-03 d16 | −0.05% | −0.07% | +0.11% | +0.03% | PASS |
| W-03 d64 | +0.11% | +0.06% | +0.33% | −0.31%（噪声底 7.21 pp） | 均值在带内；p99 **未建立** |
| threadpool d16 | +0.25% | +0.20% | +0.18% | +0.22% | PASS |
| saturate | −35.70% | −56.14% | n/a（lat_n=0） | n/a | PASS |

来源：`report-595-e5.md:124-144`；R1 comment §7。Holm–Bonferroni 家族（9 shapes × metrics，one-sided non-inferiority，α=0.05）的失败项：D2 p95/p99/throughput（真实）、W-03 d4 p95 / d64 p99（功效不足，非已证伤害）；**D2 的 CPU 两跑都在 5% 界内**。`report-595-e5.md:139-144`；R1 comment §8。

`[FROZEN]` R2 **没有重跑**该负控家族：R2 phaseA 只跑 A/A → D1 → D2 → D3 → D16；64 KiB、threadpool、saturate、W-03 家族、placement、Holm gate 均未跑。`PROTOCOL.md:160-163`。`[INFERENCE]` 因此 R2 不能单独证明冻结负控门仍全通过；R3 若做产品声明，必须补该家族或明确记录未跑。

### 2.4 旧 B（BOUNDED_PROGRESS）的 defer 判据、flush 时点、residue 通知（冻结）

`[FROZEN]` 判据（按冻结研究补丁 `patch.diff` 的源码顺序；`bounded_progress_` 是 arm-B 模式本身，后四条是准入谓词）：

1. `bounded_progress_`：构造时 `SLUICE_595_ARM` 以 `B` 开头才为真；否则立即走 immediate。`patch.diff:107`；`report-595-e5.md:26-48`。
2. **owner**：`progress_driver_seen_ && std::this_thread::get_id() == progress_driver_recent_`；driver 在 `poll()` 的 `dispatch_mtx_` 下用 `note_progress_driver_locked_()` 记录。`patch.diff:127-131,162-175`；`report-595-e5.md:30-34`。
3. **payload**：`prep.kind == detail::OperationKind::read && prep.native_length == 4096`。`patch.diff:176-182`；`report-595-e5.md:33`。
4. **witness**：`live_cookies_.load(std::memory_order_relaxed) > transport_ledger_->size()`（“kernel-visible un-retired witness”；没有 witness 就保持 immediate）。`patch.diff:183-190`；`report-595-e5.md:34-36`。
5. **cap**：`transport_ledger_->size() < kDeferCap`，`kDeferCap = 8`。`patch.diff:57,191-195`；`report-595-e5.md:36`。

`[FROZEN]` flush 时点：

- 下一次 progress pass 的现有 `submit_transport_locked()`（`poll()` 入口）；`ROOT_CAUSE.md:146-151`（`src/async/uring_backend.cpp:652-657`，flush :652，reap :657）。
- cap（8）、SQ-full/recovery、任何 ineligible arrival；parking owner 在 park 前 pass；既有 10 ms dispatch retry 约束 parked owner；control/cancel 保持**既有**排序，没有新增 flush。`report-595-e5.md:37-43`。
- deferred 分支仍调用 `service_pending_controls_locked_()`；immediate 分支先 `submit_transport_locked()` 再 service controls。`patch.diff:127-135`。
- D2 的 flush size 全为 1（`{1:20000}`），**没有跨请求 batching**；deferred SQE 由下一 poll 的无条件 submit 吸收。`RESULTS.md:215-221`；`ROOT_CAUSE.md:371-379`。

`[FROZEN]` residue 通知：

- `dispatch_after_accept()` 尾部 `advertise_residue = injected_dispatch_failure || newly_poisoned || unsubmitted_after_dispatch || post_poison_push_retired`，为真则 `signal_ready_progress()`。`patch.diff:148-155`。
- CF-1 `sigdrop` 只移除 `unsubmitted_after_dispatch` 这一项；deferral/门链/flush 时点不变。`patch.diff:148-153`；`ROOT_CAUSE.md:239`；`RESULTS.md:171-188`。
- 计数：`sqes_deferred = 0.5589/op`、`wakeups = 0.5589/op`、`wakeup_writes = 0.2353/op`、`wakeup_coalesced = 0.3236/op`、`flushes = actual_enters = 1.0000/op`、`submit_attempts 1.7893→1.2354`、`poll_passes 0.7942`、`progress_handoffs 0`。`RESULTS.md:92-106`。
- P3（notification coalescing）**未尝试**；per-deferral residue signal 作为 strict owner gating 下的保守冗余保留；没有 coalescing 声明；eventfd registrations 不变（4–9/run）。R1 comment §P3；`RESULTS.md:171-188`。
- `[GAP]` R2 文档把门链称为“五门”，`ROOT_CAUSE.md:101` 的括号只列 `owner/payload/witness/cap`；`CAUSAL_DAG.md:36,93` 明确是 `bounded→owner→payload→witness→cap`。按 `patch.diff` 源码，实际是 **5 个检查**（含 `bounded_progress_`）；R1 report 的“four conditions”指 B 模式内的四条准入谓词。引用时不要写成“四门”或漏掉 bounded。

### 2.5 机制/成本/反事实（R2 冻结）

| 项 | 冻结值 | 来源 |
|---|---|---|
| R2 split-d2 总 CPU | **+3.70%** [+2.93,+4.55]，**+189.15 ns/op** | `RESULTS.md:60,74`；`ROOT_CAUSE.md:236-240` |
| 线程分解（D2） | 用户 **+95.24**、提交线程内核 **+79.37** [37.8,124.6]、io-wq **+7.94** [−0.44,+1.36] n.s. ns/op | `RESULTS.md:67-76` |
| C3 eventfd write | on-CPU **+132~138**（raw +138.25）；syscw **+86,912** [86,541,87,263]/inv；**8.005 B/次**；0.24078/op；单次 562.7–592.5 ns/call | `RESULTS.md:108-121`；`ROOT_CAUSE.md:163-169` |
| C1 signal | **277.5 ns/call**（clean；mutex+epoch 35.3；write 592.5）；signal 155.8 ns/op；新增用户态执行仅 ≈24–27 ns/op | `RESULTS.md:53-58`；`ROOT_CAUSE.md:159-162` |
| C1 flush | 702.3→714.4 ns/call（+12.1），1.0/op | `RESULTS.md:57` |
| C5 微架构停顿 | cycles:u **+5.71%** vs instructions:u **+1.06%**（+50/op）；IPC **−4.36%**；cache-ref **+23.31%**；branch-miss **+14.94%**；≈68–71 ns/op `CORRELATION_PARTIAL` | `RESULTS.md:78-90`；`ROOT_CAUSE.md:170-175` |
| 残差 | ≈27–35 ns/op `UNOBSERVED`；82% 是上限口径 | `ROOT_CAUSE.md:240,245-259` |
| C4 调度 | runnable delay +36.4 ns/op（+0.94% [+0.38,+1.50]）= on-CPU 增量的 7.7%；timeslices +0.12% n.s.；migrations 0；progress_handoffs 0 | `RESULTS.md:123-134` |
| CF-1 merged | frozen **+3.34%** [+2.62,+4.07] → sigdrop **−0.26%** [−0.67,+0.16]；restore +3.34% [+2.40,+4.38]；延迟 frozen p50/p95/p99 +5.38/+4.73/+5.85，sigdrop +4.30/+4.28/+4.87 | `RESULTS.md:171-188`；`ROOT_CAUSE.md:294-307`；`ROOT_CAUSE.md:707-712,728-729` |
| CF-2 merged | frozen **+3.90%** [+2.91,+4.91] → deferoff **−0.76%** [−1.61,+0.06]；restore +3.94% [+2.60,+5.26]；延迟 frozen +6.26/+5.19/+6.03，deferoff **−0.71/−0.87/−0.83** | `RESULTS.md:191-208`；`ROOT_CAUSE.md:713-718,730-731` |
| CF-0 | C−A D2 **−0.52%** [−1.28,+0.22]、D16 **−0.13%** [−1.45,+0.82]；B−A D2 +3.21%、D16 −26.80%；A/A 1.47/1.51/2.95 pp PASS | `RESULTS.md:159-169`；`ROOT_CAUSE.md:431` |

`[FROZEN]` 过度消失：CF-1 点估计 107.8%、CF-2 点估计 119.4% 的“消失比例”>100%，意味着干预后 B 臂净露出 ≈0.3–0.8% 的微小优势；不单独归因，CI 含 0。`ROOT_CAUSE.md:441-443`。

### 2.6 深度边界（冻结，`PARTIAL`）

| 项 | D1 | D2 | D3 | D16 | 来源 |
|---|---|---|---|---|---|
| phaseA CPU/op | **−0.07%** [−0.73,+0.63] | **+3.73%** [+3.04,+4.42] | **−6.69%** [−7.25,−6.18] | **−26.36%** [−26.66,−26.08] | `PROTOCOL.md:125-128`；`RESULTS.md:37-40` |
| phaseA 吞吐（B 更差号） | −0.10% | +3.58% | −7.20% | −35.88% | `PROTOCOL.md:125-128` |
| phaseA p50/p95/p99 | −0.72/−0.06/−0.14 | +6.91/+4.96/+7.07 | −3.07/−11.38/−9.57 | −32.31/−31.00/−28.68 | `RESULTS.md:37-40` |
| `sqes_deferred` | 0 | 0.5589 | 0.6904 | 0.8762 | `RESULTS.md:214` |
| `flushes = actual_enters` | 1.0000 | 1.0000 | 0.6905 | 0.1974 | `RESULTS.md:215` |
| SQE/enter | 1.000 | 1.000 | 1.448 | 5.066 | `RESULTS.md:216` |
| defer→flush delay p50/p90/p99（ns） | — | 617/1234/2777 | 706/3210/5419 | 8363/14462/18919 | `RESULTS.md:220` |
| deferoff 复用（8 对/深度） CPU/op | — | — | **+1.07%** [−0.47,+2.80] | **+0.26%** [−0.69,+1.04] | `RESULTS.md:242-243`；`ROOT_CAUSE.md:390-395` |

冻结边界：

- `[FROZEN]` 深度反事实**没有 restore 块**；phaseA 的 −6.69%/−26.36% 是干预前、不同会话/二进制的测量，不构成 restore。`ROOT_CAUSE.md:396-400`；`RESULTS.md:245-246`。
- `[FROZEN]` depth 的 `bench_w02_pipeline_deferoff`（`64189aa8…`）**没有重跑 595 oracle**；正确性面只有 exit 0 + 字节 oracle + 400k measured op/inv。`ROOT_CAUSE.md:415-420,518-521`；`RESULTS.md:247-248`。
- `[FROZEN]` D3 的 deferoff 后 p95 **+1.56%** [+0.16,+3.23] 显著、p99 **+4.60%** [+0.83,+8.98] 显著、p50 +0.49% [−0.30,+1.42] n.s.；D16 三个分位均 n.s.。`ROOT_CAUSE.md:409-414`；`RESULTS.md:242-243`。
- `[FROZEN]` 因此 `DEPTH_CAUSE = PARTIAL`；“全部改善由 enter 摊销承担”超出证据。`ROOT_CAUSE.md:421-423,526-529`。

### 2.7 已冻结的文档级缺口/不一致（R3 不得静默修正）

| # | 缺口 | 证据 |
|---|---|---|
| D-1 | **R1 D2 p50 未报告** | `report-595-e5.md:124-144`；`summary-tables.md:11` |
| D-2 | R2 phaseA p95/p99 CI 两版本（点估计一致） | `PROTOCOL.md:126`、`RESULTS.md:38`、`ROOT_CAUSE.md:734` vs `ROOT_CAUSE.md:213-215,284-286`、`CAUSAL_DAG.md:86-88` |
| D-3 | 吞吐符号约定跨文档/跨块不一致（§2.0） | `summary-tables.md:3` vs `report-595-e5.md:105`；`README.md:42` vs `RESULTS.md:38`；`ROOT_CAUSE.md:64` vs `:218` |
| D-4 | “五门”括号漏写 `bounded` | `ROOT_CAUSE.md:101` vs `CAUSAL_DAG.md:36,93`；`patch.diff:162-195` |
| D-5 | R2 未重跑冻结负控家族 | `PROTOCOL.md:160-163` |
| D-6 | live issue #595 body 没有 PREREGISTERED 声称的数值门槛（10/5/3/2%） | `PREREGISTERED.md:128` 自述“from the issue text”；`gh issue view 595 --json body`（2026-11-05）无这些数字 |
| D-7 | `PROTOCOL.md:111-113` 写“the R1 single-repair option remains unspent”，但 R1 已执行 sanctioned repair m1→m5 | `PROTOCOL.md:111-113` vs `PREREGISTERED.md:101-123`；`report-595-e5.md:80-87` |

---

## 3. R3 建议门槛 vs Issue / PREREGISTERED / #604

### 3.1 逐项对照

R3 建议（本 ask 给定）：

- A/A：CPU/tput/p99 半宽 **5/3/3 pp**；
- 负控与保护：单侧 CPU/p95/p99 **≤5%**、吞吐下降 **≤3%**；
- 产品 B−A0：总 CPU 点估计 **≤−10%** 且 95% 上界 **≤−5%**；
- 峰值内存 **≤基线+2%**。

| R3 建议 | 冻结/Issue 对应 | 判定 |
|---|---|---|
| A/A 5/3/3 pp | `PREREGISTERED.md:50-54` 完全一致：CPU ≤5 pp、throughput ≤3 pp、p99 ≤3 pp | **相等**；但 R3 必须补“同 host、A/B 前、24 对、失败仅一次修复”的协议约束（`PREREGISTERED.md:41-54,101-123`） |
| 负控/保护 CPU/p95/p99 ≤5%、吞吐下降 ≤3% | `PREREGISTERED.md:135` 数值一致；但冻结门是 **Holm–Bonferroni 家族（9 shapes × metrics，one-sided α=0.05）**，且保护门是**家族合取**（R1 在 D2 CPU 4.17% 仍在 5% 内时，因 p95 5.69/p99 7.45/吞吐 3.99 判 FAIL） | **数值相等，决策结构更严**：R3 的“单侧”写法若退化为逐指标看 CI、允许 CPU 合格抵消尾部失败，则比冻结门**更弱** |
| 产品 B−A0 总 CPU ≤−10% 且 95% 上界 ≤−5% | `PREREGISTERED.md:134` 数值一致；冻结门还要求 “whole-executor CPU per useful task” 与 “unfavorable attribution end”；issue #595 要求 whole-system CPU（submitter + kernel + io-wq，+SQPOLL poller if used）并披露未计入 kernel-worker 成本 | **数值相等，口径更严**：R3 必须把“总 CPU”钉到该定义，不能只报进程用户态 CPU |
| 峰值内存 ≤基线+2% | `PREREGISTERED.md:136` 的 RSS 子句一致；同一门还要求 **zero new queue/thread/timer/per-request allocation** | **R3 省略了更严的分配门**：只报 RSS 不足以通过冻结 Resource gate |
| （R3 未列）Mechanism gate | `PREREGISTERED.md:132-133`：W-02 4k d16 pure submit enters/valid read 相对 A 减少 **≥50%**；SQE/CQE ledger 对账 | **Issue/冻结协议多出**；R3 若做产品结论必须回答 |
| （R3 未列）Correctness gate | `PREREGISTERED.md:131`：所有 mandatory oracles + data/result/cancel/lifetime/resource semantics 等价；issue #595 §4 要求修复缺失 oracle，`BASE GREEN→NAMED MUTANT RED→RESTORED GREEN`，禁止 sleep 作为同步证明 | **Issue/冻结协议多出且更严** |
| （R3 未列）样本/设计 | issue #595 §5 W4：same-host A/A + **≥15 interleaved AB/BA valid pairs** for promoted claims；R1/R2 用 24 对（满足） | **Issue 多出**；R3 若少于 15 对有效配对则不满足 |
| （R3 未列）指标覆盖 | issue #595 §5 W4：CPU/op、wall/op、throughput、p50/p95/p99（adequate samples）、actual outstanding depth、enters/SQE、progress/notify/park、kernel/system vs user、negative controls/regressions；仪器归因与无仪器 timing 分离；记录 storage，禁止跨机绝对速度比 | **Issue 多出** |
| （R3 未列）D=1/2/3 与 64 KiB | issue #595 §3 P1：Test D=1,2,3 explicitly；一个在 D=2 不能减少 enters 却引入 Core lock 的谓词不是 win；§6：no unexplained 64 KiB p95/p99 regression accepted；fallback cells 必须在 matched A/A 下 in-band | **Issue 多出且更严**（定性 + 机制条件） |
| （R3 未列）真实 consumer | #604 G0：real consumer 或 defensible representative trace + frozen oracle/dependencies/budgets/baseline；G3：end-to-end + neighboring regimes；Pilot C：fio/DuckDB 或具体 segment/WAL producer，synthetic 不算 app win | **#604 多出**（见 §4） |

### 3.2 结论

`[INFERENCE]` **数值上没有更严的门槛**：R3 的四组数字与 `PREREGISTERED.md` 的冻结值一致。**但 Issue/冻结协议在以下方面更严或额外**：

1. 负控/保护是 **Holm 家族 + 合取**，不是“每个指标各自看一眼 ≤5%/≤3%”；
2. Resource gate 还要求 **零新增 queue/thread/timer/per-request allocation**；
3. 产品 CPU 口径必须是 **whole-system/whole-executor**（含 kernel/io-wq，披露未计入 worker）；
4. **Mechanism gate（≥50% enter 减少 + ledger 对账）** 与 **Correctness gate** 是合取前置条件；
5. Issue 要求 **≥15 对有效 AB/BA**、完整指标覆盖、D=1/2/3 显式测试、64 KiB 无未解释尾部回归；
6. #604 的真实 consumer admission/transferability 是 E5-scoped 结论之外的额外层（且 handoff 未发生）。

`[GAP]` **门槛出处缺口**：`PREREGISTERED.md:128` 说这些 gate “from the issue text”，但 2026-11-05 的 live issue #595 body 没有 10/5/3/2% 这些数字（`gh issue view 595 --json body`，仅见 “≥15 interleaved AB/BA valid pairs” 等定性要求）。因此“Issue 要求”只能以 `PREREGISTERED.md` 的冻结文本为准；不得声称 live issue 原文写有这些数字。

---

## 4. #604 对真实 consumer 的准入与可复现要求

`[REMOTE]` 来源：https://github.com/jnhu76/Sluice/issues/604 （state OPEN，2026-11-05）。

### 4.1 准入（G0）

- **G0 Workload admission**：`Real consumer or defensible representative trace; frozen oracle/dependencies, budgets, baseline` → `ADMIT / REVISE / REJECT`。issue #604 §6。
- 每个 child study 必须先冻结 **workload sheet**（issue #604 §3）：
  - **Consumer and semantics**：actual program/trace、logical operations、dependency DAG、ordering、visibility、partial effects、cancellation、required durability、failure/recovery；
  - **Input envelope**：size distribution、read/write ratio、offsets、skew/hot set、arrival pattern、queue depth/in-flight bytes、concurrency、dataset size、warm/cold/cache、storage regime；
  - **Resource envelope**：RAM/pinned bytes、buffer count、workers、CPU/NUMA/SMT、device、filesystem、runtime/backend、hardware capabilities；
  - **Necessary-work hypothesis**：每个边界的最小物理字节、哪些 copies/allocations/waits/syscalls/submissions/lookups/cache lines 是语义必需/不确定/冗余；没有证明就称 estimate；
  - **Alternatives and controls**：good native baseline、current Sluice baseline、candidate/ablation、negative control；**compare only semantics-equivalent arms or disclose deltas**；
  - **Falsification criterion**：什么结果构成 NO-GO / insufficient exposure / measurement invalidity / complexity outweighing benefit。

### 4.2 证据分层与可复现

- issue #604 §5：必须分开 **mechanism / complete execution path / workload / real application** 四层证据；“A faster microbenchmark, lower instruction count or fewer copies does not alone demonstrate a faster application.”
- 必须保留 **commit/executable/toolchain/kernel/filesystem/device/cache/cpu provenance** 和 **repeated-sample uncertainty**（issue #604 §5）。
- G2：isolated A/B with a causal counterfactual；complete semantics；resource accounting（§6）。
- G3 Transferability：end-to-end result + neighboring regimes + regression/crossover + hardware sensitivity → `WORKLOAD-LOCAL / REUSABLE / NOT SUPPORTED`（§6）。
- G4 Product promotion：why inside Sluice vs app/adapter/benchmark；minimum surface and maintenance obligations；`OWNER DECISION REQUIRED`（§6）。
- Pilot C（§2）：用 separately admitted fio/DuckDB 和/或具体 segment/WAL producer 建立 application boundary；**No synthetic result counts as an app win.**
- 不得创建竞争 benchmark framework 或方法论权威；使用 `docs/performance/methodology.md` 与 #591（issue #604 §5/§7）。
- 非目标（§8）：不得优化单个有利 benchmark 并宣称通用更快路径；不得用更多 RAM/pinned memory 不计成本；不得削弱 durability/shutdown/lifetime/failure 语义；不得为跑 campaign 新开 framework/permanent telemetry/public API。

### 4.3 与 #595 的 handoff 关系

- `[FROZEN]` `PRODUCT = HOLD this round regardless (#604 has not handed off READY_FOR_595; verified 2026-11-04 — its latest comment is the #622 cost-map reference).` `PREREGISTERED.md:139-140`。
- `[REMOTE]` 2026-11-05 核查：#604 state OPEN；body 与 comments 中 `READY_FOR_595` 出现次数 **0**；最新 comment 是 6614846173（2026-11-03，#622 cost-map reference），其后无新 comment/handoff。因此 handoff 仍未发生。
- `[FROZEN]` issue #595 §6 Transferability：E5 证据只够 **E5-scoped research conclusions**；second native host 与 #578 real application evidence 是“broad default-policy portability”的前提，但**不是完成 E5 study 的 ceremonial gate**。R3 不得把 E5-scoped 结论写成 library-wide 改善。

---

## 5. 本轮不得改动的对象（immutable）

| 对象 | 边界 | 依据 |
|---|---|---|
| **主工作树** `/home/jnhu/Source/Sluice` | R3 只在独立 worktree/研究副本工作；主树保持 clean/未被 R3 修改 | `AGENTS.md:15-18`；`PROTOCOL.md:13-15`；`ROOT_CAUSE.md:648-654`；本 ask |
| **公开 API** | 不新增/修改 public header、工厂、配置或运行时 knob | `AGENTS.md:89-98`；issue #595 §2（same public v2 API）；`ROOT_CAUSE.md:653` |
| **v2-r1 契约** `docs/explicit-io-contract.md` | 不改 root、不改 requirement、不以测量结果重写契约 | `AGENTS.md:3-5`；`explicit-io-contract.md:47-61`（GOV-01）；issue #604 §7（v2-r1 sole normative contract） |
| **远端分支 / GitHub 状态** | 不 push、不建/改远端分支、不发表评论、不改 issue/PR 状态 | 本 ask；R1 comment §1（`never pushed`）；R2 comment（GitHub read-only） |
| **Issue 状态** | #595 OPEN、#604 OPEN、PR 625 OPEN 均保持；不关闭/不标记 | `gh issue view`/`gh pr view` 2026-11-05；`PREREGISTERED.md:139-140` |
| **R1 原始证据** `/home/jnhu/sluice-595-e5/evidence/` | 只读；不覆盖、不移动、不重新解释 R1 的 `KEEP_IMMEDIATE` | `PROTOCOL.md:13-14`；R2 comment（“R1 ... 保持只读、未改动”）；`README.md:13-14` |
| **R1/R2 原始证据与冻结阈值** | `PREREGISTERED.md` 的阈值不得因 R3 unblinding 而修改；R1/R2 的 raw/binary/manifest 不得重写 | `PREREGISTERED.md:3-5`（frozen BEFORE first timing run；only one sanctioned repair）；`README.md:102-122`（manifest） |
| **R2 归档** `docs/research/595-causal-r2/` + PR 625 | research record；`DO NOT MERGE`；不把 R2 文档改写成 R3 结论 | PR 625 body（https://github.com/jnhu76/Sluice/pull/625）；`README.md:8-11` |
| **#604 handoff 状态** | 不得在 `READY_FOR_595` 缺失时把 `PRODUCT=HOLD` 改成可部署 | `PREREGISTERED.md:139-140`；本会话 `gh api` 核查 |

`[INFERENCE]` 本 ask 还要求“不运行 benchmark、不修改任何文件”；本文件是唯一写入。R3 若需要新测量，应在独立 worktree/新 evidence 目录进行，并把配置、命令、原始结果与 provenance 一并归档（`AGENTS.md:76-80`；`methodology.md:10-31`）。

---

## 6. 本基线没有建立的事情

1. `[GAP]` 没有 R3 的裁决/计划文档；本轮只冻结“R3 必须回答什么”，不预写 R3 的答案。
2. `[GAP]` R1 D2 的 p50 在归档报告中缺失；未从原始 JSONL 重算（本 ask 不要求，且不得把推断写成冻结值）。
3. `[GAP]` R2 phaseA p95/p99 的 CI 有两个文档版本；未重算、未择一。
4. `[GAP]` live issue #595 body 不含 PREREGISTERED 声称的数值门槛；只能以 PREREGISTERED 冻结文本为准。
5. `[GAP]` R2 没有重跑冻结负控家族；R2 不能单独证明该家族仍全通过。
6. `[GAP]` #604 没有 `READY_FOR_595` handoff；R3 不得把 `PRODUCT=HOLD` 改为可部署。
7. `[GAP]` R3 尚未产生任何新的 A/A、D2、负控、正确性或真实 consumer 证据；本文件不提供这些证据。
8. 未运行构建、benchmark、oracle 或任何计时；本文件的所有数字均来自已归档文档/远端原文，不是本会话新测。
