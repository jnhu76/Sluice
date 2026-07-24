# Security Review Follow-up Issues

This document records issues surfaced during the Sluice security review that
were **deliberately deferred** rather than fixed in the current change set.

It is deliberately not a "list of currently exploitable severe vulnerabilities."
Every item below has been reviewed against the real code, the existing tests,
and the automated findings under `.c-review-results/`. The current set of
commits already closes the issues required to meet this round's merge gate.
The items listed here are deferred because each one needs new API surface, a
contract decision, cross-platform validation, or independent design work that
is out of scope for a focused security repair.

## Status

* **Current.** The follow-up items below are open by decision, not by
  oversight.
* **Merge gate.** The changes shipped in this round close the findings
  required for the current gate (bounded WAL allocation growth, native length
  / offset validation, io_uring submit-failure exactly-once completion,
  experimental EINTR / close / CQE handling, Group exception-safe draining,
  Release build select invariants, sanitizer fiber lifecycle, opt-in hardened
  release profile).
* **Not promised here.** None of the deferred items has a target date. They
  are revisited when their explicit *revisit trigger* fires.

## Scope and trust assumptions

Sluice is, by `AGENTS.md` §1, an I/O control-flow library:

* `sluice_core` exposes synchronous `Reader` / `Writer`, WAL, file I/O,
  positional I/O, durability, and `BlockingIoPool`.
* `sluice_async` is an opt-in runtime.
* io_uring is an **optional, off-by-default** experimental backend.

The current security posture assumes:

* file paths come from the **application**, which is trusted to validate them;
* the WAL is an **application-owned local test format**, not an untrusted-file
  parser and not a database recovery format;
* `BlockingIoPool::worker_count` is supplied by trusted application
  construction parameters;
* callers that need durability confirmation use the explicit
  `sync_data()` / `sync_all()` APIs.

The deferred items below are about *changing* one of those assumptions, not
about defects in code that already runs under them.

## Follow-up items

### WAL parsing trust boundary

**Current behavior.** `sluice::wal::read_record` is documented in
`include/sluice/wal.hpp` and `README.md` (en + zh-CN) as an application-owned
local test format, not an untrusted-file parser.

**What the current change closed.** Finding `DOS-001` reported that a 4-byte
`length` header caused `read_record` to allocate the whole declared length up
front. `read_record` now grows the payload in bounded chunks
(`detail::read_chunk_size`, 64 KiB steps) before any of those bytes arrive, and
maps a `std::bad_alloc` to `backend_error`. A corrupt short file with a
malicious `0xffffffff` header can no longer trigger one ~4 GiB allocation from
its declaration alone. This is verified by
`wal_extreme_declared_length_does_not_allocate_up_front` and
`wal_read_chunk_size_is_bounded_and_overflow_free`.

**What remains.**

* The on-disk length field is still `u32`; format compatibility is unchanged.
* `read_record` does **not** impose a record-size policy below the on-disk
  `u32` limit.
* A genuinely present, multi-gigabyte record can still consume the
  corresponding memory.
* There is no commitment that `read_record` safely parses untrusted or
  externally supplied WAL input.

**Why deferred.** A correct, non-breaking fix is an API/contract decision,
not a one-line guard.

**Future direction.**

* Add an explicit `ReadOptions` / `max_record_length` parameter.
* Separate trusted-local reading from untrusted-input parsing as distinct
  APIs with different contracts.
* Always perform an upper-bound check before any large allocation.

**Revisit trigger.**

* WAL is used for database recovery.
* WAL may originate from upload, sync, network, backup import, or untrusted
  disk.
* The project begins promising to parse arbitrary WAL files.

### Secure filesystem open API

**Current behavior.** `FileReader`, `FileWriter`, and the experimental
`UringIoContext::write_file_all` retain ordinary POSIX symlink-following open
behavior (findings `FS-001`, `FS-002`). There is no evidence that these APIs
currently sit at a privileged-directory security boundary.

**What the current change did *not* do.**

* It did **not** mechanically add `O_NOFOLLOW`. `O_NOFOLLOW` only constrains
  the *final* path component and does not provide complete path safety.
* It did not adopt `lstat()` followed by `open()`: that is a TOCTOU and is
  explicitly rejected as a future solution.

**Why deferred.** A trustworthy solution requires a designed secure-open API,
not a flag sprinkled across constructors.

**Future direction.**

