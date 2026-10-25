# R9-A — Cache-locality evidence audit (pre-experiment)

Audit of the R0–R8 evidence chain against the arithmetic it rests on, before
the R9 cache-sensitivity experiment ran. Question: does `NO_TRIGGER_NO_CHANGE`
depend on warm-cache assumptions the documents state too strongly? Five claims
were checked in `trigger.md`, `lifecycle-cost.md`, `measurement-protocol.md`,
`wsl-results.md` and `decision.md`.

## 1. "1–2 hot cache lines" vs the 32 B / 64 B arithmetic — **overstated**

`decision.md` Q1 says the reverse scan "finds the entry in ≈ D/2 probes on
1-2 hot cache lines regardless of configured capacity"; `lifecycle-cost.md`
says "2-3 hot lines". Arithmetic: RouterEntry is 32 B (established in R1
disassembly and Review A), a cache line is 64 B, so two entries pack per line.
An average D/2-probe reverse scan touches ⌈D/2⌉ distinct entries =
**⌈D/4⌉ distinct lines** (D=16 → 4 lines, D=64 → 16 lines), and the live
span occupies **D/2 lines** (D=64 → 32 lines = 2 KiB). "1–2 hot lines" is
true only for D ≤ 4–6 — smaller than every measured shape. At the exact shape
where the 2.28% E5 share was measured (D=64), an average lookup touches ~16
lines.

Two residual errors were also found: `lifecycle-cost.md`'s memory row still
said "24 B × C (router)" (Review A corrected the entry size to 32 B in
`source-dag.md` but this row was missed), and the same document's break-even
text reused the "2-3 hot lines" phrase.

**Materiality of the error**: narrative only. The perf shares are direct
measurements that already embed the true line counts under warm conditions;
no measured number changes. The wording overstated how compact the scan's
footprint is at the measured depths, which is precisely the claim R9 tests
under pressure. Both documents are corrected in this revision; the R9
experiment supersedes the phrase entirely with measured per-probe costs.

## 2. D/2 — observed workload average, not a bound over completion orders

The documents report avg probes ≈ D/2 as a *measured* fact (correct) and
claim the hot path is "O(live), never O(capacity)" (correct structural
bound). What was not stated: the per-lookup probe count is position-dependent
in [1, D] — the reverse scan finds the newest-allocated entry first (LIFO
free-list puts live entries at high indices, newest highest), so the measured
D/2 average is the expectation under a roughly **uniformly distributed
target position**, which these workloads produce via out-of-order kernel
completions. A strictly FIFO-completing workload presents the oldest target
at the deepest live position and would average ~D probes/op — 2× the
reported value; strictly LIFO completion averages ~1. D/2 is therefore an
observed average for the measured completion orders, with [1, D] the
structural range.

**Robustness if doubled**: the worst R6 shape doubles from 1.69% WSL /
2.28% E5 to ~3.4% / ~4.6% of harness-inclusive user cycles — still below
5%; total-CPU view stays <1%. The R9 D-sweep (below) extends this to D=512,
where the ordering caveat becomes load-bearing and is reported explicitly.

## 3. "1–2 cycles/probe" — an unverified warm-cache estimate, and the
   measured value is higher

`measurement-protocol.md` presents 1–2 cycles/probe as a bottom-up
cross-check that "agrees with the perf shares to within a small factor."
Dividing the attributed cycles by the measured probes gives what the
documents never computed directly:

| Cell | share | cycles/op (u) | probes/op | cyc/probe attributed |
|---|---:|---:|---:|---:|
| WSL C64/D64 (R2) | 1.57% | 7,136 | 33.7 | **3.3** |
| WSL C=D=64 (R9) | 2.56% | 6,462 | 32.2 | **5.1** |
| WSL C=D=512 (R9) | 5.07% | 11,829 | 254 | **2.4** |

So the attributed cost is ~2–5 cycles/probe warm, 1.2–5× the bottom-up
figure, with sampling noise of roughly ±40% at these sample counts. The
1–2 cycles/probe claim is an L1-resident lower bound, not hardware evidence;
no cache-miss counts were taken in R2. R9 measures per-probe cost warm and
under pressure directly (PMU + clflush bound) and retires the estimate.

## 4. The 2.28% share — harness-inclusive denominator, disclosed but not
   quantified

Both `trigger.md` and `wsl-results.md` disclose that shares use a
harness-inclusive denominator (the runner's wait/verify spin is 65–68% of
user cycles, identical across arms). Not quantified was the sensitivity: if
that spin were excluded from the denominator, the same lookup cycles would
be ~2.8–3.1× the relative share (2.28% → ~6.5%, 1.57% → ~4.5%), i.e. the
worst shape crosses the 5% line under a minimal-denominator counterfactual.
That counterfactual is ill-posed — the spin is the app-facing drive cost and
a real application's denominator includes its own logic — but the audit
records that the "below threshold" conclusion is denominator-conditional,
and R9 therefore anchors the decision on absolute attributed cycles/op, the
flush bound, and end-to-end A/B against A/A floors rather than share
arithmetic alone. The WSL `:u` hypervisor-sample contamination (16.2%,
disclosed and excluded in R6) applies unchanged.

## 5. Candidate A/B maintenance costs — lower-bound estimates that some
   summary lines state as settled

`lifecycle-cost.md`'s header correctly labels the Candidate A/B columns
"hypothetical (gate-blocked, not built) … analytic lower bounds … not
measurements." Two later statements exceed that label: "Net at every
measured shape: zero to negative" and `decision.md` Q2's "the relocation
costs more than it saves at every supported configuration." Classification
after audit: the *frequencies* (Q = 1.00 lookups/op, U = 1 insert + 1
erase/op, M ≈ 0) are measured; the *unit costs* (2 random cache-line writes
≈ 30–80 cycles, tombstone pressure, cold-table misses at large C) are
estimates; the net-negative conclusion is a lower-bound argument that holds
only while the saved lookup cost stays within the measured attribution.

The R9 flush experiment closes exactly this hole: it constructs the regime
where the saved cost would exceed anything R2–R6 measured (every lookup
fully cache-cold) and measures whether, and at what D, the scan's cost
becomes material there. That regime is synthetic; the interference matrix
tests whether any realistic interleaved working set reaches it. The revised
texts re-label the Candidate A/B net as a lower-bound argument and add the
measured boundary.

## Audit verdict

All five claims contain wording stronger than their evidence, but none
changes a measured number, and none, when corrected, moves the R0–R8 gate
by itself: the corrected arithmetic (16–32 lines at D=64, 3–5 cyc/probe,
denominator sensitivity) makes the lookup *somewhat larger* than narrated
while remaining inside every materiality bound at the standard shapes. The
R9 experiment was therefore run as a falsification of the corrected, weaker
claim set — not as a re-derivation of the old one.
