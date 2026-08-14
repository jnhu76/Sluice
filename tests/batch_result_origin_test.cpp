// F2 (issue #98) — BatchResult must expose the admission origin explicitly:
// `rejected` (submit failed BEFORE commit/accept) vs `accepted_and_completed`
// (submit crossed commit, later reached a terminal result via reap).
//
// ADR-explicit-io-request-contract Decision 9: "Batch must eventually consume
// outcome origin ... rather than reconstructing order with the process-global
// reap_seq." The origin is ORTHOGONAL to success/error: an accepted request
// that terminates with an error, or with `canceled`, is `accepted_and_completed`
// — NOT `rejected`. This file proves that distinction through the public
// BatchResult surface only (no internal/testing seams).
//
// RED on first commit: BatchResult has no `origin` field and no
// BatchResultOrigin enum, so this file does not compile until F2 lands.
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
#include <optional>
#include <utility>
#include <vector>

using namespace sluice::async;
using sluice::IoError;
using sluice::Result;

namespace {
// Deterministic in-test backend. Supports:
//   - submit-rejection by fd (the slot becomes `rejected` origin); and
//   - exact backend reap order via post-submit stage_* (accepted completions
//     are reaped through publish_from_reap, so they carry a non-zero reap_seq
//     and become `accepted_and_completed`).
// Mirrors tests/batch_reap_order_test.cpp::SequenceBackend, plus a reject map.
class OriginBackend : public AsyncBackend {
public:
    // Configure a fd whose submit_read/submit_write is synchronously rejected
    // with `e` (never claimed, never outstanding).
    void reject_fd(int fd, IoError e) { reject_[fd] = e; }

    // E15-P2-01: arm an error to return from the next wait_one() (used to let
    // await_one submit-and-capture without reaping, so reap order can be staged
    // after capture).
    void set_next_wait_one_error(IoError e) { next_wait_err_ = e; }

    // Auto-stage: when the slot at `slot` is submitted, immediately queue it
    // for reap with `bytes` (reaped on the next poll/wait_one).
    void auto_stage_size(std::size_t slot, std::size_t bytes) {
        auto_plan_[slot] = Plan{bytes, false, {}};
    }

    // Post-submit staging: queue an already-captured slot for reap with `bytes`.
    void stage_size(std::size_t slot, std::size_t bytes) {
        Completion<std::size_t>* c = captured_size_at(slot);
        size_bytes_[c] = bytes;
        reap_queue_.push_back({c});
    }
    // Post-submit staging: queue an already-captured slot for reap with error `e`.
    void stage_size_error(std::size_t slot, IoError e) {
        Completion<std::size_t>* c = captured_size_at(slot);
        size_err_[c] = e;
        reap_queue_.push_back({c});
    }

    // --- AsyncBackend interface ---
    // NOTE: this backend indexes captured read/write (size) Completions by
    // Batch slot index. A rejected submit pushes a nullptr placeholder so the
    // indices stay aligned with the Batch's add() order (the staging helpers
    // stage_size/stage_size_error take a Batch slot index). Sync ops are not
    // staged here; F2 tests use only read ops.
    Result<void> submit_read(ReadOp op, Completion<std::size_t>& c) override {
        auto it = reject_.find(op.fd);
        if (it != reject_.end()) {
            captures_.push_back({nullptr});  // keep slot-index alignment
            return make_unexpected<void>(it->second);
        }
        if (!try_claim(c))
            return make_unexpected<void>(IoError{IoError::Code::invalid_state});
        captures_.push_back({&c});
        ++outstanding_;
        apply_auto_stage(captures_.size() - 1, c);
        return {};
    }
    Result<void> submit_write(WriteOp op, Completion<std::size_t>& c) override {
        auto it = reject_.find(op.fd);
        if (it != reject_.end()) {
            captures_.push_back({nullptr});  // keep slot-index alignment
            return make_unexpected<void>(it->second);
        }
        if (!try_claim(c))
            return make_unexpected<void>(IoError{IoError::Code::invalid_state});
        captures_.push_back({&c});
        ++outstanding_;
        apply_auto_stage(captures_.size() - 1, c);
        return {};
    }
    Result<void> submit_sync_data(SyncDataOp, Completion<void>& c) override {
        if (!try_claim(c))
            return make_unexpected<void>(IoError{IoError::Code::invalid_state});
        ++outstanding_;
        return {};
    }
    Result<void> submit_sync_all(SyncAllOp, Completion<void>& c) override {
        if (!try_claim(c))
            return make_unexpected<void>(IoError{IoError::Code::invalid_state});
        ++outstanding_;
        return {};
    }

