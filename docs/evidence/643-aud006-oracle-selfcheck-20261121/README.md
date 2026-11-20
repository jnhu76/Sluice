# AUD-006 (#643 Phase A): semantic-oracle comparator self-check evidence

Worktree `/tmp/sluice-643-aud006`, branch `test/643-oracle-self-validation`,
base `e16960ff8be38d4c9773b40b595e7c1c63473d99` (origin/master at survey
time). Host: linux x86_64, gcc via ccache, xmake v3.1.2+HEAD.

## What was verified

The acceptance comparators of the shared semantic-oracle harness
(`agree()`, `run_path()`, `matches_recorded_divergences()`,
tests/semantic_oracle_harness.hpp) had no negative controls: a defective
comparator yielded false-positive GREEN for all three dependent suites
(`semantic_validation_precedence_test`, `semantic_backend_conformance_test`,
`direct_composition_fault_test`). Remediation:

1. A hand-authored truth table (`truth-table.md`) over agree/run_path/
   matches_recorded_divergences, enforced by a new shared
   `comparison_oracle_selfcheck()` in the harness, invoked as the first
   statement of all three suite mains (same probe-liveness paradigm as
   `threadpool_progress_pass_bound_test.cpp` counter_probe). One shared
   implementation, no per-suite copies, no new xmake target: a separate
   target could stay green-filtered while a broken comparator passed the
   suites, which is exactly the hazard AUD-006 names.
2. Representation correction: `matches_recorded_divergences` compared
   strcmp-sorted `vector<const char*>` with `==`, i.e. by pointer value
   (`harness-matches-before.txt`). Divergence identity is the name text
   (see `truth-table.md`), so the comparison is now element-wise strcmp
   (`harness-matches-after.txt`). The self-check was run against the
   uncorrected comparator first and failed exactly on the
   same-text/distinct-address case (`before-correction-uncorrected-comparator.txt`,
   exit 1).
3. Mutation negative controls M1/M2/M3 (`mutations.md`), each killed by the
   self-check in all three suites, each reverted afterwards.

## Call-site audit after the correction

Every acceptance call site of `matches_recorded_divergences` passes an empty
recorded set: tests/semantic_validation_precedence_test.cpp:52,74,107
(literal `{}`), tests/semantic_backend_conformance_test.cpp:66
(`request_zero_length_divergence_set()`, returns `{}` per
tests/semantic_scenarios.hpp), tests/direct_composition_fault_test.cpp:647
(literal `{}`). For empty recorded sets the verdict is decided by the size
check alone, so no current expectation relied on the pointer comparison; the
correction changes no current suite verdict. It makes a non-empty recorded
set usable at all (previously it would fail unless the compiler pooled the
identical literals with the scenario table's name pointers).

## Commands (focused validation; full matrix not run, per scope)

```
xmake f -c -y --liburing=y
xmake build semantic_validation_precedence_test semantic_backend_conformance_test direct_composition_fault_test
xmake test "semantic_validation_precedence_test/*" "semantic_backend_conformance_test/*" "direct_composition_fault_test/*"
# -> 100% tests passed, 0 failed out of 3, exit 0
```

Direct binary runs recorded in `base-run-three-suites.txt` (exit 0 each;
io_uring was available on this host, so the uring half of the conformance
suite ran). The stderr lines `MISMATCH [selfcheck-wrong] ...` and
`DIVERGENCE SET CHANGED [selfcheck] ...` on a passing run are the
self-check's intentional negative controls exercising the comparators' false
paths; the suite verdict is the exit code.

## Files

- `truth-table.md` — hand-authored expected vs observed per case.
- `harness-matches-before.txt` / `harness-matches-after.txt` — the correction.
- `before-correction-uncorrected-comparator.txt` — self-check catching the
  pointer-equality defect pre-correction (exit 1).
- `base-run-three-suites.txt` — corrected-comparator BASE runs (exit 0).
- `mutations.md`, `M1-run.txt`, `M2-run.txt`, `M3-run.txt`,
  `BASE-after-restore.txt` — mutation kills and cleanup proof.
