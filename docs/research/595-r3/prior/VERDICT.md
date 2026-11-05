# #595 R3 — 裁决（VERDICT）

| 字段 | 值 |
|---|---|
| 文档状态 | **FINAL — DELIVERED**（2026-11-05） |
| 基线 | `baseSha = 0a22a79195f1b913145fcd443496e7f70b08cdcf` |
| R2 锚点 | `271b81bfc9d64eecc0266fe7066b9040525dd710` |
| 机制裁决 | **`MECHANISM_ONLY`** |
| 产品裁决 | **`KEEP_IMMEDIATE`** |
| 真实批暴露 | 主 shape `W02-uring-4k-d2` = **`NO_REAL_BATCH_EXPOSURE`**（STARTUP_ONLY）；仅 W-03 external loop = `REAL_EXPOSURE` |
| 产品候选 PR | **不建**（`PRODUCT_CANDIDATE_READY` 不成立） |
| 正确性 | **PASS**（`BASE GREEN → NAMED MUTANT RED → RESTORED GREEN`） |
| 计时 | **未执行**（A/A、A/B、负控、保护门全部 `NOT_RUN`） |

---

## 1. 裁决（两句话）

**机制层**：W-03 external loop 的稳态补发确有真实、可证明的多 ready 组；按现有容量重切可把纯 submit enter 从 1/op 降到 d16 约 0.085/op、d4 约 0.277/op（syscall 实测 + 结构校验），并且确定性正确性与命名变异全部通过。**但 R3 没有测量 CPU/时延/吞吐，也没有 A/A 校准、负控家族与保护门**，因此不能把“enter 合并”升级为“性能收益成立”。

**产品层**：`KEEP_IMMEDIATE`。不是因为机制被否定，而是因为产品净胜门（`B−A0` CPU 点估计 ≤ −10% 且 95% 上界 ≤ −5%）**根本没有数据可判**；同时 #595 的 headline shape（`W02-uring-4k-d2`）只有 `STARTUP_ONLY` 暴露（20k ops 省 1 enter = 0.005%），`sluice-copy` 默认 `NO_REAL_BATCH_EXPOSURE`，唯一 `REAL_EXPOSURE` 的 W-03 既是合成 benchmark、也不是 #595 的主 shape。原型还带有 cap=8 + suffix 逐条的协调成本，及 B 绕过 context、A0 含共同准备开销两项归因残差。**不建议开产品候选 PR。**

---

## 2. 门槛逐项判定

| 门 | 冻结条件 | 本轮结果 | 判定 |
|---|---|---|---|
| Correctness | 全部 mandatory oracles；`BASE GREEN → NAMED MUTANT RED → RESTORED GREEN` | 13/13 BASE GREEN；M1/M2/M3 目标断言 RED；当前 BASE 再跑全绿；6 个回归全绿 | **PASS** |
| Mechanism（enter） | W-02 4k d16 pure submit enters/valid read 相对 A 减少 ≥50%；SQE/CQE ledger 对账 | W-03 d16 理论 −91.5%；W-02 D2 仅 −0.005%；**冻结门的字面 shape（W-02 d16）未被本轮普查覆盖**（只测了 W-02 d2/d4）；ledger 对账由正确性 oracle 覆盖，但**未在计时二进制上做 20k 对账** | **PARTIAL / 不满足冻结口径** |
| Real task | 真实 consumer 或可辩护代表 trace（#604 G0） | W-03 提供 benchmark 内的 REAL_EXPOSURE，但它是**合成 benchmark，不是 #604 准入真实应用**；#595 主 shape = STARTUP_ONLY | **主 shape 不满足；W-03 只算代表 trace** |
| Degradation protection（负控） | 9 shapes × metrics，单侧 ≤5%/≤3%，Holm | **NOT_RUN** | **FAIL（未检验）** |
| Performance protection | 主 shape 吞吐下降 ≤3%，p95/p99 恶化 ≤5% | **NOT_RUN** | **FAIL（未检验）** |
| Net win（产品） | whole-executor CPU/op 点估计 ≤ −10%，95% 上界 ≤ −5% | **UNMEASURED** | **FAIL（未检验）** |
| Resource | 零新增 queue/thread/timer/per-request allocation；RSS ≤ +2% | **NOT_MEASURED** | **FAIL（未检验）** |
| A/A 校准 | 同 host、A/B 前、24 对、5/3/3 pp | **NOT_RUN** | **FAIL（未检验）** |
| 预注册 | R3 计时前冻结假设/臂/门槛/停手条件 | `PREREGISTERED_R3.md` 不存在 | **FAIL** |

