#pragma once

#include <atomic>
#include <cassert>
#include <cstdint>

namespace sluice::async {

class Fiber;
class WaitQueue;

class WaitResume {
  public:
    enum class Kind : std::uint8_t { none = 0, fiber = 1, deferred = 2 };

    constexpr WaitResume() noexcept = default;

    static constexpr WaitResume none() noexcept { return WaitResume{}; }

    static constexpr WaitResume fiber(Fiber* f) noexcept {
        return f != nullptr ? WaitResume{f, Kind::fiber} : WaitResume{};
    }
    static constexpr WaitResume deferred(void* delivery_record) noexcept {
        return WaitResume{delivery_record, Kind::deferred};
    }

    constexpr Kind kind() const noexcept { return kind_; }

    constexpr Fiber* as_fiber() const noexcept { return static_cast<Fiber*>(ptr_); }

    constexpr void* as_deferred() const noexcept { return ptr_; }

  private:
    constexpr WaitResume(void* p, Kind k) noexcept : ptr_(p), kind_(k) {}
    void* ptr_ = nullptr;
    Kind kind_ = Kind::none;
};

class ActorId {
  public:
    enum class Kind : std::uint8_t { none = 0, fiber = 1, frontend = 2 };

    constexpr ActorId() noexcept = default;

    static constexpr ActorId none() noexcept { return ActorId{}; }
    static constexpr ActorId fiber(Fiber* f) noexcept { return ActorId{f, Kind::fiber}; }

    static constexpr ActorId frontend(void* token) noexcept {
        return ActorId{token, Kind::frontend};
    }

    constexpr Kind kind() const noexcept { return kind_; }
    constexpr void* token() const noexcept { return ptr_; }

    friend constexpr bool operator==(const ActorId& a, const ActorId& b) noexcept {
        return a.ptr_ == b.ptr_ && a.kind_ == b.kind_;
    }
    friend constexpr bool operator!=(const ActorId& a, const ActorId& b) noexcept {
        return !(a == b);
    }

  private:
    constexpr ActorId(void* p, Kind k) noexcept : ptr_(p), kind_(k) {}
    void* ptr_ = nullptr;
    Kind kind_ = Kind::none;
};

enum class WaitOutcome : std::uint8_t {

    unresolved = 0,

    woken = 1,

    cancelled = 2,

    expired = 3,
};

class WaitNode {
  public:
    WaitNode() noexcept = default;

    explicit WaitNode(WaitResume resume) noexcept : resume_(resume) {}

    explicit WaitNode(Fiber* fiber) noexcept : resume_(WaitResume::fiber(fiber)) {}

    ~WaitNode() {
        assert(!is_registered() && "WaitNode destroyed while Registered (resolve the wait first)");
    }

    void* user() const noexcept { return user_; }
    void set_user(void* p) noexcept { user_ = p; }

    WaitNode(const WaitNode&) = delete;
    WaitNode& operator=(const WaitNode&) = delete;
    WaitNode(WaitNode&&) = delete;
    WaitNode& operator=(WaitNode&&) = delete;

    bool is_registered() const noexcept {
        return state_.load(std::memory_order::acquire) == State::registered;
    }
    bool is_terminal() const noexcept {
        const auto s = state_.load(std::memory_order::acquire);
        return s == State::woken || s == State::cancelled || s == State::expired;
    }

    WaitOutcome outcome() const noexcept {
        const auto s = state_.load(std::memory_order::acquire);
        if (s == State::woken)
            return WaitOutcome::woken;
        if (s == State::cancelled)
            return WaitOutcome::cancelled;
        if (s == State::expired)
            return WaitOutcome::expired;
        return WaitOutcome::unresolved;
    }
    bool was_woken() const noexcept {
        return state_.load(std::memory_order::acquire) == State::woken;
    }
    bool was_cancelled() const noexcept {
        return state_.load(std::memory_order::acquire) == State::cancelled;
    }

    bool was_expired() const noexcept {
        return state_.load(std::memory_order::acquire) == State::expired;
    }

    const WaitResume& resume() const noexcept { return resume_; }

    Fiber* fiber() const noexcept { return resume_.as_fiber(); }

    WaitNode* next_{nullptr};
    WaitNode* prev_{nullptr};
    WaitQueue* home_{nullptr};

  private:
    friend class WaitQueue;

    enum class State : std::uint8_t {
        detached = 0,
        registered = 1,
        woken = 2,
        cancelled = 3,
        expired = 4,
    };

    bool register_(WaitQueue* q, const WaitResume& resume) noexcept {
        State expected = State::detached;
        if (!state_.compare_exchange_strong(expected, State::registered, std::memory_order::acq_rel,
                                            std::memory_order::acquire)) {
            return false;
        }
        resume_ = resume;
        home_ = q;
        return true;
    }

    bool resolve_(WaitOutcome outcome) noexcept {
        State target;
        if (outcome == WaitOutcome::woken)
            target = State::woken;
        else if (outcome == WaitOutcome::cancelled)
            target = State::cancelled;
        else if (outcome == WaitOutcome::expired)
            target = State::expired;
        else {
            assert(false && "resolve_ requires a terminal outcome");
            return false;
        }
        State expected = State::registered;
        return state_.compare_exchange_strong(expected, target, std::memory_order::acq_rel,
                                              std::memory_order::acquire);
    }

    WaitResume resume_{};
    std::atomic<State> state_{State::detached};
    void* user_{nullptr};
};

} // namespace sluice::async
