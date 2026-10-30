# Receipt: p2c1-mutation-a — P2C1 four-mutation test, part A (WSL2)

Unit: p2c1-mutation-a · Worktree: `/tmp/sluice-val-p2c1-mutation-a` (detached
at `84e1b7d578c2c58a3c46033487d9e1ac7c639b8b`, verified `git rev-parse HEAD`
before and after) · Date: 2026-10-29 · Result: **PASS — 4/4 valid kills**
(BASE GREEN 58/58 → MUTANT RED exit=1 with exactly one named FAIL line →
RESTORED GREEN 58/58 for each mutant; tree clean throughout).

## Environment

- OS: WSL2, `linux 6.18.54.1-microsoft-standard-WSL2` x86_64
- xmake v3.1.2+HEAD.5d21e9d; gcc (Ubuntu 15.2.0-16ubuntu1) 15.2.0
- Configure: `xmake f -c -m release --liburing=n -y` in the dedicated
  worktree; `xmake.conf` afterwards shows `liburing = false`
  (`__sluice_liburing_capability = true`, capability check ok; "Ascend SDK
  not found" warning expected).

## Static verification of definitions (before running)

All four mutants are pre-encoded as compile-time defines, not source edits.
Targets defined via `direct_mutant_target` (`xmake/tests.lua:82-89`: binary,
`set_default(false)`, defines `SLUICE_FILE_INTERNAL_TESTING` + the mutant
define, compiles `killer_source` + `src/file_resource.cpp` +
`src/blocking_file.cpp` standalone; never part of the regular test matrix):

- `blocking_file_mut_advance_by_requested` — `xmake/tests.lua:100-102`,
  killer source `tests/direct_composition_fault_test.cpp`, define
  `SLUICE_COMPOSE_MUTANT_ADVANCE_BY_REQUESTED`.
- `blocking_file_mut_retry_zero_progress` — `xmake/tests.lua:103-105`,
  killer source `tests/direct_composition_fault_test.cpp`, define
  `SLUICE_COMPOSE_MUTANT_RETRY_ZERO_PROGRESS`.
- `blocking_file_mut_short_io_retry_zero_progress` — `xmake/tests.lua:106-108`,
  killer source `tests/semantic_short_io_reference_test.cpp`, same define
  `SLUICE_COMPOSE_MUTANT_RETRY_ZERO_PROGRESS` (rules-table leg).
- `blocking_file_mut_failure_accounted` — `xmake/tests.lua:109-111`,
  killer source `tests/direct_composition_fault_test.cpp`, define
  `SLUICE_COMPOSE_MUTANT_FAILURE_ACCOUNTED`.

Mutation sites confirmed at `84e1b7d5`:

- Implementation compose loop `src/blocking_file.cpp`: original
  `transferred = step.value()` (`src/blocking_file.cpp:311-313`); under
  `SLUICE_COMPOSE_MUTANT_ADVANCE_BY_REQUESTED` (or
  `SLUICE_DIRECT_MUTANT_STEP_REPORTS_FULL_COUNT`) it becomes
  `transferred = total - confirmed_bytes` (`src/blocking_file.cpp:308-310`)
  — each step pretends full requested progress.
- Implementation zero-progress stop `src/blocking_file.cpp:323-330`:
  `transferred == 0` → `stalled_outcome(eof_before_full | write_no_progress)`;
  `#if !defined(SLUICE_COMPOSE_MUTANT_RETRY_ZERO_PROGRESS)` removes it —
  zero-progress steps retry until the loop completes.
- Rules-table twin `compose_progress`
  (`include/sluice/detail/file_semantics.hpp`): advance-by-requested arm at
  `:248-250` (arms only under the define); zero-progress stop at
  `:264-270` (removed under `!defined(SLUICE_COMPOSE_MUTANT_RETRY_ZERO_PROGRESS)`).
- `failure_publish_certainty()` `src/blocking_file.cpp:244-250`: original
  `EffectCertainty::unknown`; under `SLUICE_COMPOSE_MUTANT_FAILURE_ACCOUNTED`
  returns `EffectCertainty::accounted` on the primitive_error exit.

Killer assertions confirmed in the suites before running:

- `short_reads_report_eof_before_full_with_the_prefix`
  (`tests/direct_composition_fault_test.cpp:63`).
- `error_after_confirmed_prefix_keeps_the_prefix`
  (`tests/direct_composition_fault_test.cpp:197`), which pins
  `outcome.remaining == EffectCertainty::unknown` on the error exit
  (r4 §4; the pin at `tests/direct_composition_fault_test.cpp:214`).
- `read_exact_accumulates_a_short_prefix_then_reports_eof`
  (`tests/semantic_short_io_reference_test.cpp:59`).

## Command adjustments vs. scout matrix (recorded per instruction)

1. **Worktree substitution**: scout `testCmds` say `cd /tmp/sluice-val-release`;
   that directory belongs to another validation unit. Per unit discipline
   ("不要动别人的 Worktree") every command ran in the dedicated
   `/tmp/sluice-val-p2c1-mutation-a`, same commit `84e1b7d5`, otherwise the
   scout commands verbatim. This substitution applies to the baseline and all
   four mutant cycles; it is the only command change.
2. **`xmake -y` builds only the default target** (`libsluice_core.a`,
   "build ok, spent 3.503s" on the first baseline run); test binaries are
   compiled on demand by `xmake test -y` (first baseline suite run 63.1s wall
   including on-demand test builds; later warm full-suite reruns ~4.0s).
   `xmake -y && xmake test -y` therefore works as written; noted as actual
   xmake behavior.
3. **Injection mechanism**: building the mutant target is the injection —
   no source is edited, so `git status --porcelain -- src/ include/ tests/
   xmake/ docs/` is structurally expected to stay empty after each mutant
   run, and the RESTORED step is that check plus the full default matrix
   rebuilt and rerun green. Both were executed after every mutant.

## cmd-00 — Baseline full matrix (BASE GREEN)

```
$ cd /tmp/sluice-val-p2c1-mutation-a
$ git rev-parse HEAD
84e1b7d578c2c58a3c46033487d9e1ac7c639b8b
$ time xmake f -c -m release --liburing=n -y
  checking for platform ... linux (x86_64)
  checking for sluice_liburing_capability ... ok
  warning: Ascend SDK not found
  (2.092s wall)
$ time xmake -y
  <sluice_core> compiling.release src/file_resource.cpp
  <sluice_core> compiling.release src/blocking_file.cpp
  <sluice_core> archiving.release libsluice_core.a
  build ok, spent 3.503s   (4.603s wall)
$ time xmake test -y
  ... (per-target passed lines) ...
  100% tests passed, 0 test(s) failed out of 58, spent 3.998s
  (1:03.14 wall incl. on-demand build of all test binaries)
```

58/58 — matches the master static enumeration of 58 `add_tests` targets at
`84e1b7d5` (receipts/cmd-01). Baseline GREEN for all four killer cycles.

## cmd-01 — P2C1-1 `blocking_file_mut_advance_by_requested`

```
$ xmake build -y blocking_file_mut_advance_by_requested
  compiling.release src/blocking_file.cpp
  linking.release blocking_file_mut_advance_by_requested
  build ok, spent 1.622s
$ timeout 120 ./build/linux/x86_64/release/blocking_file_mut_advance_by_requested > out 2>&1; echo exit=$?
exit=1
$ cat out
FAIL: short_reads_report_eof_before_full_with_the_prefix
$ grep -c '^FAIL' out
1
```

MUTANT RED — valid kill: exit=1, exactly one FAIL line, exact match of the
named killer (`tests/direct_composition_fault_test.cpp:63`). The mutant makes
every step report full requested progress, so the short-read/eof prefix test
fails semantically; no crash, no timeout (well under 120s).

```
$ git status --porcelain -- src/ include/ tests/ xmake/ docs/    # empty
$ xmake -y      # build ok, spent 0.027s
$ xmake test -y
  100% tests passed, 0 test(s) failed out of 58, spent 4.018s   (5.373s wall)
```

RESTORED GREEN 58/58, tree clean.

## cmd-02 — P2C1-2 `blocking_file_mut_retry_zero_progress` (fault leg)

```
$ xmake build -y blocking_file_mut_retry_zero_progress
  build ok, spent 1.546s
$ timeout 120 ./build/linux/x86_64/release/blocking_file_mut_retry_zero_progress > out 2>&1; echo exit=$?
exit=1
$ cat out
FAIL: short_reads_report_eof_before_full_with_the_prefix
$ grep -c '^FAIL' out
1
```

MUTANT RED — valid kill: exit=1, exactly one FAIL line, the named killer.
This is the implementation-leg zero-progress-stop removal; the run terminated
on its own well before the 120s timeout (exit=1, not 124), so the kill counts
per the criterion.

```
$ git status --porcelain -- src/ include/ tests/ xmake/ docs/    # empty
$ xmake -y      # build ok, spent 0.024s
$ xmake test -y
  100% tests passed, 0 test(s) failed out of 58, spent 4.008s   (5.368s wall)
```

RESTORED GREEN 58/58, tree clean.

## cmd-03 — P2C1-3 `blocking_file_mut_short_io_retry_zero_progress` (rules leg)

```
$ xmake build -y blocking_file_mut_short_io_retry_zero_progress
  build ok, spent 1.908s
$ timeout 120 ./build/linux/x86_64/release/blocking_file_mut_short_io_retry_zero_progress > out 2>&1; echo exit=$?
exit=1
$ cat out
FAIL: read_exact_accumulates_a_short_prefix_then_reports_eof
$ grep -c '^FAIL' out
1
```

MUTANT RED — valid kill: exit=1, exactly one FAIL line, exact match of the
named killer (`tests/semantic_short_io_reference_test.cpp:59`). This mutates
the same `SLUICE_COMPOSE_MUTANT_RETRY_ZERO_PROGRESS` arm but in the SEM-05
rules table (`include/sluice/detail/file_semantics.hpp:264-270`), exercised
by the short-io reference oracle instead of the fault suite.

Handoff §5 rules-stay-green control: the rule table is a non-authoritative
oracle and must never become a second authority. Observed: the rules-table
mutation is killed by its own oracle suite (above), while the production
implementation stays green — the restored full matrix below (58/58, rules
table at its committed content) never carries the define, and the mutant
define exists only inside the `direct_mutant_target` harness
(`xmake/tests.lua:82-89`), which is `set_default(false)` and excluded from
the regular matrix.

```
$ git status --porcelain -- src/ include/ tests/ xmake/ docs/    # empty
$ xmake -y      # build ok, spent 0.087s
$ xmake test -y
  100% tests passed, 0 test(s) failed out of 58, spent 4.150s   (7.841s wall)
```

RESTORED GREEN 58/58, tree clean.

## cmd-04 — P2C1-4 `blocking_file_mut_failure_accounted`

```
$ xmake build -y blocking_file_mut_failure_accounted
  build ok, spent 1.63s
$ timeout 120 ./build/linux/x86_64/release/blocking_file_mut_failure_accounted > out 2>&1; echo exit=$?
exit=1
$ cat out
FAIL: error_after_confirmed_prefix_keeps_the_prefix
$ grep -c '^FAIL' out
1
```

MUTANT RED — valid kill: exit=1, exactly one FAIL line, the named killer
(`tests/direct_composition_fault_test.cpp:197`), which pins
`remaining == EffectCertainty::unknown` on the error exit (r4 §4; the pin at
`tests/direct_composition_fault_test.cpp:214`); the mutant returning
`accounted` flips exactly that pin.

```
$ git status --porcelain -- src/ include/ tests/ xmake/ docs/    # empty
$ git rev-parse HEAD
84e1b7d578c2c58a3c46033487d9e1ac7c639b8b
$ xmake -y      # build ok, spent 0.031s
$ xmake test -y
  100% tests passed, 0 test(s) failed out of 58, spent 4.024s   (5.576s wall)
```

RESTORED GREEN 58/58, tree clean, HEAD unchanged.

## Verdict

| Mutant | Target | Leg | Killer (exact FAIL line) | BASE | MUTANT | RESTORED | Valid |
|---|---|---|---|---|---|---|---|
| P2C1-1 | `blocking_file_mut_advance_by_requested` | implementation | `FAIL: short_reads_report_eof_before_full_with_the_prefix` | GREEN 58/58 | RED (exit=1, 1 FAIL line, exact match) | GREEN 58/58 | yes |
| P2C1-2 | `blocking_file_mut_retry_zero_progress` | implementation (fault leg) | `FAIL: short_reads_report_eof_before_full_with_the_prefix` | GREEN 58/58 | RED (exit=1, 1 FAIL line, exact match; not a timeout) | GREEN 58/58 | yes |
| P2C1-3 | `blocking_file_mut_short_io_retry_zero_progress` | rules table (SEM-05 oracle leg) | `FAIL: read_exact_accumulates_a_short_prefix_then_reports_eof` | GREEN 58/58 | RED (exit=1, 1 FAIL line, exact match) | GREEN 58/58 | yes |
| P2C1-4 | `blocking_file_mut_failure_accounted` | implementation | `FAIL: error_after_confirmed_prefix_keeps_the_prefix` | GREEN 58/58 | RED (exit=1, 1 FAIL line, exact match) | GREEN 58/58 | yes |

All four P2C1 part-A mutants are killed by their designated semantic killers
under the WSL2 release (no-liburing) configuration at `84e1b7d5`. Every kill
is a semantic test failure (exit=1, exactly one FAIL line matching the named
killer); no assertion loosening, sleeps or retry inflation were involved; no
source files were modified at any point; the rules-table mutant (P2C1-3) was
killed while the production implementation stayed 58/58 green, so the rule
table did not become a second authority. P2C1-5..7 (part B: skip_impossible_count,
step_reports_full_count, step_skips_eintr_retry) belong to the sibling unit
`p2c1-mutation-b` and were not run here.
