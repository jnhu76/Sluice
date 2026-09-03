# G1-CONTROL-C0 REPORT — fixed-file resource identity and specialization
# falsification (#279)

Status: **COMPLETE — CORRECTIVE-1 APPLIED**. Frozen preregistration
executed in full; Corrective-1 (this revision) repaired three P1-class
evidence-shape defects and one substrate-label disclosure, re-executed
exactly the affected threaded cells, and re-derived every verdict from
raw evidence with the hardened fail-closed validator. The main verdict
is UNCHANGED by the corrected evidence. RESEARCH ONLY: production source
changes ZERO, public API changes ZERO, C1 NOT entered.

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
branch:        research/g1-control-c0-fixed-file
prereg status: FROZEN text unchanged; Amendments 1-4 appended to §16
               (Corrective-1, 2026-09-03, additive only)
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
amendments, campaign.json, driver, validators, TWO immutable evidence
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

Evidence composition (Corrective-1, fail-closed with provenance in
`composite-summary.json`):

```text
C0-PERF primary (campaign verdicts):
  single-thread F0/F1 from g1-control-c0-native-1 (560-run matrix; the
  F0/F1 subset is authoritative; its threaded subset is SUPERSEDED, see
  Corrective-1)
C0-PERF threaded (EXPLORATORY ONLY):
  corrected F0-T/F1-T from g1-control-c0-native-2-threaded-corrective
  (280 runs, deterministic park/release gate, 0 gate errors)
```

Session native-1: Q0 30/30 PASS, then 560 formal runs, 0 gate errors, all
cells exactly covered. Corrective session native-2: Q0 re-qualification of
the modified bench 30/30 PASS (F0 data path unchanged), then 280 threaded
formal runs, 0 gate errors, all threaded cells exactly covered, every run
carrying the corrective gate fields.

Primary metric: steady-state wall per op (the ONLY qualified metric that
includes kernel lookup cost — perf_event_paranoid=2 pins every perf event
to userspace; prereg §3). Frozen materiality: ratio >= 1.03 AND 1.5*MAD
separation, primary cells = 4 KiB family (tmpfs directory label).

```text
READ  (primary family, ns/op median ± MAD, F0 vs F1; single-thread):
  4K d1  tmpfs:  1313.0 ± 5.5   vs 1292.8 ± 9.5   ratio 1.0156  NONE
  4K d8  tmpfs:   856.4 ± 8.4   vs  831.6 ± 4.8   ratio 1.0298  NONE
                  (separation held; ratio 2.98% < 3% threshold)
  4K d32 tmpfs:   869.6 ± 13.3  vs  839.3 ± 14.1  ratio 1.0362  NONE
                  (ratio above threshold; separation failed)
  64K d1 tmpfs:  8272           vs  8447           ratio 0.9793  NONE
  2M  d1 tmpfs: 279740          vs 283638          ratio 0.9863  NONE
  (btrfs-label cells: same shape; ONE isolated material cell, 4K d1
   btrfs-label ratio 1.0336 — ISOLATED CELL ONLY, not promoted; note the
   substrate disclosure below: both labels resolve to the same btrfs)

WRITE (all 10 cells): NONE. Mixed directions, writeback-dominated variance
  (e.g. 4K d8 btrfs-label ratio 0.856, 4K d32 tmpfs-label 1.047 — no
  robust separation anywhere).

THREADED ARMS (corrected, EXPLORATORY — cannot carry a verdict, prereg
§5/§13; workers parked across the span, K=4):
  READ:  isolated material cell 4K d8 tmpfs ratio 1.0495 (robust
         separation); neighbors direction-consistent (1.02-1.05) but
         separation-failed => REGIME-LOCAL per the frozen rule, reported
         as an exploratory observation ONLY.
  WRITE: no material direction; one isolated regression-shaped cell
         (4K d1 tmpfs 0.8981) — per-cell observation only.
  THREADED-PROCESS ADVANTAGE: NOT ESTABLISHED (HOST-LOCAL).
```

