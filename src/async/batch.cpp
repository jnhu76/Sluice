
#include <sluice/async/batch.hpp>

#include <utility>

namespace sluice::async {

std::size_t Batch::add(BatchOp op) {



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




            if (s.is_void) {
                s.void_res = sr;
            } else {
                s.size_res = sluice::make_unexpected<std::size_t>(sr.error());
            }
            s.submit_rejected = true;
            s.ready = true;
        }
    }





    bool any_ready = false;
    for (const auto& sp : slots_) {
        if (sp->ready && !sp->popped) { any_ready = true; break; }
    }





    std::optional<IoError> wait_err;
    while (!any_ready && ctx.outstanding() > 0) {
        auto wr = ctx.wait_one();
        if (!wr.has_value()) {
            wait_err = wr.error();
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



    if (wait_err.has_value()) {
        return make_unexpected<std::size_t>(*wait_err);
    }
    return ready_count;
}

std::optional<BatchResult> Batch::next() noexcept {







    std::size_t best = slots_.size();
    std::uint64_t best_seq = 0;
    for (std::size_t idx = 0; idx < slots_.size(); ++idx) {
        Slot& s = *slots_[idx];
        if (!s.ready || s.popped) continue;




        const std::uint64_t seq = (s.is_void ? s.void_c.reap_seq() : s.size_c.reap_seq());
        if (best == slots_.size() || seq < best_seq ||


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


    r.origin = s.submit_rejected ? BatchResultOrigin::rejected
                                 : BatchResultOrigin::accepted_and_completed;
    r.is_void = s.is_void;
    if (s.is_void) {
        r.void_res = std::move(*s.void_res);
    } else {
        r.size_res = std::move(*s.size_res);
    }
    return r;
}

}
