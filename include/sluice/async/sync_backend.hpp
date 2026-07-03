// sluice::async default backend for job 017 (ADR §3/§4).
//
// SyncBackend completes ops SYNCHRONOUSLY at the next poll()/wait_one(). It
// holds no kernel state and uses no threads — it is the minimal in-process
// backend that lets the 017 foundation compile, link, and be tested before the
// real backends land (019 FakeAsyncBackend, 020A ThreadPool, 020B Uring).
//
// Semantics for 017: every submitted op is buffered; poll()/wait_one() marks all
// of them ready with a synthetic result. ReadOps complete with their full `len`
// (no actual read — 017 explicitly touches no fd); WriteOps complete with `len`;
// sync ops complete with void. This is enough to test the Completion lifecycle,
// submit/poll/wait plumbing, and AsyncStats. It is NOT a correctness backend
// for real I/O — that comes with 019/020A.
//
// State is instance-owned only (no globals, gate item 6).
#pragma once

#include <sluice/async/async_io_context.hpp>
#include <sluice/async/completion.hpp>
#include <sluice/error.hpp>
#include <sluice/result.hpp>

#include <algorithm>
#include <cstddef>
#include <utility>
#include <variant>
#include <vector>

namespace sluice::async {

class SyncBackend : public AsyncBackend {
public:
    SyncBackend() = default;
    ~SyncBackend() override {
        // No implicit cancel/drain; the context checks outstanding() on destroy.
    }

    Result<void> submit_read(ReadOp op, Completion<std::size_t>& c) override {
        if (!c.idle()) return make_unexpected<void>(IoError{IoError::Code::invalid_state});
        c.mark_outstanding();
        entries_.push_back(Entry{op, &c});
        return {};
    }
    Result<void> submit_write(WriteOp op, Completion<std::size_t>& c) override {
        if (!c.idle()) return make_unexpected<void>(IoError{IoError::Code::invalid_state});
        c.mark_outstanding();
        entries_.push_back(Entry{op, &c});
        return {};
    }
    Result<void> submit_sync_data(SyncDataOp op, Completion<void>& c) override {
        if (!c.idle()) return make_unexpected<void>(IoError{IoError::Code::invalid_state});
        c.mark_outstanding();
        ventries_.push_back(VEntry{op, &c});
        return {};
    }
    Result<void> submit_sync_all(SyncAllOp op, Completion<void>& c) override {
        if (!c.idle()) return make_unexpected<void>(IoError{IoError::Code::invalid_state});
        c.mark_outstanding();
        ventries_.push_back(VEntry{op, &c});
        return {};
    }

    std::size_t poll() override {
        std::size_t n = entries_.size() + ventries_.size();
        for (auto& e : entries_) {
            // 017 synthetic result: the op "transferred" its full length.
            e.completion->complete_with(Result<std::size_t>{e.requested_bytes()});
        }
        for (auto& e : ventries_) {
            e.completion->complete_with(Result<void>{});
        }
        entries_.clear();
        ventries_.clear();
        return n;
    }

    Result<std::size_t> wait_one() override {
        // No real waiting in 017 (no kernel/threads); just drain like poll().
        return poll();
    }

    void cancel(Completion<std::size_t>& c) override {
        // Minimal (ADR §7 X2): remove from pending if present, complete canceled.
        auto it = std::find_if(entries_.begin(), entries_.end(),
                               [&](const Entry& e) { return e.completion == &c; });
        if (it != entries_.end()) {
            c.complete_with(make_unexpected<std::size_t>(IoError{IoError::Code::canceled}));
            entries_.erase(it);
        }
    }
    void cancel(Completion<void>& c) override {
        auto it = std::find_if(ventries_.begin(), ventries_.end(),
                               [&](const VEntry& e) { return e.completion == &c; });
        if (it != ventries_.end()) {
            c.complete_with(make_unexpected<void>(IoError{IoError::Code::canceled}));
            ventries_.erase(it);
        }
    }

    std::size_t outstanding() const noexcept override {
        return entries_.size() + ventries_.size();
    }

private:
    struct Entry {
        std::variant<ReadOp, WriteOp> op;
        Completion<std::size_t>* completion;
        std::size_t requested_bytes() const {
            return std::visit([](auto&& o) { return o.len; }, op);
        }
    };
    struct VEntry {
        std::variant<SyncDataOp, SyncAllOp> op;
        Completion<void>* completion;
    };
    std::vector<Entry> entries_;
    std::vector<VEntry> ventries_;
};

}  // namespace sluice::async
