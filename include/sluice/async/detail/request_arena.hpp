// sluice::async::detail::RequestArena — bounded RequestSlot arena (Phase B).
//
// ADR-explicit-io-request-contract (Accepted):
//   Decision 2  — one logical capacity and one context-provenance domain per
//                 context/backend pair; the implementation MUST NOT create two
//                 independently oversubscribable request stores.
//   Decision 4  — the unified state machine and the Scheme-B arbitration:
//                 pending cancellation may win the terminal transition directly;
//                 enqueue observing backend_ready performs a successful no-op;
//                 submit still returns success. The enqueue-in-flight pin keeps
//                 reap from publishing Completion-ready until enqueue has
//                 acknowledged (I17, I19).
//   Decision 9  — synchronous identity-bearing non-escaping ReadySink reap.
//   Decision 10 — single-waiter registration with fake stable token + lease.
//   Decision 11 — RequestKey-targeted cancellation with explicit disposition.
//   Decision 13 — request_capacity fixed at construction; arena full ->
//                 synchronous would_block; distinct slot_in_use vs
//                 accepted_outstanding counters.
//   Decision 14 — the accepted terminal path MUST NOT depend on a new unbounded
//                 allocation.
//
// slot_in_use (reserve -> release) and accepted_outstanding (commit ->
// completion-ready publication) are DISTINCT counters (P1-05).
//
// Locking: one leaf slot-lifecycle mutex guards reserve/release/prepare/commit/
// enqueue/record_terminal/reap/register_waiter/cancel_waiter/cancel and the
// free-list/generation/counters (ADR :290-297). Code holding this mutex MUST NOT
// call ReadySink, Scheduler, user code, or backend progress (ADR :296-297). The
// arena's reap path leaves the mutex BEFORE invoking the sink.
//
// Phase B reference backends (FakeAsyncBackend, SyncBackend) own one arena each.
// The arena is single-allocation at construction (a fixed array; no per-submit
// heap traffic). For Phase B it is driven single-threaded by the backend under
// AsyncIoContext::access_mtx_; the mutex + acquire/release ordering make the
// protocol correct for the multi-threaded backends of later phases.
#pragma once

#include <sluice/async/detail/fail_fast.hpp>
#include <sluice/async/detail/ready_sink.hpp>
#include <sluice/async/detail/request_key.hpp>
#include <sluice/async/detail/request_slot.hpp>
#include <sluice/error.hpp>
#include <sluice/result.hpp>

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

namespace sluice::async::detail {

// Result of a successful reserve: the caller (the backend's submit path) holds
// this by value and uses slot_index() + generation() to address the reserved
// slot. A SlotHandle whose generation no longer matches the slot's current
// generation is STALE (the slot has been released and possibly reused); every
// post-reserve authority re-validates under the arena mutex.
struct SlotHandle {
    SlotIndex slot;
    Generation generation;
};

// Cancel disposition (ADR Decision 11). The cancel lookup resolves a RequestKey
// to a slot+generation and returns one of:
//   requested        — cancel was recorded as the terminal winner (pending) or
//                      recorded as intent on an enqueued/running op
//   already_terminal — the op already has a terminal result; cancel is a no-op
//   not_found        — stale/unknown generation (the slot was released/reused)
//   not_supported    — this backend/platform cannot cancel the op
enum class CancelDisposition {
    requested,
    already_terminal,
    not_found,
    not_supported,
};

// Enqueue outcome (ADR Decision 5). Allocation-free, noexcept.
enum class EnqueueOutcome {
    enqueued,         // pending -> enqueued; the op is now dispatchable
    terminal_noop,    // backend_ready observed; successful no-op (Scheme B)
};

class RequestArena {
public:
    RequestArena(ContextIdentity context, std::size_t request_capacity)
        : context_(context), capacity_(request_capacity), slots_(request_capacity) {
        free_slots_.reserve(request_capacity);
        for (std::size_t i = request_capacity; i > 0; --i) {
            free_slots_.push_back(static_cast<std::uint32_t>(i - 1));
        }
    }

