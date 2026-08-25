// Implementation of Fiber state machine. See fiber.hpp.
#include <sluice/async/fiber.hpp>

namespace sluice::async {

bool Fiber::make_runnable() noexcept {
    FiberState expected = FiberState::created;
    // created -> runnable (CAS); or waiting -> runnable (CAS). Returns true
    // only when the transition actually occurred. This is the load-bearing
    // exactly-once-publication invariant: a successful transition into
    // runnable grants the caller the right to publish EXACTLY ONE runnable
    // ticket. A no-op transition (fiber already runnable, or running, or done)
    // returns false — the caller MUST NOT enqueue a second ticket.
    if (state_.compare_exchange_strong(expected, FiberState::runnable,
                                       std::memory_order::acq_rel)) {
        return true;
    }
    expected = FiberState::waiting;
    // waiting -> runnable.
    return state_.compare_exchange_strong(expected, FiberState::runnable,
                                          std::memory_order::acq_rel);
}

bool Fiber::make_running() noexcept {
    // Lawful from runnable only. Returns true ONLY when the transition actually
    // occurred (runnable->running). The caller (run_next_on) MUST check this
    // return value and fail-fast on failure — a silent no-op would allow
    // context-switching into an invalid Fiber context.
    FiberState expected = FiberState::runnable;
    return state_.compare_exchange_strong(expected, FiberState::running,
                                          std::memory_order::acq_rel);
}

bool Fiber::make_waiting() noexcept {
    // Lawful from running only (the fiber's own entry suspends itself).
    // Returns true ONLY when the transition actually occurred (running->waiting).
    // The caller (commit_suspend_locked) MUST check this return value and
    // fail-fast on failure — a silent no-op would strand the Fiber in an
    // inconsistent state.
    FiberState expected = FiberState::running;
    return state_.compare_exchange_strong(expected, FiberState::waiting,
                                          std::memory_order::acq_rel);
}

void Fiber::make_done() noexcept {
    // Terminal: store unconditionally (the entry returned; the fiber is done
    // regardless of what it raced with). Absorbing: no transition out of done.
    state_.store(FiberState::done, std::memory_order::release);
}

}  // namespace sluice::async
