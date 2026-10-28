# Structural I/O Mechanism Study (P3 Synthesis)

- Mission: `research/direct-beyond-parity/MISSION.md` §5 (P3 — Beyond-Parity
  Structural I/O Research). Preregistered plan of record:
  `research/direct-beyond-parity/P3_MECHANISM_STUDY_PLAN.md` (families §2–§5,
  gain-accounting protocol §6, statistics §7, verdict vocabulary §8,
  real-workload admission cap §9, API decision path §11, deferrals §12,
  limitations §13).
- Family inputs (preregistered, pre-measurement):
  `p3-families/M1_ANALYSIS.md`, `p3-families/M2_ANALYSIS.md`,
  `p3-families/M3_ANALYSIS.md`, `p3-families/M4_ANALYSIS.md`.
- Machine-work deltas per mechanism: `IO_WORK_REDUCTION_LEDGER.md` (MISSION
  §5.2 tables). Design candidates filed for Owner decision:
  `API_CAPABILITY_CANDIDATES.md`.
- **Status: MEASURED.** All numbers below were produced by the P3 execution
  session of 2026-10-28 on host E5 under the preregistered calibers; they are
  marked per the campaign legend. A post-session independent review found
  transcription defects in several single-arm values and two derived-constant
  groups (wrong field parsed, smoke-gate request set quoted for timed cells,
  single-sample/dimension-mixed ratios); every affected number has been
  re-aligned to the archived `p3-raw` analysis blocks — see the
  post-session transcription-correction record in §5 (item 8). All paired
  deltas (Δ) and CIs were re-verified exact.
  - `[RUN]` — measured or certified in the P3 execution session; raw evidence
    file cited.
  - `[TRANSCRIBED]` — copied from an archived prior-phase record (P0/P1/P2 or
    a session-verified man page), source cited.
  - `[UNCERTAIN]` — not pinned by any executed check in this campaign; stated
    as such, never guessed.
- Execution identity `[RUN]`: kit branch `direct-cost-ladder`, HEAD
  `4455d98d9823ec138c904be4c63a1d5a84903e97`, ladder binary sha256
  `5a53a9ad1ea00a9a26dee98543ef473ca15dcdfda86c2aa647b59cca4ec05507`, smoke
  gate PASS before any timed cell (`p3-raw/kit/smoke-ladder.txt`,
  `p3-raw/kit/ladder-identity.txt`, `p3-raw/env-pin.txt`). Read fixture
  sha256 `bc020fd1…b2993a` identical before and after the session
  (`p3-raw/env-pin.txt:15`, `p3-raw/fixtures.txt`). Run-integrity gates held
  throughout: exit 0, `verify_fail=0`, all cross-arm checksum-equality groups
  single-valued (the M2 sweep's dual `checksum` value is the kit's documented
  delivered-vs-issued double caliber, not a gate failure). 32 samples per wall
  cell (16 ABBA pairs), `taskset -c 2`, serial-exclusive within the session's
  own invocations. **Host-quiescence caveat, disclosed, unresolved:** the
  archived pin records low load (uptime 1-min average 0.09,
  `p3-raw/env-pin.txt:9`) but also two observations that conflict with a
  strictly quiescent host: `concurrent_measure_procs=3` at pin time
  (`env-pin.txt:10`) and `measure_proc_check: 1 matching measurement
  processes (expect 0)` (`env-pin.txt:18`). Neither observation was explained
  or adjudicated during the session; the plan §7.3 quiescent-host condition is
  therefore not fully evidenced by the pin record. No cell shows an
  integrity-gate failure, all paired CIs are tight, and every count caliber
  repeated exactly across ≥3 diagnostics, but any wall magnitude in this study
  carries this unresolved environment caveat.
- Track MDE′ floors (fresh A/A per new track, plan §7.2) `[RUN]`: vectored
  32.2 ns (`p3-raw/m1/p3-m1-aa-vectored-4k.txt`, sd 60.594 → 2.131·sd/√16);
  planner warm 42.2 ns (`p3-raw/m2/p3-m2-aa-planner-g4k.txt`, sd 79.197);
  planner cold 16 544 ns (`p3-raw/m2/p3-m2-aa-planner-cold.txt`, sd 31 053);
  copy 122.9 ns (`p3-raw/m3/p3-m3-aa-copy.txt`, sd 230.749); M4 read session
  9.51 ns (dual-reported with the pinned inherited 5.535 ns,
  `p3-raw/m4/p3-m4-aa-read-4k.txt`, sd 17.864).

## 0. Verdict summary

