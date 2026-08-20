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

// Clamp a size_t to uint64 max for overflow-safe offset arithmetic.
constexpr bool add_would_overflow(std::uint64_t a, std::uint64_t b) noexcept {
    return a > std::numeric_limits<std::uint64_t>::max() - b;
}

// ===========================================================================
// Version B — bounded reusable-buffer pipeline.
//
// `pipeline_depth` slots (each owning a fixed buffer + read/write Completion)
// are read ahead in parallel: while slot k's write is awaited, the reads of
// slots k+1 .. k+depth-1 remain outstanding against the backend, producing real
// read/write overlap. Writes are submitted in strict ascending file-offset
// order (slots are processed round-robin in offset order). Partial reads retry
// within the same slot; partial writes advance and retry; a zero write with
// data remaining is a deterministic error. On EOF, new reads stop and the
// remaining reads are drained; on any error the first meaningful error wins and
// already-submitted ops are drained.
//
// Completions and buffers are address-stable for the whole operation lifetime
// (slots live in std::vector<std::unique_ptr<Slot>>), satisfying L7.
// ===========================================================================

// Per-slot lifecycle. A slot is a fixed logical chunk: it progresses
// idle -> reading -> read_done -> writing -> (idle|done) as its chunk is read,
// written, and either reused for the next chunk or retired at EOF.
enum class SlotState : std::uint8_t {
    idle,       // no read outstanding; ready to read this chunk
    reading,    // a read is outstanding for [chunk_offset+filled, +remaining)
    read_done,  // read is terminal; buffer holds `filled` valid bytes
    writing,    // a write is outstanding for [chunk_offset+written, +remaining)
    done,       // EOF reached for this slot; retired
};

struct PipelineSlot {
    std::vector<std::byte> buffer;        // fixed chunk buffer (buffer_size)
    Completion<std::size_t> read_c;       // outstanding read (L7: address-stable)
    Completion<std::size_t> write_c;      // outstanding write (L7: address-stable)
    std::uint64_t chunk_offset = 0;       // file offset of this chunk's start
    std::size_t filled = 0;               // valid bytes read into the buffer
    std::size_t written = 0;              // valid bytes written out
    bool eof = false;                     // read hit EOF for this chunk
    SlotState state = SlotState::idle;

    explicit PipelineSlot(std::size_t cap) : buffer(cap) {}
};

