








#include "copy_task.hpp"

#include <sluice/async/await_op_helpers.hpp>
#include <sluice/async/task_result.hpp>
#include <sluice/async/threadpool_backend.hpp>

#include <cstring>
#include <limits>
#include <memory>
#include <span>
#include <utility>
#include <vector>

namespace sluice_copy {

namespace {

using namespace sluice::async;
using sluice::IoError;
using sluice::Result;


constexpr bool add_would_overflow(std::uint64_t a, std::uint64_t b) noexcept {
    return a > std::numeric_limits<std::uint64_t>::max() - b;
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
    Completion<std::size_t> read_c;
    Completion<std::size_t> write_c;
    std::uint64_t chunk_offset = 0;
    std::size_t filled = 0;
    std::size_t written = 0;
    bool eof = false;
    SlotState state = SlotState::idle;

    explicit PipelineSlot(std::size_t cap) : buffer(cap) {}
};





struct PipelinedCopyTask {
    int src_fd;
    int dst_fd;
    std::size_t buffer_size;
    std::size_t pipeline_depth;
    SyncPolicy sync;
    std::vector<std::unique_ptr<PipelineSlot>> slots;
    Completion<void> sync_c;

    void operator()(RuntimeTaskContext& ctx,
                    TaskResultSlot<Result<CopyStats>>& slot) {





        slot.publish(run_body(ctx));
    }



    Result<void> submit_slot_read(RuntimeTaskContext& ctx, PipelineSlot& s) {

        std::uint64_t off = s.chunk_offset + s.filled;
        std::byte* dst = s.buffer.data() + s.filled;
        std::size_t len = buffer_size - s.filled;
        auto rsr = ctx.submit_read(ReadOp{src_fd, dst, len, off}, s.read_c);
        if (!rsr.has_value()) return rsr;
        s.state = SlotState::reading;
        return {};
    }



    Result<void> submit_slot_write(RuntimeTaskContext& ctx, PipelineSlot& s) {
        std::uint64_t off = s.chunk_offset + s.written;
        const std::byte* src = s.buffer.data() + s.written;
        std::size_t len = s.filled - s.written;
        auto wsr = ctx.submit_write(WriteOp{dst_fd, src, len, off}, s.write_c);
        if (!wsr.has_value()) return wsr;
        s.state = SlotState::writing;
        return {};
    }








    Result<void> await_slot_read(RuntimeTaskContext& ctx, PipelineSlot& s,
                                 CopyStats& stats, bool& saw_eof) {
        const std::size_t remaining = buffer_size - s.filled;
        AwaitOpTally tally;
        auto first = await_take(ctx, s.read_c);
        if (!first.has_value()) return make_unexpected<void>(first.error());
        ++tally.ops;
        if (first.value() < remaining) ++tally.short_ops;

        if (first.value() == 0) {

            stats.read_ops += tally.ops;
            s.eof = true;
            saw_eof = true;
            s.state = (s.filled == 0) ? SlotState::done : SlotState::read_done;
            return {};
        }
        s.filled += first.value();
        if (s.filled >= buffer_size) {
            stats.read_ops += tally.ops;
            s.state = SlotState::read_done;
            return {};
        }

        auto fr = await_read_fill(
            ctx, src_fd,
            std::span<std::byte>(s.buffer.data() + s.filled,
                                 buffer_size - s.filled),
            s.chunk_offset + s.filled, s.read_c, &tally);
        if (!fr.has_value()) return make_unexpected<void>(fr.error());
        stats.read_ops += tally.ops;
        std::size_t n = fr.value();
        s.filled += n;
        if (n < buffer_size - (s.filled - n)) {

            s.eof = true;
            saw_eof = true;
        }
        s.state = (s.filled == 0) ? SlotState::done : SlotState::read_done;
        return {};
    }







