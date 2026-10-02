#include <sluice/async/request_scope.hpp>

#include <sluice/async/detail/fail_fast.hpp>

namespace sluice::async {

std::size_t RequestScope::require_capacity_(std::size_t capacity) {
    if (capacity == 0) {
        throw std::invalid_argument(
            "sluice::async::RequestScope: capacity must be nonzero");
    }
    return capacity;
}

ProgressOwner RequestScope::claim_owner_(AsyncIoContext& ctx) {
    if (!ctx.has_split_wait_capability()) {
        throw std::runtime_error(
            "sluice::async::RequestScope: the context cannot drive the owned wait protocol");
    }
    auto claimed = ctx.claim_progress_owner();
    if (!claimed.has_value()) {
        throw std::runtime_error(
            "sluice::async::RequestScope: the context progress owner is unavailable");
    }
    return std::move(claimed).value();
}

RequestScope::RequestScope(AsyncIoContext& ctx, std::size_t capacity,
                           ScopeCleanupPolicy policy)
    : ctx_(ctx),
      capacity_(require_capacity_(capacity)),
      policy_(policy),
      owner_(claim_owner_(ctx)),
      slots_(std::make_unique<Slot[]>(capacity_)) {}

RequestScope::~RequestScope() {
#if defined(SLUICE_D1_MUTANT_DESTRUCTOR_SKIPS_SETTLE)
    finished_ = true;
#else
    if (!finished_) {
        settle_and_release_();
    }
#endif
}

bool RequestScope::reserve_slot_(std::size_t& index) noexcept {
    for (std::size_t i = 0; i < capacity_; ++i) {
        if (slots_[i].state == Slot::State::free) {
            slots_[i].state = Slot::State::reserved;
            index = i;
            return true;
        }
    }
    return false;
}

void RequestScope::release_slot_(Slot& slot) noexcept {
    slot.request.template emplace<0>();
    slot.state = Slot::State::free;
}

bool RequestScope::request_ready_(const Slot& slot) const noexcept {
    if (const auto* request = std::get_if<Request<std::size_t>>(&slot.request)) {
        return request->ready();
    }
    if (const auto* request = std::get_if<Request<void>>(&slot.request)) {
        return request->ready();
    }
    return false;
}

void RequestScope::request_cancel_(Slot& slot) noexcept {
    if (auto* request = std::get_if<Request<std::size_t>>(&slot.request)) {
        (void)request->cancel();
        return;
    }
    if (auto* request = std::get_if<Request<void>>(&slot.request)) {
        (void)request->cancel();
    }
}

void RequestScope::request_discard_(Slot& slot) noexcept {
    if (auto* request = std::get_if<Request<std::size_t>>(&slot.request)) {
        request->discard();
        return;
    }
    if (auto* request = std::get_if<Request<void>>(&slot.request)) {
        request->discard();
    }
}

void RequestScope::settle_and_release_() noexcept {
    if (policy_ == ScopeCleanupPolicy::cancel_then_drain) {
        for (std::size_t i = 0; i < capacity_; ++i) {
            Slot& slot = slots_[i];
            if (slot.state == Slot::State::owning && !request_ready_(slot)) {
                request_cancel_(slot);
            }
        }
    }
    for (;;) {
        bool settled = true;
        for (std::size_t i = 0; i < capacity_; ++i) {
            const Slot& slot = slots_[i];
            if (slot.state == Slot::State::owning && !request_ready_(slot)) {
                settled = false;
                break;
            }
        }
        if (settled) {
            break;
        }
        auto pass = ctx_.poll_progress();
        if (!pass.has_value()) {
            detail::request_scope_settlement_driver_fail_fast();
        }
        settled = true;
        for (std::size_t i = 0; i < capacity_; ++i) {
            const Slot& slot = slots_[i];
            if (slot.state == Slot::State::owning && !request_ready_(slot)) {
                settled = false;
                break;
            }
        }
        if (settled) {
            break;
        }
        auto woke = ctx_.wait_one();
        if (!woke.has_value()) {
            detail::request_scope_settlement_driver_fail_fast();
        }
        switch (woke.value().kind) {
        case AsyncIoContext::ProgressWaitOutcome::Kind::progress:
            break;
        case AsyncIoContext::ProgressWaitOutcome::Kind::control_interrupted:
            ctx_.acknowledge_progress_control();
            break;
        case AsyncIoContext::ProgressWaitOutcome::Kind::deadline_expired:
            detail::request_scope_settlement_driver_fail_fast();
        case AsyncIoContext::ProgressWaitOutcome::Kind::health_failure:
            detail::request_scope_settlement_health_fail_fast();
        }
    }
    for (std::size_t i = 0; i < capacity_; ++i) {
        Slot& slot = slots_[i];
        if (slot.state == Slot::State::owning && request_ready_(slot)) {
            request_discard_(slot);
            release_slot_(slot);
        }
    }
}

Result<ScopeTicket<std::size_t>> RequestScope::submit_read(ReadOp op) {
    if (finished_) {
        return make_unexpected<ScopeTicket<std::size_t>>(
            IoError{IoError::Code::invalid_state});
    }
    std::size_t index = 0;
    if (!reserve_slot_(index)) {
        return make_unexpected<ScopeTicket<std::size_t>>(
            IoError{IoError::Code::would_block});
    }
#if defined(SLUICE_D1_MUTANT_RESERVE_AFTER_ACCEPT)
    auto accepted = ctx_.submit_read(op);
    Reservation guard{this, index};
#else
    Reservation guard{this, index};
    auto accepted = ctx_.submit_read(op);
#endif
    if (!accepted.has_value()) {
        return make_unexpected<ScopeTicket<std::size_t>>(accepted.error());
    }
    const RequestId id = accepted.value().id();
    commit_<std::size_t>(index, std::move(accepted).value());
    guard.committed = true;
    return ScopeTicket<std::size_t>{id};
}

Result<ScopeTicket<std::size_t>> RequestScope::submit_write(WriteOp op) {
    if (finished_) {
        return make_unexpected<ScopeTicket<std::size_t>>(
            IoError{IoError::Code::invalid_state});
    }
    std::size_t index = 0;
    if (!reserve_slot_(index)) {
        return make_unexpected<ScopeTicket<std::size_t>>(
            IoError{IoError::Code::would_block});
    }
#if defined(SLUICE_D1_MUTANT_RESERVE_AFTER_ACCEPT)
    auto accepted = ctx_.submit_write(op);
    Reservation guard{this, index};
#else
    Reservation guard{this, index};
    auto accepted = ctx_.submit_write(op);
#endif
    if (!accepted.has_value()) {
        return make_unexpected<ScopeTicket<std::size_t>>(accepted.error());
    }
    const RequestId id = accepted.value().id();
    commit_<std::size_t>(index, std::move(accepted).value());
    guard.committed = true;
    return ScopeTicket<std::size_t>{id};
}

Result<ScopeTicket<void>> RequestScope::submit_sync_data(SyncDataOp op) {
    if (finished_) {
        return make_unexpected<ScopeTicket<void>>(IoError{IoError::Code::invalid_state});
    }
    std::size_t index = 0;
    if (!reserve_slot_(index)) {
        return make_unexpected<ScopeTicket<void>>(IoError{IoError::Code::would_block});
    }
#if defined(SLUICE_D1_MUTANT_RESERVE_AFTER_ACCEPT)
    auto accepted = ctx_.submit_sync_data(op);
    Reservation guard{this, index};
#else
    Reservation guard{this, index};
    auto accepted = ctx_.submit_sync_data(op);
#endif
    if (!accepted.has_value()) {
        return make_unexpected<ScopeTicket<void>>(accepted.error());
    }
    const RequestId id = accepted.value().id();
    commit_<void>(index, std::move(accepted).value());
    guard.committed = true;
    return ScopeTicket<void>{id};
}

Result<ScopeTicket<void>> RequestScope::submit_sync_all(SyncAllOp op) {
    if (finished_) {
        return make_unexpected<ScopeTicket<void>>(IoError{IoError::Code::invalid_state});
    }
    std::size_t index = 0;
    if (!reserve_slot_(index)) {
        return make_unexpected<ScopeTicket<void>>(IoError{IoError::Code::would_block});
    }
#if defined(SLUICE_D1_MUTANT_RESERVE_AFTER_ACCEPT)
    auto accepted = ctx_.submit_sync_all(op);
    Reservation guard{this, index};
#else
    Reservation guard{this, index};
    auto accepted = ctx_.submit_sync_all(op);
#endif
    if (!accepted.has_value()) {
        return make_unexpected<ScopeTicket<void>>(accepted.error());
    }
    const RequestId id = accepted.value().id();
    commit_<void>(index, std::move(accepted).value());
    guard.committed = true;
    return ScopeTicket<void>{id};
}

RequestObservation<std::size_t> RequestScope::take(
    const ScopeTicket<std::size_t>& ticket) noexcept {
    return take_<std::size_t>(ticket);
}

RequestObservation<void> RequestScope::take(const ScopeTicket<void>& ticket) noexcept {
    return take_<void>(ticket);
}

Result<ScopeWaitStatus> RequestScope::wait_for(const ScopeTicket<std::size_t>& ticket,
                                               std::chrono::nanoseconds max_wait) noexcept {
    Slot* slot = resolve_<std::size_t>(ticket);
    if (slot == nullptr) {
        return make_unexpected<ScopeWaitStatus>(IoError{IoError::Code::not_found});
    }
    return wait_for_slot_<std::size_t>(*slot, max_wait);
}

Result<ScopeWaitStatus> RequestScope::wait_for(const ScopeTicket<void>& ticket,
                                               std::chrono::nanoseconds max_wait) noexcept {
    Slot* slot = resolve_<void>(ticket);
    if (slot == nullptr) {
        return make_unexpected<ScopeWaitStatus>(IoError{IoError::Code::not_found});
    }
    return wait_for_slot_<void>(*slot, max_wait);
}

void RequestScope::finish() noexcept {
    if (finished_) {
        return;
    }
    settle_and_release_();
    finished_ = true;
}

}
