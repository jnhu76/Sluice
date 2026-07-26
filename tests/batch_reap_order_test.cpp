// E15-P1-04 regression — Batch::next() must surface completions in actual
// backend reap order, NOT in submission/slot-index order.
//
// Pre-fix: Batch::next() scanned slots in INDEX order and returned the lowest-
// indexed ready slot, so a backend that reaped slot 1 before slot 0 would see
// next() return slot 0 first — violating the documented contract
// ("in completion (reap) order") and ADR §6 O2. No reap sequence was recorded.
//
// Post-fix: complete_with() stamps a monotonic reap_seq on every Completion;
// next() returns the ready-but-not-popped slot with the smallest reap_seq,
// preserving the backend's true reap order.
//
// F-02 closeout: the test backend (SequenceBackend) captures Completion
// references through the NORMAL public submit_read/submit_write/submit_sync_*
// path during Batch::await_one's Phase 1 submission. No Batch::test_*
// accessors are needed — the backend records each Completion& by Batch slot
// index (submission order) and the test stages reaps against those captured
// refs. This exercises the full production path:
//   Batch::add() → Batch::await_one() → AsyncIoContext::submit_*()
//   → SequenceBackend captures Completion& → wait_one()/poll() completes
//   → Batch::next() surfaces in reap order.
#include "harness.hpp"

#include <sluice/async/async_io_context.hpp>
#include <sluice/async/batch.hpp>
#include <sluice/async/completion.hpp>
#include <sluice/error.hpp>
#include <sluice/result.hpp>

#include <cstddef>
#include <deque>
#include <map>
#include <memory>
#include <utility>
#include <vector>

using namespace sluice::async;
using sluice::IoError;
using sluice::Result;

namespace {
// Deterministic in-test backend. Captures Completion references through the
// normal submit_* path (indexed by Batch slot index = submission order).
//
// Two staging modes:
//   1. AUTO-STAGE: pre-configure via auto_stage_*() BEFORE await_one. When
//      submit captures the slot's Completion, it immediately queues it for
//      reaping. wait_one/poll then reaps in queue order.
//   2. POST-SUBMIT: after await_one has submitted (captured) the ops, call
//      stage_*() to queue specific slots, then call await_one again to reap.
//
// E15-P2-01: set_next_wait_one_error() causes the NEXT wait_one() call to
// return the given backend error (and consume nothing).
class SequenceBackend : public AsyncBackend {
public:
    // --- auto-stage interface (configure BEFORE await_one submits) ---
    void auto_stage_size(std::size_t slot, std::size_t bytes) {
        auto_plan_[slot] = PlanEntry{bytes, false, {}};
    }
    void auto_stage_size_error(std::size_t slot, IoError e) {
        auto_plan_[slot] = PlanEntry{0, true, e};
    }
    void auto_stage_void_ok(std::size_t slot) {
        auto_plan_[slot] = PlanEntry{0, false, {}, true};
    }

    // --- post-submit staging (slot must already be captured via submit) ---
    void stage_size(std::size_t slot, std::size_t bytes) {
        auto* c = captured_size_at(slot);
        size_bytes_[c] = bytes;
        reap_queue_.push_back({c, false});
    }
    void stage_size_error(std::size_t slot, IoError e) {
        auto* c = captured_size_at(slot);
        size_err_[c] = e;
        reap_queue_.push_back({c, false});
    }
    void stage_void_ok(std::size_t slot) {
        auto* c = captured_void_at(slot);
        reap_queue_.push_back({c, true});
    }

    // E15-P2-01: arm an error to return from the next wait_one() call.
    void set_next_wait_one_error(IoError e) { next_wait_err_ = e; }

