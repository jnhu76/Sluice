# R7 AC-2 Closure — Gathered Positioned Read (#606): Consistency Review

- Issue: jnhu76/Sluice **#606** (AC-2, gathered positioned read with
  per-request results — Awaiting Owner). Phase: E5 Closure R7 (mission
  `.zcode/workflow-drafts/e5-closure-mission.md` §10). **This phase does not
  productize**: no public API is adopted, no PR is created or merged, the
  v2-r1 contract text is untouched.
- Reviewer: AC-2 Correctness Reviewer (E5 closure workflow), session of
  2026-10-29, host E5 (kernel 7.2.11-200.fc44.x86_64 — same session
  environment as `closure/E5_CLOSURE_SNAPSHOT.md` §1).
- Question put to this phase (mission §10): whether the AC-2 candidate's
  mechanisms (threshold merge, intra-set dedup, per-member dispatch) are
  semantically sound under **generic File semantics** — arbitrary concurrent
  writers, no caller consistency promise — and what the default policy must
  therefore be.
- Legend: `[RUN]` executed in this session (command + raw output under
  `closure/r7-ac2/raw/`); `[TRANSCRIBED]` copied from an archived prior-phase
  record, source cited; `[ANALYSIS]` derived argument from cited contract/
  research text, no new measurement.

## 0. Inputs read this session

| Input | Where |
|---|---|
| Mission §10 (R7 AC-2 eight items, verdict vocabulary) | `.zcode/workflow-drafts/e5-closure-mission.md:702-744` |
| Issue #606 body (candidate sketch; "intra-set dedup, default on"; owner decisions incl. consistency-window text and M2-OQ5 oracles) | `gh issue view 606` (fetched this session) |
| AC-2 candidate text | `research/direct-beyond-parity/API_CAPABILITY_CANDIDATES.md:123-262` |
| M2 family analysis (mechanism forms F1–F6; proof obligations; illegal forms §2.5; OQ5/OQ6) | `research/direct-beyond-parity/p3-families/M2_ANALYSIS.md` |
| P4 Parquet strategy-arm regressions + boundaries | `research/direct-beyond-parity/REAL_WORKLOAD_COMPETITION.md` §2.3, §2.5, §5.1 |
| Divergence record (writer mutation; outcome digests; REDO note) | `research/direct-beyond-parity/p3-raw/m2/p3-m2-concurrent-divergence.txt` |
| Planner-overhead cell (+495.216 ns median, t=−24.95, 16 pairs) | `research/direct-beyond-parity/p3-raw/m2/p3-m2-planner-overhead-g4k.txt:37-40` |
| Dedup ov30 cell (bytes 32768→21507, syscalls 8→3, wall +2146.626) | `research/direct-beyond-parity/p3-raw/m2/p3-m2-dedup-ov30.txt:36-41` |
| Mechanism verdicts (PRED-M2-5 falsified ≈5×; COST_MIGRATION_ONLY = 151% net / 60% gross) | `research/direct-beyond-parity/STRUCTURAL_IO_MECHANISM_STUDY.md:294-326,371-382` |
| v2-r1 SEM-03/04/05 (per-call admission; no cross-call order; no atomicity/snapshot; short/EOF/error discipline) | `docs/explicit-io-contract.md:398-481` (SEM-04 at `:437`, no-snapshot at `:456`) |
| THREAD-01 concurrency matrix | `docs/explicit-io-contract.md:1003-1021` |
| Current `read_at` + precheck (master bdfd0d8d) | `src/blocking_file.cpp:20-37,86-105`; `include/sluice/blocking/file.hpp:14,40-49` |
| Conformance ledger: no AC-2/gather/dedup obligation exists | `grep` over `docs/roadmap/v2-conformance.md` returned no gather/AC-2/dedup rows (candidate not adopted — consistent with Awaiting Owner) |

## 1. The eight items

### 1.1 Same-offset duplicate reads under concurrent writes — is dedup legal?

