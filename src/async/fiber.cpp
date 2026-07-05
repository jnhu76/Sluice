// Implementation of Fiber state machine (sluice-CORE-E1). See fiber.hpp.
#include <sluice/async/fiber.hpp>

namespace sluice::async {

void Fiber::make_runnable() noexcept {
    FiberState expected = FiberState::created;
    // created -> runnable (CAS); or waiting -> runnable (CAS).
    if (state_.compare_exchange_strong(expected, FiberState::runnable,
                                       std::memory_order::acq_rel)) {
        return;
    }
    expected = FiberState::waiting;
    // waiting -> runnable. If the current state is neither created nor waiting
    // (e.g. already runnable, running, or done), this is a no-op — making an
    // already-runnable fiber runnable again, or waking a finished fiber, are
    // both benign/forbidden respectively (done is absorbing: a failed CAS from
    // done leaves state at done).
    (void)state_.compare_exchange_strong(expected, FiberState::runnable,
                                         std::memory_order::acq_rel);
}

void Fiber::make_running() noexcept {
    // Lawful from runnable only. A CAS documents the transition; if it fails
    // (e.g. a concurrent wakeup raced the fiber to waiting), the worker must
    // not run it. E4's scheduler treats a failed CAS as "lost the race, put it
    // back / pick another".
    FiberState expected = FiberState::runnable;
    (void)state_.compare_exchange_strong(expected, FiberState::running,
                                         std::memory_order::acq_rel);
}

void Fiber::make_waiting() noexcept {
    // Lawful from running only (the fiber's own entry suspends itself).
    FiberState expected = FiberState::running;
    (void)state_.compare_exchange_strong(expected, FiberState::waiting,
                                         std::memory_order::acq_rel);
}

void Fiber::make_done() noexcept {
    // Terminal: store unconditionally (the entry returned; the fiber is done
    // regardless of what it raced with). Absorbing: no transition out of done.
    state_.store(FiberState::done, std::memory_order::release);
}

}  // namespace sluice::async
