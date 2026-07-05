// sluice::async::Scheduler — minimal single-worker Evented scheduler
// (sluice-CORE-E4).
//
// The smallest system that proves the E4 success criterion (ADR-execution-model
// §9.1): with ONE scheduler worker, Fiber B makes progress while Fiber A
// awaits a pending async operation. This is NOT the final runtime — it is the
// single-worker experiment that proves scheduler liveness.
//
// What it does:
//   - owns one scheduler Context + a FIFO runnable queue + a waiting map
//     (Completion* -> Fiber*).
//   - spawn(): enqueue a Fiber as runnable.
//   - run_until_idle(): cooperatively run runnable fibers. When a fiber calls
//     await_completion(c), it marks itself waiting, switches back to the
//     scheduler, and the scheduler runs another runnable fiber / polls the
//     backend. A backend completion (observed via AsyncIoContext::poll) wakes
//     the associated fiber (waiting -> runnable) and re-enqueues it.
//
// What it does NOT do (E4 scope; reserved for later phases):
//   - multi-worker scheduling (E7), work stealing (E8).
//   - Future/WaitPolicy integration (E5).
//   - Group-on-Evented (E5).
//   - timers, fairness policy, priority, network, NativeHandle.
//
// Ownership (provable; lifetimes are bounded by run_until_idle):
//   - The TEST owns the Fiber objects (stack/heap, address-stable: Fiber is
//     non-movable), the stacks (passed to init_context), the Completions
//     (non-movable, ADR §5 L7), and the AsyncIoContext (which owns the backend).
//   - The Scheduler BORROWS all of these by reference for the duration of a
//     run. The waiting map holds raw Completion* and Fiber* — both address-
//     stable for the run.
//
// Exactly-once runnable transition:
//   - A Completion is marked ready EXACTLY ONCE by the backend (Completion's
//     own state machine: outstanding -> ready is terminal).
//   - The wake path removes the Completion* -> Fiber* association when it
//     fires, so a subsequent poll observing the same (now-ready) Completion
//     cannot re-enqueue the fiber. E4-T3 proves this.
#pragma once

#include <sluice/async/async_io_context.hpp>
#include <sluice/async/completion.hpp>
#include <sluice/async/fiber.hpp>
#include <sluice/async/fiber_ctx.hpp>

#include <atomic>
#include <cstddef>
#include <deque>
#include <unordered_map>

namespace sluice::async {

// A minimal single-worker Evented scheduler. See file header for scope.
class Scheduler {
public:
    // Borrow the completion source (an AsyncIoContext wrapping e.g. a
    // FakeAsyncBackend). The scheduler polls it inside run_until_idle to
    // observe completions and wake waiting fibers.
    explicit Scheduler(AsyncIoContext& ctx) noexcept;

    Scheduler(const Scheduler&) = delete;
    Scheduler& operator=(const Scheduler&) = delete;
    Scheduler(Scheduler&&) = delete;
    Scheduler& operator=(Scheduler&&) = delete;

    // Enqueue a Fiber as runnable. The Fiber must have been initialized via
    // fiber_ctx::init_context (its entry will run on first scheduling). The
    // Fiber (and its stack) must outlive run_until_idle.
    void spawn(Fiber& fiber) noexcept;

    // Initialize a Fiber's context so the scheduler can run it. The scheduler
    // uses an internal bridge (fiber_entry_bridge in scheduler.cpp) that
    // invokes fiber.entry()(*fiber). The test owns the stack; this call wires
    // the fiber's ctx to that stack + the bridge + &fiber as user_data.
    // Returns false on invalid args (null stack, too-small stack, unsupported
    // arch). Call BEFORE spawn().
    bool init_fiber(Fiber& fiber, std::byte* stack_base, std::size_t stack_size);

    // Cooperatively run runnable fibers until none remain. Between fiber
    // switches, polls the backend to observe completions and wake waiting
    // fibers. Returns when the runnable queue is empty (a fiber whose entry
    // returned has reached `done` and is not re-enqueued).
    //
    // Single-worker: there is exactly one scheduler Context and one OS thread
    // driving it. The scheduler-liveness proof (E4-T1) holds because a fiber
    // that calls await_completion switches BACK to this scheduler context,
    // freeing it to run another runnable fiber.
    void run_until_idle();