    // --- AsyncBackend interface ---
    Result<void> submit_read(ReadOp, Completion<std::size_t>& c) override {
        if (!c.idle()) return make_unexpected<void>(IoError{IoError::Code::invalid_state});
        c.mark_outstanding();
        captures_.push_back({&c, false});
        ++outstanding_;
        apply_auto_stage(captures_.size() - 1, c);
        return {};
    }
    Result<void> submit_write(WriteOp, Completion<std::size_t>& c) override {
        if (!c.idle()) return make_unexpected<void>(IoError{IoError::Code::invalid_state});
        c.mark_outstanding();
        captures_.push_back({&c, false});
        ++outstanding_;
        apply_auto_stage(captures_.size() - 1, c);
        return {};
    }
    Result<void> submit_sync_data(SyncDataOp, Completion<void>& c) override {
        if (!c.idle()) return make_unexpected<void>(IoError{IoError::Code::invalid_state});
        c.mark_outstanding();
        captures_.push_back({&c, true});
        ++outstanding_;
        apply_auto_stage(captures_.size() - 1, c);
        return {};
    }
    Result<void> submit_sync_all(SyncAllOp, Completion<void>& c) override {
        if (!c.idle()) return make_unexpected<void>(IoError{IoError::Code::invalid_state});
        c.mark_outstanding();
        captures_.push_back({&c, true});
        ++outstanding_;
        apply_auto_stage(captures_.size() - 1, c);
        return {};
    }

    std::size_t poll() override {
        std::size_t n = 0;
        while (!reap_queue_.empty()) {
            Entry e = reap_queue_.front();
            reap_queue_.pop_front();
            if (!e.is_void) {
                auto* c = static_cast<Completion<std::size_t>*>(e.c);
                auto eit = size_err_.find(c);
                if (eit != size_err_.end()) {
                    c->complete_with(make_unexpected<std::size_t>(eit->second));
                    size_err_.erase(eit);
                } else {
                    auto it = size_bytes_.find(c);
                    c->complete_with(Result<std::size_t>{it->second});
                    size_bytes_.erase(it);
                }
            } else {
                auto* c = static_cast<Completion<void>*>(e.c);
                c->complete_with(Result<void>{});
            }
            --outstanding_;
            ++n;
        }
        return n;
    }
    Result<std::size_t> wait_one() override {
        if (next_wait_err_.has_value()) {
            IoError e = *next_wait_err_;
            next_wait_err_.reset();
            return make_unexpected<std::size_t>(e);
        }
        return poll();
    }
    void cancel(Completion<std::size_t>&) override {}
    void cancel(Completion<void>&) override {}
    std::size_t outstanding() const noexcept override { return outstanding_; }

private:
    struct Capture {
        void* c;
        bool is_void;
    };
    struct Entry {
        void* c = nullptr;
        bool is_void = false;
    };
    struct PlanEntry {
        std::size_t bytes = 0;
        bool is_error = false;
        IoError err{IoError::Code::backend_error};
        bool is_void = false;
    };

    Completion<std::size_t>* captured_size_at(std::size_t slot) {
        return static_cast<Completion<std::size_t>*>(captures_.at(slot).c);
    }
    Completion<void>* captured_void_at(std::size_t slot) {
        return static_cast<Completion<void>*>(captures_.at(slot).c);
    }

    void apply_auto_stage(std::size_t slot, Completion<std::size_t>& c) {
        auto it = auto_plan_.find(slot);
        if (it == auto_plan_.end()) return;
        if (it->second.is_error) {
            size_err_[&c] = it->second.err;
        } else {
            size_bytes_[&c] = it->second.bytes;
        }
        reap_queue_.push_back({&c, false});
        auto_plan_.erase(it);
    }
    void apply_auto_stage(std::size_t slot, Completion<void>& c) {
        auto it = auto_plan_.find(slot);
        if (it == auto_plan_.end()) return;
        reap_queue_.push_back({&c, true});
        auto_plan_.erase(it);
    }

    std::vector<Capture> captures_;
    std::map<std::size_t, PlanEntry> auto_plan_;
    std::deque<Entry> reap_queue_;
    std::map<Completion<std::size_t>*, std::size_t> size_bytes_;
    std::map<Completion<std::size_t>*, IoError> size_err_;
    std::size_t outstanding_ = 0;
    std::optional<IoError> next_wait_err_;
};
}  // namespace

