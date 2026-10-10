# sluice-hash — bounded streaming file hashing

A Sluice application under `apps/`: hashes files with SHA-256 through
`blocking::read_at` by default, using the canonical `File` resource.
`--workers 2..64` retains the legacy `ApplicationRuntime` + `ThreadPoolBackend`
bridge. Unlike `examples/`, this is a real CLI tool.

## Purpose

The default workload alternates direct positional reads and CPU hashing with
one buffer bounded by configuration, independently of input size.

## Build & run

```sh
xmake build sluice-hash
xmake run sluice-hash [options] <file>...
```

## CLI

```text
sluice-hash [options] <file>...

  --buffer-size <bytes>   read buffer (default 1 MiB; 4 KiB..64 MiB)
  --workers <count>       1: direct; 2..64: runtime (default 1)
  --help                  show this help
```

Examples:

```sh
sluice-hash file.bin
sluice-hash a.bin b.bin
sluice-hash --buffer-size 1048576 file.bin
sluice-hash --workers 4 a b c d
```

Output (stdout): `<digest>  <filename>` per file — the `sha256sum` shape.
Diagnostics go to stderr only.

## Algorithm

SHA-256, implemented app-locally in `sha256.{hpp,cpp}` as a straight FIPS
180-4 §6.2 implementation (standard IV, K constants, message schedule,
compression). It is not a novel hash. Correctness against the NIST test vectors
(empty message, "abc", 448-bit, 896-bit two-block, one-million-'a') and
chunk-boundary invariance was covered by `tests/sluice_hash_sha256_test.cpp`,
which was **removed** in `5f62b55b` and has no current equivalent — see "What is
not covered" below. Keeping the hash app-local avoids widening the repository's
dependency surface for one tool.

## I/O model

With `--workers 1`, reads complete directly on the caller thread; no Runtime,
request domain or backend worker is constructed. With `--workers 2..64`, one
`ApplicationRuntime` task hashes every file in CLI order:

```
for each file (in CLI order):
    loop:
        submit_read(offset, buffer)     # positional async read
        await_completion                # cooperative wait
        sha256.update(bytes read)       # CPU work while I/O is idle
        offset += n                     # any n > 0 is progress; n == 0 = EOF
    final() -> digest
```

Files are processed sequentially in V1 (deterministic, no output
interleaving); short reads simply advance the offset. `--workers` sizes the
legacy Runtime only for counts 2..64; its read syscalls run on the
ThreadPoolBackend's own bounded worker pool. The value 1 selects direct execution,
including when supplied explicitly. No worker count promises parallel files.

## Resource limits & memory bound

| limit             | value   |
| ----------------- | ------- |
| `kMinBufferSize`  | 4 KiB   |
| `kMaxBufferSize`  | 64 MiB  |
| `kMaxWorkers`     | 64      |

```text
memory ~= 1 x buffer_size + O(1) hasher state
```

independent of file sizes (results and opened inputs still scale with file count).
The app-local `valid_config` predicate is shared by the CLI and engine. Buffer
sizes outside 4 KiB..64 MiB and workers outside 1..64 are usage errors (exit 1),
rejected before opening any input. Engine callers receive per-input
`invalid_state`. This implements adopted D-H2: the former out-of-range buffer
behavior (open inputs, then generic read errors and exit 2) changes deliberately.
Integer syntax and overflow rejection, help exit 0 and valid inputs are retained.

## Input domain

Every input must be a **regular file** (positional reads need a seekable,
finite source). An unreadable or non-regular input is reported to stderr and
skipped; the remaining files still hash (error isolation).

## Error semantics & exit codes

| code | meaning                                                  |
| ---- | -------------------------------------------------------- |
| 0    | every input hashed                                       |
| 1    | usage error                                              |
| 2    | at least one input failed (open/read error, non-regular) |
| 3    | canceled                                                 |

A read error on one file never stops later files.

## Cancellation

The direct path runs synchronously and has no cancellation source. In the
retained legacy and injected-backend paths, cancellation is observed at the cooperative boundaries between read
operations (the Runtime task checks the cancel token before each submit).
When cancellation is requested, the in-flight read still completes (its
result is consumed per the Completion lifetime contract) and every remaining
file is marked canceled without further I/O. The CLI has no signal handler in
V1 — cancellation is reachable through the Runtime lifecycle, not Ctrl-C.

## What this application proves about Sluice

- the default path uses direct File I/O without constructing a task runtime;
- the explicitly retained compatibility path still composes async reads and CPU work;
- one reusable buffer serves arbitrarily many files/bytes (memory bound
  independent of input size);
- multi-file batches keep deterministic CLI-order output with per-file error
  isolation;
- the run-to-completion Runtime lifecycle (submit -> wait for task terminal
  -> stop/drain/join) works for a batch workload.

## Known limitations / intentionally NOT implemented

- one algorithm only (SHA-256): no MD5/SHA-1/SHA-3/BLAKE, no `--tag`,
  no check mode (`-c`), no binary-mode marker;
- sequential file processing (no read-ahead across files, no parallel
  hashing across `--workers`; counts 2..64 size the legacy Runtime; 1 selects direct execution);
- no SIGINT handling (run-to-completion workload);
- the crypto implementation is app-local on purpose (see above); it is not
  constant-time hardened (a hashing tool is not a secrets-HMAC target) and
  has no side-channel claims.

## Tests

One registered target runs under `xmake test` (group `test`), and therefore under
CI's `xmake test -v` in both the debug and release profiles:

- `app_hash_consumption_test` (5 cases) — the `hash_files` engine over the
  canonical `File` resource and direct execution:
  a known digest over a real file (`"abc"` →
  `ba7816bf…20015ad`), multi-chunk streaming of 10000 bytes checked against a
  direct in-process digest, the empty-file digest
  (`e3b0c442…7852b855`), a per-file error for an already-closed input, and input
  order preservation across two files.

## What is not covered

The single target above is the whole current test surface for this app. None of
the following has a current test, and none should be read as guaranteed:

- the NIST vector set and chunk-boundary invariance (removed with
  `sluice_hash_sha256_test.cpp` in `5f62b55b`); only the two digests in the
  consumption test are pinned, over a single buffer size (4096);
- CLI parsing: `parse_args`, the strict-integer rejections, the `--workers` cap,
  and the CLI's exit-code mapping are **not** exercised as a subprocess;
- the CLI-level non-regular-input rejection and multi-file order as printed by
  the binary;
- cancellation (the `canceled` → exit 3 path) has no test at any level;
- exception allocation failure and cancellation remain untested.

An earlier README listed `sluice_hash_sha256_test`, `sluice_hash_cli_parse_test`,
`sluice_hash_integration_test` and `sluice_hash_fault_test`; all four sources were
removed in `5f62b55b`. They are historical references only.

The subprocess oracle `tests/hash_cli_oracle.py <binary> --early-bounds` covers
known digests, multi-chunk reads, printed order/error isolation, workers 1/2/64,
strict integer errors, and invalid bounds with a FIFO that must never be opened.
Remote clone traces distinguish the default direct path from the retained
multi-worker path. Runtime source/archive/install retirement remains pending.
