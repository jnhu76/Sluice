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
// Multi-waiter / durable broadcast (D4-RM14, P0-2): a single eventfd write
// DOES wake every poller parked at that moment (Linux wakes the poll
// waitqueue), but the counter is a single CONSUMABLE token, not a notify_all:
// after the wake, do_poll() re-runs each fd's poll handler, and a poller whose
// readiness recheck finds an empty counter can go back to sleep. A FUTURE-
// generation waiter draining the counter therefore cannot be allowed to steal
// the wake of an OLD-generation waiter that was woken but has not finished its
// recheck. The wait source closes this with a generation-scoped
// register/acknowledge gate:
//   * every waiter registers (parked_count_++) atomically with its pre-park
//     drain, under mtx_;
//   * every publish (interrupt_all / signal_progress) sets
//     pending_wake_count_ = parked_count_ — the set of waiters that were
//     parked at publish time and MUST reach their recheck;
//   * a waiter that observes the epoch delta after its poll returns
//     acknowledges exactly once (pending_wake_count_--), releasing the gate
//     when the last parked-at-publish waiter acknowledges;
//   * a future-generation waiter's drain is GATED on pending_wake_count_ == 0
//     (a persistent predicate + CV notify, AGENTS.md §13.2 — no lost wake, no
//     busy-spin): it cannot consume the transport token while any old-
//     generation waiter still needs it, and it re-checks the epochs after the
//     gate so a wake that belongs to ITS invocation is reported (D4-RM13).
// The token therefore stays in the level-triggered counter until every waiter
// it was published for has rechecked, after which the next park drains it.
//
// Spurious wakes: poll(2) EINTR re-loops (epochs unchanged -> gate -> drain ->
// park again). A NON-EINTR poll(2) failure is a real wait-domain failure with
// no Result<> channel here, so it fail-fasts (stderr + terminate) instead of
// busy-spinning or fabricating a reason. POLLNVAL on either fd means the fd
// was torn down while a waiter was parked — a caller contract violation
// (parked waiters imply outstanding > 0, and quiescent destruction requires
// outstanding == 0) — fail-fast rather than busy-spin.
//
// Post-poll reason classification (control > progress > ring readiness):
// When poll(2) returns with BOTH the ring fd and the control fd readable
// (a CQE arrived in the same window as interrupt_all()/close_admission), the
// reason MUST be `interrupted`. Control is shutdown/liveness authority and
// MUST NOT be swallowed by physical ring progress: the caller (wait_one) then
// runs its final non-blocking poll/reap under access_mtx_, which reaps that
// co-ready CQE and returns its real count. Returning `progress` on a ring
// POLLIN here would re-loop (re-snapshot, re-poll) and usually still reap —
// but a concurrent consumer (another wait_one) can reap the CQE first, so the
// re-park happens against a stale token and, if a control wake also fired,
// the waiter strands. The post-poll recheck re-reads BOTH epochs under mtx_
// (the same lock interrupt_all/signal_progress publish under) in strict
// priority order: control epoch delta -> progress epoch delta -> ring POLLIN.
//
// Lock order: mtx_ is a LEAF domain; signal_progress() / interrupt_all() are
// called without holding any other lock (the backend calls signal_ready_
// progress() after reap, outside dispatch_mtx_ and the arena leaf — and the
// poison paths defer their wake past dispatch_mtx_, D4-RM16/RM17). The ONE
// exception is the D4-RM14 commit-to-park registration: arm_committed_wait()
// IS called while the Scheduler holds its global_mtx_ (MW-S2 Phase-B commit,
// via AsyncIoContext::arm_backend_wait_commit). That edge is bounded and
// acyclic — arm_committed_wait() only reads/writes the armed epoch/state
// under mtx_, never blocks, and never calls the Scheduler, user code, a
// sink, or the request lifecycle — and the reverse edge (wait-source mtx_
// -> Scheduler global_mtx_) is forbidden.
//
// The wait source is observe-only (issue #67 / D4 §21): it NEVER reaps,
// records terminals, publishes Completions, mutates RequestArena state,
// cancels operations, or changes outstanding. The context continues to own
// serialized poll/reap under access_mtx_.
#pragma once

#include <sluice/async/async_io_context.hpp>

