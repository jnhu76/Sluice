#pragma once

#include <sluice/async/timer_registration.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace sluice::async {

class Scheduler;

namespace detail {

struct SelectArmSlot;

class SelectTimerRegistration {
  public:
    enum class State : std::uint8_t {
        active = 0,
        retired = 1,
        consumed = 2,
    };

    SelectTimerRegistration() = default;
    SelectTimerRegistration(SelectArmSlot* arm, Scheduler* scheduler,
                            deadline_tick_t deadline) noexcept
        : arm_(arm), scheduler_(scheduler), deadline_(deadline) {}

    SelectTimerRegistration(const SelectTimerRegistration&) = delete;
    SelectTimerRegistration& operator=(const SelectTimerRegistration&) = delete;
    SelectTimerRegistration(SelectTimerRegistration&&) = delete;
    SelectTimerRegistration& operator=(SelectTimerRegistration&&) = delete;

    State state() const noexcept { return state_.load(std::memory_order::acquire); }

    bool is_active() const noexcept {
        return state_.load(std::memory_order::acquire) == State::active;
    }

    bool is_retired() const noexcept {
        return state_.load(std::memory_order::acquire) == State::retired;
    }

    bool is_consumed() const noexcept {
        return state_.load(std::memory_order::acquire) == State::consumed;
    }

    deadline_tick_t deadline() const noexcept { return deadline_; }
    SelectArmSlot* arm() const noexcept { return arm_; }
    Scheduler* scheduler() const noexcept { return scheduler_; }

  private:
    friend class ::sluice::async::Scheduler;

    bool try_claim_expiry() noexcept {
        State expected = State::active;
        return state_.compare_exchange_strong(expected, State::consumed, std::memory_order::acq_rel,
                                              std::memory_order::acquire);
    }

    bool retire() noexcept {
        State expected = State::active;
        return state_.compare_exchange_strong(expected, State::retired, std::memory_order::acq_rel,
                                              std::memory_order::acquire);
    }

    std::atomic<State> state_{State::active};
    SelectArmSlot* arm_{nullptr};
    Scheduler* scheduler_{nullptr};
    deadline_tick_t deadline_{0};
};

struct DeadlineHeapEntry {
    enum class Kind : std::uint8_t {
        ordinary,
        select,
    };

    deadline_tick_t deadline{};
    Kind kind{Kind::ordinary};

    union Target {
        TimerRegistration* ordinary;
        SelectTimerRegistration* select;

        constexpr Target() noexcept : ordinary(nullptr) {}
    } target{};

    static DeadlineHeapEntry for_ordinary(TimerRegistration& reg) noexcept {
        DeadlineHeapEntry e;
        e.deadline = reg.deadline();
        e.kind = Kind::ordinary;
        e.target.ordinary = &reg;
        return e;
    }

    static DeadlineHeapEntry for_select(SelectTimerRegistration& reg) noexcept {
        DeadlineHeapEntry e;
        e.deadline = reg.deadline();
        e.kind = Kind::select;
        e.target.select = &reg;
        return e;
    }
};

inline bool heap_less_entry(const DeadlineHeapEntry& a, const DeadlineHeapEntry& b) noexcept {
    return a.deadline < b.deadline;
}

} // namespace detail
} // namespace sluice::async
