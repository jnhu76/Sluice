# AUD-006 mutation negative controls (M1/M2/M3)

One-shot mutations applied to `tests/semantic_oracle_harness.hpp` in the
worktree, never committed; the applicator lived in
`/tmp/sluice-643-aud006-mutations/apply_mutation.py` and each mutant was
reverted from `/tmp/sluice-643-aud006-mutations/harness-base-corrected.hpp`
(the pre-mutation corrected file) before the next run. BASE = the corrected
harness at the commit under review; MUTANT = BASE + exactly one comparator
mutation. All runs used the binaries built by
`xmake build semantic_validation_precedence_test semantic_backend_conformance_test direct_composition_fault_test`
in the worktree configured with `xmake f -c -y --liburing=y`.

## M1 — agree() always returns true

Mutation: all three return statements of `agree()` replaced by `return true;`
(the rejection-code comparison, the observed-rejection false branch, and the
no-op consult).

- BASE: three suites exit 0 (`base-run-three-suites.txt`).
- MUTANT build: exit 0. MUTANT runs (`M1-run.txt`): all three suites exit 1.
- Killing assertion (first to fire, in every suite):
  `SELF-CHECK FAIL [agree] B_success_vs_rejection: expected disagreement`
  followed by `FAIL: the semantic-oracle comparison harness failed its
  self-check`; e.g. `./build/linux/x86_64/release/semantic_validation_precedence_test`
  -> exit 1.
- Meaning: without the self-check this mutation leaves all three suites green
  (run_path never records a divergence, the recorded sets are empty), which is
  the AUD-006 false-positive-GREEN hazard.

## M2 — matches_recorded_divergences() always returns true

Mutation: the final `return false;` of `matches_recorded_divergences()`
replaced by `return true;` (every path returns true).

- BASE: three suites exit 0.
- MUTANT build: exit 0. MUTANT runs (`M2-run.txt`): all three suites exit 1.
- Killing assertion (first to fire, in every suite):
  `SELF-CHECK FAIL [matches_recorded_divergences] G_extra_observed: expected
  mismatch`, exit 1.
- Meaning: on a healthy tree every acceptance call site records an empty set,
  so nothing but the self-check can ever observe this comparator's false
  branch.

## M3 — run_path() always reports zero mismatches

Mutation: the `++mismatches;` increment removed (mismatch names are still
recorded and MISMATCH lines still printed, but the returned count is always
0). The literal `return mismatches; -> return 0;` form was tried first and is
pre-killed by the compiler: `-Werror=unused-but-set-variable` on
`mismatches` (gcc, this worktree's flags) fails the mutant build, so no
runtime kill exists for that spelling; the increment-removal spelling is
behaviorally identical and compiles.

- BASE: three suites exit 0.
- MUTANT build: exit 0 (`M3-build.txt` not retained; build output ended
  `build ok`). MUTANT runs (`M3-run.txt`): all three suites exit 1.
- Killing assertion (first to fire, in every suite):
  `SELF-CHECK FAIL [run_path] a known wrong observation produced 0
  mismatches and 1 names`, exit 1.

## Cleanup

After M3 the corrected harness was restored from the /tmp base copy, the three
targets rebuilt (`RESTORE_BUILD_EXIT=0`) and all three suites re-run green
(`BASE-after-restore.txt`: exit 0 each). `git status` in the commit shows only
the four intended test files plus this evidence directory; no mutant was
committed. The /tmp mutation lab (applicator, restore copy, build logs) was
left outside the repository and is disposable.
