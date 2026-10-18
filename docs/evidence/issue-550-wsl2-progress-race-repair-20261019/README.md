# Issue #550 — WSL2 repair of three scheduling-sensitive oracles in `threadpool_progress_race_test`

Test-only repair. Production sources, public API, the v2 root contract and the
conformance ledger are untouched; no conformance claim changes, so no ledger
row is amended here.

BASE_SHA `7c6dbb3d0492087af1a03de11aa4f4a0f56ec496` (= `origin/master` =
`v0.0.2` at repair time; the release candidate differs from master only in
`CHANGELOG.md`).

Branch `fix/550-threadpool-progress-test-synchronization` → PR to `master`,
held OPEN. `WSL2_RESULT = PASS`; `NATIVE_LINUX_RESULT = NOT_RUN` (deferred to
the owner, per the issue's stage assignment).

## Environment (WSL2)

| Item | Value |
|---|---|
| Kernel | `6.18.54.1-microsoft-standard-WSL2 x86_64` |
| OS | Ubuntu 26.04.1 LTS (Resolute Raccoon) |
| Compiler | g++ (Ubuntu 15.2.0-16ubuntu1) 15.2.0 |
| xmake | `v2.9.7+20261005` |
| liburing | system 2.14 (`liburing-dev 2.14-1`) |
| CPUs | 8 logical |

xmake version note: the failure was reproduced with the compiled binary
executed directly (no xmake in the loop) and the issue's own diagnosis
reproduced it with a plain `g++` build, so the defect is independent of the
xmake version; build-flag or tooling differences can only widen or narrow the
scheduling window, not create or remove the missing happens-before edge that
this repair closes.

## Root cause classification

`GatedWorkerRead::release()` waited only for the worker to leave the
`WorkerOutcomePreTerminalPauseGate` (`gate.exited`). In
`ThreadPoolBackend::worker_loop()` the pause point ends *before*
`offer_terminal` → `release_execution` → `publication_pending_.push_back()` →
`signal_ready_progress()`, so `release()` returning proves nothing about the
terminal handoff, the publication enqueue, or the progress signal. Three
oracles additionally assumed an ordering that this edge cannot establish.
Classification: **test-harness synchronization defect** (`test-infrastructure`);
no production no-lost-wake violation was found, and none is claimed as proven
absent by this repair.

## Before evidence (unmodified baseline, release + gcc + liburing=y)

Orchestration: each run writes its own log and exit-code file; the parent
aggregates. (The issue body's "8 instances" snippet lacked `&` and serialized;
this campaign corrects that, as the diagnostic comment requires.)

| Campaign | Attempts | Failures | Failing subtests |
|---|---|---|---|
| Sequential | 360 | 0 | — |
| Concurrent, 8 processes × 60 batches | 480 | 6 | `saturated_notification_still_wakes_parked_owner` ×4, `control_outer_exhaustion_is_terminal_and_never_aliases` ×1, `r4_worker_completion_signal_before_final_recheck_is_caught` ×1 |
| Contention, 16 processes × 30 batches (oversubscribed) | 480 | 15 | saturated ×6, control_outer ×7, r4 ×2 |

Receipts: `/tmp/sluice-550-baseline-runs/` (`results.csv`, per-run
`logs/`, `status/`, `*.summary`). Failure signature matches issue #550
(first-failing-subtest abort; same three subtests).

## Execution DAGs and missing edges

Common target order: owner held at its gate → worker released → worker commits
terminal → worker enqueues publication → worker completes progress signal →
main obtains causal witness → main resumes owner → owner performs its intended
revalidation/pass → exact assertion.

### R4 `r4_worker_completion_signal_before_final_recheck_is_caught`

| Field | Answer |
|---|---|
| Event source | single worker's completion chain (`threadpool_backend.cpp:443-465`) |
| Publication | `signal()` advances the epoch and writes the fd inside one `ProgressSource::mtx_` hold; `snapshot()` reads under the same mutex |
| Gates | worker: `WorkerOutcomePreTerminalPauseGate` (pre-`offer_terminal`); owner: `ProgressSource` prerevalidate gate (before the first epoch revalidation, `progress_source.hpp:149-166`) |
| Required ordering | worker signal completed **before** the owner's revalidation runs |
| Old witness | worker `gate.exited` only — proves the pause point ended, nothing after |
| Missing edge | `[worker signal() critical section completes] happens-before [main resumes owner]` |
| Oracle | `is_progress(result,1) && prepark==0 && ready()`: the revalidation must catch the epoch change so the owner never parks |
| Cleanup | witness failure still resumes the owner and joins the driver; drain releases the gated read and drives held requests |

### SATURATED `saturated_notification_still_wakes_parked_owner`

| Field | Answer |
|---|---|
| Event source | same worker chain, plus test-injected eventfd saturation (`saturate_notification_for_test` writes the counter to the eventfd maximum `2^64-2`) |
| Publication | fd readiness is kernel state; the test injects readiness itself **before** releasing the worker, so fd readability cannot witness the worker |
| Gate | owner at the prepark gate — after the stale-drain, after the prepark counter (=1), after probe/revalidation, immediately before `poll(2)` |
| Required ordering | `[fd saturated] before [worker's write] before [owner resumes into poll]`, with no drain in between |
| Old witness | `notification_fd_readable` after `release()` — vacuous: that readiness was injected by the test |
| Missing edge | `[worker signal() completed under saturation] happens-before [resume owner]` — distinguishable only via the progress token |
| Oracle | saturation readable ∧ witness ∧ readiness persisted ∧ `is_progress(result,1) ∧ prepark==1 ∧ ready()`: exactly one park, woken by the saturated readiness, next pass publishes |
| Cleanup | as R4 |

### CONTROL `control_outer_exhaustion_is_terminal_and_never_aliases`

| Field | Answer |
|---|---|
| Event source | same worker chain; control interrupts advance only the control generation (`interrupt()` never touches the progress token) |
| Publication | as R4; the owner here is the main thread (no driver): each `wait_one` runs its pass first, so with the publication enqueued the very first pass reports `progress(1)` |
| Gate | worker gate only (owner ungated; the control domain `{max,max}` is absorbing, so each round's wait returns `control_interrupted` without parking) |
| Required ordering | `[worker publication enqueued and signal completed] before [the single final wait_one]` |
| Old witness | none — the tail looped `1000 × wait_one(8000ms)`; with the control domain spent every empty-pass iteration returns `control_interrupted` immediately, so the loop could exhaust before the worker was scheduled (observed: `ok=1 progress=0 kind=control_interrupted`) |
| Missing edge | `[worker signal() completed] happens-before [final wait_one called]` |
| Oracle | rounds assert `is_control ∧ prepark==0 ∧ !ready` (control terminality, unchanged); the tail now asserts a single causal `progress(1)` with no retry budget |
| Cleanup | `read.release()` and a single bounded `wait_one` always run before any verdict |

## The causal witness and why it is sufficient

`wait_worker_completion_signal(ctx, baseline)` (in the test file) spins until
`TestAccess::context_progress_token(ctx)` differs from a baseline recorded
**while the worker was still gated**, under a 10 s watchdog that only reports
failure (it is never the synchronization). Sufficiency argument:

1. `ProgressSource::signal()` executes the epoch/exhaustion advance and the
   notification-fd write (or its EAGAIN branch) inside one `mtx_` hold
   (`progress_source.hpp:252-264`); `snapshot()` takes the same mutex, so an
   observed change implies that critical section completed — the signal, not
   an in-flight prefix of it.
2. `publication_pending_.push_back(h)` precedes `signal_ready_progress()` in
   the worker by program order (`threadpool_backend.cpp:460-465`), so the
   witnessed change also implies the publication was enqueued: the owner's
   next pass deterministically publishes it. This ordering property is a
   static code guarantee; see the M5 record for its mutation status.
3. Producer set: in each scenario the only submitted non-zero op is the gated
   read, there is exactly one worker, no zero-op inline publications run, and
   `cancel`/`close_admission` are never called. `interrupt()` (CONTROL)
   advances the control generation only. A token change therefore cannot be
   counterfeited by another producer.
4. Wrap/exhaustion: epochs saturate instead of wrapping; these scenarios never
   set the progress epoch to `kMax`, and the witness compares both token
   fields, so the exhaustion-carry shape (r15) would also be distinguished.

Misuse boundaries are documented at the helper: baselining after release, or
resuming the owner before the witness, would silently lose the interleaving
under test.

## EAGAIN branch coverage (honest statement)

The repaired saturated scenario proves, in order:

- **No-lost-wake under saturation**: proven for this interleaving — the owner
  parked with a saturated counter is woken by the worker's signal and
  completes with `prepark==1`, `progress==1`.
- **eventfd saturation write occurs**: proven by preconditions, not by direct
  errno observation — (a) the owner is paused at the prepark gate, which is
  after its stale-drain, so its drain cannot run again before resume;
  (b) `saturate_notification_for_test` set the counter to the eventfd maximum
  `2^64-2` after that drain; (c) `HostInterest` only polls, never reads, and
  no `acknowledge_notification` runs before resume; therefore the worker's
  write of 1 onto `2^64-2` fails the kernel's strict-inequality admission
  check (a write succeeds only while the resulting counter stays strictly
  below `2^64-1`, so `2^64-2 + 1` is rejected) and the non-blocking fd
  deterministically returns `EAGAIN`; (d) the token witness proves that
  write completed inside the signal critical section.
- **EAGAIN handling**: the branch is exercised (mutant M3 below shows the
  execution passes through it and that mishandling it is loud), and its
  already-pending-wake semantics are what the passing oracle observes.

`SATURATION_BRANCH_WITNESS = PROVEN_BY_PRECONDITION` (kernel semantics +
locked drain discipline); a direct errno witness does not exist without
changing production, and was not added.

## Mutation and discrimination evidence

Mutants are one-off `/tmp` builds (`/tmp/sluice-550-mutants/`, direct `g++`,
patched copies; nothing entered the repository), per the repo's
manually-executed mutation discipline. Pristine rebuild passed before each
campaign.

| Mutant | Production change | Runs | Result |
|---|---|---|---|
| M1 | worker's post-enqueue `signal_ready_progress()` removed | 5 | KILLED 5/5 at `r4_...` (exit 1) |
| M2 | first epoch revalidation in `wait_if_unchanged` removed | 5 | KILLED 5/5 at `r2_...` (exit 1) |
| M3 | saturated-write `EAGAIN` branch made fail-fast | 5 | KILLED 5/5, SIGABRT with `write failed with errno=11` — confirms the saturated test's execution traverses the EAGAIN branch |
| M5 | `signal_ready_progress()` moved before `publication_pending_.push_back()` | 20 | NOT KILLED (0/20) |

Error-category coverage: (1) worker signal missing → M1; (2) final
revalidation lost → M2 (killed at r2, which shares the revalidation-catch
oracle R4 relies on); (3) saturated notification mishandled → M3; (4) control
interrupt misread as publication → the baseline failures themselves
(`ok=1 progress=0 kind=control_interrupted`): the tail assertion rejected the
control outcome exactly as designed — the flake was the missing witness, not a
weakened assertion, and the repaired tail keeps `is_progress(final_r, 1)`
exact; (5) terminal/publication ordering → M5, honestly reported.

M5 disclosure: exposing the reorder requires the owner's pass to reach
`work_mtx_` before the worker's immediately-following `push_back`; the
consumer path after the witness (gate resume, futex wake, snapshot, pass) is
strictly longer than the producer's remaining critical section, so the
exposing schedule lost deterministically on this machine. The property the
witness consumes (enqueue precedes signal) is established by code inspection
of `worker_loop`, not by this mutant. M5 is recorded as
equivalent-for-this-oracle here, not as coverage.

Teardown validation (hard gate): under M1 the new witness times out, prints
`worker completion signal missing after 10000ms: token 0/0 -> 0/0`, resumes
the owner, joins the driver, and exits 1 in 18.01 s wall = watchdog + the
driver's own 8 s bound. No hang (the 120 s alarm is untouched), no
`std::terminate`, no joinable-thread leak, no abandoned non-terminal request
(the drain publishes/discards). The old saturated test's early
`return false` between driver spawn and join (a latent `std::terminate` on
environmental failure) is gone: all three repaired tests now reach
`driver.join()` unconditionally between spawn and verdict.

## After evidence (repaired test, same orchestration, same binary config)

| Campaign | Attempts | Failures |
|---|---|---|
| Sequential | 360 | 0 |
| Concurrent, 8 × 60 | 480 | 0 |
| Contention, 16 × 30 | 480 | 0 |

Receipts: `/tmp/sluice-550-after-runs/`. 0/480 is sample evidence (no failure
observed), not a probabilistic zero-failure guarantee.

## Configuration matrix (WSL2, xmake 2.9.7, separate build dirs)

| Cell | Result |
|---|---|
| debug × liburing=n | 56/56 passed |
| debug × liburing=y | 74/74 passed |
| release × liburing=n | 56/56 passed |
| release × liburing=y | 74/74 passed |

`git diff --check` clean; `src/` and `include/` byte-identical to BASE_SHA;
no public API change. ThreadSanitizer (`-fsanitize=thread`, O1/g, direct
seam build): 5/5 runs, all 21 subtests pass, zero warnings.

## Authority contraction audit

No production authority moved; no legacy state, guard, token, lease, counter
or capability representation became eligible for removal. The new
test-only witness consumes the existing `ProgressSource` token through the
existing `TestAccess` seam and replaces no production mechanism.

## Limitations

- WSL2 only; native Linux cross-validation is `NOT_RUN` and is the next gate
  (`MERGE_READINESS = PENDING_NATIVE_LINUX`).
- The xmake used here is 2.9.7; other tooling versions were not exercised.
- Concurrent campaigns bound real time, not schedules; 0/N outcomes are
  absence-of-evidence-in-sample statements.
- Same-class latent early-return-terminate paths before `driver.join()` remain
  in unrelated subtests of this file (r2/r3/r5/r8/r10/r13/r15/f1) whose
  trigger conditions are environmental failures never observed in #550's
  campaigns; they are out of this slice's confirmed-defect scope and left for
  the owner to schedule. Likewise pre-existing (identical on master, flagged
  in review): `RequestPublicationDrain`'s tail `discard()` fail-fasts if a
  worker never reaches its enqueue, and the f1 token spin is unbounded and
  compares only `.progress` — unlike the bounded two-field witness added
  here.
- The production no-lost-wake protocol's full correctness is not re-proven
  here; the ProgressSource model and the existing oracle suites remain the
  standing evidence.

## Reproduction

```bash
xmake f -m release --liburing=y --toolchain=gcc -o /tmp/sluice-550-baseline -y
xmake build -y threadpool_progress_race_test
BIN=/tmp/sluice-550-baseline/linux/x86_64/release/threadpool_progress_race_test

mkdir -p /tmp/sluice-550-runs/logs /tmp/sluice-550-runs/status
for b in $(seq 1 60); do
  for j in $(seq 1 8); do
    ( "$BIN" >"/tmp/sluice-550-runs/logs/$b-$j.log" 2>&1
      printf '%s\n' "$?" >"/tmp/sluice-550-runs/status/$b-$j.rc" ) &
  done
  wait
done
# aggregate: count nonzero .rc files; grep '^FAIL:' logs/*.log for subtests
```
