#pragma once

#include <sluice/async/async_io_context.hpp>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <climits>
#include <condition_variable>
#include <cstdio>
#include <cstdint>
#include <exception>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>

#include <poll.h>
#include <sys/eventfd.h>
#include <unistd.h>

namespace sluice::async::detail {

class UringWaitSource final : public BackendWaitSource {
  public:
    UringWaitSource() {
        control_fd_ = ::eventfd(0, EFD_NONBLOCK);
        if (control_fd_ < 0) {
            throw std::runtime_error("sluice::async::detail::UringWaitSource: eventfd() failed");
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

    void set_ring_fd(int ring_fd) noexcept { ring_fd_ = ring_fd; }

    BackendWaitToken snapshot() const noexcept override {
        std::lock_guard<std::mutex> lk(mtx_);
        return BackendWaitToken{progress_epoch_, control_epoch_};
    }

    BackendWakeReason wait_for_change(BackendWaitToken observed) noexcept override {
        return wait_for_change(observed, std::chrono::nanoseconds::max());
    }

    bool supports_bounded_wait() const noexcept override { return true; }

    BackendWakeReason wait_for_change(BackendWaitToken observed,
                                      std::chrono::nanoseconds max_park) noexcept override {
        for (;;) {
            {
                std::unique_lock<std::mutex> lk(mtx_);
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)

                if (auto* f = wait_phase_flag_.load(std::memory_order_acquire)) {
                    f->store(true, std::memory_order_release);

                    f->notify_all();
                }
#endif

                if (control_epoch_ != observed.control_generation) {
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)

                    pause_for_control_wake_final_reap_nolock_();
#endif
                    return BackendWakeReason::interrupted;
                }
                if (progress_epoch_ != observed.progress_generation) {
                    return BackendWakeReason::progress;
                }

                cv_.wait(lk, [this] { return pending_wake_count_ == 0; });

                if (control_epoch_ != observed.control_generation) {
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)

                    pause_for_control_wake_final_reap_nolock_();
#endif
                    return BackendWakeReason::interrupted;
                }
                if (progress_epoch_ != observed.progress_generation) {
                    return BackendWakeReason::progress;
                }
                drain_eventfd_nolock_();

                ++parked_count_;
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)

                if (auto* c = prepark_counter_.load(std::memory_order_acquire)) {
                    c->fetch_add(1, std::memory_order_relaxed);

                    c->notify_all();
                }
#endif
            }

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)

            if (auto* g = before_physical_poll_gate_.load(std::memory_order_acquire)) {
                g->arrivals.fetch_add(1, std::memory_order_acq_rel);
                while (!g->release.load(std::memory_order_acquire)) {
                    std::this_thread::yield();
                }
            }
#endif

            struct pollfd pfds[2];
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)

            const int ring_poll_fd = poll_ring_fd_override_.load(std::memory_order_acquire) >= 0
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

            int timeout_ms = -1;
            if (max_park != std::chrono::nanoseconds::max()) {
                auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(max_park).count();
                if (ms < 0) {
                    ms = 0;
                }
                if (ms > INT_MAX) {
                    ms = INT_MAX;
                }
                timeout_ms = static_cast<int>(ms);
            }
            const int rc = poll_nolock_(pfds, 2, timeout_ms);
            {
                std::unique_lock<std::mutex> lk(mtx_);

                --parked_count_;
                if (rc < 0) {
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
                if ((pfds[0].revents & POLLNVAL) != 0 || (pfds[1].revents & POLLNVAL) != 0) {
                    std::fprintf(stderr, "sluice::async::detail::UringWaitSource: "
                                         "parked wait observed a closed fd (contract "
                                         "violation)\n");
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
                if ((pfds[0].revents & POLLIN) != 0) {
                    return BackendWakeReason::progress;
                }
            }
        }
    }

    void interrupt_all() noexcept override {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            ++control_epoch_;

            pending_wake_count_ = parked_count_;
        }
        wake_pollers_();
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)

        cv_.notify_all();
