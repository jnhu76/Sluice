#pragma once

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <mutex>
#include <stdexcept>

#include <poll.h>
#include <sys/eventfd.h>
#include <unistd.h>

namespace sluice::async {
class AsyncBackend;
class AsyncIoContext;
}

namespace sluice::async::detail {

// Context-owned progress notification authority. A token is a notification
// epoch pair, not an outstanding-request count; notifications may coalesce or
// be spurious. Waking a waiter never means a request completed. Each epoch
// saturates instead of wrapping, and its exhaustion sequence carries freshness
// for signals that can no longer move the epoch.
class ProgressSource {
  public:
    struct Token {
        std::uint64_t progress = 0;
        std::uint64_t progress_exhaustion = 0;
    };

    enum class WakeReason : std::uint8_t { progress, interrupted, deadline, failed };

    // Observes unacknowledged control for the driver that is about to report
    // it: the current control generation is recorded as observed so a later
    // acknowledge_control() cannot retire control that arrived afterwards.
    bool observe_pending_control() noexcept {
        std::lock_guard<std::mutex> lk(mtx_);
        if (!control_pending_nolock_()) {
            return false;
        }
        observed_control_epoch_ = control_epoch_;
        observed_control_exhaustion_ = control_exhaustion_;
        return true;
    }

    // Retires exactly the control generation the owner observed, never a
    // later arrival: acknowledge copies observed into acknowledged.
    void acknowledge_control() noexcept {
        std::lock_guard<std::mutex> lk(mtx_);
        acknowledged_control_epoch_ = observed_control_epoch_;
        acknowledged_control_exhaustion_ = observed_control_exhaustion_;
    }

    // Once set, no wait may park or report idle: the notification domain
    // cannot distinguish silence from a lost wake, so every wait must observe
    // the failure instead of a zero-completion report.
    bool wait_health_failed() const noexcept {
        std::lock_guard<std::mutex> lk(mtx_);
        return health_failed_;
    }

    // Nonblocking probe of backend-physical actionable work (kernel CQ state),
    // run between the stale-readiness drain and the final revalidation. Kernel
    // producers do not execute the userspace epoch protocol, so a notification
    // consumed by the drain can only be recovered by this probe.
    using PhysicalProbe = bool (*)(void*) noexcept;

    ProgressSource() {
        notification_fd_ = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        if (notification_fd_ < 0) {
            throw std::runtime_error("sluice::async::detail::ProgressSource: eventfd() failed");
        }
    }

    ~ProgressSource() {
        if (notification_fd_ >= 0) {
            ::close(notification_fd_);
            notification_fd_ = -1;
        }
    }

    ProgressSource(const ProgressSource&) = delete;
    ProgressSource& operator=(const ProgressSource&) = delete;

    Token snapshot() const noexcept {
        std::lock_guard<std::mutex> lk(mtx_);
        return Token{progress_epoch_, progress_exhaustion_};
    }

    // Conditional park: sleeps only if no progress/control transition occurred
    // since `observed`. The epoch revalidation and the stale-readiness drain
    // share one mutex hold, so a signal racing after the check always leaves
    // kernel readiness that wakes the poll below. When `probe` is installed,
    // it runs with the mutex released after the drain; a probe that reports
    // physical work returns without parking, and the revalidation after it
    // must never drain the notification fd again — readiness observed after
    // the probe is what wakes the poll.
    WakeReason wait_if_unchanged(Token observed) noexcept {
        return wait_if_unchanged(observed, std::chrono::nanoseconds::max(), nullptr, nullptr);
    }

    WakeReason wait_if_unchanged(Token observed, std::chrono::nanoseconds max_park,
                                 PhysicalProbe probe = nullptr,
                                 void* probe_context = nullptr) noexcept {
        const bool bounded_park = max_park != std::chrono::nanoseconds::max();
        const auto now = std::chrono::steady_clock::now();
        const auto park_deadline =
            bounded_park
                ? now + std::min(
                            std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                max_park),
                            std::chrono::steady_clock::time_point::max() - now)
                : std::chrono::steady_clock::time_point{};