    Result<void> await_slot_write(RuntimeTaskContext& ctx, PipelineSlot& s,
                                  CopyStats& stats) {
        AwaitOpTally tally;
        auto first = await_take(ctx, s.write_c);
        if (!first.has_value()) return make_unexpected<void>(first.error());
        ++tally.ops;
        if (first.value() == 0) {

            return make_unexpected<void>(IoError{IoError::Code::backend_error});
        }
        const std::size_t remaining_before = s.filled - s.written;
        if (first.value() < remaining_before) ++tally.short_ops;
        s.written += first.value();
        if (s.written < s.filled) {

            auto wr = await_write_exact(
                ctx, dst_fd,
                std::span<const std::byte>(s.buffer.data() + s.written,
                                           s.filled - s.written),
                s.chunk_offset + s.written, s.write_c, &tally);
            if (!wr.has_value()) return make_unexpected<void>(wr.error());
            s.written = s.filled;
        }
        stats.write_ops += tally.ops;
        stats.short_writes += tally.short_ops;
        return {};
    }

    Result<CopyStats> run_body(RuntimeTaskContext& ctx) {
        CopyStats stats{};
        stats.sync = sync;

        bool eof_seen = false;


        std::optional<IoError> primary_error;



        auto fail = [&](IoError e) {
            if (!primary_error.has_value()) primary_error = e;
            eof_seen = true;
        };


        for (std::size_t i = 0; i < pipeline_depth; ++i) {
            if (eof_seen) break;
            if (ctx.cancel_token().is_requested()) {
                fail(IoError{IoError::Code::canceled});
                break;
            }
            auto& s = slots[i];
            auto rsr = submit_slot_read(ctx, *s);
            if (!rsr.has_value()) {


                fail(rsr.error());
                break;
            }
        }





        while (!primary_error.has_value()) {
            if (ctx.cancel_token().is_requested()) {
                fail(IoError{IoError::Code::canceled});
                break;
            }





            PipelineSlot* read_slot = nullptr;
            for (auto& s : slots) {
                if (s->state != SlotState::reading) continue;
                if (read_slot == nullptr || s->chunk_offset < read_slot->chunk_offset)
                    read_slot = s.get();
            }

            if (read_slot != nullptr) {


                auto r = await_slot_read(ctx, *read_slot, stats, eof_seen);
                if (!r.has_value()) {
                    fail(r.error());
                    break;
                }
            }









            for (;;) {
                if (ctx.cancel_token().is_requested()) {
                    fail(IoError{IoError::Code::canceled});
                    break;
                }

                PipelineSlot* ws = nullptr;
                for (auto& s : slots) {
                    if (s->state != SlotState::read_done) continue;
                    if (ws == nullptr || s->chunk_offset < ws->chunk_offset)
                        ws = s.get();
                }
                if (ws == nullptr) break;

                if (ws->filled == 0) {

                    ws->state = SlotState::done;
                    continue;
                }
                auto wsr = submit_slot_write(ctx, *ws);
                if (!wsr.has_value()) {
                    fail(wsr.error());
                    break;
                }
                auto wr = await_slot_write(ctx, *ws, stats);
                if (!wr.has_value()) {
                    fail(wr.error());
                    break;
                }
                stats.bytes_copied += ws->filled;




                if (ws->eof || eof_seen) {
                    ws->state = SlotState::done;
                } else {
                    if (add_would_overflow(ws->chunk_offset,
                                           buffer_size * pipeline_depth)) {
                        fail(IoError{IoError::Code::invalid_state});
                        break;
                    }
                    ws->chunk_offset += buffer_size * pipeline_depth;
                    ws->filled = 0;
                    ws->written = 0;
                    ws->eof = false;
                    ws->state = SlotState::idle;
                    if (ctx.cancel_token().is_requested()) {
                        fail(IoError{IoError::Code::canceled});
                        break;
                    }
                    auto rsr = submit_slot_read(ctx, *ws);
                    if (!rsr.has_value()) {



                        eof_seen = true;
                        ws->state = SlotState::done;
                        fail(rsr.error());
                        break;
                    }
                }
            }
            if (primary_error.has_value()) break;



            bool all_done = true;
            for (auto& s : slots)
                if (s->state != SlotState::done) { all_done = false; break; }
            if (all_done) break;
        }













        for (auto& s : slots) {


            if (s->read_c.outstanding() || s->read_c.ready()) {
                auto dr = await_drain(ctx, s->read_c);
                if (!dr.has_value()) return make_unexpected<CopyStats>(dr.error());
            }
            if (s->write_c.outstanding() || s->write_c.ready()) {
                auto dr = await_drain(ctx, s->write_c);
                if (!dr.has_value()) return make_unexpected<CopyStats>(dr.error());
            }
        }

        if (primary_error.has_value())
            return make_unexpected<CopyStats>(primary_error.value());



        if (sync == SyncPolicy::data) {
            auto ssr = ctx.submit_sync_data(SyncDataOp{dst_fd}, sync_c);
            if (!ssr.has_value()) return make_unexpected<CopyStats>(ssr.error());
            auto sr = await_take(ctx, sync_c);
            if (!sr.has_value()) return make_unexpected<CopyStats>(sr.error());
        } else if (sync == SyncPolicy::all) {
            auto ssr = ctx.submit_sync_all(SyncAllOp{dst_fd}, sync_c);
            if (!ssr.has_value()) return make_unexpected<CopyStats>(ssr.error());
            auto sr = await_take(ctx, sync_c);
            if (!sr.has_value()) return make_unexpected<CopyStats>(sr.error());
        }






        sync_c.reset();

        return stats;
    }
};

}

Result<CopyStats> run_sequential_copy(int src_fd, int dst_fd,
                                      std::size_t buffer_size,
                                      unsigned workers, SyncPolicy sync) {
    return run_pipelined_copy(src_fd, dst_fd, buffer_size, 1, workers,
                              sync);
}

Result<CopyStats> run_pipelined_copy(int src_fd, int dst_fd,
                                     std::size_t buffer_size,
                                     std::size_t pipeline_depth,
                                     unsigned workers, SyncPolicy sync) {
    return run_pipelined_copy_with_backend(src_fd, dst_fd, buffer_size,
                                           pipeline_depth, workers, sync,
                                           std::make_unique<ThreadPoolBackend>());
}

Result<CopyStats> run_pipelined_copy_with_backend(
    int src_fd, int dst_fd, std::size_t buffer_size,
    std::size_t pipeline_depth, unsigned workers, SyncPolicy sync,
    std::unique_ptr<AsyncBackend> backend) {






    if (buffer_size == 0 || pipeline_depth == 0 || workers == 0 || !backend) {
        return make_unexpected<CopyStats>(IoError{IoError::Code::invalid_state});
    }
    if (buffer_size > kMaxBufferSize || pipeline_depth > kMaxPipelineDepth ||
        workers > kMaxWorkers) {
        return make_unexpected<CopyStats>(IoError{IoError::Code::invalid_state});
    }
    if (buffer_size > std::numeric_limits<std::size_t>::max() / pipeline_depth) {
        return make_unexpected<CopyStats>(IoError{IoError::Code::invalid_state});
    }
    if (buffer_size * pipeline_depth > kMaxPipelineBytes) {
        return make_unexpected<CopyStats>(IoError{IoError::Code::invalid_state});
    }





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

    PipelinedCopyTask task{src_fd,        dst_fd,      buffer_size,
                           pipeline_depth, sync,        std::move(slots),
                           {}};






    return run_task_to_result<CopyStats>(workers, std::move(backend), task);
}

Result<CopyStats> run_sequential_copy_with_backend(
    int src_fd, int dst_fd, std::size_t buffer_size, unsigned workers,
    SyncPolicy sync, std::unique_ptr<AsyncBackend> backend) {

    return run_pipelined_copy_with_backend(src_fd, dst_fd, buffer_size,
1, workers, sync,
                                           std::move(backend));
}

}