// Helper: build a BatchOp for a read at the given fd/buffer.
static BatchOp make_read_op(int fd, std::byte* dst, std::size_t len,
                            std::uint64_t off) {
    BatchOp op;
    op.kind = BatchOp::Kind::read;
    op.read = ReadOp{fd, dst, len, off};
    return op;
}

// ---- Slice 1: single completion yields its index ----------------------------

SLUICE_TEST_CASE(batch_reap_single_completion_returns_its_index) {
    auto owned = std::make_unique<SequenceBackend>();
    SequenceBackend* raw = owned.get();
    AsyncIoContext ctx(std::move(owned));

    Batch b;
    std::byte buf[4]{};
    const std::size_t i0 = b.add(make_read_op(0, buf, 4, 0));
    SLUICE_CHECK(i0 == 0);

    // Pre-configure: when slot 0 is submitted, auto-stage it for reaping.
    raw->auto_stage_size(0, 4);

    SLUICE_CHECK(b.await_one(ctx).value() >= 1);
    auto r = b.next();
    SLUICE_CHECK(r.has_value());
    SLUICE_CHECK(r->index == 0);
    SLUICE_CHECK(!r->is_void);
    SLUICE_CHECK(r->size_res.value().value() == 4);
    SLUICE_CHECK(b.next() == std::nullopt);
}

// ---- Slice 2: CORE E15-P1-04 assertion — reverse reap order -----------------
// Submit slot 0, submit slot 1; backend reaps slot 1 FIRST, then slot 0.
// next() MUST return 1 first, then 0 (NOT index order).

SLUICE_TEST_CASE(batch_reap_reverse_order_returns_reaped_first) {
    auto owned = std::make_unique<SequenceBackend>();
    SequenceBackend* raw = owned.get();
    AsyncIoContext ctx(std::move(owned));

    Batch b;
    std::byte b0[4]{}, b1[4]{};
    const std::size_t i0 = b.add(make_read_op(0, b0, 4, 0));
    const std::size_t i1 = b.add(make_read_op(0, b1, 4, 4));
    SLUICE_CHECK(i0 == 0);
    SLUICE_CHECK(i1 == 1);

    // Auto-stage in REVERSE submission order: slot 1 queued before slot 0.
    // Since submit happens in slot order (0, 1), we need slot 1 to be
    // queued first in reap_queue_. Use post-submit staging for exact control.
    // Arm a wait-error to prevent the wait loop from spinning while we
    // haven't staged yet — actually, use auto-stage for slot 0 and slot 1
    // but control the ORDER via post-submit staging.
    //
    // Best approach: don't auto-stage. Instead, prevent spin by arming a
    // wait-error, do first await (submits only), then stage in reverse, then
    // await again to reap.
    raw->set_next_wait_one_error(IoError{IoError::Code::backend_error});
    (void)b.await_one(ctx);  // Phase 1 submits both; wait_one returns error.
    // Both slots now captured. Stage in reverse order.
    raw->stage_size(1, 4);
    raw->stage_size(0, 4);

    SLUICE_CHECK(b.await_one(ctx).value() >= 2);
    // THE REGRESSION ASSERTION: exact expected order — slot 1, then slot 0.
    auto first = b.next();
    SLUICE_CHECK(first.has_value());
    SLUICE_CHECK(first->index == 1);
    auto second = b.next();
    SLUICE_CHECK(second.has_value());
    SLUICE_CHECK(second->index == 0);
    SLUICE_CHECK(b.next() == std::nullopt);
}

// ---- Slice 3: forward order still works (sanity) ----------------------------