    // ADR Decision 15 / AC-13: quiescent destruction requires every slot free.
    // Destroying the arena (via backend/context destruction) while slot_in_use
    // != 0 is a contract violation in BOTH Debug and Release — no implicit
    // drain, cancel, or abandonment. This is the guard that makes the
    // Completion-bound release capability safe: the context/backend must
    // outlive every bound slot, so a caller that destroys the context before
    // resetting ready Completions terminates deterministically instead of
    // leaving a dangling release pointer.
    ~RequestArena() {
        if (slot_in_use_ != 0) {
            request_arena_destruction_fail_fast();
        }
    }

    std::size_t capacity() const noexcept { return capacity_; }
    ContextIdentity context() const noexcept { return context_; }

    // --- accounting (Decision 13) ---
    std::size_t slot_in_use() const noexcept {
        std::lock_guard<std::mutex> lk(mutex_);
        return slot_in_use_;
    }
    std::size_t accepted_outstanding() const noexcept {
        std::lock_guard<std::mutex> lk(mutex_);
        return accepted_outstanding_;
    }
    std::size_t high_water_mark() const noexcept {
        std::lock_guard<std::mutex> lk(mutex_);
        return high_water_mark_;
    }
    std::size_t capacity_rejections() const noexcept {
        std::lock_guard<std::mutex> lk(mutex_);
        return capacity_rejections_;
    }
    // backend_ready but not yet reaped (the in-flight ready set).
    std::size_t backend_ready_count() const noexcept {
        std::lock_guard<std::mutex> lk(mutex_);
        return backend_ready_count_;
    }

    // --- Stage 1: reserve (ADR Decision 5 / I8) ---
    Result<SlotHandle> reserve() {
        std::lock_guard<std::mutex> lk(mutex_);
        if (admission_closed_) {
            // close_admission (ADR Decision 15) prevents new acceptance.
            return make_unexpected<SlotHandle>(IoError{IoError::Code::invalid_state});
        }
        if (free_slots_.empty()) {
            ++capacity_rejections_;
            return make_unexpected<SlotHandle>(IoError{IoError::Code::would_block});
        }
        std::uint32_t idx = free_slots_.back();
        free_slots_.pop_back();
        auto& slot = slots_[idx];
        slot.state_ = RequestState::reserved;
        slot.key_ = RequestKey{context_, SlotIndex{idx}, slot.generation_};
        slot.op_kind_ = OperationKind::read;
        slot.enqueue_in_flight_pin_ = false;
        slot.terminal_ = {};
        slot.registration_ = WaiterRegistration::open_no_waiter;
        slot.waiter_token_ = {};
        slot.waiter_lease_ = {};
        slot.waiter_delivery_present_ = false;
        slot.publish_pending_ = false;
        slot.borrow_ = {};
        ++slot_in_use_;
        if (slot_in_use_ > high_water_mark_) high_water_mark_ = slot_in_use_;
        return SlotHandle{SlotIndex{idx}, slot.generation_};
    }

    // --- Stage 2: prepare (ADR Decision 5) ---
    // Write the operation kind, the borrow metadata (fd identity, buffer
    // address/length), and (in a full backend) the normalized params.
    // reserved -> prepared. A stale handle returns not_found.
    Result<void> prepare(SlotHandle h, OperationKind kind, BorrowMetadata borrow) {
        std::lock_guard<std::mutex> lk(mutex_);
        RequestSlot* s = validate_(h);
        if (!s) return make_unexpected<void>(IoError{IoError::Code::not_found});
        if (s->state_ != RequestState::reserved) {
            return make_unexpected<void>(IoError{IoError::Code::invalid_state});
        }
        s->op_kind_ = kind;
        s->borrow_ = borrow;
        s->borrow_.active = false;  // borrowing begins at commit (I7), not at prepare
        s->state_ = RequestState::prepared;
        return {};
    }

