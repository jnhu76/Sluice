# G1-CONTROL-C0 REPORT — fixed-file resource identity and specialization
# falsification (#279)

Status: **COMPLETE — CORRECTIVE-2 APPLIED**. Frozen preregistration
executed in full. Corrective-1 repaired three P1-class evidence-shape
defects; the Corrective-2 review then found the frozen tmpfs primary had
still never been executed (both filesystem labels resolved to btrfs) and
that the §13 neighbor rule's tooling wiring dropped the 64 KiB / btrfs
neighbors. Corrective-2 re-executed exactly the missing tmpfs half of
the frozen matrix on REAL tmpfs behind a fail-closed substrate gate,
fixed the neighbor wiring in driver and validator (with discriminating
self-test mutants), required a clean commit-pinned execution for the
corrective session, and re-derived every verdict from raw evidence. The
main verdict is UNCHANGED by the corrected evidence. RESEARCH ONLY:
production source changes ZERO, public API changes ZERO, C1 NOT entered.

```text
VERDICT: C-PARTIAL (PRODUCT CAPABILITY ONLY — G1-CONTROL NOT PROVEN)
```

## Revision

```text
BASE:          39f9d984e562a6396b58ebbe733d89513dd7242a (origin/master at start)
FREEZE:        f122c636e6d146a86cf938520e942ba9976f733f (preregistration
               commit, BEFORE any formal evidence)
prereg blob:   1a67b3174d1046d579a2f00171b64de2aaef79d7
pre-corrective HEAD: 3d193599c2b58cba62acc960fa9310b9c906659f (evidence +
               adjudication; the revision Corrective-1 repairs)
Corrective-1 HEAD:   56dc4274 (report; the revision Corrective-2 repairs)
Corrective-2 tooling: e00acda2 (substrate gate, neighbor rule, 3-session
               composite) + 45f1ff70 (create fs roots before substrate
               resolution)
native-3 execution HEAD: 45f1ff7054e8b1f146bef7af59b80b25b3f844ef
               (tracked-clean at generate time, dirty_tracked=false)
branch:        research/g1-control-c0-fixed-file
prereg status: FROZEN text unchanged; Amendments 1-5 appended to §16
               (Corrective-1: 1-4, Corrective-2: 5; additive only)
```

## Production scope

```text
production source changed: NO
public API changed:        NO
C1 entered:                NO
```

Touched: `bench/g1_control_c0_bench.cpp` (research-only direct-liburing
bench), one gated target in `xmake/benchmarks.lua` (absent without
`--with-liburing`, matching every other research bench),
`research/g1-control-c0/**` (audit, frozen preregistration + additive
amendments, campaign.json, driver, validators, THREE immutable evidence
sessions, composite summary, this report). No production target gains a
dependency; default builds and CI stay liburing-free.

## Mechanism audit

Full evidence: `G1-CONTROL-C0-AUDIT.md` (exact-kernel source citations;
fetched-source provenance + hashes in `probes/kernel-7.1.9/MANIFEST.txt`).

```text
ordinary fd resource path:  fget(fd) -> RCU fdtable walk + atomic
  file_ref_get + double-check (fs/file.c:1111, :1018). No files lock in the
  steady-state path; lookup shape identical for 1 vs N threads.

fixed-file resource path:   IORING_OP_READ/WRITE + IOSQE_FIXED_FILE +
  sqe->fd = SLOT (NOT READ_FIXED — that is the fixed-BUFFER mechanism).
  io_file_get_fixed (io_uring.c:1575): direct array index into
  ctx->file_table.data.nodes[slot] (io_rsrc_node_lookup, rsrc.h), node->refs++,
  req->file_node = node. No fdtable walk, no RCU.

update semantics:           __io_sqe_files_update (rsrc.c): table entry swap
  via io_reset_rsrc_node — drops ONLY the table's node ref; the old
  struct file * stays alive via request-side retention (node->refs held by
  each bound request; io_req_put_rsrc_nodes io_uring.c:1089; fput at
  refs==0, rsrc.c:495).

kernel lifetime conclusion: Linux owns the physical resource lifetime of
  already-bound requests. A slot update A->B cannot yank an in-flight
  request off A; new (unbound) requests resolve the slot at issue time and
  see B. SOURCE-SUPPORTED, VERSION-BOUND (v7.1.9). NOT an executed
  witness: the executed BOUNDARY-D step updated the slot only after the
  CQE reap and was reclassified as a POST-COMPLETION UPDATE CONTROL by
  Corrective-1 (Amendment-2) — no in-flight overlap was exercised.

logical identity conclusion: the kernel exposes only an integer slot;
  replacement legality, stale-handle semantics, and quiescence are NOT
  kernel-enforced (resource tags are retirement NOTIFICATIONS — aux CQE at
  node retirement, rsrc.c:501 — they reject nothing and are NOT a
  generation check). Sluice's logical binding meaning is a real ownership
  boundary — but expressible with L0/L1 discipline, without new machinery.

binding linearization (version-bound, kernel 7.1.9): file binding is LAZY —
  io_init_req snapshots (flags, slot) at SQE consumption; io_assign_file
  resolves the table at ISSUE time (inline or io-wq). Two evidence layers,
  kept separate per Corrective-1 (Amendment-4):
    executed fact:   a prepared-but-unsubmitted SQE observed the POST-update
                     binding (BOUNDARY-A witness) — PRE-SUBMISSION FIXED
                     BINDING NOT FROZEN;
    source-derived:  the binding linearization point is the issue path
                     (io_assign_file / io_file_get_fixed) —
                     SOURCE-SUPPORTED, VERSION-BOUND, not executed.
  The validation -> submission -> binding window is real and is closed by
  L1 quiescent replacement, not by the kernel.

candidate authority split ("Sluice owns logical binding meaning; Linux owns
  physical kernel-resource lifetime"): PARTIALLY_SUPPORTED —
  Linux-owns-physical-lifetime: SUPPORTED (source).
  Sluice-owns-logical-meaning: SUPPORTED as a contract, but the implication
  that Sluice needs NEW lifetime machinery is NOT_SUPPORTED (L0/L1
  discipline sufficient; see Minimality).
```

