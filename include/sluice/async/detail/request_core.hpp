#pragma once

#include <sluice/async/detail/request_key.hpp>
#include <sluice/detail/file_semantics.hpp>

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <vector>

namespace sluice::async::detail {

using sluice::detail::IoOutcome;

enum class RequestOp : std::uint8_t { read, write, sync_data, sync_all };

struct BorrowFacts {
    int fd = -1;
    const void* buffer = nullptr;
    std::uint64_t length = 0;
};

struct RequestDescriptor {
    RequestOp op = RequestOp::read;
    std::uint64_t offset = 0;
    std::uint64_t length = 0;
    bool zero_op = false;
};

struct RequestReservation {
    SlotIndex slot{};
    Generation generation{};
    friend bool operator==(const RequestReservation&,
                           const RequestReservation&) noexcept = default;
};

enum class ReserveStatus : std::uint8_t {
    reserved,
    admission_closed,
    capacity,
    exhausted,
};

struct ReserveAttempt {
    ReserveStatus status = ReserveStatus::capacity;
    RequestReservation reservation{};
    bool ok() const noexcept { return status == ReserveStatus::reserved; }
};

enum class AcceptStatus : std::uint8_t {
    accepted,
    admission_closed,
    bad_reservation,
};

struct AcceptAttempt {
    AcceptStatus status = AcceptStatus::bad_reservation;
    RequestKey id{};
    bool ok() const noexcept { return status == AcceptStatus::accepted; }
};

enum class PublicLookup : std::uint8_t { outstanding, published, not_found };

enum class PublicCancel : std::uint8_t {
    won_before_execution,
    requested,
    already_terminal,
    not_found,
};

enum class BindingRelease : std::uint8_t { released, not_visible_yet, stale };

enum class PublicObservation : std::uint8_t { ready, pending, stale };

enum class PublicConsumption : std::uint8_t { consumed, pending, stale };

enum class TerminalCandidateKind : std::uint8_t {
    physical_outcome,
    zero_effect_cancel,
};

struct TerminalCandidate {
    TerminalCandidateKind kind = TerminalCandidateKind::physical_outcome;
    IoOutcome outcome{};
};

enum class TerminalVerdict : std::uint8_t {
    chosen,
    rejected_duplicate,
    rejected_stale,
    rejected_inadmissible,
};

enum class ExecutionClaim : std::uint8_t { claimed, late, stale };

enum class ExecutionRelease : std::uint8_t {
    released,
    borrow_touch_fully_retired,
    stale,
    underflow_rejected,
    premature_rejected,
};

enum class ControlRelease : std::uint8_t {
    released,
    stale,
    underflow_rejected,
};

struct PublicationPayload {
    RequestKey id{};
    RequestOp op = RequestOp::read;
    std::uint64_t offset = 0;
    std::uint64_t requested_bytes = 0;
    IoOutcome outcome{};
};

enum class PublicationGrant : std::uint8_t {
    granted,
    not_eligible,
    duplicate_publisher,
    stale,
};

enum class PublicationCompletion : std::uint8_t { completed, stale, not_in_flight };

struct CoreSnapshot {
    std::size_t reserved = 0;
    std::size_t accepted_live = 0;
    std::size_t terminal_live = 0;
    std::size_t published_live = 0;
    std::size_t execution_refs = 0;
    std::size_t control_refs = 0;
    std::size_t publication_refs = 0;
    std::size_t public_bindings = 0;
    std::size_t free_slots = 0;
    std::size_t retired_slots = 0;
    bool admission_open = true;
    bool health_failed = false;
};

struct CoreOccupancy {
    std::size_t accepted_live = 0;
    std::size_t outstanding = 0;
    std::size_t public_bindings = 0;
};

class RequestCore {
  public:
    enum class SlotPhase : std::uint8_t { free, reserved, accepted, retired };

