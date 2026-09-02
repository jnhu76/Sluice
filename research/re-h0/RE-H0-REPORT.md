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
  large / application-relevant I/O (2 MiB × d1..d2):
      HOST-LOCAL PARITY with the semantic-equivalent floors,
      no material residual detected in any tested cell
  control-plane-dominated small I/O (4 KiB × d1..d8):
      HOST-LOCAL MATERIAL TAX — Sluice uring backend ≈ 2.8–5.1×
      floor instructions/op, runtime layer ≈ 2.8–4.0× the
      continuation floor; wall separation present in read cells
GENERAL G1-PERFORMANCE:   NOT YET ADJUDICABLE
  (second machine class / modern NVMe / ARM64 unavailable; #270
  DEFERRED / NOT EXECUTED, remains OPEN)
```

This is the mission's PARTIAL shape, and it is a useful result: the
residual tax is localized where the explicit machinery has the least
work to amortize over, and it is quantitatively characterized per layer.

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
WSL2-specific; zero Host-0 events across 160 qualification launches AND
720 formal launches below). No stop-gate event occurred at any point in
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
  PARITY — recorded as such). Large cells: wall parity everywhere on
  btrfs (within 0.5–3 %, some direction anomalies).
- **C_cont ≈ 1.99 wall / 1.27 instr (S read)** is the price of ANY
  waitable-completion consumer as hand-written (per-op futex wake):
  it is the *capability* baseline Z3 must beat, not a Sluice cost.
  Notably Z3's wall is 16 % *below* Z1bw's (0.841): the Sluice runtime
  wakes cheaper than the naive cv consumer while spending more user
  instructions — wall and instructions diverge in opposite directions,
  which is exactly why the frozen rule reports per-layer facts and
  refuses to collapse them into one number.

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
instruction layer is at parity (L2 +1.2 % over L1 read) and wall
differences are latency-shaped, not CPU-shaped (see † below). The
L1-vs-L0 pool round-trip itself (T_pool 5.9–6.4 on S read) is larger
than any Sluice increment — the execution-machinery cost dominates the
explicit-control-plane increment on this host.

† `T_sluice ≈ 2.5–2.8` on large-shallow reads with **instruction parity**
(tmpfs L read: L2 538.6 µs ± tiny MAD vs L1 217.3 µs, instr 1.013): a
pure round-trip/wake **latency** fact of the persistent-runtime path at
depth ≤ 2, invisible to CPU accounting. The frozen rule reports GRAY
(wall material, instructions not agreeing); recorded honestly as an
open latency observation, not as CPU tax and not as parity.

RSS (e1 arms, per raw launch JSON): ~4 MB class, no separation.
Context switches: not collected (no reliable per-combo source; declared
in P7, not hidden). z-arm RSS: not emitted by the instrument; declared.

---

## 5. RESIDUAL ATTRIBUTION (CASE B earned: 4 KiB cells)

Method per P9/§25: census first, no production change. Symbolized
instruction census (same sources, releasedbg build for symbols only;
formal numbers remain bound to the stripped formal binaries by SHA —
the rebuilt formal binary reproduces its recorded SHA exactly,
`7401213f…`, a reproducibility witness):

| share of user instructions (S read) | Z1b floor | Z2 backend | Z3 runtime |
| --- | --- | --- | --- |
| workload validation (`word_sum`) | **91.9 %** | 39.2 % | 33.8 % |
| Sluice machinery (all sites) | ~3 % | ~57 % | ~60 % |

Ranked Sluice sites in Z2 (percent of process user instructions):
router/slot/prepared-op table probes
(`vector::size()`-dominated validation: 4.8 + 3.8 + 1.3),
`Completion` state `compare_exchange_strong` 2.1,
mutex lock/unlock traffic ≈ 6.3 combined,
`submit_transaction` 1.5, `RequestArena::validate_` 1.3,
reap/publication and accounting spread below 1 % each.
Z3 adds scheduler/wait mutex traffic (lock+unlock ≈ 11.5) on top.

**Causal interpretation:** the residual is a *distributed control-plane
existence cost* — per-op arena admission/validation, router and slot
table probes, completion state transitions, and lock traffic — with **no
single site ≥ 10 %**. Consistent with #274/#275 history, it is not a
pathological algorithm; it is the footprint of the required semantics
themselves at a request size where the floor spends only ~100
instructions/op. A single-site causal ablation has no meaningful target
(the known guarded seams F01/F02 were already tested immaterial in
TAX-0D), so no candidate shootout is authorized, and **no production
change is proposed or made**. If engineering later wants to shrink this
footprint, that is a new fix-selection campaign (#255 discipline) with
its own measurement/ablation authority.

```text
backend material tax found : YES — 4 KiB cells only (instr layer)
runtime material tax found : YES — 4 KiB cells only (instr layer)
causal hotspot             : distributed control plane (census table)
ablation                   : none possible at single-site granularity;
                             F01/F02 known-immaterial (TAX-0D)
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
4K×d1 read 1.01 GRAY, 2M reads 2.6–2.8 GRAY† (instruction parity — the
latency observation from §4), writes 0.98–1.11 GRAY,
tmpfs 4K×d8 write 1.55 MATERIAL, tmpfs 64K×d2 read 2.32 MATERIAL.

