# SE-2 Detection Matrix — Interpretation

Authority: `docs/results/safety/se2-detection-matrix.json` (se2-detection-matrix-schema
v1). This document is generated/interpreted FROM that JSON; it adds no cells and
no claims of its own. Campaign: SE-2 over the frozen SE-1 population (20 rows =
13 conventional + 7 induced; 1 probe companion excluded from all denominators;
6 OUT-OF-SE1 records untouched). Execution date 2026-08-30, base
`b453d85cd865b9429e48116f1ad463a5d3ecad6a`.

All counts below are DESCRIPTIVE ONLY. No safety score exists, is computed, or
is implied anywhere in SE-2.

## Matrix summary

- 20 rows × 11 layers (L0–L10), Sluice side on every row; the 13
  conventional-origin rows additionally carry a conventional side (363 cells).
- Status vocabulary and layer definitions are frozen in the JSON preamble.
- Fresh execution this campaign: 8 new probe binaries (plain + ASan+UBSan +
  TSan passes), 15 focused C++ tests (debug), 2 TSan runs, 3 real-liburing
  uring tests, 4 negative-compile gates, 8 TLA+ suites, GenMC kernel runner
  self-test, DST driver, and the pre-registered SB-10 exploratory campaign
  (TSan ×10 + plain ×100).

## Summary table (per-family view; detail lives in the JSON)

| ID | Hazard | Conv baseline | L0 | L1 | L2 | L3 | ASan | TSan | TLA+ | GenMC | DST | Real backend | Migration |
|----|--------|---------------|----|----|----|----|------|------|------|-------|-----|--------------|-----------|
| H01 | borrow lifetime | MISSES | M | M | M | M | D | n/a | n/m | n/m | n/m | REPRODUCES | M1 |
| H02 | identity reuse | MISSES | P | R | P | D | n/a | n/a | D | n/m | n/m | D | M4 |
| H03 | completion vs close | MISSES | M | M | D | D | n/a | n/a | n/m | n/m | n/m | M | M1 |
| H04 | cancel vs complete | MISSES | P | n/a | P | D | n/a | n/a | D | n/m | REPR | n/t | M5 |
| H05 | double terminal | PREVENTS (kernel) | P | R | P | D | n/a | n/a | D | D | n/m | n/t | M5 |
| H06 | submit rollback | PREVENTS (glibc shape) | P | n/a | P | D | n/a | n/a | n/m | n/m | n/m | D | M3 |
| H07 | partial/zero progress | MISSES | P | n/a | P | D | n/a | n/a | n/m | n/m | n/m | n/t | M3 |
| H08 | deadline vs completion | PREVENTS (kernel) | P | n/a | P | D | n/a | n/a | D | n/m | REPR | n/t | M5 |
| H09 | lost wake | MISSES | M | n/a | D | D | n/a | n/a | D | n/m | REPR | n/t | M2 |
| H10 | shutdown w/ in-flight | MISSES | P | n/a | D | D | n/a | n/a | D | n/m | n/m | D | M3 |
| H11 | accounting leak | PREVENTS (kernel) | P | n/a | P | D | n/a | n/a | D | n/m | n/m | n/t | M3 |
| H12 | durability (fsyncgate) | MISSES | P* | n/a | P | D | n/a | n/a | n/m | n/m | n/m | BLOCKED | MX |
| H13 | weak-memory publication | MISSES | P | n/a | P | D | n/a | D (conv) | n/m | D | n/m | n/t | M5 |

Legend: P=PREVENTS, R=REJECTS, D=DETECTS, REPR=REPRODUCES, M=MISSES, n/a=NOT_APPLICABLE,
n/m=NOT_MODELED, n/t=NOT_TESTED. Sluice-side cells shown; conventional side in the
pair table below. H12 L0 carries the policy-half/kernel-half split (P* = policy
half only; kernel half MISSES and is recorded at L9/L10).

Every positive cell above has an evidence reference in the JSON (validator
`scripts/verify-se2-detection-matrix.py` enforces this mechanically).

## Conventional vs Sluice pair table (main T-S1b evidence)

