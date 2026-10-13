# F2 final closeout receipt — 2026-10-13

Slice #402 F2 (Request/Result/Identity authority contraction, clean break).
Baseline master `0ed16588b7101f108aa7282074ba3d568f9d34e9`. Implementation
head `093332ba1b40b667763d1202200ee899962454ae`; commits after it in this
directory are documentation-only. Every capture is bound to the implementation
head by `gates.json`.

```ini
BEGIN_F2_FINAL_CLOSEOUT
F2_STATUS = CANDIDATE_COMPLETE_OWNER_DECISION_PENDING
IMPLEMENTATION_HEAD = 093332ba1b40b667763d1202200ee899962454ae
URING_POISON_RACE = CORRECTED_8689a720_DETERMINISTIC_RED_GREEN
E1_GATE_LIFETIME_RACE = CORRECTED_e974988c_TEST_ONLY
RETIRE_DELIVERY_MOVED_FROM = CORRECTED_0bcfda01
W03 = CLOSED_APPLICATION_HANDOFF
FOUR_PROFILE_MATRIX = PASS_53_53_67_67_53_53_67_67
SANITIZERS = ASAN_UBSAN_18_OF_18_TSAN_17_OF_18_RECORDED_EXCLUSION
FOCUSED_DIFFERENTIAL_STRESS = 240_OF_240_VS_BASELINE
B01_PACKAGE = PASS_DUAL_PROFILE
INSTALLED_PROBES = PASS_WITH_NEGATIVE_CONTROL
REMOVED_HEADERS = 7
INSTALLED_HEADERS_EXPECTED = 49
ASYNC_ARCHIVE_MEMBERS_EXPECTED = 30
RCORE_LIVE_EXECUTION = INCOMPLETE
RCORE_LIVE_PROOF = VERIFIED_REUSABLE
F2_FORMAL_ACCEPTANCE = ACCEPTED_BY_REUSE
CI_AT_IMPLEMENTATION_HEAD = SUCCESS_38397035018
INSTALLED_SURFACE_DECISION = PENDING_HUMAN_OWNER
MERGE_AUTHORIZATION = NOT_GRANTED
PR_516 = DRAFT_UNMERGED
F5 = PENDING
F6 = PENDING
V1_RELEASE_READY = NO
END_F2_FINAL_CLOSEOUT
```

## Receipt notes

- The two race corrections and the moved-from guard close all three
  `known-preexisting-gaps.md` entries with deterministic RED/GREEN receipts;
  the authority-contraction audit records that no runtime state was added.
- W-03 closed with an empty production diff; the historical failure
  measurements are retained in `blocker-w03-stall.md` and the PR body.
- The six-profile matrix line aggregates the four build/test profiles plus
  the two sanitizer scopes; the one load-contaminated TSan attempt is
  preserved as `tsan-n-concurrent-attempt` with its attribution evidence.
- The installed-surface break (7 headers, 3 objects, external use UNKNOWN,
  no shims) still requires the owner's explicit scope-bound decision before
  merge; this receipt does not grant it, and no synthetic approval exists
  anywhere in the slice.
- #402 stays OPEN; F5/F6 obligations are untouched and handed over as
  pending. Nothing here claims v1 release readiness.
