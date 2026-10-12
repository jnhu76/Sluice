# Part C confirmed owner decision — 2026-10-12

Activation: CONFIRMED_EFFECTIVE. The human user explicitly confirmed designated
owner identity and answered the concrete #504 five-surface decision request:

> 确认 owner；五项均接受 breakage，无额外弃用窗口

The same owner separately selected:

> 授权 scoped package 收缩，满足最终门禁后采纳

This is an owner-directed decision in the execution conversation, recorded by
the execution agent. It is not personally typed GitHub review text, not an
independent human technical review, and not GitHub formal APPROVED.

## Exact external-risk disposition

ACCEPT_SCOPED_EXTERNAL_BREAKAGE, with UNKNOWN_EXTERNAL_USE explicitly accepted
and no additional bounded deprecation window, separately for all five surfaces
disclosed in the [per-surface packet](f3-part-c-facade-risk-decision-20261012.md):

1. async/application_runtime.hpp: ApplicationRuntime, RuntimeBuilder,
   RuntimeTaskContext and RuntimeTaskFn, including lifecycle/admission/cancel/
   Completion-submit/await entry points.
2. async/await_op_helpers.hpp: AwaitOpTally, await_take overloads, await_drain,
   await_read_once/read_fill/write_exact.
3. async/task_result.hpp: independent TaskResultSlot mailbox,
   translate_task_exception and run_task_to_result templates.
4. async/group.hpp: independent default threaded Group and Scheduler-backed
   Group construction/async/await/cancel/join-by-destruction and related accessors.
5. async/evented_wait_policy.hpp: independently usable Scheduler/Future
   EventedWaitPolicy adapter.

Accept deliberate pre-v1 source/install/static-link breakage for the exact
scoped capabilities. External use remains UNKNOWN, not zero. Canonical
Request/Scope/application progress and retained optional host are required
semantic owners, not source-compatible replacements for these conveniences.
Independent threaded task grouping, mailbox/error helper and policy adaptation
capabilities are explicitly included in this exclusion decision.

The paired production source scope is exactly src/async/application_runtime.cpp,
src/async/await_op_helpers.cpp and src/async/group.cpp. Removing the facade
friend/forward edge and stale context comment is scoped mechanical cleanup.
No additional public Scheduler callable is removed.

## Scoped versioned package authorization

AUTHORIZE_SCOPED_VERSIONED_PACKAGE_CONTRACTION, subject to final implementation
gates, for the complete five-header/three-source scope above.
B-01 candidate v4 ID: F3_PART_C_B01_FACADE_CANDIDATE_V4_20261012.
Fresh exact source 1d0c368f against master cb977a12, two profiles and subsequent
retained Completion supplement show 49/36/78 -> 46/33/73 with retained public
closure equality, actual symbols/members/objects and positive/negative probes.
[Reproduction and bound artifacts](../evidence/f3-part-c-b01-v4-candidate-20261012/REPRODUCE.md).

Authorization permits this scoped contraction after the required final gates;
it does not itself adopt a package or mark master contracted. Final exact-HEAD
independent review, dependency closure, cumulative ordinary CI and explicit
merge authorization remain required. Source/test/build input equivalence
permits reusing the exact committed server matrix and package evidence for
later documentation-only decision records. Fresh postmerge master capture
remains mandatory if a future merge is authorized.

## Retained boundaries and current state

Completion, AsyncIoContext, Request/RequestScope/RequestCore, ProgressSource,
backends, Scheduler/sync/select, Fiber/fiber_ctx, StackfulIoHost,
Future/WaitPolicy/default_wait_policy remain. #475's fifteen HOLD/KEEP surfaces
are unchanged; Group/Evented removal does not authorize Future/WaitPolicy
retirement. #502's approval is distinct and is not reused for this decision.

C0 #500 and C1 #503 remain independent rollback units. No approval to merge any
PR was requested or granted by the two selections. #504 remains Draft while
dependency merge, cumulative CI and formal package adoption are outstanding.

F0_PER_SURFACE_APPROVAL=CONFIRMED_ACCEPT_SCOPED_EXTERNAL_BREAKAGE
F5_SCOPED_PACKAGE_AUTHORIZATION=CONFIRMED_SUBJECT_TO_FINAL_GATES
F5_PACKAGE_ADOPTION=NOT_ADOPTED
MERGE_AUTHORIZATION=NOT_GIVEN
MASTER_LIBRARY_OBJECTS=49
MASTER_ASYNC_ARCHIVE_MEMBERS=36
MASTER_INSTALLED_HEADERS=78
F3_FULL_RETIREMENT=NO
OPTIONAL_W04_SUPPORTED=NO
V1_RELEASE_READY=NO

## Subsequent explicit sequential merge authorization

On 2026-10-12 the same designated human owner instructed:

> 授权按 #500 → #503 → #504 顺序合并，满足各项门禁后完成 master 复测。

MERGE_AUTHORIZATION=CONFIRMED_SEQUENTIAL_500_503_504_SUBJECT_TO_GATES.
This supersedes the earlier absence of merge authorization, without waiving
review, CI, dependency or postmerge evidence obligations and without expanding
the five-surface scope. Existing server code/package evidence remains scoped
to its exact inputs; final HEAD review and actual master verification remain
mandatory. Formal package adoption follows those gates, not this text alone.
