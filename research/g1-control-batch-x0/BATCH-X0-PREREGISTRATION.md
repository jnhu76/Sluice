# BATCH-X0-PREREGISTRATION — frozen before any formal evidence

- Campaign: BATCH-X0 (G1-Control Candidate 3) under #227 (execution
  authority) / #221 (evidence ledger) / #259 (subordinate roadmap).
- Status at freeze: audit complete (`BATCH-X0-AUDIT.md`); NO harness code, NO
  probe result, NO measurement exists at this commit.
- Freeze rule: this document is frozen at its commit hash (recorded below).
  Any later change is an additive, disclosed amendment; never silent.
- Precedents consumed: #279/PR #280 (G1-Control C0, fixed-file — STOP,
  product-capability-only), PR #281 (COPY-X0 — STOP, NO C1; A/A-gate
  BLOCKED lesson). The combined lesson this campaign attacks: semantic
  discipline increasingly supported; runtime architecture expansion
  repeatedly NOT earned.

## FREEZE COMMIT

```text
(recorded by the freeze commit itself; formal evidence sessions must record
HEAD == this commit's descendant with dirty_tracked=false)
```

---

# §1 Research question

> **Can explicit Batch semantics legally amortize control-plane and/or
> transport work across N operations, while each operation retains exactly
> the same Request identity, acceptance origin, terminal arbitration,
> Completion publication, cancellation semantics, and error result?**

Working thesis T-BATCH-X0:

```text
explicit Batch boundary + bounded known operation set + frozen per-op
lifecycle semantics ⇒ some execution/control work may be amortized without
creating group-level correctness authority, while producing material benefit.
```

Decisive distinction (frozen): GROUP EXECUTION AUTHORITY is potentially
legal; GROUP CORRECTNESS AUTHORITY is NOT automatically granted. Batch may
amortize mechanics; it may not collapse per-operation correctness identity.

NOT the question: "can one lock be acquired fewer times" (§28). The semantic
questions — which transitions commute, which must stay independently
linearizable, what grouping authority the caller granted — come first.

# §2 Arms (frozen ladder)

| Arm | Definition | Substrate |
|---|---|---|
| B0 | competent raw liburing loop: N × `io_uring_get_sqe`+prep, ONE `io_uring_submit`, reap via `io_uring_peek_batch_cqe`/`io_uring_submit_and_wait`; per-op identity = distinct user_data cookie → per-op result slot | raw `<liburing.h>` in bench |
| B1 | N × `AsyncIoContext::submit_read/write` + manual `wait_one()` drive + direct Completion reads. No `Batch`. | PRODUCTION `sluice_async` + `UringAsyncBackend`, untouched |
| B2 | production `sluice::async::Batch`: `add` × N, `await_one`, `next` × N | PRODUCTION, untouched |
| MB1 | mini research backend (below), PER-OP submit entry: N admission sections, flush at drive (production-shape semantics in the mini substrate) | research-only, in bench |
| MB3 | the SAME mini backend, BATCH entry: ONE admission section for the whole batch, N full per-op admission ladders inside it, ONE SQE-install episode, ONE flush, per-op RequestArena identity fully preserved | research-only, in bench |

Causal contrasts (frozen):

```text
B2 − B1        wrapper cost (Q2)
MB3 − MB1      control-amortization value WITHIN one substrate (Q3)
B1 − MB1       mini-substrate competence anchor (MB1 must not be
               dramatically cheaper than B1; a large gap invalidates the
               MB3−MB1 attribution and is a STOP condition S-9)
B1 vs B0       Sluice per-op semantic floor vs raw mechanism (Q4)
B2 vs B1       transport topology counters (Q1)
```

## MB mini-backend shape (the "benchmark-private semantic floor")

- subclasses `AsyncBackend` (the public extension point); publication only
  through the protected publication helpers; reap-only publication.
- per op, the FULL production-equivalent ladder via the installed
  `detail::submit_transaction` + `detail::RequestArena`: reserve → validate
  → prepare → install_publication_binding → begin_binding CAS → commit →
  install/commit binding. Per-op RequestKey/generation/Completion identity,
  arena capacity accounting, and would_block refusal are ALL preserved
  (M7 guard). No cancel, no wait-source, no close_admission, no dispatch
  ring/router/ledger: single-driver research floor, quiescent destruction.
