# ISSUE-47-DIAGNOSTIC-1 — classify `multi_worker_coord_test` abnormal termination

**Status:** `ISSUE-47-DIAGNOSTIC-1: NOT-REPRODUCED` (apparatus validated; local
reproduction did not trigger the incident).
**Fix authorization:** `FIX PR NOT YET AUTHORIZED` — the failure class is the
suspected C1→C2 masking chain, but no deterministic/precise crash site was
captured because the race did not reproduce locally. The CI manual workflow is
the next diagnostic boundary.

> **DIAGNOSTIC ONLY. NO SCHEDULER FIX. NO RETRY / SLEEP / QUARANTINE.
> ROOT CAUSE NOT YET CLAIMED.**

This document records the diagnostic PR for issue #47. It exists only to
classify the observed abnormal termination of case
`mwcoord_serialized_backend_access` (`tests/multi_worker_coord_test.cpp`) into
one of the categories below, and to stand up the apparatus that will classify
the *next* occurrence unambiguously.

## Categories (issue #47 §0)

```
C1  sched.run(2) returned before all six operations completed
C2  a SLUICE_CHECK failed, returned, and teardown fail-fast masked the failure
C3  std::terminate from inside Scheduler/backend before sched.run(2) returned
C4  the process received a signal (SIGSEGV/SIGABRT/SIGILL/...)
C5  an unhandled C++ exception escaped the test case or worker thread
C6  another precisely identified abnormal exit
```

## The confirmed masking mechanism (the primary hypothesis)

