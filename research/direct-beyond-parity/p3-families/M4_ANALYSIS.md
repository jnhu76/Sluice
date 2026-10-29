# M4 Family Analysis — Filesystem / Hardware Awareness (Preregistered, Pre-Measurement)

- Family: M4 of `P3_MECHANISM_STUDY_PLAN.md` §5 (Filesystem/hardware awareness).
- Status: **PREREGISTERED BEFORE ANY P3 MEASUREMENT.** Verified this session
  (2026-10-28): `research/direct-beyond-parity/p3-raw/` does not exist — no P3
  arm, kit commit for P3, or timed cell has run (`ls` this session). Every
  prediction in §3 is pinned against numbers that exist only in P1/P0 records,
  cited inline.
- Inputs read for this analysis: `MISSION.md` §5 (M4 subsection);
  `P3_MECHANISM_STUDY_PLAN.md` §0, §1, §5, §6–§9, §12; `P0_BASELINE_AND_OWNERSHIP.md`
  §5 (semantic floor); `DIRECT_COST_FLOOR.md` §1, §4.1, §4.3, §5.3;
  `P1_MEASUREMENT_PLAN.md` §3 (environment pin), §5 (statistics), §7 (cold
  discipline); comparator kit source at the P3-plan-pinned HEAD
  `2364be4a` (`git -C /home/jnhu/Source/Sluice-direct-cost-ladder log --oneline -3`
  this session): `research/direct-cost-ladder/ladder.cpp`.
- Local semantic authorities read this session: `man 2 open` (O_DIRECT + NOTES),
  `man 2 posix_fadvise`, `man 5 tmpfs`, `man 2 read` (all via `man -w` resolved
  on this host; kernel 7.2.11-200.fc44.x86_64 per `uname -r`).
- Host facts verified this session (commands in §6): L3 25 MiB (1 instance),
  L2 2.5 MiB, L1d 320 KiB, 1 NUMA node (`lscpu`); `/` and `/var/tmp` on btrfs
  `/dev/sda3[/root]` with `compress=zstd:1,ssd,discard=async,space_cache=v2`
  (`findmnt`); `/tmp` tmpfs `size=32838152k` (~31.3 GiB), `nr_inodes=1048576`
  (`mount`); 62 GiB RAM, ~54 GiB available (`free -h`); THP `enabled=[madvise]`,
  `shmem_enabled` raw line `always within_size advise [never] deny force`;
  `/proc/sys/vm/drop_caches` present, root-writable.

## 0. Grounding arithmetic recomputed this session (before any M4 cell)

All inputs are `[TRANSCRIBED]` from `DIRECT_COST_FLOOR.md` §4.1/§4.3 and
`p1-raw/cold-cache-validation.txt` (re-read this session); arithmetic re-run
with python3 (commands in §6):

