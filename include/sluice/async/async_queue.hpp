#pragma once

#include <cstddef>
#include <new>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>

#include <sluice/async/detail/queue_port.hpp>
#include <sluice/async/scheduler.hpp>

namespace sluice::async {

using QueuePushStatus = detail::QueueOpaquePushStatus;
using QueuePopStatus = detail::QueueOpaquePopStatus;

template <class T> class QueuePushResult final {
  public:
    static QueuePushResult committed() noexcept {
        return QueuePushResult{QueuePushStatus::committed, std::nullopt};
    }

    static QueuePushResult failed(QueuePushStatus s, T&& value) noexcept {
        if (s == QueuePushStatus::committed) {
            detail::queue_lease_fail_fast();
        }
        return QueuePushResult{s, std::move(value)};
    }

    QueuePushResult(QueuePushResult&&) noexcept = default;

    QueuePushResult& operator=(QueuePushResult&& other) noexcept {
        if (this == &other)
            return *this;
        value_.reset();
        status_ = other.status_;
        if (other.value_.has_value()) {
            value_.emplace(std::move(*other.value_));
            other.value_.reset();
        }
        return *this;
    }
    QueuePushResult(const QueuePushResult&) = delete;
    QueuePushResult& operator=(const QueuePushResult&) = delete;
    ~QueuePushResult() = default;

    QueuePushStatus status() const noexcept { return status_; }

    T take_value() && noexcept {
        if (!value_.has_value()) {
            detail::queue_lease_fail_fast();
        }
        return std::move(*value_);
    }

  private:
    QueuePushResult(QueuePushStatus s, std::optional<T>&& v) noexcept
        : status_(s), value_(std::move(v)) {}
    QueuePushResult(QueuePushStatus s, T&& v) noexcept : status_(s), value_(std::move(v)) {}

    QueuePushStatus status_;
    std::optional<T> value_;
};

template <class T> class QueuePopResult final {
  public:
    static QueuePopResult item(T&& value) noexcept {
        return QueuePopResult{QueuePopStatus::item, std::move(value)};
    }
    static QueuePopResult closed() noexcept {
        return QueuePopResult{QueuePopStatus::closed, std::nullopt};
    }
    static QueuePopResult expired() noexcept {
        return QueuePopResult{QueuePopStatus::expired, std::nullopt};
    }
    static QueuePopResult would_block() noexcept {
        return QueuePopResult{QueuePopStatus::would_block, std::nullopt};
    }

    QueuePopResult(QueuePopResult&&) noexcept = default;

    QueuePopResult& operator=(QueuePopResult&& other) noexcept {
        if (this == &other)
            return *this;
        value_.reset();
        status_ = other.status_;
        if (other.value_.has_value()) {
            value_.emplace(std::move(*other.value_));
            other.value_.reset();
        }
        return *this;
    }
    QueuePopResult(const QueuePopResult&) = delete;
    QueuePopResult& operator=(const QueuePopResult&) = delete;
    ~QueuePopResult() = default;

    QueuePopStatus status() const noexcept { return status_; }

    T take_value() && noexcept {
        if (!value_.has_value()) {
            detail::queue_lease_fail_fast();
        }
        return std::move(*value_);
    }

  private:
    QueuePopResult(QueuePopStatus s, std::optional<T>&& v) noexcept
        : status_(s), value_(std::move(v)) {}
    QueuePopResult(QueuePopStatus s, T&& v) noexcept : status_(s), value_(std::move(v)) {}

    QueuePopStatus status_;
    std::optional<T> value_;
};

template <class T> class AsyncQueue final {
    static_assert(std::is_object_v<T>, "AsyncQueue<T> requires object T");
    static_assert(std::is_nothrow_move_constructible_v<T>,
                  "AsyncQueue<T> requires nothrow-move-constructible T");
    static_assert(std::is_nothrow_destructible_v<T>,
                  "AsyncQueue<T> requires nothrow-destructible T");

  public:
    explicit AsyncQueue(Scheduler& scheduler, std::size_t capacity) : port_(scheduler, capacity) {}

    ~AsyncQueue() = default;

    AsyncQueue(const AsyncQueue&) = delete;
    AsyncQueue& operator=(const AsyncQueue&) = delete;
    AsyncQueue(AsyncQueue&&) = delete;
    AsyncQueue& operator=(AsyncQueue&&) = delete;

    [[nodiscard]] QueuePushResult<T> try_push(T value) {
        detail::QueueItemLease lease = detail::QueueItemFactory::make<T>(port_, std::move(value));
        detail::QueueOpaquePushResult r = port_.try_push(std::move(lease));
        return from_opaque_push_(std::move(r));
    }

    [[nodiscard]] QueuePopResult<T> try_pop() {
        detail::QueueOpaquePopResult r = port_.try_pop();
        return from_opaque_pop_(std::move(r));
    }

    void close() noexcept { port_.close(); }

    [[nodiscard]] bool is_closed() const noexcept { return port_.is_closed(); }
    [[nodiscard]] std::size_t capacity() const noexcept { return port_.capacity(); }
    [[nodiscard]] std::size_t size() const noexcept { return port_.size(); }

    [[nodiscard]] QueuePushResult<T> push(T value) {
        detail::QueueItemLease lease = detail::QueueItemFactory::make<T>(port_, std::move(value));
        detail::QueueOpaquePushResult r = port_.push(std::move(lease));
        return from_opaque_push_(std::move(r));
    }

    [[nodiscard]] QueuePushResult<T> push_until(T value, Scheduler::deadline_t deadline) {
        detail::QueueItemLease lease = detail::QueueItemFactory::make<T>(port_, std::move(value));
        detail::QueueOpaquePushResult r = port_.push_until(std::move(lease), deadline);
        return from_opaque_push_(std::move(r));
    }

    [[nodiscard]] QueuePopResult<T> pop() {
        detail::QueueOpaquePopResult r = port_.pop();
        return from_opaque_pop_(std::move(r));
    }

    [[nodiscard]] QueuePopResult<T> pop_until(Scheduler::deadline_t deadline) {
        detail::QueueOpaquePopResult r = port_.pop_until(deadline);
        return from_opaque_pop_(std::move(r));
    }

    detail::QueueTeardownSession begin_teardown() noexcept { return port_.begin_teardown(); }

    T release_teardown(detail::QueueTeardownSession& session) noexcept {
        detail::QueueItemLease lease = session.take_next();
        return detail::QueueItemFactory::release_teardown<T>(port_, std::move(lease));
    }

  private:
    QueuePushResult<T> from_opaque_push_(detail::QueueOpaquePushResult&& r) {
        using S = detail::QueueOpaquePushStatus;
        const S s = r.status();
        if (s == S::committed) {
            return QueuePushResult<T>::committed();
        }
        detail::QueueItemLease lease = std::move(r).take_failed_lease();
        T value = detail::QueueItemFactory::release_failed<T>(port_, std::move(lease));
        return QueuePushResult<T>::failed(s, std::move(value));
    }

    QueuePopResult<T> from_opaque_pop_(detail::QueueOpaquePopResult&& r) {
        using S = detail::QueueOpaquePopStatus;
        const S s = r.status();
        if (s != S::item) {
            switch (s) {
            case S::closed:
                return QueuePopResult<T>::closed();
            case S::expired:
                return QueuePopResult<T>::expired();
            default:
                return QueuePopResult<T>::would_block();
            }
        }
        detail::QueueItemLease lease = std::move(r).take_item_lease();
        T value = detail::QueueItemFactory::release_popped<T>(port_, std::move(lease));
        return QueuePopResult<T>::item(std::move(value));
    }

    detail::QueuePort port_;
};

} // namespace sluice::async