    // --- Stage 3: commit / accept (ADR Decision 5 / I2) ---
    // prepared -> pending; set the enqueue-in-flight pin; accepted_outstanding++;
    // begin the fd/buffer borrow (I7). The Completion's idle->binding->outstanding
    // transition is driven by the backend AROUND this call (the arena owns the
    // slot side; the backend owns the Completion side). This is the submit-success
    // linearization point's slot half.
    Result<void> commit(SlotHandle h) {
        std::lock_guard<std::mutex> lk(mutex_);
        RequestSlot* s = validate_(h);
        if (!s) return make_unexpected<void>(IoError{IoError::Code::not_found});
        if (s->state_ != RequestState::prepared) {
            return make_unexpected<void>(IoError{IoError::Code::invalid_state});
        }
        s->state_ = RequestState::pending;
        s->enqueue_in_flight_pin_ = true;  // I19: set before outstanding is published
        s->borrow_.active = true;          // I7: borrow begins at commit
        ++accepted_outstanding_;
        return {};
    }

    // --- Stage 4: enqueue (ADR Decision 5 / I17, Scheme B) ---
    // Allocation-free, noexcept (returns an outcome, never throws). Two legal
    // outcomes:
    //   enqueued     — pending -> enqueued; the op enters the ready-to-dispatch
    //                  set (the backend's dispatch consumes it from there).
    //   terminal_noop — the slot is already backend_ready (a terminal winner —
    //                  typically pending cancellation — got there first). The
    //                  enqueue does NOT link, dispatch, fail-fast, overwrite the
    //                  result, or publish a second ready linkage. It is a
    //                  successful no-op (Scheme B; ADR :341-349).
    // A stale handle (generation mismatch) is also treated as a successful no-op:
    // the slot has moved on; the old submit path must not touch it (I6).
    // Any other slot state (reserved/prepared = enqueue before commit,
    // enqueued/running = double enqueue, completion_ready = enqueue after reap)
    // is an invariant violation and fails fast (design §9: "only unknown/illegal
    // state is an invariant violation (fail-fast)").
    // Either way, the enqueue pin is release-cleared as the submit path's FINAL
    // slot access; after this call the submit path must not touch the slot.
    EnqueueOutcome enqueue(SlotHandle h) noexcept {
        std::lock_guard<std::mutex> lk(mutex_);
        RequestSlot* s = validate_(h);
        if (!s) return EnqueueOutcome::terminal_noop;  // stale: treat as done
        if (s->state_ == RequestState::pending) {
            s->state_ = RequestState::enqueued;
            // The backend's dispatch path consumes enqueued slots; for the
            // reference backends, dispatch transitions enqueued -> backend_ready
            // deterministically (see record_terminal / dispatch_done).
            s->enqueue_in_flight_pin_ = false;  // final slot access
            return EnqueueOutcome::enqueued;
        }
        if (s->state_ == RequestState::backend_ready) {
            // A terminal winner (cancel/error/ordinary) reached backend_ready
            // first. Successful no-op (Scheme B). Do nothing but ack.
            s->enqueue_in_flight_pin_ = false;  // final slot access
            return EnqueueOutcome::terminal_noop;
        }
        // reserved/prepared/enqueued/running/completion_ready: illegal entry.
        request_arena_enqueue_state_fail_fast();
    }

    // Acknowledge the enqueue pin WITHOUT changing state (used by paths that
    // observed a terminal state and want to release the pin explicitly without
    // the enqueue call). Equivalent to enqueue() on a terminal slot.
    void acknowledge_enqueue_pin(SlotHandle h) noexcept {
        std::lock_guard<std::mutex> lk(mutex_);
        if (RequestSlot* s = validate_(h)) {
            s->enqueue_in_flight_pin_ = false;
        }
    }