**The mechanism.** Intra-set dedup (M2 form F2, `M2_ANALYSIS.md:81-82`)
computes the union of request extents and issues one `::pread` per disjoint
union component, dispatching per-member slices. Two members at the same
offset are therefore served by **one kernel observation**; in the un-deduped
form each member is its own `pread` — its own observation at its own instant.

**What the contract does and does not promise.** SEM-04 promises no
cross-call serialization order and no atomicity/snapshot for overlapping
access (`docs/explicit-io-contract.md:453-457`); a gathered call extends the
per-call discipline per member and inherits that same no-order/no-snapshot
stance (`API_CAPABILITY_CANDIDATES.md:207-213` discloses: "a merged extent
is one kernel observation"). So *neither* form can be called contract-wrong
by byte content alone under a writer. The question is what each form can
**prove** about its own per-member results.

**The provable core (dedup is definable).** `[ANALYSIS]` Under generic File
semantics the following IS provable for dedup, and it is the same class of
statement that makes merge definable: every delivered byte originates from a
kernel observation issued **during the call**, at an extent covering the
member; per-member outcomes are reconstructed by declared rules; no promise
stronger than SEM-04's no-order/no-snapshot language is made. This
distinguishes intra-set dedup from **cross-call memoization**, which is
illegal precisely because it can deliver bytes observed *outside* the call
(`M2_ANALYSIS.md:277` §2.5-a). Nothing above is established by byte
comparison — see the next paragraph.

**What cannot be proven (the red line).** The safety property that would
justify *default-on* dedup — "the default form preserves the observable
guarantees a caller relies on when submitting k independent positioned
reads" — does not exist under generic File semantics. In the un-deduped form
every member's outcome is the outcome of a positioned read of *that member's
extent at its own instant* (the per-call SEM-05 sense). Under dedup the
members of a union component share one instant, chosen by the planner; the
correlation is a function of the whole request set and policy, and at
default-on it is imposed without caller opt-in. No oracle in the archived
evidence can see this difference, because every archived dedup gate is a
**static-file byte digest** (`req_digest`/`ref_digest`/`outcome_digest`,
`p3-raw/m2/p3-m2-*.txt`) — and static-file byte-identity holds under BOTH
semantics by construction.

**Empirical witness — `[RUN]`** (`raw/exp1-dedup-collapse.txt`, probe
`exp1_dedup_collapse.c`; tmpfs, 1 MiB fixture, 64-byte region at offset
4096, writer thread XOR 0xFF inside a pipe handshake bracket, 100 trials per
arm, no timing measured):

| Arm | Semantics | A==B | A!=B | A==C | A!=C |
|---|---|---|---|---|---|
| U (concurrent writer) | two independent observations | 0/100 | **100/100** | 0/100 | 100/100 |
| D (concurrent writer) | dedup: B served by A's observation | **100/100** | 0/100 | 0/100 | **100/100** |
| UQ / DQ (static control) | both | 100/100 | 0/100 | 100/100 | 0/100 |

Reading: under a concurrent writer the two semantics observably diverge
(U: member B sees the mutation; D: member B is served bytes that member B's
own logical read never re-observed — the control read C proves the file
changed **inside the members' own service window**). Under a static file
both forms are byte-identical (UQ/DQ) — a direct demonstration that the
archived static-file digest gates are **structurally insensitive** to the
dedup semantics at issue. The archived divergence record independently
corroborates the same blindness from the producer side: its REDO note shows
a mutation aimed at an offset the trace never touches leaves every digest
equal (`p3-m2-concurrent-divergence.txt:1-3`). **Byte-identity is not
evidence of dedup semantic correctness.** `[RUN]`

**Conclusion (item 1).** Dedup is *semantically definable* under generic
File semantics only as a shared-observation mechanism with declared
reconstruction rules; it is NOT provably equivalent to independent reads,
and no oracle of the class that exists today can close that gap. Per the
mission's red line:

> **DEDUP_DEFAULT_ON = REJECTED_UNLESS_CONSISTENCY_PRECONDITION.**

Dedup may be retained only behind an explicit, caller-accepted consistency
precondition — for example (shape for the Owner to choose, not adopted here):
(a) the caller declares the covered extents quiescent (no concurrent
overlapping mutation) for the call's duration, or (b) dedup is an explicit
policy value whose contract text defines shared-observation semantics per
union component and explicitly waives per-member observation independence.
It must not be the silent default of an operation whose File may be mutated
concurrently. This **rejects the #606 draft's `dedup (default on)`
(`API_CAPABILITY_CANDIDATES.md:136-137`) and the issue body's "intra-set
dedup, default on"** — per mission §10, the earlier draft text is not
authority.

### 1.2 Per-request independent result responsibility

- The candidate's result shape is per member: {count, EOF-at-observed-
  position, admission rejection, OS error with native detail, effect
  certainty under ERR-02} (`API_CAPABILITY_CANDIDATES.md:140-144`).
- `[RUN]` EXP-2 (`raw/exp2-merge-dispatch.txt`, probe
  `exp2_merge_dispatch.c`; static 10 000 B tmpfs file; 6 members covering
  inside / gap-merged / overlapping / straddling-EOF / zero-length /
  wholly-past-EOF; T = 4096): reconstruction from **2 merged covering
  preads** reproduces the independent per-member pread oracle exactly —
  per-member count, bytes and EOF-0 classification equal in 6/6 members;
  straddling member gets exactly its 100-byte prefix; wholly-past-EOF member
  reports count 0 EOF success; the zero-length member is never served (no
  extent) and reports success 0. Consistent with the P3 eofmix probe
  (PRED-M2-D `[RUN]` in P3, transcribed via
  `p3-raw/m2/p3-m2-eof-attribution.txt`).
- `[RUN]` EXP-3 (`raw/exp3-midbatch-error.txt`): with a
  **continue-after-error** batch policy, every member settles with its own
  outcome using only the existing shape even across a real mid-batch OS
  error — per-member responsibility is preservable (§1.4/§1.6).
- **Residual coupling that must be contract text** `[ANALYSIS]`: with merge
  or (preconditioned) dedup enabled, a member's outcome is reconstructed
  from an observation it shares with other members; the observation point is
  a function of (request set, policy). The contract must therefore declare
  reconstruction rules and observation sharing, and must NOT claim
  per-member observation independence. With T=0 and dedup off, member
  independence is exact (each member = its own pread; EXP-1 arm U).
- **Gap found:** an abort-at-first-error batch policy cannot express later
  members' outcomes with the candidate's shape — a NOT_ATTEMPTED value does
  not exist (`API_CAPABILITY_CANDIDATES.md:140-144` has no such member;
  EXP-3 output records the gap). Either the policy must be pinned to
  continue-after-error, or the shape must be extended. Today's text pins
  neither; this is part of M2-OQ5's unresolved oracles.

### 1.3 Post-merge short read / EOF / error dispatch

- **Short/EOF** `[RUN]`: EXP-2 PASS (§1.2). Matches SEM-05's per-call rows
  reconstructed per member (`docs/explicit-io-contract.md:459-473`).
- **Error** `[RUN]` + `[ANALYSIS]`: on the Linux regular-file profile a
  single `pread` either returns a count (possibly short) or fails as a
  **whole-call** error with zero confirmed bytes — `read(2)`: EINTR is
  "interrupted by a signal **before any data was read**" (local man-pages,
  checked this session); EXP-3's injected failures behaved whole-call
  (EFAULT errno 14 on a PROT_NONE destination; EISDIR errno 21 on a
  directory fd). Consequently the candidate's intersect rule ("a merged
  syscall's OS error maps to the members whose extents intersect the failed
  transfer", `API_CAPABILITY_CANDIDATES.md:202-205`) collapses, for
  single-extent merged preads, to "every member of the failed extent gets
  the same OS error" — and EXP-3 verified this equals what each member's
  own independent read on the same fd returns (EISDIR row). There is no
  fail-after-partial observation for a merged executor to dissect on this
  profile; the "uncounted attempt / unknown-effect (ERR-02)" clause applies
  to the abort/cancellation path (members never attempted), not to pread
  error semantics.
- NOT_RUN_WITH_REASON: fault-injection of position-dependent errors *inside*
  one merged extent (e.g. mid-transfer medium error) — no safe local
  mechanism exists to inject a partial regular-file transfer failure
  deterministically without kernel support; the whole-call error model above
  covers the classes reachable here (EFAULT/EISDIR/EBADF/EINTR).

### 1.4 Batch partial admission failure

- Rule under review: per-member SEM-03 precedence; error fusion is illegal
  (M2 §2.5-c, `M2_ANALYSIS.md:279-280`).
- `[RUN]` EXP-4 (`raw/exp4-mixed-admission.txt`, probe
  `exp4_mixed_admission.cpp`, compiled against the **actual master bdfd0d8d
  SDK** — `src/blocking_file.cpp` + `src/file_resource.cpp`, g++ 16.2.1
  -std=c++20): the per-member admission oracle table the gathered call must
  reproduce:

| Member | Expression | Oracle outcome (measured) |
|---|---|---|
| a | zero-len dst @ offset UINT64_MAX | success 0 — offset/range check **skipped** (SEM-03 step 3) |
| b | zero-len dst @ valid offset | success 0 |
| c | len 8 @ offset UINT64_MAX | `invalid_argument` (unrepresentable range) |
| d | len 8 @ valid offset | count 8 |
| g | zero-len **nullptr** dst @ UINT64_MAX | success 0 |
| e | zero-len on **closed** file | `invalid_state` (closed wins over the no-op: SEM-03 steps 1 < 3) |
| f | len 8 on closed file | `invalid_state` |

  Gate: EXP4_GATE=PASS (7/7).
- The gathered form must apply this table member-wise with **no cross-member
  poisoning**: a rejected member contributes no extent to planning and does
  not change any other member's verdict. The per-call table is now an
  executable oracle; the gathered-form test does not exist (no product code
  — by design this phase), so the batch-level non-fusion property remains a
  contract obligation verified only structurally (admission precedes
  planning). This closes the oracle-construction half of M2-OQ5's
  "per-member admission rejections inside a batch".

### 1.5 Zero-length and illegal-offset mixed members

- `[RUN]`: EXP-4's table IS the mixed-member answer (zero-length at
  unrepresentable offsets is a legal no-op success; only nonzero lengths
  make the range check fire; closed-file precedence dominates both).
