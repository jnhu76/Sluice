#pragma once

#include <sluice/async/detail/request_core.hpp>

#include <atomic>
#include <cstdint>
#include <deque>
#include <map>
#include <utility>

namespace sluice_request_core_test {

using sluice::IoError;
using sluice::async::detail::AcceptAttempt;
using sluice::async::detail::BindingRelease;
using sluice::async::detail::BorrowFacts;
using sluice::async::detail::ControlRelease;
using sluice::async::detail::ExecutionClaim;
using sluice::async::detail::ExecutionRelease;
using sluice::async::detail::Generation;
using sluice::async::detail::IoOutcome;
using sluice::async::detail::ObserverRegistration;
using sluice::async::detail::ObserverRetirement;
using sluice::async::detail::PublicationCompletion;
using sluice::async::detail::PublicationGrant;
using sluice::async::detail::PublicationPayload;
using sluice::async::detail::PublicCancel;
using sluice::async::detail::PublicLookup;
using sluice::async::detail::RequestCore;
using sluice::async::detail::RequestDescriptor;
using sluice::async::detail::RequestKey;
using sluice::async::detail::RequestOp;
using sluice::async::detail::RequestReservation;
using sluice::async::detail::ReserveAttempt;
using sluice::async::detail::TerminalCandidate;
using sluice::async::detail::TerminalCandidateKind;
using sluice::async::detail::TerminalVerdict;

enum class PublishOrder : std::uint8_t { seal_before_ready, ready_before_seal };

class PublicationTarget {
  public:
    explicit PublicationTarget(PublishOrder order = PublishOrder::seal_before_ready) noexcept
        : order_(order) {}

    void store(const PublicationPayload& payload) {
        note_publisher_access_();
        payload_ = payload;
    }

    void publish_ready() noexcept {
        note_publisher_access_();
        if (order_ == PublishOrder::ready_before_seal) {
            ready_.store(true, std::memory_order::release);
            seal_();
            return;
        }
        seal_();
        ready_.store(true, std::memory_order::release);
    }

    void destroy() noexcept { destroyed_ = true; }

    bool acquire_ready() const noexcept { return ready_.load(std::memory_order::acquire); }

    const PublicationPayload& acquired_payload() const noexcept { return payload_; }

    bool publisher_touched_after_ready() const noexcept { return publisher_touched_after_ready_; }

    bool seal_followed_ready() const noexcept { return seal_followed_ready_; }

    bool used_after_destroy() const noexcept { return used_after_destroy_; }

  private:
    void note_publisher_access_() noexcept {
        if (destroyed_) {
            used_after_destroy_ = true;
        }
        if (sealed_.load(std::memory_order::relaxed)) {
            publisher_touched_after_ready_ = true;
        }
    }

    void seal_() noexcept {
        seal_followed_ready_ = seal_followed_ready_ || ready_.load(std::memory_order::relaxed);
        sealed_.store(true, std::memory_order::relaxed);
    }

    PublishOrder order_;
    PublicationPayload payload_{};
    std::atomic<bool> ready_{false};
    std::atomic<bool> sealed_{false};
    bool destroyed_ = false;
    bool publisher_touched_after_ready_ = false;
    bool seal_followed_ready_ = false;
    bool used_after_destroy_ = false;
};

class FakePhysicalDriver {
  public:
    explicit FakePhysicalDriver(RequestCore& core) : core_(core) {}

    ReserveAttempt reserve() { return core_.reserve(); }

    AcceptAttempt accept(RequestReservation reservation, std::uint64_t offset,
                         std::uint64_t length, bool zero_op = false,
                         RequestOp op = RequestOp::read) {
        RequestDescriptor descriptor;
        descriptor.op = op;
        descriptor.offset = offset;
        descriptor.length = length;
        descriptor.zero_op = zero_op;
        BorrowFacts borrow;
        borrow.fd = 3;
        borrow.buffer = reinterpret_cast<const void*>(0x1000);
        borrow.length = length;
        AcceptAttempt attempt = core_.accept(reservation, descriptor, borrow);
        if (attempt.ok()) {
            RequestState& state = state_of(attempt.id);
            state = RequestState{};
            state.accepted = true;
            state.exec = zero_op ? 0u : 1u;
            state.terminal = zero_op;
        }
        return attempt;
    }

    bool rollback(RequestReservation reservation) {
        const bool rolled = core_.rollback(reservation);
        if (rolled) {
            requests_.erase(identity_of(reservation));
        }
        return rolled;
    }

    void close_admission() { core_.close_admission(); }

    void note_health_failure() { core_.note_health_failure(); }

    PublicLookup lookup(RequestKey id) { return core_.lookup(id); }

    PublicCancel cancel(RequestKey id) {
        PublicCancel verdict = core_.cancel(id);
        if (verdict == PublicCancel::won_before_execution) {
            state_of(id).terminal = true;
        }
        return verdict;
    }

    BindingRelease release_public_binding(RequestKey id) {
        BindingRelease verdict = core_.release_public_binding(id);
        if (verdict == BindingRelease::released) {
            state_of(id).binding_released = true;
        }
        return verdict;
    }

    TerminalVerdict offer_physical_success(RequestKey id, std::uint64_t bytes) {
        TerminalCandidate candidate;
        candidate.kind = TerminalCandidateKind::physical_outcome;
        candidate.outcome = IoOutcome::success(bytes);
        return offer(id, candidate);
    }

    TerminalVerdict offer_physical_error(RequestKey id, IoError error,
                                         std::uint64_t confirmed_prefix = 0,
                                         bool unknown_effects = true) {
        TerminalCandidate candidate;
        candidate.kind = TerminalCandidateKind::physical_outcome;
        candidate.outcome = unknown_effects ? IoOutcome::uncertain(error, confirmed_prefix)
                                           : IoOutcome::failure(error, confirmed_prefix);
        return offer(id, candidate);
    }

