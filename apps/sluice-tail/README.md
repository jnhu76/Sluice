# sluice-tail — bounded last-N + follow-mode tail

A Sluice application under `apps/`: finite tailing and long-lived follow
mode through `ApplicationRuntime` + `ThreadPoolBackend`, using
installed/public headers only.

Unlike sluice-copy/hash/grep (finite run-to-completion workloads wrapped in
one blocking call), `tail -f` is a **long-lived wait/event/cancel workload**
— the reason this app exists. It drove the explicit engine lifecycle
(`start` / `request_stop` / `wait`) instead of a single blocking function.

## Build & run

```sh
xmake build sluice-tail
xmake run sluice-tail [options] <file>
```

## CLI

```text
sluice-tail [options] <file>

  -n <count>              last N lines (default 10; 0 = none)
  -f                      follow: keep reading after EOF until Ctrl-C
  --poll-interval <ms>    follow poll cadence (50..5000, default 200)
  --buffer-size <bytes>   scan/read buffer (default 64 KiB; 4 KiB..64 MiB)
  --max-line-bytes <n>    retained-line cap; longer lines are reported and
                          skipped (default 1 MiB; <= 64 MiB)
  --workers <count>       runtime workers (default 1; <= 64)
  --help                  show this help
```

Examples:

```sh
sluice-tail file.log
sluice-tail -n 10 file.log
sluice-tail -f file.log
```

## last-N: bounded backward scan

A 100 GB log is never read forward just to find the last 10 lines:

```
fstat -> size
read descending windows (positional reads, one reusable buffer)
count '\n' backward; stop at the Nth-from-last separator
    (a file-final '\n' closes the last line; it is not a separator)
forward-stream from that offset through the bounded line assembler
```

Memory: `buffer_size + max_line_bytes` — independent of file size and N.

## Follow model (-f)

```
read forward from the post-scan EOF
  -> data: assemble lines, emit
  -> EOF: sleep poll_interval (sliced into 50 ms units so a stop request is
          noticed quickly), fstat, re-read
  -> file shrank (truncate/rotation-in-place): stderr notice, reset to
     offset 0, drop the partial-line carry, continue
  -> SIGINT/SIGTERM: stop
```

- **No busy spin**: while idle the loop sleeps a full poll interval and does
  exactly one stat per wake. Measured idle cost: ~0 CPU (1 scheduler tick
  per 3 s at the 200 ms default; a busy loop would burn ~300).
- **Descriptor-follow semantics**: the open fd is followed, not the path.
  Rotation by rename + new file keeps streaming the ORIGINAL inode;
  `-F` (reopen by name) is a later feature. In-place truncate + rewrite IS
  detected (see above).
- A partial final line is held in the bounded carry and emitted once its
  newline arrives (GNU tail behavior).

## Cancellation (the acceptance-critical path)

Ctrl-C must end a follow through the Runtime lifecycle, not
`std::exit()`:

```
main blocks SIGINT/SIGTERM BEFORE any thread exists
  -> dedicated sigwait thread consumes the signal
  -> calls TailEngine::request_stop() from a real thread
     (request_stop takes locks; a signal-handler context is unsafe for it)
  -> the follow task observes the stop within one poll slice,
     finishes its in-flight Completion per the borrow contract, publishes
  -> main's wait() runs drain + join, then exits 0
```

A signal-ended follow exits **0** — the documented normal end of `tail -f`
(GNU convention), not the unified canceled=3 code.

`wait()` internally issues the Runtime's `request_stop()`, `drain()` and
`join()`. Their return values are **not** propagated: `wait()` reports
`sluice::Result<TailResult>`, and a drain/join failure has no channel through it.
No test observes a drain or join return value either — the two oracle suites
assert only the `TailResult` fields (`stopped_by_cancel`, `lines_emitted`,
`truncation_detected`, `error`) and the process exit code. Treat "a clean signal
exit" as evidence that settlement *completed*, not as evidence that a
settlement *failure* would be reported. Closing that gap needs a public
settlement-outcome surface, which is a product decision.

## Resource limits & memory bound

| limit              | value   |
| ------------------ | ------- |
| `kMinBufferSize`   | 4 KiB   |
| `kMaxBufferSize`   | 64 MiB  |
| `kMaxMaxLineBytes` | 64 MiB  |
| `kMaxLines`        | 10^9    |
| `kMaxWorkers`      | 64      |
| poll interval      | 50–5000 ms |

```text
memory ~= buffer_size + max_line_bytes (line carry)
```

## Error semantics & exit codes

| code | meaning                                             |
| ---- | --------------------------------------------------- |
| 0    | success (a signal-ended follow counts as success)   |
| 1    | usage error                                         |
| 2    | I/O error (open/stat failure, read error in follow) |

Input domain: a single regular file (positional reads need a seekable
source); directories/FIFOs are rejected.

## What this application proves about Sluice

- a LONG-LIVED workload works on the public Runtime lifecycle: the app owns
  start/stop/wait across threads without any private Scheduler access;
- clean signal-driven cancellation composes: sigwait thread ->
  `request_stop()` -> cooperative task end -> `drain()` + `join()` — no
  `std::exit`, no runtime internals;
- bounded backward positional scanning (descending offsets) works through
  the same submit/await surface as forward streaming;
- idle waiting costs ~0 CPU without a timer: there is no app-facing readiness or
  timer API to wait on (a host-level progress-notification fd exists for
  driving the loop, but it reports scheduler progress, not file readiness or a
  deadline), so a bounded poll cadence is what an app can use today.