#endif
    }

    void signal_progress() noexcept {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            ++progress_epoch_;

            pending_wake_count_ = parked_count_;
        }
        wake_pollers_();
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)

        cv_.notify_all();
#endif
    }

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

    void set_wait_prepark_counter(std::atomic<int>* counter) noexcept {
        prepark_counter_.store(counter, std::memory_order_release);
    }

    struct ControlWakeFinalReapPauseGate {
        std::atomic<bool> paused{false};
        std::atomic<bool> resume{false};
        std::atomic<bool> exited{false};
    };
    void set_control_wake_final_reap_pause_gate(ControlWakeFinalReapPauseGate* gate) noexcept {
        control_wake_final_reap_gate_.store(gate, std::memory_order_release);
    }

    struct BeforePhysicalPollPauseGate {
        std::atomic<int> arrivals{0};
        std::atomic<bool> release{false};
    };
    void set_before_physical_poll_pause_gate(BeforePhysicalPollPauseGate* gate) noexcept {
        before_physical_poll_gate_.store(gate, std::memory_order_release);
    }

    void set_poll_ring_fd_override_for_test(int fd) noexcept {
        poll_ring_fd_override_.store(fd, std::memory_order_release);
    }

    using PollFn = int (*)(struct pollfd*, unsigned long, int, void*);
    void set_poll_fn_for_test(PollFn fn, void* ctx) noexcept {
        poll_fn_.store(fn, std::memory_order_release);
        poll_fn_ctx_.store(ctx, std::memory_order_release);
    }

    int control_fd_for_test() const noexcept { return control_fd_; }

    void wait_epoch_changed(BackendWaitToken observed) noexcept {
        std::unique_lock<std::mutex> lk(mtx_);
        cv_.wait(lk, [&] {
            return progress_epoch_ != observed.progress_generation ||
                   control_epoch_ != observed.control_generation;
        });
    }

    std::optional<BackendWaitToken> try_snapshot() const noexcept {
        std::unique_lock<std::mutex> lk(mtx_, std::try_to_lock);
        if (!lk.owns_lock()) {
            return std::nullopt;
        }
        return BackendWaitToken{progress_epoch_, control_epoch_};
    }
#endif

  private:
    void drain_eventfd_nolock_() noexcept {
        std::uint64_t value = 0;
        while (::read(control_fd_, &value, sizeof(value)) == sizeof(value)) {}
    }

    void acknowledge_parked_wake_locked_() noexcept {
        if (pending_wake_count_ > 0) {
            --pending_wake_count_;
            if (pending_wake_count_ == 0) {
                cv_.notify_all();
            }
        }
    }

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)

    void pause_for_control_wake_final_reap_nolock_() noexcept {
        if (auto* g = control_wake_final_reap_gate_.load(std::memory_order_acquire)) {
            g->exited.store(false, std::memory_order_release);
            g->paused.store(true, std::memory_order_release);

            g->paused.notify_all();
            while (!g->resume.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            g->exited.store(true, std::memory_order_release);
        }
    }
#endif

    int poll_nolock_(struct pollfd* pfds, unsigned long nfds, int timeout) noexcept {
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
        if (auto* fn = poll_fn_.load(std::memory_order_acquire)) {
            return fn(pfds, nfds, timeout, poll_fn_ctx_.load(std::memory_order_acquire));
        }
#endif
        (void)nfds;
        return ::poll(pfds, static_cast<nfds_t>(nfds), timeout);
    }

    void wake_pollers_() noexcept {
        const std::uint64_t one = 1;
        // Control wake is best-effort: pollers may already be awake or about
        // to observe the epoch directly.
        if (::write(control_fd_, &one, sizeof(one)) != static_cast<ssize_t>(sizeof(one))) {
        }
    }

    mutable std::mutex mtx_;
    std::condition_variable cv_;
    std::uint64_t progress_epoch_ = 0;
    std::uint64_t control_epoch_ = 0;

    std::size_t parked_count_ = 0;
    std::size_t pending_wake_count_ = 0;

    std::uint64_t armed_control_generation_ = 0;
    bool armed_ = false;
    int ring_fd_ = -1;
    int control_fd_ = -1;

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)

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