```text
primary metric:      wall/op (median of 7 seeded-interleaved rounds/cell)
materiality:         frozen rule (1.03 + 1.5*MAD) — no single-thread
                     primary cell passes
neighbor support:    n/a — no primary-cell direction to support
same-work:           PASS everywhere (exact CQE/byte accounting per op,
                     content spot check per run, dst sha256 == frozen
                     per-size pattern constant for every WRITE run, causal
                     fields gated: align_remainder==0, slot_stride==size,
                     registered_files==1 on F1 arms; threaded runs
                     additionally threads_spawned/io_ok/ready/released/
                     joined == 4 and thread_gate_ready /
                     thread_gate_release_after_transfer == true)
isolated cell:       single-thread READ 4K d1 btrfs-label flagged
                     F1_FASTER (1.0336); per prereg §13.2: ISOLATED CELL
                     ONLY, not promoted to any verdict
registration cost:   register ~3.1 us, unregister ~3.3 us (single fd),
                     reported separately, never amortized into steady state
secondary (report-only, USERSPACE-ONLY, NOT kernel-lookup evidence):
                     instructions:u/op READ 4K d32 tmpfs: F0 167.6 vs F1
                     163.6 (-2.4%) — direction-consistent with wall, but
                     userspace-only by construction
```

**Verdict (campaign, from single-thread primaries):**
`FIXED-FILE PERFORMANCE BENEFIT NOT ESTABLISHED` (READ and WRITE
independently, frozen rule; re-derived from raw by the hardened validator
and equal to the stored verdicts). Recorded observation, explicitly BELOW
materiality and not a claim: the READ 4 KiB tmpfs-label family shows a
direction-consistent F1-faster trend of 1.6–3.6% across all three depths,
the same order as the mechanism estimate for the lookup delta (audit
§3/§4) at small-op sizes. The frozen rule did not establish it (d8:
separation held but ratio 2.98% < 3%; d32: ratio 3.62% but separation
failed; d1: neither). The corrected threaded arms show the same
direction-consistent picture slightly amplified but equally
non-established (isolated cell only; exploratory).

## FILE-ID-E0

Deterministic witness (dup2-forced reuse; no sleep, no probabilistic
reuse). Raw: `results/g1-control-c0-native-1/raw/{fileid,replacement-window}.json`
(historical; D label superseded) and
`results/g1-control-c0-native-2-threaded-corrective/raw/{probe,fileid,replacement-window}.json`
(corrected labels, re-executed by Corrective-1). Ordering guard
`check_g1_control_c0_probe_order.py` PASS (structural + executed).

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
   (frozen rule; READ and WRITE; single-thread primaries; 560-run
   matrix). The sub-material READ 4 KiB trend (1.6–3.6%) is recorded as
   an observation, below the preregistered materiality bar.
2. **Threaded-process advantage for fixed files: NOT ESTABLISHED**
   (HOST-LOCAL; corrected evidence, Corrective-1). Under the frozen §5
   threaded condition (K=4 workers parked across the span), the threaded
   arms show the same direction-consistent sub-material READ trend with
   ONE isolated material cell (4K d8 tmpfs 1.0495, REGIME-LOCAL,
   exploratory, no neighbor support) and no WRITE direction. The
   "threads => fixed files win" folk claim remains falsified for this
   regime on this host; the corrected data does not upgrade it.
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
  result:  READ REGIME-LOCAL isolated material cell (4K d8 tmpfs 1.0495);
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
           raw cells (prereg §13/§13.1, faithful neighbor rule incl. the
           64 KiB neighbor) and requires stored == derived in every mode
           (single-session, composite); --self-test proves both mutation
           directions (benefit erasure, benefit fabrication) are rejected.
           Execution: self-test PASS; single-session PASS (280 valid);
           composite PASS (840 valid runs); on native-1 the hardened
           validator fails BY DESIGN on exactly the 280 superseded
           threaded runs (no verdict/ratio mismatches — the stored
           single-thread verdicts were independently confirmed).

P2 substrate-label disclosure (found during Corrective-1):
  finding: native-1 environment.json resolves BOTH filesystem labels
           ("tmpfs", "btrfs") to the SAME btrfs substrate (/home, zstd:1,
           page-cache) — the prereg §6 "tmpfs (primary, /tmp)" intent was
           not met by the executed session; the "regime control" did not
           vary the regime.
  impact:  F0-vs-F1 causal comparison UNAFFECTED (identical substrate
           within every arm pair; the only delta remains the lookup
           mechanism); the substrate-share bias runs AGAINST F1 (harsher
           materiality), so the NOT ESTABLISHED verdicts are not
           endangered. Withdrawn: REGIME language — cross-label agreement
           is same-substrate replication, and "REGIME-LOCAL" findings
           above are regime-local within ONE substrate.
  action:  labels kept for run-id continuity (frozen matrix); resolved
           substrate now recorded per session (manifest
           substrate_fstypes); corrective threaded cells executed on the
           SAME substrate for comparability; all regime wording in this
           report read as substrate-qualified.