    // The suspension primitive a fiber's entry calls. Marks the fiber waiting,
    // associates (&c -> &fiber) so a backend completion can wake it, and
    // switches to the scheduler context. Returns when the scheduler has
    // observed the completion and switched back into the fiber. The fiber
    // resumes at the exact suspension point (E4-T2 resume fidelity).
    //
    // Pre: c is outstanding (submitted via ctx.submit_*). The fiber must be
    // running (called from the fiber's entry). Post: c is ready.
    void await_completion_size(Completion<std::size_t>& c);
    void await_completion_void(Completion<void>& c);

    // Level-triggered ready-flag wait (E5-A1). Suspends the current Fiber until
    // `ready` observes true (acquire). Used by EventedWaitPolicy to await a
    // Future's persistent readiness state without coupling to the Future's
    // mutex/condition_variable (those remain the Threaded mechanism).
    //
    // Protocol (the ordering is load-bearing; do not reorder without a proof):
    //
    //   1. if ready.load(acquire): return            (R1: already ready)
    //   2. identify current Fiber (current_)
    //   3. register: &ready -> current Fiber         (in waiting_ready_)
    //   4. re-check ready.load(acquire)
    //   5. if ready: erase registration; return      (R2: raced ready)
    //   6. transition current Fiber: running -> waiting
    //   7. context switch: current Fiber -> scheduler
    //   8. (Scheduler later polls ready; on true: erase registration,
    //       waiting -> runnable, enqueue exactly once)   (R3/R4)
    //   9. resumed Fiber returns from await_ready_flag
    //
    // The protocol does NOT lock the Future's mutex. Correctness rests on
    // `ready` being PERSISTENT level-triggered state (complete_with stores it
    // with release and never clears it): the worst case is that the ready flag
    // flips true between step 5 and step 7 — but the flag stays true and the
    // Fiber is already `waiting` (set in step 6, before the switch in step 7),
    // so the Scheduler's next poll of `&ready` observes true and wakes it (R3).
    // No ephemeral wake is dropped because there is no ephemeral wake.
    //
    // Single-waiter: one Fiber per &ready in waiting_ready_ (matches Future's
    // documented single-waiter contract). Producer threads never mutate
    // Scheduler state — they only store ready=true. The Scheduler is the sole
    // mutator of its runnable queue.
    void await_ready_flag(const std::atomic<bool>& ready);

    // Diagnostics for tests.
    std::size_t runnable_count() const noexcept { return runnable_.size(); }
    std::size_t waiting_count() const noexcept {
        return waiting_size_.size() + waiting_void_.size() + waiting_ready_.size();
    }

private:
    // Poll the backend; for each newly-ready Completion, find the associated
    // waiting Fiber, transition it waiting->runnable, enqueue it, and remove
    // the association (exactly-once wake). Returns the count woken.
    std::size_t wake_ready_completions();

    // Poll registered level-triggered readiness flags (E5-A1). For each
    // (&ready, Fiber*) in waiting_ready_ whose ready now loads true (acquire),
    // erase the registration, transition the Fiber waiting->runnable, enqueue
    // it. Exactly-once: the registration is erased on wake, so a later poll
    // cannot re-enqueue. Returns the count woken.
    std::size_t wake_ready_flags();

    // Switch into the next runnable fiber. Returns when the fiber yields back
    // to the scheduler (via await_completion_* / await_ready_flag) or reaches
    // done.
    void run_next();

    AsyncIoContext& ctx_;
    fiber_ctx::Context sched_ctx_{};  // the scheduler's own saved context
    std::deque<Fiber*> runnable_{};
    // Completion* -> waiting Fiber*. Two maps because Completion<size_t> and
    // Completion<void> are distinct types; we erase both to void* keys.
    std::unordered_map<void*, Fiber*> waiting_size_{};
    std::unordered_map<void*, Fiber*> waiting_void_{};
    // Level-triggered readiness (E5-A1): &ready -> waiting Fiber*. The key is
    // the address of a persistent std::atomic<bool> (e.g. Future::ready_); it
    // is unique per waitable and stable for the waitable's lifetime (Futures
    // are non-movable). One Fiber per &ready (Future is single-waiter).
    std::unordered_map<const std::atomic<bool>*, Fiber*> waiting_ready_{};
    // The fiber currently being driven (set in run_next, cleared on return).
    // Used by await_completion_* / await_ready_flag to know which fiber to
    // mark waiting.
    Fiber* current_ = nullptr;
    bool running_ = false;
};

}  // namespace sluice::async
