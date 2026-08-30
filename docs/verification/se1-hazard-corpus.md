# SE-1 — I/O Lifecycle Hazard Corpus (interpretation)

**DATA AUTHORITY:** [`docs/results/safety/se1-hazard-corpus.json`](../results/safety/se1-hazard-corpus.json) (`se1-corpus-schema` version 1).
This document interprets the JSON; it does not add data. Integrity is mechanically checked by
`scripts/verify-se1-hazard-corpus.py` (fail-closed; not wired into CI — human review authorizes that).

- Campaign: SE-1, authorized by issue #227 (Lane A). Corpus construction only.
- Base: `7437c8c58209f239051a8e814fd7ab44eabaada5` (== origin/master, mechanically verified 2026-08-30).
- Comparison unit: the `normalized_semantic_trace`. API spelling is provenance, not the comparison unit.
- Claim discipline: this corpus prepares evidence for T-S1b/T-S2. Both remain **NOT YET TESTED**.
  No net-safety score exists; no "Sluice is safer than X" claim is made or may be derived from this file.

## 1. Corpus shape

| Half | Entries | Notes |
| --- | ---: | --- |
| Conventional (IN-SE1) | 14 | H01–H13 all adjudicated; 0 families with NO VALID ENTRY |
| Sluice-induced (IN-SE1) | 8 | 6 production-runtime, 2 test-only, 0 internal/seam |
| OUT-OF-SE1 (preserved) | 5 | 2 production-runtime growth defects, 2 structural-authority, 1 experiment-process |
| Total | 27 | one root cause = one primary entry; aliases recorded, not re-counted |

## 2. Conventional half — family coverage H01–H13

| Family | Outcome | Entry | Quality | Sluice pairing |
| --- | --- | --- | --- | --- |
| H01 lifetime/buffer UAF | DOCUMENTED CONTRACT HAZARD FOUND | SE1-CA-H01-1 (+minimal SE1-CA-H01-2) | C1 (+C3) | PAIR-E — obligation explicit, violation silent |
| H02 request reuse / ABA | DOCUMENTED CONTRACT HAZARD FOUND | SE1-CA-H02-1 | C1 | PAIR-B — generation identity, negative-compile |
| H03 stale completion / after-close | REAL SOURCE FOUND | SE1-CA-H03-1 (CVE-2023-1872) | C0 | PAIR-C — quiescent-destruction fail-fast |
| H04 cancel vs complete | DOCUMENTED CONTRACT HAZARD FOUND | SE1-CA-H04-1 | C1 | PAIR-A — single CAS winner |
| H05 double terminal / publication | REAL SOURCE FOUND | SE1-CA-H05-1 (Axboe double-CQE patch) | C0 | PAIR-A — reap-only publication |
| H06 submit-failure rollback | DOCUMENTED CONTRACT HAZARD FOUND | SE1-CA-H06-1 | C1 | PAIR-C — transactional submit + fail-fast tail |
| H07 partial / zero-progress I/O | DOCUMENTED CONTRACT HAZARD FOUND | SE1-CA-H07-1 (OpenSSL partial-write) | C1 | PAIR-A — typed short-I/O outcomes (PARTIAL validity) |
| H08 timeout vs resource grant | DOCUMENTED CONTRACT HAZARD FOUND | SE1-CA-H08-1 (connect SO_ERROR protocol) | C1 | PAIR-A — deadline as CAS winner (PARTIAL validity) |
| H09 lost wake / timer retirement | REAL SOURCE FOUND | SE1-CA-H09-1 (glibc BZ 25847) | C0 | PAIR-D — wake protocol + DST replay |
| H10 shutdown with in-flight work | DOCUMENTED CONTRACT HAZARD FOUND | SE1-CA-H10-1 (libuv EBUSY / Asio destructor) | C1 | PAIR-C — quiescent destruction fail-fast |
| H11 resource/accounting leak | REAL SOURCE FOUND | SE1-CA-H11-1 (io_uring IOPOLL/CQE_SKIP leak fix) | C0 | PAIR-C — exactly-once retirement (PARTIAL validity) |
| H12 durability / ordering | REAL SOURCE FOUND | SE1-CA-H12-1 (PostgreSQL fsyncgate) | C0 | PAIR-X — COMPARABILITY_BLOCKED at kernel mechanism |
| H13 weak-memory publication | DOCUMENTED CONTRACT HAZARD FOUND | SE1-CA-H13-1 (memory-barriers.txt) | C1 | PAIR-D — acq_rel protocol + GenMC (PARTIAL validity) |

