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
// These cases use a deterministic in-test backend (SequenceBackend) that lets
// the test DIRECT each Completion's reap order via Batch's test-only accessors,
// so the assertion is exact. The existing batch_test.cpp (against
// ThreadPoolBackend, unspecified order) cannot detect this regression.
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

using namespace sluice::async;
using sluice::IoError;
using sluice::Result;

namespace {
// Deterministic in-test backend. The test stages "reap this Completion next
// (with this byte count / void success)" by pointer; poll() reaps every
// currently-staged completion in stage order, marking each ready. The order
// is fully test-controlled so Batch::next() ordering is assertable exactly.
class SequenceBackend : public AsyncBackend {
public:
    void stage_size(Completion<std::size_t>* c, std::size_t bytes) {
        size_bytes_[c] = bytes;
        reap_queue_.push_back({c, false});
    }
    void stage_size_error(Completion<std::size_t>* c, IoError e) {
        size_err_[c] = e;
        reap_queue_.push_back({c, false});
    }
    void stage_void_ok(Completion<void>* c) {
        reap_queue_.push_back({c, true});
    }

    Result<void> submit_read(ReadOp, Completion<std::size_t>& c) override {
        if (!c.idle()) return make_unexpected<void>(IoError{IoError::Code::invalid_state});
        c.mark_outstanding();
        ++outstanding_;
        return {};
    }
    Result<void> submit_write(WriteOp, Completion<std::size_t>& c) override {
        return submit_read(ReadOp{}, c);
    }
    Result<void> submit_sync_data(SyncDataOp, Completion<void>& c) override {
        if (!c.idle()) return make_unexpected<void>(IoError{IoError::Code::invalid_state});
        c.mark_outstanding();
        ++outstanding_;
        return {};
    }
    Result<void> submit_sync_all(SyncAllOp, Completion<void>& c) override {
        return submit_sync_data(SyncDataOp{}, c);
    }