## C0-PERF

Evidence composition (Corrective-2, fail-closed with provenance in
`composite-summary.json`, 3-session substrate-authoritative — a session
contributes only filesystem labels whose env-resolved fstype equals the
canonical fstype of the label):

```text
C0-PERF primary (campaign verdicts):
  tmpfs cells (ALL frozen primaries, real tmpfs) from
  g1-control-c0-native-3-tmpfs-corrective (280 runs = 2 ops x 5 cells x
  4 arms x 7; the F0/F1 subset carries the campaign verdict)
  btrfs F0/F1 cells from g1-control-c0-native-1 (140 runs)
C0-PERF threaded (EXPLORATORY ONLY):
  tmpfs F0-T/F1-T from native-3; btrfs F0-T/F1-T from
  g1-control-c0-native-2-threaded-corrective (140 runs, deterministic
  park/release gate)
SUPERSEDED — WRONG SUBSTRATE (retained byte-identical, excluded from
  every derived number): native-1 tmpfs-LABEL rows (280) and native-2
  tmpfs-LABEL rows (140) — both labels had resolved to btrfs
  (Amendment-5). native-1 threaded rows (280) additionally SUPERSEDED
  by the §5 violation (Amendment-1).
Valid frozen composite runs: 560 (140 + 140 + 280).
```

Session native-1: Q0 30/30 PASS, then 560 formal runs, 0 gate errors.
Corrective session native-2: Q0 re-qualification of the modified bench
30/30 PASS (F0 data path unchanged), then 280 threaded formal runs, 0
gate errors, every run carrying the corrective gate fields. Corrective
session native-3: clean commit-pinned at 45f1ff70 (dirty_tracked=false),
Q0 30/30 PASS ON REAL TMPFS, then 280 formal runs (tmpfs, all arms), 0
gate errors; the fail-closed substrate gate verified the tmpfs label
resolves to actual tmpfs before any fixture or run existed.

Primary metric: steady-state wall per op (the ONLY qualified metric that
includes kernel lookup cost — perf_event_paranoid=2 pins every perf event
to userspace; prereg §3). Frozen materiality: ratio >= 1.03 AND 1.5*MAD
separation, primary cells = 4 KiB family on tmpfs (now genuinely
executed there).

```text
READ  (FROZEN PRIMARY FAMILY — REAL TMPFS, native-3, single-thread,
       ns/op median ± MAD, F0 vs F1):
  4K d1  tmpfs:  10721.4 ± 433.2 vs 12479.1 ± 494.7 ratio 0.8591 F1_SLOWER
                  (material ISOLATED regression-shaped cell — the frozen
                   vocabulary has no isolated-regression verdict; recorded
                   as a per-cell observation)
  4K d8  tmpfs:   1591.4 ±  87.0 vs  1564.9 ±  37.6 ratio 1.0169  NONE
  4K d32 tmpfs:   1315.8 ±  47.6 vs  1398.6 ±  88.2 ratio 0.9408  NONE
  64K d1 tmpfs:  20626.9 ± 1200.6 vs 20887.5 ± 447.3 ratio 0.9875  NONE
  2M  d1 tmpfs: 359523   ± 14896 vs 313580  ± 5933  ratio 1.1465 F1_FASTER
                  (regime/control cell — NOT a primary, NOT a neighbor;
                   isolated observation only)
  (btrfs neighbors from native-1, single-substrate btrfs execution:
   4K d1 1.0336 F1_FASTER ISOLATED, 4K d8 1.0276 NONE, 4K d32 1.0309
   NONE, 64K d1 1.0061 NONE — note READ 4K d1 FLIPS DIRECTION between
   substrates, so neither cell can borrow the other as a supporting
   neighbor under the frozen rule)

WRITE (primary family + regime cells, real tmpfs): NONE (ratios
  0.9807-1.0118; btrfs cells equally direction-mixed, e.g. 4K d8 0.856,
  4K d32 1.104 — no robust separation anywhere on either substrate).

THREADED ARMS (EXPLORATORY — cannot carry a verdict, prereg §5/§13;
workers parked across the span, K=4):
  tmpfs (native-3): READ and WRITE all cells NONE — the Corrective-1
         REGIME-LOCAL btrfs cell (4K d8 1.0495) does NOT replicate on
         the preregistered substrate (tmpfs 4K d8 1.0284).
  btrfs (native-2): READ isolated material cell 4K d8 1.0495
         (REGIME-LOCAL within ONE substrate, exploratory); WRITE one
         isolated regression-shaped cell (4K d1 0.8981).
  THREADED-PROCESS ADVANTAGE: NOT ESTABLISHED (HOST-LOCAL; no material
  direction on either substrate).
```