Provenance quality across the conventional half: **C0 = 5, C1 = 8, C2 = 0, C3 = 1** (the C3 case is
visibly labeled `CONVENTIONAL-MINIMAL`). No historical incident was invented; weak analogies were
rejected and are recorded in the JSON `rejected_candidates` section.

## 3. Sluice-induced half — buckets and eligibility

| Bucket | IN-SE1 | OUT-OF-SE1 | Entries |
| --- | ---: | ---: | --- |
| production-runtime | 6 | 2 | IN: SE1-SB-01 (Q-LIV-1), SB-02 (R2-ALLOC), SB-05 (FE P1-1), SB-06 (FE P1-2), SB-07 (FE P1-3), SB-08 (null ResumeTarget). OUT: SB-03 (deadline-heap growth, fixed), SB-04 (group.hpp `reserve(size()+1)`, OPEN) |
| test-only | 2 | 0 | SE1-SB-09 (#229 seam race), SE1-SB-10 (unconfirmed one-time TSan hang — status UNKNOWN) |
| internal/seam | 0 | 0 | none established this round |
| structural-authority | 0 | 2 | SE1-SB-11 (deadline-site duplication, fixed AC-2b), SE1-SB-12 (R2/AsyncQueue authority duplication, fixed AC-2c/FE) |
| experiment-process | 0 | 1 | SE1-SB-13 (RX-1 invalid CONTROL runs, caught by preregistered gate) |

Dedup discipline: `group.hpp` and the deadline-heap defect share one root-cause class (growth-defeating
`reserve(size+1)`) and are cross-aliased; R2-ALLOC and FE P1-1 share the failure-atomicity class;
SB-11/SB-12 share authority duplication. No bug, review finding, and regression test were counted as
three hazards anywhere.

## 4. Root-cause map (Sluice-induced)

| Root cause | Primary entries |
| --- | --- |
| authority duplication | SB-11, SB-12 (both OUT-OF-SE1, structural) |
| temporal ordering gap | SB-02 (register-before-arm) |
| lifetime ownership gap | SB-06 (QueuePort pin) |
| failure-atomicity gap | SB-05 (allocation after terminal commit) |
| accounting closure gap | — (R2-ALLOC's accounting residue is an alias of SB-02, not a separate root) |
| wake/liveness reconciliation gap | SB-01 (Q-LIV-1), SB-10 (unconfirmed) |
| synchronization gap | SB-07 (rwlock owner check) |
| representation invalid state | SB-08 (null resume target) |
| complexity/resource-growth mistake | SB-03, SB-04 (OUT-OF-SE1) |
| test-seam defect | SB-09 |
| experimental-method defect | SB-13 (OUT-OF-SE1) |

## 5. Comparability

| Validity | Count | Entries |
| --- | ---: | --- |
| FAIR | 7 | H02, H03, H04, H05, H06, H09, H10 (+ induced SB-01, SB-06, SB-07, SB-09) |
| PARTIAL | 6 | H01 (×2), H07, H08, H11, H13 (+ induced SB-02, SB-05, SB-08, SB-10) |
| COMPARABILITY_BLOCKED | 1 | H12 (kernel cleared-error semantics below any user-space obligation) (+ structural/process OUT entries) |

Pairing outcomes over the conventional half: PAIR-A ×4, PAIR-B ×1, PAIR-C ×4, PAIR-D ×2, PAIR-E ×2,
PAIR-X ×1. Every Sluice-induced IN-SE1 entry is by definition a PAIR-F instance (Sluice introduced an
adjacent protocol hazard); SE-2 will measure which layers kill them. Counts are descriptive only —
**no score is computed and none may be derived.**

## 6. Current Sluice outcomes (descriptive, IN-SE1 entries only)

| Status | Count |
| --- | ---: |
| UNREPRESENTABLE | 4 |
| STATICALLY_REJECTED | 2 |
| DYNAMICALLY_DETECTED | 4 |
| FAIL_FAST | 4 |
| DETERMINISTICALLY_REPRODUCIBLE | 4 |
| SILENT_OR_UNDETECTED | 2 |
| UNKNOWN | 1 |
| NOT_APPLICABLE | 1 |

## 7. Silent / unknown Sluice cases (retained, priority SE-2 probes)

1. **SE1-CA-H01-1/-2 (H01):** a caller destroying the borrowed buffer while a request is in flight is
   not detected by Sluice; the obligation is explicit in the contract, the violation is not mechanically
   caught (detection is delegated to external tooling such as ASan).
2. **SE1-SB-10 (H09, test-only):** one unconfirmed TSan hang in `select_event_registry_test`; record
   lives only in untracked human artifacts (E4) — 20/20 clean reruns. Kept as UNKNOWN; also a
   documentation-gap finding.

## 8. CORPUS BIAS AUDIT (adversarial pass over the first draft)

| Question | Finding | Correction / status |
| --- | --- | --- |
| A. Disproportionately hazards Sluice handles well? | Initial pairing source (project S1 evidence) risks over-selecting well-handled families. | Kept the worst-looking cases: H01 SILENT pair, H12 COMPARABILITY_BLOCKED, and the full induced half (6 production hazards Sluice itself created). No deletion-based correction needed after re-check. |
| B. Which families make Sluice look worst? | H01 (silent borrow violation), H12 (hazard below the boundary), SB-10 (UNKNOWN liveness), plus the induced half itself. | All retained and surfaced in §7. |
| C. Families only by synthetic conventional cases? | H01's second entry is synthetic (labeled CONVENTIONAL-MINIMAL). | Family anchor is C1 documentation; the C3 entry exists only as an SE-2 probe skeleton and is visibly labeled. |
| D. Same severity discipline for induced hazards? | Yes — induced entries require S0 repository evidence; one unconfirmed case is explicitly UNKNOWN instead of being dropped or upgraded. | Noted: two of the OUT adjudications (SB-03/SB-04) would have inflated the "Sluice bugs" side if counted; they are excluded for the same reason a mere crash would be excluded from H10 — category discipline, not favoritism. |
| E. Bug instances vs semantic classes? | Normalized traces are the unit; aliases dedupe repeated review-round sightings of one root cause. | Confirmed. |
| F. Structural risks counted as runtime failures? | No — SB-11/SB-12 are OUT-OF-SE1 with COMPARABILITY_BLOCKED; only concrete drift bugs (SB-01) carry runtime entries. | Confirmed. |
| G. Inconvenient silent cases excluded? | No. | §7 exists precisely for them. |
| H. Comparable abstraction levels? | Conventional C0 cases are runtime-internal (kernel) while some C1 cases are contract-level; Sluice-induced are runtime-internal. Family-level comparison is like-for-like; instance-level severity is deliberately not compared. | Recorded as residual abstraction caveat. |

**BIAS FOUND:** residual selection familiarity (Sluice half drawn from well-documented project history;
conventional half required fresh research) and zero C2-grade conventional sources this round.
**CORRECTIONS:** silent/unknown/blocked cases retained; OUT adjudication applied symmetrically;
rejected-candidate log added to the JSON.
**UNRESOLVED BIAS:** none actionable within SE-1; human adversarial review should specifically probe
H07/H08/H13 (C1-anchored families) for stronger C0 sources, and check that no real conventional case
was missed for H12's blocked comparison.

## 9. What SE-1 does NOT prove

- Not that Sluice migrates hazards better than POSIX/liburing/Asio (T-S1b untested).
- Not that Sluice has positive net safety value (T-S2 untested).
- Not that the existing witness stack (TLA+/GenMC/DST/mutation/death tests) would detect every corpus
  entry — that is exactly the SE-2 matrix.
- Not that the two SILENT/UNKNOWN cases are the only ones; they are the ones found in a bounded pass.

## 10. SE-2 readiness

- Schema frozen (`se1-corpus-schema` v1, fail-closed validator PASS).
- Every entry carries `se2_probe_candidates`; existing detection evidence is recorded but the
  detection matrix is NOT built here.
- The corpus can be consumed without redesign: families, buckets, eligibility, pairing vocabulary,
  and status enums are closed and mechanically checked.

## 11. Production / API / roadmap impact

- Production code changed: **NO**.
- Public API changed: **NO**.
- Roadmap (#221/#225/#226/#227) updated: **NO**.
- Known OPEN production-side defect `group.hpp` reserve idiom: left untouched by explicit adjudication
  (SE1-SB-04, OUT-OF-SE1).
