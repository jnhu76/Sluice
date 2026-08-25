// sluice::async::detail — synchronous, identity-bearing, non-escaping ReadySink.
//
// ADR-explicit-io-request-contract (Accepted) Decision 9 / AC-15:
// the reap path makes a Completion ready and synchronously delivers a by-value
// identity event. The event deliberately carries NO Completion* and NO
// RequestSlot* — a caller that observes ready may reset/destroy the Completion
// and release/reuse the slot WHILE the synchronous sink is still running, and
// the sink must have no pointer that can dangle.
//
//   ReadyEvent {
//       RequestKey            key;     // by value: stable identity
//       OperationKind         kind;    // read/write/sync_data/sync_all
//       OptionalWaiterDelivery waiter; // by value: stable token + move-only lease
//   };
//
// Constraints (ADR :593-604):
//   - the sink callback is noexcept;
//   - invoked exactly once per Completion-ready publication;
//   - allocation-independent (no correctness-required allocation in the sink);
//   - invoked with NO slot/backend/admission lock held (reap leaves the leaf
//     slot-lifecycle domain before calling the sink);
//   - does not call user code (the sink IS the seam the Runtime hooks into);
//   - does not retain the ReadyEvent reference beyond the call;
//   - the slot may be reset/reused during the callback and the event stays safe.
//
// The abstract transfer and exactly-once rules hold with the stable
// tokens/leases defined here (ADR Decision 10 :674-676); the RoutingLease
// pins the Scheduler routing record. The ReadySink contract here is what the
// Runtime integration must satisfy.
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

// Waiter token (ADR Decision 10). A stable value identity:
// (SchedulerIdentity, RegistrationSlot, RegistrationGeneration), backed by a
// Scheduler routing record. The exactly-once transfer mechanics ride on the
// token + lease pair; record lifetime is pinned by the RoutingLease.
struct WaiterToken {
    std::uint64_t scheduler_identity = 0;
    std::uint32_t registration_slot = 0;
    std::uint32_t registration_generation = 0;

    friend bool operator==(const WaiterToken&, const WaiterToken&) noexcept = default;
};

// Move-only routing lease (ADR Decision 10). The lease pins the Scheduler
// routing record it was created for: the record index + generation identify
// the Scheduler-side WaitRecord that must not be retired or reused while a
// slot or synchronous ReadySink owns this lease. The arena
// treats the lease opaquely (stores/moves it into the slot and the ReadyEvent);
// only the Scheduler creates it (pinning()) and reads the pin on the winning
// delivery path (reap sink or waiter cancel).
class RoutingLease {
public:
    RoutingLease() = default;
    explicit RoutingLease(std::uint64_t id) noexcept : lease_id_(id) {}
    RoutingLease(RoutingLease&& other) noexcept
        : lease_id_(other.lease_id_),
          record_index_(other.record_index_),
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

    // Create a lease that pins the Scheduler routing record
    // (record_index, record_generation). The id remains a unique lease
    // identity; the pin is what the Scheduler's winning delivery path uses to
    // locate and retire the record exactly once.
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
    std::uint32_t record_index_ = 0;        // Scheduler WaitRecord index (pin)
    std::uint32_t record_generation_ = 0;   // Scheduler WaitRecord generation (pin)
};

// The by-value delivery carried out of the reap critical section. `has_waiter`
// distinguishes "no waiter registered" from "waiter registered with a token
// the sink is responsible for routing". The token + lease are moved out of the
// slot exactly once (reap and wait-cancel race for them; the loser gets none).
struct OptionalWaiterDelivery {
    bool has_waiter = false;
    WaiterToken token{};
    RoutingLease lease{};

    static OptionalWaiterDelivery none() noexcept { return {}; }
    static OptionalWaiterDelivery of(WaiterToken t, RoutingLease l) noexcept {
        return {true, t, std::move(l)};
    }
};

// The by-value identity event. NO Completion*, NO RequestSlot*. A caller may
// reset/reuse the slot while this event is live; only the by-value fields above
// survive.
struct ReadyEvent {
    RequestKey key{};
    OperationKind kind = OperationKind::read;
    OptionalWaiterDelivery waiter = OptionalWaiterDelivery::none();
};

// The synchronous sink. Reap invokes on_ready exactly once per Completion-ready
// publication, AFTER releasing every slot/backend lock. The implementation MUST
// be noexcept, allocation-independent, and must not retain the event reference.
class SynchronousReadySink {
public:
    virtual ~SynchronousReadySink() = default;
    virtual void on_ready(ReadyEvent event) noexcept = 0;
};

}  // namespace sluice::async::detail
