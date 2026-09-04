# BATCH-X0-REPORT — explicit Batch as a bounded control-plane amortization grant

- Campaign: BATCH-X0 (G1-Control Candidate 3) under #227 / #221 / #259
- Verdict summary: **STOP — NO C1** (terminal; post-Corrective-1 verdict
  state in §0.3)
- Branch: `research/g1-control-batch-x0`; BASE master `4a57cc4f`;
  freeze `c0da5db5`; evidence head: see PR.
- RESEARCH ONLY. Production code untouched (diff = research/ + bench/ +
  xmake/benchmarks.lua target only). The current public Batch contract is
  unchanged. No C1 is authorized or proposed.
- Corrective-1 (post-review) applied: the five evidence-gate/validator
  findings are closed, the formal performance session is SUPERSEDED
  (frozen substrate violation), and the compliant-substrate rerun is
  BLOCKED on this host. The semantic result is unaffected and stands.
- Corrective-2 (status closure) applied: the validator CLI now
  distinguishes VALIDATION FAIL / VALIDATION BLOCKED / VALIDATION PASS
  (§0.5); no research conclusion, verdict, or measurement changed.

## 0. Post-review correctives — evidence-gate closure (Corrective-1) and status closure (Corrective-2)

Adversarial review of the original report found that several claimed
formal gates were not actually mechanically enforced, and that the formal
performance session had violated the frozen substrate rule. This section
is the authoritative post-corrective record; sections 1–6 are retained
with their superseded content explicitly labeled.

### 0.1 Findings closed

```text
P1-1  Frozen substrate mismatch
      before: prereg §9 froze "one pre-created regular file on the host
              filesystem (ext4)"; the driver hardcoded /tmp (tmpfs) and
              recorded no filesystem evidence.
      after:  run_batch_x0.py formal modes (qualify/matrix/enters) require
              --work-dir; the gate runs findmnt -T <dir> and fails closed
              (exit 13) on any non-ext4 filesystem BEFORE any benchmark
              data-file creation or measurement; environment.json records
              work_dir / filesystem_type / filesystem_source /
              mount_target. matrix additionally requires --qualification.
      proof:  live probes — `matrix` and `qualify` with
              --work-dir /tmp/... both refuse with "SUBSTRATE GATE
              REFUSED ... is tmpfs" (exit 13, no rows written);
              validator selftests "formal matrix on tmpfs substrate" and
              "formal matrix without a filesystem record" rejected.

P1-2  A/A qualification was not part of VALIDATION PASS
      before: the validator read the qualification's precomputed
              verdicts.json (and only when a directory happened to be
              nearby); a matrix could validate with no A/A evidence.
      after:  validate_aa() recomputes the frozen 5%/90% gate from RAW
              rows-pass1/2.jsonl (two passes, B1/B2 only, full 48-cell
              grid × 7 reps); a recorded verdicts.json is cross-checked
              and must agree; the matrix session must DECLARE its
              qualification_session and the validator is explicitly bound
              with --qualification (no latest-directory heuristics).
      proof:  selftests — A/A failure (40/48) rejected, missing pass-2
              rejected, recorded-verdict disagreement rejected, binding
              mismatch rejected. Diagnostic on the REAL raw qualification
              rows: recompute = 47/48 within 5%, worst cell
              B1/read/4K/N=1 at 0.2094 — matches the recorded Amendment-1
              verdict (format compatibility proven).

P1-3  Freeze-descendant check was vacuous
      before: is_descendant() returned True unconditionally.
      after:  real git ancestry — cat-file -e <sha>^{commit} then
              git merge-base --is-ancestor <freeze> <sha>; fails for
              invalid SHAs, pre-freeze ancestors, and foreign lineages.
      proof:  selftests — freeze itself and HEAD accepted;
              pre-freeze master commit 39f9d984 and a garbage SHA
              rejected (both as direct assertions and end-to-end on a
              matrix session pinned to the pre-freeze commit).

P1-4  STOP-gate ordering: substrate anchor was checked AFTER promotion
      before: value/G1/PROMOTION were computed first; control was then
              overwritten to BLOCKED, so a session could in principle
              carry control=BLOCKED with promotion=PROMOTE-CONSIDER.
      after:  blocking gates are collected into blocked_reasons BEFORE
              the value/G1/PROMOTION derivation; any blocked reason caps
              value at NOT ESTABLISHED, G1 at NOT ESTABLISHED, and
              PROMOTION at STOP — NO C1. BLOCKED remains distinct from
              NOT MATERIAL in the verdict vocabulary.
      proof:  selftest "P1-4 substrate-anchor failure" — a synthetic
              session with a MATERIAL performance result and a
              MB1−B1 anchor of 0.5 yields substrate_anchor_ok=false,
              CONTROL=BLOCKED, G1=NOT ESTABLISHED, PROMOTION=STOP.

P1-5  Transport evidence was aggregated by arm
      before: five arm-total enter counts were the only M6 evidence; the
              cell-sensitive frozen rule (enters/ops, B0 ≈ 1/N) could not
              be reconstructed; the old "M6 corruption" selftest only
              proved verdict sensitivity, not corruption rejection.
      after:  the untimed counter pass records one row per
              arm×op×size×N (120 cells) in strace-enter-rows.jsonl
              (fields: arm, op, size, n, rounds, ops, io_uring_enter,
              counter_reps); the validator rejects missing/duplicate/
              invalid cells and derives the frozen §10 transport rule
              per cell. Conservative mechanical reading, disclosed here
              BEFORE any compliant-substrate evidence exists: B1↔B2
              equivalence per cell = |r1−r2| ≤ max(5%, 2/ops); B0 ≈ 1/N
              per cell within 25%; "B2 materially fewer" = r_B2 ≤ 0.95·
              r_B1 on ≥4/6 N-cells in BOTH op classes; evidence satisfying
              neither frozen outcome → BLOCKED (never a positive claim).
      proof:  selftests — M6-A missing/duplicate/negative cells rejected;
              M6-B valid evidence with B2 at 0.4× B1 rate recomputes the
              verdict to ADDITIONAL TRANSPORT AMORTIZATION ESTABLISHED
              (a different lawful verdict, not a rejection); equivalence
              violated without B2-fewer → BLOCKED; M6-C equivalent
              topology → ALREADY OBTAINED BY PRIMITIVE SUBMITS.

P2    Selftest/report wording overstated enforcement
      before: "validator self-test 4/4 corrupted evidence rejected"
              counted a verdict-sensitivity test as a corruption test.
      after:  the selftest enumerates what each test actually proves
              (21 named tests after Corrective-2; M6-B is labeled a
              verdict recomputation, not a rejection); the report uses
              the same precision.
```