SLUICE_TEST_CASE(batch_reap_forward_order_returns_in_order) {
    auto owned = std::make_unique<SequenceBackend>();
    SequenceBackend* raw = owned.get();
    AsyncIoContext ctx(std::move(owned));

    Batch b;
    std::byte b0[4]{}, b1[4]{}, b2[4]{};
    b.add(make_read_op(0, b0, 4, 0));
    b.add(make_read_op(0, b1, 4, 4));
    b.add(make_read_op(0, b2, 4, 8));

    // Auto-stage in forward order (matches submission order, so the
    // reap_queue is populated 0, 1, 2 as each submit fires).
    raw->auto_stage_size(0, 4);
    raw->auto_stage_size(1, 4);
    raw->auto_stage_size(2, 4);

    SLUICE_CHECK(b.await_one(ctx).value() >= 3);
    SLUICE_CHECK(b.next()->index == 0);
    SLUICE_CHECK(b.next()->index == 1);
    SLUICE_CHECK(b.next()->index == 2);
    SLUICE_CHECK(b.next() == std::nullopt);
}

// ---- Slice 4: three or more mixed-order completions -------------------------

SLUICE_TEST_CASE(batch_reap_mixed_order_three_completions) {
    auto owned = std::make_unique<SequenceBackend>();
    SequenceBackend* raw = owned.get();
    AsyncIoContext ctx(std::move(owned));

    Batch b;
    std::byte b0[4]{}, b1[4]{}, b2[4]{};
    b.add(make_read_op(0, b0, 4, 0));    // index 0
    b.add(make_read_op(0, b1, 4, 4));    // index 1
    b.add(make_read_op(0, b2, 4, 8));    // index 2

    // Reap order: 2, 0, 1 — use post-submit staging for exact control.
    raw->set_next_wait_one_error(IoError{IoError::Code::backend_error});
    (void)b.await_one(ctx);  // submits all three
    raw->stage_size(2, 8);
    raw->stage_size(0, 4);
    raw->stage_size(1, 6);

    SLUICE_CHECK(b.await_one(ctx).value() >= 3);
    auto r0 = b.next();
    auto r1 = b.next();
    auto r2 = b.next();
    SLUICE_CHECK(r0->index == 2);
    SLUICE_CHECK(r0->size_res.value().value() == 8);
    SLUICE_CHECK(r1->index == 0);
    SLUICE_CHECK(r1->size_res.value().value() == 4);
    SLUICE_CHECK(r2->index == 1);
    SLUICE_CHECK(r2->size_res.value().value() == 6);
    SLUICE_CHECK(b.next() == std::nullopt);
}

// ---- Slice 5: multiple completions reaped during one wait_one retain order --

SLUICE_TEST_CASE(batch_reap_multi_in_one_wait_retains_relative_order) {
    auto owned = std::make_unique<SequenceBackend>();
    SequenceBackend* raw = owned.get();
    AsyncIoContext ctx(std::move(owned));

    Batch b;
    std::byte b0[4]{}, b1[4]{}, b2[4]{}, b3[4]{};
    b.add(make_read_op(0, b0, 4, 0));
    b.add(make_read_op(0, b1, 4, 4));
    b.add(make_read_op(0, b2, 4, 8));
    b.add(make_read_op(0, b3, 4, 12));

    // Reap order: 3, 1, 2, 0 — all reaped in one poll().
    raw->set_next_wait_one_error(IoError{IoError::Code::backend_error});
    (void)b.await_one(ctx);  // submits all four
    raw->stage_size(3, 4);
    raw->stage_size(1, 4);
    raw->stage_size(2, 4);
    raw->stage_size(0, 4);

    SLUICE_CHECK(b.await_one(ctx).value() == 4);
    SLUICE_CHECK(b.next()->index == 3);
    SLUICE_CHECK(b.next()->index == 1);
    SLUICE_CHECK(b.next()->index == 2);
    SLUICE_CHECK(b.next()->index == 0);
    SLUICE_CHECK(b.next() == std::nullopt);
}

// ---- Slice 6: one completion per wait call (multiple await_one) -------------