**合取结论**：`PRODUCT_CANDIDATE_READY` 需要“真实任务 + 退化保护 + 正确性 + B−A0 数值”同时成立；本轮只有正确性成立。故产品裁决只能为 `KEEP_IMMEDIATE`。

---

## 3. 机制裁决：为什么是 `MECHANISM_ONLY`

### 3.1 已成立的部分（syscall / 结构层）

| 事实 | 证据 |
|---|---|
| 基线每次 `submit_read` 恰好一次 `io_uring_enter(to_submit=1, min_complete=0)`；0% 合发 | 20k 运行 `strace -c` 直接测得总数 60,004；纯 submit/probe 的 60,000+4 分解由 512 请求逐行 trace（1,536+4）与工作量推断，**不是 20k 逐调用测量**；`max SQE/enter=1` |
| W-03 稳态存在合法多 ready 组 | 组员 `(slot,offset,len,dst)` 与预测逐项一致；组内 take/wait/别名/越界/反序全 0 |
| 按现有容量重切可大幅减少 enter | d16：20,000 → 1,698/1,511/1,331（−91.5/−92.4/−93.4%）；d4：−72.3/−73.2/−72.7% |
| 原型确实合并 enter | B 臂回执（`raw/smoke/`）：d4 `5,955/20k`、d16 `11,140/20k`、d64 `17,781/20k`；`BUILD.md` §7 的另一次运行为 6,139 / 10,954 / 17,739，运行间有波动 |
| 正确性不因批量而破坏 | 13 场景 + 3 变异 + 6 回归 |

### 3.2 不成立的部分（性能层）

| 缺口 | 后果 |
|---|---|
| 没有 A/A 校准 | 无法区分效应与噪声；任何跨臂差都不可信 |
| 没有 A0/A1/B 计时 | 没有 CPU/op、wall/op、p50/p95/p99、吞吐 |
| 没有 whole-executor/whole-system CPU 口径 | 无法判断是否计入 kernel/io-wq |
| 没有负控家族 | 不能声明“无退化” |
| 没有保护门数据 | 不能排除 p95/p99/吞吐恶化 |
| 没有资源/内存/线程测量 | 不能声明 Resource gate |

### 3.3 标签选择

- 不写 `MECHANISM_SUPPORTED`：该标签会被读作“机制的性能效应已获支持”。R3 只有 syscall enter 与结构证据，没有 CPU 证据；R2 的 CPU 证据属于**另一个机制**（owner-gated defer）和另一个二进制/会话，不能借用。
- 写 **`MECHANISM_ONLY`**：enter 合并这一机制事实成立；其收益、代价、可迁移性未测。
- **词表映射**：issue #595 §7 的 closeout 词表不含 `MECHANISM_ONLY`；本字段是本轮裁决标签，映射到允许词表为 **`KEEP_IMMEDIATE`**（机制层获得 syscall 级支持，产品层未成立）。发布评论时同时给出该映射，不把 `MECHANISM_ONLY` 冒充 closeout 词。
- 不写 `BLOCKED_CORRECTNESS`：正确性门通过。
- 不写 `INCONCLUSIVE_NOISE`：没有 A/A 失败；不是噪声问题，而是**没有测量**。

---

## 4. 产品裁决：`KEEP_IMMEDIATE` 与 `NO_REAL_BATCH_EXPOSURE`

### 4.1 主 shape 没有真实批暴露