| Family | Overall verdict | Sub-verdicts carried in the record |
|---|---|---|
| M1 — vectored I/O | `CONDITIONAL_WORKLOAD_WIN` | Case A read+write `MECHANISM_WIN_EARNED` at probe level; `REQUIRES_API_DECISION` (single-span public API cannot express the form); Case B/C illegal forms `SEMANTICALLY_NOT_EQUIVALENT`; §9 cap (#578 R0 incomplete) |
| M2 — range planning | `CONDITIONAL_WORKLOAD_WIN` | dedup `MECHANISM_WIN_EARNED` at probe level + `REQUIRES_API_DECISION`; threshold merge recorded `COST_MIGRATION_ONLY` (preregistered PRED-M2-5 trigger fires at T\* = 4 KiB — dispatch overhead ≥ 50% of the winning-T saving under both evidence-internal readings; plan §8 mandatory, recorded despite the CI-excluding wall win at T\*); PRED-M2-1 wall clause falsified at T ≥ 8 KiB; cold shift `NOT_RESOLVED`; §9 cap |
| M3 — kernel file-to-file transfer | `CONDITIONAL_WORKLOAD_WIN` | btrfs cfr `MECHANISM_WIN_EARNED` at probe level (recorded out-of-band-upper as-is); tmpfs cfr `MECHANISM_WIN_EARNED` at probe level (P-M3-3's CPU −30% prediction falsified at −6.9% — a falsified prediction, not a verdict criterion); `REQUIRES_API_DECISION` (explicit copy capability); forms F1–F4 `SEMANTICALLY_NOT_EQUIVALENT`; §9 cap |
| M4 — filesystem/hardware awareness | `NO_CHANGE_JUSTIFIED` | by design: M4 proposes no mechanism; environmental constants delivered and confirmed inside preregistered bands; no SDK change is proposed or justified by M4 evidence |

Every mechanism-level win above is probe-level evidence on synthetic request
shapes only. No P3 conclusion claims real-workload benefit; the automatic cap
of plan §9 applies because `#578` R0 (fio + DuckDB baseline/admission)
remains incomplete — no admitted real-workload evidence exists anywhere in
this mission (`P3_MECHANISM_STUDY_PLAN.md:458-474`). No microbenchmark speed
in this study extends the SDK (MISSION §5.3); the three productization paths
are filed as design candidates and wait for the Owner
(`API_CAPABILITY_CANDIDATES.md`).

## 1. Family M1 — Vectored I/O (`preadv` / `pwritev`)

### 1.1 Legal framework (preregistered, `p3-families/M1_ANALYSIS.md` §2)

The v2-r1 public Direct contract is one span per call
(`include/sluice/blocking/file.hpp:14-21`; SEM-03 admission precedence at
`docs/explicit-io-contract.md:398`, SEM-04 positional semantics at `:437`,
SEM-05 short-I/O/EOF at `:459`). Three cases are kept strictly distinct:

- **Case A — file range contiguous × user buffers scattered.** One
  `preadv`/`pwritev` expresses exactly one contiguous extent `[off,
  off+Σlen)` filled into k buffers in array order, with zero intermediate
  copies (kernel fills the iov directly; glibc ≥ 2.20 wrappers are direct
  syscalls, host glibc 2.43), no shared-cursor movement (SEM-04-conformant),
  short count = prefix fill across iov boundaries (SEM-05-conformant), EOF-0
  = success. Nine equivalence obligations are pinned in the family document
  (whole-extent admission once; Σlen=0 logical no-op with no syscall; checked
  `offset+Σlen`; EINTR retried only without a completed count; ERR-02
  confirmed bytes; THREAD-01/W-01 carried; concurrency disclosure). Today the
  only legal composition is k × `read_at` (k admissions, k syscalls); the
  `preadv` form is **not expressible** through the current single-span public
  API → new-capability design candidate only; single-call `read_at`
  semantics are untouchable.
- **Case B — file ranges scattered.** No regular-file syscall reads multiple
  disjoint offsets in one call (`man 2 readv`, Linux man-pages 6.13: one
  contiguous block per call — session-verified copy). A covering `preadv`
  over the gaps is forbidden as SNE-1 (three independent divergent
  observables: unrequested bytes transferred; per-request EOF observation
  collapsed into one short total; gap-region errors attributed to clean
  requests). Legal forms: explicit caller composition, M2-style explicit
  amplification contract, or a new batched positioned-read capability
  (candidate AC-2).
- **Case C — independent operations.** Vectored syscalls return one count and
  one errno per call and `RWF_*` flags are per call, so a single syscall
  cannot express per-op admission/results/error attribution. Undeclared write
  reordering and error-responsibility fusion are SNE-2; hidden
  `lseek`+`readv` lowering violates SEM-04 (SNE-3); `O_DIRECT`/`RWF_HIPRI`
  shapes are SNE-4 (plan §5.1 prohibition).

### 1.2 Probe design (plan §2.3–§2.4)

Arms `av0` (k separate `::pread`, scattered destination buffers, contiguous
extent), `av1` (one `::preadv`, k iov entries), `avs` (k separate Sluice
`read_at`, today's only legal Sluice composition, linked frozen B1 archive);
write twins `wv0`/`wv1`/`wvs`. Trailing kit token `iovcnt=<k>`. Cells: A/A
new-track floor; k ∈ {2,4,8} at 4 KiB; 64 KiB k=4; write 4 KiB k=4; two
Sluice-gap cells; EOF-boundary oracle cell (tail-straddling extents;
per-buffer prefix-length equality + effective-prefix checksum is the M1-Q3
oracle). Diagnostics per cell: `strace -c` window-isolated integral counts
(≥3 exact repeats), `perf stat instructions:u,cycles:u`, rusage. Cross-arm
equality group {av0, av1, avs} on destination-content checksum; write group
additionally `content_ok=1`.

### 1.3 Results

**Case-A read, 4 KiB / k=4, warm tmpfs** `[RUN]`
(`p3-raw/m1/p3-m1-vectored-4k-k4.txt` + `-syscalls.txt` + `-pmu.txt`):

| Side | Syscalls/op-group | Bytes req/xfer/extra | CPU (rusage u+s, ns) | Wall median (ns) | PMU instr/group |
|---|---|---|---|---|---|
| Baseline av0 (k×pread64) | 4.000 | 4096/4096/0 | 4097 | 2451.6 | 147 077 |
| Candidate av1 (1×preadv) | 1.000 | 4096/4096/0 | 2510 | 876.7 | 146 732 |

Δwall (av0−av1) = **+1576.0 ± 21.7 ns** per op-group, CI excluding 0, 16/16
same sign, ≥ MDE′ 32.2 ns. Zero intermediate copies certified structurally
(caller buffers are the iov entries; no staging array exists in the call
shape) with PMU non-contradiction (instr delta −345/group).
PRED-M1-1/2/4 confirmed.

**k-axis and size-axis** `[RUN]` (`p3-raw/m1/p3-m1-vectored-4k-k2.txt`,
`-k8.txt`, `p3-m1-vectored-64k-k4.txt`): syscall count k.000 → 1.000 in every
cell; Δwall +511.3 ± 12.4 (k=2; 1312.7 → 806.0), +1576.0 (k=4), +3613.6 ±
38.3 (k=8; 4522.8 → 912.7) at fixed 4 KiB — **monotone in k, PRED-M1-3
confirmed**; 64 KiB/k=4 Δwall +1389.2 ± 76.9 (9390.5 → 7984.4), bytes
65536/65536/0, instr 2 300 807 → 2 300 461.

**Case-A write twin, 4 KiB / k=4** `[RUN]`
(`p3-raw/m1/p3-m1-wvectored-4k-k4.txt`, Amendment-1 per-cell scratch):
4.000 `pwrite64` → 1.000 `pwritev`; CPU 2673 → 952 ns; wall 2578.2 → 877.7;
Δwall **+1699.3 ± 9.3 ns**, 16/16; `content_ok=1`; instr 1 052 442 →
1 052 235. PRED-M1-6 confirmed.

**EOF/short-count oracle** `[RUN]`
(`p3-raw/m1/p3-m1-eof-boundary-4k.txt`): content_hash, fill_len_hash and
effective-prefix checksum identical across arms (per-buffer prefix fills
exactly equal); syscalls 4.000 → 1.000; wall 2117.1 → 653.8 (Δ+1469.7 ±
18.7, descriptive oracle cell, no claim carried). The M1-Q3 equivalence
obligations hold at probe level under the quiescent fixture (the
concurrent-reader atomicity superset is OQ-M1-1, an Owner disclosure
decision, not measurable here).

**Sluice composition gap (today's only legal form)** `[RUN]`
(`p3-raw/m1/p3-m1-sluice-gap-4k-k4.txt`, `p3-m1-wsluice-gap-4k-k4.txt`):
read k×`read_at` vs bare k×`pread` Δwall +5.7 ns — sub-MDE `NO_CLAIM`; write
Δwall −15.4 ns — sub-MDE `NO_CLAIM`. Instruction caliber: read +263
instr/op-group, write +154 instr/op-group. Read exceeds the PRED-M1-5 band
[+120, +200] by 31.5%; the auxiliary attribution diagnostic
(`p3-m1-sluice-gap-4k-k4-pmu.txt`: same-shape a0/a1/a2 arms) measured a1−a0 =
+52.0 and a2−a1 = +44.0 instr/op — the band assumed an N1-shaped baseline
while av0 is the bare N0 arm (4×44 = 176 in-band; 4×96 = 384 over-predicts;
263 sits between). The instruction-caliber wrapper-constant magnitude is
confirmed; the band miss is a caliber artifact, disclosed, not absorbed. The
write-side +154 also sits above its own PRED-M1-5w band [+55, +100] (it
falls inside the read-side band); the same N0-vs-N1 caliber caveat applies
and no write-side auxiliary attribution was run — recorded as an open
discrepancy, not a claim.

### 1.4 Verdict and benefit boundary

- **Case A (read and write): `MECHANISM_WIN_EARNED` at probe level.** §5.2
  table complete on both sides; physical syscalls 4.000 → 1.000 with zero
  extra bytes and zero user-space copies (count caliber); wall improvement ≥
  MDE′ with CI excluding 0 on warm cells; checksum/EOF oracle green; no
  semantic-equivalence violation at probe level (plan §8 criterion set).
- **`REQUIRES_API_DECISION`.** The form cannot be expressed by the one-span
  public API; design candidate AC-1 filed (`API_CAPABILITY_CANDIDATES.md`
  §1). No SDK extension follows from the microbenchmark speed alone.
- **Case B/C illegal forms: `SEMANTICALLY_NOT_EQUIVALENT`** (SNE-1…4
  register, §1.1).
- **Overall family record capped at `CONDITIONAL_WORKLOAD_WIN`** (plan §9;
  #578 R0 incomplete).

Benefit boundary: the win is (k−1) saved syscall entries minus iov-iteration
cost, measured 511 ns (k=2) to 3614 ns (k=8) per op-group at 4 KiB on warm
tmpfs, for **contiguous file extents split across caller buffers only**. It
does not transfer to scattered offsets (Case B), to independent operations
(Case C), to any admitted application workload (§9 cap), or off this host
(E5-only, plan §13.7). Tail latency: `NOT_SUPPORTABLE_WITH_REASON` (ns-scale
effects; kit arms record best-of-reps `ns_per_op` only, no per-op samples —
P0 §4.3 precedent, plan §6 row).

## 2. Family M2 — Request range planning / coalescing

### 2.1 Legal framework (preregistered, `p3-families/M2_ANALYSIS.md` §2)

The independent Direct primitive must not implicitly cache, merge or reorder
calls: `read_at` is one span per call with per-call admission
(`src/blocking_file.cpp:30-37`, `:86-105`), so library-internal planning
would invent hidden cross-call authority and make per-call admission/EOF/
error attribution history-dependent. Legal forms, exhaustively: explicit
caller composition; independent host-side policy (e.g. a dududu-side
planner); a normatively designed new capability (Owner decision). M2 sits in
Case B of the M1 discipline and is bound by Case C: per-request results,
per-request EOF attribution and per-request error identity must survive any
merge; read-set-internal reordering is legal (SEM-04 promises no cross-call
order) but changes the concurrent-writer observation-point granularity and
must be disclosed. Seven illegal forms are preregistered as
`SEMANTICALLY_NOT_EQUIVALENT` candidates (§2.5 a–g of the family document):
library-internal merge/memoization (intra-set dedup legal, cross-call
memoization illegal), delivered over-read or slice misattribution, error
fusion, EOF mis-attribution, request-set mutation, write-side merging across
sync boundaries or cursor movement (out of scope, D-P3-04), and
loop-to-full executors that hide EOF. Five proof obligations each carry a
probe-level oracle (request-set digest; per-request checksum vs an
independent serialized reference replay; per-request EOF prefix equality;
read-only boundary certification; descriptive divergence record).

### 2.2 Probe design (plan §3.3–§3.4)

Arms `rpl0` (unmerged replay, k separate `::pread` per request),
`rplT` (sort+sweep merge of extents with gap ≤ T into one `::pread` per
merged extent + in-memory slice dispatch), `rplD` (union dedup, T=0). Traces:
`g4k` (8 requests of 4 KiB, seeded-LCG gaps), `ov30` (30% duplicates),
`eofmix` (EOF-straddling extents). Cells: warm A/A; warm T-sweep
T ∈ {512, 1K, 2K, 4K, 8K, 16K}; dedup; EOF attribution (T=4K); cold btrfs
T ∈ {0, 256, 512, 1K, 4K} + cold A/A; planner-overhead (T=0, equal syscall
counts); concurrent-writer divergence (descriptive, no CI claim).

### 2.3 Results

**Warm T-sweep, g4k, tmpfs** `[RUN]` (`p3-raw/m2/p3-m2-merge-sweep-g4k-*.txt`,
one wall + syscalls + PMU triple per T; baseline side per cell: 8
requests/op-group, req_digest `0x721f35c2d480f218` pinned in every header,
8.000 `pread64`/op-group, 32 768 B requested = issued, extra 0. Extra-bytes
below are the timed cells' own `issued_per_group` counters; the kit smoke
gate ran a different, ops=64 request set (digest `0x4a6717aa…`) whose
issued deltas must not be quoted for these cells — an earlier draft of this
table did exactly that and has been corrected):

| T | Syscalls/op-group | Extra bytes (amplification) | Δwall rpl0−rplt (ns) | Outcome |
|---|---|---|---|---|
| 512 | 8.000 → 7.601 | +103 | −329.9 ± 36.4 | regression, CI ≠ 0 |
| 1 K | 8.000 → 7.156 | +444 | −156.7 ± 37.9 | regression, CI ≠ 0 |
| 2 K | 8.000 → 6.257 | +1821 | +151.6 ± 182.8 | CI includes 0 → NO_CLAIM |
| 4 K | 8.000 → 4.548 | +7061 (1.215×) | **+327.2 ± 35.2** | win; smallest CI-excluding T ⇒ **T\* = 4 KiB** |
| 8 K | 8.000 → 1.000 | +28 828 (1.880×) | −722.5 ± 49.6 | regression, CI ≠ 0 |
| 16 K | 8.000 → 1.000 | +28 828 | −752.0 ± 42.5 | regression |

At T=4 KiB: CPU 22 818 → 22 488 ns/op-group, instr 1 154 631 → 1 162 112,
wall 8599.9 → 8281.8. **PRED-M2-2 confirmed** (T\* = 4 KiB ∈ [1 KiB,
16 KiB]; and per PRED-M2-F the measured T\* sits below the two-term model's
≈5.4 KiB point estimate, as the omitted dispatch/staging terms only
subtract). **PRED-M2-1's wall clause is falsified**: at T=8 KiB the two
count clauses hold (8.00 → 1.00 syscalls; amplification 1.880× ≤ 2.0×) but
wall **regresses** by 722.5 ns — the 1.88× byte amplification's transfer
cost overtakes the 7 saved syscall entries. The merge mechanism's warm wall
win exists only inside a narrow threshold window around 4 KiB on this trace
and turns into a CI-excluding regression at both T ≤ 1 KiB (merge overhead
unpaid) and T ≥ 8 KiB (amplification-dominated). Issued-bytes monotone
non-decreasing and merged-group count monotone non-increasing in T held
exactly across diagnostic repeats (PRED-M2-B).

**Dedup, ov30** `[RUN]` (`p3-raw/m2/p3-m2-dedup-ov30.txt` + diagnostics):
syscalls 8.000 → 3.999/op-group (kit disjoint-component counter: 3);
requested 32 768 B, issued 21 507 B, extra **−11 261 B (−34.4%, union never
over-reads — PRED-M2-C confirmed)**; CPU 21 509 → 19 584 ns; wall 7078.4 →
4925.3; Δwall **+2146.6 ± 53.0 ns**, 16/16; instr 1 154 635 → 1 162 138;
every per-request digest gate green (req/ref/outcome digests equal).
**PRED-M2-4 confirmed on all three legs** (bytes −34.4% ∈ 30 ± 5 pp;
syscalls −50.0% ≥ 25%; wall +30.3% ≥ 15%). This is a genuine physical-work
reduction — duplicate bytes are not transferred at all — not a cost
migration.

**Planner own cost, warm T=0 (equal syscall counts)** `[RUN]`
(`p3-raw/m2/p3-m2-planner-overhead-g4k.txt` + `-pmu.txt`): rpl0 wall
8556.2 / CPU 22 761 / instr 1 154 631 vs rplT(T=0) wall 9059.9 / CPU 23 179
/ instr 1 162 684 → Δwall **−495.2 ± 41.7 ns** (CI ≠ 0, 0/16) and Δinstr
+8053/group (the planner cell's own `-pmu.txt` archived only the rplt arm,
three identical runs at 1 190 588 790/843/838 instr whole-process ÷ 1024
ops; the rpl0 figure is the T=4K cell's rplt-side-baseline PMU arm — same
arm, trace and digest `0x721f35c2d480f218` at ops=1024 — cross-cell reuse
disclosed). **PRED-M2-5's ≤ 100 ns bound is falsified (≈5×)** — consistent
with PRED-M2-E's structural argument that the pinned arm definition
(staging + slice dispatch of all requested bytes) cannot fit 100 ns at
32 KiB/op-group on this host.

**The `COST_MIGRATION_ONLY` trigger clause of PRED-M2-5 fires.** The
preregistered condition — "dispatch overhead ≥ 50% of the syscall saving at
the winning T ⇒ the mechanism verdict degrades to `COST_MIGRATION_ONLY`" —
is evaluated from the archived evidence at T\* = 4 KiB (8.000 → 4.548
syscalls/op-group, 3.452 saved entries; net wall saving +327.2 ns, CI ≠ 0;
dispatch overhead −495.2 ns at equal counts, CI ≠ 0):

- against the **net winning-T wall saving** (+327.2 ns): 495.2/327.2 =
  **151%** ≥ 50%;
- against the **additively decomposed gross syscall saving** (net +
  dispatch overhead = 327.2 + 495.2 = 822.4 ns, linear-additivity
  assumption disclosed): 495.2/822.4 = **60%** ≥ 50%.

The only reading that stays below the threshold prices the 3.452 saved
entries at the preregistration grounding constant (≈550–700 ns/entry ⇒
1.9–2.4 µs ⇒ 20–26%), but that constant is contradicted by this campaign's
own evidence: the measured gross saving per saved entry at T\* is
822.4/3.452 ≈ 238 ns, because a merged 4.548-syscall group transfers
39 829 B (≈8.8 KiB per issued call) at a higher per-call cost than the
4 KiB calls the grounding constant assumed. Exempting the mechanism from
the mandatory downgrade on the strength of a model constant the cells
refuted would be a post-hoc reinterpretation (plan §8). Accordingly the
**threshold-merge mechanism verdict is `COST_MIGRATION_ONLY`** (plan §8:
"recorded even when a wall number improved") — the T\* = 4 KiB wall win
(+327.2 ns, CI ≠ 0) stands as recorded evidence, but most of the gross
syscall saving is consumed by planner/dispatch work. The M2-OQ1 amendment
path — a direct-fill or plan-only arm separating bookkeeping from data
movement — was not exercised; the arm definition stayed as preregistered.

**Cold btrfs** `[RUN]` (`p3-raw/m2/p3-m2-merge-cold-g4k-{0,256,512,1024,4096}.txt`,
`p3-m2-aa-planner-cold.txt`): cold A/A sd 31 053 ns → cold MDE′ = 16 544 ns;
all five cold T deltas (T=0: +1072 ± 10 707; 256: −329 ± 15 015; 512: +330 ±
9227; 1K: −561 ± 10 102; 4K: +762 ± 6711) are sub-MDE → `NO_CLAIM` at every
T. Arm medians ≈137–140 µs/op-group (genuinely cold, verified by direct
measurement).
**PRED-M2-3 (T\*_cold ≤ 512 B) is `NOT_RESOLVED`** under the pinned caliber —
not falsified: the predicted cold break-even shift is smaller than the cold
track can resolve. Count caliber is temperature-invariant (8.000 → 4.548 at
T=4K, extra 1.215×).

**EOF attribution, eofmix @ T=4K** `[RUN]`
(`p3-raw/m2/p3-m2-eof-attribution.txt`): per-request outcome/ref/req digest
triple-replay all equal (wholly-past-EOF requests count 0 as EOF success;
straddling requests get exactly their prefix; no request inherits the merged
short count); syscalls 8.000 → 1.000; wall 4311.1 → 1208.5 (Δ+3099.4 ± 49.7,
descriptive oracle cell). PRED-M2-6/D confirmed; all per-request gates green
in every cell of the family.

**Concurrent-writer divergence (descriptive, no CI claim)** `[RUN]`
(`p3-raw/m2/p3-m2-concurrent-divergence.txt`; first construction mutated an
offset outside the ops=256 touched span — digest unchanged — and was redone
at offset 1 048 576 inside the span; both constructions archived with a REDO
note): a 4096 B XOR-0xFF mutation of `[1048576, 1052672)` placed between the
unmerged and merged replays changes both forms' outcomes identically
(pre-mutation rpl0 outcome `0x2fb7a431…`; post-mutation rpl0 and rplT(T=4K)
both `0x5b6e899a…`, each matching its own post-mutation reference, exit 0) —
under the same file state the two forms are byte-identical. The merged arm
additionally read 7503 B/op-group of unrequested gap bytes (sampled at
merge-read time, unobservable by the caller). The consistency-window
contract question is AC-2's to close.

### 2.4 Verdict and benefit boundary

- **Dedup (overlap elimination): `MECHANISM_WIN_EARNED` at probe level**
  — real byte/syscall/wall reduction, oracle green — **+
  `REQUIRES_API_DECISION`** (in-SDK capture needs the gathered-read
  capability, candidate AC-2). Capped `CONDITIONAL_WORKLOAD_WIN` (§9).
- **Threshold merge: `COST_MIGRATION_ONLY`.** A CI-excluding wall win exists
  only at T\* = 4 KiB on this trace (+327.2 ns); PRED-M2-1's wall clause is
  falsified at T=8 KiB; sub-T\* thresholds and T ≥ 8 KiB regress
  CI-excluding; and the preregistered PRED-M2-5 trigger fires (dispatch
  overhead 495.2 ns = 151% of the net / 60% of the additive-gross winning-T
  syscall saving, both ≥ the 50% threshold — calculation in §2.3), so the
  mechanism verdict is degraded per plan §8, recorded despite the wall win.
  The family carries no unconditional merge win; any product form must bind
  T to the measured window, re-derive per filesystem/cache state, and treat
  the planner's own cost as a first-class charge against the claimed saving.
- **Planner overhead: PRED-M2-5 falsified** (+495.2 ns ≫ 100 ns bound); the
  same measurement triggers the `COST_MIGRATION_ONLY` degradation of the
  merge mechanism above (both evidence-internal readings of the trigger
  ratio exceed 50%).
- **Cold shift: `NOT_RESOLVED`** (sub-MDE at every T; not a negative
  result).
- **Illegal forms: `SEMANTICALLY_NOT_EQUIVALENT`** register stands as
  preregistered.
- **Overall family record capped at `CONDITIONAL_WORKLOAD_WIN`** (§9).

Benefit boundary: warm-tmpfs, L2/L3-resident working sets, 8 × 4 KiB
request-groups with seeded-LCG gaps; dedup requires genuine
duplicate/overlap structure (ov30-class traces); merge requires a threshold
inside [2 KiB, 8 KiB) on warm tmpfs — outside it the mechanism is neutral or
a regression; cold-btrfs threshold structure unresolved (MDE′-limited);
concurrent-writer observation granularity is a disclosed hazard, not a
measured claim; write-side planning entirely out of scope (D-P3-04).

## 3. Family M3 — File-to-file kernel transfer

### 3.1 Legal framework (preregistered, `p3-families/M3_ANALYSIS.md` §2)

`copy_file_range` and `sendfile` move bytes fd→fd inside the kernel; their
ABI has no user-buffer parameter. Form adjudication: F1/F2 (either syscall
as an internal implementation of `read_at`/`write_at`)
`SEMANTICALLY_NOT_EQUIVALENT` — impossible at ABI level; F3 (hidden
`lseek`+`sendfile` emulating an explicit out-offset) non-conforming lowering
— sendfile always advances the out_fd shared cursor when seekable
(session-measured: out 1024 → 5120), violating SEM-04; F4 (NULL-offset forms
presented as positioned copy) same class — both cursors consumed and
advanced (session-measured, both directions 1024 → 5120); F7 (today's legal
composition, `read_at`+`write_at` per chunk = arm cp1s) is the §5.2
Baseline; F8 raw `pread`+`pwrite` loop (cp1) the N1-class comparator; F5
(explicit-offset cfr driving a **new explicit copy capability**) legal but
absent from v2-r1 → `REQUIRES_API_DECISION`; F6 sendfile can only be a
conditional engine/comparator under a cursor-untouched contract.

Functional support matrix `[RUN]`, re-run at cell start as the gate
(`p3-raw/m3/support-matrix.txt`, `support-matrix-smoke.txt`,
SUPPORT_GATE_PASS; tool sha256 `0f8a97f6…30501`, `p3-raw/env-pin.txt:12`):
same-fs explicit-offset 4 KiB copy ret=4096 content MATCH on tmpfs and
btrfs; NULL-offset variants advance both positions and return 0 exactly at
EOF; explicit-offset past-EOF ret=0 with `off` arguments unadvanced and
positions untouched; cross-type-fs cfr = EXDEV (errno 18) both directions;
sendfile works on all four fs pairs but always advances out_fd's position;
same-file truly-overlapping ranges EINVAL, adjacent legal; `O_APPEND`
destination cfr=EBADF / sendfile=EINVAL. EFBIG/ENOSPC/EOPNOTSUPP remain
session-unverified (see NOT_RUN register).

### 3.2 Probe design (plan §4.3–§4.4)

Arms cp1 (native `pread`+`pwrite` 64 KiB loop), cp1s (Sluice
`read_at`+`write_at` loop — today's legal form), cp2 (`copy_file_range`
loop, explicit offsets), cp3 (`sendfile` loop, explicit in-offset).
Parameter shape 64 KiB × 1024 chunks vs a 64 MiB source; per-cell Amendment-1
scratch destination; content gate = full-file checksum equality across
{cp1, cp1s, cp2, cp3} + source (`content_ok=1`); diagnostics strace/PMU/
rusage per cell; cross-fs sendfile both directions; short/error descriptive
cells.

### 3.3 Results

**cfr vs user loop, tmpfs** `[RUN]` (`p3-raw/m3/p3-m3-copy-tmpfs-cfr.txt` +
`-syscalls.txt` + `-pmu.txt`): baseline cp1 syscalls 2.000/chunk by kit
counter (`pread64`+`pwrite64`); the plan-§6 strace window caliber reads
2.125/chunk (pwrite64 1024−512 = 1.000/chunk exact; pread64 1154−578 =
1.125/chunk — a verify-read residual that scales with the window; the three
diagnostic repeats are bit-identical at 1154/578, so the count caliber is
stable and the 2→1 class claim is unaffected). Bytes req 65 536 / xfer
131 072 (read+write) / extra 0, CPU 21 579 ns/chunk, wall 19 676.0, instr
994 899; candidate cp2 1.000 cfr/chunk exact in both calibers,
65 536/65 536/0, CPU 20 092, wall 17 626.1, instr 994 833. Δwall
**+2054.2 ± 66.4 ns/chunk (+10.4%)** ≥ MDE′ 122.9; CPU **−6.9%** — short of
P-M3-1's −30% (prediction falsified, recorded); PMU whole-process instr
delta only −66/chunk, so the ≥2× instr/byte target is unresolvable at
whole-process caliber (caliber limitation disclosed; structural
certification carries the copy-elimination row: 2 user-space
traversals/byte → 0). P-M3-2's wall ≥ 10% holds. Verdict: **probe-level
`MECHANISM_WIN_EARNED`** (§5.2 table complete, syscalls 2.000 → 1.000 count
caliber, user-space copies 2 → 0/byte structural, wall ≥ MDE′ with CI ≠ 0,
oracle green; the falsified CPU prediction is a prediction outcome, not a
verdict criterion) + `REQUIRES_API_DECISION`.

**cfr vs user loop, btrfs (same fs)** `[RUN]`
(`p3-raw/m3/p3-m3-copy-btrfs-cfr.txt` + diagnostics): baseline 2.000
syscalls/chunk (kit counter; strace window 2.125 as above, identical
1154/578 repeats), 65 536/131 072/0, CPU 16 815, wall 15 343.2; candidate
1.000 cfr/chunk, 65 536/65 536/0, CPU 7581, wall 5773.4. Δwall **+9508.3 ±
114.5 ns/chunk (+62.0%)**; CPU **−54.9%** (≥ −20% holds). The wall
improvement exceeds P-M3-4's preregistered band upper [+−5%, +30%] —
recorded as-is per the plan's recording rule (improvement beyond band, not a
violation to hide). OQ-1 caveat: btrfs same-fs cfr may engage reflink-class
acceleration (man NOTES); the CPU magnitude must not be extrapolated to
non-accelerating filesystems — attribution caveat stands (no FIEMAP-class
check in P3 scope). Verdict: **probe-level `MECHANISM_WIN_EARNED`** on the
strongest cell of the family; overall capped `CONDITIONAL_WORKLOAD_WIN`.

**sendfile (comparator arm)** `[RUN]` (`p3-raw/m3/p3-m3-copy-tmpfs-sendfile.txt`,
`p3-m3-copy-crossfs-sendfile-b2t.txt`, `-t2b.txt`,
`p3-m3-copy-btrfs-sendfile.txt`): tmpfs same-fs cp3 = 1.000 sendfile/chunk
exact, Δwall vs cp1 +2123.1 ± 102.0 (+10.7%); |cp3 − cp2| = 7.9 ns
(medians 17 634.0 vs 17 626.1) < 2×MDE′ (P-M3-5 same-splice-class
confirmed). Cross-fs b2t: +2202.6 ± 51.7 (CI ≠ 0); t2b: −163.2 ± 208.6 CI
includes 0 → NO_CLAIM. btrfs same-fs: Δ median +765.7, mean +493.9 with
mean CI [75, 913] marginal; paired-delta sd 786 ns (arm sds 1099.9 / 932.0;
cfr cell 215) — high variance; |cp3 − cp2| btrfs = 9199 ns (medians
14 972.5 vs 5773.4) ≫ 2×MDE′ → **attributed** (sendfile's out-cursor
mechanism + variance), no claim. sendfile stays a comparator: its
unconditional out_fd cursor advance disqualifies it as an engine for a
cursor-untouched contract (F6/OQ-2, Owner decision in AC-3).

**Sluice composition gap** `[RUN]` (`p3-raw/m3/p3-m3-copy-tmpfs-sluicegap.txt`,
`-btrfs-`): cp1s vs cp1 tmpfs Δwall −36.6 ns sub-MDE → NO_CLAIM; Δinstr
+85/chunk (cp1s 994 984 from its own `-pmu.txt`, three runs 994 983/984/985;
cp1 994 899 from the cfr cell's PMU arm — cross-cell reuse disclosed) —
inside P-M3-6's [−50%, +150%] band around the ≈+57 instr/chunk
wrapper-constant class (2× read/write constants
`DIRECT_COST_FLOOR.md:220-228,254-260`); btrfs Δwall −103.1 sub-MDE →
NO_CLAIM. Today's legal composition pays no resolvable wall premium over the
raw loop; the kernel-transfer win is therefore not reachable by optimizing
the wrapper — only by the new capability.

**Short-at-EOF descriptive cell** `[RUN]`
(`p3-raw/m3/p3-m3-short-error-cells.txt`): all four arms publish exactly
2 097 152 confirmed bytes = source length; loop-and-advance discipline holds
(offsets strictly monotonic; no replay of confirmed ranges — P-M3-8).
**Executed-shape deviation from the preregistered evidence shape,
disclosed:** plan §4.2 disclosure 2 pinned "chunk sizes 4 KiB/64 KiB against
a 64 MiB fixture"; the archived cell is a single 64 KiB-chunk arm set
(ops=40) against a 2 MiB source constructing source-EOF shortness (header
note in the raw file) — no 4 KiB chunk arm, no 64 MiB fixture. The
size-capped-destination error-injection sub-cells were additionally
`NOT_RUN_WITH_REASON` (sudo scope, below). This deviation was applied
without the numbered plan amendment the preregistration discipline requires
(plan §Amendment log still reads "None") — recorded here as a discipline
gap; the plan amendment must be appended by the Owner for the record to be
preregistration-conformant.

**NOT_RUN_WITH_REASON register** (no substitution, plan §4.1):
M3 concurrent-modification divergence cell (plan §4.2 disclosure 4) — no
archived M3 cell exists (unlike M2's
`p3-raw/m2/p3-m2-concurrent-divergence.txt`); the disclosure's descriptive
evidence was never recorded, and no reason was registered at execution time;
cfr cross-fs cells — EXDEV per the support matrix (ret=−1 errno 18, both
directions); ENOSPC/EFBIG injection sub-cells — the session's sudo scope
(perf/PMU/strace + the planned drop_caches) cannot construct a
size-constrained destination filesystem (loop-mount/quota need mount
privileges); EXDEV/EINVAL/EBADF classes are pinned by the support matrix;
NUMA — single-node host; `O_DIRECT` interplay — excluded by plan §5.1 as a
non-equivalent access mode (D-P3-02).

### 3.4 Verdict and benefit boundary

- **btrfs cfr: `MECHANISM_WIN_EARNED` at probe level** (wall +62.0% and CPU
  −54.9%, both CI ≠ 0, oracle green, support gate green), with the
  reflink-acceleration attribution caveat (OQ-1) and the out-of-band-upper
  wall recorded as-is. **tmpfs cfr: `MECHANISM_WIN_EARNED` at probe level**
  (wall +10.4% ≥ MDE′ with CI ≠ 0; syscalls 2.000 → 1.000 count caliber;
  user-space copies 2 → 0/byte structural; P-M3-3's CPU −30% leg is
  falsified at −6.9% and the instr target is unresolvable at whole-process
  PMU caliber — both recorded as falsified/unresolved predictions, which the
  §8 verdict criteria do not condition on).
- **`REQUIRES_API_DECISION`**: the opportunity is reachable only through a
  new explicit file-copy capability (candidate AC-3); F1–F4 stay
  `SEMANTICALLY_NOT_EQUIVALENT`.
- **Overall family record capped at `CONDITIONAL_WORKLOAD_WIN`** (§9).

Benefit boundary: regular files only; same-filesystem explicit-offset
transfers; 64 KiB chunks on a 64 MiB warm source; cross-fs cfr impossible
(EXDEV — the fallback boundary any product form must surface explicitly);
EFBIG/ENOSPC/EOPNOTSUPP behavior unpinned on this host (error table of AC-3
not final until injected); concurrent-modification consistency behavior
unpinned (disclosure-4 cell never run — NOT_RUN_WITH_REASON above); btrfs
magnitude not portable to non-accelerating filesystems; tail latency
`NOT_SUPPORTABLE_WITH_REASON` (kit caliber).

## 4. Family M4 — Filesystem / hardware awareness

### 4.1 Legal framework (preregistered, `p3-families/M4_ANALYSIS.md` §1)

M4 proposes no mechanism of its own. Its products are environmental
constants — per-state marginal ns/B, fixed per-op costs, and the
syscall-vs-byte exchange rate R — consumed by M1–M3 trade-off models; any
read-shape conclusion is filed under M2's evidence. Perception axes are
fixed (cache hot/cold via P1 §7 drop discipline only; tmpfs vs btrfs with
mount-option disclosure; size sweep; working set 8 MiB < L3 25 MiB < 64 MiB
< 256 MiB with no THP/madvise/mlock/prefetch tuning; PMU instructions/cycles
cross-arm deltas as diagnostic; NUMA → `NOT_RUN_WITH_REASON`, 1 node).
Prohibited forms preregistered as `SEMANTICALLY_NOT_EQUIVALENT`-class or
forbidden: `O_DIRECT` as an equivalent replacement anywhere (alignment/
misalignment behavior makes the SEM-03/04 arbitrary-span domain
inexpressible — `man 2 open`, session-verified text); fadvise/madvise/THP
policy tuning (D-P3-03); over-read inside the primitive (belongs to M2/AC-2);
tmpfs-warm reported as device-cold; THP-shaped destination buffers.

### 4.2 Probe design (plan §5.2)

Existing a1/a2 read arms (single span, single syscall, contract unchanged)
on: size sweep {512 B, 4 KiB, 64 KiB, 1 MiB} × {tmpfs, btrfs} warm; working
set {8 m, 64 m, 256 m} at 4 KiB (ops scaled per OQ-1 so the touched region
equals the fixture); cold {4 KiB, 64 KiB} btrfs per P1 §7; R derived, not a
cell. Wall A/A warm and cold; per-cell PMU; syscall-count corroboration.

### 4.3 Results (constants, no mechanism verdict)

`[RUN]` (`p3-raw/m4/` — `p3-m4-aa-read-4k.txt`, `p3-m4-aa-cold-read-4k.txt`,
`p3-m4-size-sweep-*`, `p3-m4-ws-*`, `p3-m4-cold-*`; analysis blocks
recomputed and re-attached during the session, cold A/A arm pair corrected
to A1/A1 per the in-file note):

- Marginal byte cost, warm tmpfs (a1-arm wall medians, size-axis):
  4k→64k = (7986.0 − 797.9)/61 440 = **0.117 ns/B**, 64k→1m =
  (159 390.5 − 7986.0)/983 040 = **0.154 ns/B** — both inside PRED-M4-1's
  [0.08, 0.25] → confirmed. Warm btrfs: (5717.3 − 762.5)/61 440 = **0.081**
  and (126 366.2 − 5717.3)/983 040 = **0.123 ns/B**.
- Cold/warm ns/B ratio (btrfs denominator, 4 KiB, a1 wall medians):
  10 374.4 / 762.5 = **13.6×** ≥ 8× (64 KiB: 159 708.2 / 5717.3 = **27.9×**)
  → PRED-M4-2 confirmed. (An earlier draft published 23.4×/29.3× built from
  single-sample values of mismatched dimensions — a wall `ns_per_op` sample
  divided by a rusage `sys_ns_per_op` sample; both ratios are corrected to
  the median-over-median caliber. The ≥8× verdict holds under either
  reading; the 23.4×/29.3× constants are not archived anywhere and are
  retracted.)
- Working-set axis (4 KiB seq, warm tmpfs, a1 wall medians): 8 m = 788.2,
  64 m = 965.1, 256 m = 929.0 ns/op. 256 m vs 64 m = **−3.7% ≤ 15%** →
  PRED-M4-3(a) confirmed; the L3-crossing effect (64 m vs 8 m) is **+22.4%**,
  inside the preregistered [+3%, +40%] point band (central ≈ +22%) →
  PRED-M4-3(b) confirmed; 64 m ≈ 256 m within ±10% holds (−3.7%) →
  PRED-M4-3(c) confirmed. (An earlier draft published 420.1/677.6/635.8
  ns/op — single-sample `sys_ns_per_op` field values mis-parsed as wall
  medians — and on those wrong constants reported a +61% "exceeds the band"
  disclosure; the mis-parse is corrected and the out-of-band disclosure is
  withdrawn: the archived medians put the crossing effect in-band.)
- Exchange rate R: **R_warm ≈ 4.3–4.9 KiB per saved syscall entry vs
  R_cold ≈ 0.46 KiB → ≥ 8× main clause of PRED-M4-4 confirmed** (and
  R_warm lands inside M2's T\* window, cross-checking PRED-M2-2). The strict
  warm tmpfs > warm btrfs sub-ordering is **not resolved** (slope-window
  sensitive) — disclosed.
- Packaging parity invariance: **no a1−a2 wall delta on any M4 axis carries
  a claim** — none is simultaneously CI-excluding-0 and ≥ the session MDE′
  9.51 ns (dual-reported with the inherited pinned 5.535 ns). Five cells'
  point estimates exceed 9.51 ns in magnitude (cold-64k +4851.0, 1m-tmpfs
  −206.4, cold-4k +126.9, 1m-btrfs −34.6, 64k-tmpfs −22.1 ns) but every one
  of their CIs includes 0; the 4 KiB tmpfs cell's mean CI (−7.3,
  [−13.7, −0.8]) excludes 0 but its median −7.9 ns is sub-MDE′ → `NO_CLAIM`
  under the preregistered rule. (An earlier draft stated "every a1−a2 wall
  delta … is sub the session MDE′ 9.51 ns", which is false as written — the
  five point estimates above exceed it; the parity conclusion survives in
  its correct, weaker form: no claimable delta exists, so the P1 wrapper
  constants remain environment-invariant at wall claim caliber —
  PRED-M4-10 direction; instruction-caliber deltas corroborated on the
  archived PMU cells, +44 instr/op at 512 B/4 KiB/64 KiB, +28 at 1 MiB).
- Syscall-count axis: certified on the archived strace cell
  (`p3-m4-size-sweep-4k-tmpfs-syscalls.txt`); the remaining axes share the
  same single-span arm shape (structurally one `pread64`/op) and are not
  separately archived.

PRED-M4-6/7/8/9/11 raw evidence is archived under `p3-raw/m4/` (PMU and
sweep files listed above) without a summarized adjudication in the execution
record; their falsifier consequences did not trigger (no cell was
invalidated, no R-model downgrade occurred).

### 4.4 Verdict and benefit boundary

**`NO_CHANGE_JUSTIFIED`** — as designed: M4 proposes no mechanism, and no
SDK change is justified by its evidence. Its constants parameterize the
M1 crossover model, the M2 threshold model (T\* ≈ R_warm confirmed
consistent) and the M3 CPU-side disclosures; R is an E5-session research
constant, not portable and not product state (plan §13.7; mechanism-
justification gate). NUMA axis `NOT_RUN_WITH_REASON` (1 node); write-side
environment axes are not M4 cells (OQ-9) — any write-side R needs an
amendment.

## 5. Cross-family conclusions

1. **Three physical-work-reduction levers are proven at probe level, and
   all three are capability-gated.** (a) Syscall-entry elimination by
   vectored positional I/O on contiguous extents (M1 Case A: 4.000 → 1.000
   syscalls, +1576 ns read / +1699 ns write per op-group at 4 KiB/k=4, zero
   extra bytes, zero intermediate copies); (b) duplicate-byte elimination by
   intra-set union dedup (M2: −34.4% bytes, −50% syscalls, +30.3% wall);
   (c) user-buffer traversal elimination by kernel file-to-file transfer
   (M3 btrfs: 2 → 1 syscalls/chunk, 2 → 0 user-space traversals/byte, CPU
   −54.9%, wall +62.0%). None is expressible by today's one-span public API
   inside the SDK: each carries `REQUIRES_API_DECISION` and a filed design
   candidate (AC-1/AC-2/AC-3). Nothing auto-adopts; no microbenchmark speed
   extends the SDK (MISSION §5.3). (The M2 threshold merge is deliberately
   absent from this list: its T\* = 4 KiB wall win is real, but the
   mechanism verdict is `COST_MIGRATION_ONLY` per the PRED-M2-5 trigger —
   §2.3/§2.4.)
2. **The wrapper is not the frontier.** The only-legal-composition gap rows
   (M1 avs−av0: +5.7 ns read / −15.4 ns write, both sub-MDE `NO_CLAIM`; M3
   cp1s−cp1: −36.6 / −103.1 ns sub-MDE `NO_CLAIM`; M4 a1−a2 with no
   claimable delta on any axis) confirm that today's Sluice compositions sit
   at wall parity
   with raw POSIX loops, with instruction-caliber wrapper constants of the
   P1-pinned class (read-side +263 vs 4×≈38-model, band miss attributed to
   the N0-vs-N1 caliber; write-side +154, recorded above its own band as
   an open discrepancy). Beyond-parity gains in this campaign therefore come
   only from new execution capabilities, not from further wrapper
   compression.
3. **Amplification economics bound every merge-style mechanism.** The
   exchange rate measured by M4 (R_warm ≈ 4.3–4.9 KiB/syscall, R_cold ≈
   0.46 KiB) correctly predicted the M2 threshold structure: T\* = 4 KiB on
   warm tmpfs, wins decaying to CI-excluding regressions at T ≥ 8 KiB where
   1.88× byte amplification overtakes 7 saved entries. PRED-M2-1's wall
   clause is falsified; the planner's own userspace cost (+495.2 ns/op-group
   at equal syscall counts) falsifies PRED-M2-5's 100 ns bound **and fires
   the preregistered `COST_MIGRATION_ONLY` trigger** (dispatch overhead ≥
   50% of the winning-T syscall saving under both evidence-internal
   readings — 151% of the net +327.2 ns saving, 60% of the additive-gross
   822.4 ns; calculation in §2.3), so the threshold-merge mechanism verdict
   is `COST_MIGRATION_ONLY`, recorded despite its T\* wall win (plan §8).
   Any product
   planner must treat dispatch/staging cost and threshold binding as
   first-class contract economics, and the cold regime stays unresolved
   (sub-MDE at every T — `NOT_RESOLVED`, not negative).
4. **Cost-migration discipline held.** No verdict rests on syscall counts
   alone: every win row shows attributed CPU and/or wall reduction on both
   sides of the §5.2 table (`IO_WORK_REDUCTION_LEDGER.md`); the places
   where work moved rather than shrank are recorded as such — the planner
   overhead at T=0 as a falsified bound, and the threshold-merge mechanism
   as `COST_MIGRATION_ONLY` via the mandatory trigger — and M3's kernel
   work stays attribution-only (no
   kernel-internal byte count is reported as measured — P-M3-9 discipline).
5. **Semantic negative space is now measured-backed.** The
   `SEMANTICALLY_NOT_EQUIVALENT` registers (M1 SNE-1…4, M2 §2.5 a–g, M3
   F1–F4, M4 §1.3 a–e) are not speculations: each divergent observable has
   either a session-measured ground (sendfile/cfr cursor advance 1024→5120;
   EXDEV cross-fs; EOF-collapse mechanics demonstrated in the M2 eofmix
   reconstruction) or a structural ABI ground (no user-buffer parameter; one
   count/errno per call). The concurrent-writer divergence record shows the
   two forms byte-identical under equal file state while the merged form
   samples unrequested bytes — the consistency-window contract is the
   candidates' obligation.
6. **Admission cap and boundaries.** #578 R0 is incomplete; every
   mechanism-level win is capped `CONDITIONAL_WORKLOAD_WIN` with scope,
   shape and boundary conditions stated (warm tmpfs/btrfs, synthetic traces,
   single host, ns-to-µs-scale effects, tail latency
   `NOT_SUPPORTABLE_WITH_REASON` throughout — kit arms record best-of-reps
   `ns_per_op` only, P0 §4.3 precedent). The cap may be revisited only by
   an amendment naming admitted workload evidence (plan §9.4). Write-side
   planning (D-P3-04), io_uring-style scattered-offset execution (D-P3-01),
   `O_DIRECT`/cross-fs beyond the support matrix (D-P3-02) and cache-policy
   tuning (D-P3-03) remain deferred with reasons; the P2-carried gap
   register is unchanged.
7. **Execution-defect record (all corrected, archived with REDO notes in
   the affected files).** M1 write-side first-pass diagnostics used a
   `2>&1 >>` redirection order that dropped strace/perf stderr from the
   archive — rerun with correct redirection; the M2 divergence cell's first
   mutation sat outside the touched span (digest unchanged) — redone at an
   in-span offset, both constructions archived; the M3 short/error cell was
   executed in a source-EOF shape (2 MiB source, 64 KiB × ops=40) that
   deviates from the preregistered "4 KiB/64 KiB chunks vs 64 MiB fixture"
   evidence shape, without the numbered plan amendment the preregistration
   discipline requires — deviation disclosed in §3.3, plan amendment still
   pending (Owner action); the M3 concurrent-modification divergence cell
   was never run and never registered NOT_RUN — now recorded in §3.3.
8. **Post-session transcription-correction record (this revision).** An
   independent review of the deliverables against `p3-raw` found that the
   three in-session ad-hoc extraction snippets' `sys_ns_per_op` mis-parse
   defect (item 7, third bullet) had in fact leaked into the published
   document numbers despite the in-session "exact field matching" re-check:
   the M4 working-set constants (420.1/677.6/635.8 ns/op) were single-sample
   `sys_ns_per_op` values, not wall medians (archived medians
   788.2/965.1/929.0 — the +61% out-of-band disclosure built on them is
   withdrawn; the archived +22.4% is in-band); the M4 cold/warm ratios
   (23.4×/29.3×) divided a single-sample wall `ns_per_op` by a single-sample
   rusage `sys_ns_per_op` (corrected median-caliber 13.6×/27.9×); the M2
   T-sweep extra-bytes column quoted the ops=64 smoke gate's request set
   instead of the timed ops=1024 cells (corrected +103/+444/+1821/+7061/
   +28 828 B, amplifications 1.215×/1.880×); the planner-cell instruction
   figure had no archival counterpart (corrected 1 162 684, Δ+8053); and a
   broad set of single-arm wall/CPU/instr values deviated from the archived
   analysis-block medians (representative: M1 k4 2449.5/874.3 → 2451.6/876.7,
   M2 T=4K 8595.6/8279.4 → 8599.9/8281.8, M3 tmpfs 19 627.7/17 553.9 →
   19 676.0/17 626.1). All paired deltas and CIs were re-verified exact.
   Every affected number in this document, the ledger and the candidates
   file has been re-aligned to the archived analysis blocks; no verdict
   direction other than the two described below changed. Two consequences
   beyond transcription: the PRED-M4-3 out-of-band disclosure is withdrawn
   (in-band at the archived constants), and the M2 COST_MIGRATION_ONLY
   trigger was recomputed from the archived evidence and fires (§2.3),
   degrading the threshold-merge verdict per plan §8.

**Answer to the mission question** (MISSION §5: is there a legal I/O
execution form that completes identical application work with fewer
syscalls, fewer copies, or fewer steps): yes, in three shapes, all at probe
level — vectored positional transfer, intra-set dedup, and kernel-side
file-to-file copy — each real at count and wall/CPU caliber on this host,
each bounded to a specific request shape, and each reachable in-product only
through an Owner-admitted public capability. The negative results are
equally binding: threshold merging beyond a narrow window regresses,
planner overhead is 5× its predicted bound, cold-regime thresholds are
unresolvable at the pinned floors, and wrapper-level optimization has no
remaining wall-caliber headroom.