    // --- Terminal winner (ADR Decision 12 / I10) ---
    // Record exactly one terminal result. First caller wins: stores the result,
    // transitions to backend_ready (from pending OR enqueued), bumps
    // backend_ready_count_, and (for edge-triggered backends) is the readiness
    // signal. A second call (e.g. late cancel after ordinary completion) is a
    // no-op returning false — losers never overwrite.
    bool record_terminal(SlotHandle h, TerminalResult result) noexcept {
        std::lock_guard<std::mutex> lk(mutex_);
        RequestSlot* s = validate_(h);
        if (!s) return false;
        if (s->terminal_.stored) return false;  // exactly-once: already terminal
        s->terminal_ = result;
        // pending -> backend_ready OR enqueued -> backend_ready. Both are legal
        // terminal-winner transitions under the same arbitration domain (I17).
        if (s->state_ == RequestState::pending || s->state_ == RequestState::enqueued ||
            s->state_ == RequestState::running) {
            s->state_ = RequestState::backend_ready;
            ++backend_ready_count_;
        }
        return true;
    }

    // Convenience: record a canceled terminal result.
    bool record_canceled(SlotHandle h) noexcept {
        return record_terminal(h, TerminalResult::err(IoError{IoError::Code::canceled}));
    }

    // --- Stage 5: reap (ADR Decision 9 / I11 / I16 / I18 / I9) ---
    // Allocation-free TWO-PASS protocol (I9 / Decision 14: completion-ready
    // publication never allocates — no ready-record vector):
    //   Pass 1 (under lock): for each backend_ready slot whose enqueue pin is
    //     acknowledged, close registration (leaving any waiter token/lease in
    //     place for pass 2), mark publish_pending_, transition to
    //     completion_ready, decrement accepted_outstanding, end the borrow.
    //     A still-pinned slot stays backend_ready and reap-ineligible: linkage
    //     unconsumed, nothing published, no decrement (I19).
    //   Pass 2 (per-slot lock): for each publish_pending_ slot, copy the
    //     by-value ReadyEvent data (key/kind/terminal/waiter delivery) onto the
    //     stack, clear publish_pending_, and — after releasing the lock —
    //     publish the Completion and invoke the sink. The publish_pending_ flag
    //     (same leaf domain) guarantees exactly-once publication even under
    //     concurrent reaps. No sink/publish call is made under the lock
    //     (ADR :296-297); the slot cannot be released between pass 1 and its
    //     pass-2 publication because the Completion is not ready until pass 2
    //     publishes it (reset/destruction of a non-ready Completion fails fast).
    //
    // Returns the number of Completions made ready. The caller supplies the
    // per-slot Completion publication via `publish` (the arena cannot call
    // Completion<T>'s template publish directly, so it hands the terminal
    // payload back through the callback).
    struct ReapPublication {
        SlotHandle handle;
        RequestKey key;
        OperationKind kind;
        TerminalResult terminal;
        OptionalWaiterDelivery waiter;
    };
    template <class PublishFn>
    std::size_t reap(SynchronousReadySink& sink, PublishFn publish) {
        // Pass 1 (under lock): mark eligible slots publish_pending_ and
        // transition them to completion_ready.
        {
            std::lock_guard<std::mutex> lk(mutex_);
            for (std::size_t i = 0; i < capacity_; ++i) {
                RequestSlot& s = slots_[i];
                if (s.state_ != RequestState::backend_ready) continue;
                if (s.enqueue_in_flight_pin_) continue;  // reap-ineligible (I19)
                // Eligible: close registration (delivery stays in the slot for
                // pass 2 — allocation-free) and end the borrow (I18).
                s.registration_ = WaiterRegistration::closed;
                s.borrow_.active = false;  // borrow ends at completion-ready (I7)
                s.state_ = RequestState::completion_ready;
                s.publish_pending_ = true;
                --accepted_outstanding_;
                --backend_ready_count_;
            }
        }
        // Pass 2 (per-slot lock): publish + deliver each publish_pending_ slot.
        // Re-locking per slot keeps the protocol correct under concurrent reaps
        // (the flag is exactly-once authority) without allocating a ready
        // record. The by-value event is copied to the stack; the slot is never
        // touched after the lock is released (a caller may reset/reuse it while
        // the sink runs — I16).
        std::size_t reaped = 0;
        for (std::size_t i = 0; i < capacity_; ++i) {
            ReapPublication p;
            bool publish_this = false;
            {
                std::lock_guard<std::mutex> lk(mutex_);
                RequestSlot& s = slots_[i];
                if (!s.publish_pending_) continue;
                publish_this = true;
                p = ReapPublication{
                    SlotHandle{SlotIndex{static_cast<std::uint32_t>(i)}, s.generation_},
                    s.key_, s.op_kind_, s.terminal_,
                    OptionalWaiterDelivery::none()};
                if (s.waiter_delivery_present_) {
                    // Move the token/lease out of the slot exactly-once (races
                    // wait-cancel; the loser gets none).
                    p.waiter = OptionalWaiterDelivery::of(s.waiter_token_,
                                                          std::move(s.waiter_lease_));
                    s.waiter_token_ = {};
                    s.waiter_delivery_present_ = false;
                }
                s.publish_pending_ = false;
            }
            if (publish_this) {
                ++reaped;
                publish(p);
                ReadyEvent ev{p.key, p.kind,
                              OptionalWaiterDelivery{p.waiter.has_waiter, p.waiter.token,
                                                     std::move(p.waiter.lease)}};
                sink.on_ready(std::move(ev));
            }
        }
        return reaped;
    }