#include <atomic>
#include <cerrno>    // errno / EINTR
#include <condition_variable>
#include <cstdio>    // fprintf / fflush / stderr
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
                std::unique_lock<std::mutex> lk(mtx_);
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
                // Announce the imminent park so a test can observe the exact
                // "empty reap done, about to block in the ring/control wait"
                // state deterministically. One-way latch; disarm by null.
                if (auto* f = wait_phase_flag_.load(std::memory_order_acquire)) {
                    f->store(true, std::memory_order_release);
                }
#endif
                // Epoch check FIRST (a bump before this point is a wake we
                // must report), then the durable-broadcast gate, then the
                // pre-park drain so the park below blocks. Any write AFTER
                // the drain belongs to a bump the next epoch check will see.
                if (control_epoch_ != observed.control_generation) {
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
                    // C2e (row 15): deterministic interrupt-vs-final-ready
                    // window (see pause_for_control_wake_final_reap_nolock_).
                    pause_for_control_wake_final_reap_nolock_();
#endif
                    return BackendWakeReason::interrupted;
                }
                if (progress_epoch_ != observed.progress_generation) {
                    return BackendWakeReason::progress;
                }
                // D4-RM15 (P0-2, durable-broadcast gate): an eventfd token
                // written by a publish is the TRANSPORT for the wake of every
                // waiter that was parked when it was published. This waiter is
                // a FUTURE generation relative to any pending token (its
                // epochs were checked above), so it must not drain the counter
                // while an old-generation waiter is still woken-but-not-
                // rechecked — draining would let that poller's readiness
                // recheck find an empty counter and re-sleep, losing the
                // interrupt. Block until every parked-at-publish waiter
                // acknowledged (persistent predicate + notify; the CV wait
                // releases mtx_, and the acknowledged waiters need only mtx_
                // to return). No lost wake (AGENTS.md §13.2).
                cv_.wait(lk, [this] { return pending_wake_count_ == 0; });
                // A publish may have landed while THIS waiter was blocked on
                // the gate: re-check the epochs BEFORE draining, so a wake
                // that belongs to this invocation is reported (D4-RM13), not
                // drained away as if it were a past event.
                if (control_epoch_ != observed.control_generation) {
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
                    // Same deterministic interrupt-vs-final-ready window as
                    // the pre-poll branch (see pause_for_control_wake_final_
                    // reap_nolock_): a control wake observed via the gate
                    // re-check must also expose the window so the final poll
                    // can be proven to reap a co-ready CQE.
                    pause_for_control_wake_final_reap_nolock_();
#endif
                    return BackendWakeReason::interrupted;
                }
                if (progress_epoch_ != observed.progress_generation) {
                    return BackendWakeReason::progress;
                }
                drain_eventfd_nolock_();
                // Register this park: the waiter is now a parked participant
                // counted by the next publish (pending_wake_count_ =
                // parked_count_), and its later acknowledgement releases the
                // gate for future-generation waiters. The registration is
                // atomic with the drain (both under mtx_), so a publish either
                // preceded it (this waiter's observed token is already fresh —
                // it parks normally) or follows it (this waiter is counted
                // and woken by the token).
                ++parked_count_;
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
                // Deterministic multi-participant park observation: count EACH
                // participant reaching the final pre-poll point (snapshot done,
                // empty serialized poll done, epochs checked, eventfd drained,
                // parked registration made, about to call poll(2)). A single
                // bool cannot prove N waiters parked; the count does. The test
                // waits for count == N using a bounded deadline ONLY as a hang
                // watchdog, then drives the control wake — no sleep is ever the
                // ordering proof (AGENTS.md §13.3). One-way latch; disarm by
                // null. Compiled out of production builds.
                if (auto* c = prepark_counter_.load(std::memory_order_acquire)) {
                    c->fetch_add(1, std::memory_order_relaxed);
                }
#endif
            }

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
            // Deterministic pre-poll barrier (one arrival per distinct parked
            // participant). The barrier blocks a participant at the physical-
            // poll boundary until released, so the SAME waiter cannot reach the
            // arrival increment twice before release — arrivals == N proves N
            // distinct participants reached the poll boundary (the prepark
            // counter alone could be inflated by a waiter retrying on EINTR).
            // Holds no lock (pure atomic spin), compiled out of production.
            // Installed before the waiter is launched; null in production.
            if (auto* g = before_physical_poll_gate_.load(
                    std::memory_order_acquire)) {
                g->arrivals.fetch_add(1, std::memory_order_acq_rel);
                while (!g->release.load(std::memory_order_acquire)) {
                    std::this_thread::yield();
                }
            }