| Family | Conventional failure | Conventional enforcement | Sluice failure | Sluice enforcement | Comparison | Migration |
|--------|---------------------|--------------------------|----------------|--------------------|------------|-----------|
| H01 | silent UAF into freed buffer (P-C01) | docs only; ASan if enabled | silent UAF; success published (P-S01) | explicit borrow contract; NO mechanical layer; ASan external | PARTIAL — both sides silent at API layer; ASan detects both | M1 |
| H02 | 2 CQEs under one user_data; misattribution (P-C02) | docs only | stale identity not constructible | negative-compile gates + generation protocol + TLA EXACT | Sluice statically/protocol-rejects | M4 |
| H03 | fd-number reuse -> read hits wrong file (P-C03) | none (kernel fixed only its internal UAF) | backend-state fail-fast; fd-reuse window MISSES (below boundary) | teardown death tests; typed EBADF; §9.1 descriptor rule | PARTIAL — Sluice better on backend state, SAME on fd identity | M1 |
| H04 | caller double-terminal reconciliation (not executed; documented) | docs only | second terminal unwritable | resolve_ CAS + e10 EXACT + DST T4 | Sluice protocol-prevents | M5 |
| H05 | kernel duplicate CQE (fixed upstream; BLOCKED) | kernel mechanism today | double publication unwritable | reap-only publication + GenMC controls + negative-compile | Sluice protocol-prevents; kernel mechanism-equivalent | M5 |
| H06 | glibc listio atomic at invalid-entry shape (P-C06); completion failures docs-only | glibc PREVENTS that shape | transactional submission ladder; post-terminal fail-fast | R2-ALLOC witnesses + PUB death tests + D2 (historical) | CONVENTIONAL STRONGER at that shape; Sluice M3 on the post-terminal shape | M3 |
| H07 | wrong-offset retry duplicates block (P-C07) | docs only | silent variant unrepresentable; zero-progress typed failure | helpers + typed Result | Sluice typed-detects | M3 |
| H08 | linked-timeout double free (fixed upstream; BLOCKED) | kernel mechanism today | grant/expiry cannot both win | AC-2b authority + e11 EXACT + DST T4 | Sluice protocol-prevents; kernel mechanism-equivalent | M5 |
| H09 | lost wake reproduced; TSan blind (P-C09) | docs only | repaired law; DST-replayable; teardown fail-fast | e9 EXACT (historically found real gaps) + DST T5 | Sluice replay+detect; conventional silent | M2 |
| H10 | abandonment documented (libuv/Asio); not executable on host | docs only | destruction-with-live-work named fail-fast | teardown death tests (real liburing) + e16 | Sluice fail-fast | M3 |
| H11 | kernel leak paths (fixed upstream; BLOCKED) | kernel mechanism today | exactly-once accounting at reap; counters inspectable | arena tests + AC-1a + AC-2c census | Sluice prevent+observe | M3 |
| H12 | kernel error-clearing fools any user-space retry | none below boundary | policy half: typed sync failures, strict durable marker | WAL/durability tests | kernel half COMPARABILITY_BLOCKED both sides | MX |
| H13 | SB litmus: 4888/200000 silent; TSan detects race (P-C13) | docs only; TSan as tool | relaxed publication unrepresentable at arm() | GenMC EXACT + controls | Sluice protocol-prevents; TSan-equivalent detection | M5 |

## Migration classification (§32; qualitative ordering only, no numbers)

- M0 (no migration): none.
- M1 (observability only): H01 (borrow contract explicit, failure silent), H03
  (quiescence contract explicit; fd-reuse half unchanged).
- M2 (deterministic replay): H09 (DST replay class; with M3 evidence).
- M3 (early dynamic detection): H06, H07, H10, H11.
- M4 (static rejection): H02 (negative-compile; L0 protocol unrepresentability
  also holds).
- M5 (unrepresentable): H04, H05, H08, H13.
- MX (comparison blocked): H12 (kernel mechanism half below all user-space
  boundaries; policy half tested).

## Important misses (kept, not rescued)

1. **H01 borrow lifetime — Sluice itself is blind (REQUIRED §18 experiment).**
   P-S01: submit accepted, no fail-fast, success published into a freed borrow;
   20/20 plain runs silent; only ASan detects. Sluice makes the obligation
   explicit but enforces nothing. No production lifetime machinery was added.
2. **H03 fd-number-reuse window — below the Sluice boundary.** Between accept
   and syscall execution the fd number can be recycled; the real backend then
   targets the wrong resource, identically to raw io_uring. Sluice's fail-fast
   covers backend state, not fd identity.
3. **H12 kernel error-clearing — below every user-space layer.** Policy half
   tested and typed; the kernel mechanism half is BLOCKED and honestly recorded.