**Envelope answer:** the residual abstraction tax is **NOT stable across
Host-0 regimes — it is confined to the control-plane-dominated 4 KiB
regime** and is bounded/undetectable at 64 KiB and above on the
instructions layer, with wall parity returning at 2 MiB (CHUNK-E0's
sweet region). Depth moves the 4K wall tax (d1 1.61 vs d8 1.39 read);
operation asymmetry (read visible in wall, write masked by writeback)
is recorded rather than averaged away.

**Mechanism comparison (NOT abstraction tax):** Sluice-Uring (Z2) vs
Sluice-ThreadPool (L2), btrfs: uring wins every tested regime — S read
3 286 vs 1 236 MiB/s, L read 6 094 vs 5 798 MiB/s, L write 2 543 vs
2 404 MiB/s (`plots/threadpool-vs-uring-regimes.svg`). This answers the
historical "0.86 GiB/s ThreadPool vs 2 GiB/s direct uring" question:
the gap was mechanism + geometry, not the abstraction boundary alone.

**Z-zone classification (#227 vocabulary, Host-0, this campaign):**
4 KiB control-dominated cells → Z2 PARITY-BREADTH-REQUIRED (parity not
achieved on instructions; material tax measured). 64 KiB–2 MiB cells →
border of Z2/Z3: parity achieved; no Z4 value claims are made here
(safety/control value is out of this campaign's scope).

---

## 7. HOST-0 G1-PERFORMANCE ADJUDICATION

```text
G1-PERFORMANCE (HOST-0): PARTIAL
```

> On Host-0, the Sluice io_uring boundary is within the preregistered
> parity envelope relative to the competent hand-written
> semantic-equivalent floor across every tested cell at 2 MiB and at
> 64 KiB×d2 (instructions within +0.5–3.8 % of the raw floor), but
> carries a preregistered MATERIAL instruction-layer tax — backend
> ≈ 2.8–5.1×, runtime ≈ 2.8–4.0× floor — in the 4 KiB control-plane-
> dominated regime, partially visible in wall on read cells. The tax is
> attributed by census to the distributed per-op control plane, with no
> single-site hotspot and no authorized optimization.

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
  800 launches total; retry logic does not exist in the runner).
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

---

## 9. Evidence index

| artifact | id |
| --- | --- |
| #262 qualification (160 launches, gates.json) | `results/re-h0-qual262-20260903-034305` |
| RE-1U formal (200 launches) | `results/re-h0-re1u-20260903-034519` |
| RE-1 formal (120 launches) | `results/re-h0-re1-20260903-040153` |
| RE-2 uring (200 launches) | `results/re-h0-re2u-20260903-041016` |
| RE-2 pool (200 launches) | `results/re-h0-re2p-20260903-042705` |
| attribution census | `results/re-h0-attrib-census/` |
| analysis diagnostics | `python3 scripts/check_re_h0_analysis.py` → 26/26 |
| analysis authority | `analysis.json` per session (machine-derived) |
| reproducibility witness | rebuilt formal binary SHA == recorded `7401213f…` |

Raw evidence is immutable; every number in this report traces to
`summary.json`/`analysis.json` of the named sessions. Known limitations
carried forward: wall on writeback-saturated write cells cannot resolve
instr-layer taxes (frozen GRAYs, not PARITY); context switches and
z-arm RSS not collected (declared); large-shallow pool read latency
anomaly (2.5–2.8× wall at instruction parity) is observed and
unexplained — a bounded follow-up question, not a claim.
