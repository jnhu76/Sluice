# Receipt: p2c1-mutation-b — P2C1 mutations 5-7 (WSL2)

Unit: p2c1-mutation-b · Worktree: `/tmp/sluice-val-p2c1-mutation-b` (detached
at `84e1b7d578c2c58a3c46033487d9e1ac7c639b8b`, verified `git rev-parse HEAD`)
· Date: 2026-10-29 · Result: **PASS — 3/3 valid kills** (BASE GREEN → MUTANT
RED → RESTORED GREEN for each; full matrix RESTORED GREEN 58/58 after every
mutant).

Mutants verified (last half of the P2C1 seven):

| id | target (xmake/tests.lua) | define | killer (tests/direct_composition_fault_test.cpp) |
|----|--------------------------|--------|--------------------------------------------------|
| P2C1-5 | `blocking_file_mut_skip_impossible_count` (:112-114) | `SLUICE_COMPOSE_MUTANT_SKIP_IMPOSSIBLE_COUNT` | `impossible_count_stops_immediately` (:340, write twin `write_all_impossible_count_stops_immediately` :365) |
| P2C1-6 | `blocking_file_mut_step_reports_full_count` (:115-117) | `SLUICE_DIRECT_MUTANT_STEP_REPORTS_FULL_COUNT` | `short_reads_report_eof_before_full_with_the_prefix` (:63) |
| P2C1-7 | `blocking_file_mut_step_skips_eintr_retry` (:118-120) | `SLUICE_DIRECT_MUTANT_STEP_SKIPS_EINTR_RETRY` | `primitive_retries_eintr_without_a_completed_count` (:224) |

## Environment

- OS: WSL2, `linux 6.18.54.1-microsoft-standard-WSL2` x86_64, 8 cores
- xmake v3.1.2+HEAD.5d21e9d; gcc (Ubuntu 15.2.0-16ubuntu1) 15.2.0
- Configure: `xmake f -c -m release --liburing=n -y` at 23:56:21 (Ascend SDK
  not found warning expected; `sluice_liburing_capability` check ok, liburing
  disabled). Base build `xmake -y` 23:56:27→23:56:29 (1.7 s, incremental).
- Fault-test binary built: `xmake build -y direct_composition_fault_test`
  (2.0 s) → `build/linux/x86_64/release/direct_composition_fault_test`.

## Command adjustments vs. scout matrix (recorded per instruction)

1. **Worktree substitution**: scout `testCmds` say `cd /tmp/sluice-val-release`;
   that directory belongs to another validation unit. Per unit discipline
   ("不要动别人的 Worktree") every command ran in the dedicated
   `/tmp/sluice-val-p2c1-mutation-b`, same commit `84e1b7d5`, same arguments
   otherwise. This is the only substitution.
2. **Mutant injection mechanism is compile-time, not source edits**: targets
   defined by `direct_mutant_target` (xmake/tests.lua:81-88, instances :112-120)
   compile the killer source + `src/file_resource.cpp` + `src/blocking_file.cpp`
   standalone with `SLUICE_FILE_INTERNAL_TESTING` + the mutant define, and
   register no `add_tests`. Mutated arms verified in source before running:
   - P2C1-5: `#if !defined(SLUICE_COMPOSE_MUTANT_SKIP_IMPOSSIBLE_COUNT)`
     removes the `transferred > total - confirmed_bytes → invalid_state` guard
     (src/blocking_file.cpp:314-319).
   - P2C1-6: shares the arm with `SLUICE_COMPOSE_MUTANT_ADVANCE_BY_REQUESTED`
     (:308-310) but is a separate define/target; non-mutated arm is
     `transferred = step.value()` (:312).
   - P2C1-7: `step_retry_on_eintr` (src/blocking_file.cpp:78-84) returns
     `fn()` directly under the define instead of `detail::retry_on_eintr`;
     it wraps only the two compose step primitives (:277, :288), not the
     direct `read_at`/`write_at` primitives (:98, :119, :138, :156).
   Injection = building the mutant target; production sources are never
   touched, so the RESTORED check is `git status --porcelain -- src/ include/
   tests/ xmake/ docs/` empty + `xmake -y && xmake test -y` green. Both were
   executed after every mutant.
3. **`xmake -y` builds the library only**; test binaries are compiled on
   demand by `xmake test -y` (first full-suite run 23:57:14→23:58:05 ≈ 51 s
   wall incl. compiling all test binaries, xmake-reported test time 4.000 s;
   warm reruns ≈ 4-6 s wall). `xmake -y && xmake test -y` works as written;
   noted as actual xmake behavior.
4. Killer suite runner stops at the first failing case
   (tests/direct_composition_fault_test.cpp:702-706 prints exactly one
   `FAIL: %s` line and `return 1`), so "exactly one FAIL line" is structural.

## BASE GREEN (one run covers all three mutants)

Base source is identical for all three mutants (compile-time targets, no
source edits), so a single base run of the shared killer suite establishes
BASE GREEN for P2C1-5/6/7.

```
$ cd /tmp/sluice-val-p2c1-mutation-b
$ timeout 120 ./build/linux/x86_64/release/direct_composition_fault_test; echo exit=$?
composition read_exact_at/write_all_at: 16 scenarios compared, 5 not drivable, 0 divergences
composition read_exact/write_all: 11 scenarios compared, 10 not drivable (verdicts needing a caller-supplied offset), 0 divergences
all 15 direct composition fault-injection tests passed
exit=0
```