Validator selftest suite: 21 named tests, all passing
(`validate_batch_x0.py --selftest`; 20 from Corrective-1 + the
Corrective-2 validation-status test).

### 0.2 Superseded performance evidence and the blocked rerun

```text
SUPERSEDED: results/batch-x0-perf-native-2 (the formal performance
session) violated the frozen substrate requirement:
  preregistration §9 = one pre-created regular file on the host ext4
  execution          = /tmp tmpfs (unverified, unrecorded)
The session is marked with SUPERSEDED.json; rows are retained
unmodified as non-authoritative observations. The performance-side
verdicts previously derived from it (TRANSPORT, CONTROL, PERFORMANCE,
SLUICE-SPECIFIC-VALUE, G1-CONTROL) are no longer formal campaign claims.
```

Rerun disposition (Corrective-1 §21 protocol):

```text
BLOCKED — the host has no usable ext4 filesystem:
  lsblk -f: sda2 ext4 1.9GiB mounted at /boot (system partition,
            ~1.2GiB free — not a benchmark substrate);
            sda3 btrfs 445GiB (/ and /home); sda1/sdb2/nvme0n1p1/
            nvme0n1p3 vfat/ntfs; zram0 swap; nvme0n1p2/sdb1 unformatted.
  findmnt -t ext4: /boot only. No device-mapper volumes, no loop
  devices; interactive sudo only (provisioning partitions or images is
  destructive and out of scope).
Per the frozen protocol: no btrfs/xfs/tmpfs substitution, no post-hoc
preregistration amendment. The semantic STOP result stands independently.
```

A compliant rerun is a bounded future task on any host with a real ext4
work filesystem: `run_batch_x0.py qualify/matrix --work-dir <ext4 path>
--qualification <qualify-session>`; the corrected gates then enforce
everything P1-1..P1-5 mechanically.

### 0.3 Post-corrective verdict state

Not a validator verdicts.json (no compliant-substrate matrix exists); this
is the campaign-level fail-closed state derived from the formal semantic
evidence, the frozen disposition (prereg §6), and the supersession:

