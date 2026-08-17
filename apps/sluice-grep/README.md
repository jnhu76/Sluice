# sluice-grep — bounded streaming literal search

A Sluice application under `apps/`: grep-mini (literal substring search)
through `ApplicationRuntime` + `ThreadPoolBackend`, using installed/public
headers only. A real CLI tool, not an example.

## Purpose

Proves bounded streaming scanning: async positional reads feed a CPU matcher
with memory bounded by configuration (never "read whole file, then search"),
with deterministic multi-file output ordering.

## Build & run

```sh
xmake build sluice-grep
xmake run sluice-grep [options] <pattern> <file>...
```

## CLI

```text
sluice-grep [options] <pattern> <file>...

  -n                      prefix each match with its 1-based line number
  --buffer-size <bytes>   read buffer (default 1 MiB; 4 KiB..64 MiB)
  --max-line-bytes <n>    retained-line cap (default 1 MiB; <= 64 MiB)
  --workers <count>       runtime workers (default 1; <= 64)
  --help                  show this help
```

Examples:

```sh
sluice-grep hello file.txt
sluice-grep -n hello file.txt
sluice-grep hello a.txt b.txt
```

## Matching semantics

- **Literal byte-oriented substring** match per line. No regex, no
  case-insensitive mode, no invert.
- `\n` is the only line terminator. The final line without a trailing `\n`
  is still a line (searched and emitted).
- **Empty pattern matches every line** (including empty lines) — the
  documented grep-tradition policy.
- A pattern containing `\n` is rejected up front: a single line can never
  contain one.
- **Binary / invalid UTF-8 policy**: pure byte orientation. NUL bytes do not
  stop the scan (unlike GNU grep's binary detection); invalid UTF-8 passes
  through uninterpreted. No Unicode, locale, or grapheme claims.
- **Long lines**: a line longer than `--max-line-bytes` is not matchable
  within the memory bound, so it is skipped: a diagnostic goes to stderr and
  scanning resumes at the next newline (line numbering stays exact). A
  pattern longer than the cap can never match a retained line.

## Streaming design

```
read chunk (positional async read, one reusable buffer)
   -> LineMatcher.feed: assemble complete lines across chunk boundaries
   -> match each complete line (std::search literal)
   -> deliver matches to the sink immediately (stdout) — never buffered
   -> carry the incomplete trailing line (<= max_line_bytes)
   -> reuse the buffer
```

Cross-buffer correctness: a line (and any pattern occurrence inside it) may
straddle any chunk boundary; the carry assembles it before matching, so the
result is independent of buffer size (proved by the chunk-invariance tests).

## Output ordering (deterministic)

Files are scanned sequentially in CLI order; lines are emitted in ascending
line order within a file. No parallel-scan output interleaving. With more
than one input file each match is prefixed `path:`; `-n` adds `line:`. Matches
(stdout) are never mixed with diagnostics (stderr).

## Resource limits & memory bound

| limit                 | value  |
| --------------------- | ------ |
| `kMinBufferSize`      | 4 KiB  |
| `kMaxBufferSize`      | 64 MiB |
| `kMaxMaxLineBytes`    | 64 MiB |
| `kMaxWorkers`         | 64     |

```text
memory ~= buffer_size + max_line_bytes (line carry) + the match being emitted
```

independent of file sizes and match count (matches stream out, they are not
accumulated).

## Error semantics & exit codes

Traditional grep semantics (deliberately not the unified 0/1/2/3 scheme —
documented deviation per the application track brief):

| code | meaning                                        |
| ---- | ---------------------------------------------- |
| 0    | at least one match found                       |
| 1    | no match                                       |
| 2    | error (usage, unreadable/non-regular input, read failure) |

An unreadable or non-regular input is reported to stderr and skipped; the
remaining files still scan. A read error on one file never stops later files.

## Cancellation

Observed at cooperative boundaries between read operations; matches found
before cancellation were already streamed out. The CLI has no signal handler
in V1 (run-to-completion workload); cancellation surfaces as an error (exit
2) rather than a distinct code.

## What this application proves about Sluice

- bounded streaming scan composes on the same submit/await task surface as
  copy/hash — one reusable buffer, arbitrary file sizes;
- a caller sink receives results synchronously in deterministic order from a
  Runtime task (streaming output without buffering the result set);
- per-file error isolation works across a multi-file batch.

## Known limitations / intentionally NOT implemented

- regex, `-i`, `-v`, `-c`, `-l`, context lines (`-A/-B/-C`), `-z`;
- recursive directory walking, `.gitignore`, glob engines;
- mmap, SIMD matchers (memchr-accelerated line splitting only), PCRE;
- compressed files / archive traversal;
- stdin (`-`) support;
- parallel scanning across files (sequential keeps output deterministic).

## Tests

- `sluice_grep_matcher_test` — chunk-invariance sweeps (cross-buffer lines
  and split patterns), final line without newline, empty file/pattern, empty
  lines, long-line dropping with exact numbering (terminated and
  unterminated);
- `sluice_grep_cli_parse_test` — strict parsing, caps, usage errors exit 2,
  newline-pattern rejection, empty-pattern acceptance, `-` as operand;
- `sluice_grep_fault_test` — FakeAsyncBackend read-error/EOF injection,
  invalid-config rejection;
- `sluice_grep_integration_test` — real files + real backend: multi-file
  deterministic ordering, 1500-line cross-chunk scanning, boundary cases,
  bad-fd isolation.
