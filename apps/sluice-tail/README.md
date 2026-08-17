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
- idle waiting costs ~0 CPU without any timer API (bounded poll sleep; see
  findings for the timer gap this exposed).

## Known limitations / intentionally NOT implemented

- `-F` reopen-by-name rotation tracking (descriptor-follow only);
- byte-exact final-newline parity: a final line without a trailing `\n` is
  emitted WITH a newline added (GNU tail preserves the missing newline);
  line-for-line content is identical, byte streams are not;
- multiple files (`tail a b c`) and `-q`/`-v` headers;
- `+N` start-offset syntax, byte modes (`-c`), PID death watching (`--pid`);
- inotify/file-event wakeup (bounded polling only — the timer/readiness
  gap is recorded in the findings document, not worked around in
  foundation);
- `--follow` with stdin.

## Tests

- `sluice_tail_scan_test` — last-N (0/1/N, N > total, no final newline,
  empty, 200 KiB bounded backward scan, long-line drop + diagnostic);
  follow (append delivery, clean request_stop termination, truncation
  detection with an ordering anchor that makes the snapshot race-free;
  every case leaves a quiescent engine via an RAII guard);
- `sluice_tail_cli_parse_test` — strict parsing, zero-allowed `-n`,
  poll/buffer/line caps, usage errors;
- `sluice_tail_cli_integration_test` — fork/exec the real binary: finite
  tail output, exit codes, and the SIGINT end-to-end path (append ->
  delivery -> SIGINT -> exit 0).
