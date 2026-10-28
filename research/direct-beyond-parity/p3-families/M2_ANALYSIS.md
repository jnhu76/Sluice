# M2 — Request Range Planning / Coalescing: Pre-Measurement Semantic & Legal Analysis

- Family: **M2 (Request range planning / coalescing)** — contiguous-range merging,
  overlap dedup, sort+sweep, over-read threshold, merged-result dispatch,
  byte-amplification vs syscall-reduction trade-off.
- Author role: P3 family analyst (M2). Document status: **preregistration
  support document, written before any P3 measurement** (see §0.3).
- Parent plan: `research/direct-beyond-parity/P3_MECHANISM_STUDY_PLAN.md` §3
  (family M2, lines 196–274), §5.2 accounting (§6 of the plan), §6 statistics
  (§7), §8 verdicts, §11 API path, §12 deferrals.
- Mission anchor: `research/direct-beyond-parity/MISSION.md` §5.1 M2
  (lines 483–504) and §5.3 (six-element API decision path, lines 562–576).
- Semantic floor: `research/direct-beyond-parity/P0_BASELINE_AND_OWNERSHIP.md` §5
  (lines 634–754).

## 0. Provenance and session verification record

### 0.1 Read this session

| Input | Where |
|---|---|
| Mission M2 subsection (five must-prove bullets; "不得擅自缓存、合并或重排调用"; three legal forms) | `MISSION.md:483-504` |
| Family-M2 plan: semantic frame §3.1, proof obligations §3.2, arms §3.3, cells §3.4, predictions §3.5 | `P3_MECHANISM_STUDY_PLAN.md:196-274` |
| Inherited constants, host facts, kit state | `P3_MECHANISM_STUDY_PLAN.md:24-37` (§0) |
| Semantic floor table (obligation → contract anchor → oracle) | `P0_BASELINE_AND_OWNERSHIP.md:644-667` (§5.1) |
| v2-r1 contract text: SEM-03 | `docs/explicit-io-contract.md:398-435` |
| SEM-04 (offset/position; no cross-call order; no extra atomicity) | `docs/explicit-io-contract.md:437-457` |
| SEM-05 (short I/O, EOF, primitive does not loop) | `docs/explicit-io-contract.md:459-481` |
| SEM-06 (durability coverage) | `docs/explicit-io-contract.md:483-509` |
| THREAD-01 (public concurrency matrix) | `docs/explicit-io-contract.md:1003-1021` |
| W-01 / INV-01 headings | `docs/explicit-io-contract.md:225`, `:309` |
| Product `read_at`: declaration and implementation | `include/sluice/blocking/file.hpp:14`, `src/blocking_file.cpp:86-105`; admission precheck `src/blocking_file.cpp:30-37`; EINTR retry wrapper `src/blocking_file.cpp:100-102` |
| P1 ladder constants (N1 med ns/op: 1 B 558.214, 4 KiB 704.666, 64 KiB 8696.847; cold btrfs 4 KiB 8672.99; read-warm MDE 5.535) | `research/direct-beyond-parity/DIRECT_COST_FLOOR.md:217-227`, `:51` |

### 0.2 Ran this session (commands and results)

| Check | Command | Result |
|---|---|---|
| Kernel | `uname -r` | `7.2.11-200.fc44.x86_64` |
| Root filesystem | `findmnt -no FSTYPE /` | `btrfs` |
| /tmp filesystem | `findmnt -no FSTYPE /tmp` | `tmpfs` |
| No P3 measurement has run | `ls research/direct-beyond-parity/p3-raw` | does not exist (directory absent) |
| `pread(2)` semantics (local man-pages 6.13, 2024-07-23) | `man -P cat 2 pread > /tmp/m2_pread2.txt`, read | "reads **up to** count bytes"; "the file offset is not changed"; return "zero indicates end of file"; "it is not an error for a successful call to transfer fewer bytes than requested"; underlying syscall is `pread64` since Linux 2.6 |
| `read(2)` zero/EOF semantics | `man -P cat 2 read > /tmp/m2_read2.txt`, grep | "If count is zero … read() with a count of 0 returns zero and has no other effects"; offset at/past EOF → "no bytes are read, and read() returns zero" |

### 0.3 Prediction hygiene statement

No M2 (or any P3) probe number exists yet: `p3-raw/` is absent (§0.2). The
plan's PRED-M2-1…6 were pinned 2026-10-28 in
`P3_MECHANISM_STUDY_PLAN.md:254-274`; this document restates them verbatim as
the family's pinned set and adds mechanism-shape predictions PRED-M2-A…F
(§3.2), also pinned in this session, before any measurement. Nothing in §3 is
derived from a probe output.

### 0.4 Arithmetic re-derivation of inherited constants (disclosed, not changed)

