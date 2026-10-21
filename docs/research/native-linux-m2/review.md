# Independent review record

Two isolated reviews ran against the frozen M2 candidate tree, patches, raw
results and documents. Both were read-only with respect to production code;
dispositions were applied to the research branch only.

| Review | Scope | Verdict | Mechanism-level blocking finding |
|---|---|---|---|
| A | Concurrency, identity protocol, BACKEND-04 boundary, test validity | REVISE | none |
| B | Benchmark design, statistics, provenance, perf attribution | REVISE | none |

"Mechanism-level" means a defect that would change a KEEP/IMPROVEMENT decision.
Both reviews found evidence-validity and reporting defects instead. Each finding
below is closed unless marked residual.

## Review A: concurrency and protocol

| ID | Severity | Finding | Disposition | Evidence |
|---|---|---|---|---|
| A1 | P1 (test validity) | `stale_cookie_cannot_reach_a_reused_slot` compared the injected identity against `peek_next_cookie_for_test()`, which reads an unconsumed counter. The test passed even when the router lookup ignored the cookie, so it did not test stale-identity rejection. | Fixed in `h1.patch`: both requests consume a cookie, the test reads each live identity through `live_cookie_for_offset_for_test`, injects request A's identity after slot reuse, then completes request B with its own identity. | `results/tests/h1_slot_only_mutant.log`: a slot-only mutant fails exactly this test (20/21). |
| A2 | P2 (identity protocol) | `uring_identity_envelope_test.cpp` control 1 encoded the envelope by hand instead of using the production packing helpers, so the test could drift from the real layout. | Fixed: control 1 includes `uring_internal.hpp` and uses `make_operation_identity` / `operation_identity_slot`. | `h1-identity-matrix.md` row 15; `protocol.md` §5. |
| A3 | P2 (protocol admission) | The identity matrix did not cover the Control SQE/CQE target path, poison/Class-A recovery and the mutant overflow path end to end. | Added rows 10-16 and the limitations section; the matrix now states which paths are verified by test, by the existing conformance suite, or only as a kernel-matching rule. | `h1-identity-matrix.md`. |
| A4 | P2 (BACKEND-04) | The H2b claim that the derived counter is not settlement authority needed explicit confirmation that teardown still calls the authoritative scan. | Confirmed: destructor and `internal_work_retired()` still use `any_event_owed_locked_()`; only the progress pass consults `owed_count_`. | `h2-results.md` §"BACKEND-04 boundary". |
| A5 | P3 (counter maintenance) | The submit-reset decrement of `owed_count_` appeared unreachable. | Retained as defensive parity, disclosed as unreachable in the exercised paths; the parity test covers the reachable set/clear paths. | `h2-results.md` limitations. |
| A6 | P2 (reporting) | Layer A arm labels implied an H2a-only candidate while the measured shape was H2ab. | Relabelled; Layer A and the A/B campaign are now described by their actual shapes. | `results/layer-a/`, `h2-results.md`. |
| A7 | P2 (claim validity) | Text asserted H2a was "uniformly dominated" and H2ab "indistinguishable" from H2b, which the paired data did not support. | Replaced with direct paired contrasts; H2a is better at 1 MiB/C=64/d=1, and H2ab changes sign across configs. | `h2-results.md`, `end-to-end-ab.md` §5. |

## Review B: benchmark and attribution