```text
primary metric:      wall/op (median of 7 seeded-interleaved rounds/cell)
materiality:         frozen rule (1.03 + 1.5*MAD) — no single-thread
                     primary cell passes on EITHER substrate
neighbor support:    n/a — no primary-cell F1_FASTER direction to support
                     (Corrective-2 note: the neighbor rule TOOLING was
                     defective until Corrective-2 — the 64 KiB / btrfs
                     neighbor lookups silently resolved to None; since no
                     primary cell held a supporting direction, the fix
                     changes no verdict, and the mutants in --self-test
                     now prove the neighbors genuinely enter the path)
same-work:           PASS everywhere (exact CQE/byte accounting per op,
                     content spot check per run, dst sha256 == frozen
                     per-size pattern constant for every WRITE run, causal
                     fields gated: align_remainder==0, slot_stride==size,
                     registered_files==1 on F1 arms; threaded runs
                     additionally threads_spawned/io_ok/ready/released/
                     joined == 4 and thread_gate_ready /
                     thread_gate_release_after_transfer == true)
isolated cells:      READ 4K d1 tmpfs F1_SLOWER (0.8591, material) and
                     READ 4K d1 btrfs F1_FASTER (1.0336) — the direction
                     FLIPS between substrates; READ 2M tmpfs F1_FASTER
                     (1.1465, regime cell, not neighbor-eligible). Per
                     prereg §13.2: ISOLATED CELL ONLY, none promoted to
                     any verdict
registration cost:   register ~3.1 us, unregister ~3.3 us (single fd),
                     reported separately, never amortized into steady state
secondary (report-only, USERSPACE-ONLY, NOT kernel-lookup evidence):
                     instructions:u/op READ 4K d32 tmpfs (btrfs-label
                     session, historical): F0 167.6 vs F1 163.6 (-2.4%) —
                     direction-consistent with wall, but userspace-only
                     by construction
```

**Verdict (campaign, from the executed frozen tmpfs primaries + real
btrfs neighbors):** `FIXED-FILE PERFORMANCE BENEFIT NOT ESTABLISHED`
(READ and WRITE independently, frozen rule; re-derived from raw by the
Corrective-2 validator through the full neighbor rule and equal to the
stored verdicts). This is now the lawful closure of the frozen
tmpfs-primary campaign: the primary cells were executed on the
preregistered substrate (native-3), not inferred from a btrfs stand-in.
The earlier btrfs-substrate observation of a direction-consistent READ
4 KiB trend (1.6–3.6%) does NOT replicate on real tmpfs (d8 1.0169, d32
0.9408 direction-flipped) and the tmpfs d1 cell is materially
F1_SLOWER — recorded observations, each ISOLATED, none verdict-bearing.

## FILE-ID-E0

Deterministic witness (dup2-forced reuse; no sleep, no probabilistic
reuse). Raw: `results/g1-control-c0-native-1/raw/{fileid,replacement-window}.json`
(historical; D label superseded),
`results/g1-control-c0-native-2-threaded-corrective/raw/{probe,fileid,replacement-window}.json`
(corrected labels, re-executed by Corrective-1), and
`results/g1-control-c0-native-3-tmpfs-corrective/raw/{probe,fileid,replacement-window}.json`
(re-executed by Corrective-2, same corrected labels). Ordering guard
`check_g1_control_c0_probe_order.py` PASS (structural + executed; native-3).

```text
ordinary arm:   ORDINARY-FD WRONG-TARGET REPRODUCED
                (stale fd N, dup2-forced to B; io_uring read via N returned
                 B's marker, not A's)
fixed L0 arm:   FIXED L0 BINDING PRESERVED TARGET
                (A registered into slot S; process fd closed and dup2-forced
                 to B; IOSQE_FIXED_FILE read via slot S returned A's marker)
replacement:    REPLACEMENT HONORED GOING FORWARD
                (after files_update S <- B, fixed read via S returned B)
BOUNDARY-A:     WINDOW CONFIRMED — an SQE prepared BEFORE the slot update,
                submitted AFTER it, read the POST-update resource B
                (executed fact: PRE-SUBMISSION FIXED BINDING NOT FROZEN).
                That binding therefore happens at the kernel issue path is
                SOURCE-SUPPORTED, VERSION-BOUND (audit §6) — an inference
                from source, not an executed fact (Corrective-1
                Amendment-4).
POST-COMPLETION UPDATE CONTROL (was "BOUNDARY-D RETENTION CONFIRMED",
                WITHDRAWN): the executed topology updated the slot only
                AFTER the CQE reap — the request was complete before the
                update, so the step proves the update path works against a
                consumed request and NOTHING about in-flight retention
                (Corrective-1 P1-2 / Amendment-2). Already-bound-resource
                retention remains SOURCE-SUPPORTED, VERSION-BOUND
                (node->refs request-side retention; io_req_put_rsrc_nodes;
                fput at refs==0).
BOUNDARY-B/C:   NOT DETERMINISTICALLY OBSERVABLE from userspace (recorded;
                bounded source-based conclusion in the audit §6)
```

**Verdict (C0-IDENTITY):** `ORDINARY-FD WRONG-TARGET REPRODUCED` +
`FIXED L0 BINDING PRESERVED TARGET`. Ordinary process-fd reuse does NOT
change a frozen fixed-table binding identity; it DOES redirect ordinary-fd
I/O (the SE-2 H03 hazard shape, reproduced). The slot is NOT an eternal
identity — only a frozen binding's immunity to process-fd reuse was tested
and proven.

## Minimality

