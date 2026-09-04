# BATCH-X0-REPORT — explicit Batch as a bounded control-plane amortization grant

- Campaign: BATCH-X0 (G1-Control Candidate 3) under #227 / #221 / #259
- Verdict summary: **STOP — NO C1** (all eight verdicts mechanical, below)
- Branch: `research/g1-control-batch-x0`; BASE master `4a57cc4f`;
  freeze `c0da5db5`; evidence head `2d2108cf`+results commit (see PR).
- RESEARCH ONLY. Production code untouched (diff = research/ + bench/ +
  xmake/benchmarks.lua target only). The current public Batch contract is
  unchanged. No C1 is authorized or proposed.

## Execution record (prereg §12 order)

```text
Step 0   master synced to 4a57cc4f; branches cleaned            (pre-freeze)
Step 1   as-built audit        BATCH-X0-AUDIT.md                (pre-freeze)
Step 2-4 preregistration frozen c0da5db5 (no formal evidence earlier)
Step 5   bench g1_control_batch_x0_bench + driver + validator   8ae43020
Step 6   mutant self-tests M1-M5 all REJECT (+M4 verifier sanity)
         validator self-test 4/4 (M7 corruption, M6 flip, missing
         cell, fixture FAIL — all rejected)
Step 7   formal semantic fixtures S1-S10 all PASS
         S9 witness: DIVERGENCE (see §2)                        926e5772
Step 8   host qualification: A/A strace-wrapped FAILED twice
         (66.7% / 62.5% cells within 5%) → instrument diagnosis
         (ptrace in the timed window) → Amendment 1 (pre-formal,
         disclosed) → strace-free A/A PASS 47/48 = 97.9%,
         median ratio 0.95% (only violator B1/read/4K/N=1)
         results/batch-x0-qualify-native-1 (FAIL, retained) and
         -native-2 (PASS, full rows)                            2d2108cf
Step 9   formal matrix: first session SUPERSEDED (its environment
         recorded dirty_tracked=true — driver amendments were
         uncommitted at run time; commit-pin discipline). Clean
         re-execution: results/batch-x0-perf-native-2, 840 timed
         rows (120 cells × 7 reps, strace-free) + separate
         enter-counter pass (120 arm×cells under strace -c, untimed)
Step 10  mechanical verdicts: validator PASS → verdicts.json
Step 11  adversarial self-review (§5 below)
Step 12  this report + Draft PR
```

## 1. As-built answers (audit + measurement)

```text
Q1  Does explicit Batch change transport topology?   NO.
    B1 vs B2 total io_uring_enter over the identical 120-cell grid:
    19488 vs 19539 (0.26% apart). Per-cell evidence: read 64K N=1
    cell — B0=2003, B2=2003 enters for 2000 rounds (exactly
    1 enter/round + 3 ring setup); N=2 cells: 1003 (one enter per
    2-op round) for EVERY arm incl. B1. The production backend
    already accumulates N SQEs and flushes once per drive episode
    (audit F5: no enter per submit; flush at poll/wait_one).
    BATCH-T1 (kernel transport batching) is ALREADY OBTAINED BY
    PRIMITIVE SUBMITS whenever queue_depth >= N.

Q2  Is current Batch itself expensive?              MEASURED, small but
    signed. B2 vs B1 wrapper delta (positive = Batch faster):
    read 4K +0.6%..+7.6% (grows with N), read 64K +0.9%..+2.7%,
    write mixed (−2.0%..+6.6% at 4K; −1.5%..+1.9% at 64K). Not
    material under the frozen rule (needs ≥5% on ≥4/6 N-cells in
    BOTH sizes on BOTH op classes). At read 4K N≥16 the wrapper is
    measurably SLOWER than the primitive loop it drives (unique_ptr
    slot allocs + linear scans, audit W1-W5) — an implementation
    observation, not a defect claim.

Q3  Can per-op control cost legally amortize?       COST ≈ NIL; LEGALITY
    MOOT. The causal fused-floor contrast MB3−MB1 (one admission
    section + one install episode per batch vs 2 sections per op,
    everything else identical) is noise: read 4K −1.1%..+1.8%,
    write 64K −5.8%..+0.7%, sign-inconsistent. Even BEFORE the
    legality verdict, there is no material control-plane cost to
    fuse on this substrate and geometry.

Q4  Does a competent standalone impl get the same?  YES. B0 (raw
    liburing, no Sluice semantics) is the fastest arm in EVERY cell
    (e.g. read 4K N=32: B0 929 vs B1 1317 vs B2 1425 ns/op). The
    Sluice per-op semantic floor costs ~20-40% over raw at 4K —
    the price of per-op identity/admission, paid identically by
    primitive loops and Batch.
```