| Quantity | Value | Derivation |
|---|---|---|
| Warm-tmpfs marginal slope s(4 KiB→64 KiB) | **0.130081 ns/B** | (8696.847−704.666)/(65536−4096), N1 meds, `DIRECT_COST_FLOOR.md` §4.1 |
| Warm-tmpfs s(512 B→4 KiB) | 0.030810 ns/B | (704.666−594.244)/(4096−512) |
| Warm-tmpfs s(64 KiB→1 MiB) | 0.152226 ns/B | (158341.213−8696.847)/(1048576−65536) |
| Cold-btrfs ns/B @4 KiB | 2.1174 | 8672.99/4096 (N1, `DIRECT_COST_FLOOR.md` §4.1) |
| Cold-btrfs ns/B @64 KiB | 2.2951 | 150413.772/65536 (N1, same) |
| Warm-tmpfs ns/B @4 KiB | 0.1720 | 704.666/4096 |
| Warm-btrfs ns/B @4 KiB (A0, whole-file warm) | 0.2259 | 925.420/4096 (`p1-raw/cold-cache-validation.txt` line 6) |
| Cold/warm ratio, btrfs denominator | **9.37×** | 8672.99/925.420 (the file's own "≈9.4×") |
| Cold/warm ratio, tmpfs denominator | 12.31× | 8672.99/704.666 |
| Fixed per-op cost C(warm tmpfs) | 543–560 ns | A0 0 B floor 543.161 (`DIRECT_COST_FLOOR.md` §4.1 context floors); 1 B N1 553.850 |
| Warm write−read gap @4 KiB (N1) | +60.2 ns (+8.5%) | 764.864−704.666 (`DIRECT_COST_FLOOR.md` §4.1/§4.3) |
| Warm write−read gap @1 MiB (N1) | +2782 ns (+1.76%) | 161123.160−158341.213 |

**Discrepancy flagged (OQ-3):** the P3 plan §0 quotes the P1 slope as 0.128
ns/B; the P1 table values give 0.130081. The 1.6% transcription delta does not
move any band below (PRED-M4-1's [0.08, 0.25] is untouched), but the M4 record
should anchor on the recomputed 0.130081 and note the plan's 0.128 as a
rounding artifact — never the reverse.

**Anchor caveat (OQ-5, structural):** the P1 sweep cells carry *implicit
working sets* of `size × min(ops, file_blocks)` because sequential offsets are
`(i % file_blocks)·block` (`ladder.cpp:778`) and only the first `ops` indices
are touched: 4 KiB×1024 ops = 4 MiB (fits L3), 64 KiB×512 = 32 MiB (exceeds L3
25 MiB), 1 MiB×256 wraps the whole 64 MiB fixture 4×. The 0.130 ns/B anchor
therefore already straddles one L3 crossing. The M4 size-sweep cells reproduce
this anchor shape (same ops table) for comparability; the M4 working-set cells
exist to disentangle the axis (§3 PRED-M4-3/7, §5 OQ-1).

## 1. Legal framework — awareness dimensions vs the v2-r1 public contract

### 1.1 Position

M4 proposes **no mechanism of its own** (plan §5.1). Every M4 artifact is an
*environmental constant* (ns/B marginals, fixed per-op costs, the
syscall-vs-byte exchange rate R) consumed by M1–M3 trade-off models; any
"read bigger blocks" conclusion is filed under **M2's evidence**, and any
kernel-transfer cost comparison belongs to M3. No M4 cell may change which
public operations are called or their observable semantics: the arms are the
existing a1/a2 (and, for the buffered-write comparison, w1/w2) shapes, which
preserve the P0 §5 floor on the probe's legal domain (`P1_MEASUREMENT_PLAN.md`
§2).

### 1.2 Awareness-dimension matrix (fixed)

| # | Dimension | What varies / what is held | Legal measurement forms (v2-r1-compatible) | Prohibited / illegal forms (with ground) | Consumers of the constant |
|---|---|---|---|---|---|
| 1 | Page-cache state (hot/cold) | Cache residency of source pages | P1 §7 discipline verbatim: `sync; echo 3 > /proc/sys/vm/drop_caches` via sudo before **every** invocation, btrfs fixtures only, warmup=0, reps=1, `NOT_RUN_WITH_REASON` fallback; validated on this host (925→8720→954 ns round-trip, tmpfs control unchanged, `p1-raw/cold-cache-validation.txt`) | (a) fadvise(DONTNEED)-as-cold — G0 precedent shows it is effectively warm (`P0_BASELINE_AND_OWNERSHIP.md` §4.10; `man 2 posix_fadvise`: "attempts to free cached pages", "Requests to discard partial pages are ignored" — non-binding eviction, not a cold state); (b) reporting any tmpfs warm read as cold — tmpfs pages are shmem, `drop_caches` does not cool them (P1 §7 validation) | Cold marginals for R_cold; M2 cold T* (PRED-M2-3) |
| 2 | Filesystem (tmpfs vs btrfs) | Backing store and read path of the *same* arm shapes | Same a1/a2 arms on `/tmp` tmpfs and `/var/tmp` btrfs fixtures; disclose mount options (`compress=zstd:1,ssd,discard=async,space_cache=v2`, verified this session) and the tmpfs(5) facts (RAM-resident shmem; can employ swap under pressure — memory headroom checked: ~54 GiB available vs 256 MiB fixture, swappiness 60 recorded) | Substituting tmpfs warm numbers for device-backed behavior in any conclusion (MISSION §3 P1.4 rule, inherited by plan §1.5); presenting btrfs-warm as cold (compress/metadata state uncontrolled, P1 §7 scope) | R_warm_btrfs; M3 btrfs cells context |
| 3 | Request size | Per-op span (512 B…1 MiB), caller-visible, unchanged contract | Existing size-sweep cells a1 vs a2 (one span per call, one syscall per op — exactly the SEM-03…05 single-call shape); P1 ops table for anchor comparability | None at probe level (size is a caller choice, always was); "over-read internally to amortize" is *not* an M4 form — see §1.3(c) | R via size-axis intercept+slope (§2) |
| 4 | Working set (8 MiB < L3 25 MiB < 64 MiB < 256 MiB) | Source-region residency across the cache hierarchy; destination stays hot by construction (single reused buffer, `ladder.cpp:931-934`) | Fixture-size axis with ops scaled so the touched region equals the fixture (OQ-1 pin); warmup ≥ one full cycle so the 8 MiB cell starts L3-resident; both arms in-cell must stay sub-MDE apart so the effect is environmental, not arm-specific | THP/madvise/mlock/prefetch tuning of source or destination — unproven hardware-specific configuration (plan §5.1 prohibition, D-P3-03); current state disclosed instead of tuned (THP `madvise`, shmem THP `never`, this session) | Disentangled marginal (OQ-5); M4-Q3 |
| 5 | CPU cache / memory bandwidth | Hierarchy miss behavior at fixed logical work | PMU `instructions:u,cycles:u` cross-arm deltas per plan §6 (counts caliber, never wall); descriptive IPC change across ws cells; rusage corroborating | Presenting PMU cycles as wall time (P1 §6.3 separation rule); claiming kernel-internal byte movement as *measured* (attribution only, plan §6 row) | IPC-decline evidence for M4-Q3; M1/M2 CPU-side rows |
| 6 | Buffered I/O cost | read vs write on the buffered path (page cache both sides) | Warm tmpfs/btrfs read and write ladder comparisons (P1 §4.1/§4.3 exist; M4 restates them as environment constants; write-side environment *cells* are out of plan §5.2 scope — OQ-9) | O_DIRECT anywhere as an "equivalent" access mode — §1.3(a); O_SYNC/fdatasync-in-read-cells (side-effect boundary, D-P3-04 adjacency) | Write−read gap band; M3 CPU/byte context |
| 7 | NUMA | — | **NOT_RUN_WITH_REASON**: host has 1 NUMA node (`lscpu` this session: `NUMA node(s): 1`, `NUMA node0 CPU(s): 0-19`) | Any single-node "NUMA-aware" claim | none |
| 8 | Derived R(state) | — | Pure derivation from cells above (§2); no new runtime state, no public surface | Treating R as portable or stable across hosts/kernel versions (E5-only rule, plan §13.7); caching R in product code (mechanism-justification gate: it is a host/session research constant, not a semantic fact) | M2 threshold model (PRED-M2-2/3 cross-checks), M1 crossover framing |

### 1.3 SEMANTICALLY_NOT_EQUIVALENT candidates (M4-scoped, with grounds)

These are recorded so no later phase silently promotes them:

(a) **O_DIRECT as a replacement for buffered `read_at`/`write_at` in any cell.**
Grounds, all from this host's `man 2 open` (kernel 7.2.11): O_DIRECT "may
impose alignment restrictions on the length and address of user-space buffers
and the file offset"; "the handling of misaligned O_DIRECT I/Os also varies;
they can either fail with EINVAL or fall back to buffered I/O" — i.e. the
observable admission/outcome for an arbitrary caller span (the SEM-03/SEM-04
domain: any representable offset/length, any non-null buffer) becomes
filesystem- and alignment-dependent, which the current contract cannot
express. Additional hazards the man page pins: fork(2)+private-mapping data
corruption, "avoid mixing O_DIRECT and normal I/O to the same file", and the
man page's own summary ("a potentially powerful tool … disabled by default").
D-P3-02 already excludes it; this entry is the semantic (not just
prohibition) justification. If ever revisited, it is a §11 design candidate
with STATX_DIOALIGN-based capability disclosure, never a drop-in.

(b) **fadvise/madvise/THP policy as a performance mechanism inside the
library.** Not an observable-semantics change per se (`man 2 posix_fadvise`:
"The advice is not binding"), but (i) prohibited as unproven
hardware-specific configuration (plan §5.1, D-P3-03); (ii) a silent advisory
call under a public `read_at` would add work with no contract basis — fails
the AGENTS.md mechanism-justification gate (no requirement, no admitted
workload); (iii) DONTNEED-as-cold is *measurement-invalid* (effectively warm,
G0). Classified: forbidden-by-preregistration; would additionally be
`NO_CHANGE_JUSTIFIED`/gate-blocked if proposed as product mechanism.

(c) **"Read bigger blocks internally" (over-read inside the primitive).**
Changes bytes transferred, EOF-crossing behavior and error attribution per
call — the SEM-03/SEM-05 single-span observable. `SEMANTICALLY_NOT_EQUIVALENT`
for the primitive; legal only as M2's explicit caller composition / host
policy / §11 candidate. M4's R constant *feeds* that decision but the
conclusion is filed under M2's evidence (plan §5.1 last paragraph).

(d) **Reporting tmpfs-warm as device-cold, or fadvise-evicted as cold.**
Evidence-integrity violations rather than contract forms; listed because each
has a recorded precedent (G0's effectively-warm cold column, P0 §4.10).

(e) **THP for the destination/anonymous buffers or shmem.** Would change
fault/RSS observables and TLB behavior without contract support; prohibited
(D-P3-03); disclosed as-is (current: `enabled=[madvise]`, shmem THP `never`,
this session). No action in P3.

### 1.4 Cross-family interfaces (authoritative frames live in the sibling family docs)

M4's constants plug into three frames owned elsewhere; M4 respects their
boundaries and adds no forms of its own:

- **M1 (vectored)** three-case discipline: M4's per-state fixed cost C and
  marginal c parameterize the Case-A crossover analysis (k × C vs iov
  iteration), but the file-contiguous×buffer-scattered / file-scattered /
  independent-ops-error-responsibility distinction is M1's to enforce; M4
  cells never batch anything.
- **M2 (planner)** five proof obligations: R(state) is the *input* to the
  threshold model (T* ≈ R at the margin); M4's answer to the obligations is
  confined to providing state-specific c and C measured on unmerged single-op
  arms (request-set identity, byte correctness, error attribution, boundaries,
  concurrency are all M2-owned).
- **M3 (kernel transfer)** seven disclosures: M4's warm read/write ns/B and
  PMU calibers provide the user-loop side of disclosure 7 (CPU/memory-traffic
  reduction context); the support matrix, short-transfer, offset/error,
  consistency, partial-responsibility and fallback disclosures are M3-owned.
  The M3 disclosures already carry one behavior verified against this host's
  kernel (7.2.11) and man pages at plan §0/§4.1: same-fs `copy_file_range`
  functional on tmpfs and btrfs, NULL-offset position-advance with EOF-zero.

## 2. Exchange-rate R — derivation protocol (pinned before measurement)

Definition (plan §5.2): for each state s ∈ {warm tmpfs, warm btrfs, cold
btrfs},

    R(s) = C(s) / c(s)   [bytes]

- **C(s)** — fixed per-op wall cost attributable to one syscall-class entry,
  derived **within M4** as the size-axis intercept of ns/op vs size for state
  s (fit over the sweep sizes available in that state; cold has {4 KiB,
  64 KiB} two-point form). Cross-check gate: |C(warm tmpfs) − 543.161| ≤ 100 ns
  (A0 0 B floor, `DIRECT_COST_FLOOR.md` §4.1). Disclosed limit: the intercept
  conflates syscall entry with state-specific fixed filesystem work — that is
  precisely why R is per-state.
- **c(s)** — marginal ns/B for state s, from the same cells' slopes. For the
  warm states, both the size-axis marginal (anchor-compatible) and the
  ws-controlled marginal (4 KiB ops over 8/64/256 MiB regions) are reported;
  the R used in M2 cross-checks is the size-axis one (comparable to P1's
  0.130081 anchor), with the ws-controlled value disclosed alongside (OQ-5).
- Bands (§3 PRED-M4-4). R is an E5-session research constant: not portable
  (plan §13.7), not product state.

## 3. Preregistered predictions (each with falsifiable form)

Question IDs follow the task book (M4-Q1…Q4, matching plan §5.3 PRED-M4-1…4);
PRED-M4-5…11 are this family analysis's qualitative syscall/copy/step
predictions. All wall claims use plan §7 calibers (16 ABBA pairs, CI
excluding 0, MDE floors: read warm 5.535 ns / cold 1416.185 ns inherited;
cross-cell comparisons per PRED-M4-3 use each cell's 16 a1-arm invocation
values, median + 95% t CI, with the in-cell a2−a1 delta required sub-MDE so
the effect is environmental).

- **PRED-M4-1 (=M4-Q1).** Warm-tmpfs marginal byte cost from the size sweep
  (4 KiB→64 KiB slope, 64 MiB fixture, P1 ops table): point value ∈
  **[0.08, 0.25] ns/B** (grounding 0.130081, §0). Falsified iff the measured
  slope lies outside the band. The ws-controlled 4 KiB marginal
  (DRAM-resident source) is predicted ∈ [0.10, 0.35] ns/B — a superset band
  on the high side of the anchor because the anchor's small-size segment was
  partially L3-resident (§0 caveat).
- **PRED-M4-2 (=M4-Q2).** Cold/warm ratio on btrfs at 4 KiB, denominator
  **warm btrfs, same shape** (the 925.420-class whole-file-warm value):
  **≥ 8×**; grounding recomputed 9.37× (btrfs denominator; 12.31× with the
  tmpfs denominator — OQ-2 pins the denominator to btrfs and reports the
  tmpfs-denominator variant descriptively). Falsified iff ratio < 8×.
  Secondary shape: ratio @64 KiB ≥ ratio @4 KiB (grounding 2.2951/0.130-class
  cold marginal vs 2.1174/… at 4 KiB).
- **PRED-M4-3 (=M4-Q3).** Working-set axis at 4 KiB seq warm tmpfs (ops scaled
  per OQ-1 so the touched region = fixture):
  (a) 256 MiB vs 64 MiB (both DRAM-resident sources): ns/op increase ≤ 15%;
  falsified iff increase > 25% with CI excluding 0 (plan wording verbatim);
  (b) L3 crossing 64 MiB vs 8 MiB: ns/op(64m) > ns/op(8m) with CI excluding 0,
  point band **[+3%, +40%]** (central ≈ +22%: the ≈155 ns byte component of
  the ≈705 ns op roughly doubling when the source stream leaves L3);
  (c) shape ordering ns/op(8m) < ns/op(64m) ≈ ns/op(256m), with 64m→256m
  within ±10%. Falsified by any CI-excluding violation of (b)'s direction or
  (c)'s ordering.
- **PRED-M4-4 (=M4-Q4).** R ordering **R_warm_tmpfs > R_warm_btrfs >
  R_cold_btrfs** with **R_warm_tmpfs ≥ 8 × R_cold_btrfs**. Bands (C ∈
  [543, 700] ns, §0; c per state): R_warm_tmpfs ∈ [2.2, 8.8] KiB (central
  ≈ 4.2 KiB); R_warm_btrfs ∈ [0.9, 3.5] KiB (c_warm_btrfs ∈ [0.2, 0.6] ns/B
  prior — no direct P1 measurement, OQ-4); R_cold_btrfs ∈ [236, 333] B
  (c_cold ∈ [2.1, 2.3] ns/B). Central ratio R_wt/R_cb ≈ 16.9×. Falsified iff
  any ordering inverts, the ×8 floor fails, or any R lands outside its band.
  Cross-checks: R_warm_tmpfs must fall inside M2's PRED-M2-2 window
  T* ∈ [1 KiB, 16 KiB] (a value outside refutes one of the two predictions);
  R_cold_btrfs ≤ 330 B is the PRED-M2-3 grounding ("break-even ≤ ≈330 B") —
  a measured R_cold > 330 B invalidates that grounding.
- **PRED-M4-5 (syscalls — count invariance across all M4 axes).** At every
  size ≥ 1 B: exactly **1.000 pread64/op** (w1/w2: 1.000 pwrite64/op) for a1
  and a2; at 0 B: **0.000 syscalls/op**; identical across {tmpfs, btrfs} ×
  {warm, cold} × {8m, 64m, 256m} × all sizes. Probe criterion: `strace -c`
  integral counts, window-isolated per plan §6, exactly equal across ≥ 3
  diagnostic repeats. Falsified by any variance or any count ≠ prediction —
  environment axes move time, never call structure.
- **PRED-M4-6 (copies — one in-kernel copy per byte, zero staging).** Warm
  reads copy each payload byte exactly once in-kernel (page-cache/shmem page →
  caller buffer; "warm" ≠ zero-copy — tmpfs(5) RAM residency still pays
  copy-to-user every op); zero user-space staging copies in a1/a2 (caller span
  passed directly to `::pread`, `ladder.cpp:90-107` and the mirrored public
  path). Probe criterion: structural call-shape certification + PMU
  corroboration — instructions:u/op within ±5% across ws cells (repeat
  caliber, plan §7.6) while cycles:u/op(256m) > cycles:u/op(8m) (IPC decline
  from DRAM stalls). Falsified iff instr/op varies > 5% across ws cells (a
  staging copy appeared) or cycles/op is flat across the L3 crossing.
- **PRED-M4-7 (steps — size-sweep slope shape).** Warm-tmpfs segment slopes
  increase monotonically: s(512b→4k) < s(4k→64k) ≤ s(64k→1m) (grounding
  0.031 < 0.130 ≤ 0.152, §0): small reads syscall-dominated, large reads
  bandwidth-dominated. Falsified by any inversion beyond the plan §7.6
  non-integral repeat spread (5%).
- **PRED-M4-8 (filesystem gap, warm).** Warm btrfs ≥ warm tmpfs at every
  sweep size, with the relative gap **declining** as size grows (4 KiB
  grounding 925.4/704.7 = 1.31×; fixed per-op fs overhead amortizing).
  Falsified iff btrfs < tmpfs CI-excluding at any size, or the gap ratio
  increases with size.
- **PRED-M4-9 (buffered write-vs-read gap).** Warm-tmpfs write > read at each
  size; relative gap monotonically decreasing in size (grounding +8.5% @4 KiB
  → +1.76% @1 MiB, §0). Falsified by a CI-excluding inversion at any size or
  a non-monotone gap ordering beyond repeat spread.
- **PRED-M4-10 (wrapper steps — environment invariance of the Sluice tax).**
  The PMU cross-arm instruction delta stays the P1 constant — a2−a1 ≈ +38
  instr/op, w2−w1 ≈ +19 instr/op (±5) — across **every** M4 axis (fs, cache
  state, ws, size): environment moves kernel-side work only; the userspace
  wrapper is axis-invariant. Falsified iff the delta shifts > ±5 instr/op
  with exact-repeat corroboration (would indicate an environment-dependent
  wrapper path — a finding, and a plan §8 `COST_MIGRATION_ONLY`-relevant
  attribution alert).
- **PRED-M4-11 (cold-shape consistency).** Cold-btrfs ns/B @64 KiB ≥
  ns/B @4 KiB (grounding 2.2951 ≥ 2.1174), and both cold cells show ≥ 8×
  their warm-btrfs counterparts (ties to PRED-M4-2). Falsified by a
  CI-excluding reversal.

Falsifier consequences (plan §8 mapping): PRED-M4-1/2/4 failing → the R
model is unsound for this host; M2's PRED-M2-2/3 cross-checks lose their
grounding and any T*-derived conclusion downgrades to NO_CLAIM. PRED-M4-5/6
failing → cell invalid (probe defect, not a finding about the environment);
family blocked until understood (plan §9 discipline). PRED-M4-3 failing →
recorded as a negative environmental result; no mechanism consequence (M4
proposes none).

## 4. New public capability?

**apiCandidateNeeded = false.** M4 introduces no operation, no state, no
observable; its outputs are research constants for M1–M3 models. For
completeness, the §5.3 six-element path is sketched only for the hypothetical
case where an M4-derived conclusion ever demanded one (e.g. exposing a
read-shape/planning advisory): (1) contract — an *advisory* operation with
explicit inputs and no effect on any read_at admission/outcome; (2) relation
to v2-r1 — extends nothing in SEM-02…07; intersects TERM-01/BACKEND-04
justification; (3) opportunity — M2's §5.2 numbers, never M4's alone; (4)
semantics/errors/lifecycle/portability — advisory-only result shape, no
effect certainty, and R is host-specific so the advisory must re-derive per
host (E5-only evidence cannot generalize, plan §13.7); (5) filed as a design
issue/ADR candidate referencing the evidence; (6) awaiting Owner — nothing
auto-adopts. This sketch creates no candidate; if the read-shape route ever
productizes, it is M2's §11 filing.

## 5. Open questions (each blocks or conditions a cell; none guessed around)

- **OQ-1 (execution-critical pin): ws-cell ops scaling.** Plan §5.2 does not
  pin `ops` for the ws cells. With the P1 ops table (4 KiB → ops=1024), the
  touched region is `min(ops, file_blocks)` blocks from offset 0
  (`ladder.cpp:778`, `:925-929`) = 4 MiB regardless of fixture size — the axis
  degenerates to three identical cells. Pin before any ws cell:
  ops ≥ file_blocks (8m: ≥2048; 64m: ≥16384; 256m: ≥65536) and warmup ≥
  file_blocks (so the 8 MiB cell starts L3-resident). The kit-extension commit
  (plan §1.6(c)) must record these parameter shapes; if this requires a plan
  change beyond a cell-parameter pin, it goes through the amendment log, not
  silently.
- **OQ-2: M4-Q2 denominator.** "grounding ≈ 12×" in the plan corresponds to
  the tmpfs-denominator variant (12.31×); the btrfs-denominator grounding is
  9.37×. Pin the claim's denominator to warm-btrfs same-shape (both ≥ 8×
  hold); report the other descriptively.
- **OQ-3: anchor rounding.** Plan §0's 0.128 vs recomputed 0.130081 (§0
  above). Bands unaffected; M4 records must cite 0.130081.