- Planning consequence pinned by EXP-2 `[RUN]`: zero-length members
  contribute **no extent** to union/merge planning — they neither merge
  neighbors nor get "served" — yet still appear in results with success 0
  and **no EOF observation** (SEM-03 step 3: "A direct no-op returns
  successful zero after steps 1–3, without an OS call";
  `docs/explicit-io-contract.md:430-432`). EXP-2 verified the executor that
  skips them matches the per-call oracle (m4 row).

### 1.6 Mid-flight OS error injection

- `[RUN]` EXP-3 (`raw/exp3-midbatch-error.txt`): deterministic mid-batch OS
  error injection via a PROT_NONE destination — m2's `pread` returns
  **EFAULT (14)** while m0 (before) and m3 (after) succeed with full counts
  under the continue policy. A whole-call merged-form error was injected via
  a directory fd — **EISDIR (21)**, identical for member-shaped reads on the
  same fd. Admission rejection (m1, offset+len unrepresentable) exercised
  with no syscall. EXP3_GATE=PASS.
- Findings: (a) mid-batch member errors are constructible and deterministic
  at raw POSIX level; (b) continue-after-error preserves per-member result
  responsibility with the existing outcome shapes; (c) abort-at-first-error
  requires an outcome value the candidate does not have (§1.2 gap); (d) the
  candidate's "uncounted attempts flagged unknown-effect" clause belongs to
  the abort/cancellation path (§1.3). This closes the oracle-construction
  half of M2-OQ5's "injected mid-batch OS errors"; the **policy** itself
  remains an Owner contract decision that must be written before any product
  change.

