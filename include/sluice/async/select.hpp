// sluice::async::select — E13 Select public value types and constrained surface.
//
// This header defines the public Select API for the first production version:
// typed Event/Timer case values, SelectResult, the SelectCaseType concept,
// and the variadic select() entry point.
//
// P5/P6: the public variadic select() template is DEFINED here (a thin bridge).
// A general function-template definition must be visible to arbitrary user TUs,
// so it cannot live only in src/async/select.cpp. The template materializes a
// fixed caller-frame std::array of SelectCaseDescriptor (preserving argument
// order as the arm index) and forwards to ONE non-template Scheduler admission
// function (Scheduler::select_admit) that owns all centralized admission logic
// for BOTH the inline-ready and the no-ready suspended paths. No explicit-
// instantiation combinatorial explosion; the admission core compiles once. See
// docs/e13-select-public-api.md §3 and docs/e13-select-production-architecture.md
// §8.
//
// See docs/e13-select-public-api.md for the frozen API surface.
#pragma once

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <utility>

#include <sluice/async/scheduler.hpp>

namespace sluice::async {

class Event;

// ---- E1: Constants and aliases ----

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
    friend struct detail::SelectCaseDescriptor;

    // P5: the narrow production construction path (docs/e13-select-public-api.md
    // §6). Scheduler-only: an admitted group builds exactly ONE SelectResult
    // from the winner index. Losers never construct a result. This is NOT
    // public — no arbitrary-result constructor is exposed. A default-constructed
    // SelectResult remains the "no winner" sentinel.
    constexpr SelectResult(std::size_t index, SelectKind kind,
                           SelectTimerOutcome timer_outcome) noexcept
        : index_(index), kind_(kind), timer_outcome_(timer_outcome),
          has_winner_(true) {}

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
    friend struct detail::SelectCaseDescriptor;
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
    friend struct detail::SelectCaseDescriptor;
    Scheduler* scheduler_;
    select_deadline_t deadline_;
};

// ---- E6b: detail::SelectCaseDescriptor — the sealed variadic bridge type ----
//
// P5 CORRECTIVE: SelectCaseDescriptor is a CLASS with PRIVATE fields. Only
// its typed constructors (from EventSelectCase/TimerSelectCase) can establish
// descriptors; only friend Scheduler can read the fields. No public raw
// pointer getter, no public field access. An ordinary TU cannot forge a
// descriptor with arbitrary values.
//
// The public select() template constructs descriptors via the public typed
// constructors, then passes the array to the single non-template
// Scheduler::select_admit_inline (which reads fields under the Scheduler
// friend grant). No intermediate bridge type is needed.
namespace detail {

class SelectCaseDescriptor {
public:
    enum class Kind : std::uint8_t { event, timer };

    explicit SelectCaseDescriptor(const EventSelectCase& c) noexcept
        : kind_(Kind::event), event_(c.event_) {}
    explicit SelectCaseDescriptor(const TimerSelectCase& c) noexcept
        : kind_(Kind::timer), scheduler_(c.scheduler_),
          deadline_(c.deadline_) {}

    SelectCaseDescriptor(const SelectCaseDescriptor&) noexcept = default;
    SelectCaseDescriptor& operator=(const SelectCaseDescriptor&) noexcept = default;

private:
    friend class ::sluice::async::Scheduler;

    Kind kind_{Kind::event};
    Scheduler* scheduler_{nullptr};
    Event* event_{nullptr};
    select_deadline_t deadline_{0};
};

}  // namespace detail

// ---- CORRECTIVE: constrained select() definition (thin variadic bridge) ----
//
// The public variadic entry point. 1 <= sizeof...(Cases) <= kSelectMaxArms.
// The requires clause rejects empty packs, too-large packs, and non-case types
// at compile time (SF-1..SF-3) and keeps select() SFINAE-friendly (the P1
// SF compile-fail tests evaluate SelectInvocable<...> to false on bad inputs).
// Argument order IS the arm index (lowest-index tie-break); the variadic
// expands into a fixed caller-frame array — no per-call heap for case storage.
//
// This template is deliberately THIN: it only materializes the case descriptor
// array and calls Scheduler::select_admit directly (no intermediate bridge
// struct). Scheduler friends this exact constrained template entity, so the
// admission core remains private. No concrete bridge type is published.
//
// The admission logic compiles exactly once and is not duplicated per
// Event/Timer permutation.

template <class... Cases>
    requires (
        sizeof...(Cases) >= 1 &&
        sizeof...(Cases) <= kSelectMaxArms &&
        (SelectCaseType<Cases> && ...)
    )
SelectResult select(Scheduler& scheduler, Cases&&... cases) {
    std::array<detail::SelectCaseDescriptor, sizeof...(Cases)> descs{
        detail::SelectCaseDescriptor{std::forward<Cases>(cases)}...};
    return scheduler.select_admit(descs.data(), sizeof...(Cases));
}

}  // namespace sluice::async
