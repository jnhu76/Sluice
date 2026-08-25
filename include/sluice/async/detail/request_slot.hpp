// sluice::async::detail::RequestSlot — one slot of the bounded RequestArena.
//
// ADR-explicit-io-request-contract (Accepted) Decision 3: a RequestSlot holds the
// RequestKey, the unified RequestState, the operation kind, terminal Result
// storage, the identity-bound enqueue-in-flight pin, the single-waiter
// registration state + stable token + move-only routing lease, the borrow
// metadata, the type-erased Completion publication binding (installed
// before commit; reap publishes Completion-ready through it inside the leaf
// domain — the slot is the single identity carrier, with no parallel map),
// the ready-ring linkage (queue linkage is a PRE-RESERVED
// per-slot resource, not a side-band structure), and the cancellation-intent
// flag (Decision 11 — a running blocking syscall records
// intent rather than forcing a canceled terminal).
//
// Every resource the accepted terminal path needs is PRE-RESERVED at the reserve
// stage: the terminal progress of an accepted request does not depend on any
// new unbounded allocation. The slot is a fixed-layout record inside a
// construction-time array; its address is stable for the arena's lifetime.
//
// Every field has code that actually consumes it; there are no unused
// reservations. -Werror (warnings are errors in this repo) rejects unused
// private fields. The "no post-accept allocation" property is proven by
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
// share one slot-state arbitration domain; enqueue_in_flight_pin_ is the
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
// terminal result on the accepted path allocates nothing.
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
// registration; the Scheduler owns the referenced routing record.
//   open_no_waiter  — open, no waiter registered
//   open_registered — open, one waiter; a second register fails synchronously
//   closed          — closed by reap; no further registration
enum class WaiterRegistration : std::uint8_t {
    open_no_waiter,
    open_registered,
    closed,
};

// fd/buffer borrow metadata (ADR Decision 3 / Decision 8). The slot borrows
// the caller's buffer; it does not copy contents. Borrowing begins at commit
// (borrow_active_ = true) and ends at completion-ready publication (reap clears
// it — an acquire observer of Completion-ready sees the ended borrow).
// Reference backends perform no real I/O; the fields record the borrow
// contract so it is observable and auditable.
struct BorrowMetadata {
    int fd = -1;
    const void* address = nullptr;
    std::size_t length = 0;
    bool active = false;  // borrow lifecycle flag (commit -> completion-ready)
};

// The slot's Completion publication binding (ADR Decision 3):
// a type-erased publication record stored IN the slot record (construction-
// time storage, one per slot — there is NO parallel identity map). Installed
// by the backend BEFORE commit; reap validates it before changing any
// accounting and publishes the Completion-ready release-store THROUGH it
// inside the leaf slot-lifecycle domain. `completion` is an
// opaque Completion<T>* that the arena never dereferences; `publish` is a
// thunk written by the trusted backend-author (it reaches the protected
// AsyncBackend::publish helpers). `requested_bytes` is dispatch-time data
// (fake auto-mode / sync synthetic full-length result).
struct CompletionBinding {
    void* completion = nullptr;  // type-erased Completion<T>* (opaque to the arena)
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

    // Sentinel for the ready-ring linkage: a slot NOT currently threaded onto
    // the arena's backend-ready FIFO carries ready_next_ == kNotOnReadyRing.
    // The arena threads backend_ready slots onto the ring in terminal-winner
    // order so reap publishes in backend-known order (ADR Decision 9)
    // rather than physical slot-index order.
    static constexpr std::uint32_t kNotOnReadyRing = static_cast<std::uint32_t>(-1);

    // --- enqueue-in-flight pin (ADR Decision 4 :351-361) ---
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

private:
    friend class RequestArena;  // the arena owns slot-lifecycle transitions

    RequestState state_ = RequestState::free;
    Generation generation_{0};          // incremented on release before reuse
    RequestKey key_{};
    OperationKind op_kind_ = OperationKind::read;

    // Monotonic submission sequence (set at commit). Lets a backend that exposes
    // a FIFO completion model (FakeAsyncBackend's complete_oldest_*) identify the
    // OLDEST outstanding enqueued op of a given kind by a bounded O(capacity)
    // scan, WITHOUT a side-band HandleRing whose lifecycle is independent of the
    // slot (such a ring accumulates stale handles after a
    // cancel-then-reuse and can strand a later accepted op). Construction-time
    // storage in the slot; zero per-submit allocation.
    std::uint64_t submit_seq_ = 0;

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
    // "closed, no waiter ever stored".
    bool waiter_delivery_present_ = false;

    // The slot's Completion publication binding (see CompletionBinding above).
    // Set at install_publication_binding (before commit); cleared at release.
    // Reap validates `installed()` before any accounting change and publishes
    // the Completion-ready release-store through it inside the leaf domain.
    CompletionBinding publication_binding_{};

    // fd/buffer borrow metadata (Decision 3 / Decision 8).
    BorrowMetadata borrow_{};

    // Ready-ring linkage: index of the NEXT backend_ready
    // slot on the arena's ready FIFO, or kNotOnReadyRing when this slot is not
    // on the ring. The arena sets this when it transitions the slot to
    // backend_ready (record_terminal/cancel) and clears it (back to
    // kNotOnReadyRing) when reap publishes the slot to completion_ready.
    // Construction-time storage in the fixed slot array: NO per-terminal
    // allocation, and NO side-band ready structure to accumulate stale
    // handles.
    std::uint32_t ready_next_ = kNotOnReadyRing;

    // Cancellation intent (ADR Decision 11): set by cancel()
    // on a RUNNING blocking-syscall slot. A running op CANNOT be forced to a
    // canceled terminal — the syscall's ordinary result, ordinary error, or
    // valid interruption competes for the terminal winner (Decision 11).
    // cancel() records intent (returns `intent_recorded`) WITHOUT storing a
    // terminal; record_terminal() then records the REAL result VERBATIM (an
    // ordinary success is NOT rewritten to canceled) and consumes the intent
    // on any winner. Only a backend that CONFIRMS the interruption took effect
    // records TerminalResult::err(canceled) explicitly via record_canceled,
    // and THAT call wins the terminal. pending/enqueued cancel still wins the
    // terminal directly (returns `terminal_won`, Scheme B). The reference
    // backends never enter `running`, so this field is always false there.
    bool cancel_intent_ = false;
};

}  // namespace sluice::async::detail