### 1.7 Current planner real cost ceiling

- Claim-bearing figure `[TRANSCRIBED]` (P3, `[RUN]` there):
  planner+dispatch own cost **+495.2 ± 41.7 ns/op-group** at equal syscall
  counts (k=8, g4k, warm tmpfs; cell
  `p3-m2-planner-overhead-g4k.txt:37-40`, median −495.216, t=−24.95, 16
  pairs; verdict text `STRUCTURAL_IO_MECHANISM_STUDY.md:294-326`). This
  falsifies PRED-M2-5's ≤100 ns bound by ≈5× and fires the preregistered
  **COST_MIGRATION_ONLY** trigger: 495.2 ns = **151%** of the net winning-T
  saving (327.2 ns) and **60%** of the additive-gross saving (822.4 ns).
- Scaling shape `[RUN, NON-CLAIM characterization]` (`raw/exp5-planner-cost.txt`,
  probe `exp5_planner_cost.c`; pure userspace, no syscalls in the timed
  region, 20 000 iterations/point, seeded LCG g4k-family sets; run after
  verifying no formal benchmark was active — the `ps` pre-check's sole
  pattern match was the kernel worker `[kworker/R-dm_bufio_cache]` (the
  substring "fio" inside "bufio"), not a measurement process): planner-only merge-plan cost
  ≈ 212 ns at k=8 (26.6 ns/req), ≈ 2 560 ns at k=64 (40.0 ns/req), ≈ 39 020
  ns at k=512 (76.2 ns/req) — ≈ O(k log k) growth (64× k, ≈184× cost ≈
  64·log-ratio). Dedup-plan nearly identical (±4%). **Not a performance
  claim**; it bounds the growth shape only.
