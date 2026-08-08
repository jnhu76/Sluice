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
//   Decision 9  — synchronous identity-bearing non-escaping ReadySink reap. Reap
//                 preserves identity AND backend-known order.
//   Decision 10 — single-waiter registration with fake stable token + lease.
//   Decision 11 — RequestKey-targeted cancellation with explicit disposition.
//                 A RUNNING blocking syscall records cancel INTENT (not a
//                 terminal); the ordinary result/error/valid-interrupt later
//                 competes for the terminal winner.
//   Decision 13 — request_capacity fixed at construction; arena full ->
//                 synchronous would_block; distinct slot_in_use vs
//                 accepted_outstanding counters.
//   Decision 14 — the accepted terminal path MUST NOT depend on a new unbounded
//                 allocation.
//
// slot_in_use (reserve -> release) and accepted_outstanding (commit ->
// completion-ready publication) are DISTINCT counters (P1-05).
//
// READY-RING AUTHORITY (review findings #1 and #3): the arena owns a
// construction-time bounded FIFO of backend_ready slot indices (the ready-ring).
// A terminal-winner transition (record_terminal / pending-or-enqueued cancel)
// pushes the slot index onto the ring's tail; reap pops from the head. This is
// the single authority for both (a) the backend-known reap order (Decision 9:
// reap delivers in terminal-winner order, NOT physical slot-index order) and
// (b) the queue linkage (Decision 5 reserve pre-reserves linkage): there is NO
// side-band HandleRing or staging deque whose lifecycle is independent of the
// slot, so a cancelled/reused slot can never leave a stale handle that strands
// a later accepted op (review finding #1). The ring is single-allocation at
// construction (capacity == request_capacity) and push/pop never allocate, so
// the accepted terminal path depends on no new allocation (I9 / Decision 14).
//
// Locking: one leaf slot-lifecycle mutex guards reserve/release/prepare/commit/
// enqueue/record_terminal/reap/register_waiter/cancel_waiter/cancel and the
// free-list/generation/counters/ready-ring (ADR :290-297). Code holding this
// mutex MUST NOT call ReadySink, Scheduler, user code, or backend progress
// (ADR :296-297); the ONLY user-visible publication that happens under the
// mutex is the Completion-ready release-store, which is the leaf domain's own
// linearization point (review C3; ADR completion-ready order). The sink is
// invoked after the lock is released.
//
// Identity: the RequestSlot IS the identity carrier. The slot record holds the
// type-erased Completion publication binding (install_publication_binding
// before commit); reap validates it before changing any accounting and
// publishes Completion-ready through it inside the leaf domain (review C2/C3).
// There is no parallel identity map: cancel resolves a Completion* by a bounded
// O(capacity) scan of the fixed slot array (resolve_completion).
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
#include <limits>
#include <mutex>
#include <optional>
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

