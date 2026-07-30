# Local overnight correctness gate

`scripts/overnight-local.sh` moves the expensive, repeatable verification that
GitHub Actions deliberately does **not** run — long Debug soaks, the TSan hot
set, ASan+UBSan, and libFuzzer campaigns — onto a developer Linux/Clang
machine. It is **not** a replacement for the PR merge gate (`.github/workflows/
ci.yml`), the deterministic unit tests, or the formal TLA+ models. It reuses
the existing xmake targets, tests, and fuzz harnesses; it introduces no new
framework and changes no production code.

## When to use it

Run it overnight (or for a bounded window) when you want confidence that a
change is stable across many iterations and under sanitizers/fuzzing that CI
budgets cannot afford. CI stays cheap and fast; deep evidence stays local.

## Requirements

- Linux + **Clang** (libFuzzer is Clang-only; the fuzz targets are gated on the
  `clang` toolchain by `xmake/fuzz.lua`).
- `xmake`, `clang`/`clang++`, `git`, and the usual coreutils (`bash`, `date`,
  `timeout`, `tee`, `mkdir`).
- Optional: `shellcheck` (for linting the script itself; not a runtime dependency).

## Usage

```bash
# Default: the full ~8h gate.
./scripts/overnight-local.sh

# Prove the runner wiring end-to-end (NOT a correctness gate). Each phase runs
# once; each fuzz target runs ~10s. Use this after editing the script.
./scripts/overnight-local.sh --smoke

# A shorter window.
./scripts/overnight-local.sh --hours 6

# Controlled self-test: proves TIMEOUT classification and that one failure does
# not abort the script. Uses SYNTHETIC failures; never produces an overnight HOLD.
./scripts/overnight-local.sh --self-test

./scripts/overnight-local.sh --help
```

### Environment overrides (CLI args win where applicable)

| Variable                 | Default | Meaning                                                        |
|--------------------------|---------|----------------------------------------------------------------|
| `SLUICE_OVERNIGHT_HOURS` | `8`     | Total budget in hours.                                         |
| `SLUICE_PHASE_TIMEOUT`   | `1200`  | Per-command timeout, seconds (each external command).         |
| `SLUICE_FUZZ_SECONDS`    | split   | Override the **total** fuzz budget (divided across targets).   |
| `SLUICE_KEEP_GOING`      | `1`     | `0` = stop after the current command, but still write summary + logs. |

> **The script reconfigures the xmake build several times** (debug → tsan →
> asanubsan → debug again for fuzz → debug for Final Debug). That is expected.
> It uses `xmake f -c -m <mode> --toolchain=clang -y`, which clears the cached
> config each time for a fresh reconfigure.

## Phases

After a fixed **Debug baseline** (configure + build `sluice_core`/`sluice_async`
+ build/run the full `test` group + both negative-compile scripts + the public
acceptance consumers), the runner **reserves Final Debug time first**
(`max(20m, 1.5 × baseline elapsed)`) and then splits the remaining budget:

- **Debug soak** (~25%) — repeat the full `xmake test -v` suite.
- **TSan hot set** (~25%) — reconfigure tsan; loop the concurrency-relevant
  targets (see `TSAN_HOT_SET` in the script). `TSAN_OPTIONS=halt_on_error=1:
  second_deadlock_stack=1`.
- **ASan+UBSan** (~12.5%) — reconfigure asanubsan; one full suite, then the
  ownership/lifetime hot set. `ASAN_OPTIONS=halt_on_error=1:detect_leaks=1`,
  `UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1`.
- **Fuzz** (remainder) — reconfigure a Clang debug fuzz build; run each existing
  libFuzzer target for `fuzz_budget / n_targets` seconds, reusing the persistent
  per-target corpus.
- **Final Debug** (reserved) — reconfigure debug and run the full suite again,
  proving the sanitizer/fuzz config switching did not corrupt the final state.

A failed iteration is a **sticky** failure: retries are recorded as reproduction
evidence but can never turn a HOLD into a PASS.

## Verdicts (`summary.txt` + exit code)

| Verdict    | Exit | Meaning                                                                 |
|------------|------|-------------------------------------------------------------------------|
| `PASS`     | 0    | Required baseline + Final Debug OK; no sticky failure/timeout/sanitizer/ new fuzz crash; at least one critical TSan target, the ASan+UBSan full suite, and at least one fuzz target actually executed. |
| `INCOMPLETE` | 0  | No real failure, but a required evidence family did not execute (budget/platform/environment). |
| `HOLD`     | 1    | Any sticky failure/timeout/sanitizer report/new fuzz crash; a required baseline or Final Debug failure; or all critical TSan targets unavailable. |
| arg/env    | 2    | Bad arguments or a missing core tool.                                   |

Read the verdict from the first line of `summary.txt`: `SLUICE OVERNIGHT: PASS`
/ `HOLD` / `INCOMPLETE`.

## Artifacts

Each run writes to `overnight-artifacts/<YYYYMMDD-HHMMSS>-<short-sha>/`
(gitignored):

- `summary.txt` — human-readable verdict and counts.
- `run.log` — global timeline.
- `events.tsv` — one structured record per external command (timestamp, phase,
  iteration, target, mode, classification, raw exit, duration, timeout, log
  path, shell-escaped command).
- `failures/index.tsv` — **real failures only** (`FAIL`/`TIMEOUT`/
  `SANITIZER_FAIL`/`FUZZ_CRASH`/`BUILD_FAIL`), with the first diagnostic
  signature and the original log path. Ordinary `SKIP`s live only in
  `events.tsv`/`summary.txt`.
- `environment.txt` — date, HEAD, dirty state, tool versions, budget.
- `worktree.diff` — full `git diff` snapshot when the checkout is dirty (a dirty
  tree is recorded prominently but never refused).
- `<mode>-targets.txt` — the cached per-configuration target snapshot used for
  existence checks.
- `debug-soak/`, `tsan/`, `asanubsan/`, `fuzz/` — per-command logs (each is
  self-contained: phase/iteration/target/mode, HEAD + dirty state, the exact
  shell-escaped command, sanitizer env, start/end timestamps + epochs, timeout,
  raw exit code, classification, and TERM→KILL escalation status).
- `fuzz/<target>.corpus-stats.tsv` — corpus before/after file+byte counts.

The persistent per-target fuzz corpus lives under `.nightly-corpus/` (gitignored,
seeded from `fuzz/corpus/`, never cleared).

## Classification notes

- `timeout` is invoked plainly (`-s TERM -k <grace>`; **no** `--preserve-status`),
  so a timeout surfaces as exit `124` = `TIMEOUT`. A 137 is **not** taken as
  proof of a SIGKILL escalation; that is recorded as inferred/unknown.
- Exit `77` is treated as `SKIP` unless an individual target documents a
  different meaning.
- Sanitizer classification inspects **both** exit status and known sanitizer
  signatures (`WARNING: ThreadSanitizer`, `ERROR: AddressSanitizer`,
  `runtime error:`, …). An exit-zero log that contains a sanitizer report is
  still `FAIL`.
- libFuzzer reaching `-max_total_time` (clean exit 0, no new crash artifact) is
  `PASS`. Only a **newly generated** `crash-*`/`leak-*`/`timeout-*`/`oom-*`/
  `panic-*` artifact (counted against an isolation baseline taken before each
  invocation) makes a fuzz run `HOLD`.

## What this runner does not do

- It does **not** replace the deterministic unit tests or the formal models.
- It does **not** modify production code, tests, CI, or the xmake build.
- It does **not** push or open pull requests; it only commits nothing and writes
  local artifacts.
