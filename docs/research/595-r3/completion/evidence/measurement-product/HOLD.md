# measurement-product — HOLD (#604 真实应用准入核查)

| 字段 | 值 |
|---|---|
| 角色 | #604 真实应用准入核查员（只读 issue/PR 与仓库） |
| 核查时间 | 2026-11-05（本会话） |
| 结论 | **`HOLD_NO_REAL_APPLICATION`**；`PRODUCT-604 = PRODUCT_NO_604_HANDOFF` |
| preregOk | **false**（#604 无 `READY_FOR_595` handoff；预注册 §10.3 要求 #604 handoff + 独立产品预注册，两者均不存在） |
| 计时 | **未运行任何 A0/A1/B**（0 对；未按 24 对/区组跑）；`/home/jnhu/sluice-595-e5/r3/MEASUREMENT.lock` 未被本核查者获取或改写 |
| 合成 benchmark 冒充 | **未使用** W-03 或任何合成 benchmark 冒充真实应用 |

---

## 1. #604 状态（远端只读核查）

命令与输出（本会话执行）：

```text
$ gh issue view 604 --json body,updatedAt,state --jq '{state, updatedAt, bodyHasReady: (.body|test("READY_FOR_595")), bodyHasConsumer: (.body|test("consumer"))}'
{"bodyHasConsumer":true,"bodyHasReady":false,"state":"OPEN","updatedAt":"2026-11-03T20:37:04Z"}

$ gh api repos/jnhu76/Sluice/issues/604/comments --jq '[.[] | select(.body|test("READY_FOR_595"))] | length'
0
```

- Issue **#604 = OPEN**，标题 `research(workload): Workload-Driven I/O Optimization — data movement, ownership, layout & execution`；`updatedAt = 2026-11-03T20:37:04Z`。
- body 中 `READY_FOR_595` = **false**；`consumer` = true，但该词只出现在 §2 workload portfolio 的 "Representative operation / real consumer" 列（"Sluice apps"）以及 §3/§4 的 G0/G1 研究要求中。body 自己声明 portfolio 是 "a **hypothesis set**, not evidence"（§2），不构成 G0 admission。
- 全部 2 条 comment 中 `READY_FOR_595` 出现 **0** 次：comment 1 是 WSL2 R0 cost atlas（2026-11-01），comment 2 是 #622 E5 成本地图引用（2026-11-03，id `6614846173`）。两者都是研究输入，均未 handoff 真实 consumer，也未写 `READY_FOR_595`。

## 2. `READY_FOR_595` 缺失（仓库与 r3/evidence 全量 grep）

命令与输出（本会话执行）：

```text
$ cd /home/jnhu/Source/Sluice && grep -rn 'READY_FOR_595' . --exclude-dir=.git | wc -l
0

$ cd /home/jnhu/sluice-595-e5/r3/evidence && grep -rc 'READY_FOR_595' . | awk -F: '{s+=$2} END {print s}'
13
```

- **产品仓库 `/home/jnhu/Source/Sluice`：0 次命中**。`docs/research/` 与 `apps/` 中没有任何 `READY_FOR_595` handoff 记录；`docs/research/` 内也没有任何把某应用标为 #604 已准入的文本（`docs/research/595-causal-r2/README.md:1-15` 明确 `PRODUCT_CHANGE: NONE`、`PRODUCT_PR: NONE`、`MERGED: NO`）。
- `/home/jnhu/sluice-595-e5/r3/evidence` 的 13 次命中全部是"handoff 不存在"的**否定性记录**，分布在 `VERDICT.md`、`EVIDENCE_BASELINE.md`、`issue-comment-r3.md`、`RESULTS.md`、`PROTOCOL.md`。代表行：
  - `r3/evidence/EVIDENCE_BASELINE.md:336`：`PRODUCT = HOLD this round regardless (#604 has not handed off READY_FOR_595; verified 2026-11-04 …)`；
  - `r3/evidence/EVIDENCE_BASELINE.md:337`：2026-11-05 核查 #604 state OPEN、body/comments 中 `READY_FOR_595` 出现 **0** 次，最新 comment 为 6614846173；
  - `r3/evidence/VERDICT.md:125`：`Issue #604 … OPEN；无 READY_FOR_595 handoff`；
  - `r3/evidence/PROTOCOL.md:256`：真实 consumer 计时 `NOT_RUN`，#604 未 handoff；
  - `r3/evidence/READY_EXPOSURE.md:8,33`：主暴露形状 W-03 是**合成 benchmark，不是 #604 准入的真实应用**。
- 上游 #595 的归档 comment 同样只写 `PRODUCT = HOLD`（#604 未交接 `READY_FOR_595`），无 handoff 声明（`gh api repos/jnhu76/Sluice/issues/595/comments`，comment id `6624399984`，第 13 节）。

## 3. 没有已准入的真实应用（仓库侧）

