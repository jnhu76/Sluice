// sluice::async::select — E13 Select public value types and constrained surface.
//
// This header defines the public Select API for the first production version:
// typed Event/Timer case values, SelectResult, the SelectCaseType concept,
// and the variadic select() entry point.
//
// P5: the public variadic select() template is DEFINED here (a thin bridge). A
// general function-template definition must be visible to arbitrary user TUs,
// so it cannot live only in src/async/select.cpp. The template materializes a
// fixed caller-frame std::array of SelectCaseDescriptor (preserving argument
// order as the arm index) and forwards to ONE non-template Scheduler admission
// function (Scheduler::select_admit_inline) that owns all centralized admission
// logic. No explicit-instantiation combinatorial explosion; the admission core
// compiles once. See docs/e13-select-public-api.md §3 and
// docs/e13-select-production-architecture.md §8.
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

// ---- E6b: detail::SelectCaseDescriptor — the variadic bridge type ----
//
// P5 template/linkage architecture (docs/e13-select-production-architecture.md
// §8, the task's section 5 gate). The public select() template must be visible
// to arbitrary user TUs, but the centralized admission logic must remain in
// ONE non-template Scheduler function (no per-Event/Timer-permutation explicit
// instantiation, no admission-logic duplication in the header).
//
// SelectCaseDescriptor is the narrow internal bridge: a fixed-size, trivially-
// destructible descriptor for ONE case. The variadic expansion materializes a
// std::array<SelectCaseDescriptor, N> in the caller frame (preserving argument
// order as the arm index), then forwards the array + count to the single
// Scheduler::select_admit_inline core.
//
// It does NOT expose case private fields publicly: its constructors read
// EventSelectCase::event_ / TimerSelectCase::scheduler_/deadline_ via the friend
// grant above, and its own fields are private detail state. No public raw
// pointer getter exists; the admission core reads them under the friend
// Scheduler grant on SelectCaseDescriptor.
namespace detail {

struct SelectCaseDescriptor {
    enum class Kind : std::uint8_t { event, timer };

    // The kind discriminator.
    Kind kind{Kind::event};
    // Scheduler identity for validation (Event: read from the Event; Timer: the
    // explicit Scheduler arg). Captured once at descriptor construction, BEFORE
    // any allocation/registration, so cross-Scheduler rejection happens early.
    Scheduler* scheduler{nullptr};
    // Event payload (kind == event). Borrowed for the registration CS only.
    Event* event{nullptr};
    // Timer payload (kind == timer). Absolute monotonic deadline.
    select_deadline_t deadline{0};

    // Construct from a typed case value, reading its private fields via friend.
    // const-ref binds to both lvalue and rvalue cases; only the borrowed
    // pointers/deadline are copied into the descriptor (the case object itself
    // is not stored).
    explicit SelectCaseDescriptor(const EventSelectCase& c) noexcept
        : kind(Kind::event), event(c.event_) {}
    explicit SelectCaseDescriptor(const TimerSelectCase& c) noexcept
        : kind(Kind::timer), scheduler(c.scheduler_),
          deadline(c.deadline_) {}

    SelectCaseDescriptor(const SelectCaseDescriptor&) noexcept = default;
    SelectCaseDescriptor& operator=(const SelectCaseDescriptor&) noexcept = default;
};

// ---- SelectBridge: concrete friend routing select() -> select_admit_inline ----
//
// The public select() template forwards here; admit() calls the private
// Scheduler::select_admit_inline under the friend grant Scheduler declares
// (`friend struct detail::SelectBridge;`). A concrete struct (not a
// constrained template) is used so the friend names exactly one entity.
// admit() is a public static method — but it only reaches the private
// admission core, so no production code can forge admission without going
// through the public select().
struct SelectBridge {
    static SelectResult admit(Scheduler& scheduler,
                              SelectCaseDescriptor* descs, std::size_t count) {
        return scheduler.select_admit_inline(descs, count);
    }
};

}  // namespace detail

// ---- E7: SelectCaseType concept ----

template <class T>
concept SelectCaseType = std::is_same_v<std::remove_cvref_t<T>, EventSelectCase> ||
                         std::is_same_v<std::remove_cvref_t<T>, TimerSelectCase>;

// ---- Constrained select() definition (P5: thin variadic bridge) ----
//
// The public variadic entry point. 1 <= sizeof...(Cases) <= kSelectMaxArms.
// The requires clause rejects empty packs, too-large packs, and non-case types
// at compile time (SF-1..SF-3) and keeps select() SFINAE-friendly (the P1
// SF compile-fail tests evaluate SelectInvocable<...> to false on bad inputs).
// Argument order IS the arm index (lowest-index tie-break); the variadic
// expands into a fixed caller-frame array — no per-call heap for case storage.
//
// This template is deliberately THIN: it only materializes the case descriptor
// array and forwards through detail::SelectBridge to the single non-template
// Scheduler admission core (Scheduler::select_admit_inline), which owns all
// validation, registration, snapshot, P4 processing, and inline completion.
// The admission logic therefore compiles exactly once and is not duplicated per
// Event/Timer permutation.
//
// detail::SelectBridge is a concrete (non-template) friend of Scheduler: a
// constrained function-template friend would not name the same entity as this
// definition (the requires clause makes them distinct templates), so the bridge
// routes through a friend struct instead. SelectBridge::admit is public; the
// private select_admit_inline stays reachable only via the friendship.

template <class... Cases>
    requires (
        sizeof...(Cases) >= 1 &&
        sizeof...(Cases) <= kSelectMaxArms &&
        (SelectCaseType<std::remove_cvref_t<Cases>> && ...)
    )
SelectResult select(Scheduler& scheduler, Cases&&... cases) {
    // Fixed caller-frame storage (zero dynamic allocation for case descriptors).
    std::array<detail::SelectCaseDescriptor, sizeof...(Cases)> descs{
        detail::SelectCaseDescriptor{std::forward<Cases>(cases)}...};
    return detail::SelectBridge::admit(scheduler, descs.data(), sizeof...(Cases));
}

}  // namespace sluice::async
