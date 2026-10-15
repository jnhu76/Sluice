# sluice-tail — bounded last-N + descriptor follow

The application owns one execution thread. It reads a canonical `File` through
`sluice::blocking::read_at` and uses public headers from `sluice_core` only.
`start`, `request_stop` and `wait` retain the cross-thread lifecycle used by the
CLI's signal waiter. No ApplicationRuntime, Scheduler, Fiber or request backend
participates in TAIL.

## Build and CLI

```sh
xmake build sluice-tail
xmake run sluice-tail [options] <file>
```

```text
-n <count>              last N lines (default 10; 0 = none; <= 10^9)
-f                      follow until SIGINT/SIGTERM
--poll-interval <ms>    follow poll cadence (50..5000, default 200)
--buffer-size <bytes>   scan/read buffer (4 KiB..64 MiB, default 64 KiB)
--max-line-bytes <n>    retained-line cap (1..64 MiB, default 1 MiB)
--help                  show help
```

`--workers` has been removed. Omit it to use the app-owned driver.
All spellings and values are rejected with usage exit 1 before file open.
Existing scripts passing the former compatibility hint must remove it. Exit codes remain 0 for
success (including signal-ended follow), 1 for usage errors, and 2 for I/O or
engine failures. The CLI accepts one regular file and rejects directories/FIFOs.

## Scanning and following

The initial scan reads descending positional windows to locate the last N lines,
then streams forward through a bounded line assembler. A file-final newline
closes the last line; it does not add an empty line. `-n 0` emits no initial data.
A final initial line without a newline is emitted with a newline added, preserving
the application's existing output shape.

Follow reads from the post-scan offset. Appended partial lines remain in the carry
until a newline arrives. Lines exceeding `--max-line-bytes` are skipped with a
stderr diagnostic. At EOF the driver waits on a condition variable for the poll
interval or a persistent stop predicate. On each elapsed interval it checks size;
a shrink resets the offset and discards the partial carry with a truncation notice.
The open descriptor follows its original inode after rename; a replacement file
at the original path is not followed. File growth is polled, not event driven.

The scan buffer and bounded carry do not grow with file size or N. A read's
assembled output is bounded by the scan buffer; no whole-file line collection
is retained.

## Stop, result and lifetime

The CLI blocks SIGINT/SIGTERM before starting threads. A dedicated `sigwait`
thread calls `request_stop`; this method is not suitable for a signal handler.
Stop wakes an idle poll immediately, including at the 5000 ms interval. It is
cooperative: a filesystem call or user callback already running must return.
There is no hard stop deadline for an arbitrary regular-file syscall.

`wait()` joins the execution thread before returning a `TailResult`. Task/read/
callback errors occupy `TailResult::error`; lifecycle errors and a join
`system_error` occupy the outer `Result<TailResult>`. A second wait, a repeated
start, a wait before successful start, and a callback attempting to wait on its
own driver return outer `invalid_state`. A failed start leaves the engine
unstarted and may be retried. Pre-start stop remains a no-op.

Concurrent start/stop/wait calls are serialized by the engine's mutex; exactly one
start and one external wait can succeed. Destruction requires callers to have
finished their external engine calls. It requests stop and joins before releasing
File, read buffer, sinks or driver state. A callback must not destroy its own
engine. Held-read and held-callback tests observe that destruction cannot finish
before physical access/callback return.

## Evidence and limitations

- `app_tail_consumption_test`: 9 cases covering last-N, zero lines, validation,
  stop, truncation, repeated start, start rollback, scan-window boundaries,
  initial partial lines and long-line dropping.
- `app_tail_lifecycle_oracle_test`: 12 cases, including controlled start/wait
  competition, callback self-wait/stop and exception, held-read settlement/File
  lifetime, held-callback destruction, 5000 ms idle-stop wake, follow partial
  withholding, truncation and descriptor rotation. Its test-only ELF `pread`
  interposer holds a syscall borrow without changing production code.
- `app_tail_cli_oracle_test`: 11 cases, including real SIGINT/SIGTERM, double
  signal, held-read signal settlement, rotation, compatibility worker validation
  and 5000 ms poll interruption. Three cases self-check the subprocess harness.

These are direct execution tests in both build profiles; a liburing-enabled build
does not make TAIL use io_uring. Thread creation failure and join failure are not
fault-injected. Exhaustive allocation failure, `-n` at its maximum, inotify,
`-F` name reopening, stdin, multiple files, byte modes and byte-exact missing-final-
newline parity remain outside the evidence or implemented feature set.

See [the F3 slice record](../../docs/review/f3-tail-owned-driver-20261011.md) for
requirements, protocol, contraction audit and exact validation commands.
