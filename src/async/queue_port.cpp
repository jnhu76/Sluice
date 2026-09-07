#include <sluice/async/detail/queue_item.hpp>
#include <sluice/async/detail/queue_port.hpp>
#include <sluice/async/detail/queue_test_seam.hpp>
#include <sluice/async/lock_guard.hpp>
#include <sluice/async/scheduler.hpp>

#include "queue_detail.hpp"

#include <cstdlib>
#include <exception>
#include <new>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace sluice::async::detail {

[[noreturn]] void queue_lease_fail_fast() noexcept {
    std::terminate();
}

void QueueItemLease::require_empty_or_terminate() const noexcept {
    if (control_ != nullptr) {
        queue_lease_fail_fast();
    }
}

void QueueItemLease::adopt_control(QueueItemControl& control) noexcept {
    require_empty_or_terminate();
    control_ = &control;
}

QueueItemLease::~QueueItemLease() noexcept {
    if (control_ != nullptr) {
        queue_lease_fail_fast();
    }
}

struct QueuePort::CallGuard final {
    struct adopt_tag {};

    CallGuard(const QueuePort& port, adopt_tag) noexcept : port_(&port) {}
    ~CallGuard() noexcept {
        if (port_ != nullptr) {
            LockGuard glk(port_->scheduler_.global_mtx_);
            LockGuard lk(port_->state_mtx_);
            --port_->active_port_calls_;
        }
    }
    CallGuard(const CallGuard&) = delete;
    CallGuard& operator=(const CallGuard&) = delete;
    CallGuard(CallGuard&&) = delete;
    CallGuard& operator=(CallGuard&&) = delete;

  private:
    const QueuePort* port_;
};

QueuePort::QueuePort(Scheduler& sched, std::size_t capacity)
    : scheduler_(sched), capacity_(capacity) {
    if (capacity_ == 0) {
        throw std::invalid_argument("sluice::async::AsyncQueue capacity must be >= 1");
    }

    ring_ = std::unique_ptr<QueueItemLease[]>(new QueueItemLease[capacity_]());
}

QueuePort::~QueuePort() {
    if (ring_count_ != 0) {
        queue_lease_fail_fast();
    }
}

bool QueuePort::is_closed() const noexcept {
    {
        LockGuard glk(scheduler_.global_mtx_);
        LockGuard lk(state_mtx_);
        if (lifecycle_ != QueueLifecycle::operational) {
            queue_lease_fail_fast();
        }
        ++active_port_calls_;
    }
    CallGuard guard(*this, CallGuard::adopt_tag{});
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
    maybe_pause_queue_snapshot();
#endif

    return closed_.load(std::memory_order::acquire);
}

std::size_t QueuePort::capacity() const noexcept {
    {
        LockGuard glk(scheduler_.global_mtx_);
        LockGuard lk(state_mtx_);
        if (lifecycle_ != QueueLifecycle::operational) {
            queue_lease_fail_fast();
        }
        ++active_port_calls_;
    }
    CallGuard guard(*this, CallGuard::adopt_tag{});
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
    maybe_pause_queue_snapshot();
#endif
    return capacity_;
}

std::size_t QueuePort::size() const noexcept {
    {
        LockGuard glk(scheduler_.global_mtx_);
        LockGuard lk(state_mtx_);
        if (lifecycle_ != QueueLifecycle::operational) {
            queue_lease_fail_fast();
        }
        ++active_port_calls_;
    }
    CallGuard guard(*this, CallGuard::adopt_tag{});
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
    maybe_pause_queue_snapshot();
#endif

    LockGuard lk(state_mtx_);
    return ring_count_;
}

