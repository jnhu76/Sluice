// sluice-copy copy task implementation (M1-A, Version A).
// Algorithm: brief §22 (sequential positional asynchronous copy).
//
// The copy body runs as a Runtime task. It uses RuntimeTaskContext's
// submit_* + await_completion (Candidate A) to drive positional reads/writes
// and the optional sync. Partial reads and partial writes are handled; zero
// write progress is a deterministic error; offset overflow is checked; errors
// propagate through an app-owned result slot; cancellation is observed at the
// cooperative boundaries between operations.
#include "copy_task.hpp"

#include <sluice/async/application_runtime.hpp>
#include <sluice/async/threadpool_backend.hpp>

#include <atomic>
#include <condition_variable>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

namespace sluice_copy {

namespace {

using namespace sluice::async;
using sluice::IoError;
using sluice::Result;

// Clamp a size_t to uint64 max for overflow-safe offset arithmetic.
constexpr bool add_would_overflow(std::uint64_t a, std::uint64_t b) noexcept {
    return a > std::numeric_limits<std::uint64_t>::max() - b;
}

// The copy task body. Runs inside a Runtime task; publishes its terminal
// outcome through `out` (app-owned, lifetime exceeds the task) under `mtx`.
// Never blocks a Runtime Worker on `out`.
struct CopyTask {
    int src_fd;
    int dst_fd;
    std::size_t buffer_size;
    SyncPolicy sync;
    std::vector<std::byte> buf;

    std::mutex& mtx;
    std::optional<Result<CopyStats>>& out;
    std::atomic<bool>& done;
    std::condition_variable& done_cv;  // signaled from publish() to the main thread

    void operator()(RuntimeTaskContext& ctx) {
        Result<CopyStats> result = run_body(ctx);
        publish(std::move(result));
    }

    // Publish the terminal result and signal completion. Idempotent: only the
    // first publish wins (the task runs exactly once). Signals the main thread's
    // condition variable so it can proceed to request_stop + drain + join.
    void publish(Result<CopyStats> r) {
        {
            std::lock_guard<std::mutex> lk(mtx);
            if (!done.load(std::memory_order::relaxed)) {
                out = std::move(r);
                done.store(true, std::memory_order::release);
            }
        }
        done_cv.notify_all();
        (void)r;  // moved-from
    }