(23:56–23:57 window; 15/15 cases pass, precedence oracle 0 divergences.)

## P2C1-5 — blocking_file_mut_skip_impossible_count

```
$ xmake build -y blocking_file_mut_skip_impossible_count
[ 63%]: <blocking_file_mut_skip_impossible_count> linking.release blocking_file_mut_skip_impossible_count
[100%]: build ok, spent 1.694s
$ timeout 120 ./build/linux/x86_64/release/blocking_file_mut_skip_impossible_count; echo exit=$?
FAIL: impossible_count_stops_immediately
exit=1
```

- MUTANT RED ✓: exit=1, exactly one FAIL line, the named killer
  (`impossible_count_stops_immediately`, fault_test:340). Semantics: without
  the guard the scripted 8-of-4 over-report is absorbed
  (`confirmed_bytes += 8` → `complete_outcome(8)`) instead of stopping with
  `invalid_state` after 1 call.
- Clean check ✓: `git status --porcelain -- src/ include/ tests/ xmake/
  docs/` → empty (rc=0).
- RESTORED GREEN ✓ (23:57:14→23:58:05):
  `xmake -y && xmake test -y` →
  `100% tests passed, 0 test(s) failed out of 58, spent 4.000s`.

## P2C1-6 — blocking_file_mut_step_reports_full_count

```
$ xmake build -y blocking_file_mut_step_reports_full_count
[ 63%]: <blocking_file_mut_step_reports_full_count> linking.release blocking_file_mut_step_reports_full_count
[100%]: build ok, spent 1.707s
$ timeout 120 ./build/linux/x86_64/release/blocking_file_mut_step_reports_full_count; echo exit=$?
FAIL: short_reads_report_eof_before_full_with_the_prefix
exit=1
```

- MUTANT RED ✓: exit=1, exactly one FAIL line, the named killer
  (`short_reads_report_eof_before_full_with_the_prefix`, fault_test:63 — also
  the first entry of the runner table, :675). Semantics: the {{4,0},{2,0},
  {0,0}} short-read script is absorbed as one 16-byte step
  (`transferred = total - confirmed`), reporting `complete(16)` in 1 call
  instead of `eof_before_full, confirmed=6` in 3 calls.
- Clean check ✓: `git status --porcelain -- src/ include/ tests/ xmake/
  docs/` → empty (rc=0).
- RESTORED GREEN ✓ (23:58:30→23:58:36):
  `xmake -y && xmake test -y` →
  `100% tests passed, 0 test(s) failed out of 58, spent 4.015s`.

## P2C1-7 — blocking_file_mut_step_skips_eintr_retry

```
$ xmake build -y blocking_file_mut_step_skips_eintr_retry
[ 63%]: <blocking_file_mut_step_skips_eintr_retry> linking.release blocking_file_mut_step_skips_eintr_retry
[100%]: build ok, spent 1.586s
$ timeout 120 ./build/linux/x86_64/release/blocking_file_mut_step_skips_eintr_retry; echo exit=$?
FAIL: primitive_retries_eintr_without_a_completed_count
exit=1
```

- MUTANT RED ✓: exit=1, exactly one FAIL line, the named killer
  (`primitive_retries_eintr_without_a_completed_count`, fault_test:224).
  Semantics: the {{-1,EINTR},{4,0}} script surfaces EINTR as a step error
  (`primitive_error_outcome`) instead of replaying the interrupted attempt,
  so the second block's `read_exact_at` is not complete. (Cases 1-6 of the
  runner table use no EINTR scripts and still pass, keeping the FAIL line
  single; the direct `read_at` block of this same test is unmutated by
  design — only the compose step is.)
- Clean check ✓: `git status --porcelain -- src/ include/ tests/ xmake/
  docs/` → empty (rc=0).
- RESTORED GREEN ✓ (23:58:53→23:58:58):
  `xmake -y && xmake test -y` →
  `100% tests passed, 0 test(s) failed out of 58, spent 4.002s`.
- End state: worktree HEAD still `84e1b7d578c2c58a3c46033487d9e1ac7c639b8b`,
  `git status --porcelain` over the whole worktree → 0 lines (mutant build
  artifacts live under untracked-ignore `build/`).

## Verdict

| mutant | baseGreen | mutantRed | restoredGreen | valid |
|--------|-----------|-----------|---------------|-------|
| P2C1-5 | true (15/15 fault suite, exit 0) | true (exit 1, single `FAIL: impossible_count_stops_immediately`) | true (58/58) | yes |
| P2C1-6 | true (same base run) | true (exit 1, single `FAIL: short_reads_report_eof_before_full_with_the_prefix`) | true (58/58) | yes |
| P2C1-7 | true (same base run) | true (exit 1, single `FAIL: primitive_retries_eintr_without_a_completed_count`) | true (58/58) | yes |

All three mutations at `84e1b7d5` are semantic kills (wrong composition
outcome / lost EINTR replay), each killed by exactly the scout-named test
with exactly one FAIL line and exit=1. Nothing was weakened to obtain red.
