# R9 — Cache-locality falsification results

Provenance: master `46157fd1`; research branch at this commit; binaries and
SHA-256s in `environment.md` (R9 addendum). Codegen identity re-proven on
**both** toolchains (worktree TUs with no research defines are byte-identical
to pristine master: WSL gcc 15.2.0 — transport `17f7899c…`, completion
`fc4b0d19…`; E5 gcc 16.2.1 — transport `3e107900…`, completion `0b07a38d…`).
Raw data: `results/cache-wsl.jsonl`, `results/cache-e5.jsonl` (every timed
row carries the full byte-pattern oracle result: **0 failed runs on either
host**), reductions `results/cache-{wsl,e5}-reduced.json`, PMU artifacts
`results/pmu/` (WSL) and `results/pmu-e5/`. Every interval below is a
percentile bootstrap over 15 AB/BA pairs (B=10000, seed 7) and describes
these pairs on this host and session.

## 1. Probe invariants — counts are structural, pressure-invariant

Across the full matrix (pipeline C=D ∈ {16,64,128,256,512} × ws ∈
{0, 64 KiB, 512 KiB, 4 MiB, 16 MiB}; external D=64 single/split,
submitter/driver-side; 2 reps per cell; both hosts; 10 500–22 000 measured
ops per cell):

- `lookups/install` = **1.00** in every cell.
- **Zero misses** in every cell (all sites), including 16 MiB interference
  at D=512.
- avg probes/op: **8.4–8.6 @D16, 32.0–33.4 @D64, 60.4–68.8 @D128,
  123.6–135.2 @D256, 240.9–266.2 @D512** — ≈ D/2, with the spread between
  working-set sizes inside the rep-to-rep spread. Hosts agree (widest cell
  16 µ-probes apart).
- avg live depth tracks D as configured; execution shape (single/split,
  submitter/driver interference) changes nothing.

Cache pressure cannot change *how many* probes run — only what each probe
costs. Everything below measures that cost.

## 2. End-to-end paired timing — no locality signature survives netting

Ratios interfered/warm, wall and net_wall (net = wall − the separately
metered sweep seconds; the sweep is ~1.5–2.5% of wall, pure user time,
identical construction across conditions). A/A rows bound run-order/path
bias with byte-identical binaries.

E5 native (A/A wall floors: 1.0011 [0.995, 1.008] @D64, 1.0033 [0.987,
1.008] @D64+16M, 1.0065 [0.967, 1.022] @D512, 1.0077 [0.999, 1.018]
external-split):

| Shape (net_wall, E5) | median | 95% interval | | wall | median [95%] |
|---|---:|---:|---|---:|---:|
| D16 ws16M | 1.0072 | [1.000, 1.023] | | | 1.0269 [1.020, 1.043] |
| D64 ws64K | **1.0205** | [1.014, 1.024] | | | 1.0399 [1.034, 1.044] |
| D64 ws4M | 1.0131 | [1.007, 1.020] | | | 1.0325 [1.027, 1.040] |
| D64 ws16M | 1.0054 | [1.001, 1.018] | | | 1.0255 [1.021, 1.037] |
| D128 ws16M | 1.0061 | [1.005, 1.022] | | | 1.0251 [1.024, 1.042] |
| D256 ws16M | 1.0129 | [1.003, 1.032] | | | 1.0298 [1.022, 1.050] |
| D512 ws16M | **0.9627** | [0.955, 0.970] | | | 0.9774 [0.968, 0.984] |

WSL2 (mechanism replication; A/A net floors: 0.9929 [0.935, 1.035] @D64,
1.0112 [0.960, 1.039] @D64+16M, 1.0011 [0.975, 1.059] @D512, 1.0044
[0.994, 1.033] external): net_wall medians 0.988 @D16, 1.008 @D64-16M,
1.025 @D64-4M, **1.047 @D64-64K**, 1.012 @D128, 1.019 @D256, 1.020 @D512 —
every interval overlaps its A/A band; WSL user-time intervals remain
±20–50% wide (Hyper-V accounting, as in R6) and carry no claim.

