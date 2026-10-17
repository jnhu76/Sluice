# Test and mutation-target retention ledger

The hygiene round renamed labels and removed no registered test, no oracle
and no mutation target. Counts at base and head:

| Measure | Base `9845cee1` | Head |
|---|---|---|
| xmake test targets (binaries incl. shim) | 134 | 134 |
| Registered tests (`add_tests`) | 74 (60 default + 14 liburing-gated) | 74 |
| Mutation targets (manual builds) | 59 | 59 |
| Mutation define families | 12 | 12 (spellings updated for 9 stage-prefixed families) |

## Retention decisions

- Every `tests/*.cpp` oracle suite is kept; the duplicate-looking pairs
  (ThreadPool vs uring progress/external-loop suites, direct vs request-path
  semantic suites, engine-layer vs CLI-layer tail oracles) verify the same
  protocol property on different backends or layers and are retained by
  design.
- Removed non-target fixtures: `tests/support/direct_pread_shim.c` (compiled
  by no target; referenced only by frozen evidence run-matrix scripts) and
  `tests/test_grep_matrix_runner.py` (unit test of a frozen historical
  evidence runner, wired into no runner).
- Renamed-and-kept with a documented entry:
  `tests/cli_preflight_rejection_oracle.py` — the only witness of the
  failure mode "invalid/removed CLI options are rejected before any
  target-file open" (strace-observed); entry documented in the
  sluice-tail and sluice-copy READMEs.
- Kept manual oracles with existing documented entries:
  `app_direct_cli_oracle.py`, `grep_cli_oracle.py` + `grep_trace_oracle.py`
  + `tests/test_grep_trace_oracle.py` (its self-test), `hash_cli_oracle.py`.
- `tests/package/f4_retirement/` (16 probes + manifest + verifier unit tests)
  removed: nothing in the active tree compiled any of it; the live 11-header
  boundary negative probes are owned by `scripts/verify_v2_package.py`
  (ledger G9). The pre-v2 508/510 surfaces the probes negated are physically
  absent from `include/`, so their property is trivially maintained.

## Mutation kills re-verified at the hygiene head

All 49 stage-renamed mutant targets were rebuilt and executed; each must
EXIT NONZERO (its oracle detects the injected defect). Result: 49/49 killed
(48 in the batch runs + `progress_source_uring_mut_close_admission_always_signals`
and `request_scope_mut_destructor_skips_settle` re-run after fixing two
enumeration typos in the audit list — the target names are
`..._uring_mut_...` and `..._skips_settle`). Command shape:

```sh
xmake -y <mutant-target> && ! ./build/linux/x86_64/debug/<mutant-target>
```

Families covered: `request_core_mut_*` (5), `progress_source_mut_*` (2),
`shutdown_mut_m*` (9), `public_request_mut_*` (6), `request_scope_mut_*` (6),
`backend_contract_conformance_mut_*` (10), `uring_core_integration_mut_*` (8),
`uring_progress_race_mut_*` (4 per target set), `uring_external_loop_mut_*`.
The shutdown M6/M7/M8 kills additionally rest on the ShutdownCore TLA+
mirrors in `scripts/verify_tla.sh` (unchanged stage), which this round's
fresh gate run re-confirms.