- Real-scale reading `[TRANSCRIBED]`: at the only admitted real trace
  (Parquet, k=57, ~182 MB/pass) the planner is not the binding cost — even
  ~76 ns/req ≈ 4.3 µs/pass is six orders below the measured strategy-arm
  regressions (+40.094 ms gather-sluice staging, +216.229 ms gather-native
  multi-buffer destination; `REAL_WORKLOAD_COMPETITION.md:463-478`). The
  binding real-scale cost is the **destination/staging shape**, not sort/
  sweep bookkeeping.
- **Ceiling statement**: the planner's own cost is a first-class, disclosed
  cost — ≥495.2 ns/op-group at k=8 measured including dispatch bookkeeping,
  growing ≈O(k log k); any gathered-call economics must charge it (plus k·T
  amplification and staging copies) against syscall savings, and at the only
  probed winning shape (T\*=4 KiB g4k warm tmpfs) the planner cost alone
  exceeds the net saving → COST_MIGRATION_ONLY. The candidate text already
  requires this disclosure (`API_CAPABILITY_CANDIDATES.md:186-194`);
  this review confirms and tightens the numbers it must carry.

### 1.8 P4 Parquet regression — applicability boundary

What regressed `[TRANSCRIBED]` (`REAL_WORKLOAD_COMPETITION.md:431-503`):
all three Layer-C strategy arms at the only admitted real trace —
gather-native **+216.229 ms/pass** (CI excl 0, 16/16), gather-sluice
**+40.094 ms/pass** (staging_copies=57, staging_bytes=181,996,415),
arrowprebuf **+23.839 ms/pass** (Arrow's own coalescing planner; preads
173→29 yet slower).

Applicability boundaries (each load-bearing for the Owner decision):