#endif

            struct pollfd pfds[2];
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
            // Test-only ring-fd override: when non-negative, poll the override
            // fd instead of the production ring fd (so a test can make the
            // "ring" readable with a pipe/eventfd to prove the post-poll
            // control-wins classification deterministically). Installed before
            // the waiter is launched; production ring_fd_ stays set-once
            // construction state.
            const int ring_poll_fd =
                poll_ring_fd_override_.load(std::memory_order_acquire) >= 0
                    ? poll_ring_fd_override_.load(std::memory_order_acquire)
                    : ring_fd_;
#else
            const int ring_poll_fd = ring_fd_;
#endif
            pfds[0].fd = ring_poll_fd;
            pfds[0].events = POLLIN;
            pfds[0].revents = 0;
            pfds[1].fd = control_fd_;
            pfds[1].events = POLLIN;
            pfds[1].revents = 0;
            const int rc = poll_nolock_(pfds, 2, -1);
            {
                std::unique_lock<std::mutex> lk(mtx_);
                // Unregister: this waiter is no longer a parked participant
                // (the next publish will not count it).
                --parked_count_;
                if (rc < 0) {
                    // EINTR: re-check the epochs under mtx_ (a publish may
                    // have landed during the park — it must be reported, and
                    // this parked waiter acknowledges its wake exactly once),
                    // else re-loop through gate + drain + re-register. EINTR
                    // changes no state.
                    // Any OTHER poll failure is a real wait-domain failure
                    // with no Result<> channel here: fail-fast (stderr +
                    // terminate) rather than busy-spin or fabricate a reason
                    // (mutant D4-RM12).
                    if (errno != EINTR) {
                        std::fprintf(stderr,
                                     "sluice::async::detail::UringWaitSource: "
                                     "poll(2) failed with errno=%d "
                                     "(wait-domain failure)\n",
                                     errno);
                        std::fflush(stderr);
                        std::terminate();
                    }
                    if (control_epoch_ != observed.control_generation) {
                        acknowledge_parked_wake_locked_();
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
                        pause_for_control_wake_final_reap_nolock_();
#endif
                        return BackendWakeReason::interrupted;
                    }
                    if (progress_epoch_ != observed.progress_generation) {
                        acknowledge_parked_wake_locked_();
                        return BackendWakeReason::progress;
                    }
                    continue;
                }
                if ((pfds[0].revents & POLLNVAL) != 0 ||
                    (pfds[1].revents & POLLNVAL) != 0) {
                    // A parked waiter with a torn-down ring/control fd is a
                    // caller contract violation (quiescent destruction
                    // requires zero outstanding, and a parked waiter implies
                    // outstanding > 0).
                    std::fprintf(stderr,
                                 "sluice::async::detail::UringWaitSource: "
                                 "parked wait observed a closed fd (contract "
                                 "violation)\n");
                    std::fflush(stderr);
                    std::terminate();
                }
                // Post-poll reason classification (control > progress > ring
                // readiness). Re-read BOTH epochs under mtx_ (the same lock
                // interrupt_all/signal_progress publish under) so a control or
                // progress bump that landed during poll() is observed. Ring
                // POLLIN alone (no epoch delta) is progress; control ALWAYS
                // wins when both fired, because the caller's final poll reaps
                // the co-ready CQE and control is shutdown/liveness authority
                // (must not be swallowed by physical ring progress).
                //
                // An epoch delta here means a publish landed while THIS
                // waiter was parked: the waiter acknowledges the wake it was
                // counted for (pending_wake_count_ was set to the parked count
                // at publish, this waiter included), releasing the durable-
                // broadcast gate for future-generation waiters. Exactly one
                // acknowledgment per parked-at-publish waiter; waiters blocked
                // on the gate are notified when the count reaches zero.
                if (control_epoch_ != observed.control_generation) {
                    acknowledge_parked_wake_locked_();
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
                    // Same deterministic interrupt-vs-final-ready window as the
                    // pre-poll branch (see pause_for_control_wake_final_reap_
                    // nolock_): a control wake observed via post-poll recheck
                    // must also expose the window so the final poll can be
                    // proven to reap a co-ready CQE.
                    pause_for_control_wake_final_reap_nolock_();
#endif
                    return BackendWakeReason::interrupted;
                }
                if (progress_epoch_ != observed.progress_generation) {
                    acknowledge_parked_wake_locked_();
                    return BackendWakeReason::progress;
                }
                if ((pfds[0].revents & POLLIN) != 0) {
                    return BackendWakeReason::progress;
                }
            }
            // Spurious / control-fd-only wake: loop (epochs re-checked, gate
            // re-checked, drain).
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
            // D4-RM15 (P0-2): every waiter parked right now is an OLD-
            // generation waiter that this wake must reach. It is counted by
            // the durable-broadcast gate; the gate blocks future-generation
            // waiters from draining the transport token until each counted
            // waiter acknowledges its wake (see wait_for_change).
            pending_wake_count_ = parked_count_;
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
            // D4-RM15 (P0-2): same durable-broadcast gate as interrupt_all —
            // a progress token must reach every parked-at-publish waiter too.
            pending_wake_count_ = parked_count_;
        }
        wake_pollers_();
    }

    // D4-RM14 (P0-1, commit-to-park handshake): one-shot committed-wait
    // registration (see BackendWaitSource). Called by the Scheduler's MW-S2
    // Phase-B commit under global_mtx_ BEFORE the participant is exposed as
    // about-to-park; the consumed floor makes the NEXT wait_one() invocation
    // observe any control wake published after the registration, even when it
    // lands before the invocation's own snapshot (D4-RM13 invocation-begin
    // semantics). One-shot: a FUTURE invocation captures a fresh baseline, so
    // the interrupt stays one-shot.
    BackendWaitToken arm_committed_wait() noexcept override {
        std::lock_guard<std::mutex> lk(mtx_);
        armed_control_generation_ = control_epoch_;
        armed_ = true;
        return BackendWaitToken{progress_epoch_, control_epoch_};
    }
    BackendWaitToken consume_committed_wait() noexcept override {
        std::lock_guard<std::mutex> lk(mtx_);
        if (armed_) {
            armed_ = false;
            return BackendWaitToken{progress_epoch_, armed_control_generation_};
        }
        return BackendWaitToken{progress_epoch_, control_epoch_};
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
    // Deterministic pre-poll barrier: one arrival per distinct participant
    // reaching the physical-poll boundary (see wait_for_change). Blocks each
    // participant at the boundary until release, so arrivals == N proves N
    // distinct participants parked.
    struct BeforePhysicalPollPauseGate {
        std::atomic<int> arrivals{0};
        std::atomic<bool> release{false};
    };
    void set_before_physical_poll_pause_gate(BeforePhysicalPollPauseGate* gate) noexcept {
        before_physical_poll_gate_.store(gate, std::memory_order_release);
    }
    // Test-only ring-fd override (see wait_for_change): when non-negative, poll
    // this fd instead of the production ring fd. Installed before the waiter is
    // launched; production ring_fd_ stays set-once construction state.
    void set_poll_ring_fd_override_for_test(int fd) noexcept {
        poll_ring_fd_override_.store(fd, std::memory_order_release);
    }
    // Test-only poll(2) seam (allocation-free function pointer + context): when
    // installed, the wait source calls fn(pfds, nfds, timeout, ctx) instead of
    // ::poll. Used to inject a deterministic non-EINTR failure (return -1,
    // errno=EIO) so the fail-fast path is exercised without relying on an
    // invalid fd (which poll reports via revents POLLNVAL, not rc<0).
    using PollFn = int (*)(struct pollfd*, unsigned long, int, void*);
    void set_poll_fn_for_test(PollFn fn, void* ctx) noexcept {
        poll_fn_.store(fn, std::memory_order_release);
        poll_fn_ctx_.store(ctx, std::memory_order_release);
    }
    // Test-only: the live control fd (for the deterministic park probes).
    int control_fd_for_test() const noexcept { return control_fd_; }
#endif

  private:
    // Empty the eventfd counter (non-blocking; EAGAIN when already empty).
    // Must be called under mtx_ and only when pending_wake_count_ == 0 (the
    // durable-broadcast gate): every token in the counter is then stale (all
    // its parked-at-publish waiters have rechecked), so draining cannot steal
    // an old-generation wake.
    void drain_eventfd_nolock_() noexcept {
        std::uint64_t value = 0;
        while (::read(control_fd_, &value, sizeof(value)) == sizeof(value)) {
        }
    }

    // D4-RM15 (P0-2): acknowledge one parked waiter's wake (see
    // wait_for_change). Decrements the durable-broadcast gate and notifies
    // future-generation waiters blocked on it when the last parked-at-publish
    // waiter acknowledges. Must be called under mtx_, from a post-poll path
    // that observed an epoch delta — the acknowledgment of the wake this
    // waiter was counted for (pending_wake_count_ was set to the parked count
    // at publish, this waiter included).
    void acknowledge_parked_wake_locked_() noexcept {
        if (pending_wake_count_ > 0) {
            --pending_wake_count_;
            if (pending_wake_count_ == 0) {
                cv_.notify_all();
            }
        }
    }

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
    // Deterministic interrupt-vs-final-ready window (must be called under
    // mtx_, in every path that returns BackendWakeReason::interrupted). The
    // pause lets a test record the final terminal in the exact window between
    // a control wake being observed and this method returning interrupted, so
    // the context's final poll is proven to reap it (D4-M7 / D4-RM10
    // detector). Compiled out of production. No-op when no gate is installed.
    void pause_for_control_wake_final_reap_nolock_() noexcept {
        if (auto* g = control_wake_final_reap_gate_.load(
                std::memory_order_acquire)) {
            g->exited.store(false, std::memory_order_release);
            g->paused.store(true, std::memory_order_release);
            while (!g->resume.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            g->exited.store(true, std::memory_order_release);
        }
    }
#endif

    // poll(2) wrapper: calls the test-only PollFn seam when installed,
    // otherwise ::poll. nfds type matches poll(2) (nfds_t). Holds no lock.
    int poll_nolock_(struct pollfd* pfds, unsigned long nfds, int timeout) noexcept {
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
        if (auto* fn = poll_fn_.load(std::memory_order_acquire)) {
            return fn(pfds, nfds, timeout, poll_fn_ctx_.load(std::memory_order_acquire));
        }
#endif
        (void)nfds;
        return ::poll(pfds, static_cast<nfds_t>(nfds), timeout);
    }

    // Write one counter unit: level-triggered POLLIN wakes every poller parked
    // at this moment. Called AFTER the epoch was published under mtx_. The
    // token stays in the counter until every parked-at-publish waiter
    // rechecked (the durable-broadcast gate blocks draining), so a woken
    // poller's readiness recheck always finds it readable — the wake is never
    // stolen by a future-generation waiter.
    void wake_pollers_() noexcept {
        const std::uint64_t one = 1;
        (void)::write(control_fd_, &one, sizeof(one));
    }

    mutable std::mutex mtx_;
    std::condition_variable cv_;
    std::uint64_t progress_epoch_ = 0;
    std::uint64_t control_epoch_ = 0;
    // D4-RM15 (P0-2, durable-broadcast gate): parked_count_ = waiters currently
    // registered in poll; pending_wake_count_ = waiters that were parked when
    // the last wake was published and have not yet acknowledged it. All
    // guarded by mtx_.
    std::size_t parked_count_ = 0;
    std::size_t pending_wake_count_ = 0;
    // D4-RM14 (P0-1, commit-to-park handshake): one-shot armed control floor
    // (see arm_committed_wait / consume_committed_wait). Guarded by mtx_.
    std::uint64_t armed_control_generation_ = 0;
    bool armed_ = false;
    int ring_fd_ = -1;
    int control_fd_ = -1;

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
    // Deterministic wait-phase entry flag (see set_wait_phase_flag). Compiled
    // out of production builds; the layout cost in the internal-testing target
    // is accepted and documented (AGENTS.md §15).
    std::atomic<std::atomic<bool>*> wait_phase_flag_{nullptr};
    std::atomic<std::atomic<int>*> prepark_counter_{nullptr};
    std::atomic<ControlWakeFinalReapPauseGate*> control_wake_final_reap_gate_{nullptr};
    std::atomic<BeforePhysicalPollPauseGate*> before_physical_poll_gate_{nullptr};
    std::atomic<int> poll_ring_fd_override_{-1};
    std::atomic<PollFn> poll_fn_{nullptr};
    std::atomic<void*> poll_fn_ctx_{nullptr};
#endif
};

} // namespace sluice::async::detail
