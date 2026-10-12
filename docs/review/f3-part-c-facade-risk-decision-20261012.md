# Part C facade candidate — per-surface owner decision packet

Execution #498, parents #402/#474; root v1-r4 PROD-02/03, MIG-01/02,
HOST-01 and F0 §1.7 govern this candidate. Base master
cb977a12b951098d65942f4b4020189c7dad1a5e remains the adopted 49/36/78 package.
This packet covers exactly three new source files and five headers below.
The #502 async/file.hpp authorization covers none of them. Existing #475
Future/WaitPolicy and Scheduler closure KEEP decisions remain unchanged.

STATUS=DRAFT_NONADOPTED. Human per-surface decision, new F5 cut adoption and
merge authorization are pending. External use is UNKNOWN for every surface;
zero in-tree consumers never resolves that uncertainty.

## Common verified exposure and consumer boundary

Every candidate was a public source header and physically installed in both
original F1 profiles and B-01 v3. Static async archives shipped the three
corresponding objects. This is deliberate pre-v1 source/install/static-link
breakage; absence of stable ABI or supported-v1 claims does not authorize it.

All four apps have adopted canonical execution. #500 migrates the last COPY
selftest edge, and #503 re-owns the five real Scheduler waiter oracles directly.
The candidate preserves these tests as runnable consumers, including actual
Fiber resume, once-only delivery/route, cancellation with an accepted physical
borrow, reserve-before-attach capacity refusal and pinned-delivery cancellation.
No test was deleted to achieve zero consumers. Include-only forward declarations
in negative firewall probes are incomplete-type absence checks, not facade use.

## Exact independent decisions requested

| Surface / public header | Exported callables and historical capability | Replacement or adopted exclusion | External risk and breakage / decision |
| --- | --- | --- | --- |
| Runtime facade / async/application_runtime.hpp | RuntimeBuilder::backend/workers/build; ApplicationRuntime destructor/start/submit/request_stop/drain/join/shutdown; RuntimeTaskFn; RuntimeTaskContext::cancel_token, Completion-based submit_read/write/sync_data/sync_all and submit_*_request, await_completion and cancel_waiter overloads | Multi-worker generic runtime is outside admitted v1 PROD-02/03. Required I/O uses AsyncIoContext/Request/RequestScope and application-owned progress; retained optional candidate StackfulIoHost is separate. Fixed Scheduler tests own local Fibers/stacks and direct driver joins. | UNKNOWN external code may use admission, task cancellation and lifecycle wrapping. Removing the header/TU breaks compilation and links; canonical APIs are not drop-in replacements. Proposed ACCEPT_SCOPED_EXTERNAL_BREAKAGE with no extra window, or owner-specified bounded window/KEEP. Not approved. |
| Completed-return helpers / async/await_op_helpers.hpp | AwaitOpTally; await_take size/void; await_drain size; await_read_once; await_read_fill; await_write_exact | Existing canonical/direct composition and adopted COPY application execution own read-fill/write-exact and primary-error/settlement behavior. These RuntimeTaskContext convenience entry points have no remaining callers. | UNKNOWN external buffer composition/tally consumers lose source and static-link entry points. Runtime convenience exclusion does not silently remove File composition obligations. Proposed separate ACCEPT/no extra window, bounded window or KEEP. Not approved. |
| Task result / async/task_result.hpp | Independent TaskResultSlot<T>::publish/wait_and_take; translate_task_exception<T>; run_task_to_result<T,Fn>. First-publish mailbox and bad_alloc->no_space, system_error->canonical native error, other exception->backend_error are distinct capabilities. | Generic runtime/mailbox helper is outside admitted v1; no production/test instantiation remains. Apps and StackfulIoHost retain their own actual exception boundaries. COPY copy_exception and StackfulIoHost map_current_exception_to_task_error preserve the three error categories. Local waiter tests use standard promise/future, not a second TaskResultSlot. | UNKNOWN users may consume only the mailbox or only exception translation. Header removal breaks them even though no out-of-line template symbol needs deletion. No automatic equivalence or replacement API is promised. Proposed separate ACCEPT/no extra window, bounded window or KEEP. Not approved. |
| Group / async/group.hpp | Group() and Group(Scheduler&), destructor, async<Fn>, await, cancel, size, group_token. Default Group independently launches one thread per task, joins it and awaits Future<void>; evented Group owns Fiber stacks and futures through settlement. | No in-tree caller remains after the facade/test migration. Generic threaded/evented task group is outside admitted v1 and is not replaced by RequestScope (which owns I/O Requests, not task/thread semantics). Future<void> remains retained and linkable. | UNKNOWN external threaded Group use is independent of ApplicationRuntime. Default construction/async/await loses both source and links; cancel/join behavior is deliberately excluded, not declared redundant. Proposed separate ACCEPT/no extra window, bounded window or KEEP. If KEEP, retain group.cpp/group.hpp/evented policy closure and recapture the smaller package delta. Not approved. |
| Scheduler policy adapter / async/evented_wait_policy.hpp | Inline EventedWaitPolicy(Scheduler&), wait_until_ready and notify_ready; adapts Future waiting onto Scheduler suspension/wake handle | It has an independent Scheduler/Future adaptation purpose; no in-tree consumer remains outside removed Group. Scheduler, Future, WaitPolicy and default_wait_policy retain their installed closure and behavior. | UNKNOWN users may directly combine EventedWaitPolicy with Future. Header removal is source/install breakage; no independent out-of-line exported symbol absence is claimed for inline methods. Proposed separate ACCEPT/no extra window, bounded window or KEEP. If KEEP it remains a standalone compatibility header without restoring ApplicationRuntime. Not approved. |