From the published ladder (`DIRECT_COST_FLOOR.md:220,223,224`): marginal
warm-tmpfs byte cost from N1 endpoints = (8696.847 − 704.666)/(65536 − 4096) =
**0.130 ns/B**; from B1 endpoints = (8455.666 − 711.949)/61440 =
**0.126 ns/B**. The plan's preregistered 0.128 ns/B sits between them; the
plan value stays the pinned constant. Break-even gap at 550–700 ns per saved
syscall ⇒ ≈ 4.3–5.5 KiB (plan's ≈5.4 KiB point estimate uses the upper end).

## 1. Mechanism inventory and notation

Request set R = {r₁…r_k}, rᵢ = (offᵢ, lenᵢ, dstᵢ), submitted as k logical
reads of one open File. Definitions pinned for all M2 accounting:

- **requested(R)** = Σ lenᵢ.
- **issued(arm)** = Σ of syscall length *arguments* (the plan §6 caliber for
  "bytes transferred"). Near EOF the kernel may return fewer bytes; per-request
  oracles always use *returned* bytes.
- **extra bytes** = issued − requested (can be **negative** under dedup).
- **delivered(rᵢ)** = the bytes written into dstᵢ — the only quantity the
  caller can observe.

Mechanism forms in this family (each analyzed in §2):

| Form | Shape | Physical effect |
|---|---|---|
| F1 contiguous merge | sort requests by offset; sweep-merge adjacent extents whose gap ≤ T into one covering `::pread` into staging; slice-dispatch | syscalls k → m (m = merged extents); issued grows by Σ merged gaps; +1 userspace memcpy per covered request |
| F2 overlap dedup | compute the union of extents; read each disjoint union component once; dispatch by slicing | issued = union bytes ≤ requested; syscalls = disjoint components; duplicates transferred once |
| F3 sort+sweep (planner core) | the ordering/interval algorithm itself, independent of merge decision | pure userspace bookkeeping, O(k log k) |
| F4 over-read threshold T | the policy knob of F1: max gap in bytes the planner is willing to pay for one saved syscall | sets amplification; the exchange-rate trade-off (T vs saved syscall) |
| F5 merged-result dispatch | reconstruction of per-request outcomes (counts, EOF, prefix, errors) from merged syscall results | userspace slicing + outcome reconstruction logic |
| F6 amplification/reduction accounting | the measurement discipline tying F1/F2 to physical work | counters, not semantics |

The kernel side is fixed and does no planning of its own: every `::pread` is
one independent positioned transfer ("reads up to count bytes … the file offset
is not changed", pread(2), §0.2). All merging, dedup, ordering and dispatch
work in this family is userspace policy on top of per-syscall positioned reads.

## 2. Legal framework vs the v2-r1 public contract

### 2.1 The per-call baseline the family must not disturb

Today's public Direct read is **one span per call with per-call admission**:
`read_at(file, offset, dst)` runs `precheck` (SEM-03 precedence: closed →
access → zero-length no-op → range; `src/blocking_file.cpp:30-37`, contract
`docs/explicit-io-contract.md:408-429`), then a single `::pread` with EINTR
retry that never replays a completed count (`src/blocking_file.cpp:100-102`;
SEM-05 `docs/explicit-io-contract.md:469-473`). There is no state carried
between calls, no offset history, no request table — the library has no legal
place to notice that two calls are "near" each other. Consequences:

- Admission, result, error and EOF semantics are **per public call**
  (SEM-03/04/05; P0 §5.1 rows 1–10).
- Positional ops never touch the shared cursor (SEM-04
  `docs/explicit-io-contract.md:439-440`; pread(2) confirms the kernel side).
- No cross-call serialization order and no atomicity/snapshot guarantee is
  promised (SEM-04 `docs/explicit-io-contract.md:453-457`; THREAD-01
  `docs/explicit-io-contract.md:1016`).

Therefore any range planning **inside the primitive or library** would (a)
invent hidden cross-call authority state (mechanism-justification gate,
AGENTS.md), (b) make per-call admission and EOF/error attribution depend on
call history — an observable contract change, and (c) be exactly the
"implicitly cache/merge/reorder" prohibition of MISSION §5.1 M2
(`MISSION.md:498-502`).

### 2.2 Inherited three-case discipline (from M1) and where M2 sits

M2 inherits M1's case discipline (`P3_MECHANISM_STUDY_PLAN.md:71-109`) because
M2's mechanisms act on multi-request shapes:

| Case | Definition | Relation to M2 |
|---|---|---|
| **A — file range contiguous × user buffers scattered** | k destination buffers covering one contiguous extent | Not M2's subject (that is vectored I/O, M1). Appears inside M2 only as a possible *dispatch implementation* of a zero-gap merged extent (see M2-OQ1/M2-OQ8): a contiguous merged run could in principle be filled directly via `preadv` with gap-sink iov entries, eliminating staging copies. Any such arm is a plan amendment, not an assumption. |
| **B — file ranges scattered (disjoint offsets)** | k requests at unrelated offsets | **M2's subject.** The plan makes M2-style merge+over-read an explicitly legal Case-B form, but *only* "under an explicit amplification/consistency contract" (`P3_MECHANISM_STUDY_PLAN.md:95-98`). The merged covering read reads gap bytes the caller did not request — legal iff delivered bytes, per-request outcomes and error attribution stay exactly per-request, and the amplification is contract-declared (§4). |
| **C — independent operations: ordering and error responsibility** | each op owns admission, result, error, EOF/partial semantics | **Constraint on every M2 batch form.** Per-op result and error attribution must survive (plan Case C, `P3_MECHANISM_STUDY_PLAN.md:100-108`): one member's `invalid_argument` must not poison or vanish behind another's success; reordering side-effecting (write) ops across each other is forbidden unless independence is declared — which is why write-side planning is out of P3 scope (D-P3-04, `P3_MECHANISM_STUDY_PLAN.md:526`). |

One ordering nuance pinned for reads: since SEM-04 promises *no* cross-call
serialization order, issuing the merged/sorted preads in sorted-extent order
rather than submission order is **not** a contract violation for a read-only
set (reads have no inter-op file side effects and do not move the cursor). It
does change the observation-point profile under a concurrent writer —
disclosed in the divergence cell records (M2-OQ7), not claimed either way.

### 2.3 The three legal forms (exhaustive, per plan §3.1)

1. **Explicit caller composition** — the application plans ranges and issues
   merged reads itself (raw `pread` or Sluice `read_at` into its own staging),
   owning amplification and consistency consequences. Legal today; needs
   nothing from Sluice; captures the win *outside* the SDK.
2. **Independent Host policy** — a separate host-side layer outside the Direct
   primitive family (e.g. a future dududu-side planner), again explicit.
   Legal; the SDK stays untouched.
3. **A normatively designed new capability** — a batched/gathered read
   facility with its own contract, adopted only via Owner decision
   (MISSION §5.3; plan §11). This is the only form that can capture the
   mechanism *inside* Sluice (§4).

The P3 probe arms (`rpl0`/`rplT`/`rplD`, plan §3.3) are none of these three —
they are **measurement instruments** that quantify the opportunity and test the
proof obligations at probe level; they are not product forms and never enter
the product tree (plan §1.3).

### 2.4 The five proof obligations, answered one by one

Plan §3.2 (`P3_MECHANISM_STUDY_PLAN.md:218-224`) defines each obligation with a
probe-level oracle. This section states, for each: the contract meaning, the
oracle, the pass criterion pinned now, and the classification of a violation.

**Obligation 1 — Input request-set identity.**
*Contract meaning:* the planner's input is exactly the caller's request set;
planning may transform *execution* (syscalls issued) but must not invent,
drop, resize or reorder-with-effect the logical requests. Any silent mutation
of the request set would be hidden authority over caller intent.
*Oracle:* the request set R is generated deterministically (seeded LCG over
offsets/sizes/gaps; traces `g4k`, `ov30`, `eofmix`, plan §3.3); both arms
consume byte-identical request streams generated once and replayed; a digest
of the request set is recorded in the cell header.
*Pass criterion (pinned):* the recorded digest is identical across both arms
of every cell and across all repeats; run-integrity gates refuse the cell
otherwise (plan §7.1 run-integrity class).
*Violation classification:* digest mismatch ⇒ the cell is invalid (not a
finding about the mechanism); a *product* form that mutates the request set
would be `SEMANTICALLY_NOT_EQUIVALENT` (§2.5-e).

**Obligation 2 — Per-caller byte correctness.**
*Contract meaning:* over-read may change bytes *transferred* (issued caliber)
but never bytes *delivered* per request. This is the amplification contract's
core: `delivered(rᵢ)` must equal what an independent serialized replay of rᵢ
alone would produce on the same static file.
*Oracle:* a non-timed reference pass in the same process replays R serialized
and independent (no planning), computing each request's expected bytes; the
equality gate is **per request** (checksum of dstᵢ), never per group
(plan §3.3 checksum-group discipline).
*Pass criterion (pinned):* every rᵢ in every cell — including every T of the
sweep, dedup, EOF and divergence cells — matches its per-request reference
checksum on a static fixture (fixture sha unchanged, plan §7.1).
*Violation classification:* any per-request mismatch ⇒ cell invalid and family
blocked until understood (PRED-M2-6; plan §3.5); a product form delivering
over-read bytes or wrong slice offsets is `SEMANTICALLY_NOT_EQUIVALENT`
(§2.5-b).

**Obligation 3 — Per-item result and error attribution.**
*Contract meaning:* SEM-05 outcomes are per call: nonempty read returning 0 is
EOF *success*; 0 < n ≤ requested is successful progress; short counts are
allowed and the primitive does not loop (`docs/explicit-io-contract.md:459-473`).
After merging, these outcomes must be *reconstructed per request*, not
reported per merged group. The distinctive merged-form hazards: (i) a merged
covering extent that crosses EOF returns a short count (pread(2): "not an
error … fewer bytes than requested"; zero indicates EOF) — a request wholly
past EOF must still be reported as count-0 EOF success, and a straddling
request as exactly its prefix; (ii) one merged syscall's OS error must be
attributed to the affected requests by an explicitly declared rule, not
fused across the batch (Case C).
*Oracle:* EOF-boundary traces (`trace=eofmix`, cell `p3-m2-eof-attribution`):
per-request prefix-length equality plus per-request checksum equality across
{rpl0, rplT}; injected short-read shapes (fixture-tail pattern) attribute per
request (plan §3.2 row 3).
*Pass criterion (pinned):* per-request counts and prefix lengths identical
between rpl0 and rplT on eofmix; a count-0 request is distinguishable from a
short-prefix request in the recorded trace (the oracle must log both, not
just checksums — M2-OQ5/M2-OQ7-adjacent kit requirement, stated here so the
executor builds it).
*Violation classification:* EOF-attribution divergence ⇒ `SEMANTICALLY_NOT_EQUIVALENT`
at probe level (§2.5-d); error fusion ⇒ §2.5-c.

**Obligation 4 — No crossing side-effect/lifetime boundaries.**
*Contract meaning:* merging must not cross any boundary where the contract
attaches meaning to *when* or *how much* work happened: durability coverage
(SEM-06 covers confirmed writes by completion ordering — merging writes would
move completion points; hence **write-side planning is out of P3 scope**,
deferral D-P3-04), the shared cursor (positional reads never move it —
SEM-04; pread(2) verified), and File/buffer lifetime (the planner holds only
borrowed spans for the duration of one call; no File ownership or borrow
change — LIFE-01 class, P0 §5.1 last row).
*Oracle:* read-only family by construction: no write arms merge across a
sync/durable boundary in P3 scope (plan §3.2 row 4); buffer/file lifetime is
structural (call-shape enumeration certifies the planner takes spans, keeps
no state after return).
*Pass criterion (pinned):* the family introduces zero write-side arms; the
planner certification shows no retained state (no cross-call caching — §2.5-a)
and no cursor mutation (all issued reads are `::pread`, never seek+read; the
hidden seek-plus-read lowering is non-conforming, SEM-04
`docs/explicit-io-contract.md:440-441`).
*Violation classification:* write-merge across a sync boundary would break
SEM-06 coverage observables; cursor movement breaks SEM-04; both are
`SEMANTICALLY_NOT_EQUIVALENT` (§2.5-f).

**Obligation 5 — Concurrent writes, file changes, cancellation, partial errors.**
*Contract meaning:* the contract already promises **no** cross-call
serialization order and **no** atomicity/snapshot guarantee for overlapping
writes or concurrent resize (SEM-04 `docs/explicit-io-contract.md:453-457`).
Merging does not violate that promise — but it *changes the observable
tearing granularity*: k separate reads observe the file at k points in time;
one merged covering read observes it at one point. Under a concurrent writer
the two forms can legitimately deliver different bytes for the same request
set, on a file whose size is concurrently changing. Neither form is "wrong";
the difference is the hazard magnitude that a design candidate must disclose.
Cancellation and partial errors *under batching* (planner error mid-set,
merged syscall failing after some requests dispatched) do not exist in the
per-call world and are therefore **contract questions for the design
candidate** (per-request confirmed bytes + effect certainty under ERR-02
discipline — `include/sluice/blocking/file.hpp:40-49` carries the
EffectCertainty shape today's compositions use), documented, not simulated.
*Oracle:* descriptive divergence cell `p3-m2-concurrent-divergence`: a writer
process mutates merged extents between merged and unmerged replays; the
observable byte difference is recorded as the semantic hazard magnitude,
**no CI claim** (plan §3.2 row 5; §13.6).
*Pass criterion (pinned):* the cell records (a) the writer cadence (M2-OQ4),
(b) the merged arm's issue order (sorted extents) vs the unmerged arm's
(request order) — both legal (§2.2), and (c) the per-request byte deltas
between the two replays, as a descriptive record. The static-file cells
(Obligations 1–3) remain the only claim-bearing correctness gates.
*Violation classification:* none available by design (descriptive); but
promoting the merged form to a product **without** disclosing the
observation-point change would be an omission the §11 candidate must close —
flagged, not measured.

### 2.5 Illegal forms — SEMANTICALLY_NOT_EQUIVALENT candidates (with reasons)

| # | Form | Why illegal against v2-r1 |
|---|---|---|
| a | **Implicit merge/dedup/reorder inside the Direct primitive or library** (e.g. `read_at` internally noticing nearby offsets across calls, or memoizing earlier reads to serve later overlapping ones) | Makes per-call admission, result, EOF and error semantics depend on hidden cross-call state; violates the exhaustive legal-forms list (MISSION §5.1 M2, `MISSION.md:498-504`); invents runtime authority without an owning fact (AGENTS.md mechanism-justification gate); cross-call caching additionally changes the observation-point observable without any contract declaring it. Note the boundary: **intra-set** dedup within one submitted request set is a legal composition/capability shape; **cross-call** memoization is not (M2-OQ6). |
| b | **Over-read delivered or visible to the caller** (delivered bytes ≠ exact per-request reference; slice offsets wrong; issued-count reported as a request's count) | Changes the public observable `delivered(rᵢ)` / result counts; breaks Obligation 2 and SEM-05 result shapes. |
| c | **Error fusion** (one member's `invalid_argument`/admission rejection poisoning the batch, or one merged syscall's OS error reported for all members) | Breaks per-op error responsibility (Case C); breaks SEM-03 per-call admission precedence if a rejected member changes another member's verdict. |
| d | **EOF mis-attribution** (merged short count reported as a group error; a wholly-past-EOF request reported as error or as nonzero; straddling request given more/less than its prefix) | Breaks SEM-05 EOF/short semantics (`docs/explicit-io-contract.md:462-467`) reconstructed per request (Obligation 3). |
| e | **Request-set mutation** (planner drops a "duplicate" request whose *destination* differs, reorders into submission with different per-request results on a mutating file while claiming equivalence, resizes requests) | The caller's logical work is N requests; anything that changes the logical request set changes the compared workload, not the execution (MISSION §4.2 "禁止…人为改变原本应比较的逻辑工作量"). |
| f | **Write-side merging / reordering across sync or durability boundaries; any merged op moving the shared cursor** | SEM-06 coverage is defined by completion ordering of confirmed writes — merging moves completions; SEM-04 forbids cursor movement and hidden seek lowerings. Out of P3 scope entirely (D-P3-04). |
| g | **Merged executor that loops to satisfy exact counts** (retrying a merged read until all covered requests are full, hiding EOF) | SEM-05: a primitive does not loop to satisfy an exact/all promise; hiding an EOF behind a loop changes the reported outcome class. The merged executor must be primitive-class (return short counts; per-request reconstruction on top). |

### 2.6 Cross-family interfaces (scope of this document)

This document is the M2 family analysis; the sibling analysts own M1/M3/M4
full treatments. What M2 needs from them:

**2.6.1 From M1 (dependency).** The three-case discipline (§2.2) and, for any
future dispatch-side composition, the Case-A `preadv` machinery. M1-Q5's
Case-B ceiling is *measured by M2's cells* (`P3_MECHANISM_STUDY_PLAN.md:122-126,
188`): the k×pread − 1×covering-read spread is exactly the rpl0-vs-rplT
comparison. No M1 arm is duplicated here.

**2.6.2 From M4 — perception dimension matrix (M2-relevant slice).** M4
"produces environmental constants and the syscall-vs-byte exchange rate R that
parameterizes M2's threshold model" (`P3_MECHANISM_STUDY_PLAN.md:358-360`).
The M2-relevant dimensions:

| M4 dimension | What it parameterizes in M2 | Direction pinned (pre-measurement) | Evidence cells | Prediction links |
|---|---|---|---|---|
| Cache state (warm → cold) | R = per-saved-syscall ns / marginal ns-per-byte ⇒ T* and the whole merge trade-off | T* shrinks sharply cold (≈2.1 ns/B ⇒ break-even ≤ ≈330 B vs ≈5.4 KiB warm) | `p3-m2-merge-cold-g4k` vs `p3-m2-merge-sweep-g4k` | PRED-M2-3; PRED-M4-2/4 |
| Filesystem (tmpfs → btrfs) | marginal byte cost and fixed per-syscall class both shift; R ordering | R_warm_tmpfs > R_warm_btrfs > R_cold_btrfs; warm-btrfs wins smaller at equal T | same cells + M4 fs cells | PRED-M4-4 |
| Request size | syscall amortization already present in large requests; dispatch-copy term grows linearly in requested bytes | merge win concentrates in the small-request regime (g4k's 4 KiB class); large-request cells should show ≤ noise | `g4k` trace shape (fixed 4 KiB); M4 size sweep informs | PRED-M2-1 context |
| Working set | staging + destinations residency decides the dispatch-copy rate | probe shapes (≈8×4 KiB + gaps ≈ 60–70 KiB/op-group) sit far below L3 25 MiB ⇒ cache-resident copies; larger sets would pay DRAM rates | probe construction; M4 ws cells | limitation note (§5, M2-OQ3) |
| Derived exchange rate R | the constant in the T* model | — | `p3-m4-exchange-rate` (derived) | PRED-M2-2 |

**2.6.3 From M3 (non-overlap, one shared element).** M3's seven mandatory
disclosures (`P3_MECHANISM_STUDY_PLAN.md:293-303`) govern file-to-file kernel
transfer and belong to the M3 analysis; M2 has no file-to-file component. The
single shared element is disclosure 4 (concurrent-modification consistency),
whose discipline is identical to M2 Obligation 5's descriptive divergence cell
(no CI claim; contract question to the design candidate). No M3 disclosure is
re-analyzed here.

## 3. Preregistered predictions and probe criteria

All predictions below are pinned before measurement (§0.3). Caliber rules:
integral counts via `strace -c` 7.2 with window isolation, exact across ≥3
diagnostic repeats; byte counters are probe-internal deterministic counters
(argument caliber, §1); wall claims require 95% t CI excluding 0 on 16 ABBA
pairs **and** |median| ≥ the planner track's fresh A/A MDE′ (plan §7.2); no
count number ever feeds a wall verdict.

### 3.1 Plan-pinned set (PRED-M2-1…6, restated verbatim from `P3_MECHANISM_STUDY_PLAN.md:256-274`)

| ID | Prediction | Falsifiable form |
|---|---|---|
| PRED-M2-1 (merge win) | g4k warm tmpfs at T=8 KiB: syscalls/op-group 8.00 → ≤ 3.00; wall/op-group reduction ≥ 30% with CI excluding 0; byte amplification ≤ 2.0× | CI including 0 at every T (direction refuted → family at best NO_CHANGE_JUSTIFIED at wall caliber / COST_MIGRATION_ONLY per plan §8); amplification > 2.0× refutes the trace/threshold shape assumption |
| PRED-M2-2 (break-even threshold) | T*(warm tmpfs) ∈ [1 KiB, 16 KiB], T* = smallest T whose wall delta is CI-excluding negative; grounding 0.128 ns/B vs 550–700 ns/syscall ⇒ ≈5.4 KiB | Measured T* outside the bracket, or no T with a CI-negative delta |
| PRED-M2-3 (cold shift) | T*(cold btrfs) ≤ T*_warm/8 (grounding ≈2.1 ns/B ⇒ ≤ ≈330 B) | Cold T* > warm T*/8 |
| PRED-M2-4 (dedup) | ov30: bytes transferred −30% ± 5pp; syscalls/op-group −25% or better; wall −15% or better, CI-excluding | Any leg outside its band; CI including 0 on the wall leg |
| PRED-M2-5 (dispatch overhead bound) | At equal syscall counts (T=0, adjacent-only merges), planner's own userspace cost ≤ 100 ns/op-group vs rpl0; if dispatch overhead ≥ 50% of the syscall saving at the winning T, verdict degrades to COST_MIGRATION_ONLY | Arm wall delta > 100 ns/op-group at T=0 — **but see PRED-M2-E/OQ1: as written this is confounded unless the executor separates bookkeeping from dispatch data movement** |
| PRED-M2-6 (correctness) | All per-request checksum gates green in every cell including eofmix | Any mismatch ⇒ cell invalid, family blocked (P1 §5.9 precedent) |

### 3.2 Mechanism-shape additions (PRED-M2-A…F, pinned this session)

| ID | Prediction | Falsifiable form |
|---|---|---|
| PRED-M2-A (copy structure) | Per merged extent, rplT/rplD perform exactly one kernel→staging transfer of the issued length plus exactly one userspace memcpy per covered request of exactly its requested length; rpl0 performs one kernel→caller transfer per request and zero userspace data copies | Structural certification (call-shape enumeration) or probe copy counters disagree; if the implemented rplT direct-dispatches (e.g. preadv with gap sinks) instead of staging+memcpy, the arm deviates from the plan §3.3 arm definition and requires a numbered amendment |
| PRED-M2-B (sweep monotonicity) | On a fixed trace: issued bytes are monotone non-decreasing in T, merged-group count (hence syscalls) monotone non-increasing in T; both integral counts repeat exactly across ≥3 diagnostic runs | Any repeatable non-monotone counter (would indicate nondeterministic planning — a kit defect, cell invalid) |
| PRED-M2-C (dedup accounting) | On ov30: rplD's extra-bytes counter ≤ 0 (the union never over-reads: issued = union bytes = requested-sum − duplicated bytes); syscall count = number of disjoint union components ≤ k | Positive extra bytes, or syscall count above k, or non-integral repeats |
| PRED-M2-D (EOF reconstruction) | On eofmix at T=4K, per-request outcomes of rplT equal rpl0's: wholly-past-EOF requests → count 0 (EOF success), straddling → exact prefix; no request inherits an error from the merged short count | Any per-request divergence in count, prefix length or checksum |
| PRED-M2-E (T=0 delta is dispatch-dominated) | rplT(T=0) − rpl0 wall delta will be positive and ≈ requested_bytes × single-core copy rate — structurally ≥ ~500 ns/op-group at 32 KiB, because satisfying ≤100 ns would require 32768 B / 100 ns = 327.68 GB/s effective single-thread copy bandwidth, which no E5-2666 v3 core reaches. Hence PRED-M2-5's ≤100 ns bound is testable **only** if the executor separates bookkeeping from dispatch (T=0 arm direct-dispatches, or a plan-only diagnostic arm is added by amendment — M2-OQ1) | A staging arm showing ≤100 ns/op-group T=0 delta would refute the copy-cost model itself (a finding, recorded as such) |
| PRED-M2-F (model-bias direction) | Measured T* (if any T wins) ≤ the two-term model's ≈5.4 KiB point estimate, because the omitted terms (dispatch copies, staging touches) only subtract from merge gains; win should decay beyond the bracket's top | T* > 8 KiB, or wins growing monotonically through T=16 KiB — implying a missing *gain* term in the exchange-rate model (to be investigated, not absorbed) |

Qualitative syscall/copy/step summary pinned by the above: at the winning T,
the mechanism trades (k − m) syscall entries [≈550–700 ns each class,
`DIRECT_COST_FLOOR.md:220`] plus their per-call fixed paths, against Σ merged
gap bytes × marginal byte cost [≈0.126–0.130 ns/B recomputed, §0.4] plus one
extra userspace copy of every requested byte (PRED-M2-A) plus O(k log k)
bookkeeping. Steps per op-group: rpl0 = k × (admission + pread); rplT =
sort+sweep + m × pread + k slice-dispatches. Whether this nets positive is
exactly what the T-sweep decides (M2-Q1/Q2); the plan's own accounting rule
already forbids claiming a win if only syscalls fell while attributed total
work did not (plan §6 closing remark, §8 COST_MIGRATION_ONLY).

## 4. New public capability: needed, and the §5.3 six-element design points

**apiCandidateNeeded = true** — conditionally, on the evidence-gated path
exactly as the plan frames it ("If (and only if) a P3 mechanism shows a real
opportunity that current public APIs cannot legally express",
`P3_MECHANISM_STUDY_PLAN.md:485-487`). The reasoning: the merge/dedup
opportunity is not expressible through today's single-span-per-call API inside
the SDK; legal forms 1 and 2 (§2.3) capture it outside the SDK; only form 3
captures it inside Sluice, and MISSION §1 forbids auto-adoption ("新增公开能力须提交独立设计/ADR 候选供 Owner 裁决"). If the probes show no
CI-excluding win at any T or caliber, no candidate is filed and the answer
collapses to NO_CHANGE_JUSTIFIED. Design points per MISSION §5.3
(`MISSION.md:562-576`) / plan §11, prepared now so the candidate is ready:

1. **New Operation Contract.** A gathered positioned-read facility, shape
   class: `read_gather(file, span<ReadRequest const>, execution policy) ->
   per-request results`, where ReadRequest = {offset, length, destination
   span} and the policy declares merge threshold T and dedup (default T=0,
   dedup on — policy is *caller-visible*, never hidden). Per-member admission
   follows SEM-03 precedence *per member* (a zero-length member is a logical
   no-op that skips offset/range checks — mirroring
   `blocking_read_at_empty_buffer_ignores_unrepresentable_offset`, P0 §5.1);
   results are per member: {count, EOF-at-observed-position, admission
   rejection, OS error with retained native detail}; the executor is
   primitive-class (no loop-to-full; short counts surface per member — §2.5-g).
   The planner is a pure function of (request set, policy): stateless per
   call, no cross-call memoization (M2-OQ6).
2. **Relation to v2-r1.** Extends SEM-03/04/05 *per-call* semantics to a
   *per-member* discipline for this one operation; SEM-04 position rules
   unchanged (positioned reads only; cursor untouched); SEM-06 untouched
   (read-only); THREAD-01 unchanged (no new cross-call serialization — the
   operation itself is one call); ERR-02 effect-certainty carried per member
   (`include/sluice/blocking/file.hpp:40-49` shape); W-01/INV-01: must remain
   directly callable without any context (no IoContext/request-table
   dependency — `docs/explicit-io-contract.md:225,309`). No existing contract
   text is rewritten; `read_at` semantics stay exactly as today.
3. **Performance opportunity requiring it.** The M2 cells' §5.2 tables:
   T-sweep win (M2-Q1/Q2), dedup win (M2-Q4), the M1-Q5 Case-B ceiling, and
   the dispatch-overhead bound (M2-Q5) — each named with cell, caliber and
   CI status at filing time. No microbenchmark-only adoption (MISSION §5.3).
4. **Semantics / errors / lifecycle / resource boundaries / portability.**
   (a) Amplification contract: issued bytes ≤ requested + k·T (declared,
   bounded); delivered bytes exactly per request (Obligation 2); (b) EOF
   reconstruction rules per member (Obligation 3, PRED-M2-D); (c) error
   attribution per member; a merged syscall's OS error maps to the members
   whose extents intersect the failed transfer, with unknown-effect flags for
   uncounted attempts (ERR-02 discipline — cancellation/partial-error under
   batching is *this* clause, resolving Obligation 5's open contract
   question); (d) observation-point disclosure: a merged extent is one kernel
   observation; no atomicity/snapshot promise, consistent with SEM-04's
   existing no-snapshot language (`docs/explicit-io-contract.md:505-509` uses
   the same discipline for sync coverage); (e) buffer lifetime: destination
   spans are borrowed for the call's duration (same as today's `std::span`
   arguments); merged-extent arithmetic uses checked operations per SEM-04
   class (a merged end never exceeds max member end, but planner arithmetic
   must not wrap); (f) fallback: the T=0/dedup-off policy *is* the plain
   per-member pread loop — no silent fallback path exists to hide; (g)
   portability: pure POSIX `pread` composition, no Linux-specific primitives
   required (a `preadv` dispatch optimization, if ever, is internal and
   Case-A-shaped only); WSL2 follow-up per MISSION §8.3.
5. **Design Issue / ADR Candidate.** Filed as such, referencing the P3 cell
   evidence and this document's §2/§3; includes the SEMANTICALLY_NOT_EQUIVALENT
   catalog (§2.5) as the negative space the contract must keep.
6. **Awaiting Owner.** Explicit wait state; nothing auto-adopts; any PR stays
   OPEN (MISSION §1).

## 5. Open questions

Plan question list (registered, to be answered by the measurements — quoted
from the plan/ask):

- **M2-Q1 (merge win):** on the g4k trace warm tmpfs, what syscall and wall
  reduction does threshold merging buy at each T, at what byte amplification?
- **M2-Q2 (break-even):** where is T*(warm tmpfs), the smallest threshold
  whose wall delta is CI-excluding negative, and does it match the
  exchange-rate model (≈0.128 ns/B marginal byte cost vs ≈550–700 ns per
  saved syscall ⇒ ≈5.4 KiB)?
- **M2-Q3 (cold shift):** how much does T* shrink on cold btrfs (extra bytes
  cost ~2.1 ns/B ⇒ predicted ≤ ≈330 B, ≥ 8× shrink)?
- **M2-Q4 (dedup):** how much do overlap/duplicate elimination reduce bytes,
  syscalls and wall on a 30%-duplicate trace?
- **M2-Q5 (dispatch overhead):** what is the planner's own userspace cost at
  equal syscall counts, and does it stay ≤ 100 ns/op-group (else the mechanism
  degrades toward COST_MIGRATION_ONLY)?
- **M2-Q6 (correctness):** do all per-request checksum and EOF-attribution
  gates stay green in every cell, including under the concurrent-writer
  divergence cell's recorded hazard?

Additional open questions identified by this analysis (measurement-independent):

- **M2-OQ1 (PRED-M2-5 confound — kit-definition decision needed before the
  T-sweep is read):** the plan's rplT arm is "one `::pread` per merged extent,
  then in-memory slice dispatch" (`P3_MECHANISM_STUDY_PLAN.md:236-238`). At
  T=0 that arm still stages+slices, so its wall delta vs rpl0 contains the
  dispatch copies of all requested bytes, which PRED-M2-E shows cannot fit in
  100 ns at 32 KiB/op-group. Either the T=0 arm must direct-dispatch
  contiguous merges (a `preadv`-shaped dispatch — deviates from the arm
  definition) or a plan-only diagnostic arm (sort+sweep executed, unmerged
  issue) must be added — both via a numbered plan amendment, before the
  T-sweep cells are read; otherwise PRED-M2-5 is untestable as written.
- **M2-OQ2 (model completeness):** the two-term exchange-rate model omits the
  dispatch-copy and staging-touch terms (PRED-M2-A/E/F). M2-Q2's "does it
  match the model" answer must be read against the corrected cost identity,
  not only the two-term grounding.
- **M2-OQ3 (missing constant):** the host's single-core memcpy rate for
  L2-resident ~64 KiB buffers is not pinned by any inherited P1/P2 fact; M4's
  cells (or a one-off diagnostic) should derive it to decompose dispatch cost.
- **M2-OQ4 (divergence-cell interpretability):** the writer cadence must be
  recorded in `p3-m2-concurrent-divergence` for the hazard magnitude to be
  comparable across reruns (descriptive cell, no CI).
- **M2-OQ5 (batch admission shapes not probed):** zero-length members with
  unrepresentable offsets, per-member admission rejections, and injected
  mid-batch OS errors are design-candidate obligations (§4.1/§4.4c) with no
  probe trace today; the primitive-level oracles (P0 §5.1) cover the per-call
  forms only. If the candidate advances, these need their own oracle
  definitions before any product change.
- **M2-OQ6 (dedup-vs-caching boundary):** any candidate contract must state
  explicitly that dedup is intra-set (stateless per call); cross-call
  memoization remains illegal for the SDK (§2.5-a).
- **M2-OQ7 (issue-order disclosure):** merged arms issue preads in
  sorted-extent order, unmerged in request order — both legal (§2.2); the
  divergence-cell records must disclose which order each arm used.
- **M2-OQ8 (M1×M2 composition, future):** a `preadv` dispatch with gap-sink
  iov entries could eliminate the staging copy for merged extents entirely
  (Case-A machinery applied inside an M2 merge). Not in the preregistered arm
  set; only reachable via amendment if the dispatch-copy term (PRED-M2-E)
  turns out to dominate the mechanism's economics.
