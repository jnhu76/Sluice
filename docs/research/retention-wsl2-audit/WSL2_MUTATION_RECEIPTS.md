# WSL2 Mutation Receipts (retention-wsl2-audit)

Per-mutant BASE GREEN → MUTANT RED → RESTORED GREEN record for the WSL2
retention audit round, consolidated from the three per-unit receipts. All ten
mutants ran at `84e1b7d578c2c58a3c46033487d9e1ac7c639b8b` (= `origin/master`)
under `xmake f -c -m release --liburing=n -y`, gcc 15.2.0, in dedicated
detached `/tmp/sluice-val-{c1-mutation,p2c1-mutation-a,p2c1-mutation-b}`
worktrees. Mutant and killer definitions are frozen in
`WSL2_COMMAND_MATRIX.md` §3–§4; targets are `direct_mutant_target` instances
(`xmake/tests.lua:81-120`), each `set_default(false)` with no `add_tests`, so
no mutant ever enters the regular matrix.

**Verdict: 10/10 valid kills — C1 3/3, P2C1 7/7.** Every kill is a semantic
test failure (exit=1 with exactly one `FAIL: <killer>` line naming the
designated killer); no crash, signal, build failure or timeout was accepted as
a kill (frozen rule, `research/direct-beyond-parity/p2-scripts/P2C1-mutants.sh:60-72`).
No assertion was loosened; no sleep or retry inflation was used to obtain red.

## Injection and restoration mechanics

- Mutants are **pre-encoded compile-time defines**, not source edits:
  injection = building the mutant target (killer source + `src/file_resource.cpp`
  + `src/blocking_file.cpp` standalone, with `SLUICE_FILE_INTERNAL_TESTING` +
  the mutant define). Production sources are never touched.
- RESTORED GREEN after every mutant = `git status --porcelain -- src/ include/
  tests/ xmake/ docs/` empty **plus** the normal killer suite rebuilt and
  rerun green **plus** the full default matrix `xmake -y && xmake test -y`
  → 58/58. Executed after every mutant in all three units; end-state HEAD
  re-verified `84e1b7d5…` each time.
- The killer-suite runners stop at the first failing case and print exactly
  one `FAIL: %s` line with `return 1`
  (`tests/blocking_file_read_test.cpp:192-197`,
  `tests/direct_composition_fault_test.cpp:702-706`), so "exactly one FAIL
  line" is structural, not observed by luck.

## Consolidated verdict table