    Result<CopyStats> run_body(RuntimeTaskContext& ctx) {
        CopyStats stats{};
        stats.sync = sync;

        std::uint64_t offset = 0;
        Completion<std::size_t> rd;
        Completion<std::size_t> wr;
        Completion<void> sync_c;

        for (;;) {
            // Cooperative cancellation boundary: observe the root cancel token
            // before starting the next read. If canceled, stop cleanly. We do
            // NOT claim to interrupt a kernel op already in flight (brief §22).
            if (ctx.cancel_token().is_requested()) {
                return make_unexpected<CopyStats>(IoError{IoError::Code::canceled});
            }

            // Read Completion is idle at loop top (first iteration) or was reset
            // at the end of the previous iteration. Submit a positional read.
            auto rsr = ctx.submit_read(
                ReadOp{src_fd, buf.data(), buffer_size, offset}, rd);
            if (!rsr.has_value()) {
                // Submit-time error: synchronous, no await occurs.
                return make_unexpected<CopyStats>(rsr.error());
            }
            ctx.await_completion(rd);
            // rd is now terminal.
            auto rr = rd.result();
            if (!rr.has_value()) {
                // Completion error propagates.
                return make_unexpected<CopyStats>(rr.error());
            }
            std::size_t bytes_read = rr.value();
            ++stats.read_ops;

            // EOF: positional read returns 0 at/past end. Break cleanly.
            if (bytes_read == 0) {
                break;
            }

            // Write loop: consume all bytes_read, handling partial writes and
            // rejecting zero progress (brief §22: an invalid backend state, not
            // an infinite retry).
            std::size_t consumed = 0;
            while (consumed < bytes_read) {
                if (ctx.cancel_token().is_requested()) {
                    return make_unexpected<CopyStats>(
                        IoError{IoError::Code::canceled});
                }
                std::size_t remaining = bytes_read - consumed;
                std::uint64_t write_offset = offset + consumed;
                auto wsr = ctx.submit_write(
                    WriteOp{dst_fd, buf.data() + consumed, remaining, write_offset},
                    wr);
                if (!wsr.has_value()) {
                    return make_unexpected<CopyStats>(wsr.error());
                }
                ctx.await_completion(wr);
                auto wr_res = wr.result();
                if (!wr_res.has_value()) {
                    return make_unexpected<CopyStats>(wr_res.error());
                }
                std::size_t bytes_written = wr_res.value();
                ++stats.write_ops;
                // Reset the write Completion before the next iteration so it is
                // idle for the next submit_write (L7: submit requires idle).
                wr.reset();
                if (bytes_written == 0) {
                    // Zero progress on a non-empty write: deterministic error.
                    return make_unexpected<CopyStats>(
                        IoError{IoError::Code::backend_error});
                }
                if (bytes_written < remaining) {
                    ++stats.short_writes;
                }
                consumed += bytes_written;
            }

            // Advance the file offset, checking overflow.
            if (add_would_overflow(offset, bytes_read)) {
                return make_unexpected<CopyStats>(
                    IoError{IoError::Code::invalid_state});
            }
            offset += bytes_read;
            stats.bytes_copied += bytes_read;

            // Reset read Completion for reuse in the next iteration. It is now
            // ready + result-consumed, so reset() is legal (L7 lifecycle).
            // wr is already idle: the inner write loop reset it after its last
            // completion (L7), so no outer wr.reset() is needed here.
            rd.reset();
        }

        // Post-data sync policy (brief §22). Each sync op is submitted, awaited,
        // and its result inspected. The read/write Completions are idle here.
        if (sync == SyncPolicy::data) {
            auto ssr = ctx.submit_sync_data(SyncDataOp{dst_fd}, sync_c);
            if (!ssr.has_value()) return make_unexpected<CopyStats>(ssr.error());
            ctx.await_completion(sync_c);
            auto sr = sync_c.result();
            if (!sr.has_value()) return make_unexpected<CopyStats>(sr.error());
        } else if (sync == SyncPolicy::all) {
            auto ssr = ctx.submit_sync_all(SyncAllOp{dst_fd}, sync_c);
            if (!ssr.has_value()) return make_unexpected<CopyStats>(ssr.error());
            ctx.await_completion(sync_c);
            auto sr = sync_c.result();
            if (!sr.has_value()) return make_unexpected<CopyStats>(sr.error());
        }

        return stats;
    }
};

}  // namespace

Result<CopyStats> run_sequential_copy(int src_fd, int dst_fd,
                                      std::size_t buffer_size,
                                      unsigned workers, SyncPolicy sync) {
    return run_sequential_copy_with_backend(src_fd, dst_fd, buffer_size,
                                            workers, sync,
                                            std::make_unique<ThreadPoolBackend>());
}

Result<CopyStats> run_sequential_copy_with_backend(
    int src_fd, int dst_fd, std::size_t buffer_size, unsigned workers,
    SyncPolicy sync, std::unique_ptr<AsyncBackend> backend) {
    if (buffer_size == 0) {
        return make_unexpected<CopyStats>(IoError{IoError::Code::invalid_state});
    }
    if (workers == 0) {
        return make_unexpected<CopyStats>(IoError{IoError::Code::invalid_state});
    }
    if (!backend) {
        return make_unexpected<CopyStats>(IoError{IoError::Code::invalid_state});
    }

    RuntimeBuilder builder;
    builder.backend(std::move(backend));
    builder.workers(workers);
    auto build_r = builder.build();
    if (!build_r.has_value()) return make_unexpected<CopyStats>(build_r.error());
    auto rt = std::move(build_r.value());

    auto start_r = rt->start();
    if (!start_r.has_value()) return make_unexpected<CopyStats>(start_r.error());

    // App-owned result slot + completion signal (brief §23). Lifetime exceeds
    // the task. The Runtime Worker NEVER blocks on this slot. The optional is
    // empty until the task publishes its terminal outcome. The main thread
    // waits on done_cv (NOT a Runtime Worker) for the task to finish its copy
    // BEFORE requesting stop, so a run-to-completion copy is never aborted by
    // root cancellation.
    std::mutex mtx;
    std::condition_variable done_cv;
    std::optional<Result<CopyStats>> out;
    std::atomic<bool> done{false};

    CopyTask task{src_fd, dst_fd, buffer_size, sync,
                  std::vector<std::byte>(buffer_size),
                  mtx, out, done, done_cv};

    auto sub_r = rt->submit(std::ref(task));
    if (!sub_r.has_value()) {
        // Submit rejected (e.g. admission closed). Tear down and return.
        (void)rt->shutdown();
        return make_unexpected<CopyStats>(sub_r.error());
    }

    // Wait for the copy task to publish its terminal outcome BEFORE requesting
    // stop. The task is a run-to-completion copy, not a long-lived service: it
    // must NOT observe root cancellation while it still has work to do. The main
    // thread (not a Runtime Worker) blocks on done_cv, which the task signals
    // from publish(). The Runtime driver keeps making progress (run_live) and
    // reaps I/O independently while we wait, so the task completes via normal
    // scheduler progress.
    {
        std::unique_lock<std::mutex> wlk(mtx);
        done_cv.wait(wlk, [&] {
            return done.load(std::memory_order::acquire);
        });
    }

    // Now the task is terminal and all outstanding I/O has been reaped by the
    // driver (the task only publishes after its final await completes). Safe to
    // request stop + drain + join. This stop does not abort any copy work.
    rt->request_stop();
    auto drain_r = rt->drain();
    if (!drain_r.has_value()) {
        (void)rt->shutdown();
        return make_unexpected<CopyStats>(drain_r.error());
    }
    auto join_r = rt->join();
    if (!join_r.has_value()) {
        (void)rt->shutdown();
        return make_unexpected<CopyStats>(join_r.error());
    }

    // The task published its terminal outcome before drain completed (the
    // Runtime's drain_complete requires all tasks terminal + outstanding==0).
    std::optional<Result<CopyStats>> final_result;
    {
        std::lock_guard<std::mutex> lk(mtx);
        final_result = std::move(out);
    }
    if (!final_result.has_value()) {
        // The task did not publish (e.g. admission was rejected before publish).
        return make_unexpected<CopyStats>(IoError{IoError::Code::invalid_state});
    }
    return std::move(final_result.value());
}

}  // namespace sluice_copy
