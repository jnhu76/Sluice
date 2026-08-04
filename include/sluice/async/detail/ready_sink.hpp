// sluice::async::detail — synchronous, identity-bearing, non-escaping ReadySink.
//
// ADR-explicit-io-request-contract (Accepted) Decision 9 / AC-15 / I11 / I16:
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
//   - does not call user code (the sink IS the seam the Runtime will hook into
//     in Phase F; Phase B delivers fake stable tokens/leases only);
//   - does not retain the ReadyEvent reference beyond the call;
//   - the slot may be reset/reused during the callback and the event stays safe.
//
// Phase B proves the abstract transfer and exactly-once rules with FAKE stable
// tokens/leases and NO Scheduler modification (ADR Decision 10 :674-676). Phase
// F will replace the fake lease with a real Scheduler routing record; the
// ReadySink contract here is what Phase F must satisfy.
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

// Phase B fake waiter token (ADR Decision 10). A stable value identity that
// Phase F will replace with a real (SchedulerIdentity, RegistrationSlot,
// RegistrationGeneration) tuple backed by a Scheduler routing record. Phase B
// proves the exactly-once transfer mechanics; it does NOT prove real Scheduler
// record lifetime (that is Phase F).
struct WaiterToken {
    std::uint64_t scheduler_identity = 0;
    std::uint32_t registration_slot = 0;
    std::uint32_t registration_generation = 0;

    friend bool operator==(const WaiterToken&, const WaiterToken&) noexcept = default;
};

// Phase B fake routing lease (ADR Decision 10). Move-only authority that, in
// Phase F, will pin a Scheduler routing record until the winning delivery path
// acknowledges it. Phase B uses a move-only token to prove the exactly-once
// transfer (a lease cannot be duplicated; the winner consumes it).
class RoutingLease {
public:
    RoutingLease() = default;
    explicit RoutingLease(std::uint64_t id) noexcept : lease_id_(id) {}
    RoutingLease(RoutingLease&& other) noexcept
        : lease_id_(other.lease_id_) { other.lease_id_ = 0; }
    RoutingLease& operator=(RoutingLease&& other) noexcept {
        if (this != &other) {
            lease_id_ = other.lease_id_;
            other.lease_id_ = 0;
        }
        return *this;
    }
    RoutingLease(const RoutingLease&) = delete;
    RoutingLease& operator=(const RoutingLease&) = delete;

    bool valid() const noexcept { return lease_id_ != 0; }
    std::uint64_t id() const noexcept { return lease_id_; }

private:
    std::uint64_t lease_id_ = 0;
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
