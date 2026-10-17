# Test and mutation-target retention ledger

The hygiene round renamed labels and removed no registered test, no oracle
and no mutation target. Counts at base and head:

| Measure | Base `9845cee1` | Head |
|---|---|---|
| xmake test targets (binaries incl. shim) | 134 | 134 |
| Registered tests (`add_tests`) | 74 (56 default + 18 liburing-gated) | 74 |
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

The mutation set is 59 manual targets over 53 injection defines, identical
in size at base and head. The rename touched 18 target names and 49 of the
53 defines; 4 defines are byte-identical at base and head
(`SLUICE_ERR01_MUTANT_PHYSICAL_ERRNO_UNMAPPED`,
`SLUICE_ERR01_MUTANT_ADMISSION_CLOSE_REPORTS_CAPACITY`,
`SLUICE_LIFE_MUTANT_MOVE_CLOSES_NATIVE`,
`SLUICE_THREAD01_MUTANT_SUBMIT_UNSERIALIZED`) and each backs two targets
(the `request_error_domain_*`, `file_move_borrow_*` and
`submit_concurrency_*` suites build two targets per define), so 8 targets
are entirely unchanged by the round.

All 59 targets were rebuilt and executed at the review-fix head (debug,
liburing=y); each must EXIT NONZERO — its oracle detects the injected
defect. Result: 59/59 killed. Per-target results are committed as
`mutant-kill-matrix.txt` in this directory (one `KILLED` /
`BUILD_FAIL` / `NOT_KILLED` line per target). Command shape:

```sh
xmake f -m debug -y --liburing=y
xmake build -y <mutant-target> && xmake run -y <mutant-target>
```

The shutdown M6/M7/M8 kills additionally rest on the ShutdownCore TLA+
mirrors in `scripts/verify_tla.sh`, which this round's fresh gate run
re-confirms.
