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

    // --- auto-complete mode ---
    // When set, poll() auto-completes each outstanding op with `auto_bytes_`
    // (read/write) or void-success (sync), WITHOUT the test staging anything.
    // This lets the synchronous read_all/write_all coordinators (job 018) drive
    // the fake in a poll-loop, since they submit+poll internally and cannot have
    // the test stage results between their loop steps.
    //   auto_bytes(n)         each outstanding op completes with n bytes
    //                         (n may be < requested => short, exercises retry)
    //   auto_short_then_full(first, rest)
    //                         the FIRST outstanding op completes short (first),
    //                         subsequent ones complete their full remaining
    //                         length; models one short then clean completion.
    //   auto_error(e)         each outstanding op completes with error e
    //   auto_eof()            read completes with 0 bytes (EOF) — shortcut for
    //                         auto_bytes(0).
    //   auto_disable()        stop auto-completing (resume explicit staging).
    void auto_bytes(std::size_t n) { auto_mode_ = Auto::bytes; auto_bytes_ = n; }
    void auto_error(IoError e) { auto_mode_ = Auto::err; auto_err_ = e; }
    void auto_eof() { auto_bytes(0); }
    void auto_disable() { auto_mode_ = Auto::off; }
    void auto_short_then_full(std::size_t first_short) {
        auto_mode_ = Auto::short_then_full;
        auto_bytes_ = first_short;
        auto_short_used_ = false;
    }

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
    // (n < requested => short completion). No-op if none outstanding. The result
    // is consumed at the next poll() when the matching Completion is reaped.
    void complete_oldest_with_bytes(std::size_t n) {
        if (ready_size_.empty()) return;
        staged_size_.push_back(n);
    }
    // Stage an error result for the oldest outstanding read/write op.
    void complete_oldest_with_error(IoError e) {
        if (ready_size_.empty()) return;
        staged_size_err_.push_back(e);
    }
    // Stage a void success for the oldest outstanding sync op.
    void complete_oldest_sync_ok() {
        if (ready_void_.empty()) return;
        staged_void_ok_.push_back(true);
    }
    // Stage a void error for the oldest outstanding sync op.
    void complete_oldest_sync_error(IoError e) {
        if (ready_void_.empty()) return;
        staged_void_err_.push_back(e);
    }

    // --- reap: apply staged results to Completions (mark ready). ---
    std::size_t poll() override {
        std::size_t n = 0;
        // Auto-complete mode: every outstanding op gets a result this poll.
        if (auto_mode_ != Auto::off) {
            while (!ready_size_.empty()) {
                auto* c = ready_size_.front();
                ready_size_.pop_front();
                std::size_t requested = pending_size_.front();
                pending_size_.pop_front();
                c->complete_with(auto_size_result(requested));
                ++n;
            }
            while (!ready_void_.empty()) {
                auto* c = ready_void_.front();
                ready_void_.pop_front();
                pending_void_.pop_front();
                if (auto_mode_ == Auto::err) {
                    c->complete_with(make_unexpected<void>(auto_err_));
                } else {
                    c->complete_with(Result<void>{});
                }
                ++n;
            }
            return n;
        }
        // Explicit-staging mode (default): consume one stage per completion that
        // has a staged result.
        while (!ready_size_.empty() && has_size_stage()) {
            auto* c = ready_size_.front();
            ready_size_.pop_front();
            pending_size_.pop_front();
            c->complete_with(take_size_stage());
            ++n;
        }
        while (!ready_void_.empty() && has_void_stage()) {
            auto* c = ready_void_.front();
            ready_void_.pop_front();
            pending_void_.pop_front();
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
    // Build the auto-completion result for a read/write op given its requested
    // length (auto mode). short_then_full: first op short, then full remaining.
    Result<std::size_t> auto_size_result(std::size_t requested) {
        switch (auto_mode_) {
            case Auto::bytes:
                return Result<std::size_t>{auto_bytes_};
            case Auto::err:
                return make_unexpected<std::size_t>(auto_err_);
            case Auto::short_then_full:
                if (!auto_short_used_) { auto_short_used_ = true; return Result<std::size_t>{auto_bytes_}; }
                return Result<std::size_t>{requested};
            default:
                return Result<std::size_t>{requested};
        }
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

    // Auto-complete mode state.
    enum class Auto : std::uint8_t { off, bytes, err, short_then_full };
    Auto auto_mode_ = Auto::off;
    std::size_t auto_bytes_ = 0;
    IoError auto_err_{IoError::Code::backend_error};
    bool auto_short_used_ = false;
};

}  // namespace sluice::async
