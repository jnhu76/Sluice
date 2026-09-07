#pragma once

#include <sluice/async/detail/fail_fast.hpp>
#include <sluice/async/detail/ready_sink.hpp>
#include <sluice/async/detail/request_key.hpp>
#include <sluice/async/detail/request_slot.hpp>
#include <sluice/async/request_handle.hpp>
#include <sluice/error.hpp>
#include <sluice/result.hpp>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <mutex>
#include <optional>
#include <vector>

namespace sluice::async::detail {

struct SlotHandle {
    SlotIndex slot;
    Generation generation;
};

enum class CancelDisposition {
    terminal_won,
    intent_recorded,
    already_terminal,
    not_found,
    not_supported,
};

enum class EnqueueOutcome {
    enqueued,
    terminal_noop,
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

    ~RequestArena() {
        if (slot_in_use_ != 0) {
            request_arena_destruction_fail_fast();
        }
    }

    std::size_t capacity() const noexcept { return capacity_; }
    ContextIdentity context() const noexcept { return context_; }

    RequestHandleState identity_handle_state(SlotIndex slot, Generation gen,
                                             ContextIdentity ctx) const noexcept {
        std::lock_guard<std::mutex> lk(mutex_);
        if (slot.value >= capacity_)
            return RequestHandleState::not_found;
        const RequestSlot& s = slots_[slot.value];
        if (s.state_ == RequestState::free)
            return RequestHandleState::not_found;
        if (s.generation_ != gen)
            return RequestHandleState::not_found;
        if (s.key_.context != ctx)
            return RequestHandleState::not_found;
        switch (s.state_) {
        case RequestState::pending:
        case RequestState::enqueued:
        case RequestState::running:
        case RequestState::reserved:
        case RequestState::prepared:
            return RequestHandleState::outstanding;
        case RequestState::backend_ready:
            return RequestHandleState::backend_ready;
        case RequestState::completion_ready:
            return RequestHandleState::completion_ready;
        case RequestState::free:
        default:
            return RequestHandleState::not_found;
        }
    }

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

    std::size_t backend_ready_count() const noexcept {
        std::lock_guard<std::mutex> lk(mutex_);
        return backend_ready_count_;
    }

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)

    std::optional<std::size_t> try_accepted_outstanding() const noexcept {
        std::unique_lock<std::mutex> lk(mutex_, std::try_to_lock);
        if (!lk.owns_lock()) {
            return std::nullopt;
        }
        return accepted_outstanding_;
    }
    std::optional<std::size_t> try_backend_ready_count() const noexcept {
        std::unique_lock<std::mutex> lk(mutex_, std::try_to_lock);
        if (!lk.owns_lock()) {
            return std::nullopt;
        }
        return backend_ready_count_;
    }
#endif

