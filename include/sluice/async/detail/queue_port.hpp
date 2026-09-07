























#pragma once

#include <sluice/async/detail/queue_item.hpp>
#include <sluice/async/mutex.hpp>
#include <sluice/async/timer_registration.hpp>
#include <sluice/async/wait_node.hpp>
#include <sluice/async/wait_queue.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>

namespace sluice::async {
class Scheduler;
}

namespace sluice::async::detail {




using queue_deadline_t = ::sluice::async::deadline_tick_t;






enum class QueueRole : std::uint8_t {
    producer = 0,
    consumer = 1,
};











enum class QueueOpaquePushStatus : std::uint8_t {
    committed,
    closed,
    expired,
    would_block,
};

class QueueOpaquePushResult final {
public:
    static QueueOpaquePushResult committed() noexcept {
        return QueueOpaquePushResult{QueueOpaquePushStatus::committed,
                                     QueueItemLease{}};
    }
    static QueueOpaquePushResult failed(QueueOpaquePushStatus s,
                                        QueueItemLease&& l) noexcept {


        if (s == QueueOpaquePushStatus::committed) {
            queue_lease_fail_fast();
        }
        if (!static_cast<bool>(l)) {
            queue_lease_fail_fast();
        }
        return QueueOpaquePushResult{s, std::move(l)};
    }

    QueueOpaquePushResult(QueueOpaquePushResult&& other) noexcept
        : status_(other.status_), lease_(std::move(other.lease_)) {}

    QueueOpaquePushResult& operator=(QueueOpaquePushResult&& other) noexcept {
        if (this == &other) {
            return *this;
        }



        lease_ = std::move(other.lease_);
        status_ = other.status_;
        return *this;
    }

    QueueOpaquePushResult(const QueueOpaquePushResult&) = delete;
    QueueOpaquePushResult& operator=(const QueueOpaquePushResult&) = delete;

    ~QueueOpaquePushResult() noexcept = default;

    QueueOpaquePushStatus status() const noexcept { return status_; }

    QueueItemLease take_failed_lease() && noexcept {



        return std::move(lease_);
    }

private:
    QueueOpaquePushStatus status_;
    QueueItemLease lease_;

    explicit QueueOpaquePushResult(QueueOpaquePushStatus s,
                                   QueueItemLease&& l) noexcept
        : status_(s), lease_(std::move(l)) {}

    friend class QueuePort;
    friend class QueueItemFactory;
};





enum class QueueOpaquePopStatus : std::uint8_t {
    item,
    closed,
    expired,
    would_block,
};

class QueueOpaquePopResult final {
public:
    static QueueOpaquePopResult item(QueueItemLease&& l) noexcept {
        if (!static_cast<bool>(l)) {
            queue_lease_fail_fast();
        }
        return QueueOpaquePopResult{QueueOpaquePopStatus::item, std::move(l)};
    }
    static QueueOpaquePopResult closed() noexcept {
        return QueueOpaquePopResult{QueueOpaquePopStatus::closed,
                                    QueueItemLease{}};
    }
    static QueueOpaquePopResult expired() noexcept {
        return QueueOpaquePopResult{QueueOpaquePopStatus::expired,
                                    QueueItemLease{}};
    }
    static QueueOpaquePopResult would_block() noexcept {
        return QueueOpaquePopResult{QueueOpaquePopStatus::would_block,
                                    QueueItemLease{}};
    }

    QueueOpaquePopResult(QueueOpaquePopResult&& other) noexcept
        : status_(other.status_), lease_(std::move(other.lease_)) {}

    QueueOpaquePopResult& operator=(QueueOpaquePopResult&& other) noexcept {
        if (this == &other) {
            return *this;
        }
        lease_ = std::move(other.lease_);
        status_ = other.status_;
        return *this;
    }

    QueueOpaquePopResult(const QueueOpaquePopResult&) = delete;
    QueueOpaquePopResult& operator=(const QueueOpaquePopResult&) = delete;
    ~QueueOpaquePopResult() noexcept = default;

    QueueOpaquePopStatus status() const noexcept { return status_; }

    QueueItemLease take_item_lease() && noexcept {
        return std::move(lease_);
    }

private:
    QueueOpaquePopStatus status_;
    QueueItemLease lease_;

    explicit QueueOpaquePopResult(QueueOpaquePopStatus s,
                                  QueueItemLease&& l) noexcept
        : status_(s), lease_(std::move(l)) {}

    friend class QueuePort;
    friend class QueueItemFactory;
};















class QueueItemFactory final {
public:
    template <class T, class U>
    static QueueItemLease make(QueuePort& port, U&& value) {
        static_assert(std::is_object_v<T>, "AsyncQueue<T> requires object T");
        static_assert(std::is_nothrow_move_constructible_v<T>,
                      "AsyncQueue<T> requires nothrow-move-constructible T");
        static_assert(std::is_nothrow_destructible_v<T>,
                      "AsyncQueue<T> requires nothrow-destructible T");


        Node<T>* n = new Node<T>(port, std::forward<U>(value));
        return QueueItemLease{n->control_};
    }





