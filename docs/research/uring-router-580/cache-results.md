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
[0.955, 0.970] — the interfered runs are genuinely faster: pacing the
submitter between completions lets the ring batch deeper; sys_s ratio 0.986,
user_s 0.981 at that shape, consistent with fewer, larger reaps). The
remaining +0.5–2% net effects are D-flat within noise — a dispatch/batching
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

## 3. Forced-eviction bound (clflush) — the synthetic cold regime

`cache_ab_flush` evicts the whole router array before **every** cookie
lookup (mechanism sensitivity only; includes the `clflush` instruction cost
itself, so it overstates even the fully-cold cost):

| | E5 net/op | ratio [95%] | WSL net/op | ratio [95%] |
|---|---:|---:|---:|---:|
| D64 warm | 5.077 µs | | 5.821 µs | |
| D64 flush | 6.424 µs | **1.2623 [1.245, 1.269]** | 6.237 µs | **1.0835 [1.041, 1.129]** |
| D512 warm | 7.625 µs | | 7.458 µs | |
| D512 flush | 15.875 µs | **2.0772 [2.063, 2.107]** | 9.770 µs | **1.2963 [1.272, 1.350]** |

This is the regime `lifecycle-cost.md` Q1 named: sustained live depth in
the hundreds **and** every lookup cache-cold. There the scan is genuinely
material (E5: +108% wall at D=512), and an exact index touching 1–2 lines
would win. No realistic interleaved working set in this round's matrix came
within an order of magnitude of it — the 16 MiB sweep leaves the lookup's
attributed cycles unchanged (next section) because L2 (and on E5 even L3)
absorbs the eviction between one lookup and the next. The bound is recorded
as the falsifiable boundary of the negative verdict, not as a trigger: no
measured application shape, and none of the standard W-02/W-03 forms
(D ≤ 64), reach it.

## 4. PMU attribution (R9-C)

`find_live_router_cookie_` self-share (`perf record cycles:u -F 1999`,
`-m 64` on E5; WSL resolved via nm; shares read together with absolute
cycles/op from `perf stat` so a share change is never mistaken for a
denominator change):

| Cell | WSL share | WSL cyc/op (u) | ≈cyc/probe | E5 share | E5 cyc/op (u) | ≈cyc/probe |
|---|---:|---:|---:|---:|---:|---:|
| D64 warm | 2.56% | 6 462 | 5.1 | 2.03% | 7 476 | 4.7 |
| D64 ws16M | 1.73% | 7 094 | 3.8 | 1.77% | 8 076 | 4.4 |
| D512 warm | **5.07%** | 11 829 | 2.4 | **5.80%** | 14 902 | 3.4 |
| D512 ws16M | 4.43% | 12 355 | 2.2 | 5.65% | 14 081 | 3.1 |

(E5 stat duty 50% on cycles due to 4-event multiplexing; `perf stat -x,`
scales multiplexed counts to full duty — verified against the wall/user
clock: cycles:u ÷ (user_s × 2.90 GHz) = 0.98 on the d64-warm cell — so the
cycles/op column is absolute within a few percent. WSL ran at 100% duty.
Sampling noise at these sample counts is ~±40% on cyc/probe.)

- **Per-probe cost is ~3–5 cycles warm and does not grow under 16 MiB
  interference** — on either host, at either depth. The audit's finding 3 is
  resolved: 1–2 cyc/probe was a lower bound; the real warm cost is 3–5, and
  it is already absorbed in every R2–R6 measured share. The ±40% sampling
  noise cannot hide a material growth, because two tighter bounds exist:
  the symbol-share product above (attributed cycles/op fell or held on
  every interference cell), and the end-to-end net-wall overhead, which at
  D=64 ws16M is +0.5% ≈ +36 cycles/op of *all* non-sweep overhead — capping
  any lookup growth under interference at ≈ +1 cycle/probe, an order of
  magnitude below the clflush bound.
- **The share scales with D** (2.0–2.6% @D64 → 5.1–5.8% @D512,
  harness-inclusive, both hosts). At D=512 — a capacity no standard shape
  uses (W-02/W-03 run D ≤ 64; the R2 worst artificial shape was C1024/D64
  at 1.6–2.3%) — the warm share exceeds the 5% materiality line. In
  absolute terms that share is ≈ 600–865 cycles/op ≈ 0.21–0.30 µs/op out of
  5.1–7.6 µs/op net, at ⌈D/4⌉ ≈ 128 lines per lookup — exactly the
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

The four reopen conditions, evaluated:

1. **Router-lookup hotspot in a representative, repeatable workload?** No.
   Under every interleaved working set at every depth and both execution
   shapes, the lookup's attributed cycles are flat and its end-to-end net
   effect is within the A/A-adjacent band (D ≤ 256) or *negative* (D=512).
   The separable wall effects are D-flat or D-anticorrelated and ws-
   non-monotone — dispatch pacing, not lookup locality.
2. **Locality distinguishable from harness/scheduler/kernel noise?** Yes —
   that is exactly what the A/A floors, the netting of the separately
   metered sweep, and the D/ws monotonicity predictions did — and the
   locality component measured zero.
3. **Cost above the #580 materiality threshold?** Only at D=512 warm
   (5.07%/5.80% harness-inclusive share ≈ 0.21–0.30 µs/op), a non-standard
   shape whose per-op absolute cost is still smaller than the run-to-run
   pacing effects; and it does not grow under cache pressure. Under
   pressure, at standard shapes, everywhere: below threshold.
4. **A concrete application form making this working set realistic?**
   None exists (#578 still open, no baseline). The only regime where the
   scan dominates — forced eviction per lookup at D ≥ ~256 — is not
   produced by any measured interleaving and has no applicant.

**`NO_TRIGGER_NO_CHANGE` is retained**, now explicitly scoped to: D ≤ 512,
interleaved independent working sets ≤ 16 MiB at ≤ 4 KiB/op, the W-02
single-owner and W-03 replica (single- and split-core) execution forms, on
the WSL Zen 3 and E5 Haswell hosts. The D=512 warm-share boundary and the
clflush cold regime are recorded as the two conditions under which the
question would deserve reopening (sustained D in the hundreds **plus** per-
lookup L2+ eviction or a denominator materially leaner than any measured
harness).