```text
current workload requires replacement: NO CURRENT REQUIREMENT
  (as-built: zero fixed-file support anywhere in production; DIV-09 defers
   registered files/buffers pending a lifetime contract; no roadmap item
   (#227/#259) requires runtime slot replacement; the only conceivable
   consumer is the capability this C0 was testing)

L0: SUFFICIENT — register once -> run -> quiesce -> unregister serves every
    current and roadmap target workload; no current workload needs runtime
    replacement (Q1/Q2).
L1: EXPRESSIBLE WITH EXISTING GLOBAL QUIESCENCE AUTHORITY IF ever needed —
    "replace only when no accepted request referencing the slot is
    outstanding" is mechanically expressible with EXISTING request
    lifecycle authority (request capacity, close admission, drain, reap:
    outstanding == 0), no new per-resource state. The BOUNDARY-A window is
    exactly what this discipline closes (Q3). This is a design-level
    expressibility judgment: the L1 replacement path was NOT formally
    executed, because runtime replacement has NO CURRENT REQUIREMENT.
L2: generation NOT REQUIRED — no current or target API permits a stale
    logical handle to survive replacement; the explicit-I/O contract can
    state "replacement invalidates all old handles AND replacement only
    under quiescence" (Q5). Kernel resource tags are retirement
    notifications, not generation (audit §8) and do not change this.

generation:          NOT EARNED
per-request live-use: NOT EARNED
  (admission test Q4: the window exists mechanically, but a LEGAL concurrent
   replace A->B alongside an outstanding request referencing the slot is a
   contract choice Sluice can simply forbid — and L0 forbids it wholesale,
   L1 forbids it via existing drain/reap authority. No witness exists that
   L0 AND L1 AND existing lifecycle authority all fail to express.)
UNRESOLVED: none
```

## Control tax

Steady-state I/O extra per op, candidate designs (mechanism budget, Q7):

```text
extra registry lookup:    ABSENT (L0: slot is encoded in the SQE fd field +
                          one flag bit; no userspace lookup exists)
extra lock:               ABSENT
extra allocation:         ABSENT
extra refcount:           ABSENT (the node refcount is kernel-side; Sluice
                          adds none)
extra generation check:   ABSENT (L0/L1; a generation compare would exist
                          only in L2, which is UNEARNED)
L1 replacement gate:      control-path only (bind/replace/remove/shutdown);
                          zero steady-state I/O tax
```

The target shape `explicit FixedFile -> slot encode -> IOSQE_FIXED_FILE ->
submit` is exactly what F1 implements: the F0->F1 userspace delta is one
flag bit and an integer substitution. If correctness can be established
once (at bind), it is not re-established on every I/O.

## Competent baseline

What raw liburing + existing discipline achieves — everything observed:

```text
F1 IS the competent baseline: a raw-liburing user calls
io_uring_register_files (one call, ~3.1 us), sets IOSQE_FIXED_FILE, and
obtains BOTH observed values:
  - the (sub-material) lookup cost trend, and
  - the identity hazard conversion (FILE-ID-E0 fixed arm) — the kernel
    fixed table, not any Sluice machinery, is what process-fd reuse cannot
    redirect.
L1 is mechanically expressible as a thin quiescence discipline using
existing global request/drain/reap authority (close admission -> drain to
outstanding==0 -> files_update). It was NOT formally executed as a
replacement baseline: runtime replacement has NO CURRENT REQUIREMENT, so
no executed competent-baseline comparison exists or is needed; this is an
expressibility judgment over existing authority, not a measured result.
=> NO EVIDENCE THAT NEW SLUICE-SPECIFIC LIFETIME MACHINERY IS REQUIRED.
   The value that exists is Linux's fixed-file capability plus the
   explicit-I/O surface convention.
```

## Stop-gate adjudication

```text
Gate A (capability value):  FAIL — no material benefit established under
  the frozen rule in any primary cell (campaign verdict above; the
  corrected threaded arms are exploratory and cannot carry a verdict).
Gate B (control value):     PARTIAL, NOT SLUICE-SPECIFIC — the explicit
  fixed binding genuinely converts the ordinary-fd stale-identity hazard
  into a bounded form (FILE-ID-E0), but the conversion is delivered by the
  kernel fixed table that ANY competent liburing user already gets from
  register_files; per Issue #279 §Gate-B guidance this is product
  capability, not G1-Control.
Gate C (unresolved user-space obligation): FAIL — no concrete
  logical-binding obligation was found that Linux does not already own AND
  L0/L1 cannot express (replacement legality = existing drain/reap;
  stale handles = contractually invalidated; no uncovered window witness).
```

Per the preregistered stop conditions: performance null AND control value
no better than a competent thin baseline, AND the workload fully served by
L0 → **STOP PROMOTION**.

## G1-Control interpretation

```text
genuinely Sluice-enabled:  the explicit-I/O convention only — a fixed-file
  operation SHOULD remain a distinct typed form (different correctness
  contract: binding identity vs process-fd identity), and replacement is a
  control-path operation gated on quiescence. Both are zero-tax API-shape
  statements, already covered by the design maxim "Lazy control, explicit
  I/O". Nothing measured here required Sluice machinery to be true.

merely Linux fixed-file capability: the lookup cost trend (kernel array
  index + node ref vs RCU fdtable walk), the identity hazard conversion
  (fixed table vs process fdtable), request-side retention across slot
  updates (node->refs), and retirement notification (tags). All obtained
  identically by a raw-liburing user.
```

## Negative results