    std::size_t poll() override {
        std::size_t n = 0;
        while (!reap_queue_.empty()) {
            Completion<std::size_t>* c = reap_queue_.front().c;
            reap_queue_.pop_front();
            auto eit = size_err_.find(c);
            if (eit != size_err_.end()) {
                publish(*c, make_unexpected<std::size_t>(eit->second));
                size_err_.erase(eit);
            } else {
                auto b = size_bytes_.find(c);
                publish(*c, Result<std::size_t>{b->second});
                size_bytes_.erase(b);
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
        Completion<std::size_t>* c = nullptr;
    };
    struct Entry {
        Completion<std::size_t>* c = nullptr;
    };
    struct Plan {
        std::size_t bytes = 0;
        bool is_error = false;
        IoError err{IoError::Code::backend_error};
    };

    Completion<std::size_t>* captured_size_at(std::size_t slot) {
        return captures_.at(slot).c;
    }

    void apply_auto_stage(std::size_t slot, Completion<std::size_t>& c) {
        auto it = auto_plan_.find(slot);
        if (it == auto_plan_.end()) return;
        size_bytes_[&c] = it->second.bytes;
        reap_queue_.push_back({&c});
        auto_plan_.erase(it);
    }

    std::vector<Capture> captures_;
    std::map<int, IoError> reject_;
    std::map<std::size_t, Plan> auto_plan_;
    std::deque<Entry> reap_queue_;
    std::map<Completion<std::size_t>*, std::size_t> size_bytes_;
    std::map<Completion<std::size_t>*, IoError> size_err_;
    std::size_t outstanding_ = 0;
    std::optional<IoError> next_wait_err_;
};

static BatchOp make_read_op(int fd, std::byte* dst, std::size_t len,
                            std::uint64_t off) {
    BatchOp op;
    op.kind = BatchOp::Kind::read;
    op.read = ReadOp{fd, dst, len, off};
    return op;
}
}  // namespace

// ---- F2-1: synchronous rejection -> origin == rejected ----------------------
// submit fails before commit/accept (invalid descriptor / capacity / etc).
// The slot never becomes outstanding; next() reports origin == rejected and the
// Result carries the synchronous rejection error.
SLUICE_TEST_CASE(f2_origin_rejected_on_submit_failure) {
    auto owned = std::make_unique<OriginBackend>();
    OriginBackend* raw = owned.get();
    AsyncIoContext ctx(std::move(owned));
    raw->reject_fd(99, IoError{IoError::Code::permission_denied});

    Batch b;
    std::byte buf[4]{};
    b.add(make_read_op(99, buf, 4, 0));  // index 0 -> submit-rejected
    SLUICE_CHECK(b.await_one(ctx).value() >= 1);
    SLUICE_CHECK(ctx.outstanding() == 0);  // submit failed: never accepted

    auto r = b.next();
    SLUICE_CHECK(r.has_value());
    SLUICE_CHECK(r->index == 0);
    SLUICE_CHECK(r->origin == BatchResultOrigin::rejected);
    SLUICE_CHECK(!r->size_res.value().has_value());
    SLUICE_CHECK(r->size_res.value().error().code == IoError::Code::permission_denied);
    SLUICE_CHECK(b.next() == std::nullopt);
}

// ---- F2-2: accepted success -> origin == accepted_and_completed --------------
SLUICE_TEST_CASE(f2_origin_accepted_on_success) {
    auto owned = std::make_unique<OriginBackend>();
    OriginBackend* raw = owned.get();
    AsyncIoContext ctx(std::move(owned));
    raw->auto_stage_size(0, 4);

    Batch b;
    std::byte buf[4]{};
    b.add(make_read_op(0, buf, 4, 0));  // index 0 -> accepted + reaped 4
    SLUICE_CHECK(b.await_one(ctx).value() >= 1);

    auto r = b.next();
    SLUICE_CHECK(r.has_value());
    SLUICE_CHECK(r->index == 0);
    SLUICE_CHECK(r->origin == BatchResultOrigin::accepted_and_completed);
    SLUICE_CHECK(r->size_res.value().has_value());
    SLUICE_CHECK(r->size_res.value().value() == 4);
    SLUICE_CHECK(b.next() == std::nullopt);
}

// ---- F2-3: accepted terminal ERROR -> origin == accepted_and_completed --------
// THE KEY INVARIANT: an accepted request that terminates with an error is
// accepted_and_completed, NOT rejected. Origin is orthogonal to success/error;
// it records admission history, not the result's value/error tag.
SLUICE_TEST_CASE(f2_origin_accepted_on_terminal_error) {
    auto owned = std::make_unique<OriginBackend>();
    OriginBackend* raw = owned.get();
    AsyncIoContext ctx(std::move(owned));

    Batch b;
    std::byte buf[4]{};
    b.add(make_read_op(0, buf, 4, 0));  // index 0 -> accepted
    // Submit-and-capture without reaping, then stage an accepted-but-errored reap.
    raw->set_next_wait_one_error(IoError{IoError::Code::backend_error});
    (void)b.await_one(ctx);  // Phase 1 submits index 0; wait_one returns error
    raw->stage_size_error(0, IoError{IoError::Code::backend_error});
    SLUICE_CHECK(b.await_one(ctx).value() >= 1);

    auto r = b.next();
    SLUICE_CHECK(r.has_value());
    SLUICE_CHECK(r->index == 0);
    SLUICE_CHECK(r->origin == BatchResultOrigin::accepted_and_completed);
    SLUICE_CHECK(!r->size_res.value().has_value());
    SLUICE_CHECK(r->size_res.value().error().code == IoError::Code::backend_error);
    SLUICE_CHECK(b.next() == std::nullopt);
}

// ---- F2-4: accepted terminal result == canceled -> accepted_and_completed ----
// A cancel winner stores a `canceled` terminal and reaps it (reap_seq stamped,
// non-zero). The origin is therefore accepted_and_completed — cancellation is a
// terminal OUTCOME of an accepted request, not a submit rejection.
SLUICE_TEST_CASE(f2_origin_accepted_on_canceled_terminal) {
    auto owned = std::make_unique<OriginBackend>();
    OriginBackend* raw = owned.get();
    AsyncIoContext ctx(std::move(owned));

    Batch b;
    std::byte buf[4]{};
    b.add(make_read_op(0, buf, 4, 0));  // index 0 -> accepted
    raw->set_next_wait_one_error(IoError{IoError::Code::backend_error});
    (void)b.await_one(ctx);  // submits index 0
    raw->stage_size_error(0, IoError{IoError::Code::canceled});
    SLUICE_CHECK(b.await_one(ctx).value() >= 1);

    auto r = b.next();
    SLUICE_CHECK(r.has_value());
    SLUICE_CHECK(r->index == 0);
    SLUICE_CHECK(r->origin == BatchResultOrigin::accepted_and_completed);
    SLUICE_CHECK(!r->size_res.value().has_value());
    SLUICE_CHECK(r->size_res.value().error().code == IoError::Code::canceled);
    SLUICE_CHECK(b.next() == std::nullopt);
}

// ---- F2-5: mixed ordering — rejected first (submission order), then accepted --
// Slots: A=0 rejected, B=1 accepted, C=2 rejected, D=3 accepted. Accepted reap
// order forced: D (3) before B (1). Expected next() sequence: A, C, D, B —
// rejected entries first in submission order, then accepted in actual reap
// order. (Two await_one passes are needed because Batch skips its wait loop
// while rejected slots are already ready; the accepted reaps are staged after
// the rejected results are drained.)
SLUICE_TEST_CASE(f2_origin_mixed_ordering) {
    auto owned = std::make_unique<OriginBackend>();
    OriginBackend* raw = owned.get();
    AsyncIoContext ctx(std::move(owned));
    raw->reject_fd(0, IoError{IoError::Code::permission_denied});  // A (fd 0)
    raw->reject_fd(2, IoError{IoError::Code::permission_denied});  // C (fd 2)

    Batch b;
    std::byte b0[4]{}, b1[4]{}, b2[4]{}, b3[4]{};
    b.add(make_read_op(0, b0, 4, 0));   // A: index 0, rejected
    b.add(make_read_op(1, b1, 4, 4));   // B: index 1, accepted
    b.add(make_read_op(2, b2, 4, 8));   // C: index 2, rejected
    b.add(make_read_op(3, b3, 4, 12));  // D: index 3, accepted

    // await_one #1: Phase 1 submits all four. A and C are submit-rejected (ready
    // immediately); B and D are accepted+outstanding but not yet reaped.
    SLUICE_CHECK(b.await_one(ctx).value() == 2);  // A, C ready

    // Pop the two rejected results (submission order: A then C).
    auto a = b.next();
    auto c = b.next();
    SLUICE_CHECK(a.has_value());
    SLUICE_CHECK(a->index == 0);
    SLUICE_CHECK(a->origin == BatchResultOrigin::rejected);
    SLUICE_CHECK(c.has_value());
    SLUICE_CHECK(c->index == 2);
    SLUICE_CHECK(c->origin == BatchResultOrigin::rejected);

    // Stage accepted reaps in REVERSE submission order: D before B.
    raw->stage_size(3, 4);  // D reaped first
    raw->stage_size(1, 4);  // B reaped second

    // await_one #2: with A/C popped, the wait loop runs and reaps D then B.
    SLUICE_CHECK(b.await_one(ctx).value() == 2);

    auto d = b.next();
    auto bb = b.next();
    SLUICE_CHECK(d.has_value());
    SLUICE_CHECK(d->index == 3);
    SLUICE_CHECK(d->origin == BatchResultOrigin::accepted_and_completed);
    SLUICE_CHECK(d->size_res.value().value() == 4);
    SLUICE_CHECK(bb.has_value());
    SLUICE_CHECK(bb->index == 1);
    SLUICE_CHECK(bb->origin == BatchResultOrigin::accepted_and_completed);
    SLUICE_CHECK(bb->size_res.value().value() == 4);
    SLUICE_CHECK(b.next() == std::nullopt);
}

SLUICE_MAIN()