```text
BATCH-X0-SEMANTIC-GRANT          CURRENT BATCH DOES NOT GRANT GROUP
                                 ADMISSION        [STANDS — formal; S9
                                 DIVERGENCE + audit F2 are substrate-
                                 independent]
BATCH-X0-TRANSPORT-AMORTIZATION  BLOCKED          [was: ALREADY OBTAINED...
                                 from the superseded tmpfs session]
BATCH-X0-CONTROL-AMORTIZATION    BLOCKED          [was: NOT MATERIAL]
BATCH-X0-PERFORMANCE             BLOCKED          [was: NOT MATERIAL]
BATCH-X0-MINIMALITY              THIN FLOOR SUFFICIENT  [STANDS — static
                                 design gate on the bench source; M8]
BATCH-X0-SLUICE-SPECIFIC-VALUE   NOT ESTABLISHED  [was: PORTABLE THIN-
                                 BASELINE VALUE ONLY]
BATCH-X0-G1-CONTROL              NOT ESTABLISHED
PROMOTION                        STOP — NO C1     [fail-closed: the grant
                                 condition is unreachable under the
                                 current contract (prereg §6/§10), and
                                 no compliant perf evidence exists to
                                 argue for a positive case]
```

### 0.4 Evidence lineage (complete)

```text
freeze                        c0da5db5 (prereg + audit pre-date it)
semantic evidence             results/batch-x0-semantic-native-1
                              (S1–S10 PASS, S9 DIVERGENCE) — STANDS
failed strace qualifications  results/batch-x0-qualify-native-1
                              (66.7%) + the second attempt (62.5%,
                              recorded in Amendment 1)
Amendment 1                   2d2108cf (strace out of the timed window)
prior (tmpfs) qualification   results/batch-x0-qualify-native-2
                              (47/48; variance record; not ext4-admissible)
dirty superseded matrix       results/batch-x0-perf-native-1-superseded
tmpfs superseded matrix       results/batch-x0-perf-native-2 + SUPERSEDED.json
ext4 qualification / matrix   BLOCKED (no usable ext4 on this host)
per-cell enter evidence       BLOCKED with the matrix (the counter pass is
                              part of the formal session)
```

### 0.5 Corrective-2 (status closure) — validator CLI status model

```text
Defect: after Corrective-1, validate_matrix() correctly derived
        CONTROL=BLOCKED / PROMOTION=STOP for anchor-blocked evidence, but
        the CLI still printed "VALIDATION PASS" (exit 0) for such a
        result — a blocked campaign could look like a passing validation.
after:  the CLI distinguishes three states, keyed on the explicit
        experiment-level gate authority blocked_reasons (domain verdicts
        such as TRANSPORT=BLOCKED from valid measurements NEVER decide
        the status):
          VALIDATION FAIL     malformed/non-authoritative evidence   exit 1
          VALIDATION BLOCKED  valid evidence, a preregistered gate
                              (S-9 substrate anchor) stops the
                              downstream claims; verdicts.json is still
                              written with blocked_reasons          exit 2
          VALIDATION PASS     all mandatory gates passed             exit 0
proof:  selftest "final validation status" (valid session → PASS,
        anchor-blocked valid session → BLOCKED); live CLI probes —
        synthetic valid session → VALIDATION PASS (exit 0,
        blocked_reasons=[]), synthetic anchor-blocked session →
        VALIDATION BLOCKED (exit 2, verdicts.json preserved with
        blocked_reasons=['S-9 substrate anchor |MB1−B1|/B1 = -0.500 >
        0.3']), the superseded native-2 session → VALIDATION FAIL
        (exit 1, structurally non-authoritative).
Also:   Corrective-1's P1-1 wording "before any file creation or
        measurement" is narrowed to "before any benchmark data-file
        creation or measurement" — the driver creates the (empty) work
        directory before findmnt probes it; no measurement or data file
        precedes the gate. Driver behavior unchanged.
```

---

The sections below are the original report, retained with superseded
content labeled. Measurement numbers they cite come from the SUPERSEDED
tmpfs session unless explicitly marked as audit/structural facts.

## Execution record (prereg §12 order)

