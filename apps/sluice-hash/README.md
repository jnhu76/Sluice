# sluice-hash — bounded streaming file hashing

A Sluice application under `apps/`: hashes files with SHA-256 through
`ApplicationRuntime` + `ThreadPoolBackend`, using installed/public headers
only. Unlike `examples/`, this is a real CLI tool.

## Purpose

Proves the "async read -> bounded buffer -> CPU work -> next read" composition
on a real workload: I/O waits and CPU hashing alternate inside one Runtime
task with memory bounded by configuration, not input size.

## Build & run

```sh
xmake build sluice-hash
xmake run sluice-hash [options] <file>...
```

## CLI

```text
sluice-hash [options] <file>...

  --buffer-size <bytes>   read buffer (default 1 MiB; 4 KiB..64 MiB)
  --workers <count>       runtime workers (default 1; <= 64)
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

One `ApplicationRuntime` for the whole batch; ONE task hashes every file in
CLI order:

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
Runtime; the read syscalls run on the ThreadPoolBackend's own bounded worker
pool.

## Resource limits & memory bound

| limit             | value   |
| ----------------- | ------- |
| `kMinBufferSize`  | 4 KiB   |
| `kMaxBufferSize`  | 64 MiB  |
| `kMaxWorkers`     | 64      |

```text
memory ~= 1 x buffer_size + O(1) hasher state
```

independent of file count and file sizes. The limits are enforced twice, and the
two layers disagree about the exit code:

| rejected value                  | rejected by       | CLI result                        |
| ------------------------------- | ----------------- | --------------------------------- |
| `--buffer-size 0`               | CLI parser        | usage error, exit 1               |
| `--buffer-size` non-numeric or `size_t` overflow | CLI parser | usage error, exit 1 |
| `--workers` = 0 or > 64         | CLI parser        | usage error, exit 1               |
| `--buffer-size` < 4 KiB or > 64 MiB | engine validation | per-file `invalid_state`, exit 2 |

Both layers check before any allocation or Runtime build, but only the
`--workers` bound is a CLI usage error. A `--buffer-size` outside
4 KiB..64 MiB parses successfully and is then rejected by
`hash_files`/`run_hash_engine`, which marks **every** input with
`IoError::Code::invalid_state`. The CLI renders that per-file error with its
generic read-error message, so the user sees
`sluice-hash: <file>: read error` and exit 2 — the buffer size is not named and
no read was attempted.

Measured on the current tree:

```text
--buffer-size 4095      -> exit 2, stderr "small.txt: read error"
--buffer-size 4096      -> exit 0, digest printed
--buffer-size 67108864  -> exit 0
--buffer-size 67108865  -> exit 2, stderr "small.txt: read error"
```

This is a documented divergence between the advertised usage text (`4 KiB..64
MiB`) and the actual CLI behaviour. The README does not decide which layer should
own the range: making the range a CLI usage error is a product decision that has
not been taken, and the parser is intentionally left unchanged.

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

Cancellation is observed at the cooperative boundaries between read
operations (the Runtime task checks the cancel token before each submit).
When cancellation is requested, the in-flight read still completes (its
result is consumed per the Completion lifetime contract) and every remaining
file is marked canceled without further I/O. The CLI has no signal handler in
V1 — cancellation is reachable through the Runtime lifecycle, not Ctrl-C.

## What this application proves about Sluice

- a CPU-bound stage composes cleanly between async positional reads inside
  one Runtime task (`submit_read` + `await_completion` loop);
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
  hashing across `--workers`; the flag only sizes the Runtime);
- no SIGINT handling (run-to-completion workload);
- the crypto implementation is app-local on purpose (see above); it is not
  constant-time hardened (a hashing tool is not a secrets-HMAC target) and
  has no side-channel claims.

## Tests

One registered target runs under `xmake test` (group `test`), and therefore under
CI's `xmake test -v` in both the debug and release profiles:

- `app_hash_consumption_test` (5 cases) — the `hash_files` engine over the
  canonical `File` resource and a real `ThreadPoolBackend`:
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
- the `--buffer-size` range divergence documented above is recorded, not tested.

An earlier README listed `sluice_hash_sha256_test`, `sluice_hash_cli_parse_test`,
`sluice_hash_integration_test` and `sluice_hash_fault_test`; all four sources were
removed in `5f62b55b`. They are historical references only.