// Cancel disposition (ADR Decision 11, review round-4 refinement). The cancel
// lookup resolves a RequestKey to a slot+generation and returns one of:
//   terminal_won     — cancel WON the terminal transition and stored the
//                      canceled terminal (pending/enqueued, Scheme B). This is
//                      the ONLY disposition that establishes a canceled
//                      terminal; the backend tallies canceled_ops here.
//   intent_recorded  — running blocking syscall: cancel recorded INTENT only
//                      (cancel_intent_); the ordinary result/error/valid-
//                      interruption later competes for the terminal winner.
//                      The disposition does NOT promise a canceled terminal
//                      (ADR Decision 11: "requested ... does not promise that
//                      canceled will win").
//   already_terminal — the op already has a terminal result; cancel is a no-op
//   not_found        — stale/unknown generation (the slot was released/reused)
//   not_supported    — this backend/platform cannot cancel the op
//
// ADR `requested` (the accepted-the-request bucket) is refined into
// terminal_won + intent_recorded so the caller can distinguish "canceled
// terminal stored" from "intent only" — the ADR's own text anticipates the
// split ("for pending, cancellation may already have won and stored canceled;
// for later states, intent may only have been recorded").
enum class CancelDisposition {
    terminal_won,
    intent_recorded,
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
        // The ready-ring is a bounded singly-linked FIFO of backend_ready slot
        // indices threaded through each slot's ready_next_ field. No separate
        // storage: push/pop touch only the per-slot link + head/tail/count, so
        // the accepted terminal path depends on no new allocation (review
        // #1/#3, I9 / Decision 14). At most one entry per in-flight op.
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
        slot.publication_binding_ = {};
        slot.borrow_ = {};
        slot.ready_next_ = RequestSlot::kNotOnReadyRing;
        slot.cancel_intent_ = false;
        slot.submit_seq_ = 0;
        ++slot_in_use_;
        if (slot_in_use_ > high_water_mark_) high_water_mark_ = slot_in_use_;
        return SlotHandle{SlotIndex{idx}, slot.generation_};
    }

    // --- Stage 2: prepare (ADR Decision 5 / Decision 6) ---
    // Write the operation kind, the borrow metadata (fd identity, buffer
    // address/length), and (in a full backend) the normalized params.
    // reserved -> prepared. A stale handle returns not_found.
    //
    // Descriptor validation (Decision 6 invalid_argument) is a DECLARED but
    // DEFERRED vocabulary item for the Phase B reference backends: the reference
    // backends perform no real I/O (the fd is a metadata carrier, not a syscall
    // target — the test corpus deliberately uses ReadOp{-1, ...} as an "unused
    // by fake" sentinel), and BorrowMetadata carries no offset, so the
    // invalid_argument causes that ARE representable here (negative fd, null
    // buffer with nonzero length) would reject reference-backend test traffic
    // without backing a real safety property. They are enforced by the full-
    // backend prepare paths (Phase D/E), where a real syscall would actually
    // dereference the fd/buffer. This is the ADR closeout's explicit deferral
    // (see docs/adr/ADR-explicit-io-request-contract.md "Open risks" + the
    // Phase B closeout note); it is recorded as intentional divergence in
    // docs/architecture/divergence-registry.md.
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

    // --- Stage 2.5: install the Completion publication binding (review C2) ---
    // The slot record carries the type-erased publication binding: the opaque
    // Completion pointer, the dispatch-time requested_bytes, and the publish
    // thunk (written by the trusted backend-author). MUST be called before
    // commit; reap validates `installed()` before changing any accounting and
    // publishes Completion-ready through the thunk inside the leaf domain.
    // Exactly one binding per slot generation: a reinstall is invalid_state.
    // On any later pre-commit failure the binding is cleared together with the
    // slot by rollback_reserved_or_prepared — zero residue.
    Result<void> install_publication_binding(
        SlotHandle h, void* completion, std::uint64_t requested_bytes,
        void (*publish)(void* completion, const TerminalResult&) noexcept) {
        std::lock_guard<std::mutex> lk(mutex_);
        RequestSlot* s = validate_(h);
        if (!s) return make_unexpected<void>(IoError{IoError::Code::not_found});
        if (s->state_ != RequestState::prepared) {
            return make_unexpected<void>(IoError{IoError::Code::invalid_state});
        }
        if (s->publication_binding_.installed()) {
            return make_unexpected<void>(IoError{IoError::Code::invalid_state});
        }
        if (publish == nullptr) {
            return make_unexpected<void>(IoError{IoError::Code::invalid_state});
        }
        // CodeRabbit finding: the publication thunk dereferences `completion`
        // (it publishes Completion-ready through a typed Completion<T>*). A null
        // completion would dereference null inside the leaf domain when reap
        // invokes the thunk. Reject it here (the production binding always
        // carries a real Completion pointer). `installed()` keys only on the
        // thunk pointer, so this guard does not change reap's binding check.
        if (completion == nullptr) {
            return make_unexpected<void>(IoError{IoError::Code::invalid_state});
        }
        s->publication_binding_ = {completion, requested_bytes, publish};
        return {};
    }

    // --- Stage 3: commit / accept (ADR Decision 5 / I2) — the SLOT HALF -----
    // prepared -> pending; set the enqueue-in-flight pin; accepted_outstanding++;
    // begin the fd/buffer borrow (I7). The Completion's idle->binding->outstanding
    // transition is driven by the backend AROUND this call (the arena owns the
    // slot side; the backend owns the Completion side). This is the submit-success
    // linearization point's slot half: the FULL acceptance linearization point
    // is the backend's `binding -> outstanding` release-store (ADR §"Commit /
    // accept" Step 5 — "Step 5 is the commit/accept linearization point"; the
    // winning submit performs the protocol "while retaining its own
    // context/admission lock", :453-462).
    //
    // There is DELIBERATELY no admission_closed_ check here. ADR Decision 15
    // ("close_admission atomically prevents new acceptance") gates NEW
    // acceptance at reserve(); a slot already reserved/prepared when close
    // lands is an IN-FLIGHT submission and completes its protocol. The backend
    // serializes close_admission() against the whole Step 1-5 acceptance
    // protocol under its admission transaction lock (ADR :453-462): a close
    // that wins the lock makes every later reserve reject synchronously, and a
    // submit already inside the protocol finishes its LP before close returns.
    // Re-checking admission here would reject an in-flight submission the ADR
    // requires to complete, and would place Decision-15 arbitration in the
    // wrong domain (the arena is the leaf slot-lifecycle domain, not the
    // backend admission authority).
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
        // Record submission order so a backend can locate the oldest outstanding
        // enqueued op of a kind by a bounded scan (no side-band FIFO — review
        // finding #1). Monotonic arena-wide; cleared on release.
        s->submit_seq_ = next_submit_seq_++;
        ++accepted_outstanding_;
        return {};
    }

    // --- Stage 4: enqueue (ADR Decision 5 / I17, Scheme B) ---
    // Allocation-free, noexcept (returns an outcome, never throws). Two legal
    // outcomes:
    //   enqueued     — pending -> enqueued; the op enters the ready-to-dispatch
    //                  set (the backend's dispatch consumes it from there).
    //   terminal_noop — the slot is already backend_ready (a LEGITIMATE prior
    //                  terminal winner — typically pending cancellation — got
    //                  there first). The enqueue does NOT link, dispatch, fail-
    //                  fast, overwrite the result, or publish a second ready
    //                  linkage. It is a successful no-op (Scheme B; ADR
    //                  :341-349).
    // A STALE handle (generation mismatch) is neither outcome: it means the
    // committed submit path's slot moved on (released/reused) while its
    // identity-bound enqueue pin was still live — an I19 reuse-before-ack
    // disaster, not a normal race. It fails fast rather than being masked as a
    // successful no-op (review finding #4). Any other slot state
    // (reserved/prepared = enqueue before commit, enqueued/running = double
    // enqueue, completion_ready = enqueue after reap) is an invariant violation
    // and fails fast (design §9: "only unknown/illegal state is an invariant
    // violation (fail-fast)").
    // In the enqueued / terminal_noop outcomes the enqueue pin is release-
    // cleared as the submit path's FINAL slot access; after this call the
    // submit path must not touch the slot.
    EnqueueOutcome enqueue(SlotHandle h) noexcept {
        std::lock_guard<std::mutex> lk(mutex_);
        RequestSlot* s = validate_(h);
        if (!s) request_arena_enqueue_stale_fail_fast();  // stale: I19 violation
        if (s->state_ == RequestState::pending) {
            s->state_ = RequestState::enqueued;
            // The backend's dispatch path consumes enqueued slots; for the
            // reference backends, dispatch transitions enqueued -> backend_ready
            // deterministically (see record_terminal / dispatch_done).
            s->enqueue_in_flight_pin_ = false;  // final slot access
            return EnqueueOutcome::enqueued;
        }
        if (s->state_ == RequestState::backend_ready) {
            // A legitimate terminal winner (cancel/error/ordinary) reached
            // backend_ready first. Successful no-op (Scheme B). Do nothing but
            // ack the pin.
            s->enqueue_in_flight_pin_ = false;  // final slot access
            return EnqueueOutcome::terminal_noop;
        }
        // reserved/prepared/enqueued/running/completion_ready: illegal entry.
        request_arena_enqueue_state_fail_fast();
    }

    // --- Dispatch: enqueued -> running (design §9 dispatch authority) ---
    // The blocking-syscall dispatch path (a real ThreadPool worker in a later
    // phase) transitions an enqueued op to `running` BEFORE the syscall blocks.
    // Legal outcomes:
    //   true  — enqueued -> running; the op is now executing in the backend.
    //   false — the CURRENT-GENERATION slot is already backend_ready (a terminal
    //           winner — typically cancel — won before dispatch). The dispatch
    //           MUST NOT start the syscall and backs off (losers do not publish,
    //           ADR Decision 12). This false is RESERVED for that legitimate
    //           cancel/dispatch race.
    // A handle validate_ rejects (stale generation, free slot, out-of-range
    // index, wrong context provenance) is NEITHER outcome: the backend holds a
    // dispatch identity whose slot moved on (released/reused) — a lifecycle
    // invariant violation, not a normal race — and fails fast in BOTH Debug and
    // Release (round-5 fix 1) instead of masquerading as a backoff. Any other
    // current-generation state (reserved/prepared/pending = dispatch before
    // enqueue, running = double dispatch, completion_ready = dispatch after
    // reap) is an invariant violation and fails fast (design §9: "only
    // unknown/illegal state is an invariant violation (fail-fast)"). The Phase B
    // reference backends dispatch deterministically (enqueued -> backend_ready)
    // and never call this; it makes the shared arena correct for the
    // ThreadPool/Uring migration, and it makes the Decision-11 running-cancel
    // semantics testable (cancel() on a running slot records intent only).
    bool mark_running(SlotHandle h) noexcept {
        std::lock_guard<std::mutex> lk(mutex_);
        RequestSlot* s = validate_(h);
        if (!s) request_arena_dispatch_stale_fail_fast();  // stale identity
        switch (s->state_) {
        case RequestState::enqueued:
            s->state_ = RequestState::running;
            return true;
        case RequestState::backend_ready:
            // A terminal winner (e.g. cancel) won before dispatch; back off.
            return false;
        default:
            request_arena_dispatch_state_fail_fast();
        }
    }

    // --- Stage 5: reap (ADR Decision 9 / I11 / I16 / I18 / I9 / review C3) ---
    // Allocation-free SINGLE-DOMAIN protocol (review C3 — the Completion-ready
    // release-store is the leaf domain's own linearization point). Pops
    // backend_ready slots from the ready-ring in terminal-winner order (review
    // finding #3 — backend-known order, NOT physical slot-index order). Per
    // popped slot, under ONE lock acquisition, in ADR order:
    //   - validate slot + generation + the Completion publication binding
    //     BEFORE any accounting change (review C2 / I4 / I5: a missing binding
    //     is a fail-fast invariant violation, never a silent skip — silently
    //     dropping would lose an accepted request and strand the Completion
    //     outstanding forever);
    //   - close registration, taking any waiter token/lease exactly-once;
    //   - prepare the by-value ReadyEvent on the stack;
    //   - end the borrow, transition to completion_ready, decrement
    //     accepted_outstanding / backend_ready_count_;
    //   - publish Completion-ready THROUGH the slot binding (the release-store
    //     to ready — an acquire observer of ready sees every effect above, I18).
    // Then release the lock and invoke the sink. The sink is NEVER called under
    // the mutex (ADR :296-297); only the Completion publication is. The state
    // transition itself is the exactly-once authority (a concurrent reap sees
    // completion_ready and the ring no longer references it).
    //
    // A still-pinned backend_ready slot stays reap-ineligible (I19): the slot
    // stays on the ready-ring; a reap that pops it observes the live pin and
    // leaves it for the next reap (it is NOT re-pushed — already on the ring).
    // Once enqueue acknowledges the pin the next reap publishes it
    // (level-triggered). The slot is never touched after the lock is released
    // (a caller may reset/reuse it while the sink runs — I16).
    //
    // Returns the number of Completions made ready. The arena publishes through
    // the slot's own type-erased binding (no publish callback parameter): the
    // binding was installed before commit and validated above.
    std::size_t reap(SynchronousReadySink& sink) {
        std::size_t reaped = 0;
        for (;;) {
            ReadyEvent event{};
            bool publish_this = false;
            {
                std::lock_guard<std::mutex> lk(mutex_);
                std::optional<std::uint32_t> idx = peek_ready_front_locked_();
                if (!idx.has_value()) break;  // ready-ring drained
                RequestSlot& s = slots_[*idx];
                // A terminal-winner push leaves the slot backend_ready with the
                // enqueue pin possibly still live (Scheme-B cancel won before
                // the submit path's enqueue acknowledged its pin — I17/I19). A
                // pinned slot is reap-ineligible: leave it at the head of the
                // ready-ring and STOP this reap. The ready-ring is in
                // terminal-winner order (ADR Decision 9's backend-known order),
                // so publishing a later entry before the pinned head would
                // reorder reap delivery; stopping preserves order, and the next
                // reap publishes the head once its pin is acknowledged
                // (level-triggered). An enqueued slot's pin was already cleared
                // by enqueue, so only pending-cancel winners ever pin the head.
                if (s.enqueue_in_flight_pin_) break;
                // Validate the publication binding BEFORE any accounting
                // change: an accepted slot without its binding is an invariant
                // violation — the Completion can never be made ready.
                if (!s.publication_binding_.installed()) {
                    request_arena_missing_binding_fail_fast();
                }
                // Consume the ready-ring entry: this slot is being published.
                pop_ready_front_locked_();
                s.ready_next_ = RequestSlot::kNotOnReadyRing;
                // Close registration; take any waiter delivery exactly-once
                // (races wait-cancel; the loser gets none).
                s.registration_ = WaiterRegistration::closed;
                event = ReadyEvent{s.key_, s.op_kind_, OptionalWaiterDelivery::none()};
                if (s.waiter_delivery_present_) {
                    event.waiter = OptionalWaiterDelivery::of(s.waiter_token_,
                                                              std::move(s.waiter_lease_));
                    s.waiter_token_ = {};
                    s.waiter_delivery_present_ = false;
                }
                // Accounting / borrow / state changes (I18).
                s.borrow_.active = false;  // borrow ends at completion-ready (I7)
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
                borrow_end_at_ = ++trace_seq_;  // I18 order trace
#endif
                s.state_ = RequestState::completion_ready;
                --accepted_outstanding_;
                --backend_ready_count_;
                // Publish Completion-ready INSIDE the leaf domain: the
                // release-store to ready is the single linearization point
                // (review C3). The thunk is noexcept + allocation-free.
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
                publish_at_ = ++trace_seq_;  // I18 order trace
#endif
                s.publication_binding_.publish(s.publication_binding_.completion,
                                               s.terminal_);
                publish_this = true;
                ++reaped;
            }
            if (publish_this) {
                sink.on_ready(std::move(event));  // sink OUTSIDE the leaf domain
            }
        }
        return reaped;
    }

    // --- Terminal winner (ADR Decision 12 / I10 / review I2) ---
    // Record exactly one terminal result. First caller wins: validates that the
    // slot is a legal terminal candidate (pending/enqueued/running — accepted,
    // not yet terminal), stores the result, transitions to backend_ready, bumps
    // backend_ready_count_, PUSHES the slot index onto the ready-ring (so reap
    // delivers in terminal-winner / backend-known order — Decision 9, review
    // finding #3), and (for edge-triggered backends) is the readiness signal.
    // A second call (e.g. late cancel after ordinary completion) is a no-op
    // returning false — losers never overwrite and never double-push the ring.
    //
    // The recorded result is the syscall's REAL result, verbatim (review
    // round-4 finding 1): a running op whose cancel recorded intent is NOT
    // rewritten to canceled. Cancel is best-effort (ADR Decision 11 — "if the
    // syscall later succeeds or fails, the common terminal-winner rule decides
    // the result"); a backend that CONFIRMS the cancellation actually took
    // effect (a valid interruption, a cancel CQE winner) records
    // TerminalResult::err(canceled) explicitly. pending/enqueued cancel already
    // stored the canceled terminal directly via cancel() (ADR-legal).
    //
    // A default-constructed TerminalResult (stored == false) is REJECTED up
    // front in BOTH Debug and Release (review round-4 finding 2): recording it
    // would publish a phantom 0-byte success (terminal_to_size treats an
    // unstored result as success) and would leave cancel() unable to recognize
    // the existing terminal, risking a second ready-ring push of the same slot.
    //
    // The state is validated BEFORE the terminal is written (review I2): on a
    // reserved/prepared slot the request has not been accepted; storing a
    // terminal there would strand the op forever (a later dispatch
    // record_terminal would see the terminal already stored and the op could
    // never reach backend_ready). That is an invariant violation and fails
    // fast rather than returning a misleading true.
    bool record_terminal(SlotHandle h, TerminalResult result) noexcept {
        std::lock_guard<std::mutex> lk(mutex_);
        RequestSlot* s = validate_(h);
        if (!s) return false;
        if (!result.stored) {
            request_arena_invalid_terminal_fail_fast();
        }
        if (s->terminal_.stored) return false;  // exactly-once: already terminal
        if (s->state_ != RequestState::pending && s->state_ != RequestState::enqueued &&
            s->state_ != RequestState::running) {
            // free/reserved/prepared: not accepted; completion_ready: already
            // reaped (terminal must already be stored — guarded above).
            request_arena_terminal_state_fail_fast();
        }
        s->cancel_intent_ = false;  // any recorded cancel intent is consumed by
                                    // the terminal winner (the real result stands)
        s->terminal_ = result;
        s->state_ = RequestState::backend_ready;
        ++backend_ready_count_;
        push_ready_locked_(h.slot.value);  // backend-known reap order (review #3)
        return true;
    }

    // Convenience: record a canceled terminal result.
    bool record_canceled(SlotHandle h) noexcept {
        return record_terminal(h, TerminalResult::err(IoError{IoError::Code::canceled}));
    }

    // --- Stage 5: reap (ADR Decision 9 / I11 / I16 / I18 / I9 / review C3) ---

    // --- Waiter registration (ADR Decision 10 / I13) ---
    // Register one waiter. Second registration while open_registered returns
    // invalid_state without overwriting the first. Registration is ORTHOGONAL
    // to execution state (ADR Decision 10 :668-698): it is legal for any
    // accepted, unreaped request — pending/enqueued/running/backend_ready —
    // while registration is open, and ONLY reap closes it. A terminal winner
    // (record_terminal -> backend_ready) does NOT close registration: a waiter
    // registered after the terminal is recorded but before reap still succeeds
    // and reap delivers it. invalid_state is returned only for reserved/
    // prepared (the pre-commit binding window, :483-484) and completion_ready
    // (reap already closed registration; observationally closed ==
    // completion_ready because reap sets both in one leaf-domain critical
    // section, so the state guard is the single registration-window authority —
    // a future path that closes registration without completion_ready must
    // restore a closed-registration guard) and for a duplicate waiter. A stale
    // handle returns not_found. On any failure the candidate lease is consumed
    // at the by-value call boundary and released inline — never transferred to
    // the slot (ADR :661-662: "Scheduler reclaims it or completes inline as
    // appropriate"; Phase B completes inline).
    Result<void> register_waiter(SlotHandle h, WaiterToken token, RoutingLease lease) {
        std::lock_guard<std::mutex> lk(mutex_);
        RequestSlot* s = validate_(h);
        if (!s) return make_unexpected<void>(IoError{IoError::Code::not_found});
        if (s->state_ != RequestState::pending && s->state_ != RequestState::enqueued &&
            s->state_ != RequestState::running && s->state_ != RequestState::backend_ready) {
            // reserved/prepared: not accepted yet (pre-commit binding window);
            // completion_ready: reap already closed registration.
            return make_unexpected<void>(IoError{IoError::Code::invalid_state});
        }
        if (s->registration_ == WaiterRegistration::open_registered) {
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

    // --- Cancel (ADR Decision 11, review round-4 finding 1) ---
    // Resolve a RequestKey against the slot+generation and record intent /
    // terminal per the current state. Cancel is BEST-EFFORT (ADR Decision 11):
    // it records intent / stores a canceled terminal, but the syscall's real
    // result still competes normally via the terminal-winner rule.
    //   - pending/enqueued — cancel WINS the terminal transition directly and
    //     stores `canceled` (Scheme B; ADR-legal for both states). Returns
    //     terminal_won — this is the ONLY disposition that establishes a canceled
    //     terminal, so the backend tallies canceled_ops here. For pending the
    //     enqueue pin stays live, so reap cannot publish Completion-ready until
    //     the submit path's enqueue no-ops and acknowledges the pin (I17/I19).
    //   - running — cancel records INTENT (cancel_intent_ = true) and returns
    //     intent_recorded, but does NOT store a terminal: the running blocking
    //     syscall's ordinary result, ordinary error, or valid interruption later
    //     competes for the terminal winner via record_terminal, which records
    //     the REAL result VERBATIM (review round-4 finding 1: best-effort cancel
    //     must NOT secretly turn an ordinary success into canceled). A backend
    //     that CONFIRMS the cancellation actually took effect (a valid
    //     interruption, a cancel CQE winner) records
    //     TerminalResult::err(canceled) explicitly and THAT call wins the
    //     terminal — the backend tallies canceled_ops on that confirmed win, not
    //     here. The Phase B reference backends never enter `running`, so this
    //     branch is dormant there; it makes the shared arena correct for the
    //     later ThreadPool/Uring migration.
    //   - backend_ready with a stored terminal — already_terminal (no overwrite;
    //     ADR Decision 12: losers do not overwrite).
    //   - free/reserved/prepared — not_found (the key has no accepted terminal).
    CancelDisposition cancel(SlotHandle h) noexcept {
        std::lock_guard<std::mutex> lk(mutex_);
        RequestSlot* s = validate_(h);
        if (!s) return CancelDisposition::not_found;
        // An already-terminal op is a no-op: cancel does not overwrite the
        // existing terminal winner (ADR Decision 12). checked before the switch
        // so every state below is a legal cancel candidate with no terminal.
        if (s->terminal_.stored) {
            return CancelDisposition::already_terminal;
        }
        switch (s->state_) {
        case RequestState::pending:
        case RequestState::enqueued:
            // Cancel WINS the terminal transition directly (Scheme B). Store the
            // canceled terminal, transition to backend_ready, and push the
            // ready-ring in terminal-winner order (review finding #3). This is
            // the confirmed canceled winner — the backend tallies canceled_ops.
            s->cancel_intent_ = false;
            s->terminal_ = TerminalResult::err(IoError{IoError::Code::canceled});
            s->state_ = RequestState::backend_ready;
            ++backend_ready_count_;
            push_ready_locked_(h.slot.value);
            return CancelDisposition::terminal_won;
        case RequestState::running:
            // Decision 11 best-effort: record INTENT only. No terminal stored,
            // no ready-ring push — record_terminal later records the real result
            // verbatim and consumes the intent on any winner.
            s->cancel_intent_ = true;
            return CancelDisposition::intent_recorded;
        default:
            // free (already filtered by validate_)/reserved/prepared: not yet
            // accepted — the key has no accepted terminal. completion_ready
            // without a stored terminal is unreachable (reap only runs after a
            // stored terminal); backend_ready without one is unreachable (the
            // only path to backend_ready stores one).
            return CancelDisposition::not_found;
        }
    }

    // True iff a running-cancel intent is live on this slot (Decision 11). Read-
    // only introspection for backends/tests; not a test-only control.
    bool cancel_intent_live(SlotIndex slot) const noexcept {
        check_slot_in_range_(slot);
        std::lock_guard<std::mutex> lk(mutex_);
        return slots_[slot.value].cancel_intent_;
    }

    // --- Release (ADR Decision 15 / AC-13 :566-572 / review I1) ---
    // Two DISTINCT authorities with different failure contracts:
    //
    //   rollback_reserved_or_prepared(h) — the PRE-COMMIT rollback authority.
    //     Returns a slot that was reserved/prepared but never accepted (the
    //     backend's submit path after reserve/prepare/binding-install failure
    //     or a lost Completion binding CAS). Failures are ordinary errors
    //     (not_found for a stale handle, invalid_state for a wrong state) that
    //     the backend maps onto its synchronous reject result — zero side
    //     effects, synchronous rejection stays zero-side-effect (review C1).
    //
    //   release_completed_binding(h) — the COMPLETED-binding release authority.
    //     Returns a completion_ready slot from Completion::reset() / ready
    //     destruction. ANY failure is an internal protocol violation (the
    //     caller cannot recover the slot, and ignoring the failure would let
    //     the Completion become reusable while its old slot is permanently
    //     slot_in_use — a later context destruction fail-fast) and fails fast
    //     in BOTH Debug and Release: stale handle, live enqueue pin, open
    //     waiter registration, or a slot that is not completion_ready.
    //
    // Both paths free the slot under the leaf domain: generation++ BEFORE the
    // slot becomes visible to a new reserve (I6), binding cleared, counters
    // decremented.
    Result<void> rollback_reserved_or_prepared(SlotHandle h) {
        std::lock_guard<std::mutex> lk(mutex_);
        RequestSlot* s = validate_(h);
        if (!s) return make_unexpected<void>(IoError{IoError::Code::not_found});
        if (s->state_ != RequestState::reserved && s->state_ != RequestState::prepared) {
            return make_unexpected<void>(IoError{IoError::Code::invalid_state});
        }
        free_slot_locked_(s, h.slot.value);
        return {};
    }

    void release_completed_binding(SlotHandle h) noexcept {
        std::lock_guard<std::mutex> lk(mutex_);
        RequestSlot* s = validate_(h);
        if (!s) request_slot_release_invariant_fail_fast();  // stale handle
        if (s->enqueue_in_flight_pin_) {
            request_slot_release_invariant_fail_fast();
        }
        if (s->registration_ == WaiterRegistration::open_registered) {
            request_slot_release_invariant_fail_fast();
        }
        if (s->state_ != RequestState::completion_ready) {
            // Only a reaped slot may be released by the caller handshake;
            // releasing pending/enqueued/backend_ready would leak an in-flight
            // op, and reserved/prepared belongs to the rollback authority.
            request_slot_release_invariant_fail_fast();
        }
        free_slot_locked_(s, h.slot.value);
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

    // Production quiescence snapshot for ThreadPoolBackend destruction (Phase E P1).
    // Returns counts under one arena leaf lock so the backend can verify that no
    // accepted work, active borrow, or backend-ready terminal remains before it
    // begins worker teardown. The destructor MUST NOT implicitly drain/cancel/wait.
    struct ArenaQuiescence {
        std::size_t slot_in_use;
        std::size_t accepted_outstanding;
        std::size_t backend_ready;
    };
    ArenaQuiescence quiescence_snapshot() const noexcept {
        std::lock_guard<std::mutex> lk(mutex_);
        return {slot_in_use_, accepted_outstanding_, backend_ready_count_};
    }

    // Read-only introspection of the slot lifecycle (used by the reference
    // backends' dispatch/reap paths AND by tests; read-only, so it is not a
    // test-only control and does not belong behind SLUICE_ASYNC_INTERNAL_TESTING).
    // Each accessor bounds-checks the slot index (CodeRabbit finding: these are
    // the only slot-addressing paths without validate_'s range check — an out-
    // of-range index would be an out-of-bounds read; fail-fast in BOTH modes).
    RequestKey key_of(SlotIndex slot) const noexcept {
        check_slot_in_range_(slot);
        std::lock_guard<std::mutex> lk(mutex_);
        return slots_[slot.value].key_;
    }
    // The slot's current generation (the ABA guard), independent of whether a
    // key is currently installed. After release this is the NEW generation;
    // before the next reserve it exceeds every previously-released key's
    // generation (I6).
    Generation generation_of(SlotIndex slot) const noexcept {
        check_slot_in_range_(slot);
        std::lock_guard<std::mutex> lk(mutex_);
        return slots_[slot.value].generation_;
    }
    RequestState state_of(SlotIndex slot) const noexcept {
        check_slot_in_range_(slot);
        std::lock_guard<std::mutex> lk(mutex_);
        return slots_[slot.value].state_;
    }
    bool enqueue_pin_live(SlotIndex slot) const noexcept {
        check_slot_in_range_(slot);
        std::lock_guard<std::mutex> lk(mutex_);
        return slots_[slot.value].enqueue_in_flight_pin_;
    }
    bool terminal_stored(SlotIndex slot) const noexcept {
        check_slot_in_range_(slot);
        std::lock_guard<std::mutex> lk(mutex_);
        return slots_[slot.value].terminal_.stored;
    }
    WaiterRegistration registration_of(SlotIndex slot) const noexcept {
        check_slot_in_range_(slot);
        std::lock_guard<std::mutex> lk(mutex_);
        return slots_[slot.value].registration_;
    }
    bool borrow_active(SlotIndex slot) const noexcept {
        check_slot_in_range_(slot);
        std::lock_guard<std::mutex> lk(mutex_);
        return slots_[slot.value].borrow_.active;
    }
    OperationKind kind_of(SlotIndex slot) const noexcept {
        check_slot_in_range_(slot);
        std::lock_guard<std::mutex> lk(mutex_);
        return slots_[slot.value].op_kind_;
    }
    // Dispatch-time requested length (the binding's requested_bytes; the fake's
    // auto-mode and the sync backend's synthetic full-length result use it).
    std::uint64_t requested_bytes_of(SlotIndex slot) const noexcept {
        check_slot_in_range_(slot);
        std::lock_guard<std::mutex> lk(mutex_);
        return slots_[slot.value].publication_binding_.requested_bytes;
    }
    // Submission order of an accepted op (set at commit). Lets a backend locate
    // the oldest outstanding enqueued op of a kind without a side-band FIFO
    // (review finding #1).
    std::uint64_t submit_seq_of(SlotIndex slot) const noexcept {
        check_slot_in_range_(slot);
        std::lock_guard<std::mutex> lk(mutex_);
        return slots_[slot.value].submit_seq_;
    }
    // Find the OLDEST ENQUEUED slot of the given kind (the smallest submit_seq
    // among slots currently in the `enqueued` state with matching op_kind), for
    // a backend that completes ops in submission order. Returns the slot handle
    // or nullopt when no enqueued op of that kind is outstanding. Bounded
    // O(capacity) scan of the fixed slot array — allocation-free. A slot that
    // already went terminal (cancel/ordinary) is `backend_ready`, not
    // `enqueued`, so it is naturally skipped (no stale-handle accumulation —
    // review finding #1). The kind filter lets size and void ops share one arena
    // while completing independently.
    std::optional<SlotHandle> oldest_enqueued_of(OperationKind kind) const noexcept {
        std::lock_guard<std::mutex> lk(mutex_);
        std::optional<SlotHandle> best;
        std::uint64_t best_seq = 0;
        for (std::size_t i = 0; i < capacity_; ++i) {
            const RequestSlot& s = slots_[i];
            if (s.state_ != RequestState::enqueued) continue;
            if (s.op_kind_ != kind) continue;
            if (!best.has_value() || s.submit_seq_ < best_seq) {
                best = SlotHandle{SlotIndex{static_cast<std::uint32_t>(i)}, s.generation_};
                best_seq = s.submit_seq_;
            }
        }
        return best;
    }

    // Resolve a bound Completion pointer to its current slot+generation (the
    // pointer-keyed bridge for cancel: the public API stays pointer-keyed, ADR
    // Decision 7). Bounded O(capacity) scan of the fixed slot array —
    // allocation-free, and NO parallel identity map (review C2: the slot's own
    // binding is the authoritative identity). Returns nullopt when no slot is
    // bound to the pointer (unknown/stale/released Completion).
    std::optional<SlotHandle> resolve_completion(const void* completion) const noexcept {
        std::lock_guard<std::mutex> lk(mutex_);
        for (std::size_t i = 0; i < capacity_; ++i) {
            const RequestSlot& s = slots_[i];
            if (s.state_ != RequestState::free &&
                s.publication_binding_.completion == completion) {
                return SlotHandle{SlotIndex{static_cast<std::uint32_t>(i)},
                                  s.generation_};
            }
        }
        return std::nullopt;
    }

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
    // A single-lock observation of a SlotHandle that validates generation, context,
    // and non-free state. Returns nullopt if the handle is stale, out of range, or
    // points to a free slot. This is the preferred test seam over the piecewise
    // state_of/enqueue_pin_live/terminal_stored accessors because it cannot
    // mistake a later slot reuse for the original request.
    struct RequestObservation {
        SlotHandle handle;
        RequestState state;
        bool enqueue_pin_live;
        bool terminal_stored;
    };
    std::optional<RequestObservation> observe_for_test(SlotHandle h) const noexcept {
        std::lock_guard<std::mutex> lk(mutex_);
        if (h.slot.value >= capacity_) return std::nullopt;
        const RequestSlot& s = slots_[h.slot.value];
        if (s.state_ == RequestState::free) return std::nullopt;
        if (s.generation_ != h.generation) return std::nullopt;
        if (s.key_.context != context_) return std::nullopt;
        return RequestObservation{h, s.state_, s.enqueue_in_flight_pin_,
                                  s.terminal_.stored};
    }

    // C2c row 11: a single-lock BY-VALUE snapshot of the fd/buffer borrow
    // metadata for a validated SlotHandle (generation + context + non-free).
    // Returns nullopt for a stale/out-of-range/free handle, exactly like
    // observe_for_test. Deliberately returns a value copy — never a
    // RequestSlot* or BorrowMetadata& — so a test cannot mutate slot state.
    // This is what lets the C2c borrow matrix observe the exact
    // fd/address/length/active at every lifecycle window (prepare-stage
    // inactive, commit-active, backend_ready-before-reap still active,
    // completion-ready ended) through one generation-validated seam instead of
    // the piecewise borrow_active(SlotIndex) accessor (which cannot distinguish
    // a later slot reuse from the original request).
    struct BorrowSnapshot {
        int fd = -1;
        const void* address = nullptr;
        std::size_t length = 0;
        bool active = false;
    };
    std::optional<BorrowSnapshot> borrow_for_test(SlotHandle h) const noexcept {
        std::lock_guard<std::mutex> lk(mutex_);
        if (h.slot.value >= capacity_) return std::nullopt;
        const RequestSlot& s = slots_[h.slot.value];
        if (s.state_ == RequestState::free) return std::nullopt;
        if (s.generation_ != h.generation) return std::nullopt;
        if (s.key_.context != context_) return std::nullopt;
        return BorrowSnapshot{s.borrow_.fd, s.borrow_.address, s.borrow_.length,
                              s.borrow_.active};
    }

    // C2c rows 12-14: a single-lock BY-VALUE observation of the single-waiter
    // registration for a validated SlotHandle: the registration state, whether
    // a token/lease delivery is still stored (waiter_delivery_present_), and
    // the stored token + lease id (0 when no lease is stored). Same
    // generation-validated shape as observe_for_test/borrow_for_test; returns
    // nullopt for a stale/out-of-range/free handle. Read-only by-value — a
    // test can prove "registration still open_registered with EXACTLY token A /
    // lease A" (the no-overwrite and stale-waiter proofs) without touching slot
    // internals.
    struct WaiterObservation {
        WaiterRegistration registration;
        bool delivery_present;
        WaiterToken token;
        std::uint64_t lease_id;
    };
    std::optional<WaiterObservation> waiter_for_test(SlotHandle h) const noexcept {
        std::lock_guard<std::mutex> lk(mutex_);
        if (h.slot.value >= capacity_) return std::nullopt;
        const RequestSlot& s = slots_[h.slot.value];
        if (s.state_ == RequestState::free) return std::nullopt;
        if (s.generation_ != h.generation) return std::nullopt;
        if (s.key_.context != context_) return std::nullopt;
        return WaiterObservation{s.registration_, s.waiter_delivery_present_,
                                 s.waiter_token_, s.waiter_lease_.id()};
    }

    // C2c row 11: I18 publication-order trace. Inside reap's leaf-domain
    // critical section, borrow-end and the Completion-ready publication each
    // record a value from ONE monotonic sequence (trace_seq_). A test asserts
    // publish_seq > borrow_end_seq to pin I18 (an acquire observer of
    // Completion-ready sees the ended borrow); a mutant that moves the borrow
    // end AFTER the publication — same critical section, still before reap
    // returns — flips the order and the focused case fails, where a post-reap
    // borrow observation alone cannot distinguish that defect. Written under
    // mutex_; read after reap returns (or after a thread join) in tests.
    // Diagnostics only; the counters are compiled out of production builds.
    struct PublicationOrder {
        std::uint64_t borrow_end_seq = 0;
        std::uint64_t publish_seq = 0;
    };
    PublicationOrder publication_order_for_test() const noexcept {
        return {borrow_end_at_, publish_at_};
    }
#endif

private:
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
    // I18 publication-order trace storage (see publication_order_for_test).
    std::uint64_t trace_seq_ = 0;
    std::uint64_t borrow_end_at_ = 0;
    std::uint64_t publish_at_ = 0;
#endif
    // Bounds-check a SlotIndex for the read-only introspection accessors
    // (CodeRabbit finding: those are the only slot-addressing paths without
    // validate_'s range check). An out-of-range index is an invariant violation,
    // not a recoverable error — fail-fast in BOTH Debug and Release.
    void check_slot_in_range_(SlotIndex slot) const noexcept {
        if (slot.value >= capacity_) request_arena_slot_index_out_of_range_fail_fast();
    }
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

    // --- ready-ring (review findings #1 and #3) ---
    // A bounded FIFO of backend_ready slot indices, sized to request_capacity.
    // Terminal-winner transitions (record_terminal / pending-or-enqueued cancel)
    // push the slot's index onto the tail; reap pops from the head, publishing
    // Completions in terminal-winner / backend-known order (Decision 9). The
    // ring is single-allocation at construction and never grows, so the accepted
    // terminal path depends on no new allocation (I9 / Decision 14). Because the
    // linkage lives IN each slot (ready_next_) and the ring is owned by the
    // arena under one lock, a cancelled/released slot can never leave a stale
    // side-band handle that strands a later accepted op (review finding #1).
    // Caller MUST hold mutex_.
    void push_ready_locked_(std::uint32_t idx) noexcept {
        // Ready-ring invariants (review round-4 finding 2; round-5 fix 2): a
        // terminal-winner push is only legal on a slot that just became
        // backend_ready with a stored terminal, is not ALREADY ON the ring,
        // and whose ring has room. The membership check must cover the TAIL:
        // the ring is an intrusive singly-linked FIFO whose tail node
        // legitimately carries ready_next_ == kNotOnReadyRing (the list
        // terminator), so the link-state test alone cannot distinguish
        // "not on ring" from "linked as tail" — `already_tail` closes that
        // gap (a repeated push of the current tail is rejected). The
        // head/tail/count triple must also be structurally consistent (O(1);
        // no list traversal, no allocation). Violating any invariant would
        // corrupt the ring or let reap publish a phantom — fail-fast in BOTH
        // Debug and Release rather than corrupt silently.
        RequestSlot& s = slots_[idx];
        const bool already_tail = ready_count_ != 0 && ready_tail_ == idx;
        const bool ends_consistent =
            ready_count_ == 0
                ? (ready_head_ == RequestSlot::kNotOnReadyRing &&
                   ready_tail_ == RequestSlot::kNotOnReadyRing)
                : (ready_head_ < capacity_ && ready_tail_ < capacity_);
        if (s.state_ != RequestState::backend_ready || !s.terminal_.stored ||
            s.ready_next_ != RequestSlot::kNotOnReadyRing || already_tail ||
            !ends_consistent || ready_count_ >= capacity_) {
            request_arena_ready_ring_invariant_fail_fast();
        }
        // The slot is being newly linked: thread it onto the tail. ready_next_
        // is kNotOnReadyRing for a freshly-terminal slot (reap clears it; a
        // second terminal-winner push never happens — record_terminal/cancel
        // return early when terminal_.stored).
        if (ready_count_ == 0) {
            ready_head_ = idx;
        } else {
            slots_[ready_tail_].ready_next_ = idx;
        }
        s.ready_next_ = RequestSlot::kNotOnReadyRing;
        ready_tail_ = idx;
        ++ready_count_;
    }
    std::optional<std::uint32_t> peek_ready_front_locked_() const noexcept {
        if (ready_count_ == 0) return std::nullopt;
        return ready_head_;
    }
    void pop_ready_front_locked_() noexcept {
        // Caller has confirmed the slot is being published (reap); unlink the
        // head. The slot's ready_next_ is cleared by the caller after the pop.
        std::uint32_t idx = ready_head_;
        std::uint32_t nxt = slots_[idx].ready_next_;
        ready_head_ = nxt;
        --ready_count_;
        if (ready_count_ == 0) ready_tail_ = RequestSlot::kNotOnReadyRing;
    }

    // Free a validated slot back to the free list (caller holds the mutex and
    // has already validated the release authority's preconditions). Generation
    // increments BEFORE the slot becomes visible to a new reserve (I6); at
    // UINT64_MAX it fail-fasts rather than silently wrapping (review finding #5
    // — a wrap would re-introduce ABA and violate I6's absolute wording). The
    // publication binding is cleared so a stale resolve can never match a
    // released slot. The slot must NOT be on the ready-ring at release time
    // (reap pops it before completion_ready; release requires completion_ready).
    void free_slot_locked_(RequestSlot* s, std::uint32_t idx) noexcept {
        s->state_ = RequestState::free;
        // I6 + review finding #5: 64-bit generation with fail-fast at the max so
        // a stale key can NEVER collide with a future occupant.
        if (s->generation_.value == std::numeric_limits<std::uint64_t>::max()) {
            request_arena_generation_exhausted_fail_fast();
        }
        s->generation_ = Generation{s->generation_.value + 1};  // I6
        s->key_ = {};
        s->op_kind_ = OperationKind::read;
        s->enqueue_in_flight_pin_ = false;
        s->terminal_ = {};
        s->registration_ = WaiterRegistration::open_no_waiter;
        s->waiter_token_ = {};
        s->waiter_lease_ = {};
        s->waiter_delivery_present_ = false;
        s->publication_binding_ = {};
        s->borrow_ = {};
        s->ready_next_ = RequestSlot::kNotOnReadyRing;
        s->cancel_intent_ = false;
        s->submit_seq_ = 0;
        --slot_in_use_;
        free_slots_.push_back(idx);
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

    // Monotonic submission sequence assigned at commit (review finding #1: lets a
    // backend find the oldest enqueued op of a kind without a side-band FIFO).
    std::uint64_t next_submit_seq_ = 1;

    // ready-ring (review findings #1/#3): bounded singly-linked FIFO of
    // backend_ready slot indices, in terminal-winner order. Threaded through
    // each slot's ready_next_ field (no separate storage array); head/tail/
    // count index that linked list. No per-push allocation (I9 / Decision 14).
    std::uint32_t ready_head_ = RequestSlot::kNotOnReadyRing;
    std::uint32_t ready_tail_ = RequestSlot::kNotOnReadyRing;
    std::size_t ready_count_ = 0;
};

}  // namespace sluice::async::detail
