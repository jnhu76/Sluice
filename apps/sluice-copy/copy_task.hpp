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

}  // namespace sluice_copy
