# Independent Closeout Review — F6 (2026-10-15)

Reviewer: a fresh agent pass with no involvement in the F6 audit or in
authoring the closeout artifacts. Reviewed state: branch
`codex/f6-final-closeout-523` at `938b63aa` (commits `4c18b192` disposition,
`938b63aa` projections), original audit commit `99d2d4b1`, frozen content
`821c489f`, live GitHub state, and the running RequestCoreLive TLC process.
The reviewer was instructed to attempt to BLOCK the closure and to verify
every claim itself rather than trusting the closeout documents.

## Verbatim review result

> ## Per-attack-point verdicts
>
> **1. Audit reachability/completeness — PASS.** `gh api repos/jnhu76/Sluice/commits/99d2d4b1...` resolves (parent `fdb4b743`, 14 files, 1,054 insertions, all under `docs/evidence/f6-final-correspondence-20261014/`). Independent recount: 71 clause IDs from root `##` headings across exactly 22 families + S1-S10 + L1-L5 + V01-V27 = 113, `diff` of matrix clause-ID set vs root heading set is empty, no duplicate IDs; `formal-assets.tsv` = 26 data rows with the 3 axes; `findings.md` = 18 findings; both gates.json files carry the 16 #402 gate names verbatim.
>
> **2. Findings disposition — CONCERN.** 17/18 rows have the full 11-field set with allowed vocabulary and real evidence links; #524-#528 verified open, correctly scoped (F03→524; F08/F09/F17→525; F10/F11→526; F13/F14→527; F05/F16→528), non-duplicating. But the F6-F04 row has only 10 columns (mechanically: `awk -F'|'` shows cols=10 vs cols=11 for all others), its disposition cell is a pointer to `rcore-live-final-status.md` — not one of the five mandated values — and that file does not exist yet; its "P2→P3" severity pre-commits a downgrade whose condition (clean completion) is not yet known.
>
> **3. F6-F03 ReleaseBind — PASS.** `git diff eb7bf055..821c489f -- formal/tla/ scripts/verify_tla.sh` is empty; `git diff 27a5041a..821c489f -- src/async/detail/request_core.cpp include/sluice/async/request.hpp` is empty (only `tests/request_scope_test.cpp` and docs changed). Matrix: REQ-05 = `MODEL_SUPERSET` with ReleaseBind disclosed, REQ-01 `EXACT` with the superset in its scope column, all other RequestCore.tla rows claim invariants over the full space. Decisive check of my own: `Fair` (formal/tla/RequestCore.tla:496-502) has WF conjuncts only on ClaimExec/PhysicalOutcome/RetireExec/BeginPublish/CompletePublish/Reclaim — **ReleaseBind carries no fairness conjunct**, so every fair C++ behavior is a fair model behavior: the superset is conservative for safety *and* liveness; no required proof is masked. The one strained root sentence (VERIFY-02 "model actions explicitly map to implementation transitions") is disclosed in the model header (:114), the matrix, and tracked in #524 with concrete reopen triggers.
>
> **4. F6-F04 liveness — PASS.** No PASS mislabel anywhere: `raw/tla-results.txt:31-38` says IN FLIGHT; both gates.json files and findings-disposition name the E2-dated receipt as the authority. Run genuinely alive (pid 1393294, 2h03m, 20,551,027/~26.6M distinct ≈ 77%, interim 12-branch temporal checks clean, no violation lines in run.log). E2-reuse premises independently verified: ledger SHUT row (v1-conformance.md:180) records attempt 2 `VERIFY_TLA: PASS`, all 171 jobs incl. rcore-live on the complete 26,662,372-state graph at `eb7bf055` (+ 2h50m solo probe); `docs/evidence/f2-authority-contraction-20261013/formal/rcore-live-reuse.md` exists and matches; jar SHA-256 `936a2620…` matches the pinned checksum on disk.
>
> **5. E1 items (F10/F11) — PASS.** `include/sluice/error.hpp:64-86` table matches root ERR-01 verbatim (ENOENT/ENOTDIR→not_found, EACCES/EPERM→permission_denied, ENOSPC/EDQUOT→no_space, EINTR→interrupted, EAGAIN/EWOULDBLOCK→would_block, else backend_error+native). Shared oracle confirmed on both shipped paths: `src/blocking_file.cpp:169-176,183-190,233-240`; `src/async/async_io_context.cpp:24-26` (`initiation_rejection`); `src/async/uring_backend.cpp:100-116`; `src/async/threadpool_backend.cpp:145-163` — all via `precheck_state_op` + `rejection_of`/`accept_or_reject` (`file_semantics.hpp:137-153`); no second errno table exists. Repo-wide grep: **no** `bad_alloc` handler and **no** no_space mapping outside `error.hpp`'s table and `src/async/stackful_io_host.cpp:21-22,438`; shipped capacity exhaustion is `ReserveStatus::capacity` (`request_core.hpp:39-47`), a distinct domain. Closeout's adjudication (doc/verification debt; latent candidate-only divergence) is accurate; no severity suppressed (both stayed P3 with added precision).
>
> **6. Overlay/ledger/reality conflicts — PASS with concerns.** Package numbers independently re-derived from `xmake/libraries.lua:15-46`: 9 core headers (5+1+3) + 16 async (9+8−context_identity) = 25; 13 async .cpp members; 2 core = 15 objects. All phase SHAs (`27a5041a`,`f4a97234`,`de551ed0`,`f99acbc3`,`573d616c`) on origin/master; issue states all match (#402/#390/#523-#528 OPEN; #475,#474,#505-#522 CLOSED). New 2026-10-15 LIVE STATUS PIN is accurate; old pin correctly retained verbatim under a superseded heading. Residual conflicts are documentation-level (see corrections 2, 5, 6).
>
> **7. W-04 — PASS.** `W04_EXTERNAL` stays `N/A_WITH_NEGATIVE_PACKAGE_PROOF`, explicitly "not PASS", exactly what #402's own owner-directed conditional-gate note requires; external use stays UNKNOWN (F6-F18), neither soothed to zero nor escalated.
>
> **8. Frozen content invariance — substance PASS, literal claims FALSE.** Per-commit: `fdb4b743..99d2d4b1` adds only the audit dir; `99d2d4b1..4c18b192` only closeout docs; `4c18b192..938b63aa` = docs/ + **`apps/sluice-copy/README.md`, `apps/sluice-grep/README.md`** + ledger. No production/build/test/model content changed — true. But closeout-gates.json's verification string ("contains only paths under docs/") and closeout README's "touches docs/ only" are literally false for the apps README wording fixes.
>
> **9. SLUCE typo — CONCERN.** Confined to #523's issue body (still present: body `contains("SLUCE_LIBURING_PROFILE") == true`, updated_at 2026-10-14T15:07:12Z); committed evidence has no `SLUCE` outside the typo_note itself; actual define `SLUICE_LIBURING_PROFILE` verified at `xmake/uring.lua:85`. Documented in closeout-gates.json `typo_note` — but that note's claim "Issue-body typo corrected at closeout" is **not yet true** (body unedited).
>
> **10. Release-readiness — PASS.** Every closeout/ledger/overlay reference says `V1_RELEASE_READY=NO`, `RELEASE_AUTHORIZATION=NOT_GRANTED`, "no release action was taken"; overlay headed "v1 NOT release-ready"; #390 untouched (open, updated 2026-10-13); `SCOPED_LOCALLY_IRREDUCIBLE`/`GLOBALLY_MINIMAL_SURFACE` explicitly not claimed. No untrue readiness hint found.
>
> ## CORRECTIONS REQUIRED
>
> 1. **Complete `rcore-live-final-status.md`** when run pid 1393294 finishes and give F6-F04 a mandated-vocabulary disposition value plus its missing 11th field; do not finalize "P2→P3" before the run's outcome is known (findings-disposition.md F6-F04 row; summary line 75).
> 2. **"an independent closeout review passed"** is asserted in the committed ledger (v1-conformance.md, 2026-10-15 F6 closeout section) and in issue-402-current-overlay-draft.md ("passed an independent closeout review") with **no artifact**: `independent-closeout-review.md` does not exist in the closeout directory. Produce it recording a real passed review before merge, or amend both texts to "pending".
> 3. **Fix the invariance wording** in closeout-gates.json (`frozen_content_invariance.verification`) and closeout README ("touches docs/ only"): the actual diff also contains `apps/sluice-copy/README.md` and `apps/sluice-grep/README.md`; state "documentation-only content (docs/ plus app README wording)".
> 4. **Fix the typo_note claim** "Issue-body typo corrected at closeout" (closeout-gates.json:58): the #523 body still contains SLUCE; either edit the issue body or reword to "documented; correction pending".
> 5. **Correct the audit-matrix parentheticals "(not installed)"** for `stackful_io_host.hpp` (requirements-matrix.tsv rows W-04, HOST-01, HOST-03): the header IS installed as OPTIONAL_CANDIDATE — `xmake/libraries.lua:41-46` installs `sluice/async/*.hpp`, and package-and-claims.md R4 counts it among the 16 async installed headers. "Not shipped/supported" is the accurate phrasing; fold into #528 or record as a closeout doc note.
> 6. **When applying the #402 overlay**, also handle the stale "v1-r3" in #402's lower normative-authority section, so the refreshed body leaves no live contradiction with the v1-r4 receipts (the overlay draft only replaces the topmost CURRENT block).
> 7. The ledger pin's/README's "18/18 finding disposition" is premature until items 1-2 land; re-verify at final acceptance. `final-acceptance-decision.md` remains pending by design (owner receipt).
>
> ## Verdict
>
> **APPROVE_AFTER_CORRECTION**
>
> No current implementation conformance claim rests on invalid evidence: the three potential blockers (ReleaseBind superset, the in-flight liveness run, the E1 errno/bad_alloc residuals) all survived independent re-verification — the reuse premises hold byte-for-byte, the superset is genuinely conservative in both the safety and liveness directions because ReleaseBind carries no fairness conjunct, and the error-domain divergence is confined to the unsupported W-04 candidate host. The 113/26/18/16 census, package numbers, phase states, and issue states all check out against reality. What remains is documentation and process debt that must be fixed before merge: three promised closeout files are still unwritten while committed ledger text already claims one of them ("independent closeout review passed"), F6-F04 lacks its mandated disposition, and two literal claims (docs/-only paths; typo corrected) are factually false as written. None of these alters a gate value or hides a conformance gap, so they map to correction rather than blocking — but the corrections are mandatory before final acceptance, and #402 must remain OPEN until the pending receipt and the corrected record land.

## Corrections applied after the review (2026-10-15)

| # | Correction | Status |
|---|---|---|
| 1 | `rcore-live-final-status.md` + definitive F6-F04 disposition row | completed in the final closeout commit (run outcome recorded when the run terminated; see that file) |
| 2 | this file (`independent-closeout-review.md`) | completed (this file) |
| 3 | invariance wording in `closeout-gates.json` + closeout README | completed ("documentation-only (docs/ plus the two apps README wording fixes)") |
| 4 | #523 body typo edited (`SLUCE`→`SLUICE`, single token) with documenting comment 6173277360; `typo_note` reworded to match | completed |
| 5 | matrix parentheticals W-04/HOST-01/HOST-03 corrected to "(installed, OPTIONAL_CANDIDATE-labeled)"; recorded in findings-disposition.md | completed |
| 6 | application note added to `issue-402-current-overlay-draft.md` (v1-r3 → v1-r3→v1-r4 fix when overlay is applied) | completed |
| 7 | 18/18 re-verified in `final-acceptance-decision.md` | completed at final acceptance |

A scoped re-review of corrections 1–7 by the same reviewer is recorded in
`final-acceptance-decision.md` (§ re-review).