| shape | 稳态可合并组 | 20k ops 可省 enter | 分类 |
|---|---|---|---|
| **`W02-uring-4k-d2`（R1/R2 主 shape）** | 仅启动组 1 个（2 成员）；稳态 19,998/20,000 为单成员 | **1（0.005%）** | **STARTUP_ONLY / NO_REAL_BATCH_EXPOSURE** |
| `W02-uring-4k-d4` | 仅启动组（4 成员） | 3（0.015%） | STARTUP_ONLY |
| `W03-uring-4k-d4` | 稳态多成员组占 95.4/96.9/98.8%（其中 3–8 占 88.0/91.6/97.1%） | ~14,500（72%） | REAL_EXPOSURE（合成 benchmark） |
| `W03-uring-4k-d16` | 稳态多成员组占 95.1/91.5/98.8%（其中 >8 占 72.7/82.3/97.7%） | ~18,300（91.5%） | REAL_EXPOSURE（合成 benchmark） |
| `W03-uring-4k-d64` | 512 窗口组大小 7–64（phase 1 全 64） | 512 窗口 −98% | REAL_EXPOSURE（但受 cap 限制） |
| `sluice-copy` 默认 `depth=1` | 无 | 0 | **NO_REAL_BATCH_EXPOSURE** |

因此，对 #595 的 headline shape，**即使 enter 合并完全成功，也只有 1/20,000 的 enter 可省**；这不足以支撑任何产品声明。真实机会在 W-03 这类“consume-all-ready 后整窗补发”的 consumer，而它不是 #595 的主 shape，也不是 #604 已准入的真实应用。

### 4.2 协调/适配成本（B−A1 与 B−A0 的已观测部分）

没有计时，无法给出 `B−A1` 或 `B−A0` 的数值；但已有四项结构性成本必须随产品判断一起披露：

1. **cap=8 截断**：d16 的 16 成员组被切成“8 条一次 enter + 8 条逐条”，回执运行 flush **11,140/20k**（`BUILD.md` 10,954），而按容量 16 的理论值是 1,698/20k；d64 更差（回执 **17,781/20k**，`BUILD.md` 17,739，约为基线的 89%）。**cap 把深窗口的机制收益大部分抵消。**
2. **suffix 不二次批量**：回执 `suffix_singles` 在 d16 为 9,863、d64 为 17,464；这是 adapter 策略，不是内核限制。
3. **B 绕过 context**：`B` 直接调用 detail seam，绕过 `AsyncIoContext::submit_read` 的 `initiation_rejection` 与 `access_mtx_`；`B−A1` 同时包含“分组”和“绕过 context”两部分。A1 的 adapter 簿记也未测。**B 不是无需新公开契约即可被真实 consumer 调用的路径。**
4. **A0 不是 `baseSha` 原始循环**：worktree 的 refill 对所有臂先构造完整 `ready` span（`bench/bench_w03_external_loop.cpp:349-372`），A0 再用 `ctx.submit_read` 逐条提交（`bench/r3_ready_batch.hpp:88-98`）；`baseSha` 的原循环是在循环内逐条构造 `ReadOp`（`:339-349`）。因此 **A0 含原始 baseline 没有的组准备/`std::vector` 开销**。本轮无计时，不影响裁决；未来 A0/A1/B 计时必须改用原循环或显式披露该共同开销，否则 `B−A0` 的控制臂不是真正的原始路径。

### 4.3 与旧 B / R2 的关系

- R3 原型（consumer 侧批量、cap=8、suffix 逐条）与 R2 的 B（owner-gated deferred flush）**不是同一机制**。
- R2 的门要求 submitter 是最近的 progress driver；W-03 的 submitter 不是 driver，因此 **R2 原型不会捕获 W-03 的组**。R3 的价值恰恰是把机会移到 consumer 侧。
- R2 的 D2 历史结果（CPU +3.73%，p95 +4.96%，p99 +7.07%，吞吐下降 3.58%）是“旧 B 在主 shape 上劣于 A”的记录；R3 没有对 D2 计时，**不能声称新原型改善了 D2**，更不能声称它优于 A0。
- 按裁决规则：没有 `B−A0 ≤ −10%` 的数据，不能产品化；即便将来 D2 相比旧 B 改善，只要仍劣于 A0，也不能产品化。

### 4.4 不建产品候选 PR

