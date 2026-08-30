# SE-1 — I/O Lifecycle Hazard Corpus (interpretation)

**DATA AUTHORITY:** [`docs/results/safety/se1-hazard-corpus.json`](../results/safety/se1-hazard-corpus.json) (`se1-corpus-schema` version 1, corrected in place while Draft).
This document interprets the JSON; it does not add data. Integrity is mechanically checked by
`scripts/verify-se1-hazard-corpus.py` (fail-closed), wired into the repository mechanical gate
`scripts/gates/pre-push.sh` (and therefore CI) per SE-1-CORRECTIVE-1 human authorization: any
future change that puts the corpus into an invalid state fails the gate.

- Campaign: SE-1, authorized by issue #227 (Lane A). Corpus construction only.
- Base: `7437c8c58209f239051a8e814fd7ab44eabaada5` (== origin/master, mechanically verified 2026-08-30).
- Comparison unit: the `normalized_semantic_trace`. API spelling is provenance, not the comparison unit.
- Claim discipline: this corpus prepares evidence for T-S1b/T-S2. Both remain **NOT YET TESTED**.
  No net-safety score exists; no "Sluice is safer than X" claim is made or may be derived from this file.

## 0. Population law and semantics (SE-1-CORRECTIVE-1)

**POPULATION = `corpus_eligibility == IN-SE1` AND `entry_role == population-case`.** Probe
companions are artifacts, not denominator cases; OUT-OF-SE1 entries remain visible but are not
denominator cases. This law drives the validator counts and every table below.

- `entry_role`: closed set {`population-case`, `probe-companion`}. Only IN-SE1 population cases
  count. A probe companion carries `same_case_as` pointing at its parent population case and is
  never a second denominator entry.
- `related_entries`: semantic comparison / family relationship only; never affects counts.
- `root_cause_key` / `root_cause_class`: required for Sluice-induced population cases; the key
  identifies the concrete root-cause INSTANCE (not a broad category) and must be unique across
  induced population cases (mechanically enforced dedup).
- Source roles: provenance sources are structured `{url, role, authority}` records with closed
  vocabularies. The validator enforces **MECHANICAL PROVENANCE SHAPE** only (C0 needs a
  primary-authority incident record — `bug_record` or `official_bug_corpus` carrying
  `upstream-primary`/`official-primary` — plus an `upstream_fix` source; origin↔quality is a closed
  relation: conventional-real→{C0,C2}, conventional-documentation→{C1}, conventional-minimal→{C3},
  sluice-induced→{S0}; repo evidence that is exclusively `repo-untracked` (E4) makes the entry an
  UNCONFIRMED CANDIDATE which must be OUT-OF-SE1; etc.). Whether a URL's content truthfully matches
  its role, and whether the semantic mapping is valid, remains **HUMAN SEMANTIC PROVENANCE REVIEW**.
- `sluice_current_status` is the PRIMARY DESCRIPTIVE OUTCOME FOR CORPUS NORMALIZATION ONLY — not
  the full detection profile. Real detection layers overlap (e.g. FAIL_FAST,
  DETERMINISTICALLY_REPRODUCIBLE, TSan-detectable can co-occur). SE-2 owns the full
  hazard × detection-layer matrix.

## 1. Corpus shape

| Half | Records | Denominator notes |
| --- | ---: | --- |
| Conventional population cases | 13 | H01–H13 all adjudicated; 0 families with NO VALID ENTRY |
| Sluice-induced population cases | 7 | 5 production-runtime, 1 internal/seam, 1 test-only |
| Probe companions (artifacts, excluded) | 1 | SE1-CA-H01-2 → `same_case_as` SE1-CA-H01-1 |
| OUT-OF-SE1 (preserved, visible) | 6 | 2 production-runtime growth defects, 2 structural-authority, 1 experiment-process, 1 unconfirmed test-only candidate (SB-10) |
| Total records | 27 | one root cause = one primary entry; relations recorded, not re-counted |

