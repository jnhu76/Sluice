// sluice::async::detail::ReadyWaitSource — split-phase readiness wait domain
// (issue #67 / AGENTS.md §13.2).
//
// The pre-fix AsyncIoContext::wait_one held access_mtx_ across
// ThreadPoolBackend::wait_one's ready-cv park, so a second participant's
// poll/reap (the ONLY reap path for a backend_ready request) blocked forever,
// the final request stayed un-reaped, and ApplicationRuntime::drain never
// satisfied drain_complete_.
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
//     request/accounting state (pure observation — I3).
//
//   - two epochs, not one, so a wake reason can be reported without a sticky
//     interrupt flag (a sticky flag would make every FUTURE wait return
//     immediately and busy-spin the runtime while outstanding > 0):
//       * progress_generation — advanced by signal_progress() AFTER real
//         readiness is published (state first, then notify — I4);
//       * control_generation  — advanced by interrupt_all() as a ONE-SHOT
//         re-evaluation signal (close_admission / runtime stop). Future waits
//         snapshot the advanced generation and park normally again.
//
//   - interrupt_all() unblocks ALL parked waiters (notify_all — I6). It never
//     fabricates readiness, changes request state, publishes a Completion, or
//     cancels real I/O (I8). signal_progress() uses notify_all too: the split
//     wait allows CONCURRENT observers (multiple parked waiters), and a
//     single notification could strand a second parker on a stale token.
//
// Lock order: the ready mutex is a LEAF domain. signal_progress() /
// interrupt_all() are called without holding any other lock, so no cycle can
// form with work_mtx_, the arena leaf lock, or AsyncIoContext::access_mtx_.
// A context-level caller may hold access_mtx_ only ACROSS the reap between
// snapshot and wait_for_change, never into the park (I1).
#pragma once

#include <sluice/async/async_io_context.hpp>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>

namespace sluice::async::detail {

class ReadyWaitSource final : public BackendWaitSource {
  public:
    ReadyWaitSource() = default;  // the deleted copy ops suppress the implicit one
    BackendWaitToken snapshot() const noexcept override {
        std::lock_guard<std::mutex> lk(mtx_);
        return BackendWaitToken{ready_epoch_, control_epoch_};
    }

    BackendWakeReason wait_for_change(BackendWaitToken observed) noexcept override {
        std::unique_lock<std::mutex> lk(mtx_);
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
        // Issue #67 seam: announce the imminent park so a test can observe the
        // exact "empty reap done, about to block in the ready wait" state
        // deterministically. One-way latch; disarmed by setting a null pointer.
        if (auto* f = wait_phase_flag_.load(std::memory_order_acquire)) {
            f->store(true, std::memory_order_release);
        }
#endif
        ready_cv_.wait(lk, [&] {
            return ready_epoch_ != observed.progress_generation ||
                   control_epoch_ != observed.control_generation;
        });
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

    // Real readiness publication: the caller must have published the request
    // lifecycle state (backend_ready) FIRST (I4); this bumps the progress
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
#endif

  private:
    mutable std::mutex mtx_;
    std::condition_variable ready_cv_;
    std::uint64_t ready_epoch_ = 0;
    std::uint64_t control_epoch_ = 0;

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
    // Deterministic wait-phase entry flag (see set_wait_phase_flag). Compiled
    // out of production builds; the layout cost in the internal-testing target
    // is accepted and documented (AGENTS.md §8).
    std::atomic<std::atomic<bool>*> wait_phase_flag_{nullptr};
#endif
};

}  // namespace sluice::async::detail
