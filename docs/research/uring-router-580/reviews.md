# R7 — Independent reviews

Review A (Semantics/Concurrency) and Review B (Measurement/Causality) were
performed by two independent agents with access to the worktree sources, the
pristine master checkout, the raw JSONL and the scripts. Both independently
spot-verified claims against source/data rather than reading only conclusions.

## Review A — Semantics/Concurrency: **REVISE** (record-only revisions; no result invalidated)

Independently verified TRUE against source at `46157fd1`: cookie domain
monotonicity and guard; LIFO free-list initialization/pop/push; both scan
directions; all six scan sites S1-S6 with their lock contexts and miss
behaviors; the WE-2 observational-equivalence conclusion (strengthened: no
reachable state, not merely no legal interleaving, distinguishes the
directions, because a reservation dispatches at most once and a (slot,
generation) pair never recurs); the slot-only mutant's genuine unkillability;
the FALSE_MISS kill landing on the real reap path via named assertions; and
the no-define patch neutrality (no ABA / double-retirement / borrow-UAF /
lost-terminal / unbounded-state risk).

Findings and dispositions:

| # | Severity | Finding | Disposition |
|---|---|---|---|
| 1 | P2 | `Generation` is u64 (doc said u32); `RouterEntry` is 32 B (doc said 24 B; 2 entries/line, 32 KiB/512 lines @C=1024) | fixed in `source-dag.md` (R1-A fact 6, R1-B row) |
| 2 | P2 | slot-uniqueness invariant holds by a **second** pinning mechanism the doc omitted (control reference pins reclaim; `release_control` unpins under the same lock hold immediately before retirement) | fixed in `correctness-oracles.md` with both mechanisms |
| 3 | P2 | byte-identity proof covered only `uring_transport.cpp` while the README claimed the whole diff | extended: `uring_completion.cpp` proven byte-identical (`da1c76d3…`, both objects re-hashed on E5); manifest + protocol + README updated |
| 4 | P3 | cookie-domain upper edge `[1, 2^63-2]` vs actual `[1, 2^63)` | fixed in `source-dag.md` |
| 5 | P3 | oracle header comment said mutant threshold `>= 3`, code uses `>= 2` | fixed |
| 6 | P3 | WE-1 phrasing: baseline keys on cookie and post-verifies handle; WE-1 inverts the roles (cookie becomes the post-verified field) — which is why authority survives | fixed in `source-dag.md` WE-1 |

## Review B — Measurement/Causality: **APPROVE** (conditional on record/tooling fixes; all applied)

Independently recomputed from raw data: every E5 paired median and interval
(reproduced exactly to 4 decimals), the WSL reductions, cancel-shape
lookups/op = 1.600, S2 averages per host, zero natural misses, reducer
read-only behavior. Checklist passed cleanly on: trigger authenticity
(real-kernel rows; seam self-tagged; instrumented binary excluded from every
timing claim), arm parity (compile-time label only; disassembly differs only
in loop-direction arithmetic; per-run fixtures; same pinning), paired AB/BA
ordering with no sample selection (90/90 + 60/60 + 26/26 rows, all
oracle-green), honest A/A use, gate arithmetic consistency (2.28% share ≈ 163
cycles/op vs 34-68 bottom-up; <5% threshold at every shape), and host-switch
record integrity.

Findings and dispositions:

| # | Severity | Finding | Disposition |
|---|---|---|---|
| 1 | P2 | E5 reductions not reproducible from committed scripts (hardcoded WSL paths) | `reduce_ab.py` parameterized (argv), `run_ab.sh` OUT/REPS env-parameterized; E5 invocation documented in `measurement-protocol.md` |
| 2 | P2 | A/A statistic computed as first-slot/second-slot due to a dead arm test (both A/A rows carry `arm=baseline`), while labeled β/α | reducer now pairs on the `binary` identity; true β/α numbers regenerated (E5: user 0.9634 [0.9445,1.0777] / 1.0039, wall 1.0068 / 0.9956); `wsl-results.md` updated with a correction note; conclusions unchanged (the earlier figure was a conservative yardstick) |
| 3 | P2 | no committed build for the instrumented probe | `build_probe.sh` added (exact flags; WE2=1 variant) |
| 4 | P3 | "identical/replicate exactly" overstated; widest cell 0.9 probes apart; 993.0 → 992.9 | wording fixed in `wsl-results.md` / `lifecycle-cost.md` |
| 5 | P3 | two WSL table cells were rep-1 values, not 2-rep means; aggregation rule unstated | cells corrected (33.7 / 992.4) and the mean-of-reps rule stated in `trigger.md` |
| 6 | P3 | cancel-shape work-mix variance (bytes spread 7.6%) undisclosed | disclosed with the sensitivity check (bytes-normalized user median 0.9867 → 0.9989, toward the null) |
| 7 | P3 | negative-control row mislabeled C16/D1 | fixed (C1024/D1 depth control + C16/D16 small-capacity control) |
| 8 | P3 | perf shares lack sampling intervals and are not recomputable from the repo | ±0.8pp binomial interval note added; raw .data paths/sizes/hashes already disclosed in manifest as non-persistent tmpfs artifacts |
| 9 | P3 | timed region includes harness verification/wait work (harness-inclusive denominator) | disclosed in `trigger.md` and `wsl-results.md` (identical across arms) |
| 10 | P3 | cancel-c64 wall ratio 0.9888 [0.983,0.996] not mentioned in the summary | now stated alongside the user ratio, both inside the A/A band and not claimed |