```text
Step 0   master synced to 4a57cc4f; branches cleaned            (pre-freeze)
Step 1   as-built audit        BATCH-X0-AUDIT.md                (pre-freeze)
Step 2-4 preregistration frozen c0da5db5 (no formal evidence earlier)
Step 5   bench g1_control_batch_x0_bench + driver + validator   8ae43020
Step 6   mutant self-tests M1-M5 all REJECT (+M4 verifier sanity)
Step 7   formal semantic fixtures S1-S10 all PASS
         S9 witness: DIVERGENCE (see §2)                        926e5772
Step 8   host qualification: A/A strace-wrapped FAILED twice
         (66.7% / 62.5% cells within 5%) → instrument diagnosis
         (ptrace in the timed window) → Amendment 1 (pre-formal,
         disclosed) → strace-free A/A PASS 47/48 = 97.9%
         [tmpfs session; variance record only post-Corrective-1]
         results/batch-x0-qualify-native-1 (FAIL, retained) and
         -native-2 (PASS, full rows)                            2d2108cf
Step 9   formal matrix: first session SUPERSEDED (its environment
         recorded dirty_tracked=true — driver amendments were
         uncommitted at run time; commit-pin discipline). Clean
         re-execution: results/batch-x0-perf-native-2, 840 timed
         rows (120 cells × 7 reps, strace-free) + enter-counter
         pass [SUPERSEDED post-Corrective-1: tmpfs substrate]
Step 10  mechanical verdicts: pre-corrective validator PASS
         [verdicts.json no longer a formal claim]
Step 11  adversarial self-review (§5 below)
Step 12  this report + Draft PR
—        Corrective-1: evidence-gate closure (P1-1..P1-5, P2);
         native-2 SUPERSEDED; rerun BLOCKED (no host ext4)      (this PR)
```

## 1. As-built answers — audit facts STAND; measurement numbers are from the SUPERSEDED tmpfs session (§0.2)

```text
Q1  Does explicit Batch change transport topology?
    STRUCTURAL (audit F5, STANDS): the production backend never calls
    io_uring_submit per op — SQEs accumulate and flush at poll/wait_one,
    so primitive consecutive submits already reach the kernel in batches
    whenever queue_depth ≥ N.
    EMPIRICAL (SUPERSEDED): B1 vs B2 total io_uring_enter over the
    identical grid: 19488 vs 19539 (0.26% apart); read 64K N=1 cell:
    2003 enters for 2000 rounds; N=2 cells: 1003 for every arm incl. B1.

Q2  Is current Batch itself expensive?              [SUPERSEDED MEASUREMENT]
    Wrapper delta mixed-sign; not material under the frozen rule; at
    read 4K N≥16 the wrapper measured SLOWER than the primitive loop it
    drives (unique_ptr slot allocs + linear scans, audit W1-W5) — an
    implementation observation, not a defect claim.

Q3  Can per-op control cost legally amortize?       [SPLIT]
    LEGALITY (STANDS, measurement-independent): fused admission is NOT
    semantically legal — S9 DIVERGENCE + frozen disposition §6.
    MEASUREMENT (SUPERSEDED): MB3−MB1 was noise (sign-inconsistent) on
    the tmpfs session; no compliant-substrate measurement exists.

Q4  Does a competent standalone impl get the same?  [SUPERSEDED MEASUREMENT]
    On the tmpfs session B0 was the fastest arm in every cell; no
    compliant-substrate comparison exists post-supersession.
```

MB substrate anchor (S-9): the 18.4% figure is from the SUPERSEDED
session; the anchor check itself is now mechanically enforced BEFORE the
value/G1/promotion derivation (P1-4).

## 2. The decisive semantic witness (S9, prereg §4/§6) — STANDS

MB substrate, request capacity 3, batch {A1..A4} + external op B:

```text
per-op admission shape:  accepted {A1, A2, B}  rejected {A3, A4}
fused admission shape:   accepted {A1, A2, A3}  rejected {A4, B}
verdict: DIVERGENCE
```

The external op B crosses accept under the per-op contract and is rejected
under fused group admission. **Accepted membership is observable under the
two admission disciplines — fused admission is NOT a semantics-preserving
optimization of the current Batch contract.** Combined with the frozen
disposition (prereg §6: the contract grants no group admission — audit F2),
this fixes BATCH-X0-SEMANTIC-GRANT independent of any measurement and of
any substrate. This witness is functional (scripted/rear-backend
behavior), not a timing measurement: it is unaffected by the §0.2
supersession.

All other fixtures: S1 real-uring exactness, S2 capacity pressure
(4 accepted + 2 would_block, rejections surface first in submission
order), S3 rejected-vs-accepted-error origin distinction, S4 reap order
2,0,1 preserved, S5 mixed kinds, S6 zero members (idle + context-global
outstanding behavior recorded), S7 N=1, S8 C−1/C/C+1 exact rejection
shape, S10 per-member terminal independence — PASS.

