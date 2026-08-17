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

Two output modes:

- **Version C** (the default): safe atomic output — the copy lands in a
  uniquely-named temp file in the destination's directory and the destination
  is replaced by a single `rename()`. A failure anywhere before the rename
  leaves an existing destination completely untouched.
- **`--no-atomic`**: the original direct-write output (a mid-copy failure may
  leave a partial destination).

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
  --no-atomic              direct destination write (old Version A/B output);
                           default is the Version C temp+rename safe path
  --help                   show this help
```

Defaults: 1 MiB buffer, depth 1, 1 worker, sync=none.

### Resource limits

The CLI and the public copy entry points (`run_pipelined_copy*` in
`copy_task.hpp`) enforce fixed, explainable app-level limits — the memory
upper bound of the pipeline is approximately `buffer_size * pipeline_depth`,
and the TOTAL pipeline allocation is the primary constraint:

| limit                    | value  | meaning                                      |
| ------------------------ | ------ | -------------------------------------------- |
| `kMaxWorkers`            | 64     | Runtime worker threads (OS threads)          |
| `kMaxBufferSize`         | 64 MiB | per-slot read/write buffer                   |
| `kMaxPipelineDepth`      | 64     | number of pipeline slots                     |
| `kMaxPipelineBytes`      | 512 MiB | total `buffer_size * pipeline_depth` budget |

Values beyond a limit are a usage error (exit 1) on the CLI and
`invalid_state` from the public entry points, checked before any allocation.
This is a reference copy app, not an arbitrary thread/allocator factory.

### Input domain: regular files only

Both source and destination must be **regular files**. The Version B pipeline
needs a seekable, finite-length source that eventually reaches EOF and a
truncatable, positional destination; FIFOs, sockets, and character devices
(e.g. `/dev/zero`) do not fit that domain. The source's type is checked via
`fstat` immediately after opening and **before** the destination is created,
so a rejected source never creates a new destination and never truncates an
existing one. Source == destination (same device + inode, including hard
links) is rejected before any truncation.

Note: `open(source, O_RDONLY)` itself may block for a FIFO with no writer —
that happens before the type check can run.

## Exit codes

| code | meaning        |
| ---- | -------------- |
| 0    | success        |
| 1    | usage error    |
| 2    | I/O error      |
| 3    | canceled       |

## Rejections

The CLI rejects: missing/extra operands, zero buffer size, zero pipeline
depth, invalid worker count, unknown sync policy, source==destination,
non-regular sources/destinations, and any value beyond the resource limits
above. Integer parsing is strict: negative numbers, signs, trailing junk
(`123abc`, `1MiB`), and values that overflow `size_t` are usage errors — no
silent narrowing or truncation.

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
- a backend op-dispatch failure (e.g. a worker-thread spawn failure under
  resource exhaustion) surfaces as an `IoError::backend_error` result: the
  copy task translates ANY task-body exception into an error, so a copy can
  never hang waiting for a result that was silently swallowed;
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

**With `--no-atomic`, a mid-copy failure may leave the destination partial or
truncated.** This is the old Version A/B output path, kept for comparison; it
is not the default.

## Version C — atomicity and durability scope (default output mode)

Destination lifecycle (`safe_output.{hpp,cpp}`):

```
validate source (regular file) + destination (if it exists: regular file,
    not the same inode as the source)
