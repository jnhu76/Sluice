// sluice::async::detail::ReadyWaitSource — split-phase readiness wait domain
// (AGENTS.md §13.2).
//
// Holding AsyncIoContext::access_mtx_ across ThreadPoolBackend::wait_one's
// ready-cv park would block a second participant's poll/reap (the ONLY reap
// path for a backend_ready request) forever: the final request stays
// un-reaped and ApplicationRuntime::drain never satisfies drain_complete_.
//
// This class owns the persistent ready epochs and the interrupt (control)
// epoch, the ready cv, and the BackendWaitSource capability:
//
//   - snapshot() + wait_for_change() form the lost-wake-free predicate
//     protocol: the observer snapshots BOTH epochs, the caller reaps while
//     holding the serialized backend domain, then wait_for_change parks until
//     an epoch advances. A signal between snapshot and poll is seen by poll;
//     a signal between poll and park advances the epoch so the predicate wait
//     does not park. wait_for_change NEVER reaps, publishes, or mutates any
//     request/accounting state (pure observation).
//
//   - two epochs, not one, so a wake reason can be reported without a sticky
//     interrupt flag (a sticky flag would make every FUTURE wait return
//     immediately and busy-spin the runtime while outstanding > 0):
//       * progress_generation — advanced by signal_progress() AFTER real
//         readiness is published (state first, then notify);
//       * control_generation  — advanced by interrupt_all() as a ONE-SHOT
//         re-evaluation signal (close_admission / runtime stop). Future waits
//         snapshot the advanced generation and park normally again.
//
//   - interrupt_all() unblocks ALL parked waiters (notify_all). It never
//     fabricates readiness, changes request state, publishes a Completion, or
//     cancels real I/O. signal_progress() uses notify_all too: the split
//     wait allows CONCURRENT observers (multiple parked waiters), and a
//     single notification could strand a second parker on a stale token.
//
// Lock order: the ready mutex is a LEAF domain. signal_progress() /
// interrupt_all() are called without holding any other lock, so no cycle can
// form with work_mtx_, the arena leaf lock, or AsyncIoContext::access_mtx_.
// A context-level caller may hold access_mtx_ only ACROSS the reap between
// snapshot and wait_for_change, never into the park.
#pragma once

#include <sluice/async/async_io_context.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>

namespace sluice::async::detail {

class ReadyWaitSource final : public BackendWaitSource {
  public:
    ReadyWaitSource() = default;  // the deleted copy ops suppress the implicit one
    BackendWaitToken snapshot() const noexcept override {
        std::lock_guard<std::mutex> lk(mtx_);
        return BackendWaitToken{ready_epoch_, control_epoch_};
    }

    BackendWakeReason wait_for_change(BackendWaitToken observed) noexcept override {
        // The one-argument form is the unbounded entry; the bounded
        // variant carries the deadline-driven park cap (see below).
        return wait_for_change(observed, std::chrono::nanoseconds::max());
    }

    // cv.wait_for is a native bounded transport —
    // truthfully report the capability (BackendWaitSource contract).
    bool supports_bounded_wait() const noexcept override { return true; }

    BackendWakeReason wait_for_change(BackendWaitToken observed,
                                      std::chrono::nanoseconds max_park) noexcept override {
        std::unique_lock<std::mutex> lk(mtx_);
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
        // Issue #67 seam: announce the imminent park so a test can observe the
        // exact "empty reap done, about to block in the ready wait" state
        // deterministically. One-way latch; disarmed by setting a null pointer.
        if (auto* f = wait_phase_flag_.load(std::memory_order_acquire)) {
            f->store(true, std::memory_order_release);
            // atomic::wait consumers: persistent state first, then the
            // notify — wait() re-checks the value atomically, so the
            // store+notify pair cannot lose the wake.
            f->notify_all();
        }
        // D4-RM14 (P0-1) re-entry counter: counts EVERY wait_for_change entry
        // (monotonic — no reset race). The commit-to-park detector uses it to
        // prove the run terminated and re-entered after a stop injected in the
        // commit-to-wait_one window (the second entry parks; a single entry
        // means the first wait parked THROUGH the interrupt — the mutant).
        if (auto* c = prepark_counter_.load(std::memory_order_acquire)) {
            c->fetch_add(1, std::memory_order_relaxed);
            // atomic::wait consumers: notify after the increment so a test can
            // block zero-CPU on this counter (persistent state first, then the
            // notify — wait() re-checks the value atomically).
            c->notify_all();
        }
#endif
        // The deadline-driven cap bounds the physical park so the Scheduler's
        // timer pump re-drains before an active deadline expires. The
        // unbounded sentinel (nanoseconds::max()) keeps the classic infinite
        // park.
        if (max_park == std::chrono::nanoseconds::max()) {
            ready_cv_.wait(lk, [&] {
                return ready_epoch_ != observed.progress_generation ||
                       control_epoch_ != observed.control_generation;
            });
        } else {
            ready_cv_.wait_for(lk, max_park, [&] {
                return ready_epoch_ != observed.progress_generation ||
                       control_epoch_ != observed.control_generation;
            });
        }
        // A control generation advance wins the reason (the control plane may
        // race real readiness; the caller re-polls before returning).
        if (control_epoch_ != observed.control_generation) {
            return BackendWakeReason::interrupted;
        }
        return BackendWakeReason::progress;
    }

