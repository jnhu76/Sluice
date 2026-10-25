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