        for (;;) {
            {
                std::lock_guard<std::mutex> lk(mtx_);
                if (health_failed_) {
                    return WakeReason::failed;
                }
            }
            bool expired = false;
            int timeout_ms = -1;
            if (bounded_park) {
                const auto remaining =
                    std::chrono::duration_cast<std::chrono::nanoseconds>(park_deadline -
                                                                          std::chrono::steady_clock::now());
                if (remaining <= std::chrono::nanoseconds::zero()) {
                    expired = true;
                    timeout_ms = 0;
                } else {
                    const auto ms =
                        std::chrono::duration_cast<std::chrono::milliseconds>(remaining).count();
                    const auto ceil_ms =
                        ms + (remaining > std::chrono::milliseconds(ms) ? 1 : 0);
                    timeout_ms = static_cast<int>(ceil_ms < INT_MAX ? ceil_ms : INT_MAX);
                    if (timeout_ms < 0) {
                        timeout_ms = 0;
                    }
                }
            }

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
            pause_at_gate_(prerevalidate_pause_gate_);
#endif

            {
                std::lock_guard<std::mutex> lk(mtx_);
                if (control_pending_nolock_()) {
                    return WakeReason::interrupted;
                }
                if (progress_epoch_ != observed.progress ||
                    progress_exhaustion_ != observed.progress_exhaustion) {
                    return WakeReason::progress;
                }
                if (expired) {
                    return WakeReason::deadline;
                }
                drain_notification_nolock_();
            }

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
            if (auto* c = prepark_counter_.load(std::memory_order_acquire)) {
                c->fetch_add(1, std::memory_order_relaxed);
                c->notify_all();
            }
#endif

            if (probe != nullptr && probe(probe_context)) {
                return WakeReason::progress;
            }

            {
                std::lock_guard<std::mutex> lk(mtx_);
                if (control_pending_nolock_()) {
                    return WakeReason::interrupted;
                }
                if (progress_epoch_ != observed.progress ||
                    progress_exhaustion_ != observed.progress_exhaustion) {
                    return WakeReason::progress;
                }
            }

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
            pause_at_gate_(prepark_pause_gate_);
#endif

            struct pollfd pfds[1];
            pfds[0].fd = notification_fd_;
            pfds[0].events = POLLIN;
            pfds[0].revents = 0;

            const int rc = ::poll(pfds, 1, timeout_ms);
            {
                std::lock_guard<std::mutex> lk(mtx_);
                if (rc < 0) {
                    if (errno != EINTR) {
                        // Silence and a lost wake are now indistinguishable, so
                        // retrying could park forever; the failure stays sticky
                        // for the owner to observe. POLLNVAL below keeps
                        // fail-fast because it proves a lifetime violation.
                        health_failed_ = true;
                        return WakeReason::failed;
                    }
                } else {
                    if ((pfds[0].revents & POLLNVAL) != 0) {
                        std::fprintf(stderr,
                                     "sluice::async::detail::ProgressSource: parked wait observed "
                                     "a closed fd (contract violation)\n");
                        std::fflush(stderr);
                        std::terminate();
                    }
                    if ((pfds[0].revents & POLLIN) != 0) {
                        if (control_pending_nolock_()) {
                            return WakeReason::interrupted;
                        }
                        return WakeReason::progress;
                    }
                }
            }
        }
    }

    void interrupt() noexcept {
        std::lock_guard<std::mutex> lk(mtx_);
        // The bump advances the control generation so a control that
        // arrives between the owner's observation and its acknowledgement
        // stays distinguishable; stickiness is the unacknowledged gap
        // between the current and acknowledged generations.
        // Same saturation discipline as signal(): a frozen control epoch
        // lets the exhaustion sequence carry freshness. Once the pair is
        // spent it can no longer distinguish anything, so every wait on
        // it must treat control as pending instead of parking.
        if (control_epoch_ != std::numeric_limits<std::uint64_t>::max()) {
            ++control_epoch_;
        } else if (control_exhaustion_ != std::numeric_limits<std::uint64_t>::max()) {
            ++control_exhaustion_;
        }
        wake_notification_nolock_();
    }

    void signal() noexcept {
        std::lock_guard<std::mutex> lk(mtx_);
        // Saturation, not wrap: an active token must never alias a later
        // epoch. Once frozen, the exhaustion sequence carries freshness;
        // a saturated signal that skipped it would be revalidated as
        // stale and its notification drained from a parked owner.
        if (progress_epoch_ != std::numeric_limits<std::uint64_t>::max()) {
            ++progress_epoch_;
        } else if (progress_exhaustion_ != std::numeric_limits<std::uint64_t>::max()) {
            ++progress_exhaustion_;
        }
        wake_notification_nolock_();
    }