**Why this is not a router-locality signature.** A cache-locality mechanism
predicts the effect grows with the pressure that evicts the router (ws
ordering 16M ≥ 4M ≥ 64K once past L1) and with the number of lines each
lookup touches (monotone in D, ~⌈D/4⌉ lines). The data violate both
predictions on both hosts: the largest net effect sits at the *smallest*
working set (64 KiB: +2.1% E5 / +4.7% WSL @D64) which cannot evict the
router from L2 at all, and the effect *inverts* at D=512 (E5 −3.7% net,
[0.955, 0.970] — the interfered runs are genuinely faster: sys_s ratio
0.974, user_s 0.972 at that shape). The inversion's *mechanism* — that
pacing the submitter lets the ring reap deeper batches — is a
**hypothesis**, not a measurement: no direct batch-size counter exists in
this round (the recomputed ratios are the evidence; the batch explanation
is consistent with the sys/user reductions but was not counted). What the
inversion proves regardless of mechanism is that the interference effect
is *anti*-correlated with D at the top of the sweep, which no
locality mechanism predicts.
The remaining +0.5–2% net effects are D-flat within noise — a dispatch/pacing
cost of interleaving CPU work with the I/O loop, not a cost of the router
scan, whose absolute size (below, ≤ 0.3 µs/op) is too small to produce them.

External forms, E5 wall: single-core +2.9% [1.019, 1.038], split-core
submitter-side +2.75% [1.019, 1.032] (A/A 1.0077 [0.999, 1.018]) — same
D-flat pacing effect; driver-side interference +6.96% [1.060, 1.084]:
running the sweep *on the progress-owning thread* delays `poll_progress`
passes directly, which is exactly where a dispatch-pacing explanation
predicts the largest effect and a locality explanation does not (the router
lines touched are the same D/2 as the submitter-side cell). Caveat: the
external ratios are gross wall — in the two-thread shape the sweep can
overlap I/O wait, so unlike the pipeline rows they are not netted, and each
carries the sweep's own ~1.4–2.0% of wall inside the ratio; the
submitter-side cells' gross +2.8–2.9% are therefore pacing-plus-sweep, and
the driver-side +7.0% minus its sweep share is still ≈5 points of pacing
delay.

## 3. Forced-eviction bound (clflush) — decomposed by the delta review

`cache_ab_flush` evicts the whole router array before **every** cookie
lookup. The original R9 text reported this arm's wall ratio (+26% D64 /
+108% D512 on E5) as "the cold regime". The delta review added a capacity
sweep (`results/cache-flush-sweep-{wsl,e5}.jsonl`, 15 AB/BA pairs per cell)
that separates the two costs inside the arm, because they scale with
different variables: the `clflush` instructions scale with C (the flushed
span), the cold refill scales with the probed live span (~D/2 entries):

| Cell (net/op Δ, flush − warm) | WSL | E5 |
|---|---:|---:|
| D64 C64 (32 flush ops, ~16 probed lines) | +0.416 µs | +1.347 µs |
| D64 C512 (256 flush ops, same ~16 lines) | +0.799 µs | +8.303 µs |
| D512 C512 (256 flush ops, ~128 lines) | +2.312 µs | +8.250 µs |
| D512 C1024 (512 flush ops, same ~128 lines) | +2.420 µs | +15.910 µs |

The C-slope isolates the instruction cost: **E5 ≈ 30 ns/clflush op**
(consistent at both depths: 6.96/224 and 7.66/256 µs), **WSL ≈ 1.7 ns/op**
(Zen 3; noisier, the c512→c1024 cell gives 0.4 ns/op). Subtracting it:

- **E5**: the +108% arm cost at D512/C512 is ≈ 7.66 µs clflush mechanics +
  **≈ 0.59 µs genuine cold-refill** per op — i.e. the published +108% was
  ~13× overstatement of the actual cold-scan penalty, which is ≈ **+8% of
  net wall**. At D64/C64: ≈ 1.0 µs mechanics + ≈ 0.35 µs refill.
- **WSL**: clflush is nearly free, so its arm costs were mostly refill all
  along: **≈ 1.9–2.2 µs/op at D512 (≈ +29%), ≈ 0.35 µs at D64**.

What the decomposition establishes, and what it does not:

1. The scan's true cold penalty per lookup is single-digit percent on E5
   and ≈29% on WSL at D=512 — material at the research extreme, and an
   order of magnitude below the original +108% narrative. The dominant
   reason is structural: the reverse scan walks a **contiguous live span**,
   so hardware prefetch streams even DRAM refills. A structure with random
   per-lookup line touches would prefetch nothing — but no such structure
   was built or measured in this round, so **no claim is made here about
   whether an index would be faster or slower in the cold regime**; its
   insert/erase maintenance and its own cache behavior are unmeasured.
