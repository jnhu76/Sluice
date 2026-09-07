#pragma once

#include <sluice/async/cancel.hpp>
#include <sluice/async/fiber_ctx.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <utility>

namespace sluice::async {

enum class FiberState : std::uint8_t {
    created,
    runnable,
    running,
    waiting,
    done,
};

enum class CompletionWaitOutcome : std::uint8_t {
    pending,
    completed,
    canceled,
};

class Fiber {
  public:
    using Entry = std::function<void(Fiber&)>;

    Fiber() = default;
    explicit Fiber(Entry entry) : entry_(std::move(entry)) {}

    Fiber(const Fiber&) = delete;
    Fiber& operator=(const Fiber&) = delete;
    Fiber(Fiber&&) = delete;
    Fiber& operator=(Fiber&&) = delete;

    FiberState state() const noexcept { return state_.load(std::memory_order::acquire); }

    bool make_runnable() noexcept;

    bool make_running() noexcept;

    bool make_waiting() noexcept;

    void make_done() noexcept;

    Entry& entry() noexcept { return entry_; }
    const Entry& entry() const noexcept { return entry_; }
    void set_entry(Entry e) { entry_ = std::move(e); }

    CancelToken& cancel_token() noexcept { return token_; }
    CancelState& cancel_state() noexcept { return cstate_; }

    void* execution_tag() const noexcept { return execution_tag_; }

    CompletionWaitOutcome completion_wait_outcome() const noexcept {
        return completion_wait_outcome_;
    }

  private:
    friend class Scheduler;
    void set_execution_tag(void* tag) noexcept { execution_tag_ = tag; }

    void set_completion_wait_outcome(CompletionWaitOutcome o) noexcept {
        completion_wait_outcome_ = o;
    }

    std::atomic<FiberState> state_{FiberState::created};
    Entry entry_{};
    CancelToken token_{};
    CancelState cstate_{};
    void* execution_tag_{nullptr};
    CompletionWaitOutcome completion_wait_outcome_{CompletionWaitOutcome::pending};

  public:
    fiber_ctx::Context ctx{};

  private:
};

} // namespace sluice::async
