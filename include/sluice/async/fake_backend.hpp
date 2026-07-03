// sluice::async::FakeAsyncBackend (sluice-CORE-019, ADR §4/§10 T1).
//
// A deterministic async backend for tests: ops submitted are held outstanding
// across poll() calls UNTIL THE TEST EXPLICITLY COMPLETES THEM. No kernel, no
// threads. This is the primary unit-test vehicle for all later async work
// (018/018B/021) and the thing that makes the buffer-lifetime contract (gate
// item 1) genuinely testable.
//
// Completion model:
//   - submit_* records the op (no completion produced).
//   - The test calls one of the complete_*() helpers to stage a terminal result
//     for the OLDEST outstanding op of a given kind (FIFO by default, ADR O3 for
//     the fake). Arbitrary order is available via complete_op_at().
//   - poll()/wait_one() then move staged results into the Completions (marking
//     them ready). This keeps the "completions only inside poll/wait_one" rule
//     (ADR A3/O1) even on the fake.
//
// Error / short-completion injection:
//   - complete_oldest_with_error(IoError) — surface any error (eof/no_space/
//     backend_error/canceled) on the next poll (ADR E2/E3).
//   - complete_oldest_with_bytes(n) — surface a (possibly short) byte count for
//     a read/write op; n < requested is a short completion (exercises 018 retry).
//
// State is instance-owned only (no globals, gate item 6).
#pragma once

#include <sluice/async/async_io_context.hpp>
#include <sluice/async/completion.hpp>
#include <sluice/error.hpp>
#include <sluice/result.hpp>

#include <algorithm>
#include <cstddef>
#include <deque>
#include <utility>
#include <variant>
#include <vector>

namespace sluice::async {

class FakeAsyncBackend : public AsyncBackend {
public:
    FakeAsyncBackend() = default;
    ~FakeAsyncBackend() override = default;

    // --- submit: record outstanding, produce no completion ---
    Result<void> submit_read(ReadOp op, Completion<std::size_t>& c) override {
        return record_size(op, c);
    }
    Result<void> submit_write(WriteOp op, Completion<std::size_t>& c) override {
        return record_size(op, c);
    }
    Result<void> submit_sync_data(SyncDataOp op, Completion<void>& c) override {
        return record_void(op, c);
    }
    Result<void> submit_sync_all(SyncAllOp op, Completion<void>& c) override {
        return record_void(op, c);
    }

    // --- test-driving helpers: stage terminal results for the OLDEST
    // outstanding op of the matching kind. Applied at the next poll(). ---

    // Stage a byte-count result for the oldest outstanding read/write op
    // (n < requested => short completion). No-op if none outstanding.
    void complete_oldest_with_bytes(std::size_t n) {
        if (pending_size_.empty()) return;
        staged_size_.push_back(n);
        // Pop the matching pending entry so it's no longer "outstanding" from
        // the test's perspective once the stage is consumed at poll.
        pending_size_.pop_front();
    }
    // Stage an error result for the oldest outstanding read/write op.
    void complete_oldest_with_error(IoError e) {
        if (pending_size_.empty()) return;
        staged_size_err_.push_back(e);
        pending_size_.pop_front();
    }
    // Stage a void success for the oldest outstanding sync op.
    void complete_oldest_sync_ok() {
        if (pending_void_.empty()) return;
        staged_void_ok_.push_back(true);
        pending_void_.pop_front();
    }
    // Stage a void error for the oldest outstanding sync op.
    void complete_oldest_sync_error(IoError e) {
        if (pending_void_.empty()) return;
        staged_void_err_.push_back(e);
        pending_void_.pop_front();
    }

    // --- reap: apply staged results to Completions (mark ready). ---
    std::size_t poll() override {
        std::size_t n = 0;
        // FIFO: consume one stage per completion that has a staged result.
        while (!ready_size_.empty() && has_size_stage()) {
            auto* c = ready_size_.front();
            ready_size_.pop_front();
            c->complete_with(take_size_stage());
            ++n;
        }
        while (!ready_void_.empty() && has_void_stage()) {
            auto* c = ready_void_.front();
            ready_void_.pop_front();
            c->complete_with(take_void_stage());
            ++n;
        }
        return n;
    }
    Result<std::size_t> wait_one() override {
        // No real waiting (no kernel/threads); just poll. Tests drive timing.
        return poll();
    }

    // Minimal cancel (ADR §7 X2): the op is removed and staged as canceled;
    // applied at next poll.
    void cancel(Completion<std::size_t>& c) override {
        auto it = std::find(ready_size_.begin(), ready_size_.end(), &c);
        if (it != ready_size_.end()) {
            staged_size_err_.push_back(IoError{IoError::Code::canceled});
            ready_size_.erase(it);
        }
    }
    void cancel(Completion<void>& c) override {
        auto it = std::find(ready_void_.begin(), ready_void_.end(), &c);
        if (it != ready_void_.end()) {
            staged_void_err_.push_back(IoError{IoError::Code::canceled});
            ready_void_.erase(it);
        }
    }

    std::size_t outstanding() const noexcept override {
        return ready_size_.size() + ready_void_.size();
    }

private:
    Result<void> record_size(auto op, Completion<std::size_t>& c) {
        if (!c.idle()) return make_unexpected<void>(IoError{IoError::Code::invalid_state});
        c.mark_outstanding();
        ready_size_.push_back(&c);
        pending_size_.push_back(op.len);
        return {};
    }
    Result<void> record_void(auto /*op*/, Completion<void>& c) {
        if (!c.idle()) return make_unexpected<void>(IoError{IoError::Code::invalid_state});
        c.mark_outstanding();
        ready_void_.push_back(&c);
        pending_void_.push_back(0);
        return {};
    }

    bool has_size_stage() const {
        return !staged_size_.empty() || !staged_size_err_.empty();
    }
    bool has_void_stage() const {
        return !staged_void_ok_.empty() || !staged_void_err_.empty();
    }
    Result<std::size_t> take_size_stage() {
        if (!staged_size_err_.empty()) {
            IoError e = staged_size_err_.front();
            staged_size_err_.pop_front();
            return make_unexpected<std::size_t>(e);
        }
        std::size_t n = staged_size_.front();
        staged_size_.pop_front();
        return Result<std::size_t>{n};
    }
    Result<void> take_void_stage() {
        if (!staged_void_err_.empty()) {
            IoError e = staged_void_err_.front();
            staged_void_err_.pop_front();
            return make_unexpected<void>(e);
        }
        staged_void_ok_.pop_front();
        return {};
    }

    // Completions awaiting a staged result (in submit order — FIFO, ADR O3).
    std::deque<Completion<std::size_t>*> ready_size_;
    std::deque<Completion<void>*> ready_void_;
    // Requested byte counts (for the test to compare against when injecting
    // short completions). Populated on submit; consumed by the test when staging.
    std::deque<std::size_t> pending_size_;
    std::deque<std::size_t> pending_void_;
    // Staged terminal results the test queued; consumed at poll().
    std::deque<std::size_t> staged_size_;
    std::deque<IoError> staged_size_err_;
    std::deque<bool> staged_void_ok_;
    std::deque<IoError> staged_void_err_;
};

}  // namespace sluice::async