    template <class T>
    static T release_failed(QueuePort& port, QueueItemLease&& lease) noexcept {
        return release_typed_<T>(port, std::move(lease),
                                 QueueItemControl::Location::producer_operation,
true);
    }



    template <class T>
    static T release_popped(QueuePort& port,
                            QueueItemLease&& lease) noexcept {
        return release_typed_<T>(port, std::move(lease),
                                 QueueItemControl::Location::consumer_operation,
false);
    }



    template <class T>
    static T release_teardown(QueuePort& port,
                              QueueItemLease&& lease) noexcept {
        return release_typed_<T>(port, std::move(lease),
                                 QueueItemControl::Location::teardown,
false);
    }

private:
    static QueueItemControl make_control(QueuePort& port, void* typed_node,
                                         const void* type_token) noexcept {
        return QueueItemControl{port, typed_node, type_token};
    }

    template <class T>
    class Node final {
    private:
        template <class U>
        explicit Node(QueuePort& port, U&& value)
            : control_(QueueItemFactory::make_control(
                  port, this, queue_type_token<T>())),
              value_(std::forward<U>(value)) {}

        QueueItemControl control_;
        T value_;

        friend class QueueItemFactory;
    };




    template <class T>
    static T release_typed_(QueuePort& port, QueueItemLease&& lease,
                            QueueItemControl::Location expected,
                            bool allow_detached) noexcept {
        QueueItemControl* c = lease.release_control();
        if (c == nullptr) {
            queue_lease_fail_fast();
        }

        const bool loc_ok = (c->location_ == expected) ||
                            (allow_detached &&
                             c->location_ == QueueItemControl::Location::detached);
        if (c->owner_port_ != &port || c->type_token_ != queue_type_token<T>() ||
            !loc_ok) {
            queue_lease_fail_fast();
        }



        Node<T>* node = static_cast<Node<T>*>(c->typed_node_);


        c->location_ = QueueItemControl::Location::released;
        T value = std::move(node->value_);
        delete node;
        return value;
    }

    friend class QueueItemControl;
    friend class QueueItemLease;
    friend class QueuePort;
};






enum class QueueLifecycle : std::uint8_t {
    operational,
    tearing_down,
};

class QueueTeardownSession final {
public:
    QueueTeardownSession(QueueTeardownSession&& other) noexcept
        : port_(std::exchange(other.port_, nullptr)) {}

    QueueTeardownSession& operator=(QueueTeardownSession&&) = delete;
    QueueTeardownSession(const QueueTeardownSession&) = delete;
    QueueTeardownSession& operator=(const QueueTeardownSession&) = delete;

    ~QueueTeardownSession() noexcept;



    QueueItemLease take_next() noexcept;

    bool empty() const noexcept;

private:
    QueuePort* port_{nullptr};

    explicit QueueTeardownSession(QueuePort& port) noexcept : port_(&port) {}

    friend class QueuePort;
};









class QueuePort final {
public:
    explicit QueuePort(Scheduler& sched, std::size_t capacity);
    ~QueuePort();

    QueuePort(const QueuePort&) = delete;
    QueuePort& operator=(const QueuePort&) = delete;
    QueuePort(QueuePort&&) = delete;
    QueuePort& operator=(QueuePort&&) = delete;


    QueueOpaquePushResult try_push(QueueItemLease lease);
    QueueOpaquePopResult try_pop();
    void close() noexcept;






    bool is_closed() const noexcept;
    std::size_t capacity() const noexcept;
    std::size_t size() const noexcept;


    QueueOpaquePushResult push(QueueItemLease lease);
    QueueOpaquePushResult push_until(QueueItemLease lease,
                                     queue_deadline_t deadline);
    QueueOpaquePopResult pop();
    QueueOpaquePopResult pop_until(queue_deadline_t deadline);


    QueueTeardownSession begin_teardown() noexcept;

private:
    Scheduler& scheduler_;
    const std::size_t capacity_;



    std::unique_ptr<QueueItemLease[]> ring_;
    std::size_t ring_head_{0};
    std::size_t ring_count_{0};













    mutable Mutex state_mtx_;
    QueueLifecycle lifecycle_{QueueLifecycle::operational};





    std::atomic<bool> closed_{false};







    WaitQueue waiters_[2];







    mutable std::size_t active_port_calls_{0};
    std::size_t active_wait_associations_{0};
    std::size_t active_queue_timers_{0};
    std::size_t granted_not_resumed_{0};


    bool ring_empty_locked() const noexcept { return ring_count_ == 0; }
    bool ring_full_locked() const noexcept { return ring_count_ == capacity_; }
    std::size_t ring_slot(std::size_t logical_index) const noexcept {
        return (ring_head_ + logical_index) % capacity_;
    }
    WaitQueue& role_queue(QueueRole r) noexcept { return waiters_[static_cast<std::size_t>(r)]; }



    struct CallGuard;
    friend struct CallGuard;

    friend class QueueItemFactory;
    friend class QueueItemControl;
    friend class QueueItemLease;
    friend class QueueTeardownSession;
    friend class ::sluice::async::Scheduler;
};

}