    TerminalVerdict offer_zero_effect_cancel(RequestKey id) {
        TerminalCandidate candidate;
        candidate.kind = TerminalCandidateKind::zero_effect_cancel;
        candidate.outcome = sluice::detail::canceled_before_dispatch();
        return offer(id, candidate);
    }

    ExecutionClaim claim_execution(RequestKey id) { return core_.claim_execution(id); }

    bool acquire_execution(RequestKey id) {
        const bool acquired = core_.acquire_execution(id);
        if (acquired) {
            ++state_of(id).exec;
        }
        return acquired;
    }

    ExecutionRelease retire_execution(RequestKey id) {
        ExecutionRelease verdict = core_.release_execution(id);
        if (verdict == ExecutionRelease::released ||
            verdict == ExecutionRelease::borrow_touch_fully_retired) {
            RequestState& state = state_of(id);
            if (state.exec > 0) {
                --state.exec;
            }
        }
        return verdict;
    }

    bool acquire_control(RequestKey id) { return core_.acquire_control(id); }

    ControlRelease retire_control(RequestKey id) { return core_.release_control(id); }

    ObserverRegistration register_observer(RequestKey id) {
        return core_.register_observer(id);
    }

    ObserverRetirement retire_observer(RequestKey id) { return core_.retire_observer(id); }

    PublicationTarget& make_target() {
        targets_.emplace_back();
        return targets_.back();
    }

    PublicationGrant begin_publication(RequestKey id, PublicationPayload* out) {
        PublicationGrant grant = core_.begin_publication(id, out);
        if (grant == PublicationGrant::granted) {
            state_of(id).publication_inflight = true;
        }
        return grant;
    }

    PublicationCompletion finish_publication(RequestKey id) {
        PublicationCompletion verdict = core_.complete_publication(id);
        if (verdict == PublicationCompletion::completed) {
            RequestState& state = state_of(id);
            state.publication_inflight = false;
            state.published = true;
        }
        return verdict;
    }

    PublicationGrant publish_to(RequestKey id, PublicationTarget& target) {
        PublicationPayload payload;
        PublicationGrant grant = begin_publication(id, &payload);
        if (grant != PublicationGrant::granted) {
            return grant;
        }
        target.store(payload);
        target.publish_ready();
        return grant;
    }

    bool settle_all() {
        bool ok = true;
        for (auto& entry : requests_) {
            if (!settle(entry.first.first, entry.first.second)) {
                ok = false;
            }
        }
        return ok;
    }

    RequestCore& core() { return core_; }

  private:
    struct RequestState {
        bool accepted = false;
        bool terminal = false;
        bool published = false;
        bool publication_inflight = false;
        bool binding_released = false;
        std::uint32_t exec = 0;
    };

    using Identity = std::pair<std::uint32_t, std::uint64_t>;

    Identity identity_of(RequestReservation reservation) const {
        return {reservation.slot.value, reservation.generation.value};
    }

    Identity identity_of(RequestKey id) const { return {id.slot.value, id.generation.value}; }

    RequestState& state_of(RequestKey id) { return requests_[identity_of(id)]; }

    TerminalVerdict offer(RequestKey id, const TerminalCandidate& candidate) {
        TerminalVerdict verdict = core_.offer_terminal(id, candidate);
        if (verdict == TerminalVerdict::chosen) {
            state_of(id).terminal = true;
        }
        return verdict;
    }

    bool settle(std::uint32_t slot, std::uint64_t generation) {
        RequestKey id{core_.context(), sluice::async::detail::SlotIndex{slot},
                      sluice::async::detail::Generation{generation}};
        RequestState& state = state_of(id);
        if (!state.accepted) {
            return true;
        }
        if (!state.terminal) {
            PublicCancel verdict = core_.cancel(id);
            if (verdict == PublicCancel::won_before_execution) {
                state.terminal = true;
            } else if (verdict == PublicCancel::requested) {
                state.terminal =
                    offer(id, error_candidate()) == TerminalVerdict::chosen;
            } else {
                return false;
            }
        }
        while (state.exec > 0) {
            ExecutionRelease verdict = core_.release_execution(id);
            if (verdict == ExecutionRelease::released ||
                verdict == ExecutionRelease::borrow_touch_fully_retired) {
                --state.exec;
                continue;
            }
            return false;
        }
        if (state.publication_inflight) {
            if (core_.complete_publication(id) != PublicationCompletion::completed) {
                return false;
            }
            state.publication_inflight = false;
            state.published = true;
        }
        if (state.binding_released) {
            return true;
        }
        if (!state.published) {
            PublicationTarget& target = make_target();
            if (publish_to(id, target) != PublicationGrant::granted) {
                return false;
            }
            if (core_.complete_publication(id) != PublicationCompletion::completed) {
                return false;
            }
            state.published = true;
        }
        BindingRelease verdict = core_.release_public_binding(id);
        if (verdict != BindingRelease::released) {
            return false;
        }
        state.binding_released = true;
        return true;
    }

    static TerminalCandidate error_candidate() {
        TerminalCandidate candidate;
        candidate.kind = TerminalCandidateKind::physical_outcome;
        candidate.outcome = IoOutcome::uncertain(IoError{.code = IoError::Code::backend_error});
        return candidate;
    }

    RequestCore& core_;
    std::deque<PublicationTarget> targets_;
    std::map<Identity, RequestState> requests_;
};

}  // namespace sluice_request_core_test
