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

Cross-buffer correctness is the design intent: a line (and any pattern occurrence
inside it) may straddle any chunk boundary, and the carry is meant to assemble it
before matching so that the result does not depend on buffer size. The
chunk-invariance sweeps that demonstrated this were removed in `5f62b55b` and
have **no current equivalent** — the surviving test target uses a single buffer
size and contains no boundary-straddling case. Read this as the design, not as a
currently verified property (see "What is not covered").

## Output ordering (deterministic)

Files are scanned sequentially in CLI order; lines are emitted in ascending
line order within a file. No parallel-scan output interleaving. Matches
(stdout) are never mixed with diagnostics (stderr).

The per-match prefix depends on two independent switches, and the observed
behaviour is **not** uniform. Measured on the current tree with `hello` in
`g1.txt`/`g2.txt`:

```text
one file,  no -n   ->  hello one                 (no prefix)
one file,  -n      ->  1:hello one               (line number only)
two files, -n      ->  g1.txt:1:hello one        (path, line number)
two files, no -n   ->  g1.txthello one           (path only, NO separator)
```

The multi-file without `-n` row is a divergence, not a design decision: the path
is written and then the line is written immediately after it, so the two run
together (`g1.txthello one`). GNU grep emits `g1.txt:hello one`, and the earlier
revision of this README documented a `path:` prefix. Source:
`apps/sluice-grep/main.cpp` writes the path under `if (prefix_name)` and emits
the `':'` only under `if (prefix_name && args.line_numbers)`, so the single-file
and `-n` shapes are correct while the multi-file without-`-n` shape is not.

This is a **DOC_IMPL divergence with an open product decision**. Which shape is
authoritative — the documented `path:` prefix, or the current concatenated
output — has not been decided, so the README records the real behaviour instead
of restating the old claim. The production writer was deliberately not changed
in this documentation pass. Consumers that need machine-readable multi-file
output should pass `-n`, whose `path:line:` shape is unambiguous; the
multi-file without-`-n` output has no separator between path and match, cannot
be parsed unambiguously, and should not be relied on until a product fix and a
contract decision settle the shape.

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

The usage text advertises `4 KiB..64 MiB` for `--buffer-size` and `<= 64 MiB` for
`--max-line-bytes`, but the parser only rejects `0`, non-numeric values and
`size_t` overflow; the range is enforced afterwards by `grep_files`, which marks
every input `invalid_state`. Because this app's usage exit code is 2, the
observable difference is only the message, not the code — an over-cap value
prints the generic `read error` line instead of the usage block:

```text
--buffer-size 4095       -> exit 2, "g1.txt: read error"
--buffer-size 67108865   -> exit 2, "g1.txt: read error"
--max-line-bytes 0       -> exit 2, usage block
--max-line-bytes 67108865 -> exit 2, "g1.txt: read error"
--workers 65             -> exit 2, usage block
```

`--workers` and the zero checks are the only range checks the parser performs.
Whether the range should move into the parser is an open product decision.

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

One registered target runs under `xmake test` (group `test`), and therefore under
CI's `xmake test -v` in both the debug and release profiles:

- `app_grep_consumption_test` (4 cases) — the `grep_files` engine over the
  canonical `File` resource and a real `ThreadPoolBackend`: per-line matching
  with match count, lines-scanned count, 1-based line numbers and the path handed
  to the sink; result order and sink order across two input files; a dropped
  over-cap line reported via `dropped_long_lines` while the following lines still
  match; and a per-file error (with an empty sink) for an already-closed input.

## What is not covered

- the cross-chunk / chunk-boundary invariance sweeps (a line and a pattern split
  across buffer boundaries) that `tests/sluice_grep_matcher_test.cpp` performed —
  removed in `5f62b55b`; the current target uses a single buffer size (4096) and
  no boundary-straddling case;
- CLI parsing: `parse_args`, the newline-pattern rejection, the `-` operand, and
  the usage block are **not** exercised as a subprocess;
- the multi-file `path:` prefix shape, and therefore the divergence documented
  above;
- final-line-without-newline, empty-pattern and empty-file behaviour, though the
  streaming design and the manual CLI checks above are consistent with the
  documented semantics;
- cancellation (matches already streamed, error surfaced as exit 2).

An earlier README listed `sluice_grep_matcher_test`, `sluice_grep_cli_parse_test`
and `sluice_grep_integration_test`; together with
`sluice_grep_matcher_differential_test` and `sluice_grep_fault_test` all five
sources were removed in `5f62b55b`. They are historical references only.