- **OQ-4: no prior warm-btrfs marginal.** P1 measured warm tmpfs and cold
  btrfs; warm-btrfs c enters only via the new btrfs-warm sweep cells. The
  R_warm_btrfs band [0.9, 3.5] KiB is correspondingly wide (prior c ∈
  [0.2, 0.6] ns/B). The *ordering* prediction (M4-Q4) is robust to this; the
  magnitude is not yet.
- **OQ-5: size-axis/ws-axis entanglement in the anchor.** §0 structural
  caveat: P1 sweep cells' touched regions were 4 MiB / 32 MiB / 64 MiB. R
  derivation must state which marginal it uses (§2 pins: size-axis for the M2
  cross-check, ws-controlled disclosed alongside).
- **OQ-6: btrfs compress=zstd:1.** Whether the deterministic LCG fixture's
  extents are stored compressed is uncontrolled and affects cold-cell
  interpretation (decompression on the read path). Disclosed per P1 §7 scope;
  optionally checkable at run time (e.g. `filefrag -v` extent layout) without
  forcing either outcome.
- **OQ-7: sudo availability at P3 execution time.** Cold cells fall back to
  `NOT_RUN_WITH_REASON` (P1 §7); if that happens, R_cold is not derived and
  PRED-M4-4's third term is unmeasured — recorded, never substituted.