Every falsified mechanism/thesis, explicitly:

1. **Fixed-file steady-state performance benefit: NOT ESTABLISHED**
   (frozen rule; READ and WRITE; single-thread primaries; 560 valid
   frozen runs across three sessions — the tmpfs primary family executed
   on REAL tmpfs by Corrective-2). The btrfs-substrate READ 4 KiB trend
   (1.6–3.6%) is recorded as a substrate-local observation, below the
   preregistered materiality bar, and does not replicate on tmpfs.
2. **Threaded-process advantage for fixed files: NOT ESTABLISHED**
   (HOST-LOCAL; corrected evidence, Corrective-1; substrate-completed,
   Corrective-2). Under the frozen §5 threaded condition (K=4 workers
   parked across the span), the ONE isolated material cell (4K d8,
   1.0495) existed only on the btrfs substrate and does NOT replicate on
   the preregistered tmpfs substrate (1.0284 NONE); no WRITE direction
   on either substrate. The "threads => fixed files win" folk claim
   remains falsified for this regime on this host, now on both
   substrates; the corrected data does not upgrade it (exploratory
   only, in every case).
3. **Per-request live-use / lease: NOT EARNED** — no witness exists where
   L0 AND L1 AND existing request lifecycle authority all fail.
4. **Generation (L2): NOT EARNED** — no requirement for a stale logical
   handle to survive replacement; replacement-under-quiescence +
   handle-invalidation covers it.
5. **Runtime slot replacement: NO CURRENT REQUIREMENT** — the L0 frozen
   model serves all current/roadmap workloads.
6. **Kernel resource tags as generation: FALSIFIED as a modeling idea** —
   tags are retirement notifications (aux CQE at node refcount==0); they
   validate nothing at submit time.
7. **"Sluice needs new lifetime machinery to own logical binding meaning":
   NOT SUPPORTED** — L0/L1 discipline over existing drain/reap authority is
   sufficient; the candidate authority split survives only in its weaker
   form (Linux owns physical lifetime; Sluice owns a CONTRACT, not new
   state).
8. **G1-Control thesis (T-C1): NOT PROVEN** — the capability and identity
   values that exist do not materially depend on explicit Sluice lifecycle
   control; a competent thin baseline obtains everything.
9. **In-flight retention executed witness: WITHDRAWN** (Corrective-1) —
   the original BOUNDARY-D step could not exercise overlap; retention is
   source-supported/version-bound only.

## Corrective-1 record

Four evidence-shape defects were found and corrected AFTER the
pre-corrective adjudication (3d193599). Frozen prereg text unchanged;
Amendments 1-4 appended to §16. No frozen materiality rule, matrix,
hypothesis, or threshold was modified. Native-1 raw evidence is preserved
byte-identical; its threaded subset is marked SUPERSEDED and is excluded
from every derived number by the fail-closed composite.

```text
P1-1 threaded prereg mismatch:
  before:  workers spawned, performed their setup I/O, and were JOINED
           before the measured span (violating frozen §5: park across the
           span); the comment claimed parking, the code did not.
  after:   deterministic mutex/condvar park-release gate (ready before
           span, release after span; no sleeps/yields/busy-wait); machine
           -readable causality fields gated by driver + validator.
  rerun:   exactly the affected cells — 280 threaded runs (2 ops x 5
           cells x 2 fs x 2 arms x 7 rounds), 0 gate errors, same seed
           plan, same frozen thresholds. Single-thread evidence UNAFFECTED
           (bench diff leaves the measured engine byte-identical; Q0
           30/30 re-passed on the modified binary).
  old evidence disposition: native-1 F0-T/F1-T = SUPERSEDED, retained
           byte-identical, excluded by the composite.
  result:  READ REGIME-LOCAL isolated material cell (4K d8 1.0495 —
           btrfs substrate, then mislabeled "tmpfs"; does NOT replicate
           on real tmpfs, see Corrective-2);
           WRITE none; THREADED-PROCESS ADVANTAGE NOT ESTABLISHED
           (HOST-LOCAL). Main verdict UNCHANGED.

P1-2 BOUNDARY-D executed witness invalid:
  before:  "BOUNDARY-D RETENTION CONFIRMED" claimed in-flight retention
           from a topology that updated the slot only AFTER the CQE reap.
  after:   executed claim WITHDRAWN; step reclassified as POST-COMPLETION
           UPDATE CONTROL (non-overlap); retention = SOURCE-SUPPORTED,
           VERSION-BOUND; no replacement experiment added; label synced in
           bench JSON, probe-order validator, prereg amendment, report,
           PR body.

P1-3 verdict validator did not re-derive the campaign verdict:
  before:  validator recomputed medians/MAD/ratios/directions but only
           checked the stored verdict against the frozen VOCABULARY — a
           wrong-but-well-spelled verdict would have passed.
  after:   validator independently re-derives READ/WRITE verdicts from
           raw cells (prereg §13/§13.1) and requires stored == derived
           in every mode (single-session, composite); --self-test proves
           both mutation directions (benefit erasure, benefit
           fabrication) are rejected. Execution: self-test PASS;
           single-session PASS (280 valid); composite PASS (840 valid
           runs); on native-1 the hardened validator fails BY DESIGN on
           exactly the 280 superseded threaded runs (no verdict/ratio
           mismatches — the stored single-thread verdicts were
           independently confirmed).
  Corrective-2 finding: the re-derivation's neighbor WIRING was still
           defective — both driver and validator filtered the direction
           set to the 4 KiB tmpfs primary cells before neighbor_share,
           so the "64 KiB neighbor" claimed here was NOT actually in the
           verdict path, and the self-test passed only because it
           hand-built complete directions, bypassing the defective
           entry point. Fixed in Corrective-2 (see below); no verdict
           changed (no primary cell held a supporting direction).

P2 substrate-label disclosure (found during Corrective-1):
  finding: native-1 environment.json resolves BOTH filesystem labels
           ("tmpfs", "btrfs") to the SAME btrfs substrate (/home, zstd:1,
           page-cache) — the prereg §6 "tmpfs (primary, /tmp)" intent was
           not met by the executed session; the "regime control" did not
           vary the regime.
  impact:  F0-vs-F1 causal comparison UNAFFECTED (identical substrate
           within every arm pair; the only delta remains the lookup
           mechanism). Withdrawn in Corrective-2 (Amendment-5): the
           Corrective-1 claim that "the substrate-share bias runs AGAINST
           F1 (harsher materiality)" — the executed evidence cannot
           establish a bias direction: the identical substrate removed
           the intended cross-filesystem microscope AND made cross-label
           agreement same-substrate replication, and the net effect on
           materiality is not observably one-directional (real-tmpfs
           execution later produced a material F1_SLOWER cell at 4K d1,
           opposite to the btrfs trend). Also withdrawn: REGIME language
           — cross-label agreement is same-substrate replication, and
           "REGIME-LOCAL" findings above are regime-local within ONE
           substrate.
  action:  superseded by Corrective-2 P1-1: the frozen tmpfs primary was
           then EXECUTED on real tmpfs (native-3); labels kept for run-id
           continuity; resolved substrate recorded per session.
```