## Re-review status

Both verdicts were record-level (REVISE/APPROVE-with-conditions); every P2/P3
was a documentation, labeling or reproducibility item. All items above are
applied in this branch; no measurement, oracle run, mutant or conclusion
changed as a result. The one numerical correction (A/A β/α vs slot-order) was
in the direction that makes the prior conservative reading still valid.

## R9 review — methodology (this revision)

**Provenance disclosure:** the independent-agent review channel was
unavailable for this round — four dispatch attempts failed on the host's
model-concurrency limit (2026-10-25, attempts logged in the session
record). The review below is therefore a structured adversarial pass
performed in the authoring session against the raw JSONL/reduced data, NOT
an independent review; the owner may commission an independent re-review of
`cache-*.md` before treating R9 as fully reviewed. All spot-check commands
are reproducible from `results/cache-{wsl,e5}.jsonl`.

Checklist results (reviewer questions 1–8 of the R9 protocol):

1. **Interference separation / netting.** Sound for the pipeline shape: the
   sweep runs on the same thread inside the timed region and its seconds
   are metered with the same clock; measured sweep fraction is 1.1–2.0% of
   wall on both hosts (checked over all 135 interfered E5 and 135 WSL pair
   rows). Split-core rows are gross wall — the sweep can overlap I/O wait —
   and `cache-results.md` §2 discloses this and does not net them.
2. **Pacing vs locality.** The locality mechanism predicts ws-monotonicity
   (until LLC) and D-monotonicity; the data violate both on both hosts, and
   the D=512 inversion (net 0.963 [0.955, 0.970] vs A/A floor median
   1.007) is corroborated by an independent metric (user cycles −5.5%,
   sys −1.4%) consistent with deeper CQE batching. No cherry-picked cell is
   load-bearing: every conclusion cites all five depths or both hosts.
3. **A/A and pair count.** A/A uses byte-identical binaries under two
   paths, AB/BA alternation, and the same bootstrap as the effect rows; E5
   floors are tight (±0.5–0.8% at D64). Claims of *no effect* are stated
   where intervals overlap floors; the one claimed effect (ws64K +2.1% net
   E5) has an interval excluding the floor band, and is explicitly
   classified as non-locality (smallest working set, cannot evict L2).
4. **Cross-source cyc/probe.** Legitimate after correction: perf scales
   multiplexed counts (verified: cycles:u ÷ (user_s × 2.90 GHz) ≈ 0.98 on
   the E5 d64-warm cell), so cycles/op is absolute within a few percent.
   "Share does not grow under interference" is bounded independently of
   ±40% sampling noise by the end-to-end net cap (≲ +1 cyc/probe at D64
   ws16M) — added to `cache-results.md` §4 during this review.
5. **clflush arm.** The doc states the arm includes the clflush
   instruction cost itself and therefore overstates even the fully-cold
   cost; used only as an upper bound for a synthetic regime. Honest.
6. **Probe invariants.** Spot-checked 12 cells across both hosts (D16/D64/
   D512 pipelines incl. ws16M, external single/split/driver): 1.00
   lookups/install, 0 misses, avg within the documented ranges. Two number
   errors found in the original §1 (D128 lower bound 63.8 → 60.4; ops/cell
   range 22–42 K → 10.5–22 K) — fixed in this revision.
7. **Overstatement scan.** The decision addendum's scope (D ≤ 512, ws ≤
   16 MiB @ ≤ 4 KiB/op, W-02/W-03-replica forms, both hosts) matches what
   ran; the D=512 share crossing 5% is disclosed with its absolute
   per-op cost and non-standard-shape status; Candidate A/B remains
   labeled lower-bound analysis with the clflush regime recorded as its
   falsifiable escape condition.
8. **Arithmetic.** ⌈D/4⌉ lines (16 @D64, 128 @D512) ✓; all eight cyc/probe
   products recomputed ✓; §2 net_wall medians match `cache-e5-reduced.json`
   ✓.

Verdict: **ACCEPT_WITH_FIXES (same-session review; independence limitation
disclosed above)** — the two §1 number errors and the §4 scaling/noise
strengthening were applied during the review. The retained
`NO_TRIGGER_NO_CHANGE` verdict and its two recorded boundary conditions
stand on the corrected text.

## R9 delta review — independent closeout gate (this revision)

