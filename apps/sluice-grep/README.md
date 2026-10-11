# sluice-grep — bounded streaming literal search

The default single-worker invocation uses canonical File and
blocking::read_at on the caller thread. It creates no ApplicationRuntime,
ThreadPoolBackend, Completion, RequestCore or progress owner.

workers=2..64 and grep_files_with_backend remain isolated compatibility
consumers of ApplicationRuntime, await_read_at and Completion. The worker
option still controls runtime workers; files are scanned sequentially in
all invocation forms. Final compatibility retirement belongs to #474/#402.

## Build and run

```sh
xmake build sluice-grep
xmake run sluice-grep [options] <pattern> <file>...
```

Options: -n; --buffer-size (4096..67108864, default 1048576);
--max-line-bytes (1..67108864, default 1048576); --workers (1..64,
default 1); --help. CLI and engine share an app-local range validator.
Invalid CLI values are rejected with exit 2 before opening any operand.

## Matching and output

Literal byte-oriented substring search; no regular expressions or universal
GNU grep compatibility. LF is the only terminator; CR, NUL and invalid UTF-8
remain bytes. Empty patterns match every line; empty files have no lines.
A nonempty final line without LF is searched and emitted. Newline-containing
CLI patterns are rejected. A line exceeding the retained-line cap is skipped,
reported once per affected file, and scanning resumes with correct numbering.

One matcher and scan loop serve both readers, including cross-buffer carry
and EOF handling. Sink calls stream in file operand order and increasing
line order. The file prefix is determined by the number of CLI operands,
including operands that fail to open.

| Operands | -n | Output |
|---|---|---|
| One | No | matched line |
| One | Yes | 12:matched line |
| Multiple | No | file.txt:matched line |
| Multiple | Yes | file.txt:12:matched line |

Exit codes: 0 = match, 1 = no match, 2 = usage/open/read error. Per-file
read failures preserve already emitted output and scanning continues to the
next file. EOF finishing is skipped on read error. Sink exceptions are
translated through the existing app exception policy; completed earlier
file results survive, while the interrupted and subsequent files receive
the translated error. This API exposes no external cancellation handle;
the retained runtime checks its cancel token between files and reads and
settles accepted reads before releasing their borrows.

The existing buffer, capped carry and per-chunk event storage remain. Matches
for a chunk are assembled before sink delivery; no whole-file accumulation
is introduced. File ownership ends through RAII, preserving the existing
policy of not reporting destructor close errors.

## Reproducible evidence

Build and run app_grep_consumption_test plus direct_w01_consumer_probe.
Run the real CLI oracle with the built binary and an optional trace directory:

```sh
python3 tests/grep_cli_oracle.py /absolute/path/to/sluice-grep /tmp/grep-traces
```

The CLI oracle includes prefix combinations, bytes/CRLF/EOF, cross-buffer
matching, long-line skip, ordering, open failures, integer/range failures
with a FIFO negative control, and process thread/open traces. Consumption
tests cover direct, workers=2/64 and injected ThreadPool execution, independent
chunk-boundary expectations, partial output/read failure continuation,
configuration rejection, canceled submission and sink-exception cleanup.
The canceled-submission oracle is rejection evidence, not a claim of an
in-flight cancellation schedule. Runtime cancellation/settlement evidence
remains with runtime_waiter_observer_test.