4. **Real-backend fault-injection coverage is the weakest layer** — L9 cells
   for H04/H05/H08/H09/SB-02/SB-05 are NOT_TESTED this campaign; standing
   real-liburing records (D2/D4) cover adjacent shapes only.
5. **TSan is blind to logic lost wakes** (P-C09: 3/3 clean TSan runs with the
   hang reproduced) — a layer-fingerprint fact that bounds what TSan greens mean.

## Conventional stronger / equal cases (§37; recorded, not euphemized)

- **H06, glibc listio initiation is atomic** at the invalid-entry shape:
  batch-level -1/EIO with zero execution — mechanism-equivalent to Sluice's
  transactional submission for that shape (measured, P-C06).
- **H05/H08/H11: current upstream kernels PREVENT** the historical defects by
  mechanism. The comparison target for those families is protocol-hardening vs
  kernel-mechanism, not "conventional is silent".
- **H13: TSan detects** the conventional race class as well as the GenMC layer
  detects the modeled one — different cost model, comparable detection.

## Induced hazards (§44; separate table so they cannot vanish)

| ID | Bucket | Root cause | Original bad outcome | Original detector | Current posture | Layers that miss | Thesis implication |
|----|--------|-----------|----------------------|-------------------|-----------------|------------------|--------------------|
| SB-01 | production-runtime | inline-commit missing opposite-role reconcile (4 sites) | waiter stranding (hang) | human adversarial review (AC-2d) + DST witness | repaired, regression-pinned, DST-replayable | ASan/TSan/GenMC/stress all blind | explicitness aided replay/localization; the 4-site obligation WAS the defect surface |
| SB-02 | production-runtime | register-before-prepare ladder order | live accounting residue on OOM | human adversarial review round 2 | prepare-before-register, strong guarantee, pinned | ASan/TSan/tests-without-fault-injection | mixed: ordinary ladder error, but explicit accounting made it describable |
| SB-05 | production-runtime | allocation on post-terminal publication path | stranded/double publication risk | FE adversarial review + death/mutation campaign | noexcept tail + named fail-fast, C1–C4 pinned | ASan/TSan blind | transit vocabulary made failure nameable |
| SB-06 | production-runtime | teardown through in-flight window (no pin) | use-after-teardown / lost result | FE-CORRECTIVE-1 review | pin transfer closes window; QD1 fail-fast | sanitizer-blind window class | pin obligation = new protocol surface AND the fix |
| SB-07 | production-runtime | owner check outside serialized section | torn owner state / wrong admission | **TSan** | check inside single admission text; TSan-green | L6 abstractions could not see placement | explicit owner law made placement a checkable rule |
| SB-08 | internal/seam | nullable resume-target encoding | lost wake in representation | adversarial representation audit | static_assert + normalization (compile-time) | sanitizers/tests blind while unexercised | explicit encoding made the invalid state compile-rejectable |
| SB-09 | test-only | seam's own coordination raced | flaky verification (test process) | TSan (#229) | repaired; seam excluded from production (mechanically gated) | ASan blind; tests flake without attribution | verification apparatus is itself protocol surface |

Bucket discipline (§17): 5 production-runtime + 1 internal/seam + 1 test-only.
Never report "7 Sluice bugs" without this split.

## SB-10 exploratory result (OUT-OF-SE1; §21)

Pre-registered campaign executed: TSan ×10 + plain ×100 runs of
`select_event_registry_test`, 60 s per-run timeout. Result: 0 hangs, 0 race
reports. Outcome **B — STILL UNCONFIRMED**. Promotion recommended: **NO**. It
remains outside the denominator and outside any claim.

## Adversarial miss audit (§48)

- A. Completely missed by Sluice: H01 borrow-destroy; H03 fd-reuse window; H12
  kernel half; H07 caller-side count-ignoring misuse.
- B. "PREVENTS" resting on more than executed witnesses: H11 L0 (unretired
  terminal unrepresentable) rests on design text + the standing AC-2c census,
  not a per-path executable negative witness — flagged.
- C. "Static" claims that are actually test-enforced: SB-09 seam discipline
  (mechanical gate + TSan, not compiler); H06 ladder ordering (runtime
  witnesses, not compile-time). The negative-compile claims (H02, H05) are
  genuinely compiler-enforced.
- D. Adjacent-only formal cells: H06/SB-02 L6 (fault shapes beyond model
  scope), SB-05 L7 (ordering kernels adjacent to the allocation shape), SB-01
  L6 coverage gap (recorded per §17).
