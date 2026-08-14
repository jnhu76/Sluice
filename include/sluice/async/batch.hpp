// sluice::async::Batch — grouped completions (sluice-CORE-030, T4).
//
// Derived from Zig std.Io Batch (Io.zig:474-624). The lowest-level awaitable
// group: N operations submitted together, awaited as a whole, iterated in
// completion order via next().
//
// Zig models this as Operation.Storage[] + four intrusive lists (unused/
// submitted/pending/completed) driven by the backend's batchAwait* vtable.
// cppio's NARROWEST source-backed adaptation: Batch is a DRIVER over the
// existing AsyncIoContext — it submits N ops via the existing per-op submit_*,
// drives poll/wait_one, and surfaces completions in reap order. This reuses
// Completion<T> (no new op-storage type) and does NOT touch AsyncBackend (no
// new vtable entry) — the integration seam is the existing L1 surface.
//
// Why this and not a new Operation.Storage: Zig's Batch is tightly coupled to
// its Operation tagged union and the backend's batchAwait vtable, both of
// which cppio does not have (and which would be a much larger contract change,
// deferred). The driver-over-AsyncIoContext shape preserves the SEMANTIC
// contract — submit N, await >=1, iterate completion order, cancel as a whole
// (canceled ops absent from iteration; raced-completed ops present; exactly-
// once) — using the existing substrate. A future job may introduce a native
// Operation.Storage if a backend needs to bypass the per-op submit overhead.
//
// Layering: ABOVE AsyncIoContext (T0/T1). Composes Future/CancelToken only
// indirectly (through AsyncIoContext::cancel). No scheduler.
//
// Non-goals (deferred):
//   - awaitConcurrent() (requires a concurrency unit; PHASE E).
//   - Native Operation.Storage / batchAwait vtable entry (future job if needed).
#pragma once

#include <sluice/async/async_io_context.hpp>
#include <sluice/async/completion.hpp>
#include <sluice/error.hpp>
#include <sluice/result.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <variant>
#include <vector>

namespace sluice::async {

// A descriptor for one operation in a batch. Discriminated by the variant; the
// Batch driver dispatches to the matching AsyncIoContext::submit_*. This is the
// cppio analogue of Zig Operation (Io.zig:257) narrowed to file I/O (read/
// write/sync_data/sync_all — ADR §A5).
struct BatchOp {
    ReadOp read{};
    WriteOp write{};
    SyncDataOp sync_data{};
    SyncAllOp sync_all{};
    enum class Kind : std::uint8_t { read, write, sync_data, sync_all } kind = Kind::read;
};

// Admission origin of one batched operation (ADR Decision 9: "Batch must
// eventually consume outcome origin"). This is ORTHOGONAL to success/error: it
// records whether the submit crossed the commit/accept linearization point.
//
//   rejected               — submit failed BEFORE commit/accept (invalid
//                            descriptor, request capacity full, Completion
//                            already claimed, or other synchronous submit-time
//                            rejection). No accepted request existed.
//   accepted_and_completed — submit crossed commit/accept and later obtained a
//                            terminal result via reap. This includes successful
//                            I/O, accepted I/O returning an error, an accepted
//                            cancellation winner, and accepted post-commit
//                            dispatch failure. Therefore `origin ==
//                            accepted_and_completed` with an error Result is
//                            valid and distinct from `rejected`.
//
// Do NOT infer origin from whether the Result is success/error.
enum class BatchResultOrigin : std::uint8_t {
    rejected,
    accepted_and_completed,
};

// The result of one batched operation, returned by Batch::next(). Only the
// member matching `is_void` is populated; the other is nullopt.
struct BatchResult {
    std::size_t index = 0;                       // the add() index this matches
    BatchResultOrigin origin = BatchResultOrigin::accepted_and_completed;
    bool is_void = false;                        // sync ops carry no byte count
    std::optional<Result<std::size_t>> size_res; // populated when !is_void
    std::optional<Result<void>> void_res;        // populated when is_void
};

// A grouped-completion driver over AsyncIoContext. Submit N ops together, await
// >=1, iterate completions in reap order. Mirrors Zig Io.Batch (Io.zig:474).
class Batch {
public:
    Batch() = default;

    Batch(const Batch&) = delete;
    Batch& operator=(const Batch&) = delete;
    Batch(Batch&&) = delete;
    Batch& operator=(Batch&&) = delete;

    // Add one operation; returns its index (used to match BatchResult.index in
    // next()). Asserts nothing is currently submitted-but-not-yet-iterated
    // (call next() to drain completions before re-adding if needed).
    std::size_t add(BatchOp op);

    // Submit every added op to `ctx`, then drive ctx.wait_one() until >=1 op
    // completes.
    //
    // Returns the number of ops made ready this call on success. On a BACKEND
    // wait_one() error (E15-P2-01), the error is PROPAGATED via Result and any
    // ops that were made ready this call (Phase 1 submit-time errors, earlier
    // reaps in the same loop) REMAIN ready for next() to pop — the caller can
    // drain them before observing the error. A success-returning await_one
    // never silently swallows a backend error; a zero return on success
    // genuinely means "nothing newly ready".
    //
    // After this, next() yields completions. Idempotent-ish: ops already
    // submitted are not re-submitted.
    Result<std::size_t> await_one(AsyncIoContext& ctx);

    // Pop the next completed operation in actual BACKEND REAP order, or
    // nullopt if none ready. Each completion is dequeued exactly once. Mirrors
    // Zig Batch.next (Io.zig:551).
    //
    // E15-P1-04: ordering is by the monotonic reap sequence stamped on each
    // Completion by publish_from_reap() at backend reap time (ADR §6 O2). This
    // preserves the backend's true reap order across slot 0/1/2... regardless
    // of submission order: a backend that reaps slot 1 before slot 0 yields
    // next() returning slot 1 first, then slot 0. Submit-time failures
    // (Batch::await_one Phase 1 rejections) carry reap_seq 0 and surface
    // before any backend-reaped completion, in submission order among
    // themselves (ADR E5).
    //
    // It is not required to drain all completions before awaiting again.
    std::optional<BatchResult> next() noexcept;

    // Number of ops added but not yet popped via next() (submitted + pending +
    // completed). For inspection.
    std::size_t pending_count() const noexcept { return slots_.size() - popped_; }

private:
    struct Slot {
        BatchOp op;
        bool submitted = false;
        bool is_void = false;
        Completion<std::size_t> size_c;
        Completion<void> void_c;
        bool ready = false;       // result available to pop
        bool popped = false;      // already returned by next()
        // F2: admission origin. Set true ONLY when submit fails before
        // commit/accept (the slot is marked ready without going through
        // publish_from_reap, so its Completion keeps reap_seq 0). An accepted
        // request that later terminates with any result (success, error, or
        // canceled) leaves this false. This is the source of truth for
        // BatchResult::origin; reap_seq remains the source of truth for reap
        // ordering. The two agree because both derive from "did this slot reach
        // publish_from_reap".
        bool submit_rejected = false;
        std::optional<Result<std::size_t>> size_res{};
        std::optional<Result<void>> void_res{};
    };
    // unique_ptr so the vector may relocate POINTERS on grow without moving
    // the address-stable Completion<T> objects (ADR §5 L7).
    std::vector<std::unique_ptr<Slot>> slots_;
    std::size_t popped_ = 0;  // count of slots already returned by next()
};

}  // namespace sluice::async
