# Decision

## Verdict chain

| Level | Verdict | Basis |
|---|---|---|
| R2 trigger gate (index candidates A/B) | `NO_TRIGGER_NO_CHANGE` | 1 lookup/op; hot-path probes O(D/2) **independent of C** (8.5 @D16, 1.0 @D1); `find_live_router_cookie_` = 1.57-1.69% (WSL) / 2.28% (E5) of user cycles at the worst artificial shape, ~0.4-0.5% of total CPU; zero natural misses in 100k+ completions; theoretical maximum gain (free lookup) < 2% user cycles at every shape measured in R0–R8, below the 5% materiality threshold (R9 later measured 5.07–9.8% harness-inclusive at the non-standard D=512 warm shape — see the scope addendum; no R0–R8 shape approaches it); corroborated by #561's native ≤3.7% ceiling |
| R3 candidate admission | blocked at the gate; A/B recorded as design-level analysis only | protocol stop rule: hotspots confined to synthetic C≫D configurations do not admit product candidates; no #578 real-app evidence exists |
| WE-2 (scan-direction flip) mechanism | `EXPLORATORY_ONLY / NOT_ADMITTED` | mechanism verified (993→33 probes/cancel at C=1024), semantics fully preserved (23/23 oracle both arms; 20/20 + 21/21 + 8/8 suites; unkillable-by-construction equivalence argument strengthened by Review A), but the end-to-end effect is statistically indistinguishable from the A/A floor on E5 native (wall 0.995 [0.967, 1.021] on the target shape) |
| Full-domain identity | preserved everywhere | no cookie narrowing, no capacity change, exact 64-bit equality at every consumer; the codegen-identity proof shows production builds are byte-identical with the research patch present |
| Issue #580 | `NO_TRIGGER_NO_CHANGE` (final, this round) | with E5 native admitted mid-round by owner instruction, the negative trigger finding holds on the admission host class itself: no material avoidable cost exists to eliminate |

The internal `WSL_EXPLORATION_SUPPORTED_NATIVE_PENDING` state never applied:
the WSL round did not support a candidate, and the owner-authorized E5
campaign then confirmed the negative natively, so no pending-native step
remains. `NO_CHANGE_JUSTIFIED` describes the WE-2 sub-question specifically
(30× probe reduction buys nothing measurable end-to-end); the issue-level
verdict is `NO_TRIGGER_NO_CHANGE` because the premise — a material lookup
hotspot — was never witnessed at any admission-grade configuration.

## The three closing questions

1. **Is CQE router lookup a real problem worth solving in the Sluice path?**
   No. Every steady workload performs exactly one lookup per operation, it
   always hits, and the reverse scan aligned with the LIFO free-list finds the
   entry in ≈ D/2 probes (the average over the measured completion orders;
   the structural range is [1, D] — see the R9 scope note below) touching
   ⌈D/4⌉ cache lines per average lookup (16 lines at D=64; corrected by the
   R9 audit — the "1-2 hot cache lines" phrasing here originally
   understated the footprint ~8×). Its total weight is ≤2.28% of user cycles
   at the worst artificial shape
   (C=1024/D=64) and ~0.4% of total CPU; at production shapes (the
   harness configures C=D, default 64) it is smaller still. The capacity-scaled
   cost the issue hypothesized exists only in two non-hot places: the forward
   by-handle cancel scan (~993 probes/cancel at C=1024) and full-sweep stale
   misses (zero occurrences naturally).

2. **Would a faster structure reduce total lifecycle work?**
   No. Measured end-to-end: eliminating the only capacity-scaled scan 30×
   (WE-2) changes wall time by less than the A/A noise floor on the native
   host. Analytically: an index pays one insert and one erase (random-access
   writes, tombstone bookkeeping) per operation to accelerate a single
   O(D/2) sequential probe stream that is never the bottleneck — the
   work is relocated from completion to dispatch/retire, not removed, and
   the analytic lower-bound argument puts the relocation cost above the
   saving at every supported configuration (Candidates A/B were never
   built or measured; `lifecycle-cost.md` holds lower bounds on what such
   a structure must pay, not measurements). The
   two no-new-state work eliminations (WE-1: router index carried in ledger
   entries, retiring S1/S6; WE-2: scan direction) are strictly smaller
   mechanisms, and even WE-2's measurable benefit is zero at native noise
   resolution.

