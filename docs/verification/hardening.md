# Local hardening correctness gate

`scripts/hardening.sh` (thin shell wrapper) → `scripts/hardening.py` (Python stdlib implementation).

## What it is

The hardening gate moves the expensive, repeatable verification that GitHub Actions
deliberately does **not** run — long Debug soaks, the TSan hot set, ASan+UBSan, and
libFuzzer campaigns — onto a developer Linux/Clang machine. It is **not** a
replacement for the PR merge gate (`.github/workflows/ci.yml`), the deterministic
unit tests, or the formal TLA+ models. It reuses the existing xmake targets, tests,
and fuzz harnesses; it introduces no new framework and changes no production code.

## Requirements

- **Linux** + **Clang** (libFuzzer is Clang-only; the fuzz targets are gated on the
  `clang` toolchain by `xmake/fuzz.lua`).
- **Python >= 3.10** (stdlib only — no third-party packages required).
- `xmake`, `clang`/`clang++`, `git`.
- Optional: `shellcheck` (for linting the shell wrapper; not a runtime dependency).

## Usage

```bash
# Default: the full ~8h gate.
./scripts/hardening.sh

# Prove the runner wiring end-to-end (NOT a correctness gate). Each phase runs
# once; each fuzz target runs ~10s. Use this after editing the runner.
./scripts/hardening.sh --smoke

# A shorter window.
./scripts/hardening.sh --hours 6

# Controlled self-test: proves TIMEOUT classification, TERM→KILL escalation,
# environment propagation, sanitizer detection, fuzz artifact detection, and
# that one failure does not abort the runner. Uses SYNTHETIC failures; never
# produces an hardening HOLD.
./scripts/hardening.sh --self-test

./scripts/hardening.sh --help
```

### Environment overrides (CLI args win where applicable)

| Variable                 | Default | Meaning                                                        |
|--------------------------|---------|----------------------------------------------------------------|
| `SLUICE_HARDENING_HOURS` | `8`     | Total budget in hours.                                         |
| `SLUICE_HARDENING_PHASE_TIMEOUT`   | `1200`  | Per-command timeout, seconds (each external command).          |
| `SLUICE_HARDENING_FUZZ_SECONDS`    | split   | Override the **total** fuzz budget (divided across targets).   |
| `SLUICE_HARDENING_KEEP_GOING`      | `1`     | `0` = stop after the current command, but still write summary + logs. |
| `SLUICE_HARDENING_PYTHON`           | `python3` | Python interpreter path for the runner.                      |

> **The runner reconfigures xmake several times** (debug → tsan → asanubsan →
> debug for fuzz → debug for Final Debug). That is expected. Preflight verifies
> compiler capability before any test runs.

## Phases

After a full **Preflight** (platform, tools, compiler probes, repository, disk
space, single-instance lock), the runner executes:

1. **Phase A: Debug baseline** — configure + build `sluice_core`/`sluice_async` +
   build/run the full `test` group + both negative-compile scripts + the public
   acceptance consumers.
2. **Phase B: Debug soak** (~25% of remaining after Final Debug reserve) —
   repeat the full `xmake test -v` suite.
3. **Phase C: TSan hot set** (~25%) — reconfigure tsan; loop the concurrency-
   relevant targets (`TSAN_HOT_SET`). `TSAN_OPTIONS=halt_on_error=1:
   second_deadlock_stack=1`.
4. **Phase D: ASan+UBSan** (~12.5%) — reconfigure asanubsan; one full suite, then
   the ownership/lifetime hot set. `ASAN_OPTIONS=halt_on_error=1:detect_leaks=1`,
   `UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1`.
5. **Phase E: Fuzz** (remainder) — reconfigure a Clang debug fuzz build; run each
   existing libFuzzer target for `fuzz_budget / n_targets` seconds, reusing the
   persistent per-target corpus.
6. **Phase F: Final Debug** (reserved) — reconfigure debug and run the full suite
   again, proving the sanitizer/fuzz config switching did not corrupt the final
   state.

A failed iteration is a **sticky** failure: retries are recorded as reproduction
evidence but can never turn a HOLD into a PASS.

## Verdicts (`summary.txt` + exit code)