// Drain helper: cancel + await every outstanding Completion so the Runtime can
// shut down cleanly. Outstanding ops are awaited through the context (so the
// Scheduler reaps them); the first meaningful error is preserved. Called on
// both the EOF path (clean) and the error path (best-effort).
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
        // The task-level exception boundary (an escaping exception translated
        // into the published outcome — the Runtime swallows task exceptions at
        // the Group boundary, so an unhandled one would hang the caller's
        // wait) is the library's run_task_to_result net; the task itself only
        // publishes its terminal outcome.
        slot.publish(run_body(ctx));
    }

    // Submit the (possibly partial) read for a slot in `idle` or `reading`
    // state, advancing its filled cursor. Returns the submit Result.
    Result<void> submit_slot_read(RuntimeTaskContext& ctx, PipelineSlot& s) {
        // Read the remaining region [chunk_offset+filled, +buffer_size-filled).
        std::uint64_t off = s.chunk_offset + s.filled;
        std::byte* dst = s.buffer.data() + s.filled;
        std::size_t len = buffer_size - s.filled;
        auto rsr = ctx.submit_read(ReadOp{src_fd, dst, len, off}, s.read_c);
        if (!rsr.has_value()) return rsr;
        s.state = SlotState::reading;
        return {};
    }

    // Submit the (possibly partial) write for a slot in `read_done` state,
    // advancing its written cursor. Returns the submit Result.
    Result<void> submit_slot_write(RuntimeTaskContext& ctx, PipelineSlot& s) {
        std::uint64_t off = s.chunk_offset + s.written;
        const std::byte* src = s.buffer.data() + s.written;
        std::size_t len = s.filled - s.written;
        auto wsr = ctx.submit_write(WriteOp{dst_fd, src, len, off}, s.write_c);
        if (!wsr.has_value()) return wsr;
        s.state = SlotState::writing;
        return {};
    }

    // Await a slot's outstanding read to filled-or-EOF. The pipeline
    // PRE-SUBMITS its read window (submit_slot_read in Phase 1 / slot
    // recycle), so the already-submitted completion is consumed with
    // await_take FIRST; only a short read's remainder goes through the
    // library's await_read_fill coordinator (which submits its own retries).
    // Returns an error on a completion/submit error; the primary error is
    // captured by the caller.
    Result<void> await_slot_read(RuntimeTaskContext& ctx, PipelineSlot& s,
                                 CopyStats& stats, bool& saw_eof) {
        const std::size_t remaining = buffer_size - s.filled;
        AwaitOpTally tally;
        auto first = await_take(ctx, s.read_c);
        if (!first.has_value()) return make_unexpected<void>(first.error());
        ++tally.ops;
        if (first.value() < remaining) ++tally.short_ops;

        if (first.value() == 0) {
            // EOF for this chunk before any byte.
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
        // Short read with more room: the coordinator retries the remainder.
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
            // EOF for this chunk (a partial tail is data).
            s.eof = true;
            saw_eof = true;
        }
        s.state = (s.filled == 0) ? SlotState::done : SlotState::read_done;
        return {};
    }

    // Await a slot's outstanding write to fully written. The write was
    // PRE-SUBMITTED by submit_slot_write, so its completion is consumed with
    // await_take FIRST; only a partial write's remainder goes through the
    // library's await_write_exact coordinator (which submits its own
    // retries). Zero progress with data remaining is a deterministic
    // backend_error.
    Result<void> await_slot_write(RuntimeTaskContext& ctx, PipelineSlot& s,
                                  CopyStats& stats) {
        AwaitOpTally tally;
        auto first = await_take(ctx, s.write_c);
        if (!first.has_value()) return make_unexpected<void>(first.error());
        ++tally.ops;
        if (first.value() == 0) {
            // Zero progress on a non-empty write: deterministic error.
            return make_unexpected<void>(IoError{IoError::Code::backend_error});
        }
        const std::size_t remaining_before = s.filled - s.written;
        if (first.value() < remaining_before) ++tally.short_ops;
        s.written += first.value();
        if (s.written < s.filled) {
            // Partial write: the coordinator retries the remainder.
            auto wr = await_write_exact(
                ctx, dst_fd,
                std::span<const std::byte>(s.buffer.data() + s.written,
                                           s.filled - s.written),
                s.chunk_offset + s.written, s.write_c, &tally);
            if (!wr.has_value()) return make_unexpected<void>(wr.error());
            s.written = s.filled;  // the helper returns only after all bytes land
        }
        stats.write_ops += tally.ops;
        stats.short_writes += tally.short_ops;
        return {};
    }

    Result<CopyStats> run_body(RuntimeTaskContext& ctx) {
        CopyStats stats{};
        stats.sync = sync;

        bool eof_seen = false;
        // Primary (first meaningful) error. Secondary errors during cleanup
        // never overwrite it.
        std::optional<IoError> primary_error;

        // Helper to record a primary error (first one wins) and stop new
        // submission by setting eof_seen so the main loop drains and exits.
        auto fail = [&](IoError e) {
            if (!primary_error.has_value()) primary_error = e;
            eof_seen = true;
        };

        // ---- Phase 1: submit the initial read window (up to depth reads). ----
        for (std::size_t i = 0; i < pipeline_depth; ++i) {
            if (eof_seen) break;
            if (ctx.cancel_token().is_requested()) {
                fail(IoError{IoError::Code::canceled});
                break;
            }
            auto& s = slots[i];
            auto rsr = submit_slot_read(ctx, *s);
            if (!rsr.has_value()) {
                // Submit failed: this op never entered the backend. Do NOT
                // await it. Record the error and stop submitting new reads.
                fail(rsr.error());
                break;
            }
        }

        // ---- Phase 2: steady-state pipeline. ----
        // Process slots round-robin in offset order: await each outstanding
        // read (other slots' reads stay outstanding -> real overlap), then
        // await its write. This keeps writes strictly ordered by offset.
        while (!primary_error.has_value()) {
            if (ctx.cancel_token().is_requested()) {
                fail(IoError{IoError::Code::canceled});
                break;
            }

            // Find the LOWEST-offset slot with an outstanding read to reap.
            // Slots are recycled to ever-higher offsets, so vector order is NOT
            // offset order after the first round; we must scan all slots and
            // pick the minimum chunk_offset among those in `reading` state.
            PipelineSlot* read_slot = nullptr;
            for (auto& s : slots) {
                if (s->state != SlotState::reading) continue;
                if (read_slot == nullptr || s->chunk_offset < read_slot->chunk_offset)
                    read_slot = s.get();
            }

            if (read_slot != nullptr) {
                // Await the lowest-offset outstanding read. Other slots' reads
                // remain outstanding (real read/write overlap).
                auto r = await_slot_read(ctx, *read_slot, stats, eof_seen);
                if (!r.has_value()) {
                    fail(r.error());
                    break;
                }
            }

            // Write read_done slots in STRICTLY ASCENDING chunk_offset order
            // (at most one write outstanding at a time per Version B v1). Because
            // slots are recycled to ever-higher offsets, vector order is NOT
            // offset order after the first round; we must select the minimum-
            // offset read_done slot each time so writes never overtake a lower
            // offset. A read_done slot with data (filled>0) is always written,
            // even after EOF was seen elsewhere; only empty (pure-EOF) slots are
            // retired without writing.
            for (;;) {
                if (ctx.cancel_token().is_requested()) {
                    fail(IoError{IoError::Code::canceled});
                    break;
                }
                // Find the lowest-offset read_done slot.
                PipelineSlot* ws = nullptr;
                for (auto& s : slots) {
                    if (s->state != SlotState::read_done) continue;
                    if (ws == nullptr || s->chunk_offset < ws->chunk_offset)
                        ws = s.get();
                }
                if (ws == nullptr) break;  // no read_done slot to write

                if (ws->filled == 0) {
                    // No data (pure EOF chunk): retire, nothing to write.
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

                // Slot fully written: retire it (this slot hit EOF, or EOF was
                // seen elsewhere so no more reads) or recycle for the next chunk
                // and submit a fresh read.
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
                        // Submit failed: this op never entered the backend. Stop
                        // issuing new reads and retire the slot; record the
                        // primary error (first one wins).
                        eof_seen = true;
                        ws->state = SlotState::done;
                        fail(rsr.error());
                        break;
                    }
                }
            }
            if (primary_error.has_value()) break;

            // Termination: every slot is `done` (all EOF chunks retired, all
            // data written) and there are no outstanding reads.
            bool all_done = true;
            for (auto& s : slots)
                if (s->state != SlotState::done) { all_done = false; break; }
            if (all_done) break;
        }

        // ---- Phase 3: cleanup drain. ----
        // On the error path, drain every still-outstanding read/write so the
        // Runtime can shut down (no outstanding Completion at context close).
        // First meaningful error wins; secondary/canceled results are consumed
        // and discarded. Submit-failed ops (never entered the backend) are
        // skipped: their Completion is idle, so await is skipped.
        for (auto& s : slots) {
            // Drain an outstanding read / write: await to terminal, consume
            // and discard the secondary outcome, reset for reuse.
            if (s->read_c.outstanding()) {
                auto dr = await_drain(ctx, s->read_c);
                if (!dr.has_value()) return make_unexpected<CopyStats>(dr.error());
            }
            if (s->write_c.outstanding()) {
                auto dr = await_drain(ctx, s->write_c);
                if (!dr.has_value()) return make_unexpected<CopyStats>(dr.error());
            }
        }

        if (primary_error.has_value())
            return make_unexpected<CopyStats>(primary_error.value());

        // ---- Phase 4: post-data sync policy (only on the clean path). ----
        // All read/write Completions are idle here.
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

        // Phase B (ADR-explicit-io-request-contract, Accepted, Decision 15):
        // the sync Completion's slot stays bound until reset()/ready-destruction
        // releases it (completion_ready -> free is the caller's handshake). The
        // context/backend must be quiescent (every slot free) before it is
        // destroyed, so release the sync slot here like every other Completion.
        sync_c.reset();

        return stats;
    }
};

}  // namespace

