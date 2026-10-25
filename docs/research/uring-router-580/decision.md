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
   entry in ≈ D/2 probes on 1-2 hot cache lines regardless of configured
   capacity. Its total weight is ≤2.28% of user cycles at the worst artificial
   shape (C=1024/D=64) and ~0.4% of total CPU; at production shapes (the
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