2. The regime remains synthetic either way: no measured interleaved working
   set in the matrix evicts L2+ between consecutive lookups (§2, §4), and
   no standard W-02/W-03 form runs D > 64. The corrected numbers are
   recorded as the falsifiable boundary of the negative verdict — not as a
   trigger.

## 4. PMU attribution (R9-C)

`find_live_router_cookie_` self-share (`perf record cycles:u -F 1999`,
`-m 64` on E5; WSL resolved via nm; shares read together with absolute
cycles/op from `perf stat` so a share change is never mistaken for a
denominator change):

| Cell | WSL share | WSL cyc/op (u) | ≈cyc/probe | E5 share | E5 cyc/op (u) | ≈cyc/probe |
|---|---:|---:|---:|---:|---:|---:|
| D64 warm | 2.56% | 6 462 | 5.1 | 2.03% | 7 529 | 4.8 |
| D64 ws16M | 1.73% | 7 094 | 3.8 | 1.77% | 8 655 | 4.8 |
| D512 warm | **5.07%** | 11 829 | 2.4 | **5.80%** | 14 636 | 3.3 |
| D512 ws16M | 4.43% | 12 355 | 2.2 | 5.65% | 14 016 | 3.1 |

Source discipline (delta review): the E5 columns now pair share and
cycles/op **from the same `perf record` run** (event count ÷ 42 000 ops ×
share); the original table had paired `perf stat` totals with record
shares — defensible within a few percent, but same-source products are
exact. WSL columns pair its `perf stat` totals (100% duty) with
nm-resolved record shares as before.

Delta-review additions to this table, both from 200 000-op records
(`results/pmu/report-long-*-wsl.txt`, `results/pmu-e5/report-*long*` and
`report-d512-warm-100k/-rep2/-longwarm-e5.txt`, 1–3 K samples each):

- **WSL long runs confirm the short values** (D64 warm 1.88%, D512 warm
  5.01%, D512 ws16M 4.76%) — the WSL share is stable in run length.
- **E5 long runs do not**: D512 warm rises monotonically with total ops
  (5.80% @42 K → 6.97% @80 K → 8.13% @102 K → 9.77/9.81% @202 K) while
  total cycles/op stays constant (14 636 → 14 918) and probe counts stay
  254–257 with zero misses (`cache_probe` at 20 K/100 K/200 K ops). The
  cells also ran at different effective clocks (user-time ÷ cycles implies
  ≈3.40 GHz short vs ≈3.22 GHz sustained — turbo decay), and single-run
  walls at this precision are ±5% noisy. The PMU-independent medians
  (15-rep paired wall, probe counters) show **no** cost growth. The E5
  perf-record share for D512 is therefore **condition-dependent in the
  5.8–9.8% band**, and no single point value from it is authoritative; the
  cycles/probe entries above for E5 carry that uncertainty (3.3–5.7 across
  cells). The original 670–705-sample record runs additionally carried
  ±1.1–1.7 pp binomial intervals on every share.

(E5 stat duty 50% on cycles due to 4-event multiplexing; `perf stat -x,`
scales multiplexed counts to full duty — verified against the wall/user
clock: cycles:u ÷ (user_s × 2.90 GHz) = 0.98 on the d64-warm cell — so the
cycles/op column is absolute within a few percent. WSL ran at 100% duty.)

- **Per-probe cost is ~3–5 cycles warm and does not grow under 16 MiB
  interference** — on either host, at either depth. The audit's finding 3 is
  resolved: 1–2 cyc/probe was a lower bound; the real warm cost is 3–5, and
  it is already absorbed in every R2–R6 measured share. The ±40% sampling
  noise cannot hide a material growth, because two independent lines of
  evidence agree: the symbol-share product above (attributed cycles/op fell
  or held on every interference cell), and the end-to-end net-wall overhead,
  which at D=64 ws16M is +0.5% ≈ +36 cycles/op of *all* non-sweep overhead.
  The net-wall figure bounds lookup growth at ≈ +1 cycle/probe **modulo
  offsetting savings** — the D=512 inversion shows interference can also
  *save* cycles (batching), in which case net-wall would understate a
  simultaneous lookup growth; that residual loophole is why the
  symbol-share line is quoted alongside rather than the wall figure alone.
