# F6 Final Closeout — Findings Disposition (18/18)

Disposition of every finding from `../f6-final-correspondence-20261014/findings.md`
(frozen audit at `99d2d4b1`, frozen content `821c489f`). Each row was
independently re-adjudicated at closeout (2026-10-15) against the frozen tree
and the current GitHub state; no finding was auto-closed by its original
severity. Vocabulary: `CLOSED_DOC_CORRECTED` / `ACCEPTED_DEFERRED_WITH_SCOPE` /
`TRANSFERRED_TO_TRACKED_ISSUE` / `SUPERSEDED_BY_NEW_EVIDENCE` /
`BLOCKED_REQUIRES_REPAIR` (none used — no P0/P1 emerged).

Deferred issues created for this closeout (grouped by owner + verification
gate, not one-per-finding): #524 (formal-model maintenance), #525 (mechanical
surface cleanup), #526 (E1 error-domain residuals), #527 (post-v1 test debt),
#528 (release-time documentation).

| id | severity | affected requirement | original evidence | current claim impact | final disposition | resolution / deferral reason | owner | reopen trigger | evidence link | release gate impact |
|---|---|---|---|---|---|---|---|---|---|---|
| F6-F01 | P2 | #402 CURRENT overlay projection | #523 F6-F01; #402 body lines 1–17 vs master `df2ed3ec`/`821c489f` | none — projection-only; dated receipts govern by #402's own rule | CLOSED_DOC_CORRECTED | replacement CURRENT overlay drafted in `issue-402-current-overlay-draft.md` (this directory), applied to #402 at final acceptance; historical paragraphs preserved untouched | F6 closeout + owner | any future overlay/receipt conflict on #402 | closeout PR; #402 body after acceptance | none (does not alter any #402 gate value) |
| F6-F02 | P2 | ledger LIVE STATUS PIN (status-only projection) | #523 F6-F02; `docs/roadmap/v1-conformance.md:7` (2026-10-12 pin) | none — pin is status-only; dated sections below it are authoritative | CLOSED_DOC_CORRECTED | new dated LIVE STATUS PIN (2026-10-15) records 15 library objects / 13 async archive members / 25 installed headers, F2/F3/F5 CLOSED, F4 ADOPTED_SCOPED, F6 technical audit complete pending owner acceptance; the 2026-10-12 pin is retained verbatim under a clearly dated "superseded" heading — historical numbers are not rewritten | F6 closeout | any future pin/receipt divergence | closeout PR ledger diff | none |
| F6-F03 | P2 | REQ-01/REQ-05 model correspondence; CURRENT_…_CORRESPONDENCE gate context | #523 F6-F03; `formal/tla/RequestCore.tla:432` ReleaseBind | no current claim invalidated: matrix REQ-05 is `MODEL_SUPERSET` with the flavor disclosed; REQ-01 discloses it in its scope column; no refinement proof claimed; superset safety/liveness pass is conservative over the C++ sub-protocol | TRANSFERRED_TO_TRACKED_ISSUE → #524 | closeout re-verified the reuse premises at the frozen tree: `git diff eb7bf055..821c489f -- formal/tla/ scripts/verify_tla.sh` empty; `request_core.cpp`/`request.hpp` byte-identical from F2 head `27a5041a` to `821c489f`; "model is conservative" is recorded as the *scope* of the claim, never substituted for a refinement proof (none exists or is claimed) | formal-model owner (post-v1) | any exact reclaim-sub-graph correspondence claim; any request_core release/reclaim transition change; any refinement-proof use | #524; matrix REQ-01/REQ-05 rows; `formal-assets.tsv` RequestCore.tla row | none — W-04-independent; model-side maintenance only |
| F6-F04 | P2→P3 | REQ-06/L1 recency; VERIFY-03 | #523 F6-F04; `raw/tla-results.txt` (in-flight at audit close) | none — closure authority for REQ-06/L1 is and remains the E2-dated complete receipt (26,662,372-state behavior graph, 171/171 gate PASS at `eb7bf055`) under F2's exact historical-reuse ruling; the fresh run launched by the audit **completed clean** at the frozen tree on 2026-10-15 01:33 +0800 (COMPLETE_PASS: 153,347,909 generated / 26,662,372 distinct / 0 on queue / depth 53 / 3h10m / "Model checking completed. No error has been found.", all 12 temporal branches clean on the complete space) — the complete graph is *exactly* the E2 receipt's 26,662,372 states, directly confirming the reuse premises at `821c489f` | SUPERSEDED_BY_NEW_EVIDENCE | the finding's premise (no fresh full liveness run at the frozen tree) is eliminated by the completed run, which downgrades it to P3 per its own rule; the fresh result is supplementary recency evidence, never backdated to the audit window and never substituted for the E2 authority | formal-model owner | any future liveness violation in any run of this configuration (would reopen REQ-06/L1 as P0) | `rcore-live-final-status.md`; `../f6-final-correspondence-20261014/raw/tla-results.txt`; `../f2-authority-contraction-20261013/formal/rcore-live-reuse.md` | none (no violation found; authority unchanged) |
| F6-F05 | P3 | NO_UNQUALIFIED_CANONICAL_DOC_CLAIMS (not violated) | #523 F6-F05; `CHANGELOG.md [Unreleased]` | none — historical log, no canonical claim | TRANSFERRED_TO_TRACKED_ISSUE → #528 | refresh deferred to release-note time per its own workflow (generator + curation) | release-notes owner | v1 release-note drafting | #528 | none |
| F6-F06 | P3 | doc index (ADR navigation) | #523 F6-F06; `docs/README.md` ADR table | none — navigation gap, both ADRs already carry their own authority banners | CLOSED_DOC_CORRECTED | ADR-0003 (ADOPTED, DESIGN_AUTHORITY_ONLY) and ADR-0004 (ADOPTED via PR #453) rows added to the docs/README ADR table in this closeout PR | F6 closeout | none | closeout PR docs/README.md diff | none |
| F6-F07 | P3 | stale wording in app docs | #523 F6-F07; `apps/sluice-copy/README.md:223`, `apps/sluice-grep/README.md:79` | none — non-instructional history sentences | CLOSED_DOC_CORRECTED | both sentences reworded in this closeout PR to past-tense retirement wording (docs-only) | F6 closeout | none | closeout PR apps README diffs | none |
| F6-F08 | P3 | RETIRED_ROLE_CODE gate (disclosed, gate = 0 stands) | #523 F6-F08; `include/sluice/async/fiber.hpp:64` | none — no `Scheduler` definition exists anywhere; friendship confers no capability (verified again at closeout) | TRANSFERRED_TO_TRACKED_ISSUE → #525 | header-only mechanical removal in a cleanup slice; cannot be done in F6 closeout (production-header change forbidden) | surface owner | any Scheduler definition reappearing; cleanup slice touching headers | #525 | none — gate basis already discloses the residue |
| F6-F09 | P3 | installed-surface warning hygiene | #523 F6-F09; `raw/cleanroom-consumers.txt` | none — project gates compile `-Wall -Werror` | TRANSFERRED_TO_TRACKED_ISSUE → #525 | header-only fix; grouped with F6-F08/F17 (same owner + verification gate) | surface owner | cleanup slice | #525 | none |
| F6-F10 | P3 | ERR-01 (V15/V22 layer-A notes) | #523 F6-F10; E1 record | **adjudicated DOC_TABLE_DRIFT (documentation/verification debt), not contract divergence**: closeout traced both paths to the *same* shared oracle — direct `src/blocking_file.cpp:170,185,234` and request `src/async/async_io_context.cpp:24` + `uring_backend.cpp:103-115` + `threadpool_backend.cpp:147-162` all call `sluice::detail::precheck_state_op` + `rejection_of`/`accept_or_reject`; the errno→canonical table exists once (`include/sluice/error.hpp:71-80`) and matches root ERR-01 verbatim | TRANSFERRED_TO_TRACKED_ISSUE → #526 | residual is verification completeness only: no dedicated test re-diffs request-path submission failure spellings against the direct-path tables; the re-diff test is the work item | E1 owner | any new submit-path failure spelling added outside the shared oracle | #526; this table's adjudication section | none |
| F6-F11 | P3 | ERR-01 | #523 F6-F11; `src/async/stackful_io_host.cpp:21-22,438` | **adjudicated latent divergence confined to the unsupported W-04 candidate host**: `bad_alloc`→`no_space` (line 21-22) and task-slot capacity→`no_space` (line 438) conflate allocation/admission-capacity domains with filesystem no-space, contrary to root ERR-01's domain-distinction rule *if that surface were supported*; it is not (ADR-0003 DESIGN_AUTHORITY_ONLY; candidate-labeled install; W04_EXTERNAL=N/A_WITH_NEGATIVE_PACKAGE_PROOF). The canonical shipped paths contain no bad_alloc→error mapping at all and preserve the distinction | TRANSFERRED_TO_TRACKED_ISSUE → #526 | not upgraded to blocking: no current required conformance claim covers candidate-host behavior, and the root's W-04 obligation is explicitly conditional on a shipped/supported profile; fix is mandatory before any W-04 support promotion (recorded as a hard trigger) | E1 owner + W-04 owner | any attempt to promote StackfulIoHost/W-04 to supported | #526; this table's adjudication section | conditions W-04 only (already N/A) |
| F6-F12 | P3 | OBS-04 evidence scope | #523 F6-F12; E1-era adapter seams vs request seam | none — clause closed by request-seam evidence (fault paths in `request_core_protocol_test` + contract observer arms) | CLOSED_DOC_CORRECTED | scope note recorded in the ledger's F6 closeout dated section: adapter-level oracle is historical (E1-era seams); current OBS-04 correspondence is at the request seam | F6 closeout | any OBS-04 claim citing the retired adapter oracle as current | closeout PR ledger section | none |
| F6-F13 | P3 | LIFE-01 evidence directness | #523 F6-F13 | none — clause closed by composition/registration-lifetime evidence; directness improvement only | TRANSFERRED_TO_TRACKED_ISSUE → #527 | dedicated move-mutant oracle as a future test slice (red-against-mutant discipline required) | test owner | next lifetime test slice | #527 | none |
| F6-F14 | P3 | THREAD-01 test gap | #523 F6-F14 | none — contract established; corner unexercised | TRANSFERRED_TO_TRACKED_ISSUE → #527 | deterministic multi-thread multi-submit test as a future test slice, grouped with F6-F13 | test owner | next concurrency test slice | #527 | none |
| F6-F15 | P3 | historical record clarity | #523 F6-F15; `docs/review/f1-requirement-evidence-matrix.md` | none — file correctly frozen as-of its baseline | SUPERSEDED_BY_NEW_EVIDENCE | the F6 requirements matrix (`../f6-final-correspondence-20261014/requirements-matrix.tsv`) is the current anchor set; the F1 file stays frozen as a dated historical record and its supersession is now recorded here and in the closeout receipt | F6 closeout | none | F6 matrix; this file | none |
| F6-F16 | P3 | internal seam classification (BACKEND-01) | #523 F6-F16 | none — no public claim depends on either seam's classification | TRANSFERRED_TO_TRACKED_ISSUE → #528 | future seam documentation grouped into the release-time docs issue | seam owner | next seam-docs touch | #528 | none |
| F6-F17 | P3 | BACKEND-01/SHUT-01 route duplication | #523 F6-F17 | none — behavior correct; maintenance hazard only | TRANSFERRED_TO_TRACKED_ISSUE → #525 | mechanical cleanup when next touched; grouped with F6-F08/F09 | surface owner | cleanup slice | #525 | none |
| F6-F18 | P3 | release planning (#475 external use) | #523 F6-F18; #475 (CLOSED; residual recorded) | none for F6 gates; external-use risk remains UNKNOWN (not zero) | ACCEPTED_DEFERRED_WITH_SCOPE | no in-repo evidence resolves external use of retired surfaces either way; the F6 final receipt records UNKNOWN explicitly so release planning cannot read it as zero; owner decision deferred to the release gate (#390 scope), not re-tracked as a new issue to avoid duplicating the release gate | owner (release planning) | v1 release planning; any external-use evidence emerging | F6 final receipt limitation list; `final-acceptance-decision.md` | informs RELEASE_AUTHORIZATION (stays NOT_GRANTED); no F6 gate impact |

## E1 adjudication detail (F6-F10 / F6-F11)

Adjudicated at closeout against root ERR-01 (`docs/explicit-io-v1-final-decision.md`
"ERR-01 Error domains") and the frozen tree `821c489f`:

1. **Shared-oracle confirmation (F6-F10 = DOC_TABLE_DRIFT, no divergence).**
   Root ERR-01's canonical mapping is implemented exactly once:
   `include/sluice/error.hpp:71-80` (`ENOENT/ENOTDIR→not_found`,
   `EACCES/EPERM→permission_denied`, `ENOSPC/EDQUOT→no_space`,
   `EINTR→interrupted`, `EAGAIN/EWOULDBLOCK→would_block`, others →
   `backend_error` + native detail). Both execution paths call the same
   precheck oracle: direct path `src/blocking_file.cpp:170,185,234`; request
   path `src/async/async_io_context.cpp:24` (`initiation_rejection`) and both
   backends (`src/async/uring_backend.cpp:103-115`,
   `src/async/threadpool_backend.cpp:147-162`) via
   `sluice::detail::precheck_state_op` + `rejection_of`/`accept_or_reject`
   (`include/sluice/detail/file_semantics.hpp:137-153`). There is no separate
   submit-path errno table to have drifted. Residual debt = a dedicated
   re-diff test (tracked in #526).
2. **bad_alloc→no_space confinement (F6-F11 = latent, candidate-only).**
   The only `bad_alloc` handler in production sources is
   `src/async/stackful_io_host.cpp:21-22` (`map_current_exception_to_task_error`),
   plus the task-slot capacity → `no_space` refusal at line 438 — both inside
   the W-04 candidate host that v1 explicitly does not ship or support
   (`W04_EXTERNAL = N/A_WITH_NEGATIVE_PACKAGE_PROOF`,
   `OPTIONAL_W04_SUPPORTED=NO`). Root ERR-01's "capacity exhaustion is
   distinct from filesystem no-space" holds on the entire contracted surface.
   Because no current required conformance claim covers candidate-host
   behavior, this stays P3 and does not block F6; it is a mandatory
   precondition recorded in #526 for any future supported W-04 profile. No
   severity was lowered to hide this: the original P3 already stated the
   ambiguity; the closeout adds the precise confinement finding.

## Summary

CLOSED_DOC_CORRECTED: 5 (F01, F02, F06, F07, F12) · TRANSFERRED_TO_TRACKED_ISSUE:
10 (F03→#524, F05/F16→#528, F08/F09/F17→#525, F10/F11→#526, F13/F14→#527) ·
SUPERSEDED_BY_NEW_EVIDENCE: 2 (F04, F15) · ACCEPTED_DEFERRED_WITH_SCOPE: 1
(F18) · BLOCKED_REQUIRES_REPAIR: 0. (F6-F04: the fresh RequestCoreLive run
completed clean at the frozen tree on 2026-10-15 01:33 — see
`rcore-live-final-status.md`.)

Additional corrections discovered during the closeout and its independent
review (same category as F6-F06, recorded here for completeness):
`docs/README.md`'s current-documents table said "Sole normative root (v1-r3)"
— corrected to v1-r4 in this closeout PR; and three requirements-matrix rows
(W-04, HOST-01, HOST-03) parenthesized the StackfulIoHost header as
"(not installed)" while it is installed as OPTIONAL_CANDIDATE-labeled closure
(counted in the 16 async headers of package-and-claims.md) — the
parentheticals are corrected in this closeout PR; the rows' verdicts
(DEFERRED_OUT_OF_SCOPE / NOT_APPLICABLE, not shipped/supported) were and
remain accurate. No other projection drift was found.

P0 = 0, P1 = 0 at closeout: no finding required a production, build, test or
formal-model change to make a current conformance claim true, and the two E1
items adjudicated above did not uncover an actual contract divergence on the
shipped surface.