* Add an explicit secure-open API rooted at a trusted directory fd.
* On Linux, evaluate `openat2()` (`RESOLVE_*`) for path-resolution restrictions.
* Distinguish, as separate API choices: follow symlink / reject final
  component / reject all path traversal / secure create-or-replace.

**Revisit trigger.**

* File paths come from untrusted users.
* The process runs with elevated privileges.
* The API is used for sensitive configuration, secrets, logs, or system
  directories.
* A caller requires "create only inside this directory."

### Error-reporting close/finalize API

**Current behavior.** `FileReader::close()` / `FileWriter::close()` keep their
backward-compatible `void` interface. Destructors remain `noexcept` and
best-effort; they do not invent unreportable I/O success and do not add hidden
final flushes (per `AGENTS.md` §7).

**What the current change closed.** The *experimental* path
(`UringIoContext::write_file_all`, finding `ERR-002`) now observes the terminal
`::close()` result through its existing `Result<UringWriteResult>` channel,
preserving the primary write error when both occur. `UringWriteBatch::write_all`
now retries `io_uring_wait_cqe` on `EINTR` (finding `EINTR-001`) and maps a
negative wait result to its errno rather than a generic `backend_error`.

**What remains.**

* Writer durability errors are still observed through explicit
  `sync_data()` / `sync_all()`.
* On Linux, a failed `close()` must not be retried naively: the fd may already
  have been released and reused, so a retry could close another thread's
  descriptor.
* The production `void close()` cannot surface a delayed write error.

**Why deferred.** Surfacing a terminal close error on the production path is a
public API change that needs deliberate approval.

**Future direction.**

* Add a new explicit `finish()` / `finalize()` or `Result<void>`-returning
  close API.
* Keep the old `void close()` as a compatibility interface.
* Clarify the distinct guarantees of flush, sync, close, and durability.
* Define writer final-error precedence.

**Revisit trigger.**

* `FileWriter` is used for data whose on-disk confirmation is mandatory.
* Callers need to observe delayed write errors.
* A public API major-version adjustment is planned.

### BlockingIoPool resource policy

**Current behavior.** `BlockingIoPool::worker_count` is provided by trusted
application construction parameters. There is no arbitrary hard thread-count
cap. Mid-construction failure already has shutdown, rollback, and join handling.

**Why deferred.** A cap baked into the low-level primitive would be
unprincipled; resource policy belongs to the application.

**Future direction.**

* Add application-level configuration validation.
* Provide a `hardware_concurrency`-based recommendation factory.
* Provide a deployment budget / policy object.
* Keep the low-level primitive separate from application default policy.

This document deliberately does **not** recommend hard-coding limits such as
256, 512, or 1024 without evidence.

**Revisit trigger.**

* The `worker_count` can be controlled by user input, config file, environment
  variable, or network input.
* The project begins shipping an out-of-the-box server/runtime configuration.
* A unified resource budget is required.

### io_uring outstanding-operation lifecycle

**Current behavior.** The io_uring backend is `sluice_experimental_uring`,
off by default (`AGENTS.md` §1, §6.5).

**What the current change closed.** The submit-fatal state machine now:

* tracks an ordered `pending_sqes` queue and per-`OpRec` `submitted` flag;
* classifies `io_uring_submit()` results as transient
  (`-EINTR` / `-EAGAIN` / `-EBUSY`), a retryable single zero-progress, or a
  permanent failure (first permanent negative error, or a second consecutive
  zero-progress with pending SQEs);
* on the permanent transition, completes the provably unsubmitted operation
  suffix **exactly once** with the stored error, leaves the kernel-owned prefix
  to complete from its real CQEs, makes later submissions fail synchronously,
  and makes `wait_one()` return the stored error instead of blocking forever.

This matches the liburing contract that a positive `io_uring_submit()` return
is the exact count of SQEs the kernel accepted (the accepted prefix), and a
negative return is `-errno`. Verified by `uring_submit_failure_test.cpp`.

**What remains.**

* An operation already accepted by the kernel must still wait for its real
  CQE; no CQE is synthesized and no completion is duplicated.
* If an accepted I/O blocks indefinitely, the lifecycle still depends on the
  caller closing the resource, cancel support, and the existing L11
  outstanding-operation contract.
* The current fix does not change the public API/ABI.

**Why deferred.** A formal bounded-shutdown policy is a separate design effort.

**Future direction.**

