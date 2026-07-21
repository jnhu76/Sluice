// sluice::async::detail::SelectTimerRegistration — E13 Select Timer stable block.
//
// A Scheduler-owned, independently-stable control block that represents one
// active Select Timer arm. Mirrors TimerRegistration's retirement discipline
// but binds to Select objects (SelectArmSlot*) instead of WaitNode.
//
// The atomic state_ is the post-destruction safety boundary (I4): the pump
// reads state_ first; if not active, it skips without dereferencing the arm
// pointer. See docs/e13-select-timer-adapter.md.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

#include <sluice/async/scheduler.hpp>

namespace sluice::async::detail {

class SelectArmSlot;

// One Select Timer registration control block. Non-copyable, non-movable
// (its address is its identity for the deadline-heap ordering back-reference).
class SelectTimerRegistration {
public:
    enum class State : std::uint8_t {
        active = 0,
        retired = 1,
        consumed = 2,
    };

    SelectTimerRegistration() = default;
    SelectTimerRegistration(SelectArmSlot* arm, Scheduler* scheduler,
                            select_deadline_t deadline) noexcept
        : arm_(arm), scheduler_(scheduler), deadline_(deadline) {}

    SelectTimerRegistration(const SelectTimerRegistration&) = delete;
    SelectTimerRegistration& operator=(const SelectTimerRegistration&) = delete;
    SelectTimerRegistration(SelectTimerRegistration&&) = delete;
    SelectTimerRegistration& operator=(SelectTimerRegistration&&) = delete;

    State state() const noexcept {
        return state_.load(std::memory_order::acquire);
    }

    bool is_active() const noexcept {
        return state_.load(std::memory_order::acquire) == State::active;
    }

    bool is_retired() const noexcept {
        return state_.load(std::memory_order::acquire) == State::retired;
    }

    bool is_consumed() const noexcept {
        return state_.load(std::memory_order::acquire) == State::consumed;
    }

    // P1: single-object CAS with no external side effects.
    bool try_claim_expiry() noexcept {
        State expected = State::active;
        return state_.compare_exchange_strong(expected, State::consumed,
                                              std::memory_order::acq_rel,
                                              std::memory_order::acquire);
    }

    bool retire() noexcept {
        State expected = State::active;
        return state_.compare_exchange_strong(expected, State::retired,
                                              std::memory_order::acq_rel,
                                              std::memory_order::acquire);
    }

    select_deadline_t deadline() const noexcept { return deadline_; }
    SelectArmSlot* arm() const noexcept { return arm_; }
    Scheduler* scheduler() const noexcept { return scheduler_; }

    std::size_t heap_index = static_cast<std::size_t>(-1);

private:
    friend class Scheduler;

    std::atomic<State> state_{State::active};
    SelectArmSlot* arm_{nullptr};
    Scheduler* scheduler_{nullptr};
    select_deadline_t deadline_{0};
};

}  // namespace sluice::async::detail