## 3. Machine-readable verdicts

Post-corrective verdict state: §0.3 (authoritative). The pre-corrective
verdicts.json (results/batch-x0-perf-native-2/verdicts.json) recorded:

```text
BATCH-X0-SEMANTIC-GRANT          CURRENT BATCH DOES NOT GRANT GROUP ADMISSION
BATCH-X0-TRANSPORT-AMORTIZATION  ALREADY OBTAINED BY PRIMITIVE SUBMITS   [SUPERSEDED]
BATCH-X0-CONTROL-AMORTIZATION    NOT MATERIAL                            [SUPERSEDED]
BATCH-X0-PERFORMANCE             NOT MATERIAL                            [SUPERSEDED]
BATCH-X0-MINIMALITY              THIN FLOOR SUFFICIENT (MB region 310 ≤ 480
                                 lines; no framework tokens; M8)          [STANDS]
BATCH-X0-SLUICE-SPECIFIC-VALUE   PORTABLE THIN-BASELINE VALUE ONLY       [SUPERSEDED]
BATCH-X0-G1-CONTROL              NOT ESTABLISHED
PROMOTION                        STOP — NO C1
```

Topology table (per-op submit calls / SQEs / kernel enters / admission
sections / reap episodes) — the STRUCTURAL columns are audit facts (F4,
F5: submission shape and flush-at-drive model); the numeric enters column
was measured on the SUPERSEDED session:

```text
arm  submit calls  SQEs  enters per round (audit model)   admission sections  reap episodes
B0   1/round       N     1 (submit per round)             n/a (no arena)      1/round
B1   N/round       N     ~1 (flush+wait at first drive)   2N (ctx+backend outer)  ≤N
B2   N/round       N     ~1 (identical to B1)             2N (identical)      ≤N
MB1  N/round       N     ~1                               2N                  ≤N
MB3  1/round       N     ~1                               1                   ≤N
semantic differences: only MB3's admission section count differs; S9 proves
that difference changes accepted membership under capacity pressure.
```

## 4. What was falsified / what remains unknown

Falsified (S = stands; measurement-dependent parts labeled):

```text
F-A  "explicit Batch buys kernel-transport batching that primitive
     submits do not already get" — structurally false on the as-built
     backend (audit F5: no per-op submit; flush at poll/wait_one). [S]
     The empirical enter-counter corroboration came from the superseded
     session. [M]
F-B  "fusing admission/dispatch control work is a semantics-preserving
     optimization opportunity" — false (S9 DIVERGENCE + frozen
     disposition). [S] Its immateriality on the tmpfs session was a
     measurement observation. [M]
F-C  "the Batch wrapper currently adds measurable value over the
     primitive loop" — not established on the superseded session; no
     compliant measurement exists. [M]
F-D  "an explicit group boundary would be the first Candidate whose
     value is not a Linux capability or portable wrapper discipline" —
     not earned: the grant itself does not exist, so no measured value
     could be Sluice-specific. [S]
```

Unknown / out of scope (not claimed either way):

```text
U-1  ThreadPoolBackend control arm — not run (prereg §7: not doubled).
U-2  All-or-nothing / contiguous batch ADMISSION as a NEW contract —
     would be a separate future candidate (goal §9); BATCH-X0 neither
     prices nor proposes it. S9 shows exactly the observable it would
     have to own: which requests cross accept under capacity pressure.
U-3  Behavior at N > queue_depth (dispatch-queue retry shape) — outside
     the frozen matrix (capacity = 64 ≥ N = 32 everywhere).
U-4  Native batch value on kernels/backends with different flush
     topology — bounded by this host/kernel (7.1.9) and liburing 2.13;
     no compliant-substrate measurement exists post-supersession.
U-5  The wrapper's negative trend at 4K/N≥16 was an implementation
     observation on the superseded session (W1-W5); no change is
     proposed in this research-only campaign.
U-6  All performance-side questions on a COMPLIANT (ext4) substrate:
     blocked by host hardware (§0.2); the corrected gates are in place
     for a future rerun.
```

## 5. Adversarial self-review (goal §33) — measurement-citing answers labeled

