# Decision

## Verdict chain

| Level | Verdict | Basis |
|---|---|---|
| R2 trigger gate (index candidates A/B) | `NO_TRIGGER_NO_CHANGE` | 1 lookup/op; hot-path probes O(D/2) **independent of C** (8.5 @D16, 1.0 @D1); `find_live_router_cookie_` = 1.57-1.69% (WSL) / 2.28% (E5) of user cycles at the worst artificial shape, ~0.4-0.5% of total CPU; zero natural misses in 100k+ completions; theoretical maximum gain (free lookup) < 2% user cycles at every measured shape, below the 5% materiality threshold; corroborated by #561's native ≤3.7% ceiling |
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
   work is relocated from completion to dispatch/retire, not removed, and the
   relocation costs more than it saves at every supported configuration. The
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
change is requested. The research patch in this branch is compile-time-gated
and proven codegen-neutral; the owner may drop the `src/` hunk entirely and
keep only `docs/` + `bench/research/` when archiving.

## Open items handed back

- #578 (fio + DuckDB real-app baseline) remains the only path that could
  produce a *new* trigger from real application shapes; this round's harness
  shapes cover the supported configuration space, not arbitrary applications.
- #580 stays open for the owner's verdict; closing it with
  `NO_TRIGGER_NO_CHANGE` is consistent with this evidence, per the issue's own
  exit conditions.

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
end-to-end effect is D-flat or D-anticorrelated (dispatch pacing, not
locality; D=512 under interference is net *faster*, 0.963 [0.955, 0.970]
on E5).

Two measured boundaries qualify the negative verdict and are recorded as
reopen conditions, not triggers: (1) at D=512 warm — a depth no standard
W-02/W-03 form uses — the harness-inclusive share is 5.07% (WSL) / 5.80%
(E5) of user cycles, ≈ 0.21–0.30 µs/op absolute; (2) under forced eviction
of the router before *every* lookup (clflush, synthetic), the scan costs
+26% (D=64) to +108% (D=512) wall on E5 — the regime where an exact index
would win. Reopening Candidate A/B would require a real application form
(#578) demonstrating sustained D in the hundreds **together with** per-
lookup L2+ eviction or a denominator leaner than any measured harness;
neither exists today.

The issue-level verdict `NO_TRIGGER_NO_CHANGE` and `PRODUCT_CHANGE: NONE`
are unchanged; PR #592 remains open and unmerged by instruction.
