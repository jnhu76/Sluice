# SE-2 HAZARD DETECTION MATRIX — HUMAN AUDIT ARTIFACT

Status: UNTRACKED human audit artifact (task §54). Not committed unless
repository authority requires it. Section `EXECUTION PLAN FREEZE` was written
BEFORE any new probe execution (task §45) and is not retro-edited.

# BASE / HEAD

- BASE (SE2_BASE_SHA): `1fd8a2fdf87eb52f35efd4a5c8ae76293b8dcfbd`
  (origin/master = PR #245 merge of SE-1 corpus at frozen head `0613779`).
  CORRECTION (AMENDMENT 4): the campaign originally recorded b453d85 as base,
  which carried docs-only commit 0afd9e5 (4 archived human review reports that
  were supposed to stay untracked) into the PR — review 5060477073 Blocker 2
  ordered the branch rebuilt on 1fd8a2f; the two SE-2 commits were re-based
  (new SHAs 5534497 probes, 75b6798 matrix), the four reports restored locally
  as untracked, byte-identical (sha256-verified). The code-relevant execution
  tree is identical, so no probe was re-run.
- Precondition: PR #245 == MERGED (mergeCommit `1fd8a2f`, mergedAt
  2026-08-30T07:44:07Z); `docs/results/safety/se1-hazard-corpus.json` present on
  origin/master; `python3 scripts/verify-se1-hazard-corpus.py` == PASS
  (27 records; 20 population = 13 conventional + 7 induced; 1 probe companion;
  6 OUT-OF-SE1).
- HEAD: branch `research/se2-hazard-detection-matrix`; commits listed in the
  Draft PR (matrix + probes + validator + docs; production code unchanged).

# ROW POPULATION

Derived mechanically from the frozen corpus with the population law
`corpus_eligibility == 'IN-SE1' AND entry_role == 'population-case'`:

- CONVENTIONAL (13): SE1-CA-H01-1, SE1-CA-H02-1, SE1-CA-H03-1, SE1-CA-H04-1,
  SE1-CA-H05-1, SE1-CA-H06-1, SE1-CA-H07-1, SE1-CA-H08-1, SE1-CA-H09-1,
  SE1-CA-H10-1, SE1-CA-H11-1, SE1-CA-H12-1, SE1-CA-H13-1.
- INDUCED (7): SE1-SB-01, SE1-SB-02, SE1-SB-05 (production-runtime),
  SE1-SB-07 (production-runtime), SE1-SB-08 (internal/seam), SE1-SB-09
  (test-only), and SE1-SB-06 (production-runtime).
  Bucket split: production-runtime 5 (SB-01/02/05/06/07), internal/seam 1
  (SB-08), test-only 1 (SB-09).
- Probe companion (NOT a denominator row): SE1-CA-H01-2.
- OUT-OF-SE1 (outside main matrix): SE1-SB-03, SE1-SB-04, SE1-SB-10
  (separate exploratory probe, task §21), SE1-SB-11, SE1-SB-12, SE1-SB-13.
- TOTAL denominator rows: 20. No additions, no replacements, no removals.

# EXECUTION PLAN FREEZE

Frozen before executing any new probe. Layers L0–L10 and the result vocabulary
PREVENTS / REJECTS / DETECTS / REPRODUCES / MISSES / NOT_APPLICABLE /
NOT_MODELED / NOT_TESTED / BLOCKED / UNKNOWN are taken verbatim from the SE-2
authorization (§7–§8) and are not extended.

Environment (single host, recorded per run):
- OS: WSL2 Ubuntu; kernel 6.18.33.2-microsoft-standard-WSL2; x86_64.
- Compilers: Ubuntu clang 21.1.8 (repo builds), g++ (conventional probes if
  noted; actual commands recorded per run).
- Build modes: xmake debug / release / tsan / asanubsan (repo modes).
- Backends: FakeAsyncBackend (deterministic), ThreadPoolBackend (real threads),
  io_uring real liburing 2.14 available via `--with-liburing=true`.
