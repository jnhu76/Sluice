# F6 Round 6 — Findings (frozen master 821c489f)

Field set per finding: **id · severity · category · affected claims/gates ·
description · evidence · disposition · recommended owner/action**.
Severity vocabulary: P0/P1 = final-acceptance blockers (none found);
P2 = owner action recommended at final acceptance; P3 = hygiene/informational.
All findings are documentation, evidence-provenance or model-maintenance
items — none requires a production, build, test or formal-model change to
make a current conformance claim true, and none invalidates a gate in
gates.json.

---

**F6-F01 · P2 · documentation projection staleness · #402 gates E_REQUIRED_CLOSURE_ADOPTED context**
The #402 body's `CURRENT` overlay still says "F5 MERGED / POST-MERGE CI
VERIFICATION OUTSTANDING", lists #519/#520/#521 as open and `F6=PENDING`.
All three issues are merged/closed, F5 post-merge verification is recorded
on actual master (df2ed3ec, 821c489f), and F6 has executed.
Evidence: /tmp fetch of #402 body lines 1–17; master log fdb4b743..df2ed3ec.
Disposition: non-blocking; dated receipts supersede the overlay by #402's
own rule. Action: owner refreshes the overlay at final acceptance.

**F6-F02 · P2 · ledger projection staleness · MIG/F-phase status projection**
The ledger's topmost `LIVE STATUS PIN` (docs/roadmap/v1-conformance.md:7,
dated 2026-10-12) still pins "46 library objects / 33 async archive members /
73 installed headers", `F3_FULL_CLOSEOUT=PENDING` and "#474 OPEN F3
closeout". Current truth: 15/13/25 (F5 contraction), F4 ADOPTED_SCOPED, F5
CLOSED, #474 closed.
Disposition: non-blocking; the pin is explicitly status-only and the dated
sections below it are authoritative. Action: replace the pin with a
post-F5/F6 snapshot at final acceptance.

**F6-F03 · P2 · model correspondence · REQ-01/REQ-05, CURRENT_…_CORRESPONDENCE gate**
`RequestCore.tla` retains `ReleaseBind(s)` (:432) — the compatibility
release flavor modeling the pre-F2 caller-owned `Completion` release edge
that production deleted (F2). The model is therefore a strict superset of
the C++ protocol: every C++ behavior is modeled, plus one legal-in-model
edge no current code takes. This is safety-conservative (more states, same
invariants) and the model header documents the flavor, but the
state/transition correspondence is partially stale.
Disposition: non-blocking for v1 closure (no current claim asserts exact
1:1 correspondence for the reclaim sub-graph; safety/liveness results are
valid over a superset). Action: post-v1 model maintenance converges the
model or documents the flavor permanently in the formal evidence map.

**F6-F04 · P2 (conditionally P3) · evidence recency · REQ-06/L1, VERIFY-03**
The current closure authority for `RequestCoreLive.cfg` is the E2-dated
receipt (~26.6M states, 171-run gate) accepted with F2's historical-reuse
ruling; at audit entry no fresh full liveness run existed at the converged
frozen tree. F6 launched a fresh run; its status at audit close is recorded
in raw/tla-results.txt. If it completed clean, this finding downgrades to
P3 (recency improved); if incomplete, the E2 receipt remains the authority
and the finding records that basis honestly.
Disposition: non-blocking either way (F2's reuse ruling + safety re-runs at
the frozen tree carry the gate).

**F6-F05 · P3 · documentation hygiene · NO_UNQUALIFIED_CANONICAL_DOC_CLAIMS gate (not violated)**
CHANGELOG `[Unreleased]` still accumulates pre-contraction Scheduler-era
entries and has no F2–F5 entries; generator base paths predate the
contraction. It is a historical log, so this is hygiene, not a canonical
claim for retired authority.
Action: refresh `[Unreleased]` at release-note time.

**F6-F06 · P3 · documentation index**
docs/README.md's ADR table lists only ADR-0001/0002(+views); ADR-0003
(narrow stackful I/O host) and ADR-0004 (shutdown states) — both load-bearing
for v1 (DESIGN_AUTHORITY_ONLY / ADOPTED) — are absent from the index.
Action: add both rows.