## Known limitations / intentionally NOT implemented

- `-F` reopen-by-name rotation tracking (descriptor-follow only);
- byte-exact final-newline parity: a final line without a trailing `\n` is
  emitted WITH a newline added (GNU tail preserves the missing newline);
  line-for-line content is identical, byte streams are not;
- multiple files (`tail a b c`) and `-q`/`-v` headers;
- `+N` start-offset syntax, byte modes (`-c`), PID death watching (`--pid`);
- inotify/file-event wakeup (bounded polling only — an app-facing file-readiness
  or timer API does not exist on the current Runtime surface);
- `--follow` with stdin;
- the io_uring backend. `TailEngine::start()` builds a `ThreadPoolBackend`
  unconditionally, so neither this app nor its tests exercise io_uring.

## Tests

Three registered targets run under `xmake test` (group `test`), and therefore
under CI's `xmake test -v` in both the debug and release profiles:

- `app_tail_consumption_test` (5 cases) — the `TailEngine` public API over the
  canonical `File` resource and a real `ThreadPoolBackend`: last-N emission
  (`lines = 3` of 100), `lines = 0` emitting nothing, `start()` rejecting
  `workers = 0` with `invalid_state`, a follow stopped by `request_stop()` with
  `stopped_by_cancel` and no error, and truncation detection after
  `ftruncate(0)` + rewrite.
- `app_tail_lifecycle_oracle_test` (6 cases) — the stop / settlement / teardown
  contracts at the engine layer. It compiles the production `tail_task.cpp`
  unmodified and interposes `pread` in-process (`extern "C"` ELF symbol
  interposition, dev/ino-gated, forwarding everything else via `syscall`), so a
  follow read can be held while a stop is requested. Cases:
  `idle_follow_stops_bounded` (an idle follower's stop completes within a
  deadline), `lifecycle_api_states` (`wait()` before `start()` and a second
  `wait()` are `invalid_state`; `request_stop()` before `start()` is a no-op;
  `poll_interval_ms = 49` refused), `pending_read_settles_before_retire` (an
  accepted read must settle before the task retires — `wait()` is asserted not
  to complete while the read is held, then to complete after release with the
  held read's byte count observed), `append_delivery_and_partial_withholding`
  (a complete appended line is delivered; a newline-less fragment is withheld
  until its newline arrives), `truncate_resets_partial_carry` (truncation
  produces the `file truncated` diagnostic and discards the partial-line carry),
  and `descriptor_follow_across_rename` (the open descriptor is followed, not the
  path: appends to the original inode are delivered, appends to the recreated
  path are not).
- `app_tail_cli_oracle_test` (10 cases) — the same lifecycle contracts through the
  real `sluice-tail` binary, which the suite `fork`/`execve`s from the test
  binary's own directory (it locates the binary via `/proc/self/exe`). Seven
  cases are contract tests: `finite_tail_and_cli_exit_codes` (finite output plus
  the exit-2 and exit-1 paths), `cli_sigint_clean_exit`,
  `cli_sigterm_clean_exit`, `cli_double_signal_clean_exit`,
  `cli_pending_sigint` and `cli_pending_sigterm` (a signal delivered while a read
  is held — the child must stay alive, then exit 0 with the delivered line intact
  and stderr empty, using the `LD_PRELOAD` shim `tests/support/tail_pread_shim.c`
  to hold the read and a fifo command channel to release it), and
  `cli_descriptor_follow_rotation` (the CLI-layer rotation case). Three cases —
  `harness_watchdog_selftest`, `harness_stderr_capture_selftest`,
  `harness_split_line_selftest` — are harness self-checks that validate the
  suite's own watchdog/kill classification, stderr capture and split-line
  assembly; they are not app behaviour.

## What is not covered

- **No test observes a `drain()` or `join()` return value**, or any propagation
  of a settlement failure to the caller (see "Cancellation" above). This is an
  open evidence gap, not a passing guarantee.
- **No test verifies io_uring backend participation.** Both suites, and the app
  itself, run the ThreadPool backend only. A green run says nothing about the
  io_uring profile.
- `--poll-interval` values other than the 50 ms the suites pass explicitly (the
  200 ms default is not the value under test), `--max-line-bytes` boundary
  behaviour, `-n` at the `kMaxLines` cap, and long-line dropping are not
  exercised.
- CLI parsing as such: the suites cover the exit-1 and exit-2 paths they need,
  but there is no dedicated parser test (the removed `sluice_tail_cli_parse_test`
  covered strict parsing and the poll/buffer/line caps).

Five mutations of the follow path were injected by hand as a one-off
discriminating measurement when these oracles were introduced — ignore the stop
token in the follow loop, submit without awaiting, skip the final
drain/join, keep the partial-line carry across truncation, and follow by path
instead of by descriptor. Each was detected by this pair of suites, with the
engine suite and the CLI suite failing different cases. That was a manual check,
not a mutation matrix that `xmake test` repeats, and a mutation kill bounds what
the suites discriminate rather than proving the lifecycle correct in general.

An earlier README listed `sluice_tail_scan_test`, `sluice_tail_cli_parse_test`
and `sluice_tail_cli_integration_test`; all three sources were removed in
`5f62b55b`. The `docs/applications/file-tools-findings.md` document cited by
earlier revisions was removed in the same commit.
