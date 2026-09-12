# Explicit File Legacy Surface Audit — Final Human Review Addendum

Tracking: #355

Applies to: `docs/roadmap/explicit-file-legacy-surface-audit.md`

Final human review verdict for PR #368:

```text
VERDICT: PASS_WITH_2_MINOR_CORRECTIVES
P1 / MAJOR: 0
ARCHITECTURE BLOCKER: 0
VERDICT CHANGE: 0
```

This addendum is intentionally narrow. It does not change any §14 disposition, inventory count, implementation slice, dependency edge, or Phase A authority. It clarifies two final-review wording points in the main audit report.

## 1. Frozen proof root versus moving integration base

The audit's **frozen code-evidence root** remains:

```text
ff916c37c6bab0f9a2bc7555a19bd8a2080af639
```

The final PR integration base moved after the audit evidence had been collected. At third human review the PR base was:

```text
d4ce4e4ed7b3d8b1aa8cb242e7765650fafe36de
```

The delta from the frozen proof root to that integration base is PR #369's CI-only `.github/workflows/ci.yml` addition. It does not modify the audited `include/`, `src/`, `apps/`, `tests/`, ADR, or legacy-surface report inputs. Therefore the frozen proof root remains the evidence root for the §14 conclusions; the moving PR base is an integration fact, not a replacement proof root.

Any wording in the main report equivalent to "derived from current master = ff916c37" should be read as:

> Derived from the frozen code-evidence root `ff916c37`; the later moving integration base was checked separately and did not modify audited surfaces.

## 2. Per-fiber physical-safe point versus bulk teardown barrier

The T1–T8 ordering in the main report remains accepted, including:

```text
T1 logical task terminal
T2 Future publication/ready
T3 fiber entry returned
T4 Fiber::make_done
T5 final context switch back to scheduler
T6 scheduler invocation quiescence / run_impl return
T7 driver exit + driver_thread join
T8 Group/fiber/stack storage destruction
```

The final precision is:

```text
per-fiber physical-safe point = T5
current ApplicationRuntime bulk-destruction barrier = T6,
observed/sequenced through T7 before T8
```

At T5, that individual fiber has performed its final switch back to the scheduler and its stack is no longer executing. T6 is stronger: it establishes scheduler-invocation quiescence for the whole relevant set of fibers and is the earliest **existing ApplicationRuntime bulk barrier** suitable for authorizing destruction of Group-owned fiber/stack storage. T7 then provides the driver-thread observation/join barrier before `close_resources()` reaches T8.

Accordingly, the Reviewer-E sentence in the main report:

```text
最早合法释放点 = run_impl 返回
```

must be interpreted as:

```text
当前 ApplicationRuntime 最早可用于整体释放 Group-owned fiber/stack storage
的现成 barrier = T6 / run_impl return；
单个 fiber 的物理执行安全点更早，为 T5 final switch。
```

This clarification does **not** restore any lifetime authority to `Future::ready()` or `terminal_count_`:

```text
FUTURE_READY != FIBER_QUIESCENT
terminal_count_ != storage-release authority
```

## Final accepted disposition

```text
TOTAL         = 30
DELETE        = 29
CONVERGE      = 1   (I20 Group)
KEEP          = 0
ADD_MINIMAL   = 0
RESEARCH      = 0
OUT_OF_SCOPE  = 0

I20 Group     = CONVERGE
I21 Future    = DELETE
```

Traceability remains accepted:

```text
DUPLICATE_AUDIT_OWNER_COUNT      = 0
DUPLICATE_SLICE_OWNER_COUNT      = 0
UNAUDITED_PLANNED_REMOVAL_COUNT  = 0
```

#370 remains the required separate docs corrective for the stale vectored-decision wording and must be completed before #355 is closed.

Final state for PR #368:

```text
HUMAN_REVIEW_PASS
READY_TO_MERGE
```
