#include "copy_task.hpp"

#include <sluice/async/request_scope.hpp>
#include <sluice/async/threadpool_backend.hpp>

#include <chrono>
#include <limits>
#include <memory>
#include <system_error>
#include <utility>
#include <vector>

namespace sluice_copy {

namespace {

using namespace sluice::async;
using sluice::File;
using sluice::IoError;
using sluice::Result;

constexpr bool add_would_overflow(std::uint64_t a, std::uint64_t b) noexcept {
    return a > std::numeric_limits<std::uint64_t>::max() - b;
}

Result<CopyStats> copy_exception() noexcept {
    try {
        throw;
    } catch (const std::bad_alloc&) {
        return make_unexpected<CopyStats>(IoError{IoError::Code::no_space});
    } catch (const std::system_error& e) {
        return make_unexpected<CopyStats>(sluice::from_errno_value(e.code().value()));
    } catch (...) {
        return make_unexpected<CopyStats>(IoError{IoError::Code::backend_error});
    }
}

enum class SlotState : std::uint8_t {
    idle,
    reading,
    read_done,
    writing,
    done,
};

struct PipelineSlot {
    std::vector<std::byte> buffer;
    ScopeTicket<std::size_t> ticket;
    std::uint64_t chunk_offset = 0;
    std::size_t filled = 0;
    std::size_t written = 0;
    bool eof = false;
    SlotState state = SlotState::idle;

    explicit PipelineSlot(std::size_t cap) : buffer(cap) {}
};

struct PipelinedCopyTask {
    const sluice::File* src_file;
    NativeFileRef dst;
    std::size_t buffer_size;
    std::size_t pipeline_depth;
    SyncPolicy sync;
    std::vector<std::unique_ptr<PipelineSlot>> slots;

    Result<void> submit_slot_read(RequestScope& scope, PipelineSlot& s) {
        std::uint64_t off = s.chunk_offset + s.filled;
        std::byte* dst = s.buffer.data() + s.filled;
        std::size_t len = buffer_size - s.filled;
        auto rsr = scope.submit_read(ReadOp{*src_file, dst, len, off});
        if (!rsr.has_value())
            return make_unexpected<void>(rsr.error());
        s.ticket = rsr.value();
        s.state = SlotState::reading;
        return {};
    }

    Result<void> submit_slot_write(RequestScope& scope, PipelineSlot& s) {
        std::uint64_t off = s.chunk_offset + s.written;
        const std::byte* src = s.buffer.data() + s.written;
        std::size_t len = s.filled - s.written;
        auto wsr = scope.submit_write(WriteOp{dst, src, len, off});
        if (!wsr.has_value())
            return make_unexpected<void>(wsr.error());
        s.ticket = wsr.value();
        s.state = SlotState::writing;
        return {};
    }

    template <class T>
    Result<T> wait_take(RequestScope& scope, AsyncIoContext& ctx, ScopeTicket<T> ticket) {
        for (;;) {
            auto waited = scope.wait_for(ticket, std::chrono::nanoseconds::max());
            if (!waited.has_value())
                return make_unexpected<T>(waited.error());
            if (waited.value() == ScopeWaitStatus::ready)
                return scope.take(ticket).result;
            if (waited.value() == ScopeWaitStatus::interrupted) {
                ctx.acknowledge_progress_control();
                continue;
            }
            return make_unexpected<T>(IoError{IoError::Code::backend_error});
        }
    }

    Result<void> await_slot_read(RequestScope& scope, AsyncIoContext& ctx, PipelineSlot& s,
                                 CopyStats& stats, bool& saw_eof) {
        for (;;) {
            auto read = wait_take(scope, ctx, s.ticket);
            if (!read.has_value())
                return make_unexpected<void>(read.error());
            ++stats.read_ops;
            if (read.value() == 0) {
                s.eof = true;
                saw_eof = true;
                s.state = s.filled == 0 ? SlotState::done : SlotState::read_done;
                return {};
            }
            s.filled += read.value();
            if (s.filled == buffer_size) {
                s.state = SlotState::read_done;
                return {};
            }
            auto submitted = submit_slot_read(scope, s);
            if (!submitted.has_value())
                return submitted;
        }
    }