| Verdict             | Exit | Meaning                                                              |
|---------------------|------|----------------------------------------------------------------------|
| `PASS`              | 0    | Required baseline + Final Debug OK; no sticky failure/timeout/sanitizer/new fuzz crash; at least one critical TSan target, the ASan+UBSan full suite, and at least one fuzz target actually executed. |
| `HOLD`              | 1    | Any sticky failure/timeout/sanitizer report/new fuzz crash; a required baseline or Final Debug failure; or all critical TSan targets unavailable. |
| `ENVIRONMENT_ERROR` | 2    | Missing tool, compiler probe failure, unsupported platform, low disk space, or lock contention. |
| `RUNNER_ERROR`      | 3    | Internal runner failure (log write failure, unhandled exception, process management error). |
| `INCOMPLETE`        | 4    | No real failure, but a required evidence family did not execute (budget/platform/environment). |

Read the verdict from the first line of `summary.txt`: `SLUICE HARDENING: PASS`
/ `HOLD` / `INCOMPLETE`.

## Distinction: test failures vs. runner/environment failures

| Type                  | Examples                                                           | Behavior                           |
|-----------------------|--------------------------------------------------------------------|------------------------------------|
| **Test failure**      | assertion, race, ASan, fuzz crash, timeout, non-zero exit          | Record, sticky HOLD, keep going    |
| **Runner/environment** | missing tool, disk full, lock contention, unhandled exception      | Stop immediately, write summary    |

**Test failures do not stop the runner; evidence system failures must stop it.**

## Artifacts

Each run writes to `hardening-artifacts/<YYYYMMDD-HHMMSS>-<short-sha>/`
(gitignored):

- `summary.txt` — human-readable verdict and counts.
- `summary.json` — structured verdict (machine-readable).
- `preflight.txt` / `preflight.json` — preflight check results.
- `environment.json` — tool versions, configuration, HEAD.
- `run.log` — global timeline.
- `events.tsv` | `events.jsonl` — one structured record per external command.
- `failures.jsonl` | `failures/index.tsv` — **real failures only**.
- `worktree.diff` — full `git diff` snapshot when the checkout is dirty.
- `worktree-cached.diff` — `git diff --cached` snapshot.
- `<mode>-targets.txt` — cached per-configuration target snapshot.
- `debug-soak/`, `tsan/`, `asanubsan/`, `fuzz/` — per-command logs.
- `fuzz/<target>.corpus-stats.json` — corpus before/after file+byte counts.

The persistent per-target fuzz corpus lives under `.hardening-corpus/` (gitignored,
seeded from `fuzz/corpus/`, never cleared).

## Single-instance lock

The runner uses `fcntl.flock()` on `.hardening.lock` in the project root.
If another hardening run is already active, the new instance exits immediately
with `ENVIRONMENT_ERROR`. The lock is released automatically when the runner
exits (including on crash).

## Classification notes

- Timeout is managed by Python's `subprocess.Popen.wait(timeout=...)` with
  explicit SIGTERM → grace period → SIGKILL escalation. The `timed_out`,
  `term_sent`, and `kill_sent` fields are recorded on every `CommandResult`.
- Exit code 77 is treated as `SKIP` unless an individual target documents a
  different meaning.
- Sanitizer classification inspects **both** exit status and known sanitizer
  signatures (Python `re`). An exit-zero log that contains a sanitizer report
  is still classified as `SANITIZER_FAIL`.
- libFuzzer reaching `-max_total_time` (clean exit 0, no new crash artifact) is
  `PASS`. Only a **newly generated** `crash-*`/`leak-*`/`timeout-*`/`oom-*`/
  `panic-*` artifact (set difference against an isolation baseline taken before
  each invocation) makes a fuzz run `HOLD`.

## Running unit tests

```bash
python3 -m unittest discover -v
python3 -m unittest scripts.tests.test_hardening_runner -v
```

## Running self-test

```bash
./scripts/hardening.sh --self-test
```

## Running smoke test

```bash
./scripts/hardening.sh --smoke
```

## What this runner does not do

- It does **not** replace the deterministic unit tests or the formal models.
- It does **not** modify production code, tests, CI, or the xmake build.
- It does **not** push or open pull requests; it only writes local artifacts.
- It does **not** require `pip install`, `npm install`, or any third-party
  Python package. The implementation uses only the Python standard library.