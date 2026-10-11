# F3 #498 / PR #502 scoped owner decision

Recorded 2026-10-12 from the human user's Owner Decision in the execution
conversation. The same human user subsequently confirmed: "是，我就是指定 owner，并确认该决定生效". This is an owner confirmation in the execution conversation, not a GitHub review.

Activation: CONFIRMED_EFFECTIVE. The designated repository/phase owner explicitly
confirmed their role and that the decision is effective in the same conversation.
This satisfies the submitted activation clause; explicit merge authorization
remains absent.

## Compatibility disposition

Decision A: ACCEPT_SCOPED_EXTERNAL_BREAKAGE, confirmed effective within the exact scope below.

Scope is exactly the installed header sluice/async/file.hpp and these four
public functions:

- sluice::async::await_read_at
- sluice::async::await_write_at
- sluice::async::await_sync_data
- sluice::async::await_sync_all

Accept source, installed-header and static-link incompatibility for this surface
in the Clean Minimal V1 transition. The RuntimeTaskContext + Completion
completed-return interface is not retained as a v1 compatibility layer.
No additional bounded deprecation window is required under the confirmed scoped decision. External usage remains UNKNOWN, not zero.

Canonical migration targets are AsyncIoContext + Request, with RequestScope
where applicable. These replace required semantics; they are not source-compatible
drop-in replacements. Historical v0.0.1 deliberate-deprecation language remains
disclosed in the [per-surface packet](f3-498-async-file-retirement-candidate.md).
This is a deliberate, recorded pre-v1 compatibility break.

## Versioned package contraction

AUTHORIZE_SCOPED_VERSIONED_PACKAGE_CONTRACTION, confirmed effective, subject to
final implementation gates. Only the paired removal of src/async/file.cpp and
include/sluice/async/file.hpp is covered.

Both package profiles have the expected validated delta: installed headers
79 to 78; core + async objects 50 to 49; async archive members 37 to 36; the
four legacy wrapper symbol definitions removed. The current
[versioned package capture](../evidence/f3-498-async-file-candidate-v2/REPRODUCE.md)
is candidate evidence, not a replacement for the historical F1 baseline.
All historical F1/B-01 frozen assets remain immutable.

Before adoption, the physical deletion PR must satisfy exact final HEAD review,
cumulative CI, required remote tests, installed clean-room positive and negative
probes, package provenance and explicit merge authorization. Existing
[test and package receipts](f3-498-pr-b-final-receipt.md) remain dated evidence.
The independent review at efbfc88e6a1b5ae42723dda17fefeed617430685 predates
this decision record and does not constitute review of a later documentation HEAD.

## Exclusions and open gates

This decision does not authorize removal of ApplicationRuntime, Group, Scheduler
or other runtime implementations; Completion or canonical backend/context
mechanisms; Fiber/fiber_ctx or the StackfulIoHost substrate; Future/WaitPolicy
protected by #475; or any other installed header or public symbol.
PR #500 remains an independent test-preparation slice.

PR #502 remains Draft and unadopted. Cumulative GitHub CI is pending: the workflow
matches pull-request base master only, while this candidate is stacked on the
unmerged PR #501 branch. Server matrix results are separate evidence.
PR #501 is still unmerged; this decision does not authorize either PR merge.

F3_FULL_RETIREMENT = NO
OPTIONAL_W04_SUPPORTED = NO
V1_RELEASE_READY = NO
PHYSICAL_CONTRACTION_ADOPTED = NO
MERGE_AUTHORIZATION = NOT_GIVEN

F0_PER_SURFACE_RISK_DISPOSITION = CONFIRMED_ACCEPT_SCOPED_EXTERNAL_BREAKAGE
F5_SCOPED_VERSIONED_PACKAGE_CONTRACTION = AUTHORIZED_SUBJECT_TO_FINAL_GATES

## Subsequent merge authorization

The same designated owner explicitly instructed: "按照步骤合并。 我授权。"
This authorizes sequential #501 and #502 merges after the stated review, CI,
package and postmerge verification steps. It supersedes the earlier absence of
merge authorization; it does not waive implementation gates or expand scope.
#501 merged at f527456b5eaa10a74e872ee1d23b9dd6413f1f1d. #502 remains
unmerged at record time. See the [v3 package cut](../evidence/f3-498-b01-package-cut-v3/REPRODUCE.md).
