# R4 — Full lifecycle cost model

Frequencies are measured (E5 + WSL2; counts agree structurally, widest cell ~1 probe apart); unit costs are
bottom-up cycle estimates cross-checked against the perf-attributed shares in
`trigger.md` and the E5 `find_live_router_cookie_` self-share (2.28% of user
cycles at C=1024/D=64, the worst measured shape). Candidate A/B columns are
**hypothetical** (gate-blocked, not built) — their entries are analytic
lower bounds on what such a structure must pay, not measurements.

## Operation ledger (per logical operation unless noted)

| Operation | Frequency (measured) | Baseline (prod) | WE-1 (ledger idx) | WE-2 (dir flip) | Cand. A hash (hyp.) | Cand. B sorted (hyp.) |
|---|---:|---|---|---|---|---|
| Cookie lookup hit (op CQE) | 1.00/op | D/2 probes, hot lines | unchanged | unchanged | ~1 probe + exact verify | log2(D) ≈ 6 probes @64 |
| Cookie lookup hit (control CQE) | 0.2/op @r=0.2 | D/2 probes | unchanged | unchanged | ~1 probe + verify | log2(D) |
| Cookie lookup (consumed control S1) | 0.2/op @r=0.2 | D/2 probes | **0 (index in ledger entry)** | unchanged | ~1 probe + verify | log2(D) |
| Cookie lookup (poison Class-A S6) | ~0 (fault path) | D/2 per ledger entry | **0** | unchanged | ~1 probe + verify | log2(D) |
| Cookie lookup miss (stale CQE) | 0 natural; O(C) when it happens | C probes full sweep | unchanged | unchanged | ~1 probe (miss) + verify | log2(C) |
| Handle lookup hit (running cancel S2) | 0.2/op @r=0.2 | **(C−D)+D/2 = 993 @C1024** | unchanged | **D/2 = 33 (measured)** | ~1 probe + verify | log2(D) |
| Handle lookup miss (settled cancel) | rare, raced only | C probes | unchanged | C probes (miss unchanged) | ~1 probe | log2(C) |
| Router insert (dispatch) | 1.00/op | O(1): free-list pop + entry write + ledger append | +4 B ledger field write | unchanged | **+hash insert (random write, probe seq)** | **+O(D) ordered shift** |
| Router retirement | 1.00/op | O(1): zero entry + free-list push | unchanged | unchanged | **+hash erase (tombstone write)** | **+O(D) shift** |
| Pending-control sweep (S5) | only after SQ-full deferral (0 observed) | C probes/pass | unchanged | unchanged | unchanged | unchanged |
| Extra memory per capacity | — | 32 B × C (router) | +4 B × ring_entries (ledger) | **0** | +(12-16 B) × ~2C table | +16 B × C sorted array |
| Initialization / teardown | once | vector init C entries | +ledger field | **0** | table init + rehash-free proofs | sort-on-build or ordered inserts |
| Contention / synchronization | — | all under `dispatch_mtx_` (uncontended in measured shapes) | same | same | same + index invariants under same lock | same |
| Repair / rebuild / tombstones | — | none exist | none | none | **tombstone accumulation bounds + insert-after-erase ordering proofs required** | none |
| Added invariant surface | — | none | ledger entry must be updated where index is known (append sites) | none (match predicate unchanged) | stale-entry→misroute, overflow, false-hit, erase-skip mutants (M1/M2/M3/M5/M7) | none beyond sorted order |

## Break-even arithmetic (measured ratios)

`Net saving ≈ Q × saved_lookup_cost − U × added_maintenance − overhead`

With the measured steady-state values Q = 1.00 lookups/op, U = 1 insert + 1
erase/op, M ≈ 0:

- Baseline saved cost if lookup were free: 8.5-33 probes ≈ 12-70 cycles ≈
  4-24 ns — bounded above by the measured attribution: **2.28% of user cycles
  at C=1024/D=64 (E5), 1.6% (WSL), ~0.4-0.5% of total CPU**.
- Candidate A pays per op: 1 hash insert + 1 erase (tombstone) ≈ 2 random
  cache-line writes ≈ 30-80 cycles, plus cold-table misses at large C
  (the table is 2-3× the router array and randomly addressed).
- Net at every measured shape: **zero to negative** (lower-bound argument:
  the A/B columns are analytic lower bounds on maintenance, and the saved
  cost is bounded by the measured attribution; R9 additionally measures the
  fully-cold-per-lookup regime that could break this bound — see
  `cache-results.md`). The index converts a
  short sequential scan over ⌈D/4⌉ lines per average lookup (16 lines at
  D=64; R9 audit corrected the original "2-3 hot lines" phrasing) into
  random-access bookkeeping on
  every operation to accelerate a path that is never the bottleneck.
- Candidate B additionally turns U into O(D) shifts per op: strictly dominated
  by both alternatives at all D ≥ 2.

## The five protocol questions

1. **At what C/D/miss-rate does A beat baseline?** Only when the cookie
   lookup's share of total cycles is material — which requires sustained live
   depth D in the hundreds-plus AND a microarchitecture where the sequential
   probe stream stalls (cache-cold router). Under the supported production
   shapes (harness sets C=D, default 64) the measured share is 1.6-2.3% of
   user cycles; H1's native A/B bounded the free-lookup gain at ≤3.7% even at
   C=1024. No measured configuration reaches a 5% materiality threshold.
   Miss-rate does not help A: natural misses are zero in 100k+ completions.
2. **What workloads make A or B worse?** Churn-heavy (tombstone pressure,
   re-probe sequences), small D (maintenance dominates a 1-8 probe scan),
   cancel-heavy (extra control-entry insert/erase pair per cancel), metadata
   workloads (identical lookup profile, no offsetting savings), and any
   workload where the router table is cache-cold for the index too.
3. **Do insert/erase costs eat the savings?** Yes at production shapes:
   U=1+1 per op versus Q=1 per op means the index pays its bookkeeping on
   every operation to speed one O(D/2) in-cache probe stream.
4. **Is there a smaller control-path-only fix?** Yes — exactly WE-1 (store
   `router_index` in ledger entries: removes S1 and S6 scans, no new
   structure) and WE-2 (reverse the by-handle scan: measured 993→33 probes
   per cancel at C=1024, no new state). Both leave the per-CQE path
   untouched; both are recorded as research findings, not product changes.
5. **Does an index reduce work or relocate it?** Relocate: from the completion
   path (sequential, hot, O(D/2)) to dispatch/retire (random-access,
   every-op, plus tombstone/repair invariants and memory). The measured
   end-to-end evidence (WE-2's 30× probe reduction producing a
   statistically neutral wall/CPU change, E5 A/B §R6) shows the completion
   path's scan weight is already below the noise floor — there is nothing
   material to relocate.
