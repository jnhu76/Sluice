# sluice-copy — reference async file copy

A Sluice reference application: an **asynchronous** positional file copy driven
by `ApplicationRuntime` + `ThreadPoolBackend`. It exists under `apps/` (not
`examples/`) because it proves several public APIs compose into a real program,
using installed/public headers only.

Two copy modes share one implementation:

- **Version A** (`--pipeline-depth 1`, the default): one read outstanding at a
  time. Sequential async positional copy.
- **Version B** (`--pipeline-depth N`, N > 1): a bounded reusable-buffer
  pipeline — up to N reads outstanding at once with a single ordered writer.

## Build & run

```sh
xmake build sluice-copy
xmake run sluice-copy [options] <source> <destination>
```

## CLI

```text
sluice-copy [options] <source> <destination>

  --buffer-size <bytes>    per-chunk read/write buffer (default 1 MiB)
  --pipeline-depth <n>     read-ahead slots (default 1)
                           1   = Version A (sequential)
                           >1  = Version B bounded pipeline (multiple
                                 outstanding reads, ordered single writer)
  --workers <count>        ApplicationRuntime worker count (default 1)
  --sync none|data|all     durability policy applied after copy (default none)
  --help                   show this help
```

Defaults: 1 MiB buffer, depth 1, 1 worker, sync=none.

## Exit codes

| code | meaning        |
| ---- | -------------- |
| 0    | success        |
| 1    | usage error    |
| 2    | I/O error      |
| 3    | canceled       |

## Rejections

The CLI rejects: missing/extra operands, zero buffer size, zero pipeline depth,
invalid worker count, unknown sync policy, and source==destination. Same-file
detection uses filesystem identity (device + inode via `fstat`) rather than
path-string equality.

A `buffer_size * pipeline_depth` product that overflows `size_t` is also
rejected (it would exceed the memory upper bound).

## Algorithm

Both versions are one Runtime task. The outer call blocks until the copy
publishes its terminal outcome; there is no new public async surface
(no `CopyHandle`/future). Version A is Version B with `pipeline_depth == 1`.

### Version B — bounded reusable-buffer pipeline

```
allocate pipeline_depth slots, each owning:
    a fixed buffer (buffer_size), a read Completion, a write Completion
    (address-stable for the operation lifetime — L7)
submit up to pipeline_depth initial reads (slots at offsets 0, B, 2B, ...)
loop until all data copied and all EOF reads drained:
    observe cooperative cancellation boundary
    reap the LOWEST-offset outstanding read (other slots' reads stay
        outstanding -> real read/write overlap)
        short read (0 < n < remaining): resubmit within the same slot at
            offset+filled until filled or EOF (the global offset never
            skips an unread region)
        EOF (n == 0): mark the slot at EOF; do not write an empty slot
    write every read_done slot in STRICTLY ASCENDING chunk-offset order
    (min-offset read_done slot selected each round; at most one write
        outstanding in this version):
        partial write (0 < n < remaining): retry within the slot at
            offset+written
        zero write with data remaining: deterministic backend_error
        after a slot is fully written, retire it (EOF) or recycle it to the
            next chunk offset (depth chunks ahead) and submit a fresh read,
            keeping the read window full
on EOF seen: stop submitting new reads; keep writing slots that have data;
    drain every already-submitted read
on any error: save the FIRST meaningful error; stop submitting; drain every
    already-successfully-submitted op; secondary/canceled results never
    overwrite the primary error; submit-failed ops (never entered the
    backend) are not awaited
after data copy:
    sync none:  nothing
    sync data:  submit_sync_data + await + inspect
    sync all:   submit_sync_all  + await + inspect
```

### Correctness properties (both versions)

- multiple outstanding reads when `pipeline_depth > 1` (Version B);
- writes are submitted in ascending file-offset order regardless of read
  completion order (out-of-order reads never reorder writes);
- a slot's buffer is never reused for a new read before its write completes;
- partial reads supported (positional read may return < requested), retried
  within the same slot;
- partial writes supported, including multiple short writes per chunk;
- zero write progress on a non-empty write is a deterministic error, not an
  infinite retry;
- offset overflow and `buffer_size * pipeline_depth` overflow are checked;
- read / write / sync errors propagate through the app-owned result slot;
- cancellation is observed at the cooperative boundaries between operations
  (the copy does NOT claim to interrupt a kernel op already in flight);
- every outstanding operation reaches a terminal state before the task exits;
- outstanding I/O is reaped before Runtime close (`drain()` requires it).

## Memory and concurrency model (Version B)

- Memory upper bound is approximately `buffer_size * pipeline_depth` (one fixed
  buffer per slot) plus the read/write Completions.
- This is **not** zero-copy: each slot buffer is read into and written from.
- Writes are **not** parallel: at most one write is outstanding at a time.
  Parallelism is across reads (read-ahead) and between a read and the
  in-flight write.
- The pipeline is internal; the outermost call still blocks until completion.

## Known limitation

**On a mid-copy failure the destination may be left partial.** sluice-copy does
not use a temporary file + rename. Atomic safe output is a Version C feature.

## What this app proves

- `ApplicationRuntime` lifecycle is usable from a real program;
- a Runtime task can submit async I/O **and cooperatively await** it via
  `RuntimeTaskContext::await_completion` (the M1-A API gap, Candidate A — see
  `docs/design/m1-runtime-io-await-race.md`);
- positional async read/write with partial-I/O handling works end-to-end on a
  real filesystem through `ThreadPoolBackend`;
- a bounded, reusable-buffer pipeline with multiple outstanding reads and an
  ordered single writer composes correctly on top of the same Runtime task
  surface (Version B), with strict Completion lifetime discipline;
- the Runtime's stop/drain/shutdown semantics are usable for a run-to-
  completion workload.

## Not implemented in this slice

- temporary-file safe output (Version C);
- progress display;
- directory traversal (see `sluice-mirror-mini`, a later app);
- io_uring production backend;
- multiple parallel writes (Version B v1 keeps at most one write outstanding).