- Formal: tla2tools v1.7.4 (locked, `~/.cache/sluice/formal/tla2tools.jar`),
  entry `python3 scripts/formal/verify.py`; GenMC v0.17.0 (commit 29b03a6) at
  `~/tools/genmc-src/build/bin/genmc`.
- DST: `tests/dst_pv1_schedule_driver_test.cpp` driver (PR #241), T5 = Q-LIV-1
  replay/regression class.

## New Sluice-side probes (tests/research/se2/)

P-S01 `se2_h01_borrow_destroy_probe` (task §18 REQUIRED):
- Two modes, one hazard (borrow destroyed while request in flight):
  - `--mode=fake`: FakeAsyncBackend; submit read with heap borrow → `delete[]`
    borrow → `complete_oldest_with_bytes(64)` → wait/reap. Expected: submit
    succeeds, NO fail-fast, Completion ready with value 64 (Sluice layer
    MISSES). Fake backend never touches the borrow, so ASan expected clean
    (MISSES) — the fake-mode run isolates the SLUICE layer answer.
  - `--mode=threadpool`: ThreadPoolBackend + pipe; submit read with heap
    borrow → `delete[]` borrow → write 13 bytes to pipe → worker's real
    `read(2)` writes into freed memory → Completion ready (Sluice MISSES),
    ASan heap-use-after-free (ASan DETECTS).
- Repetitions: 1 deterministic run per mode per sanitizer (plus 20 healthy
  repeats for threadpool plain). Timeout 30 s per run.
- Expected outcomes pre-registered: fake plain exit 0; fake ASan exit 0 clean;
  threadpool plain exit 0 with success published; threadpool ASan abort with
  heap-use-after-free.

P-C01..P-C07 conventional probes (tests/research/se2/conventional/, standalone
C, raw POSIX AIO / pthreads / liburing; each obeys normal API usage except the
ONE normalized hazard):

| ID | Family | Form | Expected (pre-registered) |
|----|--------|------|---------------------------|
| P-C01 | H01 | POSIX aio read into freed heap buffer; instrumented post-completion deref of the freed target | plain: silent garbage (MISSES); ASan: heap-use-after-free (DETECTS). Kernel writes are outside ASan sight; the deref is the normalized final trace step. |
| P-C02 | H02 | raw io_uring: two in-flight ops share user_data; caller bookkeeping assumes 1:1 | two CQEs under one identity; misattribution/double-consume; plain MISSES |
| P-C03 | H03 | raw io_uring: SQE prepared on pipe fd, fd closed and number reused by a file BEFORE submit; read lands in the file | buf shows file bytes; silent wrong-target I/O; plain MISSES; fully deterministic (lowest-fd numbering) |
| P-C06 | H06 | POSIX lio_listio batch, one invalid entry; caller checks only the list return, assumes all-or-nothing | per-entry EINVAL invisible to caller; lost work; plain MISSES |
| P-C07 | H07 | pipe with F_SETPIPE_SZ=4096; partial write(8192)=4096; retry re-sends from WRONG offset | duplicated bytes on drain; silent stream corruption; plain MISSES |
| P-C09 | H09 | choreographed lost wake: signaler signals between waiter's predicate check and cond_wait (missing recheck is THE hazard); all state mutex-protected | plain: hang reproduced, bounded by alarm(5) → recorded; TSan: CLEAN (no data race — TSan MISSES logic lost-wake) |
| P-C13 | H13 | store-buffering litmus, plain (non-atomic) int publications, 200000 iterations, persistent threads + barrier | plain: both-zero observations > 0 (silent publication failure; MISSES); TSan: data race reported (DETECTS) |

Repetitions (frozen): deterministic probes 1 run (+20 healthy repeats where
noted); P-C09 TSan ×3; P-C13 plain ×1 (200k iterations) + TSan ×1; all
conventional probes timeout 30 s per run.

## Existing-witness re-execution (freshness, no new code)

- L1 negative-compile gates (H02/H05): verify-async-identity-negative-compile.sh,
  verify-request-arena-negative-compile.sh,
  verify-request-handle-authority-negative-compile.sh,
  verify-completion-authority-negative-compile.sh.
- Debug build + focused tests (H02/H03/H05/H07/H10/H12, SB-01/05/06):
  completion_binding, request_lifecycle_scheme_b, threadpool_backend_phase_e,
  async_op_helpers, async_durability, wal_writer, async_queue_lifecycle_death,
  fe2_publication_atomicity_death, dst_pv1_schedule_driver.
- TSan build + focused (SB-07/SB-09 freshness): async_rwlock_test,
  dst_pv1_schedule_driver_test.
- L6 TLA+ re-runs: request-arena, e10-waitnode, e9-park-wake, e11-timer-wait,
  e12-queue via `python3 scripts/formal/verify.py suite <name>` (record
  durations; suites too slow for the session budget are recorded NOT_TESTED
  THIS CAMPAIGN with the standing historical artifact as evidence).
- L7 GenMC: scripts/weakmem/verify-completion-weak-memory.sh (H05/H13 kernels
  + controls).
- L9 real liburing: record availability; run uring real-mode only if the
  configured build is already wired in this tree; otherwise BLOCKED/NOT_TESTED
  with the standing real-liburing evidence documents as authority.

## Mutation policy (frozen)

BIND-ONLY. All nine §49 mutation classes already have narrowly scoped,
reverted, recorded mutation witnesses in the repository (FE M1–M5, FE
corrective C1–C4, phase-c2b/c2c/c2d/c2e, phase-d2/d3/d4, PR #238 census, PR
#242/#243 evidence). No new production mutation is executed inside SE-2; the
matrix binds the existing records as P1 evidence with class IDs M1–M9.

## SB-10 exploratory campaign (task §21, OUT-OF-SE1)

- Target: `select_event_registry_test` (the #229-adjacent one-time TSan hang
  candidate, E4-only, unconfirmed).
- Pre-registered: TSan binary ×10 runs + plain binary ×100 runs; per-run
  timeout 60 s; no DST framework expansion; no seam modification.
- Possible outcomes: A reproduced → NEW FINDING (outside denominator);
  B not reproduced → STILL UNCONFIRMED; C deterministic scheduler seam allows
  falsification → DISMISSAL RECOMMENDED.

## Claim adjudication order (frozen)

1. Freeze matrix JSON → 2. adversarial miss audit (§48) → 3. migration
classification (§32) → 4. T-S1b adjudication (§33) → 5. T-S2 net-safety
ledger (§34) → STOP before any aggregate score.

# MATRIX

Authority: `docs/results/safety/se2-detection-matrix.json` (validator
`python3 scripts/verify-se2-detection-matrix.py` == PASS: 20 rows = 13 + 7,
363 cells, SB-10 outside the denominator, no score-like fields, no forbidden
claims). Interpretation: `docs/verification/se2-detection-matrix.md`.
Raw execution metadata: `docs/results/safety/se2-probe-results/execution-log.json`.

Plan amendments executed during the campaign (recorded, not silent):
1. P-C06 was redesigned after measured glibc behavior: lio_listio(LIO_WAIT)
   with an invalid entry is ATOMIC on this host (rc=-1/EIO, zero execution) —
   the planned "double-execution residue" outcome is not producible with a
   competent baseline; the probe now records the conventional-stronger fact
   (§37). Pre-registration principle (freeze expected results) was respected by
   recording the measurement and re-freezing before the evidence run.
2. P-C02: ASan caught a probe-internal bug (26-byte read from a 13-byte array)
   during bring-up; fixed inside the probe, evidence recorded after the fix.
3. P-S01 threadpool mode was moved from pipe to a temp file because the backend
   performs positional reads (pipes do not support them — ESPIPE), not because
   of any hazard-shape change.

# MUTATION RESULTS

Bind-only (frozen policy held): all nine classes bound to existing narrowly
scoped reverted records — M1 FE M1-M5 + e10; M2 phase-c2b/d3; M3 AC-2b census +
rwlock precedence negatives; M4 phase-c2e/d4; M5 FE M5 + DST V1; M6 R2-ALLOC +
c2d + wal-fuzz; M7 FE M3/PR #243; M8 M4-compile-rejection; M9 GenMC controls.
No new production mutation was executed.

# MISSES

1. H01 borrow lifetime: Sluice blind (P-S01: success published into freed
   borrow; 20/20 plain silent; ASan-only detection). REQUIRED §18 case
   preserved. No production machinery added.
2. H03 fd-number-reuse window: below the Sluice boundary; wrong-target I/O
   identical to raw io_uring in the accept→execution window.
3. H12 kernel error-clearing: below every user-space layer; policy half typed
   and tested; kernel half BLOCKED.
4. Real-backend fault-injection coverage: the weakest layer (L9 cells
   NOT_TESTED for H04/H05/H08/H09/SB-02/SB-05; standing D2/D4 historical).
5. Layer-fingerprint bounds: TSan blind to logic lost wake (P-C09 3/3 clean
   with hang reproduced); ASan blind to kernel-side writes (P-C01).

# CONVENTIONAL STRONGER CASES

1. H06: glibc lio_listio(LIO_WAIT) invalid-entry initiation is atomic
   (batch-level -1/EIO, zero residue) — mechanism-equivalent to Sluice's
   transactional submission for that shape.
2. H05/H08/H11: current upstream kernels PREVENT the historical defects by
   mechanism — the comparison is protocol-hardening vs kernel-mechanism, not
   "conventional is silent".
3. H13: TSan detects the conventional race class (P-C13 exit 66) at comparable
   detection power to the GenMC layer for the modeled law (different cost).

# SLUICE-INDUCED FINDINGS

See matrix `induced_interpretation` per row and the §44 table in
docs/verification/se2-detection-matrix.md. Bucket split preserved:
production-runtime 5 (SB-01/02/05/06/07), internal/seam 1 (SB-08), test-only 1
(SB-09). Discovery attribution: human adversarial review ×4, TSan ×2 (SB-07,
SB-09). Every induced interpretation question (§38 A–E) answered per row; not
forced favorable — e.g. SB-01's four-site reconcile obligation was both the
defect surface and the repair obligation.

# NEW BUGS

None in production code. Current-master P0/P1: none found. P2/P3: none newly
recorded (SE1-SB-04 group.hpp growth idiom remains the known open item,
OUT-OF-SE1, untouched per non-goals). Probe-internal bug (P-C02) found and
fixed inside the probe; not a repository production defect.

# ADVERSARIAL MISS AUDIT

Full A–J answers in docs/verification/se2-detection-matrix.md §Adversarial miss
audit. Highlights: H11 L0 rests partly on census text (flagged); seam/H06
"static" claims are gate/runtime-enforced not compiler-enforced (flagged);
DST T4 cells correctly labeled REPRODUCES not DETECTS; TSan/ASan blind spots
executed and recorded; real-backend fault injection is the under-covered layer;
no contradictions with SE-1 primary statuses (refinements only).

# T-S1b

**NOT PROVEN — NARROW DIRECT SUPPORT** (corrected by AMENDMENT 4; the original
"PROVEN — SCOPE-BOUNDED" verdict was retracted by human review 5060477073).

Direct current-executed comparative support: exactly 3 of 13 conventional
families — H02 (identity), H09 (lost wake), H13 (weak-memory publication).
7 of 13 are M0 vs the current baseline: current kernels prevent H05/H08/H11
by mechanism, glibc listio initiation is atomic at the executed H06 shape, H07
partial-retry is parity at the raw surface, H01/H03 executed shapes are
parity-in-miss. Claim-only classes on documented conventional postures
(excluded from direct support): H04 (M5), H10 (M3), H07 zero-progress (M3),
H03 backend-state half (M3). The earlier "10 classes with direct re-executed
evidence (M2–M5)" statement used the historical broken kernel as a comparator
and was over-counted. The GENERAL T-S1b thesis is NOT established at
population level. T-S1a (per-hazard, SE-1) unchanged. Full partition and
per-row comparison_basis live in the JSON authority (`ts1b_adjudication`,
`comparison` blocks) and are machine-checked.

# T-S2

**NOT YET READY** (unchanged; ledger figures corrected by AMENDMENT 4:
5 of 13 families carry non-M0 classes, exactly 3 with direct
current-executed support; 7 are M0 vs the current baseline; Sluice
remains-silent list, induced production 5 with root causes/discovery/failure
forms, and internal/test bucket separation unchanged). No aggregate score
and none is computed; a preregistered aggregation/severity methodology does
not exist. The ledger supports at most: "SE-2 gives #227 per-hazard facts and
a narrow comparative result; the evidence is mixed (3 direct migrations, 7
M0-vs-current-baseline families, real remaining silent faces, 5 induced
production hazards — 4 of 5 found by humans, not tooling)."

# G1-SAFETY IMPLICATION

**NOT READY FOR FORMAL ADJUDICATION** (corrected by AMENDMENT 4; the original
"READY FOR FORMAL ADJUDICATION (PARTIAL evidence base)" was retracted by
review 5060477073 — it coexisted with T-S2 NOT YET READY and a T-S1b verdict
that did not survive comparative re-adjudication). SE-2's corrected output is:
per-hazard facts + a narrow (3/13) direct comparative result + honest misses +
induced-hazard attribution. G1-Safety adjudication belongs to #227 after human
review of this PR; SE-2 alone does NOT advance it to ready.

# AMENDMENT 4 — SE-2-CORRECTIVE-1 (human review 5060477073)

Review: "CHANGES REQUIRED BEFORE SE-2 FREEZE / T-S1b PROMOTION" (review ID
5060477073 on Draft PR #246, 2026-08-30). Three blockers, all addressed:

1. **T-S1b comparator error (Blocker 1).** The original verdict counted
   families as Sluice migrations where the CURRENT conventional baseline also
   prevents (H05/H08/H11 kernel mechanisms; H06 glibc atomic initiation at the
   executed shape) or where no conventional probe was executed (H04, H10,
   H07, H03-half). Re-adjudication law: the comparator is the current
   competent conventional baseline; historical-fixed and current-documented
   rows can never be direct T-S1b support. Result: M0=7, M2=1 (H09), M3=1
   (H10, claim), M4=1 (H02), M5=2 (H04 claim, H13), MX=1 (H12); direct
   support = H02/H09/H13; T-S1b = NOT PROVEN — NARROW DIRECT SUPPORT.
   H07/H03/H06 carry per-subshape splits in their JSON comparison blocks.
2. **Branch pollution (Blocker 2).** Rebased onto 1fd8a2f; 0afd9e5 removed;
   four human review reports restored as untracked, byte-identical.
3. **Claim authority (Blocker 3).** comparison_basis / migration_class /
   direct_ts1b_support per row and the ts1b_adjudication block now live in
   `se2-detection-matrix.json`; the markdown is a derived view and the
   extended validator enforces the partition, the direct-support rule
   (direct ⇒ current-executed + executed conventional probe), and
   markdown-count export.

Gates re-run after the corrective edits: verify-se2-detection-matrix.py PASS
(extended), verify-se1-hazard-corpus.py PASS, check-doc-links.py PASS,
verify-architecture-docs.py PASS, git diff --check clean, pre-push gate with
explicit range, exact-head CI on the rebased branch. No probe re-executed
(docs-only tree delta; see execution-log.json `corrective_1`).
