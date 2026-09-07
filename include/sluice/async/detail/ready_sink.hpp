#pragma once

#include <sluice/async/detail/request_key.hpp>

#include <cstdint>
#include <utility>

namespace sluice::async::detail {

enum class OperationKind : std::uint8_t {
    read,
    write,
    sync_data,
    sync_all,
};

struct WaiterToken {
    std::uint64_t scheduler_identity = 0;
    std::uint32_t registration_slot = 0;
    std::uint32_t registration_generation = 0;

    friend bool operator==(const WaiterToken&, const WaiterToken&) noexcept = default;
};

class RoutingLease {
  public:
    RoutingLease() = default;
    explicit RoutingLease(std::uint64_t id) noexcept : lease_id_(id) {}
    RoutingLease(RoutingLease&& other) noexcept
        : lease_id_(other.lease_id_), record_index_(other.record_index_),
          record_generation_(other.record_generation_) {
        other.lease_id_ = 0;
        other.record_index_ = 0;
        other.record_generation_ = 0;
    }
    RoutingLease& operator=(RoutingLease&& other) noexcept {
        if (this != &other) {
            lease_id_ = other.lease_id_;
            record_index_ = other.record_index_;
            record_generation_ = other.record_generation_;
            other.lease_id_ = 0;
            other.record_index_ = 0;
            other.record_generation_ = 0;
        }
        return *this;
    }
    RoutingLease(const RoutingLease&) = delete;
    RoutingLease& operator=(const RoutingLease&) = delete;

    bool valid() const noexcept { return lease_id_ != 0; }
    std::uint64_t id() const noexcept { return lease_id_; }

    static RoutingLease pinning(std::uint64_t id, std::uint32_t record_index,
                                std::uint32_t record_generation) noexcept {
        RoutingLease lease{id};
        lease.record_index_ = record_index;
        lease.record_generation_ = record_generation;
        return lease;
    }
    std::uint32_t record_index() const noexcept { return record_index_; }
    std::uint32_t record_generation() const noexcept { return record_generation_; }

  private:
    std::uint64_t lease_id_ = 0;
    std::uint32_t record_index_ = 0;
    std::uint32_t record_generation_ = 0;
};

struct OptionalWaiterDelivery {
    bool has_waiter = false;
    WaiterToken token{};
    RoutingLease lease{};

    static OptionalWaiterDelivery none() noexcept { return {}; }
    static OptionalWaiterDelivery of(WaiterToken t, RoutingLease l) noexcept {
        return {true, t, std::move(l)};
    }
};

struct ReadyEvent {
    RequestKey key{};
    OperationKind kind = OperationKind::read;
    OptionalWaiterDelivery waiter = OptionalWaiterDelivery::none();
};

class SynchronousReadySink {
  public:
    virtual ~SynchronousReadySink() = default;
    virtual void on_ready(ReadyEvent event) noexcept = 0;
};

} // namespace sluice::async::detail
