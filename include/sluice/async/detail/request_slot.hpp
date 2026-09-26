#pragma once

#include <sluice/async/detail/ready_sink.hpp>
#include <sluice/async/detail/request_key.hpp>
#include <sluice/error.hpp>

#include <cstddef>
#include <cstdint>
#include <utility>

namespace sluice::async::detail {

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

enum class RequestState : std::uint8_t {
    free,
    reserved,
    prepared,
    pending,
    enqueued,
    running,
    backend_ready,
    completion_ready,
};

struct TerminalResult {
    bool stored = false;
    bool is_error = false;
    std::uint64_t bytes = 0;
    IoError error{IoError::Code::backend_error};

    static TerminalResult ok_bytes(std::uint64_t n) noexcept { return {true, false, n, {}}; }
    static TerminalResult ok_void() noexcept { return {true, false, 0, {}}; }
    static TerminalResult err(IoError e) noexcept { return {true, true, 0, e}; }
};

enum class WaiterRegistration : std::uint8_t {
    open_no_waiter,
    open_registered,
    closed,
};

struct BorrowMetadata {
    int fd = -1;
    const void* address = nullptr;
    std::size_t length = 0;
    bool active = false;
};

struct CompletionBinding {
    void* completion = nullptr;
    std::uint64_t requested_bytes = 0;
    void (*publish)(void* completion, const TerminalResult&) noexcept = nullptr;

    bool installed() const noexcept { return publish != nullptr; }
};

class RequestSlot {
  public:
    RequestSlot() = default;

    bool in_use() const noexcept { return state_ != RequestState::free; }

    RequestState state() const noexcept { return state_; }
    Generation generation() const noexcept { return generation_; }
    const RequestKey& key() const noexcept { return key_; }

    static constexpr std::uint32_t kNotOnReadyRing = static_cast<std::uint32_t>(-1);

    bool enqueue_pin_live() const noexcept { return enqueue_in_flight_pin_; }
    bool terminal_result_stored() const noexcept { return terminal_.stored; }
    bool canceled() const noexcept {
        return terminal_.stored && terminal_.is_error &&
               terminal_.error.code == IoError::Code::canceled;
    }
    OperationKind operation_kind() const noexcept { return op_kind_; }
    const TerminalResult& terminal() const noexcept { return terminal_; }
    WaiterRegistration registration() const noexcept { return registration_; }
    const WaiterToken& waiter_token() const noexcept { return waiter_token_; }
    const BorrowMetadata& borrow() const noexcept { return borrow_; }

  private:
    friend class RequestArena;

    RequestState state_ = RequestState::free;
    Generation generation_{0};
    RequestKey key_{};
    OperationKind op_kind_ = OperationKind::read;

    std::uint64_t submit_seq_ = 0;

    bool enqueue_in_flight_pin_ = false;

    TerminalResult terminal_{};

    WaiterRegistration registration_ = WaiterRegistration::open_no_waiter;
    WaiterToken waiter_token_{};
    RoutingLease waiter_lease_{};

    bool waiter_delivery_present_ = false;

    CompletionBinding publication_binding_{};

    BorrowMetadata borrow_{};

    std::uint32_t ready_next_ = kNotOnReadyRing;

    bool cancel_intent_ = false;
};

}