| id | target (`xmake/tests.lua`) | mutant define | leg | killer (exact FAIL line) | BASE GREEN | MUTANT RED | RESTORED GREEN | valid | receipt |
|----|----------------------------|---------------|-----|--------------------------|------------|------------|----------------|-------|---------|
| C1-1 | `blocking_file_mut_admit_invalid_range` (:91-93) | `SLUICE_SEM_MUTANT_ADMIT_INVALID_RANGE` | admission (`precheck_data_op` range check bypassed) | `read_with_illegal_offset_reports_invalid_argument` (`tests/blocking_file_read_test.cpp:122`) | 58/58 + killer suites 6/6, 13/13, 14/14 direct-run exit=0 | exit=1, exactly one FAIL line, exact match | suite 6/6 rerun + final full matrix 58/58, tree clean | **yes** | [receipts/c1-mutation.md](receipts/c1-mutation.md) cmd-02 |
| C1-2 | `blocking_file_mut_empty_before_closed` (:94-96) | `SLUICE_SEM_MUTANT_EMPTY_BEFORE_CLOSED` | admission (`complete_empty` hoisted above closed/access checks) | `sequential_read_on_write_only_file_rejected_upfront` (`tests/blocking_file_sequential_test.cpp:392`) | same base run | exit=1, one FAIL line, exact match | suite 13/13 rerun + final 58/58, tree clean | **yes** | [receipts/c1-mutation.md](receipts/c1-mutation.md) cmd-03 |
| C1-3 | `blocking_file_mut_admit_unrepresentable_size` (:97-99) | `SLUICE_SEM_MUTANT_ADMIT_UNREPRESENTABLE_SIZE` | admission (`size_is_representable` removed from `precheck_resize`) | `resize_overflow_reports_invalid_argument` (`tests/blocking_file_state_test.cpp:320`) | same base run | exit=1, one FAIL line, exact match | suite 14/14 rerun + final 58/58, tree clean | **yes** | [receipts/c1-mutation.md](receipts/c1-mutation.md) cmd-04 |
| P2C1-1 | `blocking_file_mut_advance_by_requested` (:100-102) | `SLUICE_COMPOSE_MUTANT_ADVANCE_BY_REQUESTED` | implementation compose loop (`transferred = total - confirmed_bytes`) | `short_reads_report_eof_before_full_with_the_prefix` (`tests/direct_composition_fault_test.cpp:63`) | 58/58 full matrix (spent 3.998s) | exit=1, one FAIL line, exact match | 58/58 (spent 4.018s), tree clean | **yes** | [receipts/p2c1-mutation-a.md](receipts/p2c1-mutation-a.md) cmd-01 |
| P2C1-2 | `blocking_file_mut_retry_zero_progress` (:103-105) | `SLUICE_COMPOSE_MUTANT_RETRY_ZERO_PROGRESS` | implementation compose loop (zero-progress stop removed) | `short_reads_report_eof_before_full_with_the_prefix` (`tests/direct_composition_fault_test.cpp:63`) | same base run | exit=1, one FAIL line, exact match; **self-terminated, not a timeout** (exit=1, not 124) | 58/58 (spent 4.008s), tree clean | **yes** | [receipts/p2c1-mutation-a.md](receipts/p2c1-mutation-a.md) cmd-02 |
| P2C1-3 | `blocking_file_mut_short_io_retry_zero_progress` (:106-108) | `SLUICE_COMPOSE_MUTANT_RETRY_ZERO_PROGRESS` | rules table (`compose_progress`, non-authoritative SEM-05 oracle) | `read_exact_accumulates_a_short_prefix_then_reports_eof` (`tests/semantic_short_io_reference_test.cpp:59`) | same base run | exit=1, one FAIL line, exact match | 58/58 (spent 4.150s), tree clean; rules-stay-green control holds (see below) | **yes** | [receipts/p2c1-mutation-a.md](receipts/p2c1-mutation-a.md) cmd-03 |
| P2C1-4 | `blocking_file_mut_failure_accounted` (:109-111) | `SLUICE_COMPOSE_MUTANT_FAILURE_ACCOUNTED` | implementation (`failure_publish_certainty()` returns `accounted` instead of `unknown`) | `error_after_confirmed_prefix_keeps_the_prefix` (`tests/direct_composition_fault_test.cpp:197`; pin `remaining == EffectCertainty::unknown` at :214) | same base run | exit=1, one FAIL line, exact match | 58/58 (spent 4.024s), tree clean, HEAD unchanged | **yes** | [receipts/p2c1-mutation-a.md](receipts/p2c1-mutation-a.md) cmd-04 |
| P2C1-5 | `blocking_file_mut_skip_impossible_count` (:112-114) | `SLUICE_COMPOSE_MUTANT_SKIP_IMPOSSIBLE_COUNT` | implementation compose loop (impossible-count guard removed) | `impossible_count_stops_immediately` (`tests/direct_composition_fault_test.cpp:340`; write twin `write_all_impossible_count_stops_immediately` :365) | shared base run: fault suite 15/15, exit=0 (`0 divergences` both precedence oracles) | exit=1, one FAIL line, exact match | 58/58 (spent 4.000s), tree clean | **yes** | [receipts/p2c1-mutation-b.md](receipts/p2c1-mutation-b.md) |
| P2C1-6 | `blocking_file_mut_step_reports_full_count` (:115-117) | `SLUICE_DIRECT_MUTANT_STEP_REPORTS_FULL_COUNT` | implementation compose loop (shares the arm with P2C1-1, separate define/target) | `short_reads_report_eof_before_full_with_the_prefix` (`tests/direct_composition_fault_test.cpp:63`) | same shared base run | exit=1, one FAIL line, exact match | 58/58 (spent 4.015s), tree clean | **yes** | [receipts/p2c1-mutation-b.md](receipts/p2c1-mutation-b.md) |
| P2C1-7 | `blocking_file_mut_step_skips_eintr_retry` (:118-120) | `SLUICE_DIRECT_MUTANT_STEP_SKIPS_EINTR_RETRY` | implementation (`step_retry_on_eintr` returns `fn()` directly — no EINTR replay; wraps only the two compose-step primitives :277, :288) | `primitive_retries_eintr_without_a_completed_count` (`tests/direct_composition_fault_test.cpp:224`) | same shared base run | exit=1, one FAIL line, exact match (cases 1-6 without EINTR scripts still pass, keeping the FAIL line single) | 58/58 (spent 4.002s); whole-worktree `git status --porcelain` = 0 lines, HEAD unchanged | **yes** | [receipts/p2c1-mutation-b.md](receipts/p2c1-mutation-b.md) |

## Baseline (BASE GREEN) evidence

- **c1-mutation** (covers C1-1/2/3): full matrix 58/58 on the clean tree
  (`100% tests passed, 0 test(s) failed out of 58, spent 4.217s`), then the
  three killer suites direct-run green: `blocking_file_read_test` 6/6,
  `blocking_file_sequential_test` 13/13, `blocking_file_state_test` 14/14,
  all exit=0.
- **p2c1-mutation-a** (covers P2C1-1/2/3/4): full matrix 58/58 on the clean
  tree (`… out of 58, spent 3.998s`); `git rev-parse HEAD` verified
  `84e1b7d5…` before the run.
- **p2c1-mutation-b** (covers P2C1-5/6/7): one shared base run of the killer
  suite — base source is identical for all three (compile-time targets, no
  source edits): `all 15 direct composition fault-injection tests passed`,
  exit=0, with `16 scenarios … 0 divergences` and `11 scenarios … 0
  divergences` precedence-oracle lines.