3. **Advance any design to further validation, or keep the production
   implementation?**
   Keep the production implementation. No candidate earned further native
   validation: there is nothing material to validate. If a future workload
   ever demonstrates sustained live depth in the hundreds with lookup shares
   above the materiality threshold (via #578 real-app evidence or a fresh
   native trigger campaign), revisit order is: WE-2 (trivial, semantics-safe,
   already measured) → WE-1 (small, ledger-local) → only then Candidate A.
   The H1 packed-cookie family remains excluded by the domain-narrowing rule
   regardless.

## Product change

`PRODUCT_CHANGE: NONE`. No production `src/`, `include/`, spec, CI or test
change is in this branch: the archival revision restored the three
`src/async` files byte-identical to master `46157fd1` and moved the
compile-time-gated research instrumentation/arm/mutant/clflush blocks into
`patches/research-gates-full.patch` (admitted by round-trip in a fresh
worktree; replayable via `bench/research/uring-router-580/prepare-research-worktree.sh`).

## Open items handed back

- #578 (fio + DuckDB real-app baseline) remains the only path that could
  produce a *new* trigger from real application shapes; this round's harness
  shapes cover the supported configuration space, not arbitrary applications.
- #580 is CLOSED with `NO_TRIGGER_NO_CHANGE` (closeout comment links the
  evidence chain); the reopen conditions are the two boundaries above plus
  the revisit order WE-2 → WE-1 → Candidate A.

## R9 scope addendum — cache-locality falsification (this revision)

The R9 audit (`cache-locality-audit.md`) found five places where the R0–R8
texts stated warm-cache assumptions more strongly than their evidence
("1–2 hot cache lines" vs the true ⌈D/4⌉-lines-per-average-lookup footprint;
D/2 stated without its [1, D] completion-order range; 1–2 cycles/probe vs
the measured 3–5; the 2.28% denominator sensitivity unquantified; Candidate
A/B net-negative stated as settled rather than a lower-bound argument). All
five are corrected in this revision; none changed a measured R0–R8 number.

The R9 experiment (`cache-experiment.md`, `cache-results.md`) then tested
the corrected, weaker claim set under cache pressure on both hosts:
interleaved independent working sets up to 16 MiB at 4 KiB/op, D ∈
{16…512}, W-02 single-owner and W-03-replica single/split/driver forms,
clflush forced-eviction bound, and PMU attribution (L1 on both hosts; LLC
on E5; WSL LLC events unavailable — partial attribution marked).

Verdict: **`NO_TRIGGER_NO_CHANGE` is retained**, now scoped to the measured
domain: D ≤ 512, working sets ≤ 16 MiB interleaved at ≤ 4 KiB/op, W-02 /
W-03-replica execution forms, WSL Zen 3 + E5 Haswell. Within that domain:
lookup counts, probe counts (≈ D/2) and zero-miss behavior are invariant
under pressure; the lookup's attributed cycles (3–5 cycles/probe warm) do
not grow under 16 MiB interference on either host; every separable
end-to-end effect is D-flat or D-anticorrelated (a pacing-shaped effect,
not a locality-shaped one; D=512 under interference is net *faster*, 0.963
[0.955, 0.970] on E5 — the batching mechanism behind it is a hypothesis,
the anti-correlation itself is measured).

Two measured boundaries qualify the negative verdict and are recorded as
reopen conditions, not triggers (figures as revised by the delta review):
(1) at D=512 warm — a depth no standard W-02/W-03 form uses — the
harness-inclusive share is ≈5% (WSL, stable across run lengths) to
5.8–9.8% (E5, condition-dependent across run length and clock), ≈
0.21–0.50 µs/op absolute — a measured mechanism hotspot at a
non-representative shape; (2) under forced eviction of the router before
*every* lookup (synthetic), the arm costs +26%/+108% wall on E5 — but the
   delta review's capacity sweep decomposed this: ≈90% of it is the clflush
   instruction itself (~30 ns per flushed line on E5 — a per-instruction
   figure at ⌈C/2⌉ flushes per lookup, not a per-I/O cost), and the scan's
   genuine cold-refill penalty is ≈ +8% (E5) / +29% (WSL) wall at D=512 —
   a capacity-gradient differential estimate (cache-results.md §3), not a
   directly measured pure miss cost — small because the scan
is sequential over a contiguous live span and hardware prefetch amortizes
even DRAM refills. Whether any alternative structure (index) would beat
the scan in that regime is **unmeasured** — no index arm was built — and
is not claimed. Reopening Candidate A/B would require a real application
form (#578) with sustained D in the hundreds, together with either
measured per-lookup L2+ eviction in that application or a *measured*
prototype win; neither exists today.

The issue-level verdict `NO_TRIGGER_NO_CHANGE` and `PRODUCT_CHANGE: NONE`
are unchanged; PR #592 remains open and unmerged by instruction.