* Define the backend shutdown policy.
* Investigate cancel-all or resource-close cooperation.
* Add a shutdown deadline.
* Define the final behavior for operations that cannot be canceled.
* Build a more formal state model for backend-fatal vs operation-fatal.

**Revisit trigger.**

* The io_uring backend is promoted from experimental/optional to the default
  production backend.
* Bounded shutdown is required.
* A service requires every Future to complete within a fixed time after
  shutdown.
* A real case appears of a kernel-accepted operation whose resource cannot be
  closed.

### Sanitizer fiber portability

**Current behavior.** The custom fiber stack now integrates both TSan fiber
lifecycle (`__tsan_create_fiber` / `__tsan_switch_to_fiber` /
`__tsan_destroy_fiber`) and ASan stack-switch lifecycle
(`__sanitizer_start_switch_fiber` / `__sanitizer_finish_switch_fiber`), plus a
terminal `context_switch_final` and a sanitizer-aware `reset_context`.

**What was verified.** x86_64, Linux/WSL2, Clang 21, compiler-rt TSan 21.

**What is not covered.**

* ARM.
* macOS.
* Windows.
* GCC sanitizer runtimes.
* Other asm context-switch ABIs.

**Future direction.**

* Build a platform CI matrix.
* Add sanitizer feature detection.
* Make unsupported runtimes an explicit configuration error or a documented
  skip.
* Verify terminal switch and fiber-destroy lifecycle under each ABI.

**Revisit trigger.**

* A new ARM / macOS / Windows fiber backend is added.
* The sanitizer runtime is switched.
* CI gains a new architecture.
* The context-switch assembly changes.

### Missing worker-1 audit provenance

**Current behavior.** `.c-review-results/20260724T011027Z/run-summary.md`
records:

```text
worker-1 | buffer-write-sinks | 8 | completed
```

**What is missing.** The run artifacts do not contain a matching finding ID,
coverage note, index, finding file, or per-item evidence for worker-1:

* `coverage/worker-1.md` — absent.
* `findings/index-worker-1.*` — absent.

The 8 finding IDs for worker-1 cannot be reliably recovered from the current
artifacts. This round re-checked buffer-write sinks by worker class only; no
new `memcpy`, overlap, unsafe-string, or format-sink problem was found in that
re-check. No finding IDs are fabricated for worker-1.

**Future direction.**

* Re-run the original worker.
* Emit a manifest, finding IDs, coverage, and checksums from each worker.
* Add a CI check that `run-summary.md` agrees with the artifact count.
* Forbid marking the review `complete` while a worker result is missing.

**Revisit trigger.**

* Complete audit provenance is required.
* This report is used for external certification or a formal security
  assertion.
* The original scan pipeline is re-run.

## Explicitly accepted current behavior

The following behaviors are accepted for this round and are not defects under
the current trust assumptions. They may change via new API or contract later,
but this round does not change them:

* General file APIs default to following symlinks (`FS-001`, `FS-002`).
* The WAL is a trusted local format, not an untrusted parser (`DOS-001`
  closed only the allocation-amplification vector).
* `void close()` remains the compatibility interface for production
  `FileReader` / `FileWriter`.
* `BlockingIoPool` resource limits are the application's responsibility.
* An operation submitted to and indefinitely blocked in the kernel is not
  fabricated as complete.
* The worker-1 original evidence cannot be recovered from the existing
  artifacts.

## Revisit priority

| Item                          | Suggested priority             |
| ----------------------------- | ------------------------------ |
| io_uring bounded shutdown     | Medium, before default production enablement |
| Secure-open API               | Medium, before untrusted paths or privileged boundaries |
| Result-returning finalize API | Medium, before durability-critical use |
| WAL max record option         | Low, before accepting external WAL input |
| BlockingIoPool policy         | Low, before configuration externalization |
| Sanitizer portability         | Medium, when extending platform CI |
| Worker-1 provenance           | Medium, before the next formal security audit |

No dates are committed.

## Related review

* Automated findings and run summary: `.c-review-results/20260724T011027Z/`
  (not part of the repository source tree; not modified by this change set).
* Repository operating contract: `AGENTS.md` §1 (architectural boundaries),
  §7 (core I/O contracts), §8 (async invariants), §10 (security and review
  findings).
* Public WAL contract: `include/sluice/wal.hpp`, `README.md`,
  `README.zh-CN.md`.