Result<CopyStats> run_sequential_copy(int src_fd, int dst_fd,
                                      std::size_t buffer_size,
                                      unsigned workers, SyncPolicy sync) {
    return run_pipelined_copy(src_fd, dst_fd, buffer_size, /*depth=*/1, workers,
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
    // ---- Argument validation (BEFORE any allocation or Runtime build). ----
    // This is the public entry point: the CLI, tests, and backend-injected
    // callers all defend here, not just the CLI. Reject zero/empty inputs,
    // the product overflow (memory upper bound ~ buffer_size * pipeline_depth),
    // and the app-level resource limits. Overflow is checked BEFORE the total
    // byte cap; the total cap is the primary constraint.
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

    // ---- Build ALL pipeline slots BEFORE the Runtime is built/started. ----
    // An allocation failure here cannot strand a started Runtime: slots are
    // allocated first, and std::bad_alloc is translated to IoError::no_space
    // so no exception escapes the public Result<T> boundary.
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

    // The library bridge runs the full lifecycle (build/start/submit/wait
    // publish/stop/drain/join + the task exception boundary), exactly the
    // audited application pattern: wait for the task's terminal outcome
    // BEFORE requesting stop (a run-to-completion copy is never aborted by
    // root cancellation), then drain/join so the Runtime closes quiescent.
    return run_task_to_result<CopyStats>(workers, std::move(backend), task);
}

Result<CopyStats> run_sequential_copy_with_backend(
    int src_fd, int dst_fd, std::size_t buffer_size, unsigned workers,
    SyncPolicy sync, std::unique_ptr<AsyncBackend> backend) {
    // Version A is Version B with pipeline_depth == 1.
    return run_pipelined_copy_with_backend(src_fd, dst_fd, buffer_size,
                                           /*pipeline_depth=*/1, workers, sync,
                                           std::move(backend));
}

}  // namespace sluice_copy
