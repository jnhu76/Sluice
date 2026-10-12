# Phase M → Phase F live handoff index — 2026-10-12

> **Status-only bookkeeping; derived/non-normative.** Source of authority is [v1-r4](../explicit-io-v1-final-decision.md) with adopted ADRs and F0/F1 gates. This index does not grant breakage, retirement, package or merge authorization, revise historical frozen evidence or certify release readiness. Historical dated M/F candidate entries remain valid records *of their time*, not the current tree.

## Decision completion versus code/package completion

| Subject | Current status | Authority and evidence |
| --- | --- | --- |
| Phase M / seven families | **CLOSED / ADOPTED**; 12 decisions, 216 census records, 0 unowned edges in the terminal projection | [#458](https://github.com/jnhu76/Sluice/issues/458), [PR #491](https://github.com/jnhu76/Sluice/pull/491), [final packet](../review/m-phase-final-adoption-20261011.md) |
| F3 File wrapper | **ADOPTED_SCOPED**; #501/#502 | B-01 v3 `49 objects / 36 async members / 78 installed headers` on merged master `cb977a12` |
| F3 COPY/waiter ownership | **MERGED**; #500/#503; five Scheduler/Fiber waiter properties retain executable owners | merges `9572bd25` / `a04044a6` |
| F3 ApplicationRuntime facade | **VERIFIED / ADOPTED_SCOPED**; #504; #498 **CLOSED / completed** | [master v4 receipt](../evidence/f3-part-c-b01-v4-adopted-20261012/REPRODUCE.md), merge `e123d42a` |
| F3 whole-family exit | **PENDING**; no unsupported optional-host promotion | [#474 F3 closeout](https://github.com/jnhu76/Sluice/issues/474), [#402](https://github.com/jnhu76/Sluice/issues/402) |
| F2 request/result/identity contraction | **PENDING** (separate from F3 facade) | #402 F2 |
| F4 legacy native File/stream and pool | **PENDING PHYSICAL IMPLEMENTATION**; M decisions already ADOPTED | [#505 File/stream](https://github.com/jnhu76/Sluice/issues/505) from #461; [#506 pool](https://github.com/jnhu76/Sluice/issues/506) from #462; remaining D-13 test-double seam duties at #402 F4/F5 |
| F5 final public/install/config/symbol and F6 evidence closure | **PENDING**, notwithstanding scoped B-01 v3/v4 adoption | #402 F5/F6; #475 HOLD/KEEP |
| v1 release | **NOT READY** | #390/#402 |

## Current installed/build evidence

At post-merge code/master `e123d42a42dadc384f64d46ea77458d26104c07c`, independently executed server Debug/Release × no-/real-liburing tests passed **53/53 targets (n)** and **67/67 (y)**; six ASan/UBSan and six shared-runtime TSan risk targets passed. Two fresh package profiles measured **46 library objects / 33 async archive members / 73 installed headers**, from the Part C census baseline **50/37/79**. Final bookkeeping master `432b65cb9df481b99e3a91c2a8d9fba698429a64` changes metadata/evidence only and its production source/test/build inputs match the full-matrix SHA. These are *scoped Linux evidence*, not a general portability claim.

Four production sources and their archive objects were removed: `src/async/{file,application_runtime,await_op_helpers,group}.cpp`; six installed headers were removed: `include/sluice/async/{file,application_runtime,await_op_helpers,task_result,group,evented_wait_policy}.hpp`. No past F1/v3/v4 candidate evidence was rewritten.

**Retained, with separate ownership:** canonical File/Request/RequestScope/AsyncIoContext/backends; necessary Completion backend/context bridge; Scheduler/sync/select and Fiber/fiber_ctx optional-host substrate; StackfulIoHost; and #475 `Future/WaitPolicy` KEEP_INSTALLED_COMPAT. `default_wait_policy()` from `src/async/wait_policy.cpp` is a retained link obligation, not a separate #475 installed-header policy decision; retained installed API ≠ supported W-04. Current `OPTIONAL_W04_SUPPORTED=NO`.

## Live follow-through and exactly scoped outstanding actions

- **#474 F3:** finish the explicit TAIL `--workers` decision (currently validated but inert) with preflight CLI behavior or meaningful semantics; link D-H1..5 and OB-1/2/3 behavioral oracles, W-03 host-neutral firewall and conditional W-04 **negative package/support-claim audit**. `N/A_WITH_NEGATIVE_PACKAGE_AND_SUPPORT_CLAIM_AUDIT` is not W-04 PASS.
- **#505 F4:** migrate/retire the second native File owner under adopted M-F A3a/B1; handle semantic range, factories, Reader→Copy and WAL vectored dependencies, shared retry/DirFsync helpers, external-use policy and a new B-01 cut before deletion.
- **#506 F4:** remove BlockingIoPool only after its own per-header risk/sunset decision, retaining its **P3 bounded bridge** until the approved P2 physical cut (both installed headers + source/archive).
- **#402 F2/F4/F5/F6:** do not treat F3's scoped B-01 adoption as whole-F5 or final formal/evidence correspondence acceptance. F4 D-13 retains the installed test-double/active-seam internalization-or-retirement decision beyond #505/#506; M-A4's isolated experimental files still require the F5 final-tree disposition (not v1 support). Normalize the F5 optional-W04 shorthand to the root-conditional gate before F5 final sign-off; apply the existing #475 A HOLD/B,C KEEP decisions at their exact installed surfaces.

Issue administration already completed: #498 and #451 **CLOSED (completed)**; formerly decision-only #461/#462 **CLOSED (decision-adopted handoff)** after opening #505/#506; #474 renamed and remains **OPEN (F3 closeout)**. #458 Phase M, not #402 Phase F, is the closed decision gate.

```ini
PHASE_M = CLOSED_ADOPTED
F3_APP_RUNTIME_FACADE = VERIFIED
F3_SCOPED_B01_V3_V4 = ADOPTED
F3_FULL_CLOSEOUT = PENDING
F4_LEGACY_NATIVE_AND_POOL = PENDING
F5_FINAL_ALIGNMENT = PENDING
F6_FINAL_ACCEPTANCE = PENDING
OPTIONAL_W04_SUPPORTED = NO
V1_RELEASE_READY = NO
```