    // Control-plane wake: unblocks ALL parked waiters so they re-evaluate
    // (close_admission / runtime stop). One-shot by construction: the bumped
    // control generation is a re-evaluation signal, NOT a persistent state, so
    // future waits snapshot it and park normally (no shutdown busy-spin).
    void interrupt_all() noexcept override {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            ++control_epoch_;
        }
        ready_cv_.notify_all();
    }

    // Commit-to-park handshake: one-shot committed-wait
    // registration. arm_committed_wait() records the current control
    // generation as the mandatory observation floor for the NEXT wait_one()
    // invocation; consume_committed_wait() hands that floor back exactly once
    // (then behaves like snapshot()). Called by the Scheduler's MW-S2 Phase-B
    // commit under global_mtx_ BEFORE the participant is exposed as about-to-
    // park, so a runtime stop landing between the commit and wait_one()'s own
    // snapshot is observed by that invocation (invocation-begin
    // semantics) instead of being rebaselined as a past event. The CV
    // predicate below is persistent (per-waiter observed token), so a future
    // waiter can never consume another waiter's wake — no transport token to
    // steal; the arm/consume only closes the pre-snapshot window.
    BackendWaitToken arm_committed_wait() noexcept override {
        std::lock_guard<std::mutex> lk(mtx_);
        armed_control_generation_ = control_epoch_;
        armed_ = true;
        return BackendWaitToken{ready_epoch_, control_epoch_};
    }
    BackendWaitToken consume_committed_wait() noexcept override {
        std::lock_guard<std::mutex> lk(mtx_);
        if (armed_) {
            armed_ = false;
            return BackendWaitToken{ready_epoch_, armed_control_generation_};
        }
        return BackendWaitToken{ready_epoch_, control_epoch_};
    }

    // Real readiness publication: the caller must have published the request
    // lifecycle state (backend_ready) FIRST; this bumps the progress
    // epoch under the mutex and notifies. notify_all (not notify_one): with
    // concurrent observers a single notification would strand a second parker
    // on a stale token (lost progress).
    void signal_progress() noexcept {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            ++ready_epoch_;
        }
        ready_cv_.notify_all();
    }

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
    void set_wait_phase_flag(std::atomic<bool>* flag) noexcept {
        wait_phase_flag_.store(flag, std::memory_order_release);
    }
    // Per-entry wait counter (see wait_for_change): counts every wait_for_change
    // entry. Observe-only; compiled out of production builds.
    void set_wait_prepark_counter(std::atomic<int>* counter) noexcept {
        prepark_counter_.store(counter, std::memory_order_release);
    }
    // Zero-CPU epoch observer for tests: blocks until the ACTUAL control/
    // progress epoch pair differs from `observed`. It parks on the SAME
    // mtx_ + ready_cv_ domain that interrupt_all()/signal_progress() use:
    // the predicate state and the park MUST share one synchronization
    // domain — a dedicated observer cv parked on epochs mutated under a
    // different mutex has a lost-wake window (the epoch advance + notify
    // lands between the observer's predicate check and its park, and the
    // notify is lost because the waiter is not yet parked). Sharing the
    // production domain makes that window impossible by cv semantics: an
    // epoch change can only happen under mtx_, which the parked observer
    // holds while checking the predicate. Non-const like wait_for_change:
    // it consumes (parks on) the cv. The predicate is the persistent epoch
    // pair — the single source of truth, no second counter. Compiled out of
    // production builds.
    void wait_epoch_changed(BackendWaitToken observed) noexcept {
        std::unique_lock<std::mutex> lk(mtx_);
        ready_cv_.wait(lk, [&] {
            return ready_epoch_ != observed.progress_generation ||
                   control_epoch_ != observed.control_generation;
        });
    }

    // Watchdog-safe epoch read for tests: a try_lock variant of snapshot()
    // for diagnostic paths that must never block behind the state they are
    // diagnosing (a case watchdog may fire while a stalled thread holds this
    // leaf domain). Returns nullopt when the domain is contended — callers
    // report "locked", they must not retry or block. Compiled out of
    // production builds.
    std::optional<BackendWaitToken> try_snapshot() const noexcept {
        std::unique_lock<std::mutex> lk(mtx_, std::try_to_lock);
        if (!lk.owns_lock()) {
            return std::nullopt;
        }
        return BackendWaitToken{ready_epoch_, control_epoch_};
    }
#endif

  private:
    mutable std::mutex mtx_;
    std::condition_variable ready_cv_;
    std::uint64_t ready_epoch_ = 0;
    std::uint64_t control_epoch_ = 0;
    // One-shot armed control floor (see arm_committed_wait /
    // consume_committed_wait). Guarded by mtx_.
    std::uint64_t armed_control_generation_ = 0;
    bool armed_ = false;

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
    // Deterministic wait-phase entry flag (see set_wait_phase_flag). Compiled
    // out of production builds; the layout cost in the internal-testing target
    // is accepted and documented (AGENTS.md §8).
    std::atomic<std::atomic<bool>*> wait_phase_flag_{nullptr};
    // D4-RM14: per-entry wait counter (see set_wait_prepark_counter).
    std::atomic<std::atomic<int>*> prepark_counter_{nullptr};
#endif
};

}  // namespace sluice::async::detail