```text
1  Does current primitive submission already batch SQ transport?
   STRUCTURALLY YES (audit F5; STANDS). Empirical confirmation came from
   the superseded session. [M]
2  What exactly does explicit Batch add today? Orchestration only
   (add/await_one/next, reap-order iteration, origin bookkeeping,
   wrapper allocs/scans — audit W1-W5). [S] Zero measured transport
   delta on the superseded session. [M]
3  Is kernel-enter reduction different between B1 and B2?
   No structural reason (F5); measured 0.26% apart on the superseded
   session. [M]
4  Which work is genuinely per-op semantic floor? The admission ladder
   (arena reserve/prepare/bind/commit/enqueue/mark_running), the accept
   CAS/release-store, cookie/SQE identity install. Present in EVERY
   arm incl. B3/MB3. [S]
5  Which work is merely current implementation repetition? The outer
   access_mtx_/admission critical sections (2 per op) — and S9 proves
   amortizing them changes accepted membership (illegal). [S] Their
   measured amortization value was ≈0 on the superseded session. [M]
6  Does the proposed fusion change admission interleaving? YES — the
   S9 DIVERGENCE witness is exactly this. [S]
7  Under capacity pressure, does the same set of requests cross accept?
   NO — B crosses accept per-op and is rejected fused. [S]
8  Are submit rejection and accepted error still distinguishable? YES
   (S3; origin orthogonal to error, pinned by fixtures + regression). [S]
9  Does result iteration preserve true reap order? YES (S4 2,0,1;
   M3 rejects the submission-order mutant). [S]
10 Does cancellation remain per-operation? YES (S10; no group-cancel
   API exists — audit F3; M5 rejects the collapse mutant). [S]
11 Did we create a Batch-level lifecycle authority? NO — MB3 is a
   research-only instrument, uninstalled, never a vtable entry; no
   BatchRequest/OperationStorage exists anywhere in the diff (M7/M8). [S]
12 Could a 50-line standalone helper obtain the same result? The RESULT
   (transport batching) is obtained by B0 — a ~60-line raw liburing
   loop — on the superseded session's measurements; structurally, B0
   submits N SQEs and issues one enter per round by construction. [S/M]
13 Is benefit caused by explicit semantic information or a better loop?
   There is no benefit to attribute; the better loop is B0. [M]
14 Are we hiding a RequestArena redesign inside a benchmark? NO — MB
   arms call the PRODUCTION detail::submit_transaction + RequestArena
   ladder per op; identity witnesses (slot+generation per member) are
   in every MB row (validator M7 enforces this). [S]
15 Would we still recommend the semantic change if the gain were zero?
   There is no semantic change to recommend; the grant does not exist. [S]
16 Would we recommend the implementation if the gain were 2× but the
   semantics changed? NO — accepted-membership is observable contract
   surface; goal §6 forbids collapsing it without a separate earned
   contract (goal §9 keeps that OUT of X0). [S]
17 Are we willing to close with NO C1? YES — that is the verdict. [S]
```

## 6. Validation / CI / worktree

```text
bench selftest:            M1-M5 REJECT + M4 sanity (exit 0)
validator selftest:        21 named tests, all passing (§0.1; Corrective-1
                           expanded suite with real-git ancestry, A/A
                           recomputation, substrate, per-cell enter gates;
                           Corrective-2 added the final-status test)
live fail-closed probes:   driver matrix/qualify on /tmp tmpfs → REFUSED
                           (exit 13, before any measurement); validator on
                           the superseded native-2 session → VALIDATION
                           FAIL (exit 1); CLI status probes §0.5 — valid
                           synthetic → PASS (0), anchor-blocked → BLOCKED (2)
semantic fixtures:         S1-S10 PASS, S9 DIVERGENCE recorded [STANDS]
A/A gate:                  strace-wrapped FAIL ×2 (retained) → Amendment 1 →
                           47/48 on tmpfs [variance record; not ext4-
                           admissible; recomputation matches: 47/48,
                           worst B1/read/4K/N=1 = 0.2094]
formal matrix:             SUPERSEDED (tmpfs); rerun BLOCKED (no host ext4)
mechanical verdicts:       none post-supersession; campaign verdict state
                           in §0.3 is fail-closed and labeled
local mechanical gates:    pre-push.sh --range (doc links, mechanical
                           facts, claim/assert hygiene, diff --check) run
                           before push; CI = the remote authority
worktree:                  clean; branch research/g1-control-batch-x0
                           (Draft PR)
```

Environment: native Fedora host, kernel 7.1.9, 20 CPUs, clang release,
liburing 2.13, CPU pinned (cpu 2), commit-pinned, binary sha256 recorded
per session. The superseded sessions ran their work files on tmpfs; no
compliant-substrate session exists on this host (§0.2).