    bool exhausted() const noexcept {
        std::lock_guard<std::mutex> lk(mtx_);
        return progress_exhaustion_ != 0;
    }

    // The documented stale-readiness acknowledgement operation. Contexts and
    // hosts drain through this path only; the fd itself is never a second
    // drain authority.
    void acknowledge_notification() noexcept {
        std::lock_guard<std::mutex> lk(mtx_);
        drain_notification_nolock_();
    }

    int notification_fd() const noexcept { return notification_fd_; }

    // Only the settled owner calls this; no park or external interest may be
    // live. A later wake on the retired source is a no-op: execution is
    // closed, so no waiter exists to lose.
    void retire_notification() noexcept {
        std::lock_guard<std::mutex> lk(mtx_);
        if (notification_fd_ >= 0) {
            ::close(notification_fd_);
            notification_fd_ = -1;
        }
    }

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
    void set_prepark_counter_for_test(std::atomic<int>* counter) noexcept {
        prepark_counter_.store(counter, std::memory_order_release);
    }

    // Deterministic pause points inside the park handshake. The prerevalidate
    // gate holds before the epoch revalidation; the prepark gate holds after
    // it and after the stale-readiness drain, immediately before poll(2).
    struct PauseGate {
        std::atomic<bool> paused{false};
        std::atomic<bool> resume{false};
        std::atomic<bool> exited{true};
    };

    void set_prerevalidate_pause_gate_for_test(PauseGate* gate) noexcept {
        prerevalidate_pause_gate_.store(gate, std::memory_order_release);
    }

    void set_prepark_pause_gate_for_test(PauseGate* gate) noexcept {
        prepark_pause_gate_.store(gate, std::memory_order_release);
    }

    void set_progress_epoch_for_test(std::uint64_t epoch) noexcept {
        std::lock_guard<std::mutex> lk(mtx_);
        progress_epoch_ = epoch;
    }

    void set_control_epoch_for_test(std::uint64_t epoch) noexcept {
        std::lock_guard<std::mutex> lk(mtx_);
        control_epoch_ = epoch;
    }

    void set_control_exhaustion_for_test(std::uint64_t exhaustion) noexcept {
        std::lock_guard<std::mutex> lk(mtx_);
        control_exhaustion_ = exhaustion;
    }

    // Deterministic injection of a verdict whose production setter is a
    // non-EINTR poll(2) failure inside the park.
    void set_wait_health_failed_for_test() noexcept {
        std::lock_guard<std::mutex> lk(mtx_);
        health_failed_ = true;
    }

    // Fills the eventfd counter in one write so the next signal observes the
    // EAGAIN (already-readable) branch deterministically.
    void saturate_notification_for_test() noexcept {
        const std::uint64_t max_counter = std::numeric_limits<std::uint64_t>::max() - 1;
        for (;;) {
            errno = 0;
            const ssize_t n = ::write(notification_fd_, &max_counter, sizeof(max_counter));
            if (n == static_cast<ssize_t>(sizeof(max_counter))) {
                return;
            }
            if (n < 0 && errno == EINTR) {
                continue;
            }
            wait_domain_fail_fast_("saturate_notification_for_test", errno);
        }
    }
#endif

  private:
    static constexpr std::uint64_t kMaxEpoch = std::numeric_limits<std::uint64_t>::max();

    // A spent outer control domain freezes at the absorbing pair
    // {max, max}; that value can no longer distinguish a fresh interrupt,
    // so a wait observing it must never revalidate control as unchanged.
    static bool control_domain_spent_(std::uint64_t epoch,
                                      std::uint64_t exhaustion) noexcept {
        return epoch == kMaxEpoch && exhaustion == kMaxEpoch;
    }

    bool control_pending_nolock_() const noexcept {
        if (control_domain_spent_(control_epoch_, control_exhaustion_)) {
            return true;
        }
        return control_epoch_ != acknowledged_control_epoch_ ||
               control_exhaustion_ != acknowledged_control_exhaustion_;
    }

