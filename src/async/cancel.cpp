





#include <sluice/async/cancel.hpp>

#include <sluice/error.hpp>
#include <sluice/result.hpp>

namespace sluice::async {

namespace {



constexpr std::uint64_t kPendingBit = 1;
constexpr std::uint64_t kEpochInc = 2;
}

void CancelToken::request() noexcept {
    auto cur = state_.load(std::memory_order::relaxed);
    while (true) {
        if ((cur & kPendingBit) != 0) {
            return;
        }
        const auto next = (cur + kEpochInc) | kPendingBit;



        if (state_.compare_exchange_weak(cur, next, std::memory_order::release,
                                         std::memory_order::relaxed)) {
            return;
        }
    }
}

bool CancelToken::is_requested() const noexcept {
    return (state_.load(std::memory_order::acquire) & kPendingBit) != 0;
}

std::uint64_t CancelToken::epoch() const noexcept {
    return state_.load(std::memory_order::acquire) >> 1;
}

void CancelToken::rearm() noexcept {
    auto cur = state_.load(std::memory_order::relaxed);
    while (true) {
        if ((cur & kPendingBit) == 0) {
            return;
        }



        const auto next = (cur + kEpochInc) | kPendingBit;
        if (state_.compare_exchange_weak(cur, next, std::memory_order::release,
                                         std::memory_order::relaxed)) {
            return;
        }
    }
}

void CancelToken::clear() noexcept {


    state_.fetch_and(~kPendingBit, std::memory_order::release);
}

CancelProtection CancelState::swap_protection(CancelProtection next) noexcept {
    const CancelProtection prev = protection_;
    protection_ = next;
    return prev;
}

bool CancelState::acknowledged(const CancelToken& token) const noexcept {




    return token.is_requested() && acknowledged_epoch_ == token.epoch();
}

void CancelState::acknowledge(const CancelToken& token) noexcept {
    acknowledged_epoch_ = token.epoch();
}








Result<void> check_cancel(const CancelToken& token, CancelState& state) noexcept {
    if (state.protection() == CancelProtection::blocked) {
        return {};
    }



    const auto word = token.state_.load(std::memory_order::acquire);
    if ((word & kPendingBit) == 0) {
        return {};
    }
    const auto epoch = word >> 1;
    if (state.acknowledged_epoch_ == epoch) {
        return {};
    }


    state.acknowledged_epoch_ = epoch;
    return make_unexpected<void>(IoError{IoError::Code::canceled});
}

}