- E. DST cells that reproduce without detecting: H04/H08 L8 (precedence
  transposition — labeled REPRODUCES, correctly not DETECTS).
- F. Sanitizers missing known mutants: TSan blind to logic lost wake (P-C09);
  ASan blind to kernel-side writes (P-C01) and to accounting classes (SB-02).
- G. Real vs fake backend: fresh real-liburing runs (identity, destruction,
  contract) all PASS — no divergence observed; but real-backend
  fault-injection remains the under-covered layer (see misses #4).
- H. Conventional already equal/better: H06 glibc initiation shape; H05/H08/H11
  kernel mechanisms; H13 TSan-as-tool — recorded in the §37 section.
- I. Induced protocol complexity: yes — each repair added obligations (4-site
  reconcile, pins, noexcept transit, serialized placement). Recorded per row;
  not forced into a favorable story (§38).
- J. SE-1 `sluice_current_status` contradictions: none. SE-2 refined compressed
  labels (e.g., H03 FAIL_FAST holds for the backend-state half; the fd-reuse
  half MISSES was inside SE-1's status evidence and is now first-class).

## "Mechanically checkable" subclaim (§51)

**PARTIAL.** Multiple independent lifecycle hazard classes have
executable/model/static witnesses (identity, terminal winner, publication,
deadline arbitration, wake law, rollback, shutdown — negative-compile, death
tests, TLA+ EXACT, GenMC EXACT, DST). But an important lifecycle obligation —
borrow lifetime (H01) — remains documentation-only, and real-backend
fault-injection cells are mostly NOT_TESTED. Strong support exists; it is not
yet the whole boundary.

## T-S1b adjudication (§33)

**T-S1b PROVEN — SCOPE-BOUNDED.**

- Scope: the frozen 20-row SE-1 population; layers actually executed on this
  host; per-hazard scope only.
- Direct support: multiple independent hazard families (identity, winner,
  publication, timeout, wake, rollback, partial-I/O, shutdown, accounting,
  memory-publication — 10 classes) have FAIR/PARTIAL probes with direct,
  re-executed evidence of migration (M2–M5 above), including witnesses that
  historically found real defects (e9 models → PRs #190/#194; DST → Q-LIV-1;
  GenMC → #197 TSO discovery; TSan → SB-07/SB-09).
- Counterevidence preserved: H01 Sluice-blind case; H03 fd-reuse half; H12
  kernel half; conventional-stronger cases; 5 induced production-runtime
  hazards with honest discovery attribution (4 of 5 found by humans, not
  tooling).
- What remains unproven: any general or net claim beyond this population and
  these layers; real-backend fault coverage; borrow-lifetime enforcement.

## T-S2 net-safety ledger (§34)

**T-S2: NOT YET READY.**

- CONVENTIONAL MIGRATIONS: 12 of 13 families classified M1–M5 (1 MX); 10
  classes with direct executed witnesses.
- SLUICE REMAINS SILENT: H01 borrow-destroy; H03 fd-reuse half; H12 kernel
  half; H07 raw-surface misuse residual.
- SLUICE-INDUCED PRODUCTION HAZARDS: 5 (SB-01/02/05/06/07) — root causes above;
  discovered by human review ×4, TSan ×1; failure forms: hang, accounting
  residue, stranded delivery, use-after-teardown window, torn lock-owner state.
  All repaired and regression/mutation-pinned; discovery attribution is honest:
  most were NOT found by the matrix's own layers before human review.
- SLUICE-INDUCED INTERNAL/TEST HAZARDS: SB-08 (internal/seam), SB-09
  (test-only) — separate buckets, never user-visible production.
- NEW HAZARDS FOUND DURING SE-2: none in production code. (One probe-internal
  bug in P-C02 was found by ASan during bring-up and fixed inside the probe.)
- WHY NO AGGREGATE SCORE: hazard classes are not interchangeable units (a
  borrow UAF, a durability violation, and a test-seam race have no common
  utility scale); a separately preregistered aggregation/severity methodology
  does not exist. SE-2 stops here (§36).

## Claim implications

Claims now supported (scope-bounded): per §33 above.
Claims still forbidden: "Sluice is safer" in any net form; any count-based win
rate; any claim that H01/H03-half/H12-half are handled; any claim that real
backends are fault-covered; any SE-1 denominator change.
