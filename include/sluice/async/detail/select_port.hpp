// sluice::async::detail::Select internal type graph — group, arm slot, port.
//
// This header defines the internal Select type graph: SelectGroup,
// SelectArmSlot (with Event/Timer payload union), EventArmPayload,
// TimerArmPayload, and SelectPort. These are detail types, not part of
// the public API.
//
// See docs/e13-select-type-and-lifetime.md for the full property sheet.
#pragma once

#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>

#include <sluice/async/scheduler.hpp>

namespace sluice::async {

class Event;
class Fiber;
class SelectResult;

namespace detail {

class SelectGroup;
class SelectPort;
class SelectTimerRegistration;

// ---- Enums ----

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

// ---- Payload types ----

struct EventArmPayload {
    Event* event_{nullptr};
};

struct TimerArmPayload {
    select_deadline_t deadline_{0};
    SelectTimerRegistration* stable_reg_{nullptr};
};

// ---- SelectArmSlot ----

struct SelectArmSlot {
    ArmKind kind{ArmKind::event};
    ArmState state{ArmState::detached};
    SelectGroup* group{nullptr};

    // Intrusive link fields (Scheduler-only access under global_mtx_).
    SelectArmSlot* next_{nullptr};
    SelectArmSlot* prev_{nullptr};
    SelectPort* home_{nullptr};

    union {
        EventArmPayload event;
        TimerArmPayload timer;
    };

    void construct_event(Event& e) noexcept {
        kind = ArmKind::event;
        event.event_ = &e;
    }

    void construct_timer(select_deadline_t deadline,
                         SelectTimerRegistration* reg = nullptr) noexcept {
        kind = ArmKind::timer;
        timer.deadline_ = deadline;
        timer.stable_reg_ = reg;
    }

    SelectArmSlot() noexcept {}
    SelectArmSlot(const SelectArmSlot&) = delete;
    SelectArmSlot& operator=(const SelectArmSlot&) = delete;
    SelectArmSlot(SelectArmSlot&&) = delete;
    SelectArmSlot& operator=(SelectArmSlot&&) = delete;
};

// ---- SelectGroup ----

// kNoWinner sentinel for winner_ atomic.
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

    // Mark this group as a real admitted Select operation (not a structural object).
    void mark_admitted() noexcept { admitted_ = true; }

    GroupPhase phase() const noexcept {
        return phase_.load(std::memory_order::acquire);
    }

    void set_phase(GroupPhase p) noexcept {
        phase_.store(p, std::memory_order::release);
    }

    std::uint32_t winner() const noexcept {
        return winner_.load(std::memory_order::relaxed);
    }

    bool claim_winner(std::uint32_t arm_index) noexcept {
        std::uint32_t expected = kNoWinner;
        return winner_.compare_exchange_strong(expected, arm_index,
                                                std::memory_order::relaxed,
                                                std::memory_order::relaxed);
    }

    // Group fields — set during admission.
    Scheduler* scheduler_{nullptr};
    SelectArmSlot* arms_{nullptr};
    std::size_t arm_count_{0};
    Fiber* caller_{nullptr};
    WorkerState* caller_owner_{nullptr};

    // Completion state.
    CompletionMode completion_mode_{CompletionMode::none};

    // Broadcast worklist fields (temporary, used inside event_set_broadcast CS).
    SelectGroup* broadcast_next_{nullptr};
    std::uint64_t broadcast_epoch_{0};

private:
    friend class Scheduler;

    std::atomic<GroupPhase> phase_{GroupPhase::building};
    std::atomic<std::uint32_t> winner_{kNoWinner};
    bool admitted_{false};
};

// ---- SelectPort ----

class SelectPort {
public:
    SelectPort() = default;

    SelectPort(const SelectPort&) = delete;
    SelectPort& operator=(const SelectPort&) = delete;
    SelectPort(SelectPort&&) = delete;
    SelectPort& operator=(SelectPort&&) = delete;

    bool empty() const noexcept { return head_ == nullptr; }

private:
    friend class Scheduler;

    SelectArmSlot* head_{nullptr};
};

}  // namespace detail
}  // namespace sluice::async