## Corrective-2 record

The Corrective-2 review of 56dc4274 found two P1 blockers and one
provenance weakness in Corrective-1. Frozen prereg text unchanged;
Amendment-5 appended to §16. No frozen materiality rule, matrix shape,
hypothesis, threshold, or §13 neighbor-rule TEXT was modified — the
neighbor defect was tooling wiring that did not implement the frozen
text. Mislabeled raw evidence is preserved byte-identical and marked
SUPERSEDED.

```text
P1-1 frozen tmpfs primary never executed:
  before:  the §6 "tmpfs (primary, /tmp)" cells were never run on tmpfs —
           native-1 AND native-2 resolved BOTH filesystem labels to the
           same btrfs (/home). A "NOT ESTABLISHED" verdict derived from
           btrfs-only execution could not lawfully close the
           tmpfs-primary campaign; Amendment-3's claim that the
           substrate bias "runs AGAINST F1" was not evidence-supported
           (withdrawn, Amendment-5).
  after:   fail-closed substrate gate in the driver — a filesystem label
           is usable (generate/q0/formal) only when the filesystem it
           resolves to equals its canonical fstype, with per-label root
           override (G1C0_FS_ROOT_TMPFS); the session manifest and
           environment.json record root + resolved fstype.
  rerun:   exactly the missing tmpfs half of the frozen matrix — 280
           formal runs (2 ops x 5 cells x 4 arms x 7) + 30-run Q0, all on
           REAL tmpfs, 0 gate errors, same seed plan, same frozen
           thresholds, clean commit-pinned execution (see P2).
  old evidence disposition: native-1 tmpfs-label rows (280 = 140 single
           + 140 threaded) and native-2 tmpfs-label rows (140) =
           SUPERSEDED — WRONG SUBSTRATE, retained byte-identical,
           excluded by the 3-session composite. Retained: native-1
           btrfs F0/F1 (140), native-2 btrfs F0-T/F1-T (140).
  result:  campaign verdicts UNCHANGED — READ/WRITE NOT ESTABLISHED —
           but now lawful (executed frozen primaries). Substantive new
           observations: the btrfs READ 4 KiB sub-material trend does
           NOT replicate on tmpfs (d1 flips to material F1_SLOWER
           0.8591, isolated); the Corrective-1 REGIME-LOCAL threaded
           cell (btrfs 4K d8 1.0495) does NOT replicate on tmpfs
           (1.0284 NONE) — threaded now NOT ESTABLISHED on both
           substrates (exploratory only, either way).

P1-2 neighbor rule not implemented in tooling:
  before:  driver cell_directions() emitted ONLY the 4 KiB tmpfs primary
           cells, so the §13 neighbors (other 4 KiB depths, 64 KiB tmpfs,
           same-depth btrfs) silently resolved to None in
           neighbor_share; the "independent" validator reproduced the
           same wiring defect (filtered to primary before neighbor_share
           in single-session AND composite), and the self-test passed
           only because it hand-built complete directions, bypassing the
           defective entry point — driver bug, validator bug, and
           self-test blind spot aligned.
  after:   driver and validator both run the FULL eligible direction set
           (4 KiB tmpfs family + 64 KiB tmpfs + same-depth btrfs) through
           neighbor_share -> derive_verdicts (which itself selects the
           tmpfs primary family for the campaign verdict); the self-test
           drives the exact production chain from synthetic per-cell
           VALUES with neighbor-specific mutants — N1 (64 KiB neighbor
           carries), N2 (btrfs neighbor carries), N4 (neighbor-supported
           regression) — plus isolated control N3, all proven to
           discriminate the old wiring (N1/N2 degraded to REGIME-LOCAL
           and N4 to NOT ESTABLISHED under it).
  result:  no verdict changed on the actual data (no primary cell held a
           supporting direction on any substrate); stored native-1/
           native-2 summaries re-derived under the fixed rule and
           committed (values unchanged; derivation corrected).

P2 corrective evidence not commit-pinned:
  before:  native-2 executed at pre-corrective HEAD 3d193599 with a dirty
           tracked worktree; the driver/orchestration bytes were not
           independently hashed and the manifest did not record them.
  after:   sessions record dirty_tracked (tracked-files-only pin) and
           driver path + sha256; native-3 was executed only AFTER the
           corrective tooling was committed, at HEAD 45f1ff70 with
           dirty_tracked=false (recorded at generate time); the composite
           and validator FAIL if the tmpfs-corrective session is not
           clean commit-pinned to a 40-hex HEAD.
```

