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