| ID | Severity | Finding | Disposition | Evidence |
|---|---|---|---|---|
| B1 | P1 (validation) | The validator accepted a timed row with `ok=false` or a non-empty `failure` as long as `ops==requests` and `allocations==0`; a post-loop shutdown failure could therefore produce an accepted timing sample. | Fixed in `analyze.py` and `validate-results.sh`: the timed row must have `ok=true` and an empty `failure`. Both campaigns re-validated with zero timed failures. | `results/{ab-15,ab-clean}/validation.txt` now report `timed_failures=0`. |
| B2 | P2 (provenance) | The replication lacked per-arm commit identity and a provenance record. | Added `results/ab-clean/arm-identity.txt` and `provenance.txt`; the frozen primary runner is committed as `results/ab-15/run_ab.frozen.sh`. | Files listed in `end-to-end-ab.md` §7. |
| B3 | P2 (reproducibility) | The exact primary runner and replication command were not preserved. | Committed the frozen runner and `results/ab-clean/replicate.sh`. | Same. |
| B4 | P2 (statistics labelling) | CPU-time confidence intervals were labelled as wall-clock intervals in two tables. | Relabelled; the reported CPU ratio, wall ratio and p99 ratio intervals are now distinct. | `h1-results.md`, `h2-results.md`. |
| B5 | P2 (attribution) | Several perf medians, symbol-share labels and paths were wrong, and the cache-miss column mixed two event groups. | Recomputed from the committed flat reports; renamed the column `cache_miss_pct`; corrected sample counts, lost-sample count and the `-m 64` rationale. | `cpu-attribution.md`, `results/perf/`. |
| B6 | P2 (claims) | "Uniformly dominated" and "indistinguishable" overstated the data; the 1 MiB and 64 KiB results reverse locally. | Replaced by the paired-contrast table; the H2ab decision now cites the sign flip. | `end-to-end-ab.md` §5, `h2-results.md`. |
| B7 | P3 (threats) | Background load and SMT-sibling sharing were not bounded in the report. | Recorded: primary median loadavg 2.48, replication 1.35, CPU 2 sibling CPU 12 not isolated; paired/rotated ordering is the mitigation, not elimination. | `end-to-end-ab.md` §6. |

## Residual risks carried into the decision

1. Both campaigns ran on one machine with no cpuset isolation and no SMT-sibling
   exclusion. Paired/rotated ordering and an independent replication reduce, but
   do not remove, interference risk.
2. `perf_event_paranoid=2` and `dmesg_restrict=1` restrict attribution to
   user-space symbols on E5. Kernel-side time is inferred from the user/system
   split, not from kernel symbols.
3. H2b's best case (4 KiB/C=1024/d=1) leaves a 0.478 CPU floor dominated by
   unchanged system time; the mechanism cannot address that floor.
4. H1's end-to-end effect (<=3.7%) is below what this design can distinguish
   from run-to-run variation at several configs, so H1 is classified
   MICROBENCH_ONLY rather than as a null result.

## Final adjudication

Both reviews closed with no mechanism-level blocking finding. The decisions in
`end-to-end-ab.md` §5 stand as written:

- H1: `MICROBENCH_ONLY`.
- H2a: `MECHANISM_IMPROVEMENT_CONFIRMED`, not selected.
- H2b: `MECHANISM_IMPROVEMENT_CONFIRMED`; minimal candidate only, not applied.
- H2ab: not selected (no repeatable gain over H2b).

## External owner review of the delivered PR (2026-10-21)

A third review (GitHub review `5599795090`, verdict REVISE) examined the
delivered PR head `1b37d835` — the research patches, `pipeline_ab.cpp`,
`run.sh`, the paired-validation records and the base production paths — and
confirmed the W-02 finding (H2b paired ratio 0.478, 95% CI [0.474, 0.497],
replicated). Its two P2 findings and their closeout in this commit:

| # | Finding | Closeout |
|---|---|---|
| 1 | W-03 coverage was requested but absent: the harness only drives `RequestScope::submit_read` → `wait_for` → `take` with `--backend=uring`; the 20 configurations are W-02 combinations, not W-02 × W-03, so a W-02 gain cannot certify W-03 wakeup/liveness | `W03_AB = NOT_RUN` is now recorded explicitly in `end-to-end-ab.md` §1, `protocol.md` §1 and `README.md`; W-03 admission is added to the H2b productization checklist as an outstanding gate (`h2-results.md` §6), and the scope delimitation is posted on Issue #561 |
| 2 | H2b's submit-reset branch silently normalizes an impossible state: if it were reachable, silently dropping an undelivered owed record while updating the derived count could mask a lifetime/control-ref bug | The obligation is now explicit in `h2-results.md` §5/§6: a product proposal must either justify the transition as legal with an independent test that reaches it, or replace the branch with an invariant-fail (fail-fast) whose unreachability a negative oracle establishes; silent normalization is not acceptable. This strengthens the earlier A5 disposition ("retained as defensive parity") |

Evidence framing accepted and now stated with the results: the 0.478 ratio is
a sparse-occupancy case (C=1024, depth 1, 64 MiB tmpfs, one pinned CPU) on
warm tmpfs, not a general 52% gain or a block-device speedup; the paired A/B
ratios are the performance claim and user-only perf shares are attribution
context only; kernel-symbol attribution is unavailable at
`perf_event_paranoid=2`.

Dependency path executed: #560 was closed out (`2e998fba`) and merged first
(merge commit on `master`), then this PR was retargeted to `master` so its
diff is M2-only.