    RequestCore(ContextIdentity context, std::size_t request_capacity);
    RequestCore(const RequestCore&) = delete;
    RequestCore& operator=(const RequestCore&) = delete;
    ~RequestCore();

    std::size_t capacity() const noexcept;
    ContextIdentity context() const noexcept;

    CoreOccupancy occupancy() const noexcept;

    ReserveAttempt reserve();
    AcceptAttempt accept(RequestReservation reservation, const RequestDescriptor& descriptor,
                         BorrowFacts borrow) noexcept;
    bool rollback(RequestReservation reservation) noexcept;

    void close_admission() noexcept;
    bool admission_open() const noexcept;
    void note_health_failure() noexcept;
    bool health_failed() const noexcept;

    PublicLookup lookup(RequestKey id) const noexcept;
    PublicCancel cancel(RequestKey id) noexcept;
    BindingRelease release_public_binding(RequestKey id) noexcept;
    PublicObservation observe_public_result(RequestKey id, IoOutcome* out) const noexcept;
    PublicConsumption consume_public_result(RequestKey id, IoOutcome* out) noexcept;

    TerminalVerdict offer_terminal(RequestKey id, const TerminalCandidate& candidate) noexcept;
    ExecutionClaim claim_execution(RequestKey id) noexcept;
    bool acquire_execution(RequestKey id) noexcept;
    ExecutionRelease release_execution(RequestKey id) noexcept;
    bool acquire_control(RequestKey id) noexcept;
    ControlRelease release_control(RequestKey id) noexcept;

    PublicationGrant begin_publication(RequestKey id, PublicationPayload* out) noexcept;
    PublicationCompletion complete_publication(RequestKey id) noexcept;

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)

    struct SlotObservation {
        SlotPhase phase = SlotPhase::free;
        Generation generation{};
        bool accepted = false;
        bool binding_live = false;
        bool terminal_chosen = false;
        bool published = false;
        bool publication_inflight = false;
        bool execution_claimed = false;
        bool cancel_intent = false;
        std::uint32_t execution_refs = 0;
        std::uint32_t control_refs = 0;
        RequestOp op = RequestOp::read;
        std::uint64_t offset = 0;
        std::uint64_t requested_bytes = 0;
        bool has_outcome = false;
        IoOutcome outcome{};
    };

    std::optional<SlotObservation> observe_slot(SlotIndex slot) const noexcept;
    CoreSnapshot snapshot() const noexcept;
    void force_generation_for_testing(SlotIndex slot, Generation generation) noexcept;
#endif

  private:
    struct Slot {
        SlotPhase phase = SlotPhase::free;
        Generation generation{0};
        bool accepted = false;
        bool binding_live = false;
        bool terminal_chosen = false;
        bool published = false;
        bool publication_inflight = false;
        bool execution_claimed = false;
        bool cancel_intent = false;
        bool zero_op = false;
        std::uint32_t execution_refs = 0;
        std::uint32_t control_refs = 0;
        RequestDescriptor descriptor{};
        BorrowFacts borrow{};
        IoOutcome outcome{};
    };

    Slot* resolve_internal_(RequestKey id) noexcept;
    const Slot* resolve_internal_(RequestKey id) const noexcept;
    Slot* resolve_public_(RequestKey id) noexcept;
    const Slot* resolve_public_(RequestKey id) const noexcept;
    Slot* resolve_reserved_(RequestReservation reservation) noexcept;

    bool reclaimable_(const Slot& slot) const noexcept;
    void try_reclaim_(Slot& slot, std::size_t index) noexcept;
    void release_slot_(Slot& slot, std::size_t index) noexcept;

    ContextIdentity context_;
    std::size_t capacity_;
    std::vector<Slot> slots_;
    std::vector<std::uint32_t> free_slots_;
    std::size_t retired_count_ = 0;
    bool admission_open_ = true;
    bool health_failed_ = false;
    mutable std::mutex mutex_;
};

}
