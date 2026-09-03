# RE-H0 REPORT — native semantic-floor residual and performance-envelope closure

Campaign **#277** (authority #227, G1-PERFORMANCE). Branch
`research/re-h0-native-performance-closure`, BASE master `7653fd8d`,
freeze commit `a0281a93` (preregistration frozen before any formal
measurement). Measured binaries unchanged from their original frozen
forms (`04e19c56` Z-ladder, `fb41a188` e1 ladder); production code as
built at `7653fd8d` (post-#274 router R1, post-#258/#276 drain fixes).

---

## 0. Verdict summary

```text
G1-PERFORMANCE (HOST-0):  PARTIAL — regime-dependent
  control-plane-dominated small I/O (4 KiB × d1..d8):
      HOST-LOCAL MATERIAL BACKEND TAX — frozen composite verdicts
      MATERIAL (btrfs S read; RE-2 4K×d1 read+write, 4K×d8 read),
      Sluice uring boundary ≈ 2.8–5.1× floor instructions/op with
      wall separation present on read cells
  large / application-relevant I/O (64 KiB–2 MiB):
      NO MATERIAL BACKEND TAX DETECTED in any tested cell — but
      most of these cells are formally GRAY under the frozen
      composite rule, NOT PARITY; the one clean PARITY witness is
      2 MiB × d1 READ, and several cells carry recorded direction
      anomalies (candidate faster in wall)
  runtime layer (T_runtime): every frozen verdict GRAY — large
      instruction overhead at 4 KiB (≈ 2.8–4.0× the hand-written
      continuation) but the wall layer moves in the opposite
      direction / lacks material separation, so the frozen
      composite MATERIAL gate is never met
GENERAL G1-PERFORMANCE:   NOT YET ADJUDICABLE
  (second machine class / modern NVMe / ARM64 unavailable; #270
  DEFERRED / NOT EXECUTED, remains OPEN)
```

This is the mission's PARTIAL shape, and it is a useful result: the
residual tax is localized where the explicit machinery has the least
work to amortize over, and it is quantitatively characterized per
layer. The defensible large-I/O claim is "no material tax detected in
the tested cells", not "parity proven": the frozen P8 vocabulary is
the only verdict authority, and it returns GRAY for most large cells
(§3.3, §6).

---

## 1. RE-0H-H0 — environment qualification

| fact | value |
| --- | --- |
| native Linux | YES — Fedora 44, kernel 7.1.9-200.fc44 x86_64, no virtualization |
| CPU | 10-core Haswell-EP Xeon E5-2666 v3, 1 NUMA node, schedutil |
| cache line / page | 64 B / 4 KiB |
| RAM | 62 GiB |
| real block device | YES — SATA SSD (`sda`, 447G, ROTA=0) |
| filesystem (primary) | btrfs `compress=zstd:1,ssd,discard=async` on `/home` |
| tmpfs control | `/tmp` (tmpfs, 32 GiB) |
| real io_uring / liburing | YES / 2.13 |
| compiler | clang 22.1.8 (Release, warnings-as-errors) |
| perf | 7.1.9, `perf_event_paranoid=2`, `instructions:u` verified |
| memlock | 8 MiB (constrains perf-record mmap; census used `-m 1`) |
| NVMe | an unmounted `nvme0n1` exists with no Linux filesystem; NOT part of the fixed campaign environment (mission scope) — recorded for honesty, not used |
| second machine class | NOT AVAILABLE |

**RE-0H verdict: HOST-0 QUALIFIED FOR HOST-LOCAL NATIVE MEASUREMENT.
NOT QUALIFIED FOR BROAD NEAR-NATIVE / CROSS-HOST CLAIM.** No tmpfs
evidence is used in any real-storage claim below.

---

## 2. #262 pre-measurement stop gate

Protocol: perf-wrapped Z2/Z3 × READ/WRITE × cells {S=4K×d8, L=2M×d2},
N = 20 launches each = **160 launches**,
session `re-h0-qual262-20260903-034305`.

```text
unexpected -ECANCELED : 0
named drain stall     : 0
wait error            : 0
teardown failure      : 0
same-work mismatch    : 0
```

**PASS — 160/160 clean.** The campaign proceeded; #262 remains OPEN for
the unresolved kernel-originated trigger (its historical observations are
WSL2-specific; zero Host-0 events across 160 qualification launches,
720 formal launches and 30 ATTR-B corrective launches). No stop-gate
event occurred at any point in
this campaign; there was nothing to retry.

---

## 3. RE-1U-H0 — semantic-floor ladder

Session `re-h0-re1u-20260903-034519`: 40 combos × 53 measured reps
(11 wall + 2×(7/14) perf double-difference), 200 launches, 0 invalid.
Primary metric: user instructions/op (two independent estimates each).
Verdicts by the frozen P8 vocabulary (wall ratio + interval separation +
all instruction estimates agreeing); both layers reported everywhere.

### 3.1 Cell S — 4 KiB × d8 (control-plane dominated)

| btrfs READ | instr/op | wall/op | MiB/s |
| --- | --- | --- | --- |
| Z1 raw liburing | 1 044 | 834 ns | 4 685 |
| Z1b semantic floor | 1 097 | 883 ns | 4 422 |
| Z1bw +continuation | 1 391 | 1 761 ns | 2 218 |
| Z2 Sluice backend | 3 098 | 1 189 ns | 3 286 |
| Z3 Sluice runtime | 3 947 | 1 481 ns | 2 637 |

| btrfs WRITE | instr/op | wall/op |
| --- | --- | --- |
| Z1 / Z1b | 637 / 694 | 3 392 / 3 214 ns |
| Z1bw | 1 048 | 3 486 ns |
| Z2 | 3 572 | 3 003 ns |
| Z3 | 4 143 | 2 951 ns |

tmpfs controls show the same instruction-layer shape
(Z1b 1 111 → Z2 3 230 read; Z1b 634 → Z2 2 919 write) with faster walls.

### 3.2 Cell L — 2 MiB × d2 (sweet-region representative)

| btrfs READ | instr/op | wall/op | MiB/s |
| --- | --- | --- | --- |
| Z1 | 458 951 | 324 µs | 6 180 |
| Z1b | 459 006 | 332 µs | 6 018 |
| Z1bw | 459 544 | 336 µs | 5 959 |
| Z2 | 461 277 | 328 µs | 6 094 |
| Z3 | 462 655 | 324 µs | 6 169 |

WRITE (btrfs): Z1 215 168 → Z3 223 290 instr/op (+3.8 % worst case),
walls 786–839 µs (writeback-dominated). tmpfs mirrors this.

### 3.3 Frozen decomposition verdicts

| block | C_sem Z1b/Z1 | T_backend Z2/Z1b | C_cont Z1bw/Z1b | T_runtime Z3/Z1bw | CASE |
| --- | --- | --- | --- | --- | --- |
| btrfs S read | 1.060 GRAY | **1.346 MATERIAL** | **1.994 MATERIAL** | 0.841 GRAY† | **B** |
| btrfs S write | 0.948 GRAY† | 0.934 GRAY† (instr 5.1×) | 1.085 GRAY | 0.846 GRAY† | GRAY |
| btrfs L read | 1.027 GRAY | 0.987 GRAY† | 1.010 PARITY | 0.966 GRAY† | GRAY |
| btrfs L write | 1.008 GRAY | 0.938 GRAY† | 0.977 GRAY† | 1.025 GRAY | GRAY |
| tmpfs S read | 1.002 GRAY | **1.345 MATERIAL** | **1.488 MATERIAL** | 1.067 GRAY | **B** |
| tmpfs S write | 0.986 GRAY | **1.293 MATERIAL** | **1.569 MATERIAL** | 0.945 GRAY† | **B** |
| tmpfs L read | 1.067 GRAY | 1.008 GRAY† | 1.092 GRAY | 0.920 GRAY† | GRAY |
| tmpfs L write | 0.994 GRAY | 1.061 GRAY | 1.067 GRAY | 0.930 GRAY† | GRAY |

† direction anomaly: the candidate measured **faster in wall** than its
baseline. The frozen P8 rule classifies any ratio < 1.0 as GRAY and
reports it; it is never folded into parity and never called a
negative tax.

**Layer interpretation (both layers always shown):**

- **Instructions (primary, P7).** At 4 KiB the backend layer costs
  ≈ 2.8× (read) to 5.1× (write) the semantic floor's instructions
  (deltas ≈ +2 000 and +2 880 instr/op), and the runtime layer costs
  ≈ 2.8–4.0× the hand-written continuation's instructions
  (≈ +2 556 / +3 095 instr/op). At 2 MiB the entire ladder sits within
  +0.5–3.8 % of Z1 (459 K→463 K read; 215 K→223 K write instr/op):
  the per-op control plane is amortized to noise.
- **Wall.** Read cell S shows the instr tax partially visible in wall
  (T_backend 1.35 MATERIAL); write cells are writeback-saturated and
  wall cannot resolve the instr-layer tax (frozen rule says GRAY, not
  PARITY — recorded as such). Large cells: wall sits within 0.5–3 % of
  baseline on btrfs with several direction anomalies — formally GRAY†,
  not PARITY. (The only formal wall-PARITY of the whole campaign is
  RE-2's 2 MiB × d1 READ; §6.)
- **C_cont ≈ 1.99 wall / 1.27 instr (S read)** is the price of ANY
  waitable-completion consumer as hand-written (per-op futex wake):
  it is the *capability* baseline Z3 must beat, not a Sluice cost.
  Notably Z3's measured rep-envelope wall is 16 % *below* Z1bw's
  (0.841) while spending materially more user instructions. The
  mechanism behind this direction anomaly is NOT attributed: the two
  arms share the rep envelope, not the substrate (AUDIT §3.1), and
  wall-vs-instruction divergence is exactly why the frozen rule
  reports per-layer facts and refuses to collapse them into one
  number.

Plots: `plots/z-ladder-instructions-per-op.svg`,
`z-ladder-cost-ratios.svg`, `z-ladder-throughput.svg`.

---

## 4. RE-1-H0 — blocking / ThreadPool ladder

Session `re-h0-re1-20260903-040153`: 24 combos, 120 launches, 0 invalid.
e1 ladder unchanged; `W = depth` (competent fixed-pool sizing),
`T_pool = L1/L0`, `T_sluice = L2/L1`.

| btrfs | L0 instr/op | L1 | L2 | T_pool | T_sluice |
| --- | --- | --- | --- | --- | --- |
| S read | 1 016 | 1 821 | 4 323 | **5.87 MATERIAL** | **1.80 MATERIAL** |
| S write | 544 | 1 346 | 3 993 | **1.19 MATERIAL** | 1.08 GRAY |
| L read | 458 890 | 459 873 | 465 406 | 1.15 GRAY | 1.75 GRAY† |
| L write | 214 924 | 216 246 | 222 733 | 1.04 GRAY | 1.02 GRAY |

| tmpfs | T_pool | T_sluice |
| --- | --- | --- |
| S read | **6.43 MATERIAL** | **2.03 MATERIAL** |
| S write | **2.35 MATERIAL** | **1.65 MATERIAL** |
| L read | 0.98 GRAY | 2.48 GRAY† |
| L write | 2.31 GRAY | 1.006 PARITY |

**RE-1 primary question answered:** relative to a competent fixed
ThreadPool, the synchronous/Sluice abstraction adds a MATERIAL wall cost
only in the control-dominated small-read regime (1.8–2.0×); at 2 MiB the
instruction layer shows no material tax (L2 +1.2 % over L1 read; frozen
T_sluice GRAY) and wall differences are latency-shaped, not CPU-shaped
(see † below). The
L1-vs-L0 pool round-trip itself (T_pool 5.9–6.4 on S read) is larger
than any Sluice increment — the execution-machinery cost dominates the
explicit-control-plane increment on this host.

† `T_sluice ≈ 2.5–2.8` on large-shallow reads with the **instruction
layer inside the parity band** (tmpfs L read: L2 538.6 µs ± tiny MAD vs
L1 217.3 µs, instr 1.013; the frozen composite verdict stays GRAY): a
wall-latency divergence whose cause is UNRESOLVED — wake path,
scheduler/blocking-worker topology, storage scheduling and runtime
round-trip shape are all candidate contributors. The frozen rule reports
GRAY (wall material, instructions not agreeing); recorded as an open
latency question, not as a claimed mechanism, not as CPU tax and not as
parity.

RSS (e1 arms, per raw launch JSON): ~4 MB class, no separation.
Context switches: not collected (no reliable per-combo source; declared
in P7, not hidden). z-arm RSS: not emitted by the instrument; declared.

---

## 5. RESIDUAL ATTRIBUTION (CASE B earned: 4 KiB cells)

Prereg P9 requires for CASE B: `census → rank → ONE causal ablation →
remeasure`. The original evidence commit stopped after census/rank and
described the result as a completed attribution. The #278 review
correctly rejected that: **a census locates instructions; it does not
prove the located sites causally explain the residual.** This section
reports the corrected state: the census with its instrument disclosure,
the corrected semantic-boundary reading, and the one preregistered
causal ablation (`RE-1U-ATTR-B`, frozen separately before its own
measurement).

### 5.1 Census (as captured) and instrument disclosure

Method per P9: census first, no production change. Symbolized
instruction census (same sources; formal numbers remain bound to the
stripped formal binaries by SHA — the rebuilt formal binary reproduces
its recorded SHA exactly, `7401213f…`, a reproducibility witness):

| share of user instructions (S read) | Z1b floor | Z2 backend | Z3 runtime |
| --- | --- | --- | --- |
| workload validation (`word_sum`) | **91.9 %** | 39.2 % | 33.8 % |
| Sluice machinery (all sites) | ~3 % | ~57 % | ~60 % |

Ranked Sluice symbols in Z2 (percent of process user instructions, as
captured): `RouterEntry::size()` 4.8, `RequestSlot::size()` 3.8,
`Completion` state CAS 2.1, mutex lock/unlock traffic ≈ 6.3 combined,
`submit_transaction` 1.5, `PreparedUringOp::size()` 1.3,
`RequestArena::validate_` 1.3, reap/publication and accounting below
1 % each. Z3 adds scheduler/wait mutex traffic (lock+unlock ≈ 11.5).

**Instrument disclosure (corrective).** The symbolized census was
captured on a `releasedbg` rebuild for symbols. In this project
`releasedbg` is NOT an optimized mode: only `mode.debug`, `mode.release`
and `mode.valgrind` rules are registered (`xmake.lua`), so a
`releasedbg` configuration injects no optimization flag (clang default
`-O0`, symtab only). At `-O0`, each `vector<T>::size()` probe is an
outlined call whose body computes a pointer difference with an integer
DIVISION by `sizeof(T)` (`idivq`) — a code shape that does not exist at
the release `-O3` optimization of the measured binaries (user-CPU
contrast on the same S-read workload: ≈ 7×). **The census per-symbol
shares are therefore properties of the `-O0` symbolization build, not
of the measured release binaries.** The census remains valid as
evidence of which source-level code families execute per op (the
ranking input P9 requires); its shares are never cited as
release-binary share authority, and no claim in this report rests on
them quantitatively. All instruction/wall measurements are unaffected
(they were taken on the release binaries by SHA).

### 5.2 Semantic-boundary reading (corrected)

Z1b itself prices the required semantic capability: ≈ 1 097 instr/op at
S read for the frozen F05 checklist (bounded in-flight, stable identity,
stale protection, exactly-once, buffer safety). The Z1b→Z2 residual
(≈ +2 000 instr/op) is therefore — by construction of the frozen
decomposition — the cost of **Sluice's implementation** of those
semantics, not the cost of the semantics themselves. The earlier
description ("the footprint of the required semantics themselves") was
contradicted by the campaign's own Z1→Z1b step and is withdrawn.

The census is **consistent with** that implementation cost being
distributed across Sluice's realization of the semantic boundary —
per-op arena admission stages, dispatch/ledger bookkeeping, CQE
routing/terminal/reap, completion publication — rather than concentrated
in one pathological site (no single census symbol exceeded ~5 % of
process instructions as captured). Consistency is not attribution:
the preregistered ablation below (§5.3) is the campaign's only causal
test, and it falsified the census-ranked hypothesis rather than
completing attribution. Whether the distributed implementation
overhead is structurally reducible remains an open engineering
question outside this campaign's authority (a fix-selection campaign
under #255 discipline would own it).

### 5.3 RE-1U-ATTR-B — the one preregistered causal ablation

Frozen in `RE-H0-ATTR-B-PREREGISTRATION.md` BEFORE its own measurement
(one narrowly defined, semantics-identical research-only treatment on
the census-ranked family; R0 = production behavior, R1 = treatment;
same protocol P6–P8; outcome vocabulary A/B/C with all three outcomes
allowed, including "the family is falsified" and "attribution remains
unresolved"). No production change is proposed or made; no candidate
shootout is authorized.

Session `re-h0-attrb-20260903-102706`: 30 launches, 0 invalid, 0
stop-gate events; measured at freeze head `2e247aee` (tracked tree
clean; the session's `dirty` flag is the untracked build dirs), bench
SHA `e2695993…` (internal-testing link — the F01/F02/F07 seams live
only there). Same-work word_sum identical across all arms and equal to
the formal campaign's.

| btrfs S read (4K × d8) | instr/op (2 estimates) | wall/op |
| --- | --- | --- |
| Z1b floor | 1 097 / 1 097 | 828 ns |
| Z2-R0 (production behavior) | 3 302 / 3 302 | 1 123 ns |
| Z2-R1 (F07 cached extents) | 3 288 / 3 288 | 1 129 ns |

```text
denominator (Z2-R0 − Z1b)     : 2 205 instr/op  (CASE B witness
                                reproduced in-session)
recovery (R0 − R1)            :   14 instr/op
fraction (per estimate)       : 0.006 / 0.006   → threshold: < 0.02
OUTCOME                       : NO_RECOVERY   (frozen A7 mapping B)
wall R1/R0                    : 1.006 (parity band; the treatment's
                                per-call mode-flag branch costs nothing
                                measurable)
```

Instrument witnesses and declared limitations:

- Z1b reproduces the formal RE-1U floor EXACTLY (1 097 = 1 097
  instr/op) — the A/B instrument reproduces the campaign floor.
- Z2-R0 sits ≈ +6.6 % above the formal production-binary z2 level
  (3 302 vs 3 098): the seam build carries the guarded TAX-0U lookup
  diagnostics on the CQE path in BOTH arms, so the A/B subtracts it
  out; the absolute z2 level in this binary is not a production
  number (the production numbers remain bound to `7401213f…`).
- The tmpfs control block was NOT classified: its R0-vs-R1 estimate
  pair crosses zero at noise level (3 440 vs 3 452 on one estimate,
  −12 instr/op ≈ −0.35 %), and the frozen A6 rule fails CLOSED on any
  negative recovery (a treatment anomaly, never a verdict). Declared
  as refused-by-rule; |Δ| ≤ 12 instr/op bounds any tmpfs effect far
  below the NO_RECOVERY threshold, but per the frozen rule the control
  block supports no classification at all.

**Outcome B reading:** the census-ranked router extent-probe family is
causally IMMATERIAL at the release optimization — removing every
per-op extent recomputation recovers 0.6 % of the residual, below half
the NO_RECOVERY threshold and far below the family's captured census
share. H1's census-transfer reading is falsified. What the result
proves is bounded: the F07 extent-probe family does not causally
explain the measured release-optimized residual. It does NOT exclude
the remaining candidate families (completion state transitions,
mutex/locking traffic, RequestArena admission, dispatch/ledger
bookkeeping, reap/publication, the scheduler/wait path) or another
unidentified `-O3` hotspot. The distributed-implementation
interpretation of §5.2 remains CONSISTENT with the evidence, but this
ablation does not causally establish it. Causal attribution of the
residual to a release-level hotspot remains UNRESOLVED — none is
identified, and none is claimed.

```text
backend material tax found : YES — 4 KiB cells only, instruction layer,
                             with wall separation on read cells (frozen
                             composite MATERIAL verdicts; §3.3/§6)
runtime material tax found : NO under the frozen composite rule — every
                             T_runtime verdict is GRAY; a large 4 KiB
                             instruction overhead (≈ 2.8–4.0×) is
                             observed, but the wall layer moves in the
                             opposite direction / lacks material
                             separation, so the frozen MATERIAL gate is
                             not met (§3.3)
attribution status         : the one preregistered ablation (ATTR-B)
                             found the census-ranked family causally
                             immaterial (recovery 0.6 %, NO_RECOVERY);
                             the distributed implementation-cost
                             interpretation remains consistent with the
                             evidence but is NOT causally established —
                             the release-level causal hotspot is
                             UNRESOLVED
candidate                  : none selected
production change          : NO
```

---

## 6. RE-2-H0 — initial performance/value envelope

Sessions `re-h0-re2u-20260903-041016` + `re-h0-re2p-20260903-<ts>`:
40 combos per sub-ladder (5 cells × 2 ops × 2 arms × btrfs/tmpfs),
0 invalid. Ratios are candidate-vs-own-floor, btrfs primary shown.

**Uring ladder Z2/Z1b (wall):** 4K×d1 read **1.61 MATERIAL**, 4K×d1
write **1.34 MATERIAL**, 4K×d8 read **1.39 MATERIAL**, 4K×d8 write
0.92 GRAY† (writeback-saturated wall; instr layer 2.8–5×), 64K×d2
1.09/1.02 GRAY, 2M×d1 read **1.004 PARITY**, 2M×d2 1.00/1.01 GRAY†.

**Pool ladder L2/L1 (wall):** 4K×d8 read **1.94 MATERIAL** (both fs),
4K×d1 read 1.01 GRAY, 2M reads 2.6–2.8 GRAY† (instruction layer inside
the parity band — the latency observation from §4), writes 0.98–1.11 GRAY,
tmpfs 4K×d8 write 1.55 MATERIAL, tmpfs 64K×d2 read 2.32 MATERIAL.

**Envelope answer:** the residual abstraction tax is **NOT stable across
Host-0 regimes — it is confined to the control-plane-dominated 4 KiB
regime** and no MATERIAL tax is detected at 64 KiB and above on either
layer. Those large cells are formally GRAY under the frozen composite
rule, not PARITY (several carry direction anomalies; the clean PARITY
witness is 2 MiB × d1 READ — CHUNK-E0's sweet region). Depth moves the
4K wall tax (d1 1.61 vs d8 1.39 read); operation asymmetry (read
visible in wall, write masked by writeback) is recorded rather than
averaged away.

**Mechanism comparison (NOT abstraction tax):** Sluice-Uring (Z2) vs
Sluice-ThreadPool (L2), btrfs: uring wins every tested regime — S read
3 286 vs 1 236 MiB/s, L read 6 094 vs 5 798 MiB/s, L write 2 543 vs
2 404 MiB/s (`plots/threadpool-vs-uring-regimes.svg`). This answers the
historical "0.86 GiB/s ThreadPool vs 2 GiB/s direct uring" question:
the gap was mechanism + geometry, not the abstraction boundary alone.

**Z-zone classification (#227 vocabulary, Host-0, this campaign):**
4 KiB control-dominated cells → Z2 PARITY-BREADTH-REQUIRED (parity not
achieved on instructions; material tax measured). 64 KiB–2 MiB cells →
border of Z2/Z3: no material tax detected in the tested cells, but
formally mostly GRAY rather than PARITY (one clean PARITY witness,
2 MiB × d1 read); no Z4 value claims are made here (safety/control
value is out of this campaign's scope).

---

## 7. HOST-0 G1-PERFORMANCE ADJUDICATION

```text
G1-PERFORMANCE (HOST-0): PARTIAL
```

> On Host-0, the Sluice io_uring boundary carries a preregistered
> MATERIAL instruction-layer tax — backend ≈ 2.8–5.1× the
> semantic-equivalent floor — in the 4 KiB control-plane-dominated
> regime (frozen composite MATERIAL verdicts; wall separation confirmed
> on read cells). At 64 KiB–2 MiB no MATERIAL backend tax is detected
> in any tested cell, but those cells are formally GRAY under the
> frozen composite rule rather than proven parity — the clean PARITY
> witness is 2 MiB × d1 READ — so the defensible claim is "no material
> tax detected in the tested cells", not "broad parity". The runtime
> layer's frozen T_runtime verdicts are GRAY in every cell: a large
> 4 KiB instruction overhead (≈ 2.8–4.0× the hand-written
> continuation) is observed, but the wall layer never meets the frozen
> MATERIAL gate. Attribution: the census is consistent with a
> distributed Sluice implementation cost of the semantic boundary
> (§5.2); the one preregistered causal ablation (RE-1U-ATTR-B) then
> found the census-ranked family causally immaterial at release
> optimization — recovery 0.6 % of the residual, NO_RECOVERY. The
> distributed interpretation therefore remains consistent with the
> evidence but is NOT causally established: no release-level causal
> hotspot has been identified, the `-O3` hotspot structure remains
> unresolved, and no optimization is authorized.

Scope-bounded: Host-0, tested cells, buffered ordinary I/O, no
SQPOLL/registered/fixed features, workers=1 (uring) / W=d (pool).

```text
GENERAL G1-PERFORMANCE: NOT YET ADJUDICABLE
reason: second machine class unavailable; modern NVMe unavailable;
        ARM64 unavailable
#270  : OPEN — DEFERRED UNTIL HOST AVAILABLE (NOT EXECUTED; not a
        failure of this campaign)
```

---

## 8. Adversarial self-review (prereg P14)

- Z1 fewer semantics than Z1b called "Sluice tax"? **No** — decomposition
  frozen at Z1b; every table keeps Z1 separate (§3.3, §5).
- Z1bw/Z3 continuation obligations comparable? **Comparable rep-envelope
  (one wait per op, one admission per rep), not identical substrates** —
  declared in AUDIT §3.1 and attached to every T_runtime reading.
- Z3 span including admission/setup absent in comparator? Z3 pays one
  task admission + in-task `comp(D)` construction per rep; Z1bw pays
  one thread create/join per rep — both declared, neither tuned.
- Different queue depth / buffers / validation across arms? **No** —
  fail-closed same-work witnesses per combo (word_sum, byte counts,
  outstanding_max ≤ depth, stale_dropped == 0, write byte-verify).
- tmpfs leaked into real-storage claims? **No** — tmpfs is labeled
  control everywhere; no claim mixes regimes.
- 2M×d2 treated as universal? **No** — "sweet-region representative of
  this host" only; RE-2 exists precisely to check breadth.
- Unexpected ECANCELED retried away? **None occurred** (gate + formal,
  160 qualification + 720 formal launches at this commit; retry logic
  does not exist in the runner).
- Stale evidence reused across changed code paths? **No** — everything
  re-measured on `7653fd8d`; only #274-closed router slope kept closed.
- Cells added after results? **No** — matrix frozen in `a0281a93`;
  the mid cell was explicitly *not* added despite post-hoc temptation.
- OLS slope called "statistically zero"? **No such claim made**; only
  frozen-rule verdicts (PARITY/MATERIAL/GRAY) are used.
- Host-0 converted into Linux/x86 claims? **No** — HOST-LOCAL vocabulary
  throughout; GENERAL declared NOT YET ADJUDICABLE.
- Missing second host treated as success? **No** — recorded as an
  external-validity limitation, not a pass.
- Formal verdicts (PARITY/MATERIAL/GRAY) replaced by summary prose?
  **Caught and corrected** — the frozen composite verdict tables are
  the only authority; "parity" is claimed only where the rule returned
  PARITY (2 MiB × d1 read) and GRAY is never folded into parity (§0,
  §3.3, §6, §7).
- Census shares promoted to release-binary shares? **Caught and
  corrected** — the symbolized census ran on a `-O0` symbolization
  build (`releasedbg` carries no optimization flag in this project;
  outlined `size()` bodies contain an `idivq` that does not exist at
  `-O3`); shares rank families and confirm per-op paths only (§5.1).
- Implementation overhead attributed to the "required semantics"?
  **Caught and corrected** — Z1b prices the semantic capability; the
  Z1b→Z2 residual is Sluice's implementation cost by construction of
  the frozen decomposition (§5.2).
- Causal language used before the preregistered ablation ran?
  **Caught and corrected** — attribution was downgraded to
  "census-consistent", and RE-1U-ATTR-B — the campaign's only causal
  test — falsified the census-ranked hypothesis rather than completing
  attribution (§5.3).
- Launch accounting? Formal 720 (RE-1U 200 + RE-1 120 + RE-2U 200 +
  RE-2P 200) + qualification 160 = 880 at this commit; ATTR-B adds 30
  (prereg A8).
- ATTR-B run after its freeze? **Yes** — prereg + analysis authority +
  F07 seam committed at `2e247aee`; the session measured AT that head
  (tracked tree clean; environment `dirty` flag = untracked build
  dirs). Thresholds, arms, cell and outcome mapping were not touched
  after any ATTR-B number existed.
- ATTR-B pilot data before the freeze? **No** — only instrument probes
  on unchanged R0 behavior (the `-O0`/`-O3` contrast and the `-O3`
  machine-code audit) informed the disclosure, never the thresholds;
  no R0-vs-R1 comparison was run before the freeze.
- ATTR-B R0-arm comparability? **Declared** — the seam build carries
  the guarded TAX-0U lookup diagnostics in BOTH arms (subtracted out
  by the A/B); its absolute z2 level is ≈ +6.6 % above the production
  binary and is never cited as a production number; Z1b reproduces the
  formal floor exactly (1 097 = 1 097).
- tmpfs control treated as a verdict? **No** — the frozen fail-closed
  rule refused classification on a noise-level negative estimate
  (−12 instr/op); declared as refused-by-rule, never reinterpreted
  (§5.3).

---

## 9. Evidence index

| artifact | id |
| --- | --- |
| #262 qualification (160 launches, gates.json) | `results/re-h0-qual262-20260903-034305` |
| RE-1U formal (200 launches) | `results/re-h0-re1u-20260903-034519` |
| RE-1 formal (120 launches) | `results/re-h0-re1-20260903-040153` |
| RE-2 uring (200 launches) | `results/re-h0-re2u-20260903-041016` |
| RE-2 pool (200 launches) | `results/re-h0-re2p-20260903-042705` |
| ATTR-B ablation (30 launches) | `results/re-h0-attrb-20260903-102706` |
| attribution census | `results/re-h0-attrib-census/` |
| analysis diagnostics | `python3 scripts/check_re_h0_analysis.py` → 38/38 |
| analysis authority | `analysis.json` per session (machine-derived) |
| reproducibility witness | rebuilt formal binary SHA == recorded `7401213f…` (re-verified after the F07 seam: unchanged) |

Launch accounting: 160 qualification + 720 formal + 30 ATTR-B
corrective = **910 campaign launches**. Raw evidence is immutable;
every number in this report traces to `summary.json`/`analysis.json`
of the named sessions. Known limitations carried forward: wall on
writeback-saturated write cells cannot resolve instr-layer taxes
(frozen GRAYs, not PARITY); context switches and z-arm RSS not
collected (declared); census per-symbol shares are `-O0`
symbolization-build facts (§5.1); large-shallow pool read latency
anomaly (2.5–2.8× wall with the instruction layer inside the parity
band) is observed and unexplained — a bounded follow-up question, not
a claim; causal attribution of the 4 KiB residual remains incomplete
(§5.3 outcome B).