- MB3 differences from MB1, EXACTLY: (a) one admission critical section
  around the N ladders, (b) one SQE-install + flush episode for the batch,
  (c) one reap episode. Counters: admission_sections, flushes.
- budget: the MB backend + arms must stay ≤ ~400 lines; no registry,
  planner, OperationStorage framework, or backend hierarchy (M8 guard). If
  it cannot be written that thin, STOP (thesis not earnable by a thin floor).

B3 naming note: the goal text's "B3" is instantiated here as the MB1/MB3
pair so that the fused floor never gets credited with substrate laxity. All
four goal-level arms exist: B0, B1, B2, and B3 = MB3 (judged only against
MB1).

# §3 Frozen semantic floor (MUST-MATCH for every perf arm)

| # | Dimension | Rule |
|---|---|---|
| 1 | per-op bytes transferred exact (READ: full buffer filled with the op's unique pattern; WRITE: file word-sum verified after last rep) | MUST MATCH; per-run witness |
| 2 | per-op result identity: arm returns N results, each attributable to its op index | MUST MATCH |
| 3 | same work per run: identical ops, offsets, sizes, count across arms in a cell | MUST MATCH (fail-closed same-work accounting) |
| 4 | capacity pressure reported as `would_block` per-op; partial batch admission legal | MUST MATCH (S2/S8) |
| 5 | origin (rejected vs accepted_and_completed) distinguishable in every arm that surfaces results | MUST MATCH (B2; MB arms expose origin too) |
| 6 | durability: NO arm syncs; no durability credit | witness: zero sync-class syscalls |
| 7 | interleaving: arms do NOT get to assume contiguous admission | frozen disposition §6 |

Declared differences (RECORDED, not adjudicated): B0 has no arena/capacity
admission (raw ring depth is its only bound — B0 is a mechanism floor, never
a semantic comparator); MB arms lack production dispatch/router/ledger
machinery (they are attribution instruments, never production evidence).

# §4 Fixtures (formal semantic suite; bench `semantic` mode)

Existing regression coverage (batch_test / batch_reap_order_test /
batch_result_origin_test) already pins: basic iteration, mixed kinds, reap
orders (single/reverse/forward/mixed/multi-per-wait), next-before-ready,
exactly-once, submit-failure-first, wait-error propagation + poppable ready
slots, origin on success/error/cancel. BATCH-X0 formal fixtures:

```text
S1  all succeed (N independent positional reads, unique patterns, exact)
S2  mixed success + submit rejection on the REAL arena backend: batch size
    > available request capacity; record exactly which members rejected
    (would_block) vs accepted_and_completed; origins verified
S3  accepted operation returns I/O error; distinguishable from submit
    rejection (origin asserted, not inferred from success/error)
S4  out-of-order completion: scripted backend reaps 2,0,1 for submit 0,1,2;
    Batch::next() must yield 2,0,1
S5  mixed-kinds batch (read + write + sync_data/sync_all via scripted
    backend): iteration order + origins across kinds
S6  zero members: audited as-built behavior (idle ctx: await_one == 0,
    next() == nullopt) — recorded, not invented
S7  one member: batch advantage must vanish (shape check vs N=2 cell)
S8  capacity boundary on the REAL arena backend: N = C-1, C, C+1 with clean
    capacity state; rejection shape recorded
S9  INTERLEAVING WITNESS (decisive): on the MB substrate with capacity C=3,
    per-op shape: submit A1,A2,B1,A3,A4 (external op injected between
    members — deterministic, we own the call sequence) → record accepted
    set. Then MB3 fused shape under the same capacity → record accepted
    set. If the accepted sets differ, the fused admission is OBSERVABLY
    different from per-op admission: B3 IS NOT SEMANTICALLY EQUIVALENT to
    the current per-op contract. (The current production Batch cannot have
    an external submit injected INSIDE await_one; its interleavability is
    established by F2/F4 — it is a driver over independently-linearizable
    per-op submits — and by S2/S8's real-backend rejection shape.)
S10 cancellation race: scripted backend; member completes before cancel /
    member cancel wins / submit-rejected member; per-member terminals
    preserved; no group-cancel shortcut exists (audit F3)
```

Fixture FAIL of S4/S5/S10 on production B2 = harness bug (production is
pinned by regression tests); fixture FAIL of S9 equivalence on the MB pair
= the expected falsification evidence, not a harness bug.

# §5 Mutants (validator self-tests; each must REJECT)

```text
M1  batch-level terminal collapse (one member completes ⇒ whole batch
    ready) → iteration/count/order checks reject
M2  origin collapse (submit rejection == accepted I/O error) → origin
    assertions reject
M3  submission-order iteration (index order instead of reap order) → S4
    permutation rejects
M4  hidden atomic admission (fused path claims semantic equivalence) → S9
    accepted-set comparison rejects the equivalence claim
M5  group cancellation collapse (one cancel ⇒ all canceled) → S10 rejects
M6  fake batching (batching claimed while measured enters == N per N ops)
    → validator compares claimed vs measured enter counts per arm row
M7  correctness bypass → design gate: MB3 rows must carry per-op arena
    identity witnesses (slot+generation observation per member) proving
    RequestKey/generation/publication/accounting active; a fused row
    without witnesses is rejected by the validator
M8  framework inflation → design gate: validator line/structure budget on
    the MB/B3 region (≤ ~400 lines; forbidden tokens: OperationStorage,
    BatchBackend hierarchy, BatchPlanner, CapabilityRegistry, BulkRequest)
```

# §6 FROZEN interleaving / admission disposition

Decided BEFORE any B3/MB3 formal execution (goal §21 critical rule):

```text
1. The current Batch contract does NOT grant group/contiguous/atomic
   admission (audit F2; batch.hpp documents a driver over per-op submit_*).
2. Therefore fused admission is NOT a legal optimization of the CURRENT
   Batch semantics. This holds REGARDLESS of what MB3 measures.
3. S9 exists to DEMONSTRATE the observable consequence (accepted-membership
   change under capacity pressure), converting the code reading into
   evidence; it cannot flip verdict 2 — only an audit-revealed contract
   grant could, and none exists at baseline.
4. If S9 showed NO membership difference, that would be evidence the fusion
   is observably equivalent under the tested schedules; the verdict
   vocabulary's "GROUP EXECUTION GRANT SUPPORTED" additionally requires the
   CONTRACT to name the grant, which it does not — so even then the
   SEMANTIC-GRANT verdict caps at SEMANTICALLY AMBIGUOUS — BLOCKED for
   production purposes, with the demonstration recorded.
5. All-or-nothing batch admission is a DIFFERENT future candidate, never an
   optimization finding of BATCH-X0 (goal §9).
```

# §7 Performance matrix (small first; 4K/64K primary)

```text
operation:  READ (primary), WRITE (control)
size:       4 KiB, 64 KiB
batch N:    1, 2, 4, 8, 16, 32
capacity:   request_capacity = queue_depth = 64 (admits N=32 with
            headroom; capacity-pressure semantics live in S2/S8, not perf)
backend:    real io_uring (UringAsyncBackend for B1/B2; raw ring for B0;
            raw ring for MB arms)
arms:       B0, B1, B2, MB1, MB3
cells:      2 ops × 2 sizes × 6 N × 5 arms = 120 cells
reps:       7 per cell, median + IQR; median is the cell statistic
work:       per rep: R rounds; R sized so the rep wall is ≥ 50 ms
            (R = ceil(2000/N) ops submitted+reaped per rep episode loop)
file:       one pre-created 8 MiB file per op class; reads at rotating
            offsets; writes to a dedicated scratch file, word-sum checked
            after the LAST rep of each WRITE cell
```

ThreadPoolBackend control arm: NOT run by default (goal §17: do not
automatically double the experiment). It may be added only if Q1/Q2 leave
wrapper-vs-transport ambiguous AND a disclosed amendment freezes it first.

# §8 Metrics (per row)

```text
wall/op ns (median, p25/p75)   — primary
cpu/op ns                       — CLOCK_PROCESS_CPUTIME_ID deltas
ops/sec
submit_calls                    — bench-counted (ctx.submit_* for B1/B2;
                                  submit invocations for B0/MB arms)
drive_episodes                  — bench-counted wait_one/poll/enter-wait
                                  calls per rep
kernel_enters                   — strace -c -e trace=io_uring_enter,
                                  io_uring_enter2 (external counter; valid
                                  for B0/B1/B2/MB alike)
admission_sections              — MB arms internal counters; B1/B2
                                  statically N per episode (source fact
                                  F4; not instrumented in production)
flushes                         — MB arms internal; B1/B2 = drive-side
                                  flush model from audit F5, verified via
                                  kernel_enters ratio
Batch wrapper scans/allocs      — NOT instrumented (W1-W5 are static
                                  source facts; B2−B1 wall delta is the
                                  measured wrapper cost)
```

Counters are research/bench-only; no production file changes.

# §9 Measurement validity (COPY-X0 lessons applied)

```text
host:      native Fedora host (kernel 7.1.9, the RE-work host; WSL2 NOT
           used — COPY-X0 lesson)
A/A gate:  BEFORE formal evidence, two consecutive full-matrix passes of
           B1 and B2 (A/A); per matched cell, |median1 − median2| /
           min(median) ≤ 5% required on ≥ 90% of cells; failure ⇒
           CAPABILITY = BLOCKED, stop (no noisy-run shopping, no retry
           inflation)
pinning:   CPU affinity via sched_setaffinity inside the bench (one pinned
           core per run) + nice; environment.json records cpu model,
           governor, load, kernel, commit, binary sha256
substrate: one pre-created regular file on the host filesystem (ext4),
           page-cache warm; OS page cache state identical across arms by
           construction (same file, interleaved arm order per rep block)
ordering:  per rep block, cells run in a fixed seeded order that
           interleaves arms within each (op,size,N) cell — arm comparison
           is same-session, minutes apart, never across sessions
commit pin: dirty_tracked=false; binary hash recorded per run
gates:     no formal perf row may postdate the freeze commit; semantic
           fixtures may postdate it only in the disclosed execution order
           (§12)
```

# §10 Materiality + verdict rules (frozen)

Materiality: an arm delta is MATERIAL iff, for BOTH op classes, median
delta ≥ +5% in the claimed direction on ≥ 4 of 6 N-cells AND the sign is
consistent across both sizes. Anything else is NOT MATERIAL.

Verdicts (mechanically derived by the validator from rows):

```text
BATCH-X0-SEMANTIC-GRANT ∈ {
  GROUP EXECUTION GRANT SUPPORTED          (needs contract text naming the
                                            grant — none exists ⇒ unreachable
                                            at baseline),
  CURRENT BATCH DOES NOT GRANT GROUP ADMISSION (S9 membership differs or
                                            contract reading + S2/S8 shape),
  SEMANTICALLY AMBIGUOUS — BLOCKED         (S9 shows no difference but no
                                            contract grant either),
  NOT SUPPORTED }
BATCH-X0-TRANSPORT-AMORTIZATION ∈ { ADDITIONAL TRANSPORT AMORTIZATION
  ESTABLISHED | ALREADY OBTAINED BY PRIMITIVE SUBMITS | NOT MATERIAL |
  BLOCKED }
BATCH-X0-CONTROL-AMORTIZATION ∈ { MATERIAL AMORTIZATION LEGALLY AVAILABLE |
  COST EXISTS BUT FUSION NOT SEMANTICALLY LEGAL | NOT MATERIAL | BLOCKED }
BATCH-X0-PERFORMANCE ∈ { MATERIAL | NOT MATERIAL | BLOCKED }
BATCH-X0-MINIMALITY ∈ { THIN FLOOR SUFFICIENT | FRAMEWORK EARNED | BLOCKED }
BATCH-X0-SLUICE-SPECIFIC-VALUE ∈ { ESTABLISHED | PORTABLE THIN-BASELINE
  VALUE ONLY | NOT ESTABLISHED }
BATCH-X0-G1-CONTROL ∈ { POSITIVE CANDIDATE | NOT ESTABLISHED }
PROMOTION ∈ { PROMOTE-CONSIDER | STOP — NO C1 }
```

Mechanical derivation (frozen):

```text
TRANSPORT: B1 vs B2 kernel_enters/ops identical (±1 episode boundary) and
  B0/B1 enter ratio ≈ 1/N already ⇒ ALREADY OBTAINED BY PRIMITIVE SUBMITS.
  B2 materially fewer enters than B1 ⇒ ESTABLISHED (not expected).
CONTROL: MB3−MB1 wall/op ≥ +5% MATERIAL on both classes ⇒ COST EXISTS;
  legality from SEMANTIC-GRANT: grant absent ⇒ FUSION NOT SEMANTICALLY
  LEGAL. MB3−MB1 < 5% ⇒ NOT MATERIAL.
PERFORMANCE: best legal arm (B2 or B1) vs B1 — a wrapper cannot beat its
  own substrate; materiality judged on MB3−MB1 for the fusion question and
  B2−B1 for the wrapper question; PERFORMANCE = MATERIAL only if a LEGAL
  (semantically clean) arm shows a MATERIAL gain over B1.
SLUICE-SPECIFIC-VALUE: ESTABLISHED requires a legal, material gain that B0
  cannot reproduce without reconstructing the explicit semantic
  information; B0 already achieving the topology ⇒ PORTABLE THIN-BASELINE
  VALUE ONLY or NOT ESTABLISHED.
G1-CONTROL: POSITIVE CANDIDATE requires ALL of: legal (grant present or
  unchanged observables), MATERIAL, thin-floor-sufficient, and not
  obtainable by a competent thin baseline. Otherwise NOT ESTABLISHED.
PROMOTION: PROMOTE-CONSIDER only with G1-CONTROL positive; else STOP.
```

Hard stop conditions (goal §27) are adopted verbatim; additionally S-9
(substrate incompetence): if |MB1 − B1|/B1 > 30% on the 4K/N=8 cell, the MB
substrate is judged not comparable and MB3−MB1 is inadmissible as evidence
(BLOCKED control verdict).

# §11 Validator

`scripts/validate_batch_x0.py` (research scripts dir):

```text
- parses results/<run-id>/rows.jsonl (one row per arm×op×size×N×rep)
- re-derives per-cell medians/IQRs and ALL §10 verdicts mechanically
- re-checks the A/A gate record, same-work witnesses (bytes, patterns,
  word-sums), identity witnesses (M7), enter-count claims (M6)
- runs the M1-M8 mutant self-test record and requires all REJECT
- refuses rows whose commit != pinned freeze descendant or with
  dirty_tracked=true
- emits verdicts.json + a one-line PASS/FAIL; self-test mode validates
  itself against a deliberately corrupted fixture copy
```

# §12 Execution sequence (frozen)

```text
0  sync master, baseline record                      (done pre-freeze)
1  audit                                             (done, pre-freeze)
2  semantic table                                    (this document)
3  arms + mutants design                             (this document)
4  freeze commit                                     (this commit)
—  STOP CHECK: no formal evidence predates the freeze —
5  implement bench (semantic+perf modes) + validator
6  mutant self-tests (M1-M8 all REJECT)
7  formal semantic fixtures incl. S9
   STOP if S9 unexpectedly contradicts the frozen disposition §6
8  host qualification (A/A gate §9)
9  formal performance matrix
10 mechanical verdicts
11 adversarial self-review (goal §33 questions answered in report)
12 report + Draft PR (RESEARCH ONLY; production untouched; no C1)
```

# §13 Adversarial pre-commitments

Recorded now, answered again in the report (goal §33):

```text
- We pre-commit that "fewer enters for B2 than B1" is NOT expected and its
  observation would trigger a harness audit before any claim (audit F5).
- We pre-commit that a MATERIAL MB3 gain does NOT justify production fusion
  while §6 stands; it may only justify a SEPARATE future contract proposal.
- We pre-commit to accepting STOP — NO C1 as a first-class outcome.
- We pre-commit to disclosing any harness defect found AFTER formal rows as
  a superseded run, never as a silent re-run (COPY-X0 discipline).
```

---

## Amendment 1 (pre-formal, disclosed) — strace removed from the TIMED window

Two A/A qualification attempts with the timing window wrapped in
`strace -c` FAILED the frozen gate (66.7% and 62.5% of cells within 5%;
violators concentrated in 4K WRITE and 64K cells, max ratio 29%). A
strace-free A/A rerun PASSED: 47/48 cells (97.9%) within 5%, median
ratio 0.95% (only violator: B1/read/4K/N=1, the highest-variance
single-op cell). ptrace adds two context switches per syscall; on this
DRAM-less NVMe host that amplifies run-to-run variance beyond the frozen
gate.

Decision (harness repair, NOT a gate change): formal timed rows are
collected WITHOUT ptrace; `strace -c -e trace=io_uring_enter` remains the
M6 enter COUNTER, collected in a SEPARATE untimed 1-rep pass per
arm×cell. The A/A gate, the 5%/90% thresholds, the matrix, and every
verdict rule are unchanged. Evidence: results/batch-x0-qualify-native-1
(strace-wrapped, FAIL) and results/batch-x0-qualify-native-2 (strace-free,
PASS).