Corrective-2 validator executions: --self-test PASS (production chain +
neighbor mutants); native-2 single-session PASS (280); native-3
single-session PASS (280, incl. substrate-authority and clean-pin
gates); --composite PASS (native-1 + native-2 + native-3, 560 valid
frozen runs; superseded accounting 280 + 140 + 140); probe-order PASS
(native-3).

## Evidence

```text
session (single-thread authoritative for BTRFS): research/g1-control-c0/
  results/g1-control-c0-native-1/  — IMMUTABLE, byte-identical to 3d193599.
  Q0: raw/q0.json (30/30 QUALIFIED; #262 NOT closed)
  identity: raw/fileid.json, raw/replacement-window.json (D label
    superseded by Corrective-1)
  capability: raw/probe.json (features 0x3FFFF incl. IORING_FEAT_RSRC_TAGS)
  perf: raw/runs.jsonl (590 lines = 30 q0 + 560 formal), raw/perf.csv,
    gates.json (560 recorded / 0 errors)
  RETAINED: btrfs-label F0/F1 rows (140). THREADED SUBSET (F0-T/F1-T,
    280): SUPERSEDED (Amendment-1). TMPFS-LABEL SINGLE ROWS (140):
    SUPERSEDED — WRONG SUBSTRATE (Amendment-5)

session (threaded corrective, authoritative for BTRFS threaded):
  research/g1-control-c0/results/g1-control-c0-native-2-threaded-corrective/
  Q0: raw/q0.json (30/30, modified-bench re-qualification)
  identity: raw/probe.json, raw/fileid.json, raw/replacement-window.json
    (corrected POST-COMPLETION UPDATE CONTROL label, re-executed)
  perf: raw/runs.jsonl (310 lines = 30 q0 + 280 threaded formal),
    raw/perf.csv, gates.json (280 formal / 0 errors), every threaded run
    carries the corrective gate fields
  manifest: scope=threaded-corrective, supersedes=native-1 threaded,
    substrate_fstypes={tmpfs: btrfs, btrfs: btrfs}
  analysis: summary.json (re-derived under the Corrective-2 fixed
    neighbor rule; verdicts unchanged)
  RETAINED: btrfs-label F0-T/F1-T rows (140). TMPFS-LABEL ROWS (140):
    SUPERSEDED — WRONG SUBSTRATE (Amendment-5)

session (tmpfs corrective, authoritative for ALL TMPFS cells):
  research/g1-control-c0/results/g1-control-c0-native-3-tmpfs-corrective/
  execution HEAD: 45f1ff7054e8b1f146bef7af59b80b25b3f844ef,
    dirty_tracked=false (clean commit-pinned, recorded at generate time)
  Q0: 30/30 PASS ON REAL TMPFS (G1C0_FS_ROOT_TMPFS=/tmp/g1c0-tmpfs)
  identity: raw/probe.json, raw/fileid.json, raw/replacement-window.json
    (re-executed; same corrected labels)
  perf: raw/runs.jsonl (310 lines = 30 q0 + 280 tmpfs formal, all 4
    arms), raw/perf.csv, gates.json (280 formal / 0 errors)
  manifest: scope=tmpfs-corrective, corrective=2, fs_scope=[tmpfs],
    substrate={tmpfs: {root: /tmp/g1c0-tmpfs, fstype: tmpfs}}
  analysis: summary.json (session-local basis), composite-summary.json

composite (Corrective-2, 3-session substrate-authoritative):
  research/g1-control-c0/results/g1-control-c0-native-3-tmpfs-corrective/
  composite-summary.json — btrfs F0/F1 <- native-1, btrfs F0-T/F1-T <-
  native-2, ALL tmpfs cells <- native-3; 560 valid frozen runs; each
  source carries session, git head, dirty flags, driver sha256, bench
  sha256; superseded accounting recorded per session. The Corrective-1
  composite (2-session, superseded shape) remains in native-2's
  composite-summary.json as historical evidence.

validators: scripts/check_g1_control_c0_analysis.py
              (--self-test PASS — production-chain derivation with
               neighbor mutants N1 64 KiB / N2 btrfs / N4 regression /
               N3 isolated control, plus both verdict-falsification
               mutations;
               single-session PASS on native-2 and native-3 — native-3
               additionally gated on tmpfs substrate authority and the
               clean commit pin;
               --composite PASS over the three sessions, 1120 ok runs,
               560 valid frozen composite runs;
               native-1 standalone fails by design on the 280 superseded
               threaded runs)
            scripts/check_g1_control_c0_probe_order.py PASS (native-3;
              structural + executed: probe/fileid/window ordering,
              withdrawn-claim absence, threaded park-gate ordering)
commands:   g1_control_c0.py probe|generate --fs tmpfs|q0|
            formal --fs tmpfs|summarize|
            composite <native-1> <native-2> <native-3>
build:      xmake f -m release --toolchain=clang --with-liburing=true -y
            xmake build g1_control_c0_bench
environment: per-session environment.json (kernel 7.1.9-200.fc44.x86_64,
            Fedora 44 bare metal, Xeon E5-2666 v3, perf paranoid=2,
            schedutil, liburing 2.14 xrepo; native-1/native-2 both labels
            on btrfs zstd:1; native-3 tmpfs label on REAL tmpfs
            /tmp/g1c0-tmpfs, 32 GiB)
bench sha256 (all sessions, unchanged since Corrective-1):
            6e75182ee925734c9c295502511cd304fc50e0e8dea98fa377e271e680ad06e3
bench sha256 (pre-corrective, native-1): 359b221ba6d1ea0eb34cf7678a975f58ee5f364659c81f64141740b6ca3aec9d
driver sha256 (native-3 execution): recorded in native-3 environment.json
preregistration:  FROZEN at f122c636 (blob 1a67b317) before ANY formal run;
                  Amendments 1-5 appended additively (§16)
```

