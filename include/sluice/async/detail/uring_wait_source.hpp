// sluice::async::detail::UringWaitSource — split-phase readiness wait domain
// for the Uring backend (Phase D4; issue #67 / AGENTS.md §13.2).
//
// AsyncIoContext::wait_one's split protocol (snapshot -> poll -> park) requires
// a backend wait source that can block for progress WITHOUT holding
// access_mtx_. For Uring the progress primitive is the KERNEL: the private
// io_uring ring fd is poll(2)-able, and POLLIN on it holds exactly while CQEs
// are pending (empirically verified on the D4 proof kernel, 6.18; empty ring
// -> poll returns 0; parked poll wakes with POLLIN exactly when a CQE is
// delivered; after reap the ring is not readable again). The control plane
// (close_admission / interrupt_backend_waiters) uses a one-shot control
// eventfd (EFD_NONBLOCK) written AFTER the control epoch is published.
//
// Lost-wake protocol (the three-window theorem, AGENTS.md §13.2):
//   * progress/control BEFORE poll            -> epoch check sees it (and the
//                                                caller's poll/reap observes
//                                                ring readability directly);
//   * progress/control BETWEEN poll and park  -> the write lands after the
//                                                pre-park drain, so the
//                                                eventfd counter is non-zero
//                                                and poll(2) returns
//                                                immediately; the epoch check
//                                                then sees the bump;
//   * progress/control AFTER park             -> the write wakes the parked
//                                                poll(2).
// The pre-park drain (non-blocking read, EAGAIN-tolerant) empties the counter
// immediately before parking, so a consumed wake can never busy-spin a future
// park; any write after the drain is a wake for a bump the waiter WILL see
// (both signal_progress() and interrupt_all() publish the epoch under mtx_
// BEFORE writing).
//
// Multi-waiter: eventfd counter semantics + level-triggered poll(2) wake ALL
// parked pollers on one write (empirically verified: one 8-byte write woke
// both pollers). Every woken waiter re-checks the epochs under mtx_; the
// pre-park drain is idempotent (EAGAIN on empty).
//
// Spurious wakes: poll(2) EINTR / a transient POLLIN just re-loop (epochs
// unchanged -> drain -> park again). POLLNVAL on either fd means the fd was
// torn down while a waiter was parked — a caller contract violation (parked
// waiters imply outstanding > 0, and quiescent destruction requires
// outstanding == 0) — fail-fast rather than busy-spin.
//
// Lock order: mtx_ is a LEAF domain; signal_progress() / interrupt_all() are
// called without holding any other lock (the backend calls signal_ready_
// progress() after reap, outside dispatch_mtx_ and the arena leaf).
//
// The wait source is observe-only (issue #67 / D4 §21): it NEVER reaps,
// records terminals, publishes Completions, mutates RequestArena state,
// cancels operations, or changes outstanding. The context continues to own
// serialized poll/reap under access_mtx_.
#pragma once

#include <sluice/async/async_io_context.hpp>

#include <atomic>
#include <cstdio>   // fprintf / fflush / stderr
#include <cstdint>
#include <exception> // std::terminate
#include <mutex>
#include <stdexcept>
#include <thread>

#include <poll.h>
#include <sys/eventfd.h>
#include <unistd.h>

namespace sluice::async::detail {

class UringWaitSource final : public BackendWaitSource {
  public:
    // Creates the control eventfd (EFD_NONBLOCK). Construction may throw
    // std::runtime_error if eventfd(2) fails. The Uring backend constructs the
    // wait source inside its ring-init try block, so a throw tears down the
    // ring and propagates: backend construction FAILS (truthful construction
    // failure — there is no silent "no wait source" capability downgrade).
    UringWaitSource() {
        control_fd_ = ::eventfd(0, EFD_NONBLOCK);
        if (control_fd_ < 0) {
            throw std::runtime_error(
                "sluice::async::detail::UringWaitSource: eventfd() failed");
        }
    }
    ~UringWaitSource() override {
        if (control_fd_ >= 0) {
            ::close(control_fd_);
            control_fd_ = -1;
        }
    }
    UringWaitSource(const UringWaitSource&) = delete;
    UringWaitSource& operator=(const UringWaitSource&) = delete;

