# AUD-002 admission-health probe synchronization (2026-11-21)

P0 remediation record for audit finding AUD-002 (#643), plus the adjacent
admission/retirement ordering pair the same deterministic schedule exposed.
Branch `fix/643-aud002-fatal-error-locked-probe`, based on master
`9c630d1f5ff14da01a09d5c9cada8a2aa5ca2279`.

## 1. Baseline

- Base SHA: `9c630d1f5ff14da01a09d5c9cada8a2aa5ca2279` (master, 2026-11-21).
- Host: Fedora 44, kernel 7.3.3-200.fc44.x86_64, liburing 2.13,
  GCC 16.2.1, xmake 3.1.2.
- Pre-fix baseline suites: release `--liburing=n` 60/60; release
  `--liburing=y` 81/81 (matches the audit's 81/81 baseline row).

## 2. As-built concurrent protocol (revalidated at the base SHA)

`UringAsyncBackend::fatal_error_` is a `std::optional<IoError>` with exactly
one writer: `poison_and_recover_locked()` (`src/async/uring_transport.cpp:340`),
reached under `dispatch_mtx_` from

- `submit_transport_locked()` → `account_transport_result_locked()`
  (`uring_transport.cpp:233`/`:278`), and
- `reap_cqes()`'s overflow-flush failure path (`uring_completion.cpp:272`).

Both are reachable from the progress owner's `poll()` (and from the shutdown
driver's progress pass), which takes `dispatch_mtx_` but **not** the context
`access_mtx_`. Every other `fatal_error_` access is inside `dispatch_mtx_`.

`UringAsyncBackend::submit_request()` runs under the context `access_mtx_`
only. At the base SHA it probed backend health before taking `dispatch_mtx_`:

- `have_ring_` read at `src/async/uring_backend.cpp:433` (plain `bool`,
  also written by `retire_execution_resources()` at `:401`, outside any
  lock);
- `fatal_error_.has_value()` / `*fatal_error_` read at `:436-437`.

THREAD-01 (contract §13, rows "submit from different threads: Yes" and
"request cancel / ID cancel: Yes with submit, poll, admission close")
makes a submit from one thread concurrent with the owner's `poll()` — and
with the shutdown driver's settlement passes — a legal execution. The
unlocked reads therefore form unsynchronized write-read pairs with the
poison write and with the retirement write.

## 3. Validated failure interleaving

Both pairs were reproduced deterministically by
`tests/uring_submit_poison_race_test.cpp` (case
`admission_probe_unordered_with_owner_poison`):

1. Setup submits one operation through a sequenced transport hook whose
   first call reports *nothing submitted* while leaving the prepared-only
   ledger entry intact (`transport_ledger_size_for_test() == 1`,
   `sq_ready_for_test() == 1`).
2. `ctx.shutdown(drain)` runs on a driver thread. Its progress passes hold
   no `access_mtx_`; the transport hook parks the driver inside its
   `dispatch_mtx_` critical section immediately before the hard transport
   failure that poisons.
3. A submitter thread enters `AsyncIoContext::submit_read`, takes
   `access_mtx_`, and parks at the backend submit-entry pause gate.
4. The test releases the driver and the submitter together without
   observing either side's completion, so the `:436` read and the `:340`
   write (and the `:433`/`:401` pair) are unordered.

Symbolized TSan RED (5/5 runs, two race reports each):

```text
Read of size 1 ... std::optional<sluice::IoError>::has_value()
  sluice::async::UringAsyncBackend::submit_request<ReadOp>
  src/async/uring_backend.cpp:436            (thread holds access_mtx_)
Previous write of size 1 ... optional<sluice::IoError>::operator=
  sluice::async::UringAsyncBackend::poison_and_recover_locked
  src/async/uring_transport.cpp:340           (thread holds dispatch_mtx_)
  <- account_transport_result_locked <- submit_transport_locked
  <- poll <- poll_progress <- run_progress_pass_ <- drive_settlement_
  <- shutdown

Write of size 1 ... UringAsyncBackend::retire_execution_resources
  src/async/uring_backend.cpp:401
  <- drive_settlement_ (shutdown)
```

The full report is `tsan-red-prefix.log`; the run is
`uring_submit_poison_race_test` built with `--sanitizers=tsan` from the
base SHA plus this test file only.

## 4. Root cause

Admission probed backend health on the context's `access_mtx_` serialization
domain while the authority for that health is `dispatch_mtx_`: the poison
flag/object and the ring-liveness flag are published and retired under
`dispatch_mtx_` by the owner/shutdown driver, which never takes
`access_mtx_`. A mutex on only one side (the submitter) does not order the
pair, so the reads were a data race (UB); a torn `optional<IoError>` could
also surface a garbage public submit error.

## 5. Minimal implementation

`src/async/uring_backend.cpp`:

- `submit_request`: both health checks now run in one short
  `dispatch_mtx_`-protected critical section; the poison error is copied
  under the lock.
- `retire_execution_resources`: the `have_ring_ = false` publication now
  runs under `dispatch_mtx_`, matching the reading side.

No new state, no atomics, no public-header change. Lock order: the
critical section is taken alone (no nested locks) at a point where the
submitter holds no backend lock; the retirement store takes
`dispatch_mtx_` after `io_uring_queue_exit` and after the eventfd
unregistration, and no caller of `retire_execution_resources()` holds
`dispatch_mtx_` (context destructor releases its scope before the call;
`drive_settlement_` calls it outside any backend lock). No inversion is
introduced. Poison, admission and public error semantics are unchanged:
a submit that wins the lock before retirement/poison proceeds exactly as
before; one that observes the poison returns the recorded
`{backend_error, os_errno}`.

Alternatives rejected:

- **Atomic poison flag + locked error copy**: introduces a second,
  derived summary of `fatal_error_` that must be kept consistent
  (Mechanism Justification Gate / BACKEND-04) and does not by itself
  cover `have_ring_`; no measured need.
- **Remove the probe** (admission relies on `core_->reserve()` returning
  admission-closed): changes the public error from the recorded health
  error to `invalid_state` — a contract-visible behavior change.
- **Move the probe after the `:489` lock**: reservations/preparation would
  then be attempted on a poisoned backend before refusal, changing
  admission semantics and adding work after poison.

## 6. Tests executed and RED/GREEN evidence

Deterministic regression: `tests/uring_submit_poison_race_test.cpp`
(registered as `uring_submit_poison_race_test`; compiled with
`SLUICE_ASYNC_INTERNAL_TESTING` + `SLUICE_HAS_LIBURING` from the same
production TUs as `uring_request_core_integration_test`).

- TSan, debug, base SHA + test only: 5/5 runs report the two pairs above
  (`tsan-red-prefix.log`; `red_final_2..5.log` retained in the round
  workspace with identical pairs).
- TSan, debug, fixed: 10/10 runs, 0 race reports, PASS
  (`tsan-green-postfix.log`).
- Release fixed: 10/10 functional runs PASS.
- Functional semantics case `poisoned_admission_reports_the_recorded_error`:
  after the owner poll poisons, admission returns exactly
  `{backend_error, EIO}`; the abandoned prepared-only operation settles
  with the same error and exactly one `completion_errors` increment; the
  fixed code passes this on every build (the base code also passes it when
  sequenced — the RED evidence is the TSan race, not this value check).

Matrix (all 0 failed):

| Config | Tests |
|---|---|
| release, liburing=y | 82/82 |
| debug, liburing=y | 82/82 |
| release, liburing=n | 60/60 |
| debug, liburing=n | 60/60 |
| asan+ubsan, liburing=y | 82/82 |
| tsan, liburing=y | 82/82 |

Installed-package verification: `scripts/verify_v2_package.py --mode
release --liburing` — all functional gates PASS; `V2_B_PROVENANCE_RECORDED`
reported the expected dirty-worktree failure mid-branch (it requires a
clean tree) and passes at the committed PR head. No installed header or
archive baseline changed (src-only fix + test target).

## 7. Public compatibility assessment

- No installed header changes; no public class/function/layout change.
- `AsyncStats`, public error types and factory signatures untouched.
- Behavior change: none for any legal execution's observable outcome; a
  poisoned/retired backend still refuses admission with the same values.
- Test-only surface: new `SLUICE_ASYNC_INTERNAL_TESTING` target.

## 8. Performance effect

Not measured: the fix adds one uncontended mutex acquisition on the
admission path of a backend whose admission already takes the context
`access_mtx_` and whose dispatch immediately takes `dispatch_mtx_`; no
claim is made in either direction. No benchmark path was changed.

## 9. Remaining assumptions and limitations

- The regression's TSan verdict requires the sanitizer build; non-TSan
  builds run the functional case only.
- `have_ring_` readers other than admission (`has_immediate_physical_work`,
  `internal_work_retired`, `poll`, `progress_port_attached`,
  `signals_physical_progress`) stay unsynchronized by design: they are
  driven on the progress-owner thread, the same thread that drives
  shutdown (THREAD-01 "driving shutdown: Owner only"), so they are
  sequenced rather than racy under the contract. A caller invoking the
  capability query concurrently with another thread's shutdown drive is
  outside the tested contract and is not covered by this fix.
- `UringBackendSubmitTestHooks::before_poison_wait` is declared but never
  invoked in production code (observed while building this reproducer);
  recorded here as an audit note, not changed.
- Formal models do not cover the io_uring health-flag publication; the
  evidence class is deterministic C++ + TSan, consistent with the audit's
  stated validation.

## 10. Review status

PR: see branch `fix/643-aud002-fatal-error-locked-probe` (separate from the
AUD-001 PR). Not merged; Owner gate pending.
