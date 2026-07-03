# liburing validation — 2026-07-03 (HONESTLY PENDING)

**Status: NOT RUN.** This is an honest pending record, not a fabricated result.

## Why pending

liburing is **not installed** in this development environment, so the
experimental io_uring path could not be validated against a real kernel ring
here. Fabricating a result would violate the project's no-claims discipline, so
this file records the pending state instead.

## Environment

- Host: WSL2 (Linux 6.18.33.2-microsoft-standard-WSL2 x86_64)
- liburing: **NOT FOUND** (`pkg-config --modversion liburing` empty; no
  `/usr/include/liburing/io_uring.h`)
- Kernel: 6.18 — io_uring is supported by the kernel in principle, but the
  userspace library is absent.
- xmake offered to fetch liburing 2.14 from xrepo, but the fetch was not
  confirmed/completed in this session.
- Commit at time of record: `d85ee55` (CPPIO-CORE-014C)

## What was verified (the stub path)

Without liburing the project builds and the 35-test suite passes (see 014G
closeout). The experimental targets compile as unsupported stubs:

- `experimental_uring_write` → prints `SKIPPED (liburing unavailable)`, exit 0.
- `uring_write_bench` → emits `uring_write_batch_SKIPPED_NO_LIBURING` rows.
- uring tests assert the clean `backend_error` skip.

## What was NOT verified

- Real `io_uring_setup` / submit / completion on this kernel.
- Real write correctness (bytes match) through the uring path.
- Real `UringStats` increments.
- Any measured comparison between blocking and uring write paths.

## How to complete this validation

Follow `docs/liburing-validation-runbook.md`: install liburing, reconfigure with
`--with-liburing=true`, rebuild, run tests/example/bench. Replace this file with
a real result (or add a dated sibling) once run. **Do not promote io_uring** in
the decision matrix based on a stub — only on a real, repeated, liburing-equipped
run per `docs/optimization-runbook.md`.