## Observed MUTANT RED outputs (verbatim FAIL lines)

```
C1-1    FAIL: read_with_illegal_offset_reports_invalid_argument            exit=1
C1-2    FAIL: sequential_read_on_write_only_file_rejected_upfront          exit=1
C1-3    FAIL: resize_overflow_reports_invalid_argument                     exit=1
P2C1-1  FAIL: short_reads_report_eof_before_full_with_the_prefix           exit=1
P2C1-2  FAIL: short_reads_report_eof_before_full_with_the_prefix           exit=1   (self-terminated, not timeout)
P2C1-3  FAIL: read_exact_accumulates_a_short_prefix_then_reports_eof       exit=1
P2C1-4  FAIL: error_after_confirmed_prefix_keeps_the_prefix                exit=1
P2C1-5  FAIL: impossible_count_stops_immediately                           exit=1
P2C1-6  FAIL: short_reads_report_eof_before_full_with_the_prefix           exit=1
P2C1-7  FAIL: primitive_retries_eintr_without_a_completed_count            exit=1
```

Each mutant's `grep -c '^FAIL'` on its captured output = 1 (exact-match
single FAIL line); every run finished well under the 120 s timeout.

## Rules-stay-green control (P2C1-3)

The SEM-05 rules table (`compose_progress`, `include/sluice/detail/file_semantics.hpp:243-281`)
is a non-authoritative oracle and must never become a second authority.
Observed: mutating the rules-table leg is killed by its own oracle suite
(`semantic_short_io_reference_test`), while the production implementation
stays green — the restored full matrices (58/58) never carry the mutant
define, and the define exists only inside the `direct_mutant_target` harness
(`xmake/tests.lua:82-89`), which is excluded from the regular matrix.
(`receipts/p2c1-mutation-a.md` cmd-03.)

## RESTORED GREEN closure

- c1-mutation final check (cmd-05): `xmake -y` (no-op) + `xmake test -y` →
  58/58; `git status --porcelain -- src/ include/ tests/ xmake/ docs/` empty;
  HEAD `84e1b7d5…` unchanged.
- p2c1-mutation-a: after each of the four cycles, tree-clean check + full
  matrix 58/58 (final HEAD re-read `84e1b7d5…`).
- p2c1-mutation-b: after each of the three cycles, tree-clean check + full
  matrix 58/58; end state `git status --porcelain` over the whole worktree =
  0 lines (mutant binaries live under ignored `build/`).

## Independent cross-checks performed this audit

1. **Correctness re-review re-ran 2/10 mutants** spanning both mechanism
   sides — P2C1-5 (compose) and C1-1 (admission) — each RED with exit=1 and
   the exact named FAIL line, tree clean afterwards, and a clean
   `direct_composition_fault_test` rebuild green after P2C1-5
   (`correctness-review-20261030.md` §1.4). The other 8 mutants were not
   re-run there and are taken from these receipts (disclosed as such).
2. **Second compiler:** the C1 platform/assumptions audit independently
   killed all 10 mutants under clang 21.1.8 and under GCC 15.2.0 in its own
   worktree (`receipts/10-c1-clang21-suite-and-mutants.txt`,
   `receipts/09-c1-gcc152-mutant-kills.txt`), so no kill depends on
   GCC-specific codegen.

## Command essentials (per unit)

```sh
cd /tmp/sluice-val-<unit>                       # dedicated detached worktree @ 84e1b7d5
xmake f -c -m release --liburing=n -y && xmake -y && xmake test -y     # BASE GREEN 58/58
xmake build -y <mutant_target>                  # injection = building the mutant target
timeout 120 ./build/linux/x86_64/release/<mutant_target>; echo exit=$?
#   expected: exit=1 and exactly one line: FAIL: <designated killer>
git status --porcelain -- src/ include/ tests/ xmake/ docs/    # must be empty
xmake -y && xmake test -y                       # RESTORED GREEN 58/58
```

Recorded adaptation in all three units: the scout commands say
`cd /tmp/sluice-val-release`; per unit discipline each ran in its own
dedicated worktree at the same commit with identical arguments. All other
commands verbatim; the `xmake -y` builds only default targets and test
binaries compile on demand inside `xmake test` (actual xmake behavior, noted
in each receipt).

## Validity summary

| family | mutants | valid kills | invalid/survived | verdict |
|--------|---------|-------------|------------------|---------|
| C1 (admission dispatch, #602/#600 C1) | C1-1, C1-2, C1-3 | 3 | 0 | **3/3 valid** |
| P2C1 (prevalidated read compositions, #608/#612) | P2C1-1 … P2C1-7 | 7 | 0 | **7/7 valid** |
| total | 10 | 10 | 0 | **10/10 valid** |

Validity criterion per mutant: BASE green (full matrix 58/58 and/or direct
killer-suite run) → MUTANT red by the named semantic assertion only (exit=1,
exactly one FAIL line matching the designated killer; crash/signal/build
failure/timeout would not count) → RESTORED green (tree clean + killer suite
rerun + full matrix 58/58). All ten satisfy all three legs on this host.