Commissioned by owner instruction after the R9 round ("R9 Independent
Delta Review & Closeout Gate"), executed as a fresh adversarial pass
against the raw data with independent recomputation (new script, not the
campaign reducer), targeted new measurements where evidence gaps were
found, and no presumption in either direction. Frozen R9 HEAD verified at
`350e7f7f` (clean); production master `46157fd1` verified pristine; branch
diff confined to the three gated `src/` TUs, docs, and `bench/research/`.

### Review A — forced eviction

Gate location verified in source (`find_live_router_cookie_` entry, whole
`router_` array, 64 B stride). The published +108% (E5, D512) conflated
eviction mechanics with refill cost. **New targeted measurement** (capacity
sweep, 15 AB/BA pairs per cell, both hosts; `results/cache-flush-sweep-*`):
the C-slope isolates the `clflush` instruction at ≈30 ns/op on E5
(consistent at both depths), so ≈7.66 of the 8.25 µs/op flush-arm delta is
eviction mechanics; the genuine cold-refill penalty of the scan is ≈
+0.59 µs/op (≈+8% wall) on E5 and ≈1.9–2.2 µs (≈+29%) on WSL at D=512.
The refill is small because the scan is sequential over a contiguous live
span — hardware prefetch amortizes even DRAM refills. Finding applied: §3
of `cache-results.md` rewritten; the unsupported "an exact index … would
win" claim (also present in the decision addendum) removed everywhere —
no index arm was built, and no cold-regime index claim is made in either
direction.

### Review B — interference attribution

`net_wall = wall − metered sweep` verified sound for the pipeline shape
(same thread, same clock, sweep fraction measured 1.1–2.0% of wall);
external rows correctly not netted and now explicitly caveated. The
"+1 cycle/probe cap" argument was overstated as strict: net_wall bounds
lookup growth only modulo offsetting savings (the D=512 inversion proves
such savings exist at scale). Reworded, with the symbol-share evidence
carried alongside. The D=512 inversion's batching mechanism **relabeled a
hypothesis** — no batch-size counter exists in this round; the
anti-correlation itself is the measured fact. PMU audit: original record
runs held only 670–705 samples (±1.1–1.7 pp on shares); **new 200 K-op
records** (1–3 K samples) show WSL shares stable (5.07→5.01% at D512) but
**E5 shares rising monotonically with run length** (5.80% @42 K → 9.81%
@202 K) with constant total cycles/op, constant probe counts (254→257,
instrumented verification at 20 K/100 K/200 K ops), and PMU-independent
15-rep medians flat — plus evidence of turbo-clock drift between cells
(≈3.40 vs ≈3.22 GHz from user-time ÷ cycles). Conclusion recorded as a
band (E5 D512 warm: 5.8–9.8%, condition-dependent), with the instability
disclosed in §4 rather than resolved — resolving it would need a
clock-controlled PMU campaign outside this review's scope.

### Review C — materiality at D=512

Four levels, now explicit in §5: **measured mechanism hotspot — yes**
(≈5% stable WSL, 5.8–9.8% E5 band, ≈0.21–0.50 µs/op at a shape no
standard bench uses); **representative application trigger — no** (no
standard form runs D>64; #578 has no baseline); **established avoidable
cost — no** (no measured alternative; the scan's cold penalty is
prefetch-amortized, index costs unknown); **product admission — no**.
`NO_TRIGGER_NO_CHANGE` remains valid as the exit verdict, with sharpened
revisit conditions: (a) #578 real-app sustained D in the hundreds; (b)
measured per-lookup L2+ eviction in that application; (c) a measured
prototype win in that regime — none exists.

### Review D — closeout recomputation and provenance

Independent recomputation from the raw JSONL (fresh script) reproduced
every load-bearing published number exactly: E5 net_wall medians
(1.0205/1.0131/1.0054/1.0061/1.0129/0.9627), A/A bands (1.0011/1.0033/
1.0065/1.0077), flush ratios (1.2623/2.0772), zero invariant violations
across all 118 counters rows on both hosts, D=512 inversion absolutes
(7.562→7.258 µs/op; user 0.972, sys 0.974 — the doc's earlier 0.981/0.986
came from the PMU cells and was replaced). **Provenance defect found and
fixed**: the manifest and `environment.md` recorded WSL `cache_*` binary
SHA-256s from a pre-rebuild build; the binaries that produced the
committed WSL results are the post-offset-fix rebuild (SHAs corrected
in-place with a correction note; E5 SHAs were correct; the committed
sources deterministically rebuild the correct binaries). Secondary checks:
E5 sweep file deduplicated after a double-run during the review; E5
records re-paired share × cycles from the same run; one stat-derived
cycles/op figure (14,902 → 14,636, now same-source) corrected.

### Verdict

**REVISE → re-reviewed → APPROVE.** All four review tracks found real
defects (overstated cold-bound inference, unproven index claim, strict-cap
overstatement, hypothesis-stated-as-mechanism, stale binary SHAs, E5 share
instability undisclosed); all were fixed in this revision and re-checked
against the raw data; no production algorithm, spec, or public surface was
touched; the corrected evidence still supports the R9 conclusion — the
interference result set was never in doubt (it is PMU-independent and
reproduced independently), and the two boundary conditions are now stated
with their true magnitudes and uncertainty. `CACHE_VERDICT` within scope:
`HOT_PATH_ROBUST`.