理由：产品门未检验 + 主 shape 无真实暴露 + cap/suffix 协调成本 + B 绕过 context + 无资源/保护数据。任何“产品候选”都会把未测机制带入公开路径。

---

## 5. 不可变对象复核（本轮实际执行）

| 对象 | 检查 | 结果 |
|---|---|---|
| 主工作树 `/home/jnhu/Source/Sluice` | `git status --porcelain` | **0 行**；HEAD `0a22a791…` |
| 公开 async 头（5 个） | `git diff --stat 0a22a791 -- include/sluice/async/*.hpp` | **空**（未改） |
| 全部 `include/` | `git diff --stat 0a22a791 -- include/` | 仅 `detail/test_access.hpp`（+11）、`detail/uring_backend.hpp`（+40）；均为内部 detail 头 |
| v2-r1 契约 | `git diff --stat 0a22a791 -- docs/explicit-io-contract.md docs/roadmap/v2-conformance.md` | **空** |
| R2 归档 | 主树 `git status docs/research/595-causal-r2` | **0 行**；最后提交仍为 `0a22a791` |
| R1 原始证据 | `/home/jnhu/sluice-595-e5/evidence` 下 1854 文件 | **0 个** mtime 晚于 2026-11-05 00:00 |
| R2 原始证据 | `/home/jnhu/sluice-595-e5/causal-r2` | **0 个**文件 mtime 晚于 2026-11-05 11:00 |
| Issue #595 | `gh issue view 595` | **OPEN**；最新 comment 仍为 R2（6636745376，2026-11-04T16:59:52Z）；无 R3 comment |
| Issue #604 | `gh issue view 604` | **OPEN**；无 `READY_FOR_595` handoff |
| PR 625 | `gh pr view 625` | **OPEN**（未改） |
| 远端 | 未 push、未建/改分支、未发评论 | 符合约束 |

结论：**未发现对公开 API、v2-r1、远端、Issue 或 R1/R2 原始证据的修改。** 唯一 `include/` 变更是隔离 worktree 内的内部 detail 头，不属于公开 async 头。

---

## 6. 交付缺陷与处置

| # | 缺陷 | 处置 |
|---|---|---|
| V-1 | `PREREGISTERED_R3.md` 不存在 | 不补造；`PROTOCOL.md` P-2 记录；下一阶段必须先预注册 |
| V-2 | `measurement-{main,product,mechanism,analysis}/` 不存在 | 不补造；`RESULTS.md` §2 全部 `NOT_RUN` |
| V-3 | 旧 `evidence/MANIFEST.sha256`（22 项）在正确性轮后 3 项失败 | 旧清单保留为 `MANIFEST.sha256.pre-delivery`；本轮重生成 `MANIFEST.sha256` 覆盖当前态，`sha256sum -c` 通过 |
| V-4 | `evidence/r3.patch` 已过期，reverse check 失败 | 保留为历史实现轮 patch；新增 `r3-final.patch`（10 文件，+2883/−51，reverse check exit 0）捕获最终态 |
| V-5 | `BUILD.md` §7 smoke 计数与本轮回执不同（d16 10,954 vs 11,140） | 记录为 P-10；回执 `raw/smoke/`；不得把 smoke 计数当确定性常量 |
| V-6 | 正确性运行未持 `MEASUREMENT.lock` | 记录为 P-5；无计时数值，不影响性能证据 |
| V-7 | `OPEN-1` 未解除 | `A0/A1/B` 只作研究标签；不得作为冻结臂引用 |
| V-8 | 评论正文曾不在主清单覆盖范围 | 主清单现包含 `issue-comment-r3.md`；`MANIFEST.comment.sha256` 独立记录评论哈希；`MANIFEST.sha256.hash` 记录主清单哈希（评论只引用该文件路径，不内嵌哈希，避免循环依赖） |
| V-9 | `A0` 不是 `baseSha` 原始路径 | `VERDICT.md` §4.2 第 4 条与 `RESULTS.md` §3.4 披露；未来计时必须改用原循环或显式计入共同准备开销 |

---

## 7. 未测 / 不可观测（必须随裁决列出）