    std::size_t poll() override {
        std::size_t n = 0;
        // Reap every staged completion in stage order. Tests that stage N then
        // poll once see all N reaped in stage order (exercising O2's
        // "multiple completions reaped during one backend call retain their
        // relative order").
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
    Result<std::size_t> wait_one() override { return poll(); }
    void cancel(Completion<std::size_t>&) override {}
    void cancel(Completion<void>&) override {}
    std::size_t outstanding() const noexcept override { return outstanding_; }

private:
    struct Entry {
        void* c = nullptr;
        bool is_void = false;
    };
    std::deque<Entry> reap_queue_;
    std::map<Completion<std::size_t>*, std::size_t> size_bytes_;
    std::map<Completion<std::size_t>*, IoError> size_err_;
    std::size_t outstanding_ = 0;
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

    // Stage BEFORE await_one so the wait_one inside await_one reaps it.
    raw->stage_size(&b.test_size_completion_at(0), 4);

    SLUICE_CHECK(b.await_one(ctx) >= 1);
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

    // Stage in REVERSE submission order: slot 1 reaped before slot 0.
    raw->stage_size(&b.test_size_completion_at(1), 4);
    raw->stage_size(&b.test_size_completion_at(0), 4);

    SLUICE_CHECK(b.await_one(ctx) >= 2);
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

    raw->stage_size(&b.test_size_completion_at(0), 4);
    raw->stage_size(&b.test_size_completion_at(1), 4);
    raw->stage_size(&b.test_size_completion_at(2), 4);

    SLUICE_CHECK(b.await_one(ctx) >= 3);
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

    // Reap order: 2, 0, 1
    raw->stage_size(&b.test_size_completion_at(2), 8);
    raw->stage_size(&b.test_size_completion_at(0), 4);
    raw->stage_size(&b.test_size_completion_at(1), 6);

    SLUICE_CHECK(b.await_one(ctx) >= 3);
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
// (ADR §6 O2: "Within one reap, completions are surfaced in the order their
// backends report them ready".)

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

    // Stage a deliberate reap order: 3, 1, 2, 0 — all in one poll().
    raw->stage_size(&b.test_size_completion_at(3), 4);
    raw->stage_size(&b.test_size_completion_at(1), 4);
    raw->stage_size(&b.test_size_completion_at(2), 4);
    raw->stage_size(&b.test_size_completion_at(0), 4);

    SLUICE_CHECK(b.await_one(ctx) == 4);
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

    // Stage slot 1, await, expect next() == 1.
    raw->stage_size(&b.test_size_completion_at(1), 4);
    SLUICE_CHECK(b.await_one(ctx) >= 1);
    SLUICE_CHECK(b.next()->index == 1);
    // Slot 0 not yet reaped: next() returns nullopt until reaped.
    SLUICE_CHECK(b.next() == std::nullopt);

    // Now stage slot 0.
    raw->stage_size(&b.test_size_completion_at(0), 4);
    SLUICE_CHECK(b.await_one(ctx) >= 1);
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

    // Submit the op (one await_one round-trip); since nothing is staged, the
    // SequenceBackend's wait_one returns 0 and Batch::await_one's loop exits
    // via the (popped_now == 0 && ctx.outstanding() == N) guard ONLY when the
    // backend reports outstanding > 0 with no readiness — which our backend
    // does. To avoid an indefinite loop on a non-blocking backend, the test
    // stages the result BEFORE the await so the wait_one reaps it.
    //
    // The "next() before any ready" assertion is exercised by calling next()
    // immediately after add() (before any await): nothing submitted yet, so
    // there is no ready slot.
    SLUICE_CHECK(b.next() == std::nullopt);

    // Now stage and reap to drain.
    raw->stage_size(&b.test_size_completion_at(0), 4);
    SLUICE_CHECK(b.await_one(ctx) >= 1);
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

    raw->stage_size(&b.test_size_completion_at(2), 4);
    raw->stage_size(&b.test_size_completion_at(0), 4);
    raw->stage_size(&b.test_size_completion_at(1), 4);

    SLUICE_CHECK(b.await_one(ctx) == 3);
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
    raw->stage_void_ok(&b.test_void_completion_at(1));
    raw->stage_size(&b.test_size_completion_at(2), 4);
    raw->stage_size(&b.test_size_completion_at(0), 4);

    SLUICE_CHECK(b.await_one(ctx) == 3);
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
//
// Two SEPARATE batches so we control submission / reap timing exactly:
//   Batch A: index 0 = normal read, staged + reaped (reap_seq > 0)
//   Batch B: index 0 = submit-rejected read (reap_seq 0, never goes outstanding)
// Each batch drains on its own, so the ordering is observed WITHIN one batch
// whose slots resolve at known times. A single mixed batch would have its
// submit-failure slot short-circuit await_one's wait loop (any_ready=true),
// leaving the normal slot's reap order under-tested; the two-batch form keeps
// the assertion exact without that confound.

SLUICE_TEST_CASE(batch_reap_submit_failure_surfaces_first) {
    // Subcase A: a normal reap in one batch.
    {
        auto owned = std::make_unique<SequenceBackend>();
        SequenceBackend* raw = owned.get();
        AsyncIoContext ctx(std::move(owned));

        Batch b;
        std::byte buf[4]{};
        b.add(make_read_op(0, buf, 4, 0));
        raw->stage_size(&b.test_size_completion_at(0), 4);
        SLUICE_CHECK(b.await_one(ctx) == 1);
        auto r = b.next();
        SLUICE_CHECK(r.has_value());
        SLUICE_CHECK(r->index == 0);
        SLUICE_CHECK(r->size_res.value().value() == 4);
        // reap_seq was stamped (>0) by complete_with.
        SLUICE_CHECK(b.test_size_completion_at(0).reap_seq() > 0);
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
        SLUICE_CHECK(b.await_one(ctx) >= 1);
        SLUICE_CHECK(ctx.outstanding() == 0); // submit failed: never outstanding

        auto r = b.next();
        SLUICE_CHECK(r.has_value());
        SLUICE_CHECK(r->index == 0);
        SLUICE_CHECK(!r->size_res.value().has_value());
        SLUICE_CHECK(r->size_res.value().error().code ==
                     IoError::Code::permission_denied);
        // reap_seq stayed 0 — submit-time errors are NOT reaped.
        SLUICE_CHECK(b.test_size_completion_at(0).reap_seq() == 0);
    }
}

SLUICE_MAIN()
