// sluice::async::select — E13 Select public value types and constrained surface.
//
// This header defines the public Select API for the first production version:
// typed Event/Timer case values, SelectResult, the SelectCaseType concept,
// and the variadic select() entry point.
//
// P1: the select() function template is declared (with its full requires clause)
// but NOT defined. It has no callable production behavior. The declaration is
// present so compile-time constraint checks (SF-1..SF-3) can verify substitution
// failure, and so the return type is visible for decltype/requires checks.
//
// See docs/e13-select-public-api.md for the frozen API surface.
#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <type_traits>

#include <sluice/async/scheduler.hpp>

namespace sluice::async {

class Event;

// ---- E1: Constants and aliases ----

inline constexpr std::size_t kSelectMaxArms = 8;

using select_deadline_t = Scheduler::deadline_t;

// ---- E2: Winning kind ----

enum class SelectKind : std::uint8_t {
    event = 0,
    timer = 1,
};

// ---- E3: Timer outcome ----

enum class SelectTimerOutcome : std::uint8_t {
    fired = 0,
};

// ---- E4: SelectResult ----

class SelectResult {
public:
    // Default: no winner sentinel.
    constexpr SelectResult() noexcept = default;

    [[nodiscard]] constexpr bool has_winner() const noexcept {
        return has_winner_;
    }

    [[nodiscard]] constexpr std::size_t index() const noexcept {
        if (!has_winner_) {
            assert(false && "SelectResult::index() called with no winner");
        }
        return index_;
    }

    [[nodiscard]] constexpr SelectKind kind() const noexcept {
        if (!has_winner_) {
            assert(false && "SelectResult::kind() called with no winner");
            return SelectKind::event;
        }
        return kind_;
    }

    [[nodiscard]] constexpr SelectTimerOutcome timer_outcome() const noexcept {
        if (!(has_winner_ && kind_ == SelectKind::timer)) {
            assert(false && "SelectResult::timer_outcome() called when not timer winner");
            return SelectTimerOutcome::fired;
        }
        return timer_outcome_;
    }

private:
    friend class Scheduler;

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
public:
    struct TestInit {};
    constexpr SelectResult(std::size_t index, SelectKind kind,
                           SelectTimerOutcome timer_outcome,
                           TestInit) noexcept
        : index_(index), kind_(kind), timer_outcome_(timer_outcome),
          has_winner_(true) {}
private:
#endif

    std::size_t index_{0};
    SelectKind kind_{SelectKind::event};
    SelectTimerOutcome timer_outcome_{SelectTimerOutcome::fired};
    bool has_winner_{false};
};

// ---- E5: EventSelectCase ----

class EventSelectCase {
public:
    explicit EventSelectCase(Event& event) noexcept : event_(&event) {}

private:
    friend class Scheduler;
    Event* event_;
};

// ---- E6: TimerSelectCase ----

class TimerSelectCase {
public:
    explicit TimerSelectCase(Scheduler& scheduler,
                             select_deadline_t deadline) noexcept
        : scheduler_(&scheduler), deadline_(deadline) {}

private:
    friend class Scheduler;
    Scheduler* scheduler_;
    select_deadline_t deadline_;
};

// ---- E7: SelectCaseType concept ----

template <class T>
concept SelectCaseType = std::is_same_v<std::remove_cvref_t<T>, EventSelectCase> ||
                         std::is_same_v<std::remove_cvref_t<T>, TimerSelectCase>;

// ---- Constrained select() declaration ----
// P1: declared but NOT defined. No callable production behavior.

template <class... Cases>
    requires (
        sizeof...(Cases) >= 1 &&
        sizeof...(Cases) <= kSelectMaxArms &&
        (SelectCaseType<std::remove_cvref_t<Cases>> && ...)
    )
SelectResult select(Scheduler& scheduler, Cases&&... cases);

}  // namespace sluice::async