- **OQ-8: L3-residency assumption of the 8 MiB cell.** Near-full residency of
  the 8 MiB source region in the 25 MiB shared L3 on a quiescent host is an
  assumption, not directly verifiable without kernel counters. Mitigation
  preregistered: report both 64m−8m and 256m−64m deltas (partial residency
  appears as a gradient, breaking prediction (c) of PRED-M4-3 rather than
  silently passing (b)).
- **OQ-9: write-side environment axes are not M4 cells.** Plan §5.2 cells are
  read-side only; the buffered-write comparison (PRED-M4-9) rests on P1's
  warm tmpfs write ladder and P1's single cold-write cell (whose dependence on
  dirty-writeback timing is already disclosed, P1 §7). If any M2/M3
  conclusion needs a write-side R, that is an amendment, not an assumption.
- **OQ-10: cross-cell comparison caliber for M4-Q3.** The ABBA pairing
  controls arm-vs-arm within a cell; the ws axis compares *cells*. Pinned in
  §3: N1-arm medians over 16 invocations per cell, 95% t CI, in-cell a2−a1
  required sub-MDE; this caliber limitation is disclosed with every ws claim.

## 6. Commands run this session (verification record)

```sh
uname -r                          # 7.2.11-200.fc44.x86_64
lscpu | grep -E 'Model name|L1d|L2|L3|NUMA|Socket|Core|Thread'
findmnt -n -o FSTYPE,SOURCE,TARGET / ; … /tmp ; findmnt -T /var/tmp   # btrfs /dev/sda3[/root]; /tmp tmpfs
mount | grep -E ' on /tmp '        # size=32838152k, nr_inodes=1048576
free -h                            # 62 GiB total / ~54 available
cat /proc/sys/vm/swappiness        # 60
cat /sys/kernel/mm/transparent_hugepage/enabled        # always [madvise] never
cat /sys/kernel/mm/transparent_hugepage/shmem_enabled  # always within_size advise [never] deny force
ls -la /proc/sys/vm/drop_caches    # --w------- root root (present)
man -w 2 readv preadv2 copy_file_range sendfile open posix_fadvise madvise read ; man 5 tmpfs
man 2 open | sed -n '/O_DIRECT/,…'  # alignment/misalignment/EINVAL-or-fallback/mixing/fork hazards
man 2 posix_fadvise                 # "The advice is not binding"
ls …/research/direct-beyond-parity/p3-raw  # absent → no P3 measurement yet
git -C /home/jnhu/Source/Sluice-direct-cost-ladder log --oneline -3   # 2364be4a (plan §0 kit state)
grep/sed of ladder.cpp at 2364be4a   # seq offsets :778; file_blocks :925-927; single reused buf :931-934
python3 (arithmetic of §0)          # slopes, ns/B, ratios, R bands
grep DIRECT_COST_FLOOR.md §5.3 + sed p1-raw/cold-cache-validation.txt  # 925.420/8720.546/954.377/1010.792… lines
```

Checks **not** run (stated, not assumed): no M4 cell, no A/A, no strace/perf
run — this document exists precisely before those; sudo drop_caches
availability for the *P3 session* is OQ-7 (P1-validated, not re-validated
here); no attempt was made to determine fixture compression state (OQ-6).
