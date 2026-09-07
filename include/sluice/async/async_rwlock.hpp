#pragma once

#include <cassert>
#include <cstddef>

#include <sluice/async/detail/fail_fast.hpp>
#include <sluice/async/fiber.hpp>
#include <sluice/async/scheduler.hpp>
#include <sluice/async/wait_node.hpp>
#include <sluice/async/wait_queue.hpp>

namespace sluice::async {

class AsyncRwLock {
  public:
    explicit AsyncRwLock(Scheduler& scheduler) noexcept
        : scheduler_(scheduler), active_readers_(0), writer_active_(false),
          writer_owner_(ActorId::none()),
          expire_ctx_{&waiters_, &active_readers_, &writer_active_, &writer_owner_} {}

    ~AsyncRwLock() {
        if (active_readers_ != 0 || writer_active_) {
            assert(active_readers_ == 0 && "AsyncRwLock destroyed with active readers");
            assert(!writer_active_ && "AsyncRwLock destroyed with active writer");
            detail::async_rwlock_lifetime_fail_fast();
        }
    }

    AsyncRwLock(const AsyncRwLock&) = delete;
    AsyncRwLock& operator=(const AsyncRwLock&) = delete;
    AsyncRwLock(AsyncRwLock&&) = delete;
    AsyncRwLock& operator=(AsyncRwLock&&) = delete;

    [[nodiscard]] bool try_read_lock() {
        return scheduler_.rwlock_try_read_lock(waiters_, active_readers_, writer_active_);
    }

    void read_lock(WaitNode& node) {
        scheduler_.rwlock_read_lock(waiters_, active_readers_, writer_active_, node);
    }

    void read_lock_until(WaitNode& node, Scheduler::deadline_t deadline) {
        scheduler_.rwlock_read_lock_until(waiters_, active_readers_, writer_active_, node, deadline,
                                          &expire_ctx_);
    }

    [[nodiscard]] bool try_write_lock() {
        return scheduler_.rwlock_try_write_lock(waiters_, active_readers_, writer_active_,
                                                writer_owner_);
    }

    void write_lock(WaitNode& node) {
        scheduler_.rwlock_write_lock(waiters_, active_readers_, writer_active_, writer_owner_,
                                     node);
    }

    void write_lock_until(WaitNode& node, Scheduler::deadline_t deadline) {
        scheduler_.rwlock_write_lock_until(waiters_, active_readers_, writer_active_, writer_owner_,
                                           node, deadline, &expire_ctx_);
    }

    void unlock_read() noexcept {
        scheduler_.rwlock_unlock_read(waiters_, active_readers_, writer_active_, writer_owner_);
    }

    void unlock_write() noexcept {
        scheduler_.rwlock_unlock_write(waiters_, active_readers_, writer_active_, writer_owner_);
    }

    [[nodiscard]] bool cancel(WaitNode& node) {
        return scheduler_.rwlock_cancel(waiters_, active_readers_, writer_active_, writer_owner_,
                                        node);
    }

  private:
    friend class Scheduler;

    struct ExpireCtx {
        WaitQueue* waiters;
        std::size_t* active_readers;
        bool* writer_active;
        ActorId* writer_owner;
    };

    Scheduler& scheduler_;
    std::size_t active_readers_;
    bool writer_active_;
    ActorId writer_owner_;
    WaitQueue waiters_;
    ExpireCtx expire_ctx_;
};

} // namespace sluice::async