- **The share scales with D** (2.0–2.6% @D64 → 5.1–5.8% @D512 short-run,
  both hosts; E5 long-run up to 9.8%, see the instability note above —
  honest band at D512: **≈5–10%**, harness-inclusive). At D=512 — a depth
  no standard shape
  uses (W-02/W-03 run D ≤ 64; the R2 worst artificial shape was C1024/D64
  at 1.6–2.3%) — the warm share sits at or above the 5% materiality line.
  In absolute terms that share is ≈ 590–1 450 cycles/op ≈ 0.21–0.50 µs/op
  out of 5.1–7.6 µs/op net, at ⌈D/4⌉ ≈ 128 lines per lookup — exactly the
  arithmetic the audit corrected, measured, not narrated. Scope note added
  to `decision.md`.
- L1-dcache-load-misses (stat, E5): 6.50M warm → 7.03M @16M (D64) and
  43.2M → 44.4M (D512) — the delta is the sweep's own misses and buffer
  refetch, not separable to the router by totals; the symbol-level shares
  above are the router attribution. LLC on E5: LLC-load-misses **1 239
  warm vs 2 502 @16M** for the whole 42 K-op run (≈0.03–0.06/op) — even the
  16 MiB sweep stays inside the 25 MiB L3 on that host. **WSL LLC events:
  `<not supported>` → CACHE_ATTRIBUTION_UNAVAILABLE (partial)**; L1/branch/
  generic cache events were available and are archived.
- Stalled-cycle attribution was not attempted: no credible per-slot event
  mapping on the WSL virtual PMU, and the question (does the scan stall
  under pressure) is answered directly by the unchanged attributed cycles.

## 5. Decision gate (R9-D)

The four reopen conditions, evaluated (delta-review revision):

1. **Router-lookup hotspot in a representative, repeatable workload?** No.
   Under every interleaved working set at every depth and both execution
   shapes, the lookup's attributed cycles are flat and its end-to-end net
   effect is within the A/A-adjacent band (D ≤ 256) or *negative* (D=512).
   The separable wall effects are D-flat or D-anticorrelated and ws-
   non-monotone — pacing-shaped, not locality-shaped (mechanism a
   hypothesis; the shape evidence is the measurement).
2. **Locality distinguishable from harness/scheduler/kernel noise?** Yes —
   that is exactly what the A/A floors, the netting of the separately
   metered sweep, and the D/ws monotonicity predictions did — and the
   locality component measured zero.
3. **Cost above the #580 materiality threshold?** At the *mechanism* level
   only: D=512 warm sits at ≈5% (WSL, stable) to 5.8–9.8% (E5,
   condition-dependent) harness-inclusive share ≈ 0.21–0.50 µs/op — a
   **measured mechanism hotspot at a non-representative shape**. It is not
   a *representative application trigger* (no standard form runs D > 64;
   #578 has produced no real-app baseline), not an *established avoidable
   cost* (no alternative structure was measured to beat it — the delta
   review further showed the scan's cold penalty is prefetch-amortized to
   +8%/+29% wall, and an index's own cold/maintenance costs are unknown),
   and not *admissible product evidence*. Under cache pressure, at standard
   shapes, everywhere: below threshold.
4. **A concrete application form making this working set realistic?**
   None exists (#578 still open, no baseline). The forced-eviction regime
   is not produced by any measured interleaving and has no applicant.

**`NO_TRIGGER_NO_CHANGE` is retained**, now explicitly scoped to: D ≤ 512,
interleaved independent working sets ≤ 16 MiB at ≤ 4 KiB/op, the W-02
single-owner and W-03 replica (single- and split-core) execution forms, on
the WSL Zen 3 and E5 Haswell hosts. Revisit conditions (sharpened by the
delta review): (a) a #578 real-app shape with sustained live depth in the
hundreds, where the ≈5–10% share band stops being synthetic; (b) any
measured workload that actually evicts L2+ between consecutive lookups
(the forced-eviction experiments bound that regime at +8%/+29% wall —
material, but produced only synthetically); (c) a *measured* index
prototype beating the scan in that regime, which nothing in this campaign
provides. Condition (c) is required because "an index would win when cold"
was shown by the decomposition to be unproven — the scan is sequential and
prefetch-amortized, and no index arm was ever built.