For each row the owner should explicitly accept UNKNOWN_EXTERNAL_USE breakage,
select a named bounded deprecation window, or KEEP. The five are not among #475
enumerated fifteen headers. Removing Group/EventedWaitPolicy meets only a
necessary prerequisite to reconsider Future/WaitPolicy someday; it does not
change that pair's present KEEP or authorize wait_policy.cpp removal.

## Installation, source and rollback scope

Full candidate source removal: src/async/application_runtime.cpp,
src/async/await_op_helpers.cpp, src/async/group.cpp. Full candidate header
removal: the five table entries. xmake source/install globs resolve the
physical absence; no unused public declarations are shipped for removed TUs.
The facade friend/forward declaration leaves Scheduler; retained public
execution-tag access remains and no additional installed callable is removed.

Conditional expected delta: 49 -> 46 library objects, 36 -> 33 async members,
78 -> 73 installed headers, with exact defined/undefined symbol checks.
Actual fresh package capture controls the result, not these expectations.
A partial owner approval requires a separately captured partial contraction;
no count is a mandatory KPI. KEEP rows can be split into a second rollback unit.
Restore the paired source/header files and Scheduler friend edge by reverting
only C2. C0 and C1 remain valid independent test migrations.

Completion, AsyncIoContext, Request/RequestScope/RequestCore, ProgressSource,
ThreadPoolBackend/UringAsyncBackend, Scheduler/sync/select, Fiber/fiber_ctx,
StackfulIoHost, Future/WaitPolicy/default_wait_policy all remain. Canonical,
retained Scheduler and optional-host dependency closures must remain identical,
and retained compatibility positive consumers must link/run against installed
prefixes in n/y. Clean-room removed-header probes must fail for the intended
missing header; original out-of-line runtime/helper/Group link probes must
demonstrate actual removed definitions. Inline template/policy reconstruction
is not evidence of missing exported symbols.

## Evidence and formal/document responsibility

Required current request result/lifetime/progress/observer obligations remain
with canonical production tests and package contract consumers. Five
Scheduler/Fiber host obligations remain with #503, with actual deterministic
controlled gates and sanitizer executions; Group's generic task-group behavior
is an explicitly disclosed excluded capability requiring its own owner choice.
Retained Future/WaitPolicy installed link semantics have a dedicated positive
probe. Existing historical facade/model reports are dated evidence, not claims
that the removed facade still implements v1. No historical F1/B-01 asset or
model is rewritten or relabeled as proof of this candidate.

New versioned package ID is F3_PART_C_B01_FACADE_CANDIDATE_V4_20261012.
It is a candidate, not B-01/F5 adoption. Required evidence includes fresh exact
Git archives, independent n/y source/config/build/install roots, resolved xmake
objects, ar members, nm definitions/undefined references, installed headers,
compiler dependency closures, canonical/host/backend and retained compatibility
executables, negative old-header/link probes and reproducible package hashes.
Independent review, exact final HEAD evidence and authorized dependency merges
remain gates. Master counts remain 49/36/78 until an explicitly authorized merge
and fresh postmerge capture; candidate counts must never fill master fields.

F0_PER_SURFACE_APPROVAL=PENDING
F5_PACKAGE_APPROVAL=PENDING
MERGE_AUTHORIZATION=NOT_GIVEN
F3_FULL_RETIREMENT=NO
OPTIONAL_W04_SUPPORTED=NO
V1_RELEASE_READY=NO