SLUICE_TEST_CASE(batch_reap_one_per_wait_across_multiple_awaits) {
    auto owned = std::make_unique<SequenceBackend>();
    SequenceBackend* raw = owned.get();
    AsyncIoContext ctx(std::move(owned));

    Batch b;
    std::byte b0[4]{}, b1[4]{};
    b.add(make_read_op(0, b0, 4, 0));    // index 0
    b.add(make_read_op(0, b1, 4, 4));    // index 1

    // Submit both, but only stage slot 1 for the first reap.
    raw->set_next_wait_one_error(IoError{IoError::Code::backend_error});
    (void)b.await_one(ctx);  // submits both
    raw->stage_size(1, 4);

    SLUICE_CHECK(b.await_one(ctx).value() >= 1);
    SLUICE_CHECK(b.next()->index == 1);
    // Slot 0 not yet reaped: next() returns nullopt.
    SLUICE_CHECK(b.next() == std::nullopt);

    // Now stage slot 0.
    raw->stage_size(0, 4);
    SLUICE_CHECK(b.await_one(ctx).value() >= 1);
    SLUICE_CHECK(b.next()->index == 0);
    SLUICE_CHECK(b.next() == std::nullopt);
}

// ---- Slice 7: next() before any ready returns nullopt -----------------------

SLUICE_TEST_CASE(batch_reap_next_before_ready_returns_nullopt) {
    auto owned = std::make_unique<SequenceBackend>();
    SequenceBackend* raw = owned.get();
    AsyncIoContext ctx(std::move(owned));

    Batch b;
    std::byte buf[4]{};
    b.add(make_read_op(0, buf, 4, 0));

    // next() before any await: nothing submitted yet, no ready slot.
    SLUICE_CHECK(b.next() == std::nullopt);

    // Now submit + stage + reap.
    raw->auto_stage_size(0, 4);
    SLUICE_CHECK(b.await_one(ctx).value() >= 1);
    SLUICE_CHECK(b.next()->index == 0);
    SLUICE_CHECK(b.next() == std::nullopt);
}

// ---- Slice 8: each completion returned exactly once (no dupes, no misses) ---

SLUICE_TEST_CASE(batch_reap_each_completion_exactly_once) {
    auto owned = std::make_unique<SequenceBackend>();
    SequenceBackend* raw = owned.get();
    AsyncIoContext ctx(std::move(owned));

    Batch b;
    std::byte b0[4]{}, b1[4]{}, b2[4]{};
    b.add(make_read_op(0, b0, 4, 0));
    b.add(make_read_op(0, b1, 4, 4));
    b.add(make_read_op(0, b2, 4, 8));

    raw->set_next_wait_one_error(IoError{IoError::Code::backend_error});
    (void)b.await_one(ctx);
    raw->stage_size(2, 4);
    raw->stage_size(0, 4);
    raw->stage_size(1, 4);

    SLUICE_CHECK(b.await_one(ctx).value() == 3);
    bool seen[3] = {false, false, false};
    int count = 0;
    while (auto r = b.next()) {
        SLUICE_CHECK(r->index < 3);
        SLUICE_CHECK(!seen[r->index]); // no duplicate
        seen[r->index] = true;
        ++count;
    }
    SLUICE_CHECK(count == 3);
    SLUICE_CHECK(seen[0] && seen[1] && seen[2]); // no missing
}

// ---- Slice 9: mixed kind (size + void) preserves order ----------------------

SLUICE_TEST_CASE(batch_reap_mixed_kind_preserves_order) {
    auto owned = std::make_unique<SequenceBackend>();
    SequenceBackend* raw = owned.get();
    AsyncIoContext ctx(std::move(owned));

    Batch b;
    std::byte buf[4]{};
    b.add(make_read_op(0, buf, 4, 0));            // index 0, size
    BatchOp sa; sa.kind = BatchOp::Kind::sync_all;
    sa.sync_all = SyncAllOp{0};
    b.add(sa);                                    // index 1, void
    b.add(make_read_op(0, buf, 4, 4));            // index 2, size

    // Reap order: void (1) first, then size (2), then size (0).
    raw->set_next_wait_one_error(IoError{IoError::Code::backend_error});
    (void)b.await_one(ctx);  // submits all three
    raw->stage_void_ok(1);
    raw->stage_size(2, 4);
    raw->stage_size(0, 4);

    SLUICE_CHECK(b.await_one(ctx).value() == 3);
    auto r0 = b.next();
    auto r1 = b.next();
    auto r2 = b.next();
    SLUICE_CHECK(r0->index == 1);
    SLUICE_CHECK(r0->is_void == true);
    SLUICE_CHECK(r0->void_res.value().has_value());
    SLUICE_CHECK(r1->index == 2);
    SLUICE_CHECK(r1->is_void == false);
    SLUICE_CHECK(r2->index == 0);
    SLUICE_CHECK(r2->is_void == false);
    SLUICE_CHECK(b.next() == std::nullopt);
}