    // Install the ring fd. Called once by the backend after io_uring_queue_init
    // succeeds, before any wait can park (no lock needed).
    void set_ring_fd(int ring_fd) noexcept { ring_fd_ = ring_fd; }

    BackendWaitToken snapshot() const noexcept override {
        std::lock_guard<std::mutex> lk(mtx_);
        return BackendWaitToken{progress_epoch_, control_epoch_};
    }

    BackendWakeReason wait_for_change(BackendWaitToken observed) noexcept override {
        for (;;) {
            {
                std::lock_guard<std::mutex> lk(mtx_);
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
                // Announce the imminent park so a test can observe the exact
                // "empty reap done, about to block in the ring/control wait"
                // state deterministically. One-way latch; disarm by null.
                if (auto* f = wait_phase_flag_.load(std::memory_order_acquire)) {
                    f->store(true, std::memory_order_release);
                }
#endif
                // Epoch check FIRST (a bump before this point is a wake we
                // must report), then drain the wake counter so the park below
                // blocks. Any write AFTER this drain belongs to a bump the
                // next epoch check will see.
                if (control_epoch_ != observed.control_generation) {
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
                    // C2e (row 15): deterministic interrupt-vs-final-ready
                    // window. The pause lets a test record the final terminal
                    // in the exact window between the control wake and the
                    // context's final poll, proving the final poll returns it
                    // (uring_c2e_interrupt_final_reap_closes_ready_race;
                    // mutant D4-M7 detector). Compiled out of production.
                    if (auto* g = control_wake_final_reap_gate_.load(
                            std::memory_order_acquire)) {
                        g->exited.store(false, std::memory_order_release);
                        g->paused.store(true, std::memory_order_release);
                        while (!g->resume.load(std::memory_order_acquire)) {
                            std::this_thread::yield();
                        }
                        g->exited.store(true, std::memory_order_release);
                    }
#endif
                    return BackendWakeReason::interrupted;
                }
                if (progress_epoch_ != observed.progress_generation) {
                    return BackendWakeReason::progress;
                }
                drain_eventfd_nolock_();
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
                // Deterministic multi-participant park observation: count EACH
                // participant reaching the final pre-poll point (snapshot done,
                // empty serialized poll done, epochs checked, eventfd drained,
                // about to call poll(2)). A single bool cannot prove N waiters
                // parked; the count does. The test waits for count == N using a
                // bounded deadline ONLY as a hang watchdog, then drives the
                // control wake — no sleep is ever the ordering proof
                // (AGENTS.md §13.3). One-way latch; disarm by null. Compiled
                // out of production builds.
                if (auto* c = prepark_counter_.load(std::memory_order_acquire)) {
                    c->fetch_add(1, std::memory_order_relaxed);
                }
#endif
            }

            struct pollfd pfds[2];
            pfds[0].fd = ring_fd_;
            pfds[0].events = POLLIN;
            pfds[0].revents = 0;
            pfds[1].fd = control_fd_;
            pfds[1].events = POLLIN;
            pfds[1].revents = 0;
            const int rc = ::poll(pfds, 2, -1);
            if (rc < 0) {
                // EINTR: re-check epochs and park again (no state changed).
                continue;
            }
            if ((pfds[0].revents & POLLNVAL) != 0 ||
                (pfds[1].revents & POLLNVAL) != 0) {
                // A parked waiter with a torn-down ring/control fd is a caller
                // contract violation (quiescent destruction requires zero
                // outstanding, and a parked waiter implies outstanding > 0).
                std::fprintf(stderr,
                             "sluice::async::detail::UringWaitSource: parked "
                             "wait observed a closed fd (contract violation)\n");
                std::fflush(stderr);
                std::terminate();
            }
            if ((pfds[0].revents & POLLIN) != 0) {
                // Kernel progress is directly observable on the ring fd: a CQE
                // is pending. Report progress so the caller re-polls and reaps
                // (the ring fd poll does not consume CQEs). Control wakes are
                // still preferred: the epoch check below may have raced a
                // control bump into this return — the caller re-polls either
                // way and the interrupted-branch final poll closes the race.
                return BackendWakeReason::progress;
            }
            // Spurious / control-fd wake: loop (epochs re-checked, drain).
        }
    }

