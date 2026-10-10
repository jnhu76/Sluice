# Phase M closeout — wave 2 (remaining-family disposition and gate recompute)

- **Status**: EVIDENCE_RECORD — PENDING_OWNER_DECISION on the registered
  decision set (§5). This wave changes no production code, no root text, no
  ADR, no F1 frozen manifest, and no ledger row. It supersedes nothing
  silently: the 2026-10-08 closeout snapshot
  ([`m-phase-final-closeout.md`](m-phase-final-closeout.md) /
  `.json`, merge `ffb5afa8`) is retained unmodified as the original time
  cross-section; this file is the wave-2 layer above it.
- **BASE_SHA** (live master at wave start, re-fetched): `94b5cfcd`
- **Wave-2 branch code face**: identical to `94b5cfcd` for src/include/tests
  (docs-only additions).
- **Authority order**: root v1-r3 > adopted ADRs > #402 current text >
  F0/F1 adopted artifacts > #458 (reset + §8) > ledger/tests > live code.

## 0. What changed between the two closeout snapshots

Adopted/merged since `ffb5afa8`: ADR-0003 **adopted** DESIGN_AUTHORITY_ONLY
(PR #476 `6df45c0d`; #458 M-H `REPLACEMENT_ADOPTED=PASS` backfilled); #460
and #475 **closed** as decision tickets (#475 policy: A-family=BOUNDED_HOLD,
B/C=KEEP_INSTALLED_COMPAT; POLICY_DECIDED ≠ HEADERS_CONTRACTED); M-R
projection round-2 (#472 `b0b0e391`); e1 COPY-B oracle (#477 `b0f55285`);
e4 TAIL oracles (#478 `e44a8707`); e1 settlement (#481 `31dc948e`, #484
`51eaa8b7`, #485 `fc51b9e5`, ledger #486 `6000745e`; final master
`94b5cfcd`); e2 four-README PR #479 @ `901acfca` open with a Gate-0
independent-review record (ACCURATE_AGAINST_LIVE_MASTER, no corrective
needed; merge authorization = owner's).

## 1. Wave-2 disposition inputs (the Gate-1 deliverables)

| family | carrier doc (this wave) | decision requested |
|---|---|---|
| M-H four apps | [`mh-four-app-disposition.md`](mh-four-app-disposition.md) | D-H1 GREP separator, D-H2 param-boundary layering, D-H3 COPY-B waiter-level precedence, D-H4 TAIL settlement visibility, D-H5 adopt the per-app KEEP |
| M-F | [`mf-vectored-second-authority-disposition.md`](mf-vectored-second-authority-disposition.md) | ruling A (A1/A2/A3×{a,b}/A4; candidate A3), ruling B (B1/B2/B3; candidate B1); U-03/U-04 separately to F5 |
| M-A1 | [`ma1-blocking-io-pool-disposition.md`](ma1-blocking-io-pool-disposition.md) | PROD-03 admittance (P1/P2/P3; candidate P3 isolation, no deletion) |

All three are DISPOSITION_INPUT_COMPLETE with per-item evidence, options,
counterexamples and owners; none is self-adopted.

## 2. Gate 2 — dependency families

### M-A2 (wal/copy/buffer/observed + L-06..L-09) — D-12 conditional record

Live state at `94b5cfcd` (verified this wave): the entire family is
**intra-legacy-only** — `wal.hpp` included only by `src/wal.cpp`;
`copy_all`'s only callers are `Reader::stream_to` overloads whose own callers
number zero; `Observed*` referenced only by `src/observed.cpp`; the
buffer/copy_strategy/sync/iovec faces are included only inside the legacy
stream world itself. apps 0 / tests 0 / external unknown (installed via the
`sluice/*.hpp` glob in both manifests).

- **If M-F = A3+B1** (candidate): no v1 workload admits this family either;
  it retires **with** the L-01 family in the same F4 batch (A3a sub-option:
  wal retires unconsumed — no retype owed; accepted-degradation record covers
  the writev single-syscall property).
- **If M-F = A1/A2** (canonical vectored): wal becomes the accepted-workload
  candidate and D-12 must then admit wal and whichever spelling it needs;
  copy/buffer/observed remain unadmitted.
- **If M-F = A4/B2** (compat isolation): the family inherits the compat
  isolation + sunset with the L-01 batch; D-12 records their sunset jointly.

Recorded as PENDING the M-F ruling — exactly the binding #458 §8 defines.
No carrier issue needed until M-F rules.

### M-A3 (test doubles: `fault.hpp`, `memory_io_context.hpp`) — D-13 record

Both headers are F1 frozen installed surface (79-header set). Options with
consequences: (1) **KEEP installed, labeled TEST_ONLY** (status quo; zero
B-01 churn; matches F0 D-13's HIGH-confidence KEEP/TEST_ONLY reading);
(2) relocate to a test-only location / (3) internalize — both are package
changes requiring the B-01 same-slice re-freeze (both profiles, clean-room
probes) and F5 alignment; (4) retire with the L-01 family if M-F retires it
(its `IoContext` interface belongs to the legacy stream world).
**Recommendation (PENDING_OWNER): (1) for the v1 window, revisit at F5
package alignment; M-A3's gate `PACKAGE_DECISION_RECORDED` is satisfied as
soon as the owner adopts this record or picks another option.** No physical
change in M.

### M-A4 (experimental uring write face) — no action

#463 is CLOSED with the prior isolation verdict: the face is absent from
install globs and both manifests; F1's
`NEGATIVE_EXPERIMENTAL_HEADERS_NOT_INSTALLED` gate passed in both profiles.
This wave adds no action and reopens nothing; any installed-header set change
re-triggers B-01 (now owed anyway, see §4).

## 3. Seven-family gate recompute

Fields per the Phase-M closeout contract:
`DISPOSITION | AUTHORITY | CONSUMERS | PRESERVED_BEHAVIOR | EVIDENCE | OWNER | EXIT_TRIGGER | DOWNSTREAM_F_GATE`.

| family | gate & value | DISPOSITION | AUTHORITY | CONSUMERS | PRESERVED_BEHAVIOR | EVIDENCE | OWNER | EXIT_TRIGGER | DOWNSTREAM_F_GATE |
|---|---|---|---|---|---|---|---|---|---|
| **M-R** | `RETAINED_CONSUMERS_MIGRATED_OR_EXPLICITLY_ISOLATED` = **PASS** (unchanged) | tier-1 migrated + tier-2 isolated (75/75 rows, 82 subscopes, 0 unexplained; validator in CI) | #458 §3.1/§6/§7 (re-anchored by #472) | as recorded per row in `m-r-consumer-disposition-final.json` | every POLICY_ISOLATED/COMPAT_TEST_RETAINED row carries reason, owner, exit | #468/#469/#471 merged; #472 round-2 + CI guard | F2 (+tier-2 per-unit owners) | F2 carrier retirement consumes it | F2 |
| **M-H** | `REPLACEMENT_ADOPTED` = **PASS** (backfilled #458); consumer-disposition arm = **INPUT_COMPLETE, PENDING_OWNER (D-H1..D-H5)** | four apps KEEP on compat substrate (candidate) | ADR-0003 §8 COMPATIBILITY_ONLY + MIG-02 + #458 reset | four apps; 8 census rows under T2-08 | app CLI contracts, pipeline/atomic/follow semantics (OB-1/2/3) | e1 #477, e4 #478, e2 #479 + Gate-0 record, 53/53 suite | F3 (F2 shares T2-08) | owner adopts D-H5; D2/W-04 verification is a **separate support gate, still NO** | F3 |
| **M-F** | `VECTORED+SECOND-AUTHORITY_DECIDED` = **BLOCKED_OWNER_DECISION** (was BLOCKED_AUTHORITY_DECISION; inputs now complete) | candidate A3+B1 (see M-F doc); U-03/U-04 separate | PROD-03 root:127/:129, MIG-02 :933, F0 D-09/D-10/D-12 | legacy stream world: zero live in-tree reachability; 1 test file (down from 6, M-R delta); external unknown | installed headers/bytes/symbols unchanged until an authorized batch | M-F doc §0-§6 (this wave, all anchors live) | F4 (+F5 for U-03/U-04) | owner rules A×B; U-03/U-04 human decision precedes any deletion | F4 |
| **M-A1** | `PROD-03_ADMITTANCE_DECIDED` = **BLOCKED_OWNER_DECISION** (inputs complete) | candidate P3: not admitted; compat isolation + sunset; no deletion | PROD-03; F0 D-11; policy §1.7/§1.8 | in-tree 0 (re-verified); external unknown (U-row requested) | installed headers/symbols/archive members unchanged | M-A1 doc (this wave) + history 1918701e/c83a0acd/5f62b55b | F4, F5 | owner ruling; external-use decision precedes any removal batch | F4, F5 |
| **M-A2** | `PROD-03_ADMITTANCE_DECIDED` (bound) = **BLOCKED_UPSTREAM** (conditional record now on file) | rides M-F ×3 branches (§2) | #458 §3.5; F0 D-12 | intra-legacy only (verified) | unchanged until M-F ruling executes | §2 of this doc | F4 | M-F ruling → D-12 taken in the same ruling | F4 |
| **M-A3** | `PACKAGE_DECISION_RECORDED` = **READY_FOR_OWNER** (record on file) | candidate KEEP installed TEST_ONLY | F0 D-13; #458 §3.6 | test doubles only (no production consumer) | installed, frozen, unchanged | §2 of this doc + manifests | F4/F5 | owner adopts the record (or picks relocate/internalize with B-01) | F4, F5 |
| **M-A4** | none (isolated by F1) = **CLOSED_NO_ACTION** | prior verdict stands | #463 CLOSED | none installed | negative-manifest gates | #463 record; manifests unchanged | none | any install-set change re-triggers B-01 | F5 |

## 4. New finding this wave — B-01 re-freeze owed on master

**F-W2-1 (blocker for any package-claiming closeout):** the e1 settlement
changed **installed production surface** without a same-slice B-01
re-freeze. Evidence: `git diff a34d96c6..94b5cfcd -- src include` =
`include/sluice/async/threadpool_backend.hpp` (+5),
`src/async/threadpool_backend.cpp` (+41/−5),
`src/async/threadpool_test_seams.hpp` (+10). `threadpool_backend.hpp` is one
of the 79 frozen installed headers (`f1-package-manifest-*.json:74`), and the
manifests' `PRODUCTION_BASELINE_SHA` remains `a34d96c6` (last manifest
commit `6404e07b`). Empirically confirmed this wave by running the verifier:
`--check-frozen` on `94b5cfcd` reports
`[FAIL] F1_B_PROVENANCE_VERIFIED (… production diff=['threadpool_backend.hpp',
'threadpool_backend.cpp', 'threadpool_test_seams.hpp'] …)` — every other F1
gate passes (all P1 contracts, negatives, clean-room). The F1 CI job only
self-tests the verifier, so nothing red flagged this. The defect fixes
themselves were
owner-authorized (#481/#484 merged after review) — what is missing is the
package obligation, not the fix.

Remedy options (owner): (a) authorize a re-freeze slice — run the freeze
protocol in both profiles, land versioned manifests + archive baselines as a
new dated artifact set (the protocol re-freeze precedent is `6404e07b`);
(b) record an explicit B-01 policy scoping decision (e.g. provenance gate
re-freeze deferred to the next package-touching slice) as a root/GOV-level
note. Until one lands, no claim of the form "both frozen F1 profiles
reproduce at HEAD" is true of `94b5cfcd` — wave-2 documents carry the older
reproductions as history and this record corrects the standing claim.

## 5. PENDING_OWNER_DECISION registry (the complete decision set)

| id | question | carrier | recommendation on file |
|---|---|---|---|
| D-H1 | GREP multi-file no-`-n` separator defect vs intended | #474 + M-H doc §3 | treat as defect, minimal fix |
| D-H2 | param-range layering + HASH/GREP diagnostics | #474 + M-H doc §3 | keep exit codes, fix diagnostics |
| D-H3 | COPY-B waiter-level error precedence contract | #474 + M-H doc §3 | bind decision to F3 retype slice |
| D-H4 | TAIL settlement-failure visibility surface | #474 + M-H doc §3 | product decision, owner F3 |
| D-H5 | adopt four-app KEEP disposition | #474 + M-H doc §1 | KEEP on compat substrate |
| D3a | vectored ruling (A1/A2/A3a/A3b/A4) | #461 + M-F doc §4 | A3 (a-preferred) |
| D3b | second-authority ruling (B1/B2/B3) | #461 + M-F doc §4 | B1 |
| D-12 | wal/copy/buffer/observed admittance | #458 + this doc §2 | rides D3a/D3b |
| D-13 | test-double package landing | #458 + this doc §2 | KEEP installed TEST_ONLY |
| D4 | BlockingIoPool PROD-03 | #462 + M-A1 doc §2 | P3 isolation, no deletion |
| U-03/U-04 | file.hpp/io_context.hpp external-use policy | F5 path, M-F doc §5 | no agent default |
| F-W2-1 | B-01 re-freeze vs policy scoping | this doc §4 | re-freeze slice |

## 6. Closeout checks (each verified this wave)

1. No unexplained UNMIGRATED consumer: M-R overlay 75/75 dispositioned, 0
   unexplained, CI-guarded; M-F live delta (6→1 test files) recorded with its
   cause (M-R tier-1); M-A1 zero-consumer drift recorded with history.
2. Every COMPAT_TEST_RETAINED (4) and POLICY_ISOLATED (35) row carries its
   reason in the overlay; the twelve tier-2 units carry authority, owner,
   exit condition.
3. No deprecated/installed exposure is claimed as supported-v1: ADR-0003 §8
   COMPATIBILITY_ONLY; #475 POLICY_DECIDED ≠ HEADERS_CONTRACTED; this wave
   adds no support claim anywhere.
4. No test, consumer or source was deleted to improve counts (this wave
   touches only new docs files).
5. Every isolated-but-unmigrated surface has owner + exit: §3 table +
   per-family docs.
6. #402 F2/F3/F4/F5 preconditions remain intact: F4_PRODUCTION_RETIREMENT
   still unauthorized; nothing retired.
7. D2/W-04 obligations are NOT restated as OPTIONAL_SUPPORTED: ADR-0003
   inequality chain (`:58-63`) reaffirmed; D2 verified = NO.
8. Ledger/README/formal-decision consistency: the e1 settlement is ledgered
   (PROG row, `94b5cfcd`); READMEs (#479) match live behavior; no
   contradiction found. The one standing inconsistency found is F-W2-1
   (documented, not papered over).
9. Historical preservation: `m-phase-final-closeout.{md,json}` untouched;
   this wave is a new dated layer with exact SHAs.

## 7. Goal states (wave 2)

```text
PHASE_M                        = NO  (decision set complete and routed; owner rulings outstanding)
M_FAMILY_DISPOSITIONS          = COMPLETE (all seven; six with owner+exit; M-A4 none owed)
M_DECISIONS_COMPLETE           = NO   (12-item registry §5 outstanding)
M_EXECUTED_SCOPE_EVIDENCE      = PASS (docs-only wave: 53/53 noliburing release re-run,
                                       Gate-0 CLI re-verification, F1 provenance check run,
                                       all anchors re-verified at 94b5cfcd)
F1_HISTORICAL_BASELINE         = REPRODUCIBLE_AT_a34d96c6_ONLY (F-W2-1: re-freeze owed at HEAD)
F2/F3/F4_PRODUCTION_RETIREMENT = NOT_EXECUTED
M_FINAL_INDEPENDENT_REVIEW     = see wave-2 PR record
```

Phase M formally completes when the §5 registry is ruled by the owner and
the #458 §8 rows are backfilled; the F-W2-1 re-freeze (or scoping decision)
belongs to that same authorization wave because several family gates quote
frozen-package evidence.
