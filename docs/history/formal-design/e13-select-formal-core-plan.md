# E13 Select Formal Core 实现计划

> **面向 AI 代理的工作者：** 在当前 feature 分支内按 TDD 执行；每个模型层先建立失败的 TLC 配置，再写最少模型使其通过。禁止派生 PR #18 negative models。

**目标：** 将 PR #17 重构为 Contract、Central Claim、Event/Timer 三层正式模型，并用 TLC 验证两级 refinement 与 R1-R12 因果 reachability。

**架构：** `E13SelectContract.tla` 定义稳定语义；`E13SelectCentralClaim.tla` 以顶层 `INSTANCE ... WITH` 显式映射并细化 contract；`E13SelectEventTimer.tla` 同样细化 Central Claim并承载第一阶段 adapter；`E13Select.tla` 仅作为兼容 root。

**技术栈：** TLA+、TLC 2.19、shell formal gate。

---

## 任务 1：建立三层测试入口

**文件：**
- 创建：`docs/spec/e13_select/E13SelectContract.cfg`
- 创建：`docs/spec/e13_select/E13SelectContract.reach_inline.cfg`
- 创建：`docs/spec/e13_select/E13SelectContract.reach_suspended.cfg`
- 创建：`docs/spec/e13_select/E13SelectCentralClaim.cfg`
- 创建：`docs/spec/e13_select/E13SelectCentralClaim.reach.cfg`
- 创建：`docs/spec/e13_select/E13SelectEventTimer.cfg`

- [x] 写配置，分别引用 `ContractSpec/ContractInv`、`CentralSpec/CentralInv/RefinesContract`、`EventTimerSpec/EventTimerInv/RefinesCentralClaim`。
- [x] 运行 TLC，确认因模块/operator 尚不存在而失败，建立 RED 基线。

## 任务 2：实现稳定 Select contract

**文件：**
- 创建：`docs/spec/e13_select/E13SelectContract.tla`

- [x] 定义抽象 evidence、reservation、winner linearization、winner commit、loser/rollback release、authority closure、publication 和 destruction 状态。
- [x] 定义 `ContractTypeOK` 与 contract coherence invariants：winner 唯一、commit-after-linearization、loser never publishes、reservation close-once、completion-after-authority-closure、publication bounds。
- [x] 定义 inline、suspended、reversible-reservation reachability predicates。
- [x] [corrective] 把 rollback 限定在 registration rollback 域（`contract_phase = "Building"` / `central_phase = "Registering"` / `caller_state = "Running"` / `winner = NoArm`），并加入 `TerminalCallerStateWellFormed` / `NoBadTerminalWaiting` / 三层 `*RegistrationRollbackDisabledAfterSuspension` 不变量；post-suspension cancellation 不在本 PR 模拟。
- [x] 运行 contract safety 与 reach 配置，确认 GREEN。

## 任务 3：实现 Central Claim refinement

**文件：**
- 创建：`docs/spec/e13_select/E13SelectCentralClaim.tla`

- [x] 加入 `candidate_ready`、`claim_candidates`、lowest-index claim、claim mode、Winner/Loser classification。
- [x] 每个 Central action显式对应 contract action或 contract stutter。
- [x] 使用顶层 `INSTANCE E13SelectContract WITH ...` 定义 `ContractRefinement`，将 `ContractRefinement!ContractSpec` 暴露为 `RefinesContract`。
- [x] 运行 Central safety、refinement property 与 reach 配置，确认 GREEN。

## 任务 4：实现 Event/Timer adapter refinement

**文件：**
- 创建：`docs/spec/e13_select/E13SelectEventTimer.tla`
- 修改：`docs/spec/e13_select/E13Select.tla`
- 修改：`docs/spec/e13_select/E13Select*.cfg`

- [x] 建模 Event identity、arm-to-Event mapping、admission observation 与不可绕过的两阶段 broadcast。
- [x] 建模 Timer Active gate、node dereference history、winner consume、loser cancel-before-retire、stale skip。
- [x] 建模 WaitNode terminal-before-detach、typed-context clear、逐 arm accounting closure。
- [x] 将 claim snapshot/action history接入 R1-R12，使每个 witness 绑定真实因果 action。
- [x] 使用顶层 `INSTANCE E13SelectCentralClaim WITH ...` 定义 `RefinesCentralClaim`。
- [x] 将 `E13Select.tla` 缩为 thin root，保持现有 scene cfg 兼容。
- [x] 逐个运行 R1-R12 reach 配置，确认预期 named inverse invariant CEX。

## 任务 5：可重复验证与证据

**文件：**
- 创建：`tools/formal/verify-e13-select-core.sh`
- 创建：`docs/spec/e13_select/README.md`
- 创建：`docs/reviews/E13-SELECT-FORMAL-MODEL-CORE-1-REVIEW-REQUEST.md`

- [x] formal gate 使用 fresh `-metadir`，校验三个正向模型 PASS、两级 refinement PROPERTY PASS、每个 R1-R12 命中正确 named predicate。
- [x] README 记录三层 seam、refinement mapping、变量/action 映射、TLC version/states/depth/runtime、抽象边界与 PR #18 延后项。
- [x] review artifact 按 A-O 章节记录 baseline、范围、证据与残余风险。
- [x] 运行 formal gate、`git diff --check`、scope audit 和最终 status。
