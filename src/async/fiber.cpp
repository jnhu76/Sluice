#include <sluice/async/fiber.hpp>

namespace sluice::async {

bool Fiber::make_runnable() noexcept {
    FiberState expected = FiberState::created;

    if (state_.compare_exchange_strong(expected, FiberState::runnable,
                                       std::memory_order::acq_rel)) {
        return true;
    }
    expected = FiberState::waiting;

    return state_.compare_exchange_strong(expected, FiberState::runnable,
                                          std::memory_order::acq_rel);
}

bool Fiber::make_running() noexcept {
    FiberState expected = FiberState::runnable;
    return state_.compare_exchange_strong(expected, FiberState::running,
                                          std::memory_order::acq_rel);
}

bool Fiber::make_waiting() noexcept {
    FiberState expected = FiberState::running;
    return state_.compare_exchange_strong(expected, FiberState::waiting,
                                          std::memory_order::acq_rel);
}

void Fiber::make_done() noexcept {
    state_.store(FiberState::done, std::memory_order::release);
}

} // namespace sluice::async
