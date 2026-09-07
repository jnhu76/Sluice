#pragma once

#include <sluice/async/detail/fail_fast.hpp>

#include <cstdint>
#include <utility>

namespace sluice::async {
class Scheduler;
}

namespace sluice::async::detail {

class QueuePort;
class QueueOpaquePushResult;
class QueueOpaquePopResult;
class QueueTeardownSession;
class QueueItemFactory;

[[noreturn]] void queue_lease_fail_fast() noexcept;

template <class T> inline const void* queue_type_token() noexcept {
    static_assert(std::is_object_v<T>, "AsyncQueue<T> requires an object type");
    static const std::byte token{0};
    return &token;
}

class QueueItemControl final {
  public:
    enum class Location : std::uint8_t {
        detached,
        producer_operation,
        ring,
        consumer_operation,
        teardown,
        released,
    };

  private:
    QueuePort* const owner_port_;
    void* const typed_node_;
    const void* const type_token_;
    Location location_{Location::detached};

    explicit QueueItemControl(QueuePort& owner_port, void* typed_node,
                              const void* type_token) noexcept
        : owner_port_(&owner_port), typed_node_(typed_node), type_token_(type_token) {}

    QueueItemControl(const QueueItemControl&) = delete;
    QueueItemControl& operator=(const QueueItemControl&) = delete;
    QueueItemControl(QueueItemControl&&) = delete;
    QueueItemControl& operator=(QueueItemControl&&) = delete;

    friend class QueuePort;
    friend class QueueItemLease;
    friend class QueueTeardownSession;
    friend class QueueItemFactory;
    friend class ::sluice::async::Scheduler;
};

class QueueItemLease final {
  public:
    QueueItemLease(QueueItemLease&& other) noexcept
        : control_(std::exchange(other.control_, nullptr)) {}

    QueueItemLease& operator=(QueueItemLease&& other) noexcept {
        if (this == &other) {
            return *this;
        }

        require_empty_or_terminate();
        control_ = std::exchange(other.control_, nullptr);
        return *this;
    }

    QueueItemLease(const QueueItemLease&) = delete;
    QueueItemLease& operator=(const QueueItemLease&) = delete;

    ~QueueItemLease() noexcept;

    explicit operator bool() const noexcept { return control_ != nullptr; }

  private:
    QueueItemControl* control_{nullptr};

    QueueItemLease() noexcept = default;
    explicit QueueItemLease(QueueItemControl& control) noexcept : control_(&control) {}

    QueueItemControl* release_control() noexcept { return std::exchange(control_, nullptr); }

    void adopt_control(QueueItemControl& control) noexcept;

    void require_empty_or_terminate() const noexcept;

    friend class QueuePort;
    friend class QueueOpaquePushResult;
    friend class QueueOpaquePopResult;
    friend class QueueTeardownSession;
    friend class QueueItemFactory;
    friend class ::sluice::async::Scheduler;
};

} // namespace sluice::async::detail