MB substrate anchor (S-9 check): MB1 is 18.4% cheaper than B1 at
read/4K/N=8 — inside the frozen 30% comparability bound, so the MB3−MB1
fusion delta is admissible evidence (and it is ≈ 0).

## 2. The decisive semantic witness (S9, prereg §4/§6)

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
this fixes BATCH-X0-SEMANTIC-GRANT independent of any measurement. Per goal
§25, this is where semantics constrain optimization — and per §1/Q3, even
the illegal fusion would have bought nothing material on this evidence.

All other fixtures: S1 real-uring exactness, S2 capacity pressure
(4 accepted + 2 would_block, rejections surface first in submission
order), S3 rejected-vs-accepted-error origin distinction, S4 reap order
2,0,1 preserved, S5 mixed kinds, S6 zero members (idle + context-global
outstanding behavior recorded), S7 N=1, S8 C−1/C/C+1 exact rejection
shape, S10 per-member terminal independence — PASS.

## 3. Machine-readable verdicts (results/batch-x0-perf-native-2/verdicts.json)

```text
BATCH-X0-SEMANTIC-GRANT          CURRENT BATCH DOES NOT GRANT GROUP ADMISSION
BATCH-X0-TRANSPORT-AMORTIZATION  ALREADY OBTAINED BY PRIMITIVE SUBMITS
BATCH-X0-CONTROL-AMORTIZATION    NOT MATERIAL
BATCH-X0-PERFORMANCE             NOT MATERIAL
BATCH-X0-MINIMALITY              THIN FLOOR SUFFICIENT (MB region 310 ≤ 480
                                 lines; no framework tokens; M8 word-boundary)
BATCH-X0-SLUICE-SPECIFIC-VALUE   PORTABLE THIN-BASELINE VALUE ONLY
BATCH-X0-G1-CONTROL              NOT ESTABLISHED
PROMOTION                        STOP — NO C1
```

Topology table (per-op submit calls / SQEs / kernel enters / admission
sections / reap episodes), from rows + counters:

```text
arm  submit calls  SQEs  enters per round  admission sections  reap episodes
B0   1/round       N     1                 n/a (no arena)      1/round
B1   N/round       N     ~1 (flush+wait at first drive)  2N (ctx+backend outer)  ≤N
B2   N/round       N     ~1 (identical to B1)            2N (identical; Batch adds wrapper scans/allocs)
MB1  N/round       N     ~1                 2N                 ≤N
MB3  1/round       N     ~1                 1                  ≤N
semantic differences: only MB3's admission section count differs; S9 proves
that difference changes accepted membership under capacity pressure.
```

## 4. What was falsified / what remains unknown

Falsified:

```text
F-A  "explicit Batch buys kernel-transport batching that primitive
     submits do not already get" — false on the as-built backend
     (B1 ≈ B2 ≈ 1 enter/round at every N; audit F5 confirmed).
F-B  "fusing admission/dispatch control work is a semantics-preserving
     optimization opportunity" — false (S9 DIVERGENCE + frozen
     disposition), and independently immaterial (MB3−MB1 ≈ 0).
F-C  "the Batch wrapper currently adds measurable value over the
     primitive loop" — not established; it is a wash that trends
     negative with N at 4K reads.
F-D  "an explicit group boundary would be the first Candidate whose
     value is not a Linux capability or portable wrapper discipline"
     — not earned: the only unambiguous winner is the raw liburing
     loop (B0), i.e. a portable mechanism, and the semantic grant
     itself does not exist.
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
     topology — bounded by this host/kernel (7.1.9) and liburing 2.13.
U-5  The wrapper's negative trend at 4K/N≥16 is an implementation
     observation (W1-W5), not a optimization mandate; no change is
     proposed in this research-only campaign.
```