**Post-freeze analysis-infrastructure disclosure (rules untouched):** the
first `summarize` execution ran on contaminated data — the 30 Q0
qualification runs share the `READ 4K d8 tmpfs F0` cell signature and were
not yet excluded, and that contaminated run printed a transient
`BENEFIT ESTABLISHED` verdict. The fail-closed validator flagged the
violation (`37 valid runs in one cell, expected 7`) BEFORE any verdict was
used; the driver/validator were fixed to exclude `q0-*` run ids (and a
rounding-tolerance bug in the validator's ratio re-check was fixed); the
clean re-run produced the verdicts above. The frozen decision rule (1.03 +
1.5*MAD), matrix, and hypotheses were NEVER modified. Corrective-1 later
found three further evidence-shape issues (thread lifetime prereg
mismatch, non-overlapping Boundary-D witness, verdict validator not
re-deriving the campaign verdict) plus the substrate-label disclosure —
all corrected above without changing frozen materiality rules.
Corrective-2 then found the frozen tmpfs primary had never been executed
(both labels resolved to btrfs) and that the neighbor rule's tooling
wiring dropped the 64 KiB / btrfs neighbors (driver, validator, and
self-test blind spot aligned); both fixed behind fail-closed gates with
discriminating self-test mutants, the missing tmpfs half executed clean
commit-pinned on real tmpfs, and every verdict re-derived — none changed.
Append-only historical honesty is preserved; the frozen prereg text is
unchanged.

## Scope boundaries / blocked facts

```text
HOST-LOCAL ONLY: every performance number is Host-0 (Xeon E5-2666 v3,
  kernel 7.1.9, SATA SSD). No cross-host, no NVMe claim.
SUBSTRATE SPLIT (post Corrective-2): the frozen tmpfs primary family is
  executed on REAL tmpfs (native-3); the btrfs cells on btrfs
  (native-1 single, native-2 threaded). The native-1/native-2
  "tmpfs"-label rows are SUPERSEDED — WRONG SUBSTRATE (btrfs) — and
  excluded from every derived number; before Corrective-2 all cells ran
  on ONE substrate, so no cross-substrate claim predates native-3. The
  4 KiB d1 direction FLIP between substrates is a recorded two-point
  observation, not a regime law.
VERSION-BOUND:   kernel mechanism findings are pinned to v7.1.9 source
  (fetched provenance + hashes in probes/kernel-7.1.9/MANIFEST.txt);
  io_ring_submit_lock's exact definition was not in the fetched snapshot
  (call sites only; non-load-bearing, audit §11).
NOT EXECUTED:    in-flight (overlap) retention witness — source-supported
  only; L1 replacement path — expressible with existing authority, not
  executed (no current requirement).
NOT DETERMINISTICALLY OBSERVABLE: submission->consumption and
  consumption->issue windows (audit boundaries B/C) — userspace cannot
  deterministically place an update inside them; bounded by the source-
  based issue-time-binding conclusion.
2 MiB / 64 KiB cells: single-depth regime/control cells only (prereg §6
  shrink, recorded BEFORE results).
plots:            none generated (matplotlib SVG trailing-whitespace
  breaks `git diff --check`; the summary table carries the data).
G1-SAFETY:        this campaign supplies H03-relevant mechanism evidence
  (ordinary-fd wrong-target reproduced; fixed L0 binding preserved target)
  but does NOT adjudicate G1-Safety or any H03-b split.
#262:             NOT closed (Q0 records only that it did not reproduce in
  this restricted single-worker direct-liburing regime).
```

## Recommendation

```text
C1: DO NOT PROMOTE
```

If promoted, exactly what was earned: **nothing beyond what already
exists.** A future PRODUCT-side fixed-file capability (optional, explicit
typed I/O form, L0 frozen binding, quiescence-gated replacement, zero
steady-state tax) remains designable on product grounds — the identity
hazard conversion is real and the mechanism tax is one flag bit — but
this campaign provides NO G1-Control evidence for it, no performance case
above materiality on EITHER substrate (campaign verdict over the executed
frozen tmpfs primaries; the threaded arms are exploratory and
non-replicating across substrates), and no requirement from any current
workload. Such a step, if ever taken, must be a new product issue (and
would interact with G1-Safety H03-b), NOT a C1 control prototype.

Do NOT implement it (per campaign scope).