```

## Evidence

```text
session (single-thread authoritative): research/g1-control-c0/results/
  g1-control-c0-native-1/  — IMMUTABLE, byte-identical to 3d193599.
  Q0: raw/q0.json (30/30 QUALIFIED; #262 NOT closed)
  identity: raw/fileid.json, raw/replacement-window.json (D label
    superseded by Corrective-1)
  capability: raw/probe.json (features 0x3FFFF incl. IORING_FEAT_RSRC_TAGS)
  perf: raw/runs.jsonl (590 lines = 30 q0 + 560 formal), raw/perf.csv,
    gates.json (560 recorded / 0 errors)
  THREADED SUBSET (F0-T/F1-T): SUPERSEDED — do not derive threaded claims
    from it (prereg Amendment-1)

session (threaded corrective): research/g1-control-c0/results/
  g1-control-c0-native-2-threaded-corrective/
  Q0: raw/q0.json (30/30, modified-bench re-qualification)
  identity: raw/probe.json, raw/fileid.json, raw/replacement-window.json
    (corrected POST-COMPLETION UPDATE CONTROL label, re-executed)
  perf: raw/runs.jsonl (310 lines = 30 q0 + 280 threaded formal),
    raw/perf.csv, gates.json (280 formal / 0 errors), every threaded run
    carries the corrective gate fields
  manifest: scope=threaded-corrective, supersedes=native-1 threaded,
    substrate_fstypes={tmpfs: btrfs, btrfs: btrfs}
  analysis: summary.json (threaded scope), composite-summary.json
    (fail-closed composition: F0/F1 <- native-1, F0-T/F1-T <- native-2,
    provenance: sessions, git heads, bench sha256s, superseded disposition)

validators: scripts/check_g1_control_c0_analysis.py
              (--self-test PASS; single-session PASS; --composite PASS;
               native-1 standalone fails by design on the 280 superseded
               runs)
            scripts/check_g1_control_c0_probe_order.py PASS (structural +
              executed: probe/fileid/window ordering, withdrawn-claim
              absence, threaded park-gate ordering)
commands:   g1_control_c0.py probe|generate|q0|formal --arms F0-T,F1-T|
            summarize|composite
build:      xmake f -m release --toolchain=clang --with-liburing=true -y
            xmake build g1_control_c0_bench
environment: per-session environment.json (kernel 7.1.9-200.fc44.x86_64,
            Fedora 44 bare metal, Xeon E5-2666 v3, btrfs zstd:1 substrate
            for BOTH labels, perf paranoid=2, schedutil, liburing 2.14
            xrepo)
bench sha256 (corrective): 6e75182ee925734c9c295502511cd304fc50e0e8dea98fa377e271e680ad06e3
bench sha256 (pre-corrective, native-1): 359b221ba6d1ea0eb34cf7678a975f58ee5f364659c81f64141740b6ca3aec9d
preregistration:  FROZEN at f122c636 (blob 1a67b317) before ANY formal run;
                  Amendments 1-4 appended additively (§16)
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
all corrected above without changing frozen materiality rules. Append-only
historical honesty is preserved; the frozen prereg text is unchanged.

## Scope boundaries / blocked facts

```text
HOST-LOCAL ONLY: every performance number is Host-0 (Xeon E5-2666 v3,
  kernel 7.1.9, SATA SSD). No cross-host, no NVMe claim.
SINGLE-SUBSTRATE: all cells (both labels) executed on btrfs (zstd:1,
  page-cache) — no tmpfs-vs-btrfs regime variation was measured; regime
  findings are single-substrate (Corrective-1 P2 disclosure).
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
hazard conversion is real and the mechanism tax is one flag bit — but this
campaign provides NO G1-Control evidence for it, no performance case above
materiality (campaign verdict; the corrected threaded arms are exploratory
and isolated), and no requirement from any current workload. Such a step,
if ever taken, must be a new product issue (and would interact with
G1-Safety H03-b), NOT a C1 control prototype.

Do NOT implement it (per campaign scope).
