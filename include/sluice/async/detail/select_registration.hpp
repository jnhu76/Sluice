// sluice::async::detail — E13 Select Timer stable block + tagged deadline heap.
//
// This header is SELF-CONTAINED (Addendum A): it does NOT include
// scheduler.hpp. It depends only on timer_registration.hpp (for the ordinary
// TimerRegistration pointer the heap shares) plus forward-declared Scheduler.
// The internal Timer substrate uses deadline_tick_t (the underlying tick
// type), NOT select_deadline_t — the latter is the public Select API alias
// only and lives in select.hpp. Both alias the same std::uint64_t.
//
// Two types live here:
//
//   SelectTimerRegistration — a Scheduler-owned, independently-stable control
//   block for one active Select Timer arm. Mirrors TimerRegistration's
//   retirement discipline but binds to Select objects (SelectArmSlot*)
//   instead of WaitNode. Its atomic state_ is the post-destruction safety
//   boundary (I4): the pump reads state_ first; if not active, it skips
//   without dereferencing the arm pointer. See docs/e13-select-timer-adapter.md.
//
//   DeadlineHeapEntry — the unified internal entry the Scheduler's deadline
//   min-heap stores. It tags each entry Ordinary or Select and holds the
//   matching stable-block pointer. Internal-only; no public API exposure.
//   See docs/e13-select-timer-adapter.md §4.
#pragma once

#include <sluice/async/timer_registration.hpp>  // TimerRegistration, deadline_tick_t

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace sluice::async {

class Scheduler;  // owning scheduler — stored by pointer, forward-declared

namespace detail {

class SelectArmSlot;

// One Select Timer registration control block. Non-copyable, non-movable
// (its address is its identity for the Scheduler-owned stable pool and for
// the deadline-heap entry that references it by pointer).
//
// NOTE on heap_index (Addendum G): the ordinary TimerRegistration retains its
// legacy heap_index field to avoid unrelated E11 churn, but no production
// reader treats it as an authority. SelectTimerRegistration carries NO
// heap_index: the DeadlineHeapEntry's vector position is the sole position
// authority, and there is no indexed-removal behaviour (the pump only ever
// pops the min). Adding a second heap-position authority is explicitly
// forbidden (brief §6.1).
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

    // Single-object CAS with no external side effects. The Scheduler's
    // accounting helpers (select_timer_retire_locked /
    // select_timer_consume_locked) own the counter + earliest-deadline
    // bookkeeping AROUND these CASes. Direct CAS calls are only valid for
    // detached, never-registered local objects (e.g. T1 state-transition tests).
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

    deadline_tick_t deadline() const noexcept { return deadline_; }
    SelectArmSlot* arm() const noexcept { return arm_; }
    Scheduler* scheduler() const noexcept { return scheduler_; }

private:
    friend class ::sluice::async::Scheduler;

    std::atomic<State> state_{State::active};
    SelectArmSlot* arm_{nullptr};
    Scheduler* scheduler_{nullptr};
    deadline_tick_t deadline_{0};
};

// The unified internal entry stored by the Scheduler's deadline min-heap.
// One entry is either Ordinary (an ordinary TimerRegistration*) or Select
// (a SelectTimerRegistration*); both share the same min-heap ordering keyed
// by the cached deadline (Addendum B).
//
// Heap ordering contract (Addendum B): smaller deadline first; the relative
// order of equal-deadline entries is UNSPECIFIED. No test may rely on stable
// FIFO ordering for equal deadlines.
//
// The entry is always constructed through for_ordinary / for_select so the
// discriminator and the active union member are both initialized (no
// partially-initialized aggregate union).
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

    static DeadlineHeapEntry for_select(
        SelectTimerRegistration& reg) noexcept {
        DeadlineHeapEntry e;
        e.deadline = reg.deadline();
        e.kind = Kind::select;
        e.target.select = &reg;
        return e;
    }
};

// Min-heap comparator: strictly less by deadline. Equal-deadline entries are
// unordered (no stable tie-break). Static so the heap helpers in scheduler.cpp
// can call it without a Scheduler instance.
inline bool heap_less_entry(const DeadlineHeapEntry& a,
                            const DeadlineHeapEntry& b) noexcept {
    return a.deadline < b.deadline;
}

}  // namespace detail
}  // namespace sluice::async