1. **Footprint confounder (disclosed by P4 §2.5.3):** the gather-native
   falsification measures the multi-buffer destination shape — 57 live
   buffers (~182 MB, DRAM-resident) vs plain replay's one 16 MB L3-resident
   buffer. The measured statement is "the multi-buffer gather destination
   costs far more than the syscalls it saves **at this destination shape**";
   it does not preclude a different conclusion for consumers that already
   require scattered live buffers. **No admitted workload of that shape
   exists** (HF_SMALL_IO BLOCKED, DUCKDB BLOCKED — §3/§4 of the same
   record).
2. **Trace shape:** the frozen trace is 57 requests ≥ 33 KiB (median
   1.39 MB), one contiguous segment with one 4-byte hole and **0.016%
   overlap** (`REAL_WORKLOAD_COMPETITION.md:355-359`). At this shape dedup
   has almost nothing to deduplicate and merge degenerates to
   near-whole-scan coalescing whose destination cost dominates. The positive
   dedup evidence (ov30: −34.4% issued bytes, 8→3 syscalls, 30% duplicates,
   4 KiB requests — `p3-m2-dedup-ov30.txt`) is a **synthetic** small-request
   high-duplicate shape with no admitted real-workload counterpart. P4's
   regression therefore bounds large-read near-contiguous traces; it neither
   proves nor refutes small-request/high-duplicate real workloads — it
   establishes that **none is admitted**.
3. **Evidence class:** strategy arms are new-application-strategy
   measurements (request plans differ by declaration), single host/session/
   kernel, no cross-kernel comparability, and they measure **no Sluice code
   change** (public API untouched). They are AC-opportunity evidence only.
4. **Consequence:** the only admitted real-trace opportunity evidence for
   AC-2 is **negative — both sides' best legal planning pays**. The positive
   evidence is probe-level synthetic traces only, capped
   CONDITIONAL_WORKLOAD_WIN. No new-workload opportunity was admitted in
   this phase (per mission: benchmark search not expanded), so this
   asymmetry stands.

## 2. Verdict

**DESIGN_CONDITIONALLY_ADMISSIBLE** — with binding conditions, none of which
is adopted by this phase:

1. **`DEDUP_DEFAULT_ON = REJECTED_UNLESS_CONSISTENCY_PRECONDITION`** (§1.1).
   Dedup survives only behind an explicit caller-accepted consistency
   precondition or an explicit shared-observation policy semantics; the
   #606 draft's default-on is rejected.
2. **Default policy: T = 0 (no merging); dedup off/preconditioned.** T=0 was
   already the candidate's merge default (`API_CAPABILITY_CANDIDATES.md:136`);
   the dedup default flips, an explicit deviation from the draft permitted
   by mission §10.
3. **Batch error policy must be pinned before any product change:**
   continue-after-error (recommended — preserves per-member responsibility
   with existing shapes, EXP-3), or an explicit NOT_ATTEMPTED outcome value
   added to the member shape.
4. **Per-member oracle set defined and executable** (M2-OQ5 closed at oracle-
   construction level): static dispatch/EOF reconstruction (EXP-2), mixed
   admission incl. zero-length/illegal-offset/closed precedence (EXP-4),
   mid-batch OS error (EXP-3). These are research oracles in this closure
   directory, not merged tests; they become normative only through an
   Owner-adopted ADR.
5. **Contract text must carry:** observation-point disclosure per union
   component (one kernel observation; no snapshot/order beyond SEM-04);
   per-member reconstruction rules; amplification bound issued ≤ requested +
   k·T; planner own-cost disclosure (≥495.2 ns/op-group at k=8, ≈O(k log k)
   growth, COST_MIGRATION_ONLY at the probed winning shape); the seven
   illegal forms of `M2_ANALYSIS.md §2.5` as binding negative space.
6. **Owner-facing adoption context (unchanged by this review):** the only
   admitted real-workload opportunity evidence is negative (§1.8); whether
   the capability ships at all given the narrow merge window remains the
   Owner decision already flagged in #606.

