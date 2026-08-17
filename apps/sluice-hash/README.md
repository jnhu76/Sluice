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
compression). It is not a novel hash; correctness is anchored by the NIST
test vectors (empty message, "abc", 448-bit, 896-bit two-block, one-million
'a') in `tests/sluice_hash_sha256_test.cpp`, plus chunk-boundary invariance
tests. Keeping it app-local avoids widening the repository's dependency
surface for one tool; see `docs/applications/file-tools-findings.md` for the
swap-out/promotion discussion.

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

independent of file count and file sizes. Out-of-range values are usage
errors (exit 1) on the CLI and per-file `invalid_state` from the engine
entry points, checked before any allocation or Runtime build.

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

- `sluice_hash_sha256_test` — NIST vectors + million-'a' + chunk-boundary
  invariance;
- `sluice_hash_cli_parse_test` — strict parsing, overflow, caps, wiring;
- `sluice_hash_fault_test` — FakeAsyncBackend error/EOF injection,
  invalid-config rejection without running;
- `sluice_hash_integration_test` — real files + real backend: known digests,
  multi-buffer streaming, multi-file order, bad-fd isolation.
