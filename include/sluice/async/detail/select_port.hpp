#pragma once

#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <type_traits>
#include <utility>

#include <sluice/async/select.hpp>
#include <sluice/async/scheduler.hpp>

namespace sluice::async {

class Event;
class Fiber;
class SelectResult;

namespace detail {

class SelectGroup;
class SelectPort;
class SelectTimerRegistration;

enum class ArmKind : std::uint8_t {
    event = 0,
    timer = 1,
};

enum class ArmState : std::uint8_t {
    detached = 0,
    prepared = 1,
    registered = 2,
    candidate_ready = 3,
    retired = 4,
};

enum class GroupPhase : std::uint8_t {
    building = 0,
    selecting = 1,
    armed = 2,
    completed = 3,
    consumed = 4,
    rollback = 5,
    aborted = 6,
};

enum class CompletionMode : std::uint8_t {
    none = 0,
    inline_ = 1,
    suspended = 2,
};

struct EventArmPayload {
    Event* event_{nullptr};
};

struct TimerArmPayload {
    select_deadline_t deadline_{0};
    SelectTimerRegistration* stable_reg_{nullptr};
};

static_assert(std::is_trivially_destructible_v<EventArmPayload>,
              "EventArmPayload must be trivially destructible");
static_assert(std::is_trivially_destructible_v<TimerArmPayload>,
              "TimerArmPayload must be trivially destructible");

struct SelectArmSlot {
    ArmKind kind{ArmKind::event};
    ArmState state{ArmState::detached};
    SelectGroup* group{nullptr};

    SelectArmSlot* next_{nullptr};
    SelectArmSlot* prev_{nullptr};
    SelectPort* home_{nullptr};

    union {
        EventArmPayload event;
        TimerArmPayload timer;
    };

    void construct_event(Event& e) noexcept {
        if (kind == ArmKind::timer) {
            std::destroy_at(std::addressof(timer));
            std::construct_at(std::addressof(event), EventArmPayload{&e});
        } else {
            event = EventArmPayload{&e};
        }
        kind = ArmKind::event;
    }

    void construct_timer(select_deadline_t deadline,
                         SelectTimerRegistration* reg = nullptr) noexcept {
        if (kind == ArmKind::event) {
            std::destroy_at(std::addressof(event));
            std::construct_at(std::addressof(timer), TimerArmPayload{deadline, reg});
        } else {
            timer = TimerArmPayload{deadline, reg};
        }
        kind = ArmKind::timer;
    }

    SelectArmSlot() noexcept : event{} {}

    ~SelectArmSlot() noexcept {
        if (kind == ArmKind::event) {
            std::destroy_at(std::addressof(event));
        } else {
            std::destroy_at(std::addressof(timer));
        }
    }

    SelectArmSlot(const SelectArmSlot&) = delete;
    SelectArmSlot& operator=(const SelectArmSlot&) = delete;
    SelectArmSlot(SelectArmSlot&&) = delete;
    SelectArmSlot& operator=(SelectArmSlot&&) = delete;
};

inline constexpr std::uint32_t kNoWinner = static_cast<std::uint32_t>(-1);

class SelectGroup {
  public:
    SelectGroup() = default;

    SelectGroup(const SelectGroup&) = delete;
    SelectGroup& operator=(const SelectGroup&) = delete;
    SelectGroup(SelectGroup&&) = delete;
    SelectGroup& operator=(SelectGroup&&) = delete;

    ~SelectGroup() {
        if (admitted_) {
            const auto ph = phase_.load(std::memory_order::acquire);
            assert((ph == GroupPhase::consumed || ph == GroupPhase::aborted) &&
                   "SelectGroup destroyed without Consumed or Aborted phase");
            (void)ph;
        }
    }

    void mark_admitted() noexcept { admitted_ = true; }

    GroupPhase phase() const noexcept { return phase_.load(std::memory_order::acquire); }

    void set_phase(GroupPhase p) noexcept { phase_.store(p, std::memory_order::release); }

    std::uint32_t winner() const noexcept { return winner_.load(std::memory_order::relaxed); }

    Scheduler* scheduler_{nullptr};
    SelectArmSlot* arms_{nullptr};
    std::size_t arm_count_{0};
    Fiber* caller_{nullptr};
    WorkerState* caller_owner_{nullptr};

    CompletionMode completion_mode_{CompletionMode::none};

    SelectGroup* broadcast_next_{nullptr};
    std::uint64_t broadcast_epoch_{0};

  private:
    friend class ::sluice::async::Scheduler;

    SelectResult result_{};

    bool claim_winner_locked(std::uint32_t arm_index) noexcept {
        std::uint32_t expected = kNoWinner;
        return winner_.compare_exchange_strong(expected, arm_index, std::memory_order::relaxed,
                                               std::memory_order::relaxed);
    }

    std::atomic<GroupPhase> phase_{GroupPhase::building};
    std::atomic<std::uint32_t> winner_{kNoWinner};
    bool admitted_{false};
};

class SelectPort {
  public:
    SelectPort() = default;

    SelectPort(const SelectPort&) = delete;
    SelectPort& operator=(const SelectPort&) = delete;
    SelectPort(SelectPort&&) = delete;
    SelectPort& operator=(SelectPort&&) = delete;

    bool empty() const noexcept { return head_ == nullptr; }

  private:
    friend class ::sluice::async::Scheduler;

    SelectArmSlot* head_{nullptr};
};

} // namespace detail
} // namespace sluice::async