    // Control-plane wake: unblocks ALL parked waiters so they re-evaluate
    // (close_admission / runtime stop). One-shot by construction: the bumped
    // control generation is a re-evaluation signal, NOT persistent state, so
    // future waits snapshot it and park normally (no shutdown busy-spin).
    // Never fabricates readiness, changes request state, publishes a
    // Completion, or cancels real I/O (I8).
    void interrupt_all() noexcept override {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            ++control_epoch_;
        }
        wake_pollers_();
    }

    // Real readiness publication: the caller must have published the request
    // lifecycle state (backend_ready / Completion-ready) FIRST (I4); this
    // bumps the progress epoch under the mutex and wakes all parked waiters so
    // they re-poll (notify_all equivalent; a single wake could strand a second
    // parker on a stale token — lost progress).
    void signal_progress() noexcept {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            ++progress_epoch_;
        }
        wake_pollers_();
    }

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
    void set_wait_phase_flag(std::atomic<bool>* flag) noexcept {
        wait_phase_flag_.store(flag, std::memory_order_release);
    }
    // Per-participant park counter (see wait_for_change): counts every waiter
    // reaching the final pre-poll point. Observe-only; the wait source owns no
    // lifecycle state.
    void set_wait_prepark_counter(std::atomic<int>* counter) noexcept {
        prepark_counter_.store(counter, std::memory_order_release);
    }
    // Deterministic interrupt-vs-final-ready window (see wait_for_change).
    struct ControlWakeFinalReapPauseGate {
        std::atomic<bool> paused{false};
        std::atomic<bool> resume{false};
        std::atomic<bool> exited{false};
    };
    void set_control_wake_final_reap_pause_gate(ControlWakeFinalReapPauseGate* gate) noexcept {
        control_wake_final_reap_gate_.store(gate, std::memory_order_release);
    }
    // Test-only: the live control fd (for the deterministic park probes).
    int control_fd_for_test() const noexcept { return control_fd_; }
#endif

  private:
    // Empty the eventfd counter (non-blocking; EAGAIN when already empty).
    void drain_eventfd_nolock_() noexcept {
        std::uint64_t value = 0;
        while (::read(control_fd_, &value, sizeof(value)) == sizeof(value)) {
        }
    }

    // Write one counter unit: level-triggered POLLIN wakes every parked
    // poll(2). Called AFTER the epoch was published under mtx_.
    void wake_pollers_() noexcept {
        const std::uint64_t one = 1;
        (void)::write(control_fd_, &one, sizeof(one));
    }

    mutable std::mutex mtx_;
    std::uint64_t progress_epoch_ = 0;
    std::uint64_t control_epoch_ = 0;
    int ring_fd_ = -1;
    int control_fd_ = -1;

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
    // Deterministic wait-phase entry flag (see set_wait_phase_flag). Compiled
    // out of production builds; the layout cost in the internal-testing target
    // is accepted and documented (AGENTS.md §15).
    std::atomic<std::atomic<bool>*> wait_phase_flag_{nullptr};
    std::atomic<std::atomic<int>*> prepark_counter_{nullptr};
    std::atomic<ControlWakeFinalReapPauseGate*> control_wake_final_reap_gate_{nullptr};
#endif
};

} // namespace sluice::async::detail