mkstemp("<dst_dir>/.sluice-copy.tmp.XXXXXX")      # same filesystem
fchmod(temp, src_mode & 0777)
pipelined copy src -> temp fd                     # Version A/B engine
sync policy (data/all) applies to the TEMP fd     # copy task, before rename
close(temp)
rename(temp, dst)                                 # the atomic replacement
--sync data|all only: fsync(dst_dir)              # make the rename durable
```

Guaranteed:

- an existing destination is **never** visible with partial content — every
  failure before the rename leaves it byte-identical to before;
- cancellation, read/write/sync errors, temp cleanup failures: destination
  untouched, temp file unlinked;
- the rename is atomic within one filesystem (the temp file is created in the
  destination's own directory).

NOT guaranteed (documented scope):

- `--sync none`: no durability claim at all. The rename is still atomic, but a
  crash may lose the new content or revert to the old file;
- metadata preservation is **permission bits only** (`src_mode & 0777`; the
  setuid/setgid/sticky bits are deliberately dropped, umask is not applied).
  Owner, group, timestamps, ACLs, and xattrs are NOT preserved;
- a **symlink destination**: the symlink's *target* is validated (and rejected
  if it is the source itself), but the rename replaces the LINK with the new
  regular file — the target is never written through;
- with `--sync data|all`, a *directory* fsync failure is reported as exit 2
  **after** the rename already happened: the destination holds the new
  content; only the crash-durability of the rename is missing;
- no recursive copy, no reflink, no sparse handling, no delta transfer.

## Durability scope (`--sync`)

In the default (Version C) output mode, `sync=data` / `sync=all` apply
`fdatasync` / `fsync` to the **temp file** before the rename and `fsync` the
**parent directory** after it, so both the copied data and the replacement
itself survive a crash. With `--no-atomic` the sync applies to the destination
file descriptor only (no directory fsync — the old behavior). `--sync none`
makes no durability claim in either mode. This app does not preserve ACLs,
ownership, xattrs, or other metadata beyond the permission bits.

## What this app proves

- `ApplicationRuntime` lifecycle is usable from a real program;
- a Runtime task can submit async I/O **and cooperatively await** it via
  `RuntimeTaskContext::await_completion` (the M1-A API gap, Candidate A — see
  `docs/history/implementation-plans/m1-runtime-io-await-race.md`);
- positional async read/write with partial-I/O handling works end-to-end on a
  real filesystem through `ThreadPoolBackend`;
- a bounded, reusable-buffer pipeline with multiple outstanding reads and an
  ordered single writer composes correctly on top of the same Runtime task
  surface (Version B), with strict Completion lifetime discipline;
- the Runtime's stop/drain/shutdown semantics are usable for a run-to-
  completion workload.

## Not implemented in this slice

- owner/group/timestamp/ACL/xattr preservation (Version C preserves permission
  bits only);
- O_NOFOLLOW-style symlink rejection for the final destination component
  (a symlink destination is atomically replaced — see Version C scope);
- progress display;
- directory traversal (see `sluice-mirror-mini`, a later app);
- io_uring production backend;
- multiple parallel writes (Version B v1 keeps at most one write outstanding).

## Test targets

The Version B test family (all in the default `xmake test` group):

- `scripted_backend_test` — the deterministic test backend itself
  (cancel idempotency, staged accounting, controller lifetime);
- `sluice_copy_pipeline_contract_test` — pipeline contracts against the
  scripted backend (read-ahead, write order, short reads/writes, drains,
  bounded memory across multiple slot-reuse rounds, allocation failure,
  bounded-failure watchdog);
- `sluice_copy_pipeline_integration_test` — real files + ThreadPoolBackend
  (exact destination size, multi-round reuse, multi-worker, sync policies,
  real concurrency probe). The buffer-size matrix derives each case size
  from buffer/depth and the slot-reuse rounds (a few hundred ops in the
  default group); `SLUICE_PIPELINE_BUFSTRESS_N=<n>` re-opts a run into the
  explicit large workload (`100003` in the Version B nightly gate), and
  honors only a fully-valid positive integer — anything else falls back to
  the small matrix size;
- `sluice_copy_pipeline_stress_test` — deterministic randomized matrix
  (`--seed` / `--iterations`);
- `sluice_copy_integration_test` / `sluice_copy_fault_test` — Version A
  integration and fault injection;
- `sluice_copy_cli_parse_test` / `sluice_copy_file_domain_test` — CLI parsing
  and the regular-file input domain;
- `sluice_copy_safe_output_test` — Version C safe output: input domain, temp
  placement + permission preservation, commit/discard cleanup, rename-failure
  path, fault-injected and real-backend atomic flows;
- hardening `python3 scripts/hardening.py --version-b` — the Version B
  nightly gate (Debug soak rounds, required TSan and ASan+UBSan target sets,
  final Debug).