The code contains a confirmed **failure-masking** path (issue #47 §1), NOT yet
proven to be the CI incident's path:

```
SLUICE_CHECK(ops_done == N) fails
→ record_failure(); return;            (harness.hpp SLUICE_CHECK macro)
→ local Scheduler / AsyncIoContext destruction (stack unwind)
→ ~AsyncIoContext sees backend_->outstanding() != 0
→ detail::async_context_outstanding_fail_fast()   (src/async/async_io_context.cpp)
→ std::terminate                                     (fail_fast.hpp contract)
→ default terminate handler → std::abort() → SIGABRT
→ the harness never reaches report_and_exit() → no "FAILED in case" line
```

This explains every observed CI symptom (issue #47 §0): only the first case name
printed, abnormal exit, xmake `-1` / shell `255`, and the missing harness
failure summary. The masking is real and load-bearing: it converts an
`ops_done != N` (C1) failure into a teardown SIGABRT that hides the original
result.

**This is still a hypothesis for the CI incident.** It is NOT claimed as root
cause until captured evidence (a core/backtrace repeatedly identifying the
invalid state, or a deterministic phase-controller reproduction) is obtained
(issue #47 §20).

## What this PR adds (scope: diagnostic only)

| Artifact | Purpose |
| --- | --- |
| `scripts/run_issue47_diag.py` | Direct-binary runner; executes the built test binary via `subprocess` directly (never `xmake run`/`xmake test`) and classifies the real process/signal status. |
| `tests/multi_worker_coord_test.cpp` (case `mwcoord_serialized_backend_access` only) | Test-local phase breadcrumbs, a post-run snapshot, a `std::set_terminate` marker (I47-T00 + SIGABRT), and a diagnostic early-exit seam (`std::_Exit(90/91/92)`) that runs BEFORE local destructors so teardown fail-fast cannot overwrite the original result. All gated on `SLUICE_ISSUE47_DIAG=1`. |
| `.github/workflows/issue47-diagnostic.yml` | Manual `workflow_dispatch` workflow (NOT a required check). 4 independent shards build once, run the binary directly many times, stop at the first abnormal iteration, and upload all evidence + best-effort core/backtrace. |

**No production change. No public-header change. No Scheduler/MW-S1/S2/S3 /
work-stealing / Fiber-transition / wait_one change. No sleeps, retries,
quarantine, or assertion-policy change.** (issue #47 §3.)

## Apparatus validation (issue #47 §11)

The apparatus was validated WITHOUT the real flaky race (per §11: "Do not use
the real flaky race to validate the tooling").

### Classification logic (unit, `run_issue47_diag.classify`)

| Input `returncode` | Classification | Verified |
| --- | --- | --- |
| `0` | `PASS` | ✓ |
| `1` | `NORMAL_HARNESS_FAILURE` | ✓ |
| `-6` | `SIGNAL_TERMINATION` / `SIGABRT` | ✓ |
| `-11` | `SIGNAL_TERMINATION` / `SIGSEGV` | ✓ |
| `-4` | `SIGNAL_TERMINATION` / `SIGILL` | ✓ |
| `-7` | `SIGNAL_TERMINATION` / `SIGBUS` | ✓ |
| `-8` | `SIGNAL_TERMINATION` / `SIGFPE` | ✓ |
| `-15` | `SIGNAL_TERMINATION` / `SIGTERM` | ✓ |
| `-9` | `SIGNAL_TERMINATION` / `SIGKILL` | ✓ |
| `90` | `EARLY_RUN_RETURN_WITH_LIVE_WORK` | ✓ |
| `91` | `RUN_RETURNED_WITH_NO_OUTSTANDING_BUT_INCOMPLETE_FIBERS` | ✓ |
| `92` | `CONCURRENT_BACKEND_ACCESS_OBSERVED` | ✓ |
| `7` | `ABNORMAL_NON_SIGNAL_EXIT` | ✓ |

(17/17 unit checks passed.) Input rejection: `--count <= 0`, invalid
`--timeout-seconds`, missing/non-executable binary all exit nonzero (✓).

### End-to-end via stand-in binaries

Stand-in programs producing known exit statuses were run through the runner:

| Stand-in | runner rc | first-iteration classification |
| --- | --- | --- |
| exit 0 | 0 | `PASS` |
| exit 1 (harness failure) | 1 | `NORMAL_HARNESS_FAILURE` |
| `I47-T00` then `SIGABRT` | 1 | `SIGNAL_TERMINATION` / `SIGABRT`, `last_phase=[I47] I47-T00...` |
| direct `SIGSEGV` (no marker) | 1 | `SIGNAL_TERMINATION` / `SIGSEGV`, last phase = pre-crash `I47-P04` (no T00 — distinguishes terminate from a direct signal) |
| diagnostic early-exit 90 | 1 | `EARLY_RUN_RETURN_WITH_LIVE_WORK` |
| hang | 1 | `TIMEOUT` |

### Real-binary apparatus self-test (`SLUICE_I47_FORCE_EARLY=1`)

`SLUICE_I47_FORCE_EARLY=1` (diagnostic-mode only) makes the backend NOT
auto-complete, deterministically forcing `outstanding() != 0` after `run(2)`
returns. On the real test binary this produced exactly the §4 hypothesized
state and proved the early-exit seam preserves it:

```
rc = 90
[I47] I47-SNAPSHOT
[I47] ops_done=0
[I47] outstanding=6
[I47] waiting=6
[I47] runnable=0
[I47] fiber_states=[waiting,waiting,waiting,waiting,waiting,waiting]
[I47] completion_ready=[0,0,0,0,0,0]
[I47] I47-CLASS EARLY_RUN_RETURN_WITH_LIVE_WORK (ops_done=0/6 outstanding=6)
# I47-T00 count: 0  (destructor fail-fast bypassed by std::_Exit)
```

This is the §11 goal: the same `outstanding=6, ops_done=0` shape that would
otherwise trigger `~AsyncIoContext` → fail-fast → I47-T00 + SIGABRT (masking)
instead yields `exit=90` with the snapshot preserved.

## Reproduction results (issue #47 §15 / §D)

Local environment: WSL2 Linux x86_64, 8 CPUs (vs. the CI 2-CPU hosted runner).
`master` = `6d67f9c6e7113503a8826d7a4220600e1fd6badc` (PR #46 merged).

| Matrix entry | Mode | CPUs | Iters | PASS | Fail/abnormal |
| --- | --- | --- | --- | --- | --- |
| D1 baseline | debug | taskset 0,1 | 200 | 200 | 0 |
| D1 (free) | debug | none (8 free) | 500 | 500 | 0 |
| D1 release | release | taskset 0,1 | 200 | 200 | 0 |
| D1 release | release | taskset 0,2 | 200 | 200 | 0 |
| **total** | | | **1100** | **1100** | **0** |

No SIGABRT, SIGSEGV, SIGILL, timeout, diagnostic early-return, or harness
failure was observed. **Local result: NOT-REPRODUCED**, which is an acceptable
diagnostic outcome (issue #47 §19). The race is CI-environment-dependent (the
hosted 2-CPU runner's scheduling interleaving is the suspected trigger); the
manual `issue47-diagnostic.yml` workflow is the apparatus that will reproduce
it on the matching host.

Full Clang Debug gate (AGENTS.md §4) before these changes: 113/113 tests
passed, 0 failed.

## First abnormal trace

None captured locally (NOT-REPRODUCED). When the CI workflow captures one, its
uploaded artifact will contain: per-iteration stdout/stderr, `iterations.jsonl`
(with `last_phase`, returncode, classification, signal), `summary.txt`, the
environment report, and a best-effort core/backtrace.

## Hypothesis ledger (issue #47 §F)

| Hypothesis | Status | Evidence |
| --- | --- | --- |
| early run return + teardown terminate (C1→C2 masking) | **open** | mechanism is mechanically present & apparatus-validated; not yet captured in CI |
| direct terminate inside Scheduler (C3) | open | not observed |
| invalid Fiber context switch | open | unproven (issue #47 §18); no instrumentation added yet (§16 defers it) |
| duplicate/stale runnable ticket | open | unproven (issue #47 §18) |
| MW-S2 premature termination | open | not observed |
| probe-induced timing | open | not observed; D4 (probe vs direct) is exercisable via the workflow |

## Fix authorization (issue #47 §G)

`FIX PR NOT YET AUTHORIZED`. The next diagnostic boundary is running the manual
`issue47-diagnostic.yml` workflow (Debug + Release × stress/no-stress, multiple
shards) on the hosted 2-CPU runner to capture the first abnormal iteration.
A fix PR is authorized only after one of (issue #47 §20): a deterministic
phase-controller reproduction, a core/backtrace repeatedly identifying the same
invalid state/site, or an internal-testing invariant repeatedly identifying the
same protocol violation with an exact causal trace.

## Notes on Scheduler instrumentation (issue #47 §16-§18)

This first diagnostic commit uses ONLY: direct runner, test-local phases,
post-run snapshot, terminate marker, diagnostic exit classification, and the
manual workflow. No Scheduler-internal observation was added (§16). If the
above apparatus fails to classify the incident, a later commit MAY add
internal-testing-only phase observations (§17) or probe the Fiber-ticket
hypothesis (§18), guarded by `SLUICE_ASYNC_INTERNAL_TESTING`, behind a report
of why the test-local evidence was insufficient.