    [[noreturn]] static void wait_domain_fail_fast_(const char* op, int err) noexcept {
        std::fprintf(stderr,
                     "sluice::async::detail::ProgressSource: %s failed with errno=%d "
                     "(wait-domain failure)\n",
                     op, err);
        std::fflush(stderr);
        std::terminate();
    }

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
    static void pause_at_gate_(std::atomic<PauseGate*>& gate) noexcept {
        PauseGate* g = gate.load(std::memory_order_acquire);
        if (g == nullptr) {
            return;
        }
        g->exited.store(false, std::memory_order_release);
        g->paused.store(true, std::memory_order_release);
        g->paused.notify_all();
        g->resume.wait(false, std::memory_order_acquire);
        g->exited.store(true, std::memory_order_release);
        g->exited.notify_all();
    }
#endif

    void drain_notification_nolock_() noexcept {
        std::uint64_t value = 0;
        for (;;) {
            errno = 0;
            const ssize_t n = ::read(notification_fd_, &value, sizeof(value));
            if (n == static_cast<ssize_t>(sizeof(value))) {
                continue;
            }
            if (n < 0) {
                if (errno == EINTR) {
                    continue;
                }
                if (errno == EAGAIN) {
                    return;
                }
            }
            wait_domain_fail_fast_("read", errno);
        }
    }

    // Caller holds mtx_; a retired source (execution closed) has no waiter
    // that could miss the wake, so the write is skipped instead of failing.
    void wake_notification_nolock_() noexcept {
        if (notification_fd_ < 0) {
            return;
        }
        const std::uint64_t one = 1;
        for (;;) {
            errno = 0;
            const ssize_t n = ::write(notification_fd_, &one, sizeof(one));
            if (n == static_cast<ssize_t>(sizeof(one))) {
                return;
            }
            if (n < 0 && errno == EINTR) {
                continue;
            }
            // Saturation leaves read-readiness asserted, so EAGAIN is an
            // already-pending wake rather than a lost one.
            if (n < 0 && errno == EAGAIN) {
                return;
            }
            wait_domain_fail_fast_("write", errno);
        }
    }

    mutable std::mutex mtx_;
    std::uint64_t progress_epoch_ = 0;
    std::uint64_t control_epoch_ = 0;
    std::uint64_t progress_exhaustion_ = 0;
    std::uint64_t control_exhaustion_ = 0;

    // Control generations: current (advanced by interrupt), observed (recorded
    // when a wait reports control), acknowledged (advanced only by
    // acknowledge_control copying observed). Unacknowledged control is the gap
    // between current and acknowledged.
    std::uint64_t observed_control_epoch_ = 0;
    std::uint64_t observed_control_exhaustion_ = 0;
    std::uint64_t acknowledged_control_epoch_ = 0;
    std::uint64_t acknowledged_control_exhaustion_ = 0;

    bool health_failed_ = false;

    int notification_fd_ = -1;

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
    std::atomic<std::atomic<int>*> prepark_counter_{nullptr};
    std::atomic<PauseGate*> prerevalidate_pause_gate_{nullptr};
    std::atomic<PauseGate*> prepark_pause_gate_{nullptr};
#endif
};

// The minimum backend-facing capability: physical progress signaling and a
// one-shot kernel-notification registration grant. It exposes no wait,
// acknowledgement, arming, owner-control operation, and never lends the
// context notification fd itself.
class BackendProgressPort {
  public:
    // An unattached port has no waiter domain: both operations are absent
    // until the context installs the source.
    void signal() noexcept {
        if (source_ != nullptr) {
            source_->signal();
        }
    }

    // Attachment-time only: runs `install` once with the context notification
    // fd so the backend can register it as a kernel notification target (e.g.
    // io_uring_register_eventfd). The fd is borrowed for the call frame
    // alone — registration-only; the backend must not read, write, close,
    // dup, store, or otherwise retain it. Returns install's verdict.
    bool register_notification_with_kernel(bool (*install)(void* context, int fd),
                                           void* context) noexcept {
        if (source_ == nullptr || install == nullptr) {
            return false;
        }
        return install(context, source_->notification_fd());
    }

  private:
    friend class sluice::async::AsyncIoContext;
    friend class sluice::async::AsyncBackend;
    BackendProgressPort() noexcept = default;
    explicit BackendProgressPort(ProgressSource* source) noexcept : source_(source) {}

    ProgressSource* source_ = nullptr;
};

}