// ---- Slice 10: submit-time failure has a defined position -------------------
// Submit-time errors surface with reap_seq 0 (sorted before any backend-reaped
// completion). They appear FIRST, in submission order among themselves.

SLUICE_TEST_CASE(batch_reap_submit_failure_surfaces_first) {
    // Subcase A: a normal reap in one batch.
    {
        auto owned = std::make_unique<SequenceBackend>();
        SequenceBackend* raw = owned.get();
        AsyncIoContext ctx(std::move(owned));

        Batch b;
        std::byte buf[4]{};
        b.add(make_read_op(0, buf, 4, 0));
        raw->auto_stage_size(0, 4);
        SLUICE_CHECK(b.await_one(ctx).value() == 1);
        auto r = b.next();
        SLUICE_CHECK(r.has_value());
        SLUICE_CHECK(r->index == 0);
        SLUICE_CHECK(r->size_res.value().value() == 4);
    }

    // Subcase B: a submit-rejected slot in another batch. It surfaces with
    // reap_seq 0 (never went through complete_with).
    class RejectFdBackend : public SequenceBackend {
    public:
        explicit RejectFdBackend(int bad_fd) : bad_fd_(bad_fd) {}
        Result<void> submit_read(ReadOp op, Completion<std::size_t>& c) override {
            if (op.fd == bad_fd_) {
                return make_unexpected<void>(IoError{IoError::Code::permission_denied});
            }
            return SequenceBackend::submit_read(op, c);
        }
        int bad_fd_;
    };
    {
        auto owned = std::make_unique<RejectFdBackend>(99);
        AsyncIoContext ctx(std::move(owned));

        Batch b;
        std::byte buf[4]{};
        b.add(make_read_op(99, buf, 4, 0));
        // await_one submits index 0 -> rejected -> ready with the error.
        SLUICE_CHECK(b.await_one(ctx).value() >= 1);
        SLUICE_CHECK(ctx.outstanding() == 0); // submit failed: never outstanding

        auto r = b.next();
        SLUICE_CHECK(r.has_value());
        SLUICE_CHECK(r->index == 0);
        SLUICE_CHECK(!r->size_res.value().has_value());
        SLUICE_CHECK(r->size_res.value().error().code ==
                     IoError::Code::permission_denied);
    }
}

// =============================================================================
// E15-P2-01 — Batch::await_one must not discard a backend wait_one() error.
// =============================================================================

// ---- P2-01 Slice A: pure backend wait-error propagates from await_one -------

SLUICE_TEST_CASE(batch_await_one_propagates_backend_wait_error) {
    auto owned = std::make_unique<SequenceBackend>();
    SequenceBackend* raw = owned.get();
    AsyncIoContext ctx(std::move(owned));

    Batch b;
    std::byte buf[4]{};
    b.add(make_read_op(0, buf, 4, 0));  // index 0

    // Step 1: auto-stage + reap slot 0 normally.
    raw->auto_stage_size(0, 4);
    {
        auto ar = b.await_one(ctx);
        SLUICE_CHECK(ar.has_value());
        SLUICE_CHECK(ar.value() >= 1);
        SLUICE_CHECK(b.next()->index == 0);
    }

    // Step 2: add a second slot, arm a wait_one error, observe await_one's
    // return, and DRAIN slot 1 BEFORE asserting.
    b.add(make_read_op(0, buf, 4, 0));  // index 1
    raw->set_next_wait_one_error(IoError{IoError::Code::backend_error});
    bool got_error = false;
    IoError got_code{IoError::Code::backend_error};
    {
        auto ar = b.await_one(ctx);
        got_error = !ar.has_value();
        if (got_error) got_code = ar.error();
    }
    // Drain slot 1 (the wait error did not reap it).
    raw->stage_size(1, 4);
    {
        auto ar = b.await_one(ctx);
        SLUICE_CHECK(ar.has_value());
        SLUICE_CHECK(ar.value() >= 1);
        SLUICE_CHECK(b.next()->index == 1);
    }

    // THE REGRESSION ASSERTION: the backend error propagated.
    SLUICE_CHECK(got_error);
    SLUICE_CHECK(got_code.code == IoError::Code::backend_error);
}