    // --- Waiter registration (ADR Decision 10 / I13) ---
    // Register one waiter. Second registration while open_registered returns
    // invalid_state without overwriting the first. Returns invalid_state if the
    // slot is not pending/enqueued (registration only makes sense pre-terminal).
    Result<void> register_waiter(SlotHandle h, WaiterToken token, RoutingLease lease) {
        std::lock_guard<std::mutex> lk(mutex_);
        RequestSlot* s = validate_(h);
        if (!s) return make_unexpected<void>(IoError{IoError::Code::not_found});
        if (s->state_ != RequestState::pending && s->state_ != RequestState::enqueued) {
            return make_unexpected<void>(IoError{IoError::Code::invalid_state});
        }
        if (s->registration_ == WaiterRegistration::open_registered) {
            return make_unexpected<void>(IoError{IoError::Code::invalid_state});
        }
        if (s->registration_ == WaiterRegistration::closed) {
            return make_unexpected<void>(IoError{IoError::Code::invalid_state});
        }
        s->registration_ = WaiterRegistration::open_registered;
        s->waiter_token_ = token;
        s->waiter_lease_ = std::move(lease);
        s->waiter_delivery_present_ = true;
        return {};
    }

    // Wait-cancel (ADR Decision 10 :668-670): removes ONLY the waiter; does NOT
    // cancel the I/O, does NOT end the borrow. Races reap exactly-once for the
    // lease: if reap already closed registration, returns not_found (no double
    // delivery). On success the caller receives the lease to retire.
    Result<RoutingLease> cancel_waiter(SlotHandle h) {
        std::lock_guard<std::mutex> lk(mutex_);
        RequestSlot* s = validate_(h);
        if (!s) return make_unexpected<RoutingLease>(IoError{IoError::Code::not_found});
        if (s->registration_ != WaiterRegistration::open_registered) {
            // Already closed by reap, or no waiter was registered.
            return make_unexpected<RoutingLease>(IoError{IoError::Code::not_found});
        }
        // Take the lease exactly-once; reopen registration (no waiter).
        RoutingLease lease = std::move(s->waiter_lease_);
        s->waiter_token_ = {};
        s->registration_ = WaiterRegistration::open_no_waiter;
        s->waiter_delivery_present_ = false;
        return lease;
    }