**F6-F07 · P3 · stale wording in app docs**
apps/sluice-copy/README.md:223 and apps/sluice-grep/README.md:79 say
"Retained Scheduler/Fiber compatibility" — the compatibility layer those
sentences describe was retired by F2/F5.
Action: reword at next app-doc touch.

**F6-F08 · P3 · inert residue in an installed header · RETIRED_ROLE_CODE gate (disclosed)**
include/sluice/async/fiber.hpp:64 keeps `friend class Scheduler;` plus the
private setters only that nonexistent class could call. No `Scheduler`
definition exists anywhere in the canonical tree (verified), so the
declaration confers no capability and no authority — classified inert
cosmetic residue, not retired role code. It is nonetheless visible on the
installed surface.
Action: delete in a mechanical cleanup slice (comment/header-only, no
behavior).

**F6-F09 · P3 · warning hygiene on installed surface**
`detail/reference_ready_sink.hpp` emits one `-Wextra` unused-parameter
warning in consumer builds (raw/cleanroom-consumers.txt). Project gates
compile at `-Wall -Werror` and are unaffected.
Action: silence in a mechanical cleanup slice.

**F6-F10 · P3 · E1 residual · ERR-01 (and V15/V22 layer-A notes)**
Submit-path errno-table alignment with the direct-path oracle tables is
still open (E1 record). The shared oracle and semantic_errno_mapping_test
cover the direct path; the request-path submission failure spellings were
never re-diffed against the tables.
Action: E1-owner ticket; no v1 behavior change implied.

**F6-F11 · P3 · E1 residual · ERR-01**
`bad_alloc` → `no_space` mapping ambiguity (allocation failure domain vs
ENOSPC errno domain) remains unadjudicated.
Action: E1-owner ticket.

**F6-F12 · P3 · oracle level · OBS-04**
The adapter-level post-accept failure oracle is historical (E1-era adapter
seams). Current correspondence for OBS-04 is at the request seam (fault
paths in request_core_protocol_test + contract_observer arms), which is
sufficient for the clause, but the adapter-level discrimination no longer
exists.
Action: record in ledger OBS row scope note at final acceptance.

**F6-F13 · P3 · evidence directness · LIFE-01**
The File-borrow move-clause arm is covered indirectly (composition tests,
registration lifetime) rather than by a dedicated move-mutant oracle.
Action: candidate for a future test slice; not a conformance gap.

**F6-F14 · P3 · test gap · THREAD-01**
No dedicated multi-thread multi-submit ThreadPool test exercises the public
concurrency matrix's concurrent-submission corner (cutover + progress-race
tests cover the serialized seam and wake paths).
Action: candidate for a future test slice; contract itself is established.

**F6-F15 · P3 · historical record clarity**
docs/review/f1-requirement-evidence-matrix.md (correctly frozen as-of its
baseline) cites `runtime_waiter_observer_test`, retired by F2. Readers must
use this audit's requirements-matrix.tsv for current anchors.
Action: none (this file records the supersession).

**F6-F16 · P3 · internal seam classification · BACKEND-01**
`acquire_execution` is classified protocol-test surface (F2 record), and the
ready-sink rebind contract is documented only at the seam level. No public
claim depends on either.
Action: none required for v1; note for future seam docs.

**F6-F17 · P3 · redundant route · BACKEND-01/SHUT-01**
Two internal routes reach the backend admission-closed state (context
shutdown path and backend-internal close). Both converge on the same
serialized state; behavior is correct, the duplication is a maintenance
hazard only.
Action: mechanical cleanup slice if touched.

**F6-F18 · P3 · informational · release planning**
#475 (external use of retired surfaces) is UNKNOWN, not zero. No in-repo
evidence resolves it either way.
Action: owner decision at release planning; does not affect F6 gates.

---

Blocker count: **P0 = 0, P1 = 0**. Final technical decision therefore
follows the no-blocker branch (see gates.json and final-receipt.md).