1. A/A 校准；A0/A1/B 任何配对计时；CPU/op、wall/op、吞吐、p50/p95/p99。
2. 负控家族（D1/D2/D3、64 KiB d16、W-03 d4/d16/d64、threadpool、saturate）与 Holm 判定。
3. 性能保护门与资源门（RSS、线程、queue/timer、per-request allocation）。
4. `A_R2` / `B_R2`（无定义）。
5. #595 主 shape 上的原型行为（W-02 未接 adapter）。
6. `cap>8`、suffix 二次批量、d64 完整组重用。
7. 真实块设备、cold cache、O_DIRECT、其他 block size、非 tmpfs。
8. threadpool backend 的批量路径；跨线程并发 submit 与批量的交错。
9. #604 真实 consumer（fio/DuckDB/segment/WAL producer）与 `READY_FOR_595` handoff。
10. 第二台原生主机 / 跨机可移植性。
11. kernel/system CPU 归因、io-wq 机制、eventfd 内部阶段、指令级/微架构来源。
12. R2 历史数值的本轮复现。
13. stale generation 批量、pre-park wake with batch residue、overflow 期间批量 flush 的完整时序。

---

## 8. 下一阶段触发条件（建议）

只有同时满足以下条件，才应重新评估 `PRODUCT_CANDIDATE_READY`：

1. **先写 `PREREGISTERED_R3.md`**（或 R4）：冻结 `A0/A1/A_R2/B_R2` 的定义、二进制映射、24 对、AB/BA、bootstrap seed、A/A 5/3/3 pp、负控家族与停手条件。
2. 在同一二进制/会话上完成 **A/A 校准**；失败则按冻结规则只修一次，仍失败则该指标 `INCONCLUSIVE_NOISE`。
3. 完成 **A0/A1/B 24 对 AB/BA**，给出 whole-executor/whole-system CPU/op（submitter + kernel + io-wq；若 SQPOLL 含 poller，披露未计入 worker）与 p50/p95/p99/吞吐的配对差 + 95% CI。
4. 完成 **负控家族 + Holm**、保护门、RSS/线程/分配门。
5. 解决 **cap=8 + suffix 逐条** 的协调成本（至少给出 cap=8/16/64 的对照，或 suffix 二次批量），并显式拆分 `B−A1` 的 context 绕过残差。
6. 在 **W-03 REAL_EXPOSURE** 上验证；若要支撑 #595 主 shape，必须证明 W-02 也能形成真实组（目前只有 STARTUP_ONLY，不能靠加等待造组）。
7. 若要产品化，另开**窄范围产品候选 PR**，只含 public-path 可达的最小机制；在此之前保持 `KEEP_IMMEDIATE`。

---

## 9. 复核命令（本轮已执行）

```bash
git -C /home/jnhu/Source/Sluice status --porcelain
git -C /home/jnhu/Source/Sluice rev-parse HEAD
git -C /home/jnhu/sluice-595-e5/r3/worktree status --porcelain
git -C /home/jnhu/sluice-595-e5/r3/worktree diff --stat 0a22a791 -- include/sluice/async/*.hpp
git -C /home/jnhu/sluice-595-e5/r3/worktree diff --stat 0a22a791 -- docs/explicit-io-contract.md docs/roadmap/v2-conformance.md
sha256sum -c /home/jnhu/sluice-595-e5/r3/evidence/correctness/MANIFEST.sha256   # 33/33 OK（在 r3/ 下）
sha256sum -c /home/jnhu/sluice-595-e5/r3/evidence/MANIFEST.sha256              # 见清单，必须全 OK
gh issue view 595 --json number,state,updatedAt
gh issue view 604 --json number,state,updatedAt
gh pr view 625 --json number,state
cd /home/jnhu/sluice-595-e5/r3/worktree/build/linux/x86_64/release
./uring_ready_batch_deterministic_test
./uring_ready_batch_det_mut_includes_not_ready --only=sq_full_residue
./uring_ready_batch_det_mut_residue_dropped --only=partial_submit
./uring_ready_batch_det_mut_redispatch_consumed --only=d2_positive
```