## 2. Conventional half — family coverage H01–H13 (population cases only)

| Family | Outcome | Entry | Quality | Sluice pairing |
| --- | --- | --- | --- | --- |
| H01 lifetime/buffer UAF | DOCUMENTED CONTRACT HAZARD FOUND | SE1-CA-H01-1 (+probe companion SE1-CA-H01-2) | C1 | PAIR-E — obligation explicit, violation silent |
| H02 request reuse / ABA | DOCUMENTED CONTRACT HAZARD FOUND | SE1-CA-H02-1 | C1 | PAIR-B — generation identity, negative-compile |
| H03 stale completion / after-close | REAL SOURCE FOUND | SE1-CA-H03-1 (CVE-2023-1872 + verified stable fix 08681391) | C0 | PAIR-C — quiescent-destruction fail-fast (PARTIAL validity) |
| H04 cancel vs complete | DOCUMENTED CONTRACT HAZARD FOUND | SE1-CA-H04-1 | C1 | PAIR-A — single CAS winner |
| H05 double terminal / publication | REAL SOURCE FOUND | SE1-CA-H05-1 (Axboe double-CQE patch) | C0 | PAIR-A — reap-only publication |
| H06 submit-failure rollback | DOCUMENTED CONTRACT HAZARD FOUND | SE1-CA-H06-1 | C1 | PAIR-C — transactional submit + fail-fast tail |
| H07 partial / zero-progress I/O | DOCUMENTED CONTRACT HAZARD FOUND | SE1-CA-H07-1 (OpenSSL partial-write) | C1 | PAIR-A — typed short-I/O outcomes (PARTIAL validity) |
| H08 timeout vs completion arbitration | REAL SOURCE FOUND | SE1-CA-H08-1 (io_uring linked-timeout race, mainline 447c19f) | C0 | PAIR-A — deadline as CAS winner |
| H09 lost wake / timer retirement | REAL SOURCE FOUND | SE1-CA-H09-1 (glibc BZ 25847) | C0 | PAIR-D — wake protocol + DST replay |
| H10 shutdown with in-flight work | DOCUMENTED CONTRACT HAZARD FOUND | SE1-CA-H10-1 (libuv EBUSY / Asio destructor) | C1 | PAIR-C — quiescent destruction fail-fast |
| H11 resource/accounting leak | REAL SOURCE FOUND | SE1-CA-H11-1 (io_uring IOPOLL/CQE_SKIP leak fix, lore permalink) | C0 | PAIR-C — exactly-once retirement (PARTIAL validity) |
| H12 durability / ordering | REAL SOURCE FOUND | SE1-CA-H12-1 (PostgreSQL fsyncgate) | C0 | PAIR-X — policy half PARTIAL, kernel-mechanism half BLOCKED |
| H13 weak-memory publication | DOCUMENTED CONTRACT HAZARD FOUND | SE1-CA-H13-1 (memory-barriers.txt) | C1 | PAIR-D — acq_rel protocol + GenMC (PARTIAL validity) |

Provenance quality across the conventional population: **C0 = 6, C1 = 7, C2 = 0, C3 = 0** (the one
CONVENTIONAL-MINIMAL entry is the H01 probe companion and is excluded from the denominator by the
population law). No historical incident was invented; weak analogies were rejected and are recorded
in the JSON `rejected_candidates` section — including the original H08 connect(2) anchor, demoted
by SE-1-CORRECTIVE-1 because connect(2) does not document the timeout-vs-grant arbitration.

H08 notes (B1 correction): the anchor is now a verified upstream bug + fix — io_uring linked
timeout racing its request's completion (syzbot-reported double free; mainline fix
`447c19f3b5074409c794b350b10306e1da1ef4ba`, verified on git.kernel.org; liburing contract defines
the arbitration: loser is canceled with `-ETIME`, exactly one terminal). The comparison upgraded to
FAIR because both sides are the same-object winner race.

