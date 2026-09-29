#pragma once

#include <atomic>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cstdint>
#include <cstdio>
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
// be spurious. Waking a waiter never means a request completed.
class ProgressSource {
  public:
    struct Token {
        std::uint64_t progress = 0;
        std::uint64_t control = 0;
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
        return Token{progress_epoch_, control_epoch_};
    }

    Token arm_committed_wait() noexcept {
        std::lock_guard<std::mutex> lk(mtx_);
        armed_control_epoch_ = control_epoch_;
        armed_ = true;
        return Token{progress_epoch_, control_epoch_};
    }

    Token consume_committed_wait() noexcept {
        std::lock_guard<std::mutex> lk(mtx_);
        if (armed_) {
            armed_ = false;
            return Token{progress_epoch_, armed_control_epoch_};
        }
        return Token{progress_epoch_, control_epoch_};
    }

    WakeReason wait_for_change(Token observed) noexcept {
        return wait_for_change(observed, std::chrono::nanoseconds::max());
    }

    WakeReason wait_for_change(Token observed, std::chrono::nanoseconds max_park) noexcept {
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

            {
                std::lock_guard<std::mutex> lk(mtx_);
                readiness_fd = bound_readiness_fd_;
                if (control_epoch_ != observed.control) {
                    return WakeReason::interrupted;
                }
                if (progress_epoch_ != observed.progress) {
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
                        std::fprintf(stderr,
                                     "sluice::async::detail::ProgressSource: poll(2) failed with "
                                     "errno=%d (wait-domain failure)\n",
                                     errno);
                        std::fflush(stderr);
                        std::terminate();
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
                        if (control_epoch_ != observed.control) {
                            return WakeReason::interrupted;
                        }
                        if (progress_epoch_ != observed.progress) {
                            return WakeReason::progress;
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
            ++control_epoch_;
        }
        wake_notification_();
    }

    void signal() noexcept {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            ++progress_epoch_;
        }
        wake_notification_();
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
        bound_readiness_fd_ = fd;
    }

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
    void set_prepark_counter_for_test(std::atomic<int>* counter) noexcept {
        prepark_counter_.store(counter, std::memory_order_release);
    }
#endif

  private:
    void drain_notification_nolock_() noexcept {
        std::uint64_t value = 0;
        while (::read(notification_fd_, &value, sizeof(value)) == sizeof(value)) {
        }
    }

    void wake_notification_() noexcept {
        const std::uint64_t one = 1;
        // Saturation/coalescing keeps readiness asserted; the epoch bump under
        // the lock already guarantees the next parked recheck observes the
        // transition, so a failed write is not a lost wake.
        if (::write(notification_fd_, &one, sizeof(one)) != static_cast<ssize_t>(sizeof(one))) {
        }
    }

    mutable std::mutex mtx_;
    std::uint64_t progress_epoch_ = 0;
    std::uint64_t control_epoch_ = 0;

    std::uint64_t armed_control_epoch_ = 0;
    bool armed_ = false;

    int notification_fd_ = -1;
    int bound_readiness_fd_ = -1;

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
    std::atomic<std::atomic<int>*> prepark_counter_{nullptr};
#endif
};

// The minimum backend-facing capability: physical progress signaling and a
// poll-only readiness-fd lend. It exposes no wait, acknowledgement, arming, or
// owner-control operation, and it never lends the context notification fd.
class BackendProgressPort {
  public:
    void signal() noexcept {
        source_->signal();
    }

    void bind_physical_readiness(int fd) noexcept {
        source_->bind_physical_readiness(fd);
    }

  private:
    friend class sluice::async::AsyncIoContext;
    friend class sluice::async::AsyncBackend;
    BackendProgressPort() noexcept = default;
    explicit BackendProgressPort(ProgressSource* source) noexcept : source_(source) {}

    ProgressSource* source_ = nullptr;
};

}
