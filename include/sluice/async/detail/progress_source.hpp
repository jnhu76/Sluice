#pragma once

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
        std::uint64_t control = 0;
        std::uint64_t progress_exhaustion = 0;
        std::uint64_t control_exhaustion = 0;
    };

    enum class WakeReason : std::uint8_t { progress, interrupted };

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
        return Token{progress_epoch_, control_epoch_, progress_exhaustion_, control_exhaustion_};
    }

    Token arm_committed_wait() noexcept {
        std::lock_guard<std::mutex> lk(mtx_);
        armed_control_epoch_ = control_epoch_;
        armed_control_exhaustion_ = control_exhaustion_;
        armed_ = true;
        return Token{progress_epoch_, control_epoch_, progress_exhaustion_, control_exhaustion_};
    }

    Token consume_committed_wait() noexcept {
        std::lock_guard<std::mutex> lk(mtx_);
        if (armed_) {
            armed_ = false;
            return Token{progress_epoch_, armed_control_epoch_, progress_exhaustion_,
                         armed_control_exhaustion_};
        }
        return Token{progress_epoch_, control_epoch_, progress_exhaustion_, control_exhaustion_};
    }

    // Conditional park: sleeps only if no progress/control transition occurred
    // since `observed`. The epoch revalidation and the stale-readiness drain
    // share one mutex hold, so a signal racing after the check always leaves
    // kernel readiness that wakes the poll below.
    WakeReason wait_if_unchanged(Token observed) noexcept {
        return wait_if_unchanged(observed, std::chrono::nanoseconds::max());
    }

    WakeReason wait_if_unchanged(Token observed, std::chrono::nanoseconds max_park) noexcept {
        const bool bounded_park = max_park != std::chrono::nanoseconds::max();
        const auto park_deadline = bounded_park
                                       ? std::chrono::steady_clock::now() + max_park
                                       : std::chrono::steady_clock::time_point{};

        for (;;) {
            int readiness_fd = -1;
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
                readiness_fd = bound_readiness_fd_;
                if (control_changed_nolock_(observed)) {
                    return WakeReason::interrupted;
                }
                if (progress_epoch_ != observed.progress ||
                    progress_exhaustion_ != observed.progress_exhaustion) {
                    return WakeReason::progress;
                }
                if (expired) {
                    return WakeReason::interrupted;
                }
                drain_notification_nolock_();
            }

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
            if (auto* c = prepark_counter_.load(std::memory_order_acquire)) {
                c->fetch_add(1, std::memory_order_relaxed);
                c->notify_all();
            }
            pause_at_gate_(prepark_pause_gate_);
#endif

            struct pollfd pfds[2];
            unsigned long nfds = 0;
            pfds[nfds].fd = notification_fd_;
            pfds[nfds].events = POLLIN;
            pfds[nfds].revents = 0;
            ++nfds;
            if (readiness_fd >= 0) {
                pfds[nfds].fd = readiness_fd;
                pfds[nfds].events = POLLIN;
                pfds[nfds].revents = 0;
                ++nfds;
            }

            const int rc = ::poll(pfds, nfds, timeout_ms);
            {
                std::lock_guard<std::mutex> lk(mtx_);
                if (rc < 0) {
                    if (errno != EINTR) {
                        wait_domain_fail_fast_("poll(2)", errno);
                    }
                } else {
                    if ((pfds[0].revents & POLLNVAL) != 0 ||
                        (nfds > 1 && (pfds[1].revents & POLLNVAL) != 0)) {
                        std::fprintf(stderr,
                                     "sluice::async::detail::ProgressSource: parked wait observed "
                                     "a closed fd (contract violation)\n");
                        std::fflush(stderr);
                        std::terminate();
                    }
                    if ((pfds[0].revents & POLLIN) != 0 ||
                        (nfds > 1 && (pfds[1].revents & POLLIN) != 0)) {
                        if (control_changed_nolock_(observed)) {
                            return WakeReason::interrupted;
                        }
                        return WakeReason::progress;
                    }
                }
            }
        }
    }

    void interrupt() noexcept {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            // Same saturation discipline as signal(): a frozen control epoch
            // must stay sticky and let the exhaustion sequence distinguish a
            // fresh interrupt from the observed token. Once the exhaustion
            // sequence is spent too, the pair freezes for good: it can no
            // longer distinguish anything, so every wait on it must treat
            // control as pending instead of parking on a reusable value.
            if (control_epoch_ != std::numeric_limits<std::uint64_t>::max()) {
                ++control_epoch_;
            } else if (control_exhaustion_ != std::numeric_limits<std::uint64_t>::max()) {
                ++control_exhaustion_;
            }
        }
        wake_notification_();
    }

    void signal() noexcept {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            // Saturation, not wrap: an active token must never alias a later
            // epoch. Once frozen, the exhaustion sequence carries freshness;
            // a saturated signal that skipped it would be revalidated as
            // stale and its notification drained from a parked owner.
            if (progress_epoch_ == std::numeric_limits<std::uint64_t>::max()) {
                ++progress_exhaustion_;
            } else {
                ++progress_epoch_;
            }
        }
        wake_notification_();
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

    void bind_physical_readiness(int fd) noexcept {
        std::lock_guard<std::mutex> lk(mtx_);
        if (bound_readiness_fd_ != -1) {
            wait_domain_fail_fast_("bind_physical_readiness", EEXIST);
        }
        bound_readiness_fd_ = fd;
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

    bool control_changed_nolock_(const Token& observed) const noexcept {
        if (control_domain_spent_(observed.control, observed.control_exhaustion) ||
            control_domain_spent_(control_epoch_, control_exhaustion_)) {
            return true;
        }
        return control_epoch_ != observed.control ||
               control_exhaustion_ != observed.control_exhaustion;
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

    void wake_notification_() noexcept {
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

    std::uint64_t armed_control_epoch_ = 0;
    std::uint64_t armed_control_exhaustion_ = 0;
    bool armed_ = false;

    int notification_fd_ = -1;
    int bound_readiness_fd_ = -1;

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
    std::atomic<std::atomic<int>*> prepark_counter_{nullptr};
    std::atomic<PauseGate*> prerevalidate_pause_gate_{nullptr};
    std::atomic<PauseGate*> prepark_pause_gate_{nullptr};
#endif
};

// The minimum backend-facing capability: physical progress signaling and a
// poll-only readiness-fd lend. It exposes no wait, acknowledgement, arming, or
// owner-control operation, and it never lends the context notification fd.
class BackendProgressPort {
  public:
    // An unattached port has no waiter domain: both operations are absent
    // until the context installs the source.
    void signal() noexcept {
        if (source_ != nullptr) {
            source_->signal();
        }
    }

    void bind_physical_readiness(int fd) noexcept {
        if (source_ != nullptr) {
            source_->bind_physical_readiness(fd);
        }
    }

  private:
    friend class sluice::async::AsyncIoContext;
    friend class sluice::async::AsyncBackend;
    BackendProgressPort() noexcept = default;
    explicit BackendProgressPort(ProgressSource* source) noexcept : source_(source) {}

    ProgressSource* source_ = nullptr;
};

}
