# Receipt: c1-mutation — C1 three-mutation test (WSL2)

Unit: c1-mutation · Worktree: `/tmp/sluice-val-c1-mutation` (detached at
`84e1b7d578c2c58a3c46033487d9e1ac7c639b8b`, verified `git rev-parse HEAD`)
· Date: 2026-10-29 · Result: **PASS — 3/3 valid kills** (BASE GREEN → MUTANT
RED → RESTORED GREEN for each; final full matrix RESTORED GREEN 58/58).

## Environment

- OS: WSL2, `linux 6.18.54.1-microsoft-standard-WSL2` x86_64, 8 cores
- xmake v3.1.2+HEAD.5d21e9d; gcc (Ubuntu 15.2.0-16ubuntu1) 15.2.0
- Configure: `xmake f -c -m release --liburing=n -y` (Ascend SDK not found
  warning expected; `sluice_liburing_capability` check ok, liburing disabled)

## Command adjustments vs. scout matrix (recorded per instruction)

1. **Worktree substitution**: scout `testCmds` say `cd /tmp/sluice-val-release`;
   that directory belongs to another validation unit. Per unit discipline
   ("不要动别人的 Worktree") all commands ran in the dedicated
   `/tmp/sluice-val-c1-mutation`, same commit `84e1b7d5`, same arguments
   otherwise. This is the only substitution.
2. **`xmake -y` builds only `libsluice_core.a`**: observed
   `build/linux/x86_64/release/` contained only `libsluice_core.a` after
   `xmake -y` (2.2 s). Test binaries are compiled on demand by `xmake test -y`
   (first baseline run: 62.6 s wall incl. compiling all test binaries; later
   full-suite reruns ~4.0 s warm). `xmake -y && xmake test -y` therefore works
   as written; noted as actual xmake behavior.
3. **Mutant injection mechanism**: at `84e1b7d5` the three mutants are
   pre-encoded as compile-time defines, not source edits. Targets in
   `xmake/tests.lua:90-99` (`direct_mutant_target`, lines 81-88) compile the
   killer suite + `src/file_resource.cpp` + `src/blocking_file.cpp` standalone
   with `SLUICE_FILE_INTERNAL_TESTING` + the mutant define. Mutant guards in
   `include/sluice/detail/file_semantics.hpp`:
   - C1-1 `SLUICE_SEM_MUTANT_ADMIT_INVALID_RANGE`: `return execute` before the
     `range_is_valid` / `kMaxNativeTransfer` checks (precheck_data_op).
   - C1-2 `SLUICE_SEM_MUTANT_EMPTY_BEFORE_CLOSED`: hoists `length == 0 →
     complete_empty` before the closed and access checks (precheck_data_op).
   - C1-3 `SLUICE_SEM_MUTANT_ADMIT_UNREPRESENTABLE_SIZE`: `#if !defined(...)`
     removes the `size_is_representable` check in `precheck_resize`.
   Injection = building the mutant target; consequently `git checkout --` is a
   no-op and the RESTORED check is `git status --porcelain -- src/ include/
   tests/ xmake/ docs/` empty + the normal killer target rebuilt and rerun
   green. Both were executed after every mutant.
4. Killer names/lines verified in the suites before running:
   `read_with_illegal_offset_reports_invalid_argument`
   (tests/blocking_file_read_test.cpp:122),
   `sequential_read_on_write_only_file_rejected_upfront`
   (tests/blocking_file_sequential_test.cpp:392),
   `sequential_zero_length_read_after_close_reports_invalid_state` (:418),
   `resize_overflow_reports_invalid_argument`
   (tests/blocking_file_state_test.cpp:320). Runner contract: prints
   `FAIL: <name>` to stderr and returns 1 on first failure
   (tests/blocking_file_read_test.cpp:192-197), so "exactly one FAIL line" is
   structural.

## cmd-01 — Baseline full matrix (BASE GREEN, all three killers)

```
$ cd /tmp/sluice-val-c1-mutation
$ git status --porcelain            # empty before start
$ xmake f -c -m release --liburing=n -y
  checking for platform ... linux (x86_64)
  checking for sluice_liburing_capability ... ok
  warning: Ascend SDK not found
$ xmake -y
  build ok, spent 1.762s
$ xmake test -y
  100% tests passed, 0 test(s) failed out of 58, spent 4.217s
  (wall 1:02.56 incl. on-demand build of all test binaries)
```

58/58 — matches the master static enumeration of 58 `add_tests` targets at
`84e1b7d5`. Direct killer-suite runs on the clean tree:

```
$ ./build/linux/x86_64/release/blocking_file_read_test        → all 6 blocking file read tests passed, exit=0
$ ./build/linux/x86_64/release/blocking_file_sequential_test  → all 13 blocking file sequential tests passed, exit=0
$ ./build/linux/x86_64/release/blocking_file_state_test       → all 14 blocking file state tests passed, exit=0
```

## cmd-02 — C1-1 `blocking_file_mut_admit_invalid_range`

```
$ xmake build -y blocking_file_mut_admit_invalid_range   # build ok, spent 2.214s
$ timeout 120 ./build/linux/x86_64/release/blocking_file_mut_admit_invalid_range 2>&1; echo exit=$?
FAIL: read_with_illegal_offset_reports_invalid_argument
exit=1
```

Exactly one FAIL line, the named killer: `read_at(file, UINT64_MAX, 1)` is no
longer rejected as invalid_argument (mutant admits the unvalidated range).
MUTANT RED — valid kill (semantic test failure, exit=1, no crash/signal).
RESTORED: `git status --porcelain -- src/ include/ tests/ xmake/ docs/` empty;
`xmake build -y blocking_file_read_test` (0.05s) → rerun → `all 6 blocking
file read tests passed`, exit=0.

## cmd-03 — C1-2 `blocking_file_mut_empty_before_closed`

```
$ xmake build -y blocking_file_mut_empty_before_closed   # build ok, spent 1.38s
$ timeout 120 ./build/linux/x86_64/release/blocking_file_mut_empty_before_closed 2>&1; echo exit=$?
FAIL: sequential_read_on_write_only_file_rejected_upfront
exit=1
```

Exactly one FAIL line, the named killer: zero-length read on a write-only file
succeeds via the hoisted `complete_empty` before the access check instead of
being rejected invalid_argument upfront. MUTANT RED — valid kill (semantic
test failure, exit=1, no crash/signal). RESTORED: tree check empty;
`xmake build -y blocking_file_sequential_test` (0.028s) → rerun → `all 13
blocking file sequential tests passed`, exit=0.

## cmd-04 — C1-3 `blocking_file_mut_admit_unrepresentable_size`

```
$ xmake build -y blocking_file_mut_admit_unrepresentable_size  # build ok, spent 0.867s
$ timeout 120 ./build/linux/x86_64/release/blocking_file_mut_admit_unrepresentable_size 2>&1; echo exit=$?
FAIL: resize_overflow_reports_invalid_argument
exit=1
```

Exactly one FAIL line, the named killer: `resize(UINT64_MAX)` is admitted and
reaches ftruncate with the wrapped negative offset → EINVAL → backend_error
instead of invalid_argument. MUTANT RED — valid kill (semantic test failure,
exit=1, no crash/signal). RESTORED: tree check empty;
`xmake build -y blocking_file_state_test` (0.04s) → rerun → `all 14 blocking
file state tests passed`, exit=0.

## cmd-05 — Final RESTORED full matrix

```
$ cd /tmp/sluice-val-c1-mutation
$ xmake -y      # build ok, spent 0.021s
$ xmake test -y
  100% tests passed, 0 test(s) failed out of 58, spent 4.013s
$ git status --porcelain -- src/ include/ tests/ xmake/ docs/   # empty
$ git rev-parse HEAD   # 84e1b7d578c2c58a3c46033487d9e1ac7c639b8b
```

## Verdict

| Mutant | Target | Killer | BASE | MUTANT | RESTORED | Valid |
|---|---|---|---|---|---|---|
| C1-1 | `blocking_file_mut_admit_invalid_range` | `read_with_illegal_offset_reports_invalid_argument` | GREEN (58/58) | RED (exit=1, 1 FAIL line, exact match) | GREEN (suite 6/6 + final 58/58) | yes |
| C1-2 | `blocking_file_mut_empty_before_closed` | `sequential_read_on_write_only_file_rejected_upfront` | GREEN (58/58) | RED (exit=1, 1 FAIL line, exact match) | GREEN (suite 13/13 + final 58/58) | yes |
| C1-3 | `blocking_file_mut_admit_unrepresentable_size` | `resize_overflow_reports_invalid_argument` | GREEN (58/58) | RED (exit=1, 1 FAIL line, exact match) | GREEN (suite 14/14 + final 58/58) | yes |

All three C1 mutants are killed by their designated semantic test suites under
the WSL2 release (no-liburing) configuration; no assertion loosening, sleeps or
retries were involved; no source files were modified at any point.
