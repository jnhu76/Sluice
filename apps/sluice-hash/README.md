# sluice-hash — bounded streaming file hashing

SHA-256 over canonical `File` and `blocking::read_at` on the caller thread.
Files are processed sequentially in input order with one reusable read buffer.
The application and its consumption test link only `sluice_core`.

```sh
xmake build sluice-hash
sluice-hash [--buffer-size <bytes>] <file>...
sluice-hash --help
```

The buffer defaults to 1 MiB and accepts 4096..67108864 bytes. Invalid bounds
and numeric syntax are usage errors before any input is opened. Inputs must be
regular files. An open/read failure is reported on stderr; later files continue.
Each successful file prints `<SHA-256 digest>  <path>` on stdout.

| Exit | Meaning |
|---|---|
| 0 | Every input hashed |
| 1 | Usage error |
| 2 | At least one input failed |
| 3 | Reserved canceled result mapping; no direct cancellation source |

## Migration from the legacy worker bridge

The owner explicitly approved removal for F3 Step 3 on 2026-10-11. `--workers`
is removed, including explicit `--workers 1`. When encountered, it reports
`--workers has been removed; omit it to use direct execution`, prints usage,
and returns 1 before opening inputs. `--workers=<value>` receives the same
migration diagnosis; that spelling was already unsupported. The parser retains
its existing early `--help` return.

Replace `sluice-hash --workers 4 a b` with `sluice-hash a b`. The former worker
setting sized a legacy task runtime and did not promise parallel file hashing.
There is no deprecation window or substitute worker pool.

The app-local `hash_files_with_backend` and the workers argument of `hash_files`
are deliberately removed. In-tree injected callers were test-only. App-header
consumers must call `hash_files(inputs, buffer_size)` for this direct workload.
These headers were not installed SDK interfaces; unknown external source use
is not assumed zero. No injected-backend replacement is provided. Canonical
ThreadPool/io_uring Request execution remains available in the SDK and is
verified by its backend conformance suites.

## Evidence and limitations

`app_hash_consumption_test` retains six direct cases: known digest, multi-buffer
streaming, empty file, closed-file error, input order, and continuation after a
read failure. `tests/hash_cli_oracle.py <binary> --early-bounds` checks hashlib
digests with multiple buffer sizes, output/error isolation, removed workers and
numeric validation. `tests/app_direct_cli_oracle.py hash <binary>` also uses a
FIFO to distinguish rejection before open and checks EOF/multiple buffer sizes.

The SHA-256 implementation remains app-local. The historical full NIST vector
suite was removed in `5f62b55b`; it is not claimed as current evidence. Allocation
failure is not deterministically injected. There is no signal cancellation,
read-ahead or parallel file processing. ApplicationRuntime has been retired.
COPY uses canonical RequestScope, TAIL uses its app-owned driver, and HASH
reads through the direct blocking surface; no Scheduler or Fiber exists in
the tree.
