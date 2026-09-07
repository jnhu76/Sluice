#pragma once

#include <atomic>
#include <cstdint>

namespace sluice::async {

class WaitNode;
class WaitQueue;
class Scheduler;

using deadline_tick_t = std::uint64_t;

class TimerRegistration {
  public:
    using OnResolveFn = void (*)(void* owner_ctx, bool timer_won) noexcept;

    enum class State : std::uint8_t {
        active = 0,
        retired = 1,
        consumed = 2,
    };

    TimerRegistration() = default;
    TimerRegistration(WaitNode* node, WaitQueue* queue, deadline_tick_t deadline) noexcept
        : node_(node), queue_(queue), deadline_(deadline) {}

    TimerRegistration(const TimerRegistration&) = delete;
    TimerRegistration& operator=(const TimerRegistration&) = delete;
    TimerRegistration(TimerRegistration&&) = delete;
    TimerRegistration& operator=(TimerRegistration&&) = delete;

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

    bool is_active() const noexcept {
        return state_.load(std::memory_order::acquire) == State::active;
    }
    bool is_retired() const noexcept {
        return state_.load(std::memory_order::acquire) == State::retired;
    }
    bool is_consumed() const noexcept {
        return state_.load(std::memory_order::acquire) == State::consumed;
    }
    State state() const noexcept { return state_.load(std::memory_order::acquire); }

    WaitNode* node() const noexcept { return node_; }
    WaitQueue* queue() const noexcept { return queue_; }
    deadline_tick_t deadline() const noexcept { return deadline_; }

    bool has_on_resolve() const noexcept { return on_resolve_ != nullptr; }
    void fire_on_resolve_locked(bool timer_won) noexcept {
        if (on_resolve_ != nullptr) {
            on_resolve_(owner_ctx_, timer_won);
        }
    }

    std::size_t heap_index = static_cast<std::size_t>(-1);

  private:
    friend class Scheduler;

    std::atomic<State> state_{State::active};
    WaitNode* node_{nullptr};
    WaitQueue* queue_{nullptr};
    deadline_tick_t deadline_{0};

    OnResolveFn on_resolve_{nullptr};
    void* owner_ctx_{nullptr};
};

} // namespace sluice::async