- `apps/` 只有 4 个参考应用：`sluice-copy`、`sluice-grep`、`sluice-hash`、`sluice-tail`。它们的 README 自述是 "reference application"/公共 API 组合示例（`apps/sluice-copy/README.md:3-6`），不是 #604 的已准入 consumer；`apps/sluice-hash/README.md:40`、`apps/sluice-grep/README.md:60` 里的 "consumers" 只是 API 调用说明。
- R3 的 consumer 普查已给出独立分类：`r3/evidence/READY_EXPOSURE.md:25-33` 把 `bench/bench_w03_external_loop.cpp` 列为唯一 REAL_EXPOSURE 形状，并明确 "W-03 是合成 benchmark，不是 #604 准入的真实应用"；`apps/sluice-copy/copy_task.cpp` 默认 `pipeline_depth=1` 为 **NO_REAL_BATCH_EXPOSURE**，`depth>1` 仅 **STARTUP_ONLY**（`READY_EXPOSURE.md:31,249,328`）。
- `docs/research/` 下没有真实应用 admission 记录；`grep -rn 'G0\|ADMIT' docs/research --include='*.md'` 未命中任何 #604 consumer 准入。仓库内 `#604` 的提交/PR 也没有 handoff（`git log --all --grep=604` 仅命中无关的 `58a5604d`；`gh pr list --search 604` 无相关 PR）。

## 4. 缺失的复现入口（为什么不能跑）

即便忽略 handoff，当前也没有可执行的"同一完整任务"产品测量入口：

1. **没有 consumer 定义**：#604 的 G0 要求 "Real consumer or defensible representative trace; frozen oracle/dependencies, budgets, baseline"（#604 body §6 G0）。目前没有指定程序、逻辑操作、依赖 DAG、输入输出、oracle 或 baseline。
2. **没有产品 harness 模式**：完成轮的 harness 只驱动两个 bench 二进制——`harness/run_session.sh:13-15` 定义 `R3_BENCH=…/bench_w03_external_loop`、`R2_BENCH=…/bench_w02_pipeline`；`grep -n 'product|consumer|apps/|duckdb|fio' harness/measure_pairs.py harness/run_session.sh` 无产品/应用入口命中。`harness/measure_pairs.py` 只接受注册 case（`W03-*`/`W02-*`）。
3. **没有产品预注册**：`PREREGISTERED_R3_COMPLETION.md:722-730`（§10.3）规定真实应用必须另行写独立预注册（语义/端到端 oracle、全执行者 CPU、p50/p95/p99、吞吐、峰值 RSS、线程/队列/timer/每请求分配、负控、A/A、退出码/SHA），且"当前 #604 OPEN、无 handoff → `PRODUCT=HOLD`；`measurement-product/` 只记录 HOLD"。该独立预注册**不存在**。
4. **预注册裁决已冻结**：`PREREGISTERED_R3_COMPLETION.md:379-381`（§5.6）写 `PRODUCT=HOLD`、"`measurement-product/` 只能记录 HOLD/blocked，不能产生产品 PASS"；`:879`（§14.1）写 `PRODUCT-604 … PRODUCT_NO_604_HANDOFF`，激活条件为 "#604 handoff + §10.3 独立预注册"。
5. **W-03 不可替代**：`PREREGISTERED_R3_COMPLETION.md:725-729` 明确 "W-03 d16 的任何收益/无退化**不能**替代真实应用门"；#604 body §2 也写 "No synthetic result counts as an app win"。因此本核查**没有**用 W-03 合成 benchmark 冒充真实应用，也没有产生任何 A0/A1/B 产品数字。

结论：`preregOk = false`。按任务约束，preregOk 为 false 时只写 HOLD 与原因，不进入 `/home/jnhu/sluice-595-e5/r3/MEASUREMENT.lock` 下的 24 对/区组 A0/A1/B 计时。本核查未获取该锁（锁文件仍为 0 字节，mtime `2026-11-05 19:15:14`，本会话未写入），也未创建 `raw/product/`。

## 5. 解除 HOLD 所需的最小条件

1. #604 在其 body 或 comment 中给出 `READY_FOR_595` handoff，并明确 G0 `ADMIT`。
2. handoff 必须点名一个真实 consumer（或可辩护代表 trace）及其：任务定义、输入/输出、冻结 oracle、依赖/预算、baseline、复现命令、退出码/SHA 口径。
3. 按 `PREREGISTERED_R3_COMPLETION.md:722-729`（§10.3）另写**独立产品预注册**，覆盖语义/端到端 oracle、全执行者 CPU、p50/p95/p99、吞吐、峰值 RSS、线程/队列/timer/每请求分配、负控、A/A。
4. 上述齐备后，才在 `MEASUREMENT.lock` 下按预注册跑同一完整任务的 A0/A1/B（24 对/区组），写 `measurement-product/` 与 `raw/product/`。

## 6. 本核查明确未做

- 未运行任何 A0/A1/B 或 A/A 计时（0 对）；未获取 `MEASUREMENT.lock`。
- 未创建 `raw/product/`；未产生任何产品 PASS/FAIL 裁决。
- 未修改产品代码、预注册、协议或任何既有证据文件；本文件是唯一写入。
- 未把 W-03、W-02 或 #622/#595 的机制数字外推为应用加速。