QueueOpaquePushResult QueuePort::try_push(QueueItemLease lease) {
    {
        LockGuard glk(scheduler_.global_mtx_);
        LockGuard lk(state_mtx_);
        if (lifecycle_ != QueueLifecycle::operational) {
            queue_lease_fail_fast();
        }
        ++active_port_calls_;
    }
    CallGuard guard(*this, CallGuard::adopt_tag{});

    QueueItemControl* c = lease.control_;
    if (c == nullptr || c->owner_port_ != this ||
        c->location_ != QueueItemControl::Location::detached) {
        queue_lease_fail_fast();
    }

    c->location_ = QueueItemControl::Location::producer_operation;

    LockGuard glk(scheduler_.global_mtx_);
    LockGuard lk(state_mtx_);

    if (closed_) {
        c->location_ = QueueItemControl::Location::detached;
        return QueueOpaquePushResult::failed(QueueOpaquePushStatus::closed, std::move(lease));
    }

    if (ring_full_locked()) {
        c->location_ = QueueItemControl::Location::detached;
        return QueueOpaquePushResult::failed(QueueOpaquePushStatus::would_block, std::move(lease));
    }

    const std::size_t tail = ring_slot(ring_count_);
    c->location_ = QueueItemControl::Location::ring;
    ring_[tail] = std::move(lease);
    ++ring_count_;

    (void)scheduler_.queue_grant_consumer_locked(*this);
    return QueueOpaquePushResult::committed();
}

QueueOpaquePopResult QueuePort::try_pop() {
    {
        LockGuard glk(scheduler_.global_mtx_);
        LockGuard lk(state_mtx_);
        if (lifecycle_ != QueueLifecycle::operational) {
            queue_lease_fail_fast();
        }
        ++active_port_calls_;
    }
    CallGuard guard(*this, CallGuard::adopt_tag{});

    LockGuard glk(scheduler_.global_mtx_);
    LockGuard lk(state_mtx_);

    if (!ring_empty_locked()) {
        const std::size_t head = ring_head_;

        QueueItemLease out = std::move(ring_[head]);
        ring_head_ = (ring_head_ + 1) % capacity_;
        --ring_count_;

        out.control_->location_ = QueueItemControl::Location::consumer_operation;

        (void)scheduler_.queue_grant_producer_locked(*this);
        return QueueOpaquePopResult::item(std::move(out));
    }

    if (closed_) {
        return QueueOpaquePopResult::closed();
    }

    return QueueOpaquePopResult::would_block();
}

void QueuePort::close() noexcept {
    {
        LockGuard glk(scheduler_.global_mtx_);
        LockGuard lk(state_mtx_);
        if (lifecycle_ != QueueLifecycle::operational) {
            queue_lease_fail_fast();
        }
        ++active_port_calls_;
    }
    CallGuard guard(*this, CallGuard::adopt_tag{});

    LockGuard glk(scheduler_.global_mtx_);
    LockGuard lk(state_mtx_);

    closed_.store(true, std::memory_order::release);

    while (scheduler_.queue_grant_consumer_locked(*this) != nullptr) {}
    while (scheduler_.queue_grant_producer_locked(*this) != nullptr) {}
}

QueueOpaquePushResult QueuePort::push(QueueItemLease lease) {
    {
        LockGuard glk(scheduler_.global_mtx_);
        LockGuard lk(state_mtx_);
        if (lifecycle_ != QueueLifecycle::operational) {
            queue_lease_fail_fast();
        }
        ++active_port_calls_;
    }
    CallGuard guard(*this, CallGuard::adopt_tag{});
    QueueItemControl* c = lease.control_;
    if (c == nullptr || c->owner_port_ != this ||
        c->location_ != QueueItemControl::Location::detached) {
        queue_lease_fail_fast();
    }
    c->location_ = QueueItemControl::Location::producer_operation;
    WaitNode node;
    scheduler_.queue_push_admit(*this, node, lease);

    if (lease.control_ == nullptr) {
        return QueueOpaquePushResult::committed();
    }

    return QueueOpaquePushResult::failed(QueueOpaquePushStatus::closed, std::move(lease));
}