    // --- Cancel (ADR Decision 11) ---
    // Resolve a RequestKey against the slot+generation and record intent /
    // terminal per the current state. A pending cancel that wins the terminal
    // transition stores `canceled` directly (Scheme B). The enqueue pin stays
    // live; reap cannot publish Completion-ready until the submit path's enqueue
    // no-ops and acknowledges the pin (I17/I19).
    CancelDisposition cancel(SlotHandle h) noexcept {
        std::lock_guard<std::mutex> lk(mutex_);
        RequestSlot* s = validate_(h);
        if (!s) return CancelDisposition::not_found;
        if (s->state_ == RequestState::free ||
            s->state_ == RequestState::reserved ||
            s->state_ == RequestState::prepared) {
            // Not yet accepted — cancel of a non-accepted key is not_found /
            // invalid; treat as not_found (the key has no accepted terminal).
            return CancelDisposition::not_found;
        }
        if (s->terminal_.stored) {
            // Already terminal — cancel is a no-op (does not overwrite).
            return CancelDisposition::already_terminal;
        }
        if (s->state_ == RequestState::pending) {
            // Scheme B: cancel wins the terminal transition directly.
            s->terminal_ = TerminalResult::err(IoError{IoError::Code::canceled});
            s->state_ = RequestState::backend_ready;
            ++backend_ready_count_;
            return CancelDisposition::requested;
        }
        // enqueued / running — record intent (mark canceled terminal). The
        // dispatch/reap path observes it. For the reference backends this is
        // effectively the same as pending-wins because dispatch is deterministic.
        s->terminal_ = TerminalResult::err(IoError{IoError::Code::canceled});
        if (s->state_ == RequestState::enqueued || s->state_ == RequestState::running) {
            s->state_ = RequestState::backend_ready;
            ++backend_ready_count_;
        }
        return CancelDisposition::requested;
    }

    // --- Release (ADR Decision 15 / AC-13 :566-572) ---
    // Release a slot back to the free list and INCREMENT its generation before
    // the slot can become visible to a new reserve (I6). Release is a contract
    // violation (fail-fast in BOTH Debug and Release) when the enqueue pin is
    // still live, registration is still open, or a stored token/lease has not
    // been consumed. On a completion_ready slot this is the slot-release half
    // of Completion::reset() / ready destruction.
    Result<void> release(SlotHandle h) {
        std::lock_guard<std::mutex> lk(mutex_);
        RequestSlot* s = validate_(h);
        if (!s) return make_unexpected<void>(IoError{IoError::Code::not_found});
        if (s->enqueue_in_flight_pin_) {
            request_slot_release_invariant_fail_fast();
        }
        if (s->registration_ == WaiterRegistration::open_registered) {
            request_slot_release_invariant_fail_fast();
        }
        if (s->state_ != RequestState::completion_ready &&
            s->state_ != RequestState::reserved &&
            s->state_ != RequestState::prepared) {
            // Only a completion_ready, reserved, or prepared slot may be released.
            // (Releasing pending/enqueued/running/backend_ready is a leak of an
            // in-flight op — not a release path.)
            return make_unexpected<void>(IoError{IoError::Code::invalid_state});
        }
        s->state_ = RequestState::free;
        s->generation_ = Generation{s->generation_.value + 1};  // I6
        s->key_ = {};
        s->op_kind_ = OperationKind::read;
        s->enqueue_in_flight_pin_ = false;
        s->terminal_ = {};
        s->registration_ = WaiterRegistration::open_no_waiter;
        s->waiter_token_ = {};
        s->waiter_lease_ = {};
        s->waiter_delivery_present_ = false;
        s->publish_pending_ = false;
        s->borrow_ = {};
        --slot_in_use_;
        free_slots_.push_back(h.slot.value);
        return {};
    }