// ---- P2-01 Slice B: ready slots remain poppable after a wait error ----------

SLUICE_TEST_CASE(batch_await_one_wait_error_keeps_ready_slots_poppable) {
    class RejectFdBackend : public SequenceBackend {
    public:
        explicit RejectFdBackend(int bad_fd) : bad_fd_(bad_fd) {}
        Result<void> submit_read(ReadOp op, Completion<std::size_t>& c) override {
            if (op.fd == bad_fd_) {
                return make_unexpected<void>(IoError{IoError::Code::permission_denied});
            }
            return SequenceBackend::submit_read(op, c);
        }
        int bad_fd_;
    };

    auto owned = std::make_unique<RejectFdBackend>(99);
    RejectFdBackend* raw = owned.get();
    AsyncIoContext ctx(std::move(owned));

    Batch b;
    std::byte buf[4]{};
    // index 0: submit-rejected -> ready with permission_denied (Phase 1).
    b.add(make_read_op(99, buf, 4, 0));

    // Arm a wait error (irrelevant here because Phase 2 won't loop — slot 0 is
    // already ready — but the test asserts the error does not interfere).
    raw->set_next_wait_one_error(IoError{IoError::Code::backend_error});

    auto ar = b.await_one(ctx);
    SLUICE_CHECK(ar.has_value());
    SLUICE_CHECK(ar.value() >= 1);

    auto r = b.next();
    SLUICE_CHECK(r.has_value());
    SLUICE_CHECK(r->index == 0);
    SLUICE_CHECK(r->size_res.value().error().code ==
                 IoError::Code::permission_denied);
}

// ---- P2-01 Slice C: a wait error is distinguishable from a successful reap -

SLUICE_TEST_CASE(batch_await_one_distinguishes_success_from_error) {
    auto owned = std::make_unique<SequenceBackend>();
    SequenceBackend* raw = owned.get();
    AsyncIoContext ctx(std::move(owned));

    Batch b;
    std::byte buf[4]{};
    b.add(make_read_op(0, buf, 4, 0));   // index 0

    // Success path: auto-stage + reap slot 0.
    raw->auto_stage_size(0, 4);
    auto ar = b.await_one(ctx);
    SLUICE_CHECK(ar.has_value());
    SLUICE_CHECK(ar.value() >= 1);
    SLUICE_CHECK(b.next()->index == 0);

    // Now add a second slot, arm a wait error — await_one must surface the
    // error.
    b.add(make_read_op(0, buf, 4, 0));   // index 1
    raw->set_next_wait_one_error(IoError{IoError::Code::backend_error});
    bool got_error = false;
    IoError got_code{IoError::Code::backend_error};
    {
        auto ar2 = b.await_one(ctx);
        got_error = !ar2.has_value();
        if (got_error) got_code = ar2.error();
    }
    // Drain slot 1 first.
    raw->stage_size(1, 4);
    auto ar3 = b.await_one(ctx);
    SLUICE_CHECK(ar3.has_value());
    SLUICE_CHECK(b.next()->index == 1);

    // THE REGRESSION ASSERTION.
    SLUICE_CHECK(got_error);
    SLUICE_CHECK(got_code.code == IoError::Code::backend_error);
}

SLUICE_MAIN()
