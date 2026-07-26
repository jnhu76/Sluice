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

Result<std::size_t> Batch::await_one(AsyncIoContext& ctx) {
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
    // E15-P2-01: capture a backend wait_one() error rather than discarding it.
    // The error propagates as the await_one() return value (see below). Slots
    // already made ready this call (submit-time errors above, or completions
    // reaped in earlier iterations of this loop) REMAIN ready and poppable via
    // next(); the caller may drain them before observing the error.
    std::optional<IoError> wait_err;
    while (!any_ready && ctx.outstanding() > 0) {
        auto wr = ctx.wait_one();
        if (!wr.has_value()) {
            wait_err = wr.error();  // capture instead of discarding
            break;
        }
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
    // E15-P2-01: a captured backend wait error takes precedence over the
    // ready-count return so callers cannot mistake a backend failure for "no
    // newly ready items". Ready slots remain poppable via next().
    if (wait_err.has_value()) {
        return make_unexpected<std::size_t>(*wait_err);
    }
    return ready_count;
}

std::optional<BatchResult> Batch::next() noexcept {
    // E15-P1-04: return the ready-but-not-popped slot with the SMALLEST reap
    // sequence (i.e. the one the backend reaped earliest), not the lowest
    // index. The reap sequence is stamped on each Completion by complete_with
    // at backend reap time, so this preserves true backend reap order across
    // slot 0/1/2... regardless of submission order. Each slot is popped
    // exactly once (the `popped` flag enforces it). A small linear scan is
    // fine: the batch path is small-N and next() is called once per result.
    std::size_t best = slots_.size();
    std::uint64_t best_seq = 0;
    for (std::size_t idx = 0; idx < slots_.size(); ++idx) {
        Slot& s = *slots_[idx];
        if (!s.ready || s.popped) continue;
        // Submit-time errors are surfaced with reap_seq 0 (the slot is marked
        // ready without ever going through complete_with). They sort BEFORE
        // any backend-reaped completion (first-available), matching the ADR E5
        // "submit-time errors are synchronous" model.
        const std::uint64_t seq = (s.is_void ? s.void_c.reap_seq() : s.size_c.reap_seq());
        if (best == slots_.size() || seq < best_seq ||
            // tie-break: stable by index for equal seq (e.g. submit-time
            // errors all at seq 0 surface in submission order)
            (seq == best_seq && idx < best)) {
            best = idx;
            best_seq = seq;
        }
    }
    if (best == slots_.size()) return std::nullopt;
    Slot& s = *slots_[best];
    s.popped = true;
    ++popped_;
    BatchResult r;
    r.index = best;
    r.is_void = s.is_void;
    if (s.is_void) {
        r.void_res = std::move(*s.void_res);
    } else {
        r.size_res = std::move(*s.size_res);
    }
    return r;
}

}  // namespace sluice::async