    Result<SlotHandle> reserve() {
        std::lock_guard<std::mutex> lk(mutex_);
        if (admission_closed_) {
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
        if (slot_in_use_ > high_water_mark_)
            high_water_mark_ = slot_in_use_;
        return SlotHandle{SlotIndex{idx}, slot.generation_};
    }

    Result<void> prepare(SlotHandle h, OperationKind kind, BorrowMetadata borrow) {
        std::lock_guard<std::mutex> lk(mutex_);
        RequestSlot* s = validate_(h);
        if (!s)
            return make_unexpected<void>(IoError{IoError::Code::not_found});
        if (s->state_ != RequestState::reserved) {
            return make_unexpected<void>(IoError{IoError::Code::invalid_state});
        }
        s->op_kind_ = kind;
        s->borrow_ = borrow;
        s->borrow_.active = false;
        s->state_ = RequestState::prepared;
        return {};
    }

    Result<void>
    install_publication_binding(SlotHandle h, void* completion, std::uint64_t requested_bytes,
                                void (*publish)(void* completion, const TerminalResult&) noexcept) {
        std::lock_guard<std::mutex> lk(mutex_);
        RequestSlot* s = validate_(h);
        if (!s)
            return make_unexpected<void>(IoError{IoError::Code::not_found});
        if (s->state_ != RequestState::prepared) {
            return make_unexpected<void>(IoError{IoError::Code::invalid_state});
        }
        if (s->publication_binding_.installed()) {
            return make_unexpected<void>(IoError{IoError::Code::invalid_state});
        }
        if (publish == nullptr) {
            return make_unexpected<void>(IoError{IoError::Code::invalid_state});
        }

        if (completion == nullptr) {
            return make_unexpected<void>(IoError{IoError::Code::invalid_state});
        }
        s->publication_binding_ = {completion, requested_bytes, publish};
        return {};
    }

    Result<void> commit(SlotHandle h) {
        std::lock_guard<std::mutex> lk(mutex_);
        RequestSlot* s = validate_(h);
        if (!s)
            return make_unexpected<void>(IoError{IoError::Code::not_found});
        if (s->state_ != RequestState::prepared) {
            return make_unexpected<void>(IoError{IoError::Code::invalid_state});
        }
        s->state_ = RequestState::pending;
        s->enqueue_in_flight_pin_ = true;
        s->borrow_.active = true;

        s->submit_seq_ = next_submit_seq_++;
        ++accepted_outstanding_;
        return {};
    }

    EnqueueOutcome enqueue(SlotHandle h) noexcept {
        std::lock_guard<std::mutex> lk(mutex_);
        RequestSlot* s = validate_(h);
        if (!s)
            request_arena_enqueue_stale_fail_fast();
        if (s->state_ == RequestState::pending) {
            s->state_ = RequestState::enqueued;

            s->enqueue_in_flight_pin_ = false;
            return EnqueueOutcome::enqueued;
        }
        if (s->state_ == RequestState::backend_ready) {
            s->enqueue_in_flight_pin_ = false;
            return EnqueueOutcome::terminal_noop;
        }

        request_arena_enqueue_state_fail_fast();
    }

    bool mark_running(SlotHandle h) noexcept {
        std::lock_guard<std::mutex> lk(mutex_);
        RequestSlot* s = validate_(h);
        if (!s)
            request_arena_dispatch_stale_fail_fast();
        switch (s->state_) {
        case RequestState::enqueued:
            s->state_ = RequestState::running;
            return true;
        case RequestState::backend_ready:

            return false;
        default:
            request_arena_dispatch_state_fail_fast();
        }
    }

    std::size_t reap(SynchronousReadySink& sink) {
        std::size_t reaped = 0;
        for (;;) {
            ReadyEvent event{};
            bool publish_this = false;
            {
                std::lock_guard<std::mutex> lk(mutex_);
                std::optional<std::uint32_t> idx = peek_ready_front_locked_();
                if (!idx.has_value())
                    break;
                RequestSlot& s = slots_[*idx];

                if (s.enqueue_in_flight_pin_)
                    break;

                if (!s.publication_binding_.installed()) {
                    request_arena_missing_binding_fail_fast();
                }

                pop_ready_front_locked_();
                s.ready_next_ = RequestSlot::kNotOnReadyRing;

                s.registration_ = WaiterRegistration::closed;
                event = ReadyEvent{s.key_, s.op_kind_, OptionalWaiterDelivery::none()};
                if (s.waiter_delivery_present_) {
                    event.waiter =
                        OptionalWaiterDelivery::of(s.waiter_token_, std::move(s.waiter_lease_));
                    s.waiter_token_ = {};
                    s.waiter_delivery_present_ = false;
                }

                s.borrow_.active = false;
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
                borrow_end_at_ = ++trace_seq_;
#endif
                s.state_ = RequestState::completion_ready;
                --accepted_outstanding_;
                --backend_ready_count_;

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
                publish_at_ = ++trace_seq_;
#endif
                s.publication_binding_.publish(s.publication_binding_.completion, s.terminal_);
                publish_this = true;
                ++reaped;
            }
            if (publish_this) {
                sink.on_ready(std::move(event));
            }
        }
        return reaped;
    }

    bool record_terminal(SlotHandle h, TerminalResult result) noexcept {
        std::lock_guard<std::mutex> lk(mutex_);
        RequestSlot* s = validate_(h);
        if (!s)
            return false;
        if (!result.stored) {
            request_arena_invalid_terminal_fail_fast();
        }
        if (s->terminal_.stored)
            return false;
        if (s->state_ != RequestState::pending && s->state_ != RequestState::enqueued &&
            s->state_ != RequestState::running) {
            request_arena_terminal_state_fail_fast();
        }
        s->cancel_intent_ = false;

        s->terminal_ = result;
        s->state_ = RequestState::backend_ready;
        ++backend_ready_count_;
        push_ready_locked_(h.slot.value);
        return true;
    }

    bool record_canceled(SlotHandle h) noexcept {
        return record_terminal(h, TerminalResult::err(IoError{IoError::Code::canceled}));
    }

    Result<void> register_waiter(SlotHandle h, WaiterToken token, RoutingLease lease) {
        std::lock_guard<std::mutex> lk(mutex_);
        RequestSlot* s = validate_(h);
        if (!s)
            return make_unexpected<void>(IoError{IoError::Code::invalid_state});
        if (s->state_ != RequestState::pending && s->state_ != RequestState::enqueued &&
            s->state_ != RequestState::running && s->state_ != RequestState::backend_ready) {
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

    Result<RoutingLease> cancel_waiter(SlotHandle h) {
        std::lock_guard<std::mutex> lk(mutex_);
        RequestSlot* s = validate_(h);
        if (!s)
            return make_unexpected<RoutingLease>(IoError{IoError::Code::not_found});
        if (s->registration_ != WaiterRegistration::open_registered) {
            return make_unexpected<RoutingLease>(IoError{IoError::Code::not_found});
        }

        RoutingLease lease = std::move(s->waiter_lease_);
        s->waiter_token_ = {};
        s->registration_ = WaiterRegistration::open_no_waiter;
        s->waiter_delivery_present_ = false;
        return lease;
    }

    CancelDisposition cancel(SlotHandle h) noexcept {
        std::lock_guard<std::mutex> lk(mutex_);
        RequestSlot* s = validate_(h);
        if (!s)
            return CancelDisposition::not_found;

        if (s->terminal_.stored) {
            return CancelDisposition::already_terminal;
        }
        switch (s->state_) {
        case RequestState::pending:
        case RequestState::enqueued:

            s->cancel_intent_ = false;
            s->terminal_ = TerminalResult::err(IoError{IoError::Code::canceled});
            s->state_ = RequestState::backend_ready;
            ++backend_ready_count_;
            push_ready_locked_(h.slot.value);
            return CancelDisposition::terminal_won;
        case RequestState::running:

            s->cancel_intent_ = true;
            return CancelDisposition::intent_recorded;
        default:

            return CancelDisposition::not_found;
        }
    }

    bool cancel_intent_live(SlotIndex slot) const noexcept {
        check_slot_in_range_(slot);
        std::lock_guard<std::mutex> lk(mutex_);
        return slots_[slot.value].cancel_intent_;
    }

    Result<void> rollback_reserved_or_prepared(SlotHandle h) {
        std::lock_guard<std::mutex> lk(mutex_);
        RequestSlot* s = validate_(h);
        if (!s)
            return make_unexpected<void>(IoError{IoError::Code::not_found});
        if (s->state_ != RequestState::reserved && s->state_ != RequestState::prepared) {
            return make_unexpected<void>(IoError{IoError::Code::invalid_state});
        }
        free_slot_locked_(s, h.slot.value);
        return {};
    }

    void release_completed_binding(SlotHandle h) noexcept {
        std::lock_guard<std::mutex> lk(mutex_);
        RequestSlot* s = validate_(h);
        if (!s)
            request_slot_release_invariant_fail_fast();
        if (s->enqueue_in_flight_pin_) {
            request_slot_release_invariant_fail_fast();
        }
        if (s->registration_ == WaiterRegistration::open_registered) {
            request_slot_release_invariant_fail_fast();
        }
        if (s->state_ != RequestState::completion_ready) {
            request_slot_release_invariant_fail_fast();
        }
        free_slot_locked_(s, h.slot.value);
    }

    void close_admission() noexcept {
        std::lock_guard<std::mutex> lk(mutex_);
        admission_closed_ = true;
    }
    bool admission_closed() const noexcept {
        std::lock_guard<std::mutex> lk(mutex_);
        return admission_closed_;
    }

    struct ArenaQuiescence {
        std::size_t slot_in_use;
        std::size_t accepted_outstanding;
        std::size_t backend_ready;
    };
    ArenaQuiescence quiescence_snapshot() const noexcept {
        std::lock_guard<std::mutex> lk(mutex_);
        return {slot_in_use_, accepted_outstanding_, backend_ready_count_};
    }

    RequestKey key_of(SlotIndex slot) const noexcept {
        check_slot_in_range_(slot);
        std::lock_guard<std::mutex> lk(mutex_);
        return slots_[slot.value].key_;
    }

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

    std::optional<bool> terminal_stored(SlotHandle h) const noexcept {
        std::lock_guard<std::mutex> lk(mutex_);
        const RequestSlot* s = validate_(h);
        if (!s)
            return std::nullopt;
        return s->terminal_.stored;
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

    std::uint64_t requested_bytes_of(SlotIndex slot) const noexcept {
        check_slot_in_range_(slot);
        std::lock_guard<std::mutex> lk(mutex_);
        return slots_[slot.value].publication_binding_.requested_bytes;
    }

    std::uint64_t submit_seq_of(SlotIndex slot) const noexcept {
        check_slot_in_range_(slot);
        std::lock_guard<std::mutex> lk(mutex_);
        return slots_[slot.value].submit_seq_;
    }

    std::optional<SlotHandle> oldest_enqueued_of(OperationKind kind) const noexcept {
        std::lock_guard<std::mutex> lk(mutex_);
        std::optional<SlotHandle> best;
        std::uint64_t best_seq = 0;
        for (std::size_t i = 0; i < capacity_; ++i) {
            const RequestSlot& s = slots_[i];
            if (s.state_ != RequestState::enqueued)
                continue;
            if (s.op_kind_ != kind)
                continue;
            if (!best.has_value() || s.submit_seq_ < best_seq) {
                best = SlotHandle{SlotIndex{static_cast<std::uint32_t>(i)}, s.generation_};
                best_seq = s.submit_seq_;
            }
        }
        return best;
    }

    std::optional<SlotHandle> resolve_completion(const void* completion) const noexcept {
        std::lock_guard<std::mutex> lk(mutex_);
        for (std::size_t i = 0; i < capacity_; ++i) {
            const RequestSlot& s = slots_[i];
            if (s.state_ != RequestState::free && s.publication_binding_.completion == completion) {
                return SlotHandle{SlotIndex{static_cast<std::uint32_t>(i)}, s.generation_};
            }
        }
        return std::nullopt;
    }

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)

    struct RequestObservation {
        SlotHandle handle;
        RequestState state;
        bool enqueue_pin_live;
        bool terminal_stored;
    };
    std::optional<RequestObservation> observe_for_test(SlotHandle h) const noexcept {
        std::lock_guard<std::mutex> lk(mutex_);
        if (h.slot.value >= capacity_)
            return std::nullopt;
        const RequestSlot& s = slots_[h.slot.value];
        if (s.state_ == RequestState::free)
            return std::nullopt;
        if (s.generation_ != h.generation)
            return std::nullopt;
        if (s.key_.context != context_)
            return std::nullopt;
        return RequestObservation{h, s.state_, s.enqueue_in_flight_pin_, s.terminal_.stored};
    }

    struct BorrowSnapshot {
        int fd = -1;
        const void* address = nullptr;
        std::size_t length = 0;
        bool active = false;
    };
    std::optional<BorrowSnapshot> borrow_for_test(SlotHandle h) const noexcept {
        std::lock_guard<std::mutex> lk(mutex_);
        if (h.slot.value >= capacity_)
            return std::nullopt;
        const RequestSlot& s = slots_[h.slot.value];
        if (s.state_ == RequestState::free)
            return std::nullopt;
        if (s.generation_ != h.generation)
            return std::nullopt;
        if (s.key_.context != context_)
            return std::nullopt;
        return BorrowSnapshot{s.borrow_.fd, s.borrow_.address, s.borrow_.length, s.borrow_.active};
    }

    struct WaiterObservation {
        WaiterRegistration registration;
        bool delivery_present;
        WaiterToken token;
        std::uint64_t lease_id;
    };
    std::optional<WaiterObservation> waiter_for_test(SlotHandle h) const noexcept {
        std::lock_guard<std::mutex> lk(mutex_);
        if (h.slot.value >= capacity_)
            return std::nullopt;
        const RequestSlot& s = slots_[h.slot.value];
        if (s.state_ == RequestState::free)
            return std::nullopt;
        if (s.generation_ != h.generation)
            return std::nullopt;
        if (s.key_.context != context_)
            return std::nullopt;
        return WaiterObservation{s.registration_, s.waiter_delivery_present_, s.waiter_token_,
                                 s.waiter_lease_.id()};
    }

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

    std::uint64_t trace_seq_ = 0;
    std::uint64_t borrow_end_at_ = 0;
    std::uint64_t publish_at_ = 0;
#endif

    void check_slot_in_range_(SlotIndex slot) const noexcept {
        if (slot.value >= capacity_)
            request_arena_slot_index_out_of_range_fail_fast();
    }

    RequestSlot* validate_(SlotHandle h) noexcept {
        if (h.slot.value >= capacity_)
            return nullptr;
        RequestSlot& s = slots_[h.slot.value];
        if (s.generation_ != h.generation)
            return nullptr;
        if (s.state_ == RequestState::free)
            return nullptr;
        if (s.key_.context != context_)
            return nullptr;
        return &s;
    }
    const RequestSlot* validate_(SlotHandle h) const noexcept {
        if (h.slot.value >= capacity_)
            return nullptr;
        const RequestSlot& s = slots_[h.slot.value];
        if (s.generation_ != h.generation)
            return nullptr;
        if (s.state_ == RequestState::free)
            return nullptr;
        if (s.key_.context != context_)
            return nullptr;
        return &s;
    }

    void push_ready_locked_(std::uint32_t idx) noexcept {
        RequestSlot& s = slots_[idx];
        const bool already_tail = ready_count_ != 0 && ready_tail_ == idx;
        const bool ends_consistent = ready_count_ == 0
                                         ? (ready_head_ == RequestSlot::kNotOnReadyRing &&
                                            ready_tail_ == RequestSlot::kNotOnReadyRing)
                                         : (ready_head_ < capacity_ && ready_tail_ < capacity_);
        if (s.state_ != RequestState::backend_ready || !s.terminal_.stored ||
            s.ready_next_ != RequestSlot::kNotOnReadyRing || already_tail || !ends_consistent ||
            ready_count_ >= capacity_) {
            request_arena_ready_ring_invariant_fail_fast();
        }

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
        if (ready_count_ == 0)
            return std::nullopt;
        return ready_head_;
    }
    void pop_ready_front_locked_() noexcept {
        std::uint32_t idx = ready_head_;
        std::uint32_t nxt = slots_[idx].ready_next_;
        ready_head_ = nxt;
        --ready_count_;
        if (ready_count_ == 0)
            ready_tail_ = RequestSlot::kNotOnReadyRing;
    }

    void free_slot_locked_(RequestSlot* s, std::uint32_t idx) noexcept {
        s->state_ = RequestState::free;

        if (s->generation_.value == std::numeric_limits<std::uint64_t>::max()) {
            request_arena_generation_exhausted_fail_fast();
        }
        s->generation_ = Generation{s->generation_.value + 1};
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

    std::uint64_t next_submit_seq_ = 1;

    std::uint32_t ready_head_ = RequestSlot::kNotOnReadyRing;
    std::uint32_t ready_tail_ = RequestSlot::kNotOnReadyRing;
    std::size_t ready_count_ = 0;
};

} // namespace sluice::async::detail
