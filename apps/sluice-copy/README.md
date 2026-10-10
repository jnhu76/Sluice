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
  --atomic                 explicitly select the default Version C safe path
  --help                   show this help
```

Defaults: 1 MiB buffer, depth 1, 1 worker, sync=none, atomic on. `--no-atomic` and
`--atomic` both exist; the last one on the command line wins. This block is the
full accepted option set — `--atomic` is accepted but is not listed by the
program's own `--help` output.

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

Values beyond a limit are rejected **before any allocation**, but the exit code
depends on which limit and which layer rejects it (measured on the current
tree, see "Rejections" below):

| rejected value                     | rejected by        | CLI result        |
| ---------------------------------- | ------------------ | ----------------- |
| `--workers` > 64                   | CLI parser         | usage error, exit 1 |
| any of the three flags = 0         | CLI parser         | usage error, exit 1 |
| `--buffer-size` > 64 MiB           | engine validation  | `invalid_state`, exit 2 |
| `--pipeline-depth` > 64            | engine validation  | `invalid_state`, exit 2 |
| `buffer_size * pipeline_depth` > 512 MiB | engine validation | `invalid_state`, exit 2 |

The three engine-rejected rows print `copy failed: invalid_state` on stderr, not
the usage text. `--buffer-size 8388608 --pipeline-depth 64` (exactly 512 MiB) is
accepted. This is a reference copy app, not an arbitrary thread/allocator factory.

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

### Symlink destinations (intentionally mode-dependent)

`--no-atomic` rejects a symlink in the final destination component rather
than following it: the destination is opened with `O_NOFOLLOW`, so a symlink
destination fails as a normal `cannot open destination` error (`ELOOP`,
exit 2), and a dangling symlink is rejected the same way — `O_CREAT` never
punches through the link to create its target. Overwriting an existing
*regular* destination remains supported in this mode. `O_NOFOLLOW` protects
only the final component; symlinks in intermediate path components are still
followed (documented limitation).

The default atomic mode does not open the destination for writing; its final
`rename()` replaces the destination directory entry, including a symlink
entry. The two modes' symlink semantics are intentionally different.

## Exit codes

| code | meaning        |
| ---- | -------------- |
| 0    | success        |
| 1    | usage error, and the `source == destination` rejection |
| 2    | I/O error, engine validation failure (`invalid_state`), failed commit |
| 3    | canceled       |

Exit 1 is produced by the CLI parser and by the same-file rejection; the latter
prints `source and destination refer to the same file` rather than the usage
text. All other failures exit 2, except cancellation, which exits 3 — not
reachable from this CLI today, since nothing requests a stop, so the row is
contract-of-record rather than observed behaviour.

## Rejections

The CLI rejects: missing/extra operands, zero buffer size, zero pipeline
depth, invalid worker count, unknown sync policy, source==destination,
non-regular sources/destinations, and worker counts beyond the resource limits
above. Integer parsing is strict: negative numbers, signs, trailing junk
(`123abc`, `1MiB`), and values that overflow `size_t` are usage errors — no
silent narrowing or truncation.

The CLI parser does **not** range-check `--buffer-size` or `--pipeline-depth`;
those limits are enforced by `run_pipelined_copy*`, so an over-cap value
surfaces as exit 2 with `copy failed: invalid_state` rather than a usage error.

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
  content; only the crash-durability of the rename is missing. An interrupted
  (`EINTR`) directory fsync is retried through Sluice's POSIX retry authority
  before any durability failure is reported; a real fsync error is reported
  verbatim. This does not broaden the application's documented
  platform-support or durability guarantees;
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
  `RuntimeTaskContext::await_completion`;
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
- progress display;
- directory traversal (see `sluice-mirror-mini`, a later app);
- io_uring production backend;
- multiple parallel writes (Version B v1 keeps at most one write outstanding).

## Test targets

Three registered targets run under `xmake test` (group `test`), and therefore
under CI's `xmake test -v` in both the debug and release profiles:

- `app_copy_consumption_test` (10 cases) — engine/domain entry points against the
  real `ThreadPoolBackend`: a Version A path round-trip through
  `open_copy_files` + `run_pipelined_copy`, outcome move semantics, the
  `src_open` / `src_not_regular` / `dst_open` / `same_file` rejections, the
  atomic round-trip through `open_atomic_copy` + `commit_atomic_copy` (including
  "no temp file left behind"), and `discard_atomic_copy` cleanup.
- `app_copy_pipeline_oracle_test` (21 cases) — the Version B pipeline against a
  scripted backend, driving controlled submission/completion/failure ordering.
  Covers the depth-1 single outstanding read, the full depth-4 read window,
  out-of-order read completion with in-order writes, short-read retry in the same
  slot, partial and zero-progress writes, read/write error settlement of
  read-ahead, primary-error selection, settlement of already-accepted work on
  submit failure and capacity refusal, EOF draining without post-EOF writes,
  sync-after-all-writes, bounded multi-round slot reuse, and slot
  non-recycling before its write completes. Also covers synchronous rejection of
  invalid arguments (`depth 0`, `buffer 0`, `workers 0`, null backend, each cap
  + 1, the 512 MiB product limit, and size_t overflow). Three of the 21 cases
  (`borrow_live_write_peak_selftest`, `watchdog_selftest_child`,
  `watchdog_selftest_bounded`) are harness self-checks, not pipeline behavior.
- `app_copy_dir_fsync_test` (4 cases) — the atomic commit's directory-fsync
  EINTR retry through the `SLUICE_COPY_INTERNAL_TESTING` `DirFsyncScript` seam:
  one interruption retried to success, repeated interruptions all retried, a
  real error after an interruption reported verbatim (stage `dir_sync`,
  destination already replaced), and a terminal error not retried.

Not covered by any of these targets: the CLI itself is never exercised as a
subprocess — `parse_args` and the CLI exit codes above are **not** under test.
There is no automated test for a symlink destination, for the
`--no-atomic` `O_NOFOLLOW` rejection, or for `--sync data|all` end-to-end
against the real filesystem.

There is no mutation matrix in CI, and no nightly gate. When the Version B
pipeline was made an oracle (`app_copy_pipeline_oracle_test`), five mutations of
`copy_task.cpp` were injected by hand as a one-off discriminating check —
serialize reads, scramble write order, skip the post-failure drain, reuse a slot
buffer prematurely, and treat a short read as a full read. Each was detected by
this suite; that was a manual measurement, not something `xmake test` repeats.
Two limits of this evidence: a mutation kill shows the suite discriminates those
five defects, not that the pipeline is verified in general, and the
waiter-level error-precedence question (whether a waiter-failure result can
reach the task before the primary error) is source-confirmed but not reachable
through the public single-task API, so it is deliberately **not** frozen as
contract — it still needs a product decision, and the oracle asserts only the
reachable arm (drained op results never override the primary error).

## Known documentation gaps

Earlier revisions of this README listed a `sluice_copy_*` test family
(`scripted_backend_test`, `sluice_copy_pipeline_contract_test`,
`sluice_copy_pipeline_integration_test`, `sluice_copy_pipeline_stress_test`,
`sluice_copy_integration_test`, `sluice_copy_fault_test`,
`sluice_copy_cli_parse_test`, `sluice_copy_file_domain_test`,
`sluice_copy_safe_output_test`) and a `scripts/hardening.py --version-b` nightly
gate. Those test sources were removed in `5f62b55b` and the script does not exist
in this tree, so none of them is a current guarantee. The engine limit checks
(`copy_task.cpp`), the slot-reuse and ordering contracts, and the
directory-fsync retry were re-derived by the three targets above; CLI parsing,
the regular-file input domain at the CLI layer, and fault injection into the
pipeline are **not** currently re-covered.