Why not the other verdicts:

- *REQUIRES_STRONGER_CONSISTENCY_CONTRACT* — rejected: the T=0/no-dedup core
  and the merge form are semantically admissible under current v2-r1 with
  per-member extension and disclosure (EXP-2 reconstruction + SEM-04's
  existing no-snapshot language suffice); no new consistency *foundation* is
  required. What needs a precondition is specifically the dedup **default**,
  which condition 1 carries as a policy condition rather than a foundation
  gap.
- *NO_GO_FOR_GENERIC_DIRECT* — rejected: nothing in the evidence shows the
  design cannot be made sound; the evidence shows the default is not
  retainable (dedup), the merge economics are COST_MIGRATION_ONLY, and the
  admitted real-workload opportunity is negative. Those are Owner adoption
  inputs, not a semantic impossibility.
- *INCONCLUSIVE* — rejected: every item is answered with `[RUN]` evidence
  from this session or archived `[RUN]` cells; the residual NOT_RUN items
  are scoped and do not block any conclusion above.

## 3. Experiments executed this session (reproduction)

All raw outputs under `closure/r7-ac2/raw/`; probes beside them. Host: E5,
kernel 7.2.11-200.fc44.x86_64, tmpfs `/tmp` fixtures. No PR created or
merged; no product file modified (probes and raw outputs are new untracked
files under `research/direct-beyond-parity/closure/r7-ac2/`).

| ID | Command (abbreviated) | Result | Status |
|---|---|---|---|
| EXP-1 | `gcc -O2 -Wall -Wextra -o /tmp/r7ac2-exp1 exp1_dedup_collapse.c -pthread && /tmp/r7ac2-exp1` | U: A≠B 100/100; D: A==B 100/100 & A≠C 100/100; UQ/DQ byte-identical → static oracles blind to dedup semantics | `[RUN]` |
| EXP-2 | `gcc -O2 -Wall -Wextra -o /tmp/r7ac2-exp2 exp2_merge_dispatch.c && /tmp/r7ac2-exp2` | EXP2_GATE=PASS: 6/6 members reconstructed from 2 merged preads equal the independent-read oracle | `[RUN]` |
| EXP-3 | `gcc -O2 -Wall -Wextra -o /tmp/r7ac2-exp3 exp3_midbatch_error.c && /tmp/r7ac2-exp3` | EXP3_GATE=PASS: EFAULT(14) mid-batch, continue-policy member-after-error succeeds, EISDIR(21) whole-call uniform | `[RUN]` |
| EXP-4 | `g++ -std=c++20 -O1 -I include -o /tmp/r7ac2-exp4 exp4_mixed_admission.cpp src/blocking_file.cpp src/file_resource.cpp && /tmp/r7ac2-exp4` | EXP4_GATE=PASS (7/7): per-member admission oracle against actual master SDK | `[RUN]` |
| EXP-5 | `gcc -O2 -Wall -Wextra -o /tmp/r7ac2-exp5 exp5_planner_cost.c && /tmp/r7ac2-exp5` | planner-only ≈212/2560/39020 ns at k=8/64/512 (≈O(k log k)) — NON-CLAIM characterization | `[RUN, NON-CLAIM]` |
| P3 cells | (archived) planner-overhead +495.2 ± 41.7 ns; dedup-ov30 bytes −34.4%, syscalls 8→3, wall +2146.6 | cited as claim-bearing figures | `[TRANSCRIBED]` |
| P4 cells | (archived) gather-native +216.229 ms, gather-sluice +40.094 ms, arrowprebuf +23.839 ms | cited as real-trace opportunity evidence | `[TRANSCRIBED]` |

Not run: position-dependent partial-failure injection inside one merged
extent (no safe deterministic mechanism on this host — §1.3); gathered-form
product tests (no product code exists — by design of this phase).