    // --- Close admission (ADR Decision 15) ---
    // Prevent new acceptance (reserve will return invalid_state). Existing
    // accepted requests continue to ordinary terminal; reap/cancel/waiter-cancel
    // remain legal.
    void close_admission() noexcept {
        std::lock_guard<std::mutex> lk(mutex_);
        admission_closed_ = true;
    }
    bool admission_closed() const noexcept {
        std::lock_guard<std::mutex> lk(mutex_);
        return admission_closed_;
    }

    // Read-only introspection of the slot lifecycle (used by the reference
    // backends' dispatch/reap paths AND by tests; read-only, so it is not a
    // test-only control and does not belong behind SLUICE_ASYNC_INTERNAL_TESTING).
    RequestKey key_of(SlotIndex slot) const noexcept {
        std::lock_guard<std::mutex> lk(mutex_);
        return slots_[slot.value].key_;
    }
    // The slot's current generation (the ABA guard), independent of whether a
    // key is currently installed. After release this is the NEW generation;
    // before the next reserve it exceeds every previously-released key's
    // generation (I6).
    Generation generation_of(SlotIndex slot) const noexcept {
        std::lock_guard<std::mutex> lk(mutex_);
        return slots_[slot.value].generation_;
    }
    RequestState state_of(SlotIndex slot) const noexcept {
        std::lock_guard<std::mutex> lk(mutex_);
        return slots_[slot.value].state_;
    }
    bool enqueue_pin_live(SlotIndex slot) const noexcept {
        std::lock_guard<std::mutex> lk(mutex_);
        return slots_[slot.value].enqueue_in_flight_pin_;
    }
    bool terminal_stored(SlotIndex slot) const noexcept {
        std::lock_guard<std::mutex> lk(mutex_);
        return slots_[slot.value].terminal_.stored;
    }
    WaiterRegistration registration_of(SlotIndex slot) const noexcept {
        std::lock_guard<std::mutex> lk(mutex_);
        return slots_[slot.value].registration_;
    }
    bool borrow_active(SlotIndex slot) const noexcept {
        std::lock_guard<std::mutex> lk(mutex_);
        return slots_[slot.value].borrow_.active;
    }
    bool publish_pending(SlotIndex slot) const noexcept {
        std::lock_guard<std::mutex> lk(mutex_);
        return slots_[slot.value].publish_pending_;
    }

private:
    // Validate a handle under the lock; returns nullptr if stale/wrong-domain.
    RequestSlot* validate_(SlotHandle h) noexcept {
        if (h.slot.value >= capacity_) return nullptr;
        RequestSlot& s = slots_[h.slot.value];
        if (s.generation_ != h.generation) return nullptr;  // stale (I6)
        if (s.state_ == RequestState::free) return nullptr;
        if (s.key_.context != context_) return nullptr;  // wrong domain
        return &s;
    }
    const RequestSlot* validate_(SlotHandle h) const noexcept {
        if (h.slot.value >= capacity_) return nullptr;
        const RequestSlot& s = slots_[h.slot.value];
        if (s.generation_ != h.generation) return nullptr;
        if (s.state_ == RequestState::free) return nullptr;
        if (s.key_.context != context_) return nullptr;
        return &s;
    }

    ContextIdentity context_;
    std::size_t capacity_;
    std::vector<RequestSlot> slots_;
    std::vector<std::uint32_t> free_slots_;
    mutable std::mutex mutex_;

    std::size_t slot_in_use_ = 0;
    std::size_t accepted_outstanding_ = 0;
    std::size_t high_water_mark_ = 0;
    std::size_t capacity_rejections_ = 0;
    std::size_t backend_ready_count_ = 0;
    bool admission_closed_ = false;
};

}  // namespace sluice::async::detail
