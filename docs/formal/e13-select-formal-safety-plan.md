# E13 Select Formal Safety 实现计划 (PR #18)

> **面向 AI 代理的工作者：** 在 `feat/e13-select-formal-safety` 分支内施工；基线权限 commit 为 `57435a913ad6d679df19a17f17f1c36e711dfc60`（PR #17 合并）。严禁修改、删除、暂存或提交预存未追踪文件 `tests/test_t3_simple.cpp` 与 `tla2tools.jar`。仅允许新增/修改 `docs/spec/e13_select/**`、`docs/formal/**`、`docs/reviews/**`、`tools/formal/**`。

**目标：** 在 PR #17 三层正式模型基础上，加入分层 safety invariants、focused negative models、non-vacuity evidence、bounded multi-group non-interference、可重复 source-safe verifier，闭合 E13 Select 的安全基础。不写任何生产 C++。

**架构：** 保留 PR #17 三层稳定 seam；新增 `*SafetyInv` aggregates 与 PR #17 canonical `*Inv` 并存；negative models 用 INSTANCE-WITH wrapper 包裹 canonical spec；non-vacuity 用 inverse-reachability invariant；multi-group 用直接建模（two-group bounded）。

**技术栈：** TLA+、TLC 2.19 (rev 5a47802)、OpenJDK 25.0.3、`TLC_WORKERS=1`、mktemp 隔离工作区。

---

## 任务 A：重命名分支与基线权限

**已完成。**

- [x] 分支 `feat/e13-select-formal-safety`（由 `feat/e13-select-formal-safety-implementation` 起步；最终分支名确认）。
- [x] `BASE_AUTHORITY_COMMIT = 57435a913ad6d679df19a17f17f1c36e711dfc60`。
- [x] 运行 PR #17 verifier (`tools/formal/verify-e13-select-core.sh`) 确认 GREEN 基线。

## 任务 B-F：Layer C/S/A safety invariants

**已完成。**

- [x] **Layer C**：`ContractSafetyInv`（32 laws, H1-H5 + I），提交 `aaa87d7`。
- [x] **Layer S**：`CentralSafetyInv`（14 laws, J），提交 `7a56e31`。
- [x] **Layer A**：`AdapterSafetyInv`（K + L + M + N, 42 laws），包括每 arm exactly-once 记账（M）与 step-indexed history（N），提交 `799cf22`。
- [x] 强化 `E_InvEventPersistentSetNotConsumed`（从占位 `TRUE` 变为真实 state law），canonical safety 与 safety3mix 仍 PASS。

## 任务 O：bounded multi-group non-interference

**已完成。**

- [x] `E13SelectMultiGroup.tla`：two-group bounded 组合，shared Event identity，提交 `87a6110`。
- [x] `MGSafetyInv`（11 laws）+ 3 reach witnesses（shared-Event double completion、mixed Event+Timer、one-rollback-other-complete）。

## 任务 R：Contract negative models

**已完成。**

- [x] `E13SelectContractNeg.tla`：NEG-C1..C8（8 faults）+ restore cfg，提交 `33acca1`。
- [x] 每个 fault reachable、target invariant violated、restoration PASS。

## 任务 S：Central Claim negative models

**已完成。**

- [x] `E13SelectCentralClaimNeg.tla`：NEG-S1..S6（6 faults）+ restore cfg，提交 `ad6040e`。

## 任务 E/T/A：Event/Timer/Accounting negative models

**已完成。**

- [x] `E13SelectEventTimerNeg.tla`：NEG-E1..E6（6 Event faults）+ NEG-T1..T5（5 Timer faults）+ NEG-A1..A4（4 Accounting faults）+ restore cfg，提交 `cb836d2`。
- [x] NEG-E6 targets Contract-layer `C_InvOnlyWinnerPublishes` 经两步 refinement chain。

## 任务 P：positive safety configurations

**已完成。**

- [x] 7 layered safety aggregates：Contract 2/3-arm、Central 2/3/4-arm、Adapter 2/3-mix。
- [x] 全部 PASS（Contract 346/3436 states；Central 111/531/2871；Adapter 22528/2671164）。

## 任务 W：non-vacuity matrix

**已完成。**

- [x] 9 Contract + 5 Central + 6 Adapter inverse-reachability witnesses，提交 `1bf2b33`。
- [x] 全部 20 witnesses 报告 `Invariant NotReach_<X> is violated`。

## 任务 X：refinement checks

**已完成。**

- [x] `E13SelectCentralClaim.refine3.cfg`：3-arm admission tie refinement，提交 `6fe064c`。
- [x] 4-arm adapter refinement PROPERTY 因状态空间爆炸（>5 分钟）放弃；3-arm adapter domain 改由 `AdapterSafetyInv` 在 safety3mix 中覆盖。

## 任务 Y：source-safe verifier

**已完成。**

- [x] `tools/formal/verify-e13-select-safety.sh`：4 个 expectation gates（`expect_pass`、`expect_reach`、`expect_negative`、`expect_restored`），65 distinct TLC runs，提交 `6fe064c`。
- [x] mktemp 隔离 + defensive cleanup trap + path-pattern 校验。

## 任务 (docs)：safety 文档

- [x] `docs/spec/e13_select/INVARIANTS.md`：分层 named laws 全表。
- [x] `docs/spec/e13_select/NEGATIVE_MODELS.md`：29 focused faults 矩阵。
- [x] `docs/spec/e13_select/NON_VACUITY.md`：20 witnesses 矩阵。
- [x] `docs/spec/e13_select/REFINEMENT.md`：mapping 表 + X cfgs。
- [x] `docs/spec/e13_select/EVIDENCE_SAFETY.md`：source-safety 与 reproducibility。
- [x] `docs/formal/e13-select-formal-safety-design.md`：架构与边界。
- [x] `docs/formal/e13-select-formal-safety-plan.md`（本文件）。

## 任务 AA：README status

- [x] `docs/spec/e13_select/README.md`：新增 PR #18 status section（FORMAL SAFETY: IN PROGRESS — PR #18）。

## 任务 Z/AB：review request

- [x] `docs/reviews/E13-SELECT-FORMAL-MODEL-SAFETY-1-REVIEW-REQUEST.md`：按 A-O 章节记录 baseline、范围、证据、残余风险。

## 任务 AD/AF/AG：final validation

- [x] 运行 `verify-e13-select-core.sh`（PR #17 regression）PASS。
- [x] 运行 `verify-e13-select-safety.sh`（PR #18 suite）PASS。
- [x] `git diff --check`、scope audit、untracked-files preservation check。