## 5. Adversarial self-review (goal §33)

```text
1  Does current primitive submission already batch SQ transport? YES —
   measured (Q1; audit F5 predicted it structurally).
2  What exactly does explicit Batch add today? Orchestration only:
   add/await_one/next with reap-order iteration, origin bookkeeping,
   wrapper allocs/scans. Zero transport delta (B1 vs B2 enters 0.26%).
3  Is kernel-enter reduction different between B1 and B2? NO (19488 vs
   19539 over the same grid).
4  Which work is genuinely per-op semantic floor? The admission ladder
   (arena reserve/prepare/bind/commit/enqueue/mark_running), the accept
   CAS/release-store, cookie/SQE identity install. Present in EVERY
   arm incl. B3/MB3.
5  Which work is merely current implementation repetition? The outer
   access_mtx_/admission critical sections (2 per op) — and MB3 shows
   amortizing them is worth ≈ 0 AND is not semantically legal (S9).
6  Does the proposed fusion change admission interleaving? YES — the
   S9 DIVERGENCE witness is exactly this.
7  Under capacity pressure, does the same set of requests cross accept?
   NO — B crosses accept per-op and is rejected fused.
8  Are submit rejection and accepted error still distinguishable? YES
   (S3; origin orthogonal to error, pinned by fixtures + regression).
9  Does result iteration preserve true reap order? YES (S4 2,0,1;
   M3 rejects the submission-order mutant).
10 Does cancellation remain per-operation? YES (S10; no group-cancel
   API exists — audit F3; M5 rejects the collapse mutant).
11 Did we create a Batch-level lifecycle authority? NO — MB3 is a
   research-only instrument, uninstalled, never a vtable entry; no
   BatchRequest/OperationStorage exists anywhere in the diff (M7/M8).
12 Could a 50-line standalone helper obtain the same result? The
   RESULT (transport batching) is obtained by B0 — a ~60-line raw
   liburing loop — with better latency than every Sluice arm.
13 Is benefit caused by explicit semantic information or a better
   loop? There is no benefit to attribute; the better loop is B0.
14 Are we hiding a RequestArena redesign inside a benchmark? NO — MB
   arms call the PRODUCTION detail::submit_transaction + RequestArena
   ladder per op; identity witnesses (slot+generation per member) are
   in every MB row (validator M7 enforces this).
15 Would we still recommend the semantic change if the gain were zero?
   There is no semantic change to recommend; the grant does not exist.
16 Would we recommend the implementation if the gain were 2× but the
   semantics changed? NO — accepted-membership is observable contract
   surface; goal §6 forbids collapsing it without a separate earned
   contract (goal §9 keeps that OUT of X0).
17 Are we willing to close with NO C1? YES — that is the verdict.
```

## 6. Validation / CI / worktree

```text
bench selftest:            M1-M5 REJECT + M4 sanity (exit 0)
validator self-test:       4/4 corruptions rejected
semantic fixtures:         S1-S10 PASS, S9 DIVERGENCE recorded
A/A gate:                  FAIL (strace-wrapped, ×2) → Amendment 1 →
                           PASS 97.9% (47/48), median 0.95%
formal matrix:             840 rows, grid complete, all work witnesses ok
mechanical verdicts:       validator VALIDATION PASS
local mechanical gates:    pre-push.sh --range (doc links, mechanical
                           facts, claim/assert hygiene, diff --check) run
                           before push; CI = the remote authority
worktree:                  clean after the results commit; branch
                           research/g1-control-batch-x0 (Draft PR)
```

Environment: native Fedora host, kernel 7.1.9, 20 CPUs, clang release,
liburing 2.13, CPU pinned (cpu 2), DRAM-less NVMe + tmpfs work files,
commit-pinned, binary sha256 recorded per session.
