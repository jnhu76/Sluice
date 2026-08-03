// sluice::async::detail::RequestSlot — one slot of the bounded RequestArena.
//
// ADR-explicit-io-request-contract (Accepted) Decision 3: a RequestSlot holds the
// RequestKey, the unified RequestState, the operation kind, terminal Result
// storage, the identity-bound enqueue-in-flight pin, the single-waiter
// registration state + stable token + move-only routing lease, and the borrow
// metadata.
//
// Every resource the accepted terminal path needs is PRE-RESERVED at the reserve
// stage (I9: the terminal progress of an accepted request does not depend on any
// new unbounded allocation). The slot is a fixed-layout record inside a
// construction-time array; its address is stable for the arena's lifetime.
//
// NOTE on field introduction: each field is added together with the code that
// actually consumes it, rather than as an unused reservation. -Werror (warnings
// are errors in this repo) rejects unused private fields. The I9 "no post-accept
// allocation" property is proven by
//   (a) the slot living in the construction-time fixed array (no per-submit heap
//       traffic), and
//   (b) ASan/UBSan + the reap-path tests observing that publishing a terminal
//       result touches only pre-existing slot storage.
//
// Locking: all mutating access goes through the arena's leaf slot-lifecycle mutex
// (the single arbitration domain — ADR :290-297). The slot itself carries no
// internal lock; the arena guards every transition.
#pragma once

#include <sluice/async/detail/ready_sink.hpp>
#include <sluice/async/detail/request_key.hpp>
#include <sluice/error.hpp>

#include <cstddef>
#include <cstdint>

namespace sluice::async::detail {

// RequestSlot lifecycle states (ADR Decision 4). The competing transitions
//   pending -> enqueued
//   pending -> backend_ready (canceled | error | ordinary)
// share one slot-state arbitration domain (I17); enqueue_in_flight_pin_ is the
// flag bit in that same domain.
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

// Terminal result payload stored in the slot under the leaf domain. A reference
// backend submits read/write (carrying a byte count) and sync_data/sync_all
// (carrying no value). The terminal result is either a byte count (success) or
// an IoError (eof/canceled/backend_error/...). Stored here so publishing the
// terminal result on the accepted path allocates nothing (I9).
struct TerminalResult {
    bool stored = false;          // terminal_result_stored flag, as data
    bool is_error = false;
    std::uint64_t bytes = 0;      // valid when !is_error and kind is read/write
    IoError error{IoError::Code::backend_error};

    static TerminalResult ok_bytes(std::uint64_t n) noexcept {
        return {true, false, n, {}};
    }
    static TerminalResult ok_void() noexcept {
        return {true, false, 0, {}};
    }
    static TerminalResult err(IoError e) noexcept {
        return {true, true, 0, e};
    }
};

// Single-waiter registration state (ADR Decision 10). The slot owns the
// registration; the Scheduler (Phase F) will own the referenced routing record.
//   open_no_waiter  — open, no waiter registered
//   open_registered — open, one waiter; a second register fails synchronously
//   closed          — closed by reap; no further registration
enum class WaiterRegistration : std::uint8_t {
    open_no_waiter,
    open_registered,
    closed,
};

// fd/buffer borrow metadata (ADR Decision 3 / Decision 8, I7). The slot borrows
// the caller's buffer; it does not copy contents. Borrowing begins at commit
// (borrow_active_ = true) and ends at completion-ready publication (reap clears
// it — I18: an acquire observer of Completion-ready sees the ended borrow).
// Phase B reference backends perform no real I/O; the fields record the borrow
// contract so it is observable and auditable.
struct BorrowMetadata {
    int fd = -1;
    const void* address = nullptr;
    std::size_t length = 0;
    bool active = false;  // borrow lifecycle flag (commit -> completion-ready)
};

class RequestSlot {
public:
    RequestSlot() = default;

    bool in_use() const noexcept { return state_ != RequestState::free; }

    RequestState state() const noexcept { return state_; }
    Generation generation() const noexcept { return generation_; }
    const RequestKey& key() const noexcept { return key_; }

    // --- enqueue-in-flight pin (ADR Decision 4 :351-361, I19) ---
    // Set at commit before Completion outstanding is published. Cleared by the
    // submit path as its FINAL slot access (after either linking the pending
    // queue or confirming a terminal-no-op). Reap acquire-checks this bit: a
    // still-pinned backend_ready slot stays reap-ineligible (linkage unconsumed,
    // nothing published, no accepted_outstanding--, no borrow end).
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
    // True while the slot is completion_ready and this reap's publication step
    // has not yet run (see RequestArena::reap — the allocation-free two-pass
    // protocol).
    bool publish_pending() const noexcept { return publish_pending_; }

private:
    friend class RequestArena;  // the arena owns slot-lifecycle transitions

    RequestState state_ = RequestState::free;
    Generation generation_{0};          // incremented on release before reuse (I6)
    RequestKey key_{};
    OperationKind op_kind_ = OperationKind::read;

    // Unified-arbitration flag (same domain as state_ — guarded by the arena
    // mutex; not bit-packed with state_ to keep the type readable).
    bool enqueue_in_flight_pin_ = false;

    // Terminal result payload (pre-reserved; populated by the terminal winner
    // under the arena mutex before backend_ready is observable).
    TerminalResult terminal_{};

    // Single-waiter registration (Decision 10). While open_registered, the
    // waiter_token_ + waiter_lease_ are present; reap closes registration and
    // takes them exactly-once (races wait-cancel for the lease).
    WaiterRegistration registration_ = WaiterRegistration::open_no_waiter;
    WaiterToken waiter_token_{};
    RoutingLease waiter_lease_{};
    // Whether a registered waiter's token/lease is still stored and must be
    // delivered by reap (set at register_waiter; cleared at cancel_waiter and
    // at reap's publication step). Distinguishes "closed after delivery" from
    // "closed, no waiter ever stored" for the allocation-free two-pass reap.
    bool waiter_delivery_present_ = false;

    // Completion-ready publication bookkeeping (I9 / Decision 14): set by reap
    // pass 1 under the leaf domain, cleared by reap pass 2 (the per-slot
    // publication step). Makes the two-pass reap allocation-free: pass 2
    // re-locks per slot and copies the by-value ReadyEvent data out, so no
    // ready-record vector is needed.
    bool publish_pending_ = false;

    // fd/buffer borrow metadata (Decision 3 / Decision 8 / I7 / I18).
    BorrowMetadata borrow_{};
};

}  // namespace sluice::async::detail
