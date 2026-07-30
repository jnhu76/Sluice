# sluice-copy — reference async file copy (M1-A, Version A)

The first real Sluice reference application: a sequential **asynchronous**
positional file copy driven by `ApplicationRuntime` + `ThreadPoolBackend`. It
exists under `apps/` (not `examples/`) because it proves several public APIs
compose into a real program, using installed/public headers only.

## Build & run

```sh
xmake build sluice-copy
xmake run sluice-copy [options] <source> <destination>
```

## CLI

```text
sluice-copy [options] <source> <destination>

  --buffer-size <bytes>   per-chunk read/write buffer (default 1 MiB)
  --workers <count>       ApplicationRuntime worker count (default 1)
  --sync none|data|all    durability policy applied after copy (default none)
  --help                  show this help
```

Defaults: 1 MiB buffer, 1 worker, sync=none.

## Exit codes

| code | meaning        |
| ---- | -------------- |
| 0    | success        |
| 1    | usage error    |
| 2    | I/O error      |
| 3    | canceled       |

## Rejections

The CLI rejects: missing/extra operands, zero buffer size, invalid worker
count, unknown sync policy, and source==destination. Same-file detection uses
filesystem identity (device + inode via `fstat`) rather than path-string
equality.

## Algorithm (Version A — sequential)

```
offset = 0
loop:
    observe cooperative cancellation boundary
    submit positional read          (RuntimeTaskContext::submit_read)
    cooperatively await Completion  (RuntimeTaskContext::await_completion)
    inspect terminal result
    if EOF (bytes_read == 0): break
    while consumed < bytes_read:
        observe cancellation boundary
        submit positional write for remaining bytes
        cooperatively await Completion
        inspect terminal result
        if zero progress: return deterministic error
        consumed += bytes_written
    offset += bytes_read            (overflow checked)
    reset Completions (only after ready + result consumption)
after data copy:
    sync none:  nothing
    sync data:  submit_sync_data + await + inspect
    sync all:   submit_sync_all  + await + inspect
```

### Correctness properties

- partial reads supported (positional read may return < requested);
- partial writes supported, including multiple short writes per chunk;
- zero write progress on a non-empty write is a deterministic error, not an
  infinite retry;
- offset overflow is checked;
- read / write / sync errors propagate through the app-owned result slot;
- cancellation is observed at the cooperative boundaries between operations
  (the copy does NOT claim to interrupt a kernel op already in flight);
- every outstanding operation reaches a terminal state before the task exits;
- outstanding I/O is reaped before Runtime close (`drain()` requires it).

## Known Version-A limitation

**On a mid-copy failure the destination may be left partial.** Version A does
not use a temporary file + rename. Atomic safe output is a Version C feature.

## What this app proves

- `ApplicationRuntime` lifecycle is usable from a real program;
- a Runtime task can submit async I/O **and cooperatively await** it via
  `RuntimeTaskContext::await_completion` (the M1-A API gap, Candidate A — see
  `docs/design/m1-runtime-io-await-race.md`);
- positional async read/write with partial-I/O handling works end-to-end on a
  real filesystem through `ThreadPoolBackend`;
- the Runtime's stop/drain/shutdown semantics are usable for a run-to-
  completion workload.

## Not implemented in this slice

- bounded pipeline (Version B);
- multiple reusable buffers (Version B);
- temporary-file safe output (Version C);
- progress display;
- directory traversal (see `sluice-mirror-mini`, a later app);
- io_uring production backend.
