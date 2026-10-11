# sluice-grep — bounded streaming literal search

Canonical `File` and `blocking::read_at` execute on the caller thread. Files
are scanned sequentially in operand order. The application and its consumption
test link only `sluice_core`.

```sh
xmake build sluice-grep
sluice-grep [options] <pattern> <file>...
```

Options: `-n`; `--buffer-size` (4096..67108864, default 1048576);
`--max-line-bytes` (1..67108864, default 1048576); `--help`.
CLI and engine share app-local bounds validation. Invalid CLI values return 2
before opening operands; help returns 0.

## Matching and output

Literal byte-oriented substring search; LF is the only terminator. CR, NUL and
invalid UTF-8 remain bytes. Empty patterns match every line; empty files have
no lines. A nonempty final line without LF is searched and emitted. CLI patterns
containing LF are rejected. A line exceeding the retained-line cap is skipped,
reported once per affected file, and scanning resumes with correct numbering.

One matcher and scan loop handle cross-buffer carry and EOF. Sink calls stream
in operand order and increasing line order. Filename prefixes count all CLI
operands, including failed opens.

| Operands | -n | Output |
|---|---|---|
| One | No | matched line |
| One | Yes | 12:matched line |
| Multiple | No | file.txt:matched line |
| Multiple | Yes | file.txt:12:matched line |

Exit codes: 0 = match, 1 = no match, 2 = usage/open/read error. Per-file read
failures preserve already emitted output and scanning continues to later files.
EOF finishing is skipped on read error. Sink exceptions preserve earlier file
results; the interrupted and subsequent files receive the translated error:
`bad_alloc` maps to `no_space`, `system_error` retains native errno and canonical
mapping, other exceptions map to `backend_error`.

The reusable buffer, capped carry and per-chunk event storage remain. No
whole-file accumulation or external cancellation handle is introduced. File
ownership ends through RAII with the existing destructor close-error policy.

## Migration from the legacy worker bridge

The owner explicitly approved removal for F3 Step 3 on 2026-10-11. `--workers`
is removed, including explicit `--workers 1`. When encountered, it reports
`--workers has been removed; omit it to use direct execution`, prints usage,
and returns 2 before opening inputs. `--workers=<value>` receives the same
migration diagnosis; this spelling was already unsupported. The existing early
`--help` return remains. Replace `sluice-grep --workers 4 hit a b` with
`sluice-grep hit a b`. There is no deprecation window or substitute worker pool.
The former worker setting did not promise parallel file scanning.

The app-local `grep_files_with_backend` and workers argument of `grep_files`
are deliberately removed. In-tree injected callers were test-only. App-header
consumers must call `grep_files(pattern, inputs, buffer_size, max_line_bytes,
sink)`. These headers were not installed SDK interfaces; unknown external source
use is not assumed zero. No injected adapter replaces this API. Canonical
ThreadPool/io_uring Request execution remains in the SDK.

## Reproducible evidence

`app_grep_consumption_test` retains eight direct semantic cases: matching,
order, long-line skip, closed-file errors, independent byte/chunk expectations,
partial-output/read-failure continuation, config rejection and sink exceptions.
Run `tests/grep_cli_oracle.py <binary> [trace-directory]` for prefix, byte/CRLF/EOF,
cap, ordering, error and bound oracles plus direct thread/open traces. Run
`tests/app_direct_cli_oracle.py grep <binary> [short-read-shim.so]` for removed
workers, FIFO rejection, multi-buffer output and controlled short reads/EIO.

The removed injected canceled-submission test returned `canceled` before
admission; it did not prove an accepted-request cancellation schedule. Canonical
admission/cancellation obligations remain with `async_sync_admission_test`,
`backend_conformance_e1_threadpool_test`, `backend_conformance_e1_uring_test`,
and the backend core-cutover suites. Runtime waiter/observer tests remain for
COPY/TAIL and other consumers. This app exit does not retire ApplicationRuntime
or complete F3.