    Result<void> await_slot_write(RequestScope& scope, AsyncIoContext& ctx, PipelineSlot& s,
                                  CopyStats& stats) {
        for (;;) {
            auto written = wait_take(scope, ctx, s.ticket);
            if (!written.has_value())
                return make_unexpected<void>(written.error());
            if (written.value() == 0)
                return make_unexpected<void>(IoError{IoError::Code::backend_error});
            ++stats.write_ops;
            if (written.value() < s.filled - s.written)
                ++stats.short_writes;
            s.written += written.value();
            stats.bytes_copied += written.value();
            if (s.written == s.filled)
                return {};
            auto submitted = submit_slot_write(scope, s);
            if (!submitted.has_value())
                return submitted;
        }
    }

    Result<CopyStats> run_body(RequestScope& scope, AsyncIoContext& ctx) {
        CopyStats stats{};
        stats.sync = sync;

        bool eof_seen = false;

        std::optional<IoError> primary_error;

        auto fail = [&](IoError e) {
            if (!primary_error.has_value())
                primary_error = e;
            eof_seen = true;
        };

        for (std::size_t i = 0; i < pipeline_depth; ++i) {
            if (eof_seen)
                break;
            auto& s = slots[i];
            auto rsr = submit_slot_read(scope, *s);
            if (!rsr.has_value()) {
                fail(rsr.error());
                break;
            }
        }

        while (!primary_error.has_value()) {

            PipelineSlot* read_slot = nullptr;
            for (auto& s : slots) {
                if (s->state != SlotState::reading)
                    continue;
                if (read_slot == nullptr || s->chunk_offset < read_slot->chunk_offset)
                    read_slot = s.get();
            }

            if (read_slot != nullptr) {
                auto r = await_slot_read(scope, ctx, *read_slot, stats, eof_seen);
                if (!r.has_value()) {
                    fail(r.error());
                    break;
                }
            }

            for (;;) {

                PipelineSlot* ws = nullptr;
                for (auto& s : slots) {
                    if (s->state != SlotState::read_done)
                        continue;
                    if (ws == nullptr || s->chunk_offset < ws->chunk_offset)
                        ws = s.get();
                }
                if (ws == nullptr)
                    break;

                if (ws->filled == 0) {
                    ws->state = SlotState::done;
                    continue;
                }
                auto wsr = submit_slot_write(scope, *ws);
                if (!wsr.has_value()) {
                    fail(wsr.error());
                    break;
                }
                auto wr = await_slot_write(scope, ctx, *ws, stats);
                if (!wr.has_value()) {
                    fail(wr.error());
                    break;
                }

                if (ws->eof || eof_seen) {
                    ws->state = SlotState::done;
                } else {
                    if (add_would_overflow(ws->chunk_offset, buffer_size * pipeline_depth)) {
                        fail(IoError{IoError::Code::invalid_state});
                        break;
                    }
                    ws->chunk_offset += buffer_size * pipeline_depth;
                    ws->filled = 0;
                    ws->written = 0;
                    ws->eof = false;
                    ws->state = SlotState::idle;
                    auto rsr = submit_slot_read(scope, *ws);
                    if (!rsr.has_value()) {
                        eof_seen = true;
                        ws->state = SlotState::done;
                        fail(rsr.error());
                        break;
                    }
                }
            }
            if (primary_error.has_value())
                break;

            bool all_done = true;
            for (auto& s : slots)
                if (s->state != SlotState::done) {
                    all_done = false;
                    break;
                }
            if (all_done)
                break;
        }

        if (primary_error.has_value()) {
            (void)scope.finish();
            return make_unexpected<CopyStats>(*primary_error);
        }

        if (sync == SyncPolicy::data) {
            auto ssr = scope.submit_sync_data(SyncDataOp{dst});
            if (!ssr.has_value())
                return make_unexpected<CopyStats>(ssr.error());
            auto sr = wait_take(scope, ctx, ssr.value());
            if (!sr.has_value())
                return make_unexpected<CopyStats>(sr.error());
        } else if (sync == SyncPolicy::all) {
            auto ssr = scope.submit_sync_all(SyncAllOp{dst});
            if (!ssr.has_value())
                return make_unexpected<CopyStats>(ssr.error());
            auto sr = wait_take(scope, ctx, ssr.value());
            if (!sr.has_value())
                return make_unexpected<CopyStats>(sr.error());
        }

        auto settled = scope.finish();
        if (!settled.has_value())
            return make_unexpected<CopyStats>(settled.error());

        return stats;
    }
};

}

Result<CopyStats> run_sequential_copy(const File& src_file, const NativeFileRef& dst,
                                      std::size_t buffer_size, unsigned workers, SyncPolicy sync) {
    return run_pipelined_copy(src_file, dst, buffer_size, 1, workers, sync);
}

Result<CopyStats> run_pipelined_copy(const File& src_file, const NativeFileRef& dst,
                                     std::size_t buffer_size, std::size_t pipeline_depth,
                                     unsigned workers, SyncPolicy sync) {
    if (!valid_config(buffer_size, pipeline_depth, workers))
        return make_unexpected<CopyStats>(IoError{IoError::Code::invalid_state});
    try {
        return run_pipelined_copy_with_backend(
            src_file, dst, buffer_size, pipeline_depth, workers, sync,
            std::make_unique<ThreadPoolBackend>(ThreadPoolConfig{64, workers}));
    } catch (...) {
        return copy_exception();
    }
}

Result<CopyStats> run_pipelined_copy_with_backend(const File& src_file, const NativeFileRef& dst,
                                                  std::size_t buffer_size,
                                                  std::size_t pipeline_depth, unsigned workers,
                                                  SyncPolicy sync,
                                                  std::unique_ptr<AsyncBackend> backend) {
    if (!valid_config(buffer_size, pipeline_depth, workers) || !backend)
        return make_unexpected<CopyStats>(IoError{IoError::Code::invalid_state});

    std::vector<std::unique_ptr<PipelineSlot>> slots;
    try {
        slots.reserve(pipeline_depth);
        for (std::size_t i = 0; i < pipeline_depth; ++i) {
            auto s = std::make_unique<PipelineSlot>(buffer_size);
            s->chunk_offset = static_cast<std::uint64_t>(i) * buffer_size;
            slots.push_back(std::move(s));
        }
    } catch (const std::bad_alloc&) {
        return make_unexpected<CopyStats>(IoError{IoError::Code::no_space});
    }

    PipelinedCopyTask task{&src_file, dst, buffer_size, pipeline_depth, sync, std::move(slots)};

    try {
        AsyncIoContext ctx(std::move(backend));
        auto result = [&]() -> Result<CopyStats> {
            try {
                RequestScope scope(ctx, pipeline_depth, ScopeCleanupPolicy::cancel_then_drain);
                return task.run_body(scope, ctx);
            } catch (...) {
                return copy_exception();
            }
        }();
        auto shutdown = ctx.shutdown(ShutdownPolicy::drain);
        if (result.has_value()) {
            if (!shutdown.has_value())
                return make_unexpected<CopyStats>(shutdown.error());
            if (shutdown.value() != ShutdownOutcome::completed)
                return make_unexpected<CopyStats>(IoError{IoError::Code::backend_error});
        }
        return result;
    } catch (...) {
        return copy_exception();
    }
}

Result<CopyStats> run_sequential_copy_with_backend(const File& src_file, const NativeFileRef& dst,
                                                   std::size_t buffer_size, unsigned workers,
                                                   SyncPolicy sync,
                                                   std::unique_ptr<AsyncBackend> backend) {
    return run_pipelined_copy_with_backend(src_file, dst, buffer_size, 1, workers, sync,
                                           std::move(backend));
}

}