QueueOpaquePushResult QueuePort::push_until(QueueItemLease lease, queue_deadline_t deadline) {
    {
        LockGuard glk(scheduler_.global_mtx_);
        LockGuard lk(state_mtx_);
        if (lifecycle_ != QueueLifecycle::operational) {
            queue_lease_fail_fast();
        }
        ++active_port_calls_;
    }
    CallGuard guard(*this, CallGuard::adopt_tag{});
    QueueItemControl* c = lease.control_;
    if (c == nullptr || c->owner_port_ != this ||
        c->location_ != QueueItemControl::Location::detached) {
        queue_lease_fail_fast();
    }
    c->location_ = QueueItemControl::Location::producer_operation;
    WaitNode node;
    scheduler_.queue_push_admit_until(*this, node, lease, deadline);
    if (lease.control_ == nullptr) {
        return QueueOpaquePushResult::committed();
    }
    const bool expired = node.was_expired();
    return QueueOpaquePushResult::failed(
        expired ? QueueOpaquePushStatus::expired : QueueOpaquePushStatus::closed, std::move(lease));
}

QueueOpaquePopResult QueuePort::pop() {
    {
        LockGuard glk(scheduler_.global_mtx_);
        LockGuard lk(state_mtx_);
        if (lifecycle_ != QueueLifecycle::operational) {
            queue_lease_fail_fast();
        }
        ++active_port_calls_;
    }
    CallGuard guard(*this, CallGuard::adopt_tag{});
    QueueItemLease out;
    WaitNode node;
    scheduler_.queue_pop_admit(*this, node, out);
    if (out.control_ != nullptr) {
        return QueueOpaquePopResult::item(std::move(out));
    }
    return QueueOpaquePopResult::closed();
}

QueueOpaquePopResult QueuePort::pop_until(queue_deadline_t deadline) {
    {
        LockGuard glk(scheduler_.global_mtx_);
        LockGuard lk(state_mtx_);
        if (lifecycle_ != QueueLifecycle::operational) {
            queue_lease_fail_fast();
        }
        ++active_port_calls_;
    }
    CallGuard guard(*this, CallGuard::adopt_tag{});
    QueueItemLease out;
    WaitNode node;
    scheduler_.queue_pop_admit_until(*this, node, out, deadline);
    if (out.control_ != nullptr) {
        return QueueOpaquePopResult::item(std::move(out));
    }
    const bool expired = node.was_expired();
    return expired ? QueueOpaquePopResult::expired() : QueueOpaquePopResult::closed();
}

QueueTeardownSession QueuePort::begin_teardown() noexcept {
    LockGuard glk(scheduler_.global_mtx_);
    LockGuard lk(state_mtx_);

    if (lifecycle_ != QueueLifecycle::operational || active_port_calls_ != 0 ||
        active_wait_associations_ != 0 || active_queue_timers_ != 0 || granted_not_resumed_ != 0 ||
        !scheduler_.queue_role_waiters_empty_locked(*this)) {
        queue_lease_fail_fast();
    }

    lifecycle_ = QueueLifecycle::tearing_down;
    return QueueTeardownSession{*this};
}

QueueItemLease QueueTeardownSession::take_next() noexcept {
    if (port_ == nullptr) {
        queue_lease_fail_fast();
    }
    LockGuard lk(port_->state_mtx_);
    if (port_->lifecycle_ != QueueLifecycle::tearing_down) {
        queue_lease_fail_fast();
    }

    if (port_->ring_empty_locked()) {
        return QueueItemLease{};
    }
    const std::size_t head = port_->ring_head_;
    QueueItemLease out = std::move(port_->ring_[head]);
    port_->ring_head_ = (port_->ring_head_ + 1) % port_->capacity_;
    --port_->ring_count_;
    out.control_->location_ = QueueItemControl::Location::teardown;
    return out;
}

bool QueueTeardownSession::empty() const noexcept {
    if (port_ == nullptr) {
        return true;
    }
    LockGuard lk(port_->state_mtx_);
    return port_->ring_empty_locked();
}

QueueTeardownSession::~QueueTeardownSession() noexcept {
    if (port_ == nullptr) {
        return;
    }
    LockGuard lk(port_->state_mtx_);
    if (!port_->ring_empty_locked()) {
        queue_lease_fail_fast();
    }
}

} // namespace sluice::async::detail
