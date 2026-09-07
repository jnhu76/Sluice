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

using select_deadline_t = Scheduler::deadline_t;

enum class SelectKind : std::uint8_t {
    event = 0,
    timer = 1,
};

enum class SelectTimerOutcome : std::uint8_t {
    fired = 0,
};

class SelectResult {
  public:
    constexpr SelectResult() noexcept = default;

    [[nodiscard]] constexpr bool has_winner() const noexcept { return has_winner_; }

    [[nodiscard]] constexpr std::size_t index() const noexcept {
        if (!has_winner_) {
            assert(false && "SelectResult::index() called with no winner");
            return 0;
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

    constexpr SelectResult(std::size_t index, SelectKind kind,
                           SelectTimerOutcome timer_outcome) noexcept
        : index_(index), kind_(kind), timer_outcome_(timer_outcome), has_winner_(true) {}

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
  public:
    struct TestInit {};
    constexpr SelectResult(std::size_t index, SelectKind kind, SelectTimerOutcome timer_outcome,
                           TestInit) noexcept
        : index_(index), kind_(kind), timer_outcome_(timer_outcome), has_winner_(true) {}

  private:
#endif

    std::size_t index_{0};
    SelectKind kind_{SelectKind::event};
    SelectTimerOutcome timer_outcome_{SelectTimerOutcome::fired};
    bool has_winner_{false};
};

class EventSelectCase {
  public:
    explicit EventSelectCase(Event& event) noexcept : event_(&event) {}

  private:
    friend class Scheduler;
    friend class detail::SelectCaseDescriptor;
    Event* event_;
};

class TimerSelectCase {
  public:
    explicit TimerSelectCase(Scheduler& scheduler, select_deadline_t deadline) noexcept
        : scheduler_(&scheduler), deadline_(deadline) {}

  private:
    friend class Scheduler;
    friend class detail::SelectCaseDescriptor;
    Scheduler* scheduler_;
    select_deadline_t deadline_;
};

namespace detail {

class SelectCaseDescriptor {
  public:
    enum class Kind : std::uint8_t { event, timer };

    explicit SelectCaseDescriptor(const EventSelectCase& c) noexcept
        : kind_(Kind::event), event_(c.event_) {}
    explicit SelectCaseDescriptor(const TimerSelectCase& c) noexcept
        : kind_(Kind::timer), scheduler_(c.scheduler_), deadline_(c.deadline_) {}

    SelectCaseDescriptor(const SelectCaseDescriptor&) noexcept = default;
    SelectCaseDescriptor& operator=(const SelectCaseDescriptor&) noexcept = default;

  private:
    friend class ::sluice::async::Scheduler;

    Kind kind_{Kind::event};
    Scheduler* scheduler_{nullptr};
    Event* event_{nullptr};
    select_deadline_t deadline_{0};
};

} // namespace detail

template <class... Cases>
    requires(sizeof...(Cases) >= 1 && sizeof...(Cases) <= kSelectMaxArms &&
             (SelectCaseType<Cases> && ...))
SelectResult select(Scheduler& scheduler, Cases&&... cases) {
    std::array<detail::SelectCaseDescriptor, sizeof...(Cases)> descs{
        detail::SelectCaseDescriptor{std::forward<Cases>(cases)}...};
    return scheduler.select_admit(descs.data(), sizeof...(Cases));
}

} // namespace sluice::async
