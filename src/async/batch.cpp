// Implementation of Batch (sluice-CORE-030, T4). See batch.hpp for the model.
#include <sluice/async/batch.hpp>

#include <utility>

namespace sluice::async {

std::size_t Batch::add(BatchOp op) {
    // Slot contains non-copyable, non-movable Completion<T>, so we store
    // unique_ptr<Slot> — the vector relocates pointers on grow, not the
    // address-stable Completions (ADR §5 L7).
    const bool is_void = (op.kind == BatchOp::Kind::sync_data ||
                          op.kind == BatchOp::Kind::sync_all);
    const std::size_t index = slots_.size();
    slots_.push_back(std::make_unique<Slot>());
    Slot& s = *slots_.back();
    s.op = std::move(op);
    s.is_void = is_void;
    return index;
}

std::size_t Batch::await_one(AsyncIoContext& ctx) {
    // Phase 1: submit every not-yet-submitted op to ctx. A submit may fail
    // (queue full / invalid); on failure the slot is marked ready with the
    // error so next() surfaces it (mirrors ADR E5: submit-time errors are
    // synchronous, but a batch surfaces them through the completion channel
    // for uniform iteration).
    for (auto& sp : slots_) {
        Slot& s = *sp;
        if (s.submitted) continue;
        s.submitted = true;
        Result<void> sr{};
        switch (s.op.kind) {
            case BatchOp::Kind::read:
                sr = ctx.submit_read(s.op.read, s.size_c);
                break;
            case BatchOp::Kind::write:
                sr = ctx.submit_write(s.op.write, s.size_c);
                break;
            case BatchOp::Kind::sync_data:
                sr = ctx.submit_sync_data(s.op.sync_data, s.void_c);
                break;
            case BatchOp::Kind::sync_all:
                sr = ctx.submit_sync_all(s.op.sync_all, s.void_c);
                break;
        }
        if (!sr.has_value()) {
            // Submit-time error: surface as a ready completion carrying it.
            if (s.is_void) {
                s.void_res = sr;
            } else {
                s.size_res = sluice::make_unexpected<std::size_t>(sr.error());
            }
            s.ready = true;
        }
    }

    // Phase 2: if no UNPOPPED slot is ready yet AND there is outstanding work,
    // drive ctx.wait_one() until >=1 ready. Counting only not-popped slots
    // matters: a popped-but-not-cleared slot (ready still true) must not fool
    // us into skipping the wait while another op is still in flight.
    bool any_ready = false;
    for (const auto& sp : slots_) {
        if (sp->ready && !sp->popped) { any_ready = true; break; }
    }
    while (!any_ready && ctx.outstanding() > 0) {
        auto wr = ctx.wait_one();
        if (!wr.has_value()) break;  // backend error; stop, surface nothing new
        for (auto& sp : slots_) {
            Slot& s = *sp;
            if (!s.ready) {
                if (!s.is_void && s.size_c.ready()) {
                    s.size_res = s.size_c.result();
                    s.ready = true;
                    any_ready = true;
                } else if (s.is_void && s.void_c.ready()) {
                    s.void_res = s.void_c.result();
                    s.ready = true;
                    any_ready = true;
                }
            }
        }
    }
    // Harvest any other ops that became ready alongside the one wait_one() saw.
    for (auto& sp : slots_) {
        Slot& s = *sp;
        if (!s.ready) {
            if (!s.is_void && s.size_c.ready()) {
                s.size_res = s.size_c.result();
                s.ready = true;
            } else if (s.is_void && s.void_c.ready()) {
                s.void_res = s.void_c.result();
                s.ready = true;
            }
        }
    }

    std::size_t ready_count = 0;
    for (const auto& sp : slots_) {
        if (sp->ready && !sp->popped) ++ready_count;
    }
    return ready_count;
}

std::optional<BatchResult> Batch::next() noexcept {
    // Scan all slots for a ready-but-not-popped one. Completions arrive in
    // arbitrary order under concurrency, so a monotonic cursor would miss
    // late-ready earlier slots. Small N (batch path); the full scan is fine.
    // Each slot is popped exactly once (the `popped` flag enforces it).
    for (std::size_t idx = 0; idx < slots_.size(); ++idx) {
        Slot& s = *slots_[idx];
        if (s.ready && !s.popped) {
            s.popped = true;
            ++popped_;
            BatchResult r;
            r.index = idx;
            r.is_void = s.is_void;
            if (s.is_void) {
                r.void_res = std::move(*s.void_res);
            } else {
                r.size_res = std::move(*s.size_res);
            }
            return r;
        }
    }
    return std::nullopt;
}

}  // namespace sluice::async
