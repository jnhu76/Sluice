// sluice-copy — reference async file copy application (M1-A, Version A).
//
// Sequential asynchronous positional copy driven by ApplicationRuntime +
// ThreadPoolBackend. This header exposes the copy-task result type and the
// copy entry point so unit/fault/integration tests can drive the same code the
// CLI uses, WITHOUT depending on test headers or private Scheduler access.
//
// All code uses installed/public headers only. No SLUICE_ASYNC_INTERNAL_TESTING.
//
// Version A limitation (documented): on a mid-copy failure the destination
// may be left partial. Atomic temp-file + rename output is a Version C feature
// and is intentionally NOT implemented here.
#pragma once

#include <sluice/async/application_runtime.hpp>
#include <sluice/async/threadpool_backend.hpp>
#include <sluice/error.hpp>
#include <sluice/result.hpp>

#include <cstddef>
#include <cstdint>
#include <string>

namespace sluice_copy {

// Sync policy applied after the data copy completes (brief §21).
enum class SyncPolicy {
    none,
    data,  // fdatasync via submit_sync_data
    all,   // fsync via submit_sync_all
};

// Result statistics published by the copy task (brief §23). The Runtime runs
// void tasks, so the result is published through an app-owned slot whose
// lifetime exceeds the task.
struct CopyStats {
    std::uint64_t bytes_copied = 0;
    std::uint64_t read_ops = 0;
    std::uint64_t write_ops = 0;
    std::uint64_t short_writes = 0;  // write completions that wrote < requested
    SyncPolicy sync = SyncPolicy::none;
};

// Run a sequential positional async copy of `src_fd` -> `dst_fd` as ONE Runtime
// task. `buffer_size` is the per-chunk read/write buffer size (must be > 0).
// The function drives the full Runtime lifecycle internally: it builds the
// Runtime with the supplied worker count, starts it, submits the copy task,
// request_stop() + drain() + join(), and returns the final CopyStats or an
// error. This shape lets tests exercise the real public surface end-to-end.
//
// `src_fd` is opened read-only by the caller; `dst_fd` is opened write/create/
// truncate by the caller. The caller owns both descriptors and closes them.
sluice::Result<CopyStats> run_sequential_copy(int src_fd, int dst_fd,
                                              std::size_t buffer_size,
                                              unsigned workers,
                                              SyncPolicy sync);

// Backend-injecting variant for deterministic fault tests (brief §25). Same
// algorithm and lifecycle as run_sequential_copy, but the caller supplies the
// AsyncBackend. The production CLI path (run_sequential_copy above) always
// uses ThreadPoolBackend; this overload lets tests drive the SAME copy code
// against a deterministic backend (e.g. a fake) to inject short/zero/error
// completions. The header does NOT reference any test backend type — it takes
// the abstract AsyncBackend, so the app target stays production-clean.
sluice::Result<CopyStats> run_sequential_copy_with_backend(
    int src_fd, int dst_fd, std::size_t buffer_size, unsigned workers,
    SyncPolicy sync, std::unique_ptr<sluice::async::AsyncBackend> backend);

// ---------------------------------------------------------------------------
// Version B — bounded reusable-buffer pipeline (multiple outstanding reads,
// single ordered writer). Documented behavior:
//
//   * `pipeline_depth` fixed logical chunks (slots) are read ahead in parallel,
//     so up to `pipeline_depth` reads may be outstanding at once. The first
//     version allows at most ONE write outstanding (writes are submitted in
//     strict ascending file-offset order; reads may complete out of order).
//   * Each slot covers a fixed interval [chunk_offset, chunk_offset +
//     buffer_size). A short read (0 < n < remaining) is retried within the same
//     slot until the buffer is filled or EOF is reached — the global offset is
//     never advanced past an unread region.
//   * A partial write advances `written` within the slot and retries; a write
//     that returns 0 with data remaining is a deterministic backend error.
//   * On EOF, submission of new reads stops, the last partial slot is written,
//     and every already-submitted read is drained (awaited + consumed + reset)
//     before the task returns.
//   * On any submit/completion error, the first meaningful error is saved, new
//     submission stops, and already-successfully-submitted operations are
//     drained; secondary errors during cleanup never overwrite the primary
//     error.
//   * Memory upper bound is approximately `buffer_size * pipeline_depth`
//     (one buffer per slot) plus the read/write Completions. This is NOT
//     zero-copy and does NOT issue multiple parallel writes.
//
// The outermost call still blocks until the copy completes (the Runtime task
// publishes its terminal outcome); no CopyHandle / future / new public async
// surface is introduced. `pipeline_depth == 1` reproduces Version A's
// one-read-at-a-time behavior.
//
// `pipeline_depth` must be >= 1. `buffer_size` must be > 0. The product
// `buffer_size * pipeline_depth` must not overflow `std::size_t`.
sluice::Result<CopyStats> run_pipelined_copy(int src_fd, int dst_fd,
                                             std::size_t buffer_size,
                                             std::size_t pipeline_depth,
                                             unsigned workers,
                                             SyncPolicy sync);

// Backend-injecting variant for deterministic fault/contract tests. Same
// algorithm and lifecycle as run_pipelined_copy, but the caller supplies the
// AsyncBackend (the abstract type keeps the app target production-clean).
sluice::Result<CopyStats> run_pipelined_copy_with_backend(
    int src_fd, int dst_fd, std::size_t buffer_size,
    std::size_t pipeline_depth, unsigned workers, SyncPolicy sync,
    std::unique_ptr<sluice::async::AsyncBackend> backend);

}  // namespace sluice_copy