H03 notes (B2 correction): the fix record is stable commit `08681391b84da27133deefaaddefd0acfa90c2be`
("io_uring: add missing lock in io_get_file_fixed"), verified to repair the cited fixed-file UAF;
its commit message honestly records that no single mainline patch exists (the fix was absorbed into
the 5.18 file-assignment rework). The advisory's labeled "primary fix" `da24142b1ef9` was
re-verified and is a different commit; it is not used. Comparability downgraded FAIR → PARTIAL
(no mechanically demonstrated Sluice registered-resource slot lifecycle equivalent to the kernel
fixed-file table). Round 2 (review 5060124249 FIX 1): the STAR Labs advisory was demoted
`bug_record` → `supporting`, so H03's C0 rests solely on primary-authority provenance — NVD
(`official_bug_corpus`/`official-primary`) + verified fix (`upstream_fix`/`upstream-primary`) —
as now mechanically enforced.

## 3. Sluice-induced half — buckets and eligibility

| Bucket | IN-SE1 | OUT-OF-SE1 | Entries |
| --- | ---: | ---: | --- |
| production-runtime | 5 | 2 | IN: SE1-SB-01 (Q-LIV-1), SB-02 (R2-ALLOC), SB-05 (FE P1-1), SB-06 (FE P1-2), SB-07 (FE P1-3). OUT: SB-03 (deadline-heap growth, fixed), SB-04 (group.hpp `reserve(size()+1)`, OPEN) |
| internal/seam | 1 | 0 | IN: SE1-SB-08 (null ResumeTarget) — per the #227 hazard-bucket taxonomy: "internal/seam representation hazard: nullable Fiber-kind token", an orthogonal bucket; "Do not mix these counts in SE-1/SE-2 net-safety statistics" |
| test-only | 1 | 1 | IN: SE1-SB-09 (#229 seam race). OUT: SE1-SB-10 (UNCONFIRMED CANDIDATE — E4-only evidence, status UNKNOWN; SE-2 priority probe) |
| structural-authority | 0 | 2 | SE1-SB-11 (deadline-site duplication, fixed AC-2b), SE1-SB-12 (R2/AsyncQueue authority duplication, fixed AC-2c/FE) |
| experiment-process | 0 | 1 | SE1-SB-13 (RX-1 invalid CONTROL runs, caught by preregistered gate) |

Round 2 (review 5060124249): SB-08 was re-bucketed production-runtime → internal/seam to match the
#227 taxonomy (bucket correction only; population total unchanged), and SB-10 left the established
population as an OUT-OF-SE1 unconfirmed candidate — an untracked E4 record with UNKNOWN status is
retained, but retention is not establishment.

Dedup discipline (mechanically enforced since SE-1-CORRECTIVE-1): every induced population case
carries a unique concrete `root_cause_key` (e.g. `timed-admission-register-before-timer-arming-alloc`,
not merely "failure-atomicity"); duplicates fail the validator. `group.hpp` and the deadline-heap
defect share one root-cause CLASS (growth-defeating `reserve(size+1)`) and are cross-linked via
`related_entries`; R2-ALLOC and FE P1-1 share the failure-atomicity class; SB-11/SB-12 share
authority duplication. No bug, review finding, and regression test were counted as three hazards
anywhere.

## 4. Root-cause map (Sluice-induced population cases)

| Root-cause key | Class | Entry |
| --- | --- | --- |
| `scheduler-queue-inline-success-skips-opposite-role-reconcile` | liveness-reconciliation | SB-01 (Q-LIV-1) |
| `timed-admission-register-before-timer-arming-alloc` | failure-atomicity | SB-02 (R2-ALLOC) |
| `deferred-publication-transit-insert-may-throw-after-terminal-commit` | failure-atomicity | SB-05 |
| `deferred-queue-op-teardown-window-unpinned` | lifetime | SB-06 |
| `rwlock-owner-check-outside-serialized-ladder-section` | synchronization | SB-07 |
| `wait-resume-valid-kind-null-target` | identity-representation | SB-08 |
| `internal-test-seam-coordination-unsynchronized` | test-seam-synchronization | SB-09 |

OUT-OF-SE1 induced records (SB-03/SB-04, SB-11/SB-12, SB-13, and the SB-10 unconfirmed candidate)
stay outside the denominator; their classes are described in §3. SB-10 retains its suspect key
`select-event-registry-termination-vs-plain-wait-residency-unconfirmed` purely as the SE-2
graduation identity — it names the suspect instance, not a confirmed cause, and is not a population
case.

## 5. Comparability (population cases only)

| Validity | Count | Entries |
| --- | ---: | --- |
| FAIR | 11 | H02, H04, H05, H06, H08, H09, H10 (+ induced SB-01, SB-06, SB-07, SB-09) |
| PARTIAL | 8 | H01, H03, H07, H11, H13 (+ induced SB-02, SB-05, SB-08) |
| COMPARABILITY_BLOCKED | 1 | H12 (overall; policy half PARTIAL, kernel-mechanism half below the Sluice boundary) |

Pairing outcomes over the conventional population: PAIR-A ×4, PAIR-B ×1, PAIR-C ×4, PAIR-D ×2,
PAIR-E ×1, PAIR-X ×1. Every Sluice-induced IN-SE1 entry is by definition a PAIR-F instance
(Sluice introduced an adjacent protocol hazard); SE-2 will measure which layers kill them. Counts
are descriptive only — **no score is computed and none may be derived.**

## 6. Current Sluice outcomes (descriptive, IN-SE1 population cases only)

| Status | Count |
| --- | ---: |
| UNREPRESENTABLE | 4 |
| STATICALLY_REJECTED | 2 |
| DYNAMICALLY_DETECTED | 4 |
| FAIL_FAST | 4 |
| DETERMINISTICALLY_REPRODUCIBLE | 4 |
| SILENT_OR_UNDETECTED | 1 |
| NOT_APPLICABLE | 1 |

Total 20 population cases. UNKNOWN is no longer represented in the population: the former UNKNOWN
case (SB-10) is an OUT-OF-SE1 unconfirmed candidate since round 2 (review 5060124249 FIX 3).

## 7. Silent / unknown Sluice cases (retained, priority SE-2 probes)

1. **SE1-CA-H01-1 (H01):** a caller destroying the borrowed buffer while a request is in flight is
   not detected by Sluice; the obligation is explicit in the contract, the violation is not
   mechanically caught (detection is delegated to external tooling such as ASan). The H01 probe
   companion (SE1-CA-H01-2) is the deterministic SE-2 probe skeleton for exactly this case and
   attributes its result to the parent population case.
2. **SE1-SB-10 (H09, test-only):** one unconfirmed TSan hang in `select_event_registry_test`; record
   lives only in untracked human artifacts (E4) — 20/20 clean reruns. Kept as UNKNOWN; also a
   documentation-gap finding. Its `root_cause_key` names the suspect instance, not a confirmed cause.
   Since round 2 (review 5060124249 FIX 3) it is an **OUT-OF-SE1 UNCONFIRMED CANDIDATE, not a
   population case**: E4-only evidence must not claim repo-primary/S0 establishment. SE-2 graduation
   law: reproduce it → it enters the corpus population as a formal entry with root-cause
   classification; never reproduce it → it is explicitly dismissed.

## 8. CORPUS BIAS AUDIT (adversarial pass over the first draft)

| Question | Finding | Correction / status |
| --- | --- | --- |
| A. Disproportionately hazards Sluice handles well? | Initial pairing source (project S1 evidence) risks over-selecting well-handled families. | Kept the worst-looking cases: H01 SILENT pair, H12 COMPARABILITY_BLOCKED, and the full induced half (6 production hazards Sluice itself created). No deletion-based correction needed after re-check. |
| B. Which families make Sluice look worst? | H01 (silent borrow violation), H12 (hazard below the boundary), SB-10 (UNKNOWN liveness), plus the induced half itself. | All retained and surfaced in §7. |
| C. Families only by synthetic conventional cases? | H01's second entry is synthetic (labeled CONVENTIONAL-MINIMAL). | Family anchor is C1 documentation; the C3 entry is now formally a probe companion excluded from the denominator (SE-1-CORRECTIVE-1). |
| D. Same severity discipline for induced hazards? | Yes — induced population entries require repo-primary S0 repository evidence; the one unconfirmed case is neither dropped nor upgraded: it is an OUT-OF-SE1 UNCONFIRMED CANDIDATE (round 2), retained as an SE-2 priority probe. | Noted: two of the OUT adjudications (SB-03/SB-04) would have inflated the "Sluice bugs" side if counted; they are excluded for the same reason a mere crash would be excluded from H10 — category discipline, not favoritism. |
| E. Bug instances vs semantic classes? | Normalized traces are the unit; root-cause keys now dedupe at the concrete-instance level (validator-enforced). | Confirmed. |
| F. Structural risks counted as runtime failures? | No — SB-11/SB-12 are OUT-OF-SE1 with COMPARABILITY_BLOCKED; only concrete drift bugs (SB-01) carry runtime entries. | Confirmed. |
| G. Inconvenient silent cases excluded? | No. | §7 exists precisely for them. |
| H. Comparable abstraction levels? | Conventional C0 cases are runtime-internal (kernel) while some C1 cases are contract-level; Sluice-induced are runtime-internal. Family-level comparison is like-for-like; instance-level severity is deliberately not compared. | Recorded as residual abstraction caveat. H03 downgraded to PARTIAL on exactly this discipline. |

**BIAS FOUND:** residual selection familiarity (Sluice half drawn from well-documented project history;
conventional half required fresh research) and zero C2-grade conventional sources this round.
**CORRECTIONS:** silent/unknown/blocked cases retained; OUT adjudication applied symmetrically;
rejected-candidate log added to the JSON; SE-1-CORRECTIVE-1 answered the human review's directed
probes (H08 upgraded to a verified C0; H03 comparability tightened rather than defended).
**UNRESOLVED BIAS:** none actionable within SE-1; human adversarial review should specifically probe
H07/H13 (C1-anchored families) for stronger C0 sources, and check that no real conventional case
was missed for H12's blocked comparison.

## 9. What SE-1 does NOT prove

- Not that Sluice migrates hazards better than POSIX/liburing/Asio (T-S1b untested).
- Not that Sluice has positive net safety value (T-S2 untested).
- Not that the existing witness stack (TLA+/GenMC/DST/mutation/death tests) would detect every corpus
  entry — that is exactly the SE-2 matrix.
- Not that the two SILENT/UNKNOWN cases are the only ones; they are the ones found in a bounded pass.
- Not that source roles prove URL contents: the validator enforces provenance SHAPE only.

## 10. SE-2 readiness

- Schema frozen (`se1-corpus-schema` v1 as corrected in Draft, including the round-2 corrections of
  review 5060124249: C0 primary-authority rule, origin↔quality closed relation, `repo-untracked`
  authority, SB-08 bucket, SB-10 candidate status; fail-closed validator PASS, wired into
  `scripts/gates/pre-push.sh` and CI).
- Every entry carries `se2_probe_candidates`; existing detection evidence is recorded but the
  detection matrix is NOT built here.
- The corpus can be consumed without redesign: families, buckets, eligibility, entry roles, source
  roles, pairing vocabulary, and status enums are closed and mechanically checked. SE-2 probes may
  attach to the H01 companion but must attribute results to the parent population case.

## 11. Production / API / roadmap impact

- Production code changed: **NO**.
- Public API changed: **NO**.
- Roadmap (#221/#225/#226/#227) updated: **NO**.
- Known OPEN production-side defect `group.hpp` reserve idiom: left untouched by explicit adjudication
  (SE1-SB-04, OUT-OF-SE1).
