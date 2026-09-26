#include <sluice/async/uring_backend.hpp>

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
#include "tax0_ablation_seams.hpp"
#endif

#include <sluice/async/detail/fail_fast.hpp>
#include <sluice/detail/file_semantics.hpp>
#include <sluice/detail/uring_submit.hpp>
#include <sluice/error.hpp>
#include <sluice/measurement.hpp>
#include <sluice/result.hpp>

#include <cstdio>
#include <limits>
#include <utility>

#if defined(SLUICE_HAS_LIBURING)
#include <liburing.h>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <thread>
#endif

namespace sluice::async {

#if !defined(SLUICE_HAS_LIBURING)

UringAsyncBackend::UringAsyncBackend(unsigned queue_depth) : available_(false) {
    (void)queue_depth;
}

UringAsyncBackend::~UringAsyncBackend() = default;

Result<detail::RequestKey> UringAsyncBackend::submit_read(ReadOp, Completion<std::size_t>*) {
    return make_unexpected<detail::RequestKey>(IoError{IoError::Code::backend_error});
}
Result<detail::RequestKey> UringAsyncBackend::submit_write(WriteOp, Completion<std::size_t>*) {
    return make_unexpected<detail::RequestKey>(IoError{IoError::Code::backend_error});
}
Result<detail::RequestKey> UringAsyncBackend::submit_sync_data(SyncDataOp, Completion<void>*) {
    return make_unexpected<detail::RequestKey>(IoError{IoError::Code::backend_error});
}
Result<detail::RequestKey> UringAsyncBackend::submit_sync_all(SyncAllOp, Completion<void>*) {
    return make_unexpected<detail::RequestKey>(IoError{IoError::Code::backend_error});
}

std::size_t UringAsyncBackend::poll() {
    return 0;
}
Result<std::size_t> UringAsyncBackend::wait_one() {
    return std::size_t{0};
}
void UringAsyncBackend::cancel(Completion<std::size_t>&) {}
void UringAsyncBackend::cancel(Completion<void>&) {}

Result<void> UringAsyncBackend::register_waiter(Completion<std::size_t>&, detail::WaiterToken,
                                                detail::RoutingLease) {
    return make_unexpected<void>(IoError{IoError::Code::not_supported});
}
Result<void> UringAsyncBackend::register_waiter(Completion<void>&, detail::WaiterToken,
                                                detail::RoutingLease) {
    return make_unexpected<void>(IoError{IoError::Code::not_supported});
}
Result<detail::RoutingLease> UringAsyncBackend::cancel_waiter(Completion<std::size_t>&) {
    return make_unexpected<detail::RoutingLease>(IoError{IoError::Code::not_supported});
}
Result<detail::RoutingLease> UringAsyncBackend::cancel_waiter(Completion<void>&) {
    return make_unexpected<detail::RoutingLease>(IoError{IoError::Code::not_supported});
}
void UringAsyncBackend::close_admission() {}
std::size_t UringAsyncBackend::outstanding() const noexcept {
    return 0;
}
bool UringAsyncBackend::available() const noexcept {
    return available_;
}

#else

struct UringRingState {
    ::io_uring ring{};
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
    UringBackendSubmitTestHooks test_hooks{};
#endif
};

namespace {

constexpr std::uint64_t CONTROL_TAG = std::uint64_t{1} << 63u;
constexpr std::uint64_t COOKIE_MASK = CONTROL_TAG - 1u;

constexpr bool is_control_cookie(std::uint64_t user_data) noexcept {
    return (user_data & CONTROL_TAG) != 0;
}

constexpr std::uint64_t make_control_cookie(std::uint64_t operation_cookie) noexcept {
    return CONTROL_TAG | operation_cookie;
}

constexpr std::uint64_t control_target_cookie(std::uint64_t user_data) noexcept {
    return user_data & COOKIE_MASK;
}

inline void bump(sluice::AsyncStats* s, std::uint64_t sluice::AsyncStats::* field) {
    if (s)
        ++(s->*field);
}

constexpr bool cookie_terminal_is_canceled(const detail::TerminalResult& t) noexcept {
    return t.stored && t.is_error && t.error.code == IoError::Code::canceled;
}

}
class UringAsyncBackend::BoundedDispatchQueue {
  public:
    explicit BoundedDispatchQueue(std::size_t capacity) : storage_(capacity), capacity_(capacity) {}
    bool empty() const noexcept { return size_ == 0; }
    std::size_t size() const noexcept { return size_; }

    detail::SlotHandle front() const noexcept {
        if (size_ == 0) {
            std::fprintf(stderr, "sluice::async::UringAsyncBackend: ring "
                                 "front() on empty queue (invariant violation)\n");
            std::fflush(stderr);
            std::terminate();
        }
        return storage_[head_];
    }

    void push_back(detail::SlotHandle h) noexcept {
        if (size_ >= capacity_) {
            std::fprintf(stderr, "sluice::async::UringAsyncBackend: ring overflow "
                                 "(invariant violation)\n");
            std::fflush(stderr);
            std::terminate();
        }
        storage_[(head_ + size_) % capacity_] = h;
        ++size_;
    }
    bool pop_front(detail::SlotHandle& out) noexcept {
        if (size_ == 0)
            return false;
        out = storage_[head_];
        head_ = (head_ + 1) % capacity_;
        --size_;
        return true;
    }

    bool remove_exact(detail::SlotHandle h) noexcept {
        for (std::size_t i = 0; i < size_; ++i) {
            std::size_t idx = (head_ + i) % capacity_;
            if (storage_[idx].slot.value == h.slot.value &&
                storage_[idx].generation.value == h.generation.value) {
                for (std::size_t j = i; j + 1 < size_; ++j) {
                    std::size_t a = (head_ + j) % capacity_;
                    std::size_t b = (head_ + j + 1) % capacity_;
                    storage_[a] = storage_[b];
                }
                --size_;
                return true;
            }
        }
        return false;
    }

  private:
    std::vector<detail::SlotHandle> storage_;
    std::size_t head_ = 0;
    std::size_t size_ = 0;
    std::size_t capacity_;
};

class UringAsyncBackend::TransportLedger {
  public:
    enum class Kind : std::uint8_t { operation, cancel_control };

    struct Entry {
        std::uint64_t sequence = 0;
        std::uint32_t physical_position = 0;
        Kind kind = Kind::operation;
        std::uint64_t cookie = 0;
        detail::SlotHandle handle{};
        bool class_a_recovery_retired = false;
    };

    explicit TransportLedger(std::size_t capacity) : storage_(capacity), capacity_(capacity) {
        if (capacity_ == 0) {
            std::fprintf(stderr, "sluice::async::UringAsyncBackend: zero-capacity transport "
                                 "ledger (invariant violation)\n");
            std::fflush(stderr);
            std::terminate();
        }
    }

    bool empty() const noexcept { return size_ == 0; }
    std::size_t size() const noexcept { return size_; }

    void append(Kind kind, std::uint32_t physical_position, std::uint64_t cookie,
                detail::SlotHandle handle) noexcept {
        const std::uint32_t expected_physical =
            last_sequence_ == 0
                ? 0
                : static_cast<std::uint32_t>(
                      (static_cast<std::uint64_t>(last_physical_position_) + 1u) % capacity_);
        if (size_ >= capacity_ || physical_position >= capacity_ || next_sequence_ == 0 ||
            next_sequence_ == std::numeric_limits<std::uint64_t>::max() ||
            physical_position != expected_physical || next_sequence_ != last_sequence_ + 1u) {
            std::fprintf(stderr, "sluice::async::UringAsyncBackend: transport ledger "
                                 "overflow/non-monotonic physical sequence/sequence exhaustion "
                                 "(invariant violation)\n");
            std::fflush(stderr);
            std::terminate();
        }
        storage_[(head_ + size_) % capacity_] =
            Entry{next_sequence_++, physical_position, kind, cookie, handle, false};
        last_sequence_ = next_sequence_ - 1u;
        last_physical_position_ = physical_position;
        ++size_;
    }

    Entry pop_front() noexcept {
        if (size_ == 0) {
            std::fprintf(stderr, "sluice::async::UringAsyncBackend: transport ledger "
                                 "underflow (invariant violation)\n");
            std::fflush(stderr);
            std::terminate();
        }
        Entry out = storage_[head_];
        if (out.sequence != retired_prefix_sequence_ + 1u) {
            std::fprintf(stderr, "sluice::async::UringAsyncBackend: transport ledger retired a "
                                 "non-monotonic logical prefix (invariant violation)\n");
            std::fflush(stderr);
            std::terminate();
        }
        retired_prefix_sequence_ = out.sequence;
        head_ = (head_ + 1) % capacity_;
        --size_;
        return out;
    }

    Entry& at(std::size_t offset) noexcept {
        if (offset >= size_) {
            std::fprintf(stderr, "sluice::async::UringAsyncBackend: transport ledger index "
                                 "out of range (invariant violation)\n");
            std::fflush(stderr);
            std::terminate();
        }
        return storage_[(head_ + offset) % capacity_];
    }

    bool all_class_a_recovery_retired() const noexcept {
        for (std::size_t i = 0; i < size_; ++i) {
            if (!storage_[(head_ + i) % capacity_].class_a_recovery_retired)
                return false;
        }
        return true;
    }

  private:
    std::vector<Entry> storage_;
    std::size_t head_ = 0;
    std::size_t size_ = 0;
    std::size_t capacity_ = 0;
    std::uint64_t next_sequence_ = 1;
    std::uint64_t last_sequence_ = 0;
    std::uint64_t retired_prefix_sequence_ = 0;
    std::uint32_t last_physical_position_ = 0;
};

Result<void> UringAsyncBackend::validate_read(ReadOp op) {
    const sluice::detail::DataOpVerdict verdict = sluice::detail::precheck_data_op(
        {op.file.fd < 0, op.file.access, sluice::detail::FileOperation::read, op.offset, op.len});
    if (verdict == sluice::detail::DataOpVerdict::execute && op.dst == nullptr)
        return make_unexpected<void>(IoError{.code = IoError::Code::invalid_argument});
    return sluice::detail::accept_or_reject(verdict);
}
Result<void> UringAsyncBackend::validate_write(WriteOp op) {
    const sluice::detail::DataOpVerdict verdict = sluice::detail::precheck_data_op(
        {op.file.fd < 0, op.file.access, sluice::detail::FileOperation::write, op.offset, op.len});
    if (verdict == sluice::detail::DataOpVerdict::execute && op.src == nullptr)
        return make_unexpected<void>(IoError{.code = IoError::Code::invalid_argument});
    return sluice::detail::accept_or_reject(verdict);
}
Result<void> UringAsyncBackend::validate_sync(SyncDataOp op) {
    return sluice::detail::accept_or_reject(sluice::detail::precheck_state_op(
        op.file.fd < 0, op.file.access, sluice::detail::FileOperation::sync_data));
}
Result<void> UringAsyncBackend::validate_sync(SyncAllOp op) {
    return sluice::detail::accept_or_reject(sluice::detail::precheck_state_op(
        op.file.fd < 0, op.file.access, sluice::detail::FileOperation::sync_all));
}

template <class Op> Result<void> UringAsyncBackend::validate_op(const Op& op) noexcept {
    if constexpr (std::is_same_v<Op, ReadOp>) {
        return validate_read(op);
    } else if constexpr (std::is_same_v<Op, WriteOp>) {
        return validate_write(op);
    } else {
        return validate_sync(op);
    }
}

void UringAsyncBackend::publish_size_ready(void* completion,
                                           const sluice::detail::IoOutcome& outcome) noexcept {
    Result<std::size_t> result =
        outcome.succeeded
            ? Result<std::size_t>{static_cast<std::size_t>(outcome.effect.confirmed_bytes)}
            : make_unexpected<std::size_t>(outcome.error);
    AsyncBackend::publish(*static_cast<Completion<std::size_t>*>(completion), std::move(result));
}

void UringAsyncBackend::publish_void_ready(void* completion,
                                           const sluice::detail::IoOutcome& outcome) noexcept {
    Result<void> result = outcome.succeeded ? Result<void>{} : make_unexpected<void>(outcome.error);
    AsyncBackend::publish(*static_cast<Completion<void>*>(completion), std::move(result));
}

void UringAsyncBackend::publish_request_ready(void* completion,
                                              const sluice::detail::IoOutcome& outcome) noexcept {
    (void)completion;
    (void)outcome;
}

UringAsyncBackend::UringAsyncBackend(unsigned queue_depth)
    : UringAsyncBackend(UringConfig{static_cast<std::size_t>(queue_depth > 0 ? queue_depth : 64),
                                    queue_depth > 0 ? queue_depth : 64}) {}

UringAsyncBackend::UringAsyncBackend(UringConfig config)
    : UringAsyncBackend(validate_config_(config), ValidatedConfigTag{}) {}

UringConfig UringAsyncBackend::validate_config_(UringConfig config) {
    constexpr std::size_t slot_index_max =
        static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max());
    if (config.request_capacity == 0 || config.request_capacity > slot_index_max ||
        config.queue_depth == 0) {
        throw std::invalid_argument(
            "UringConfig request_capacity must be in [1, UINT32_MAX] and queue_depth must be > 0");
    }
    return config;
}

UringAsyncBackend::UringAsyncBackend(UringConfig config, ValidatedConfigTag)
    : capacity_(config.request_capacity), prepared_ops_(config.request_capacity),
      delivery_(config.request_capacity), router_(config.request_capacity),
      cookie_free_list_(config.request_capacity),
      ring_state_(std::make_unique<UringRingState>()) {
    for (std::uint32_t i = 0; i < config.request_capacity; ++i) {
        cookie_free_list_[i] = detail::SlotIndex{i};
    }
    dispatch_ = std::make_unique<BoundedDispatchQueue>(config.request_capacity);
    publication_pending_ = std::make_unique<BoundedDispatchQueue>(config.request_capacity);
    if (::io_uring_queue_init(config.queue_depth, &ring_state_->ring, 0) == 0) {
        try {
            transport_ledger_ =
                std::make_unique<TransportLedger>(ring_state_->ring.sq.ring_entries);
            wait_source_ = std::make_unique<detail::UringWaitSource>();
            wait_source_->set_ring_fd(ring_state_->ring.ring_fd);
        } catch (...) {
            ::io_uring_queue_exit(&ring_state_->ring);
            throw;
        }
        have_ring_ = true;
        available_ = true;
    }
}

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
UringAsyncBackend::UringAsyncBackend(UringConfig config, UringBackendSubmitTestHooks hooks)
    : UringAsyncBackend(config) {
    ring_state_->test_hooks = hooks;
}
#endif

UringAsyncBackend::~UringAsyncBackend() {
    {
        std::lock_guard<std::mutex> lk(dispatch_mtx_);
        const detail::CoreOccupancy occupancy =
            core_ != nullptr ? core_->occupancy() : detail::CoreOccupancy{};
        const bool ledger_quiescent =
            transport_ledger_ == nullptr || transport_ledger_->empty() ||
            (fatal_error_.has_value() && transport_ledger_->all_class_a_recovery_retired());
        if (!dispatch_->empty() || !publication_pending_->empty() ||
            live_cookies_.load(std::memory_order_relaxed) != 0 ||
            live_control_sqes_.load(std::memory_order_relaxed) != 0 || !ledger_quiescent ||
            occupancy.accepted_live != 0) {
            detail::uring_non_quiescent_destruction_fail_fast();
        }
    }
    if (have_ring_) {
        ::io_uring_queue_exit(&ring_state_->ring);
        have_ring_ = false;
    }
}

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
std::size_t UringAsyncBackend::dispatch_size_for_test() const noexcept {
    std::lock_guard<std::mutex> lk(dispatch_mtx_);
    return dispatch_->size();
}

std::size_t UringAsyncBackend::transport_ledger_size_for_test() const noexcept {
    std::lock_guard<std::mutex> lk(dispatch_mtx_);
    return transport_ledger_ == nullptr ? 0 : transport_ledger_->size();
}

std::size_t UringAsyncBackend::sq_ready_for_test() const noexcept {
    std::lock_guard<std::mutex> lk(dispatch_mtx_);
    return have_ring_ ? static_cast<std::size_t>(::io_uring_sq_ready(&ring_state_->ring)) : 0;
}

std::size_t UringAsyncBackend::live_control_sqes_for_test() const noexcept {
    return live_control_sqes_.load(std::memory_order_relaxed);
}
#endif

template <class Op, class Comp>
Result<detail::RequestKey> UringAsyncBackend::submit_request(Op op, Comp* c,
                                                             detail::OperationKind kind,
                                                             detail::RequestOp core_op) {
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)

    wait_submit_entry_pause_();
#endif
    if (auto v = validate_op(op); !v.has_value()) {
        return make_unexpected<detail::RequestKey>(v.error());
    }
    if (!have_ring_) {
        return make_unexpected<detail::RequestKey>(IoError{IoError::Code::backend_error});
    }
    if (fatal_error_.has_value()) {
        return make_unexpected<detail::RequestKey>(*fatal_error_);
    }

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)

    if (auto inj = injected_precommit_stage_failure_(SubmitStage::reserve); inj.has_value()) {
        return make_unexpected<detail::RequestKey>(*inj);
    }
#endif

    const auto reservation = core_->reserve();
    if (!reservation.ok()) {
        const IoError::Code code = reservation.status == detail::ReserveStatus::admission_closed
                                       ? IoError::Code::invalid_state
                                       : IoError::Code::would_block;
        return make_unexpected<detail::RequestKey>(IoError{code});
    }
    const detail::SlotHandle h{reservation.reservation.slot, reservation.reservation.generation};

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)

    if (auto inj = injected_precommit_stage_failure_(SubmitStage::prepare); inj.has_value()) {
        (void)core_->rollback(reservation.reservation);
        return make_unexpected<detail::RequestKey>(*inj);
    }
#endif

    std::uint64_t length = 0;
    std::uint64_t offset = 0;
    bool zero_op = false;
    if constexpr (std::is_same_v<Op, ReadOp> || std::is_same_v<Op, WriteOp>) {
        length = op.len;
        offset = op.len == 0 ? std::uint64_t{0} : op.offset;
        zero_op = op.len == 0;
    }
#if defined(SLUICE_B1C_MUTANT_PREMATURE_ZERO_OP_DISPATCH)
    zero_op = false;
#endif
    prepared_ops_[h.slot.value] =
        PreparedUringOp{kind, op.file.fd, buffer_of(op), static_cast<std::size_t>(length),
                        sluice::detail::uring_chunk_length(static_cast<std::size_t>(length)),
                        offset};

    DeliveryRecord& record = delivery_[h.slot.value];
    record.completion = c;
    record.publish = c != nullptr ? publish_thunk<Comp>() : &UringAsyncBackend::publish_request_ready;
    record.kind = kind;
    record.registration = detail::WaiterRegistration::open_no_waiter;
    record.waiter_token = {};
    record.waiter_lease = {};
    record.waiter_delivery_present = false;
    record.event_owed = false;
    record.owed_key = {};

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)

    if (auto inj = injected_precommit_stage_failure_(SubmitStage::commit); inj.has_value()) {
        (void)core_->rollback(reservation.reservation);
        return make_unexpected<detail::RequestKey>(*inj);
    }

    wait_pre_accept_commit_pause_();
#endif

    if (c != nullptr && !begin_binding(*c)) {
        (void)core_->rollback(reservation.reservation);
        return make_unexpected<detail::RequestKey>(IoError{IoError::Code::invalid_state});
    }

    const detail::RequestDescriptor descriptor{core_op, offset, length, zero_op};
    const detail::BorrowFacts borrow{op.file.fd, buffer_of(op), length};
    const auto accepted = core_->accept(reservation.reservation, descriptor, borrow);
    if (!accepted.ok()) {
        if (c != nullptr) {
            rollback_binding_before_accept(*c);
        }
        (void)core_->rollback(reservation.reservation);
        return make_unexpected<detail::RequestKey>(IoError{IoError::Code::invalid_state});
    }

    if (c != nullptr) {
        install_core_binding(*c, core_, accepted.id);
        commit_binding(*c);
    }

    if (descriptor.zero_op) {
        publish_zero_op_inline(accepted.id, h);
    } else {
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)

        wait_accepted_pre_dispatch_pause_();
#endif
        dispatch_after_accept(h);
    }
    return accepted.id;
}

Result<detail::RequestKey> UringAsyncBackend::submit_read(ReadOp op, Completion<std::size_t>* c) {
    return submit_request(op, c, detail::OperationKind::read, detail::RequestOp::read);
}
Result<detail::RequestKey> UringAsyncBackend::submit_write(WriteOp op, Completion<std::size_t>* c) {
    return submit_request(op, c, detail::OperationKind::write, detail::RequestOp::write);
}
Result<detail::RequestKey> UringAsyncBackend::submit_sync_data(SyncDataOp op, Completion<void>* c) {
    return submit_request(op, c, detail::OperationKind::sync_data, detail::RequestOp::sync_data);
}
Result<detail::RequestKey> UringAsyncBackend::submit_sync_all(SyncAllOp op, Completion<void>* c) {
    return submit_request(op, c, detail::OperationKind::sync_all, detail::RequestOp::sync_all);
}

void UringAsyncBackend::dispatch_after_accept(detail::SlotHandle h) noexcept {
    bool injected_dispatch_failure = false;
    bool newly_poisoned = false;
    {
        std::lock_guard<std::mutex> lk(dispatch_mtx_);
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)

        auto* inj = dispatch_failure_injection_.load(std::memory_order_acquire);
        if (inj != nullptr && inj->armed.load(std::memory_order_acquire)) {
            inj->fired.fetch_add(1, std::memory_order_relaxed);
            const detail::RequestKey key{core_->context(), h.slot, h.generation};
            detail::TerminalCandidate candidate;
            candidate.kind = detail::TerminalCandidateKind::physical_outcome;
            candidate.outcome = sluice::detail::IoOutcome::failure(
                IoError{IoError::Code::backend_error});
            if (core_->offer_terminal(key, candidate) != detail::TerminalVerdict::chosen) {
                detail::uring_core_handoff_fail_fast();
            }
            if (core_->release_execution(key) !=
                detail::ExecutionRelease::borrow_touch_fully_retired) {
                detail::uring_core_handoff_fail_fast();
            }
            publication_pending_->push_back(h);
            injected_dispatch_failure = true;
        } else
#endif
        {
            dispatch_->push_back(h);

            const bool poisoned_before = fatal_error_.has_value();
            for (;;) {
                if (dispatch_->empty())
                    break;
                const detail::SlotHandle front = dispatch_->front();
                if (!dispatch_one_locked(front))
                    break;
            }
            newly_poisoned = !poisoned_before && fatal_error_.has_value();
        }
    }
    if (injected_dispatch_failure || newly_poisoned) {
        signal_ready_progress();
    }
}

bool UringAsyncBackend::dispatch_one_locked(detail::SlotHandle h) noexcept {
    if (fatal_error_.has_value())
        return false;

    if (cookie_free_list_.empty()) {
        std::fprintf(stderr, "sluice::async::UringAsyncBackend: router exhaustion "
                             "(invariant violation)\n");
        std::fflush(stderr);
        std::terminate();
    }

    io_uring_sqe* sqe = ::io_uring_get_sqe(&ring_state_->ring);
    if (sqe == nullptr) {
        (void)submit_transport_locked();
        if (fatal_error_.has_value())
            return false;
        sqe = ::io_uring_get_sqe(&ring_state_->ring);
        if (sqe == nullptr)
            return false;
    }

    const detail::RequestKey key{core_->context(), h.slot, h.generation};
    if (core_->claim_execution(key) != detail::ExecutionClaim::claimed) {
        // The SQE slot was taken but nothing was written into it and no
        // submission occurred, so returning it to the userspace tail keeps
        // the ring accounting exact.
        --ring_state_->ring.sq.sqe_tail;
        (void)dispatch_->remove_exact(h);
        return true;
    }

    detail::SlotIndex router_slot = cookie_free_list_.back();
    cookie_free_list_.pop_back();

    const std::uint64_t op_cookie = allocate_cookie_();
    const PreparedUringOp& prep = prepared_ops_[h.slot.value];
    switch (prep.kind) {
    case detail::OperationKind::read:
        ::io_uring_prep_read(sqe, prep.fd, const_cast<std::byte*>(prep.buffer), prep.native_length,
                             static_cast<off_t>(static_cast<std::int64_t>(prep.offset)));
        break;
    case detail::OperationKind::write:
        ::io_uring_prep_write(sqe, prep.fd, prep.buffer, prep.native_length,
                              static_cast<off_t>(static_cast<std::int64_t>(prep.offset)));
        break;
    case detail::OperationKind::sync_data:
        ::io_uring_prep_fsync(sqe, prep.fd, IORING_FSYNC_DATASYNC);
        break;
    case detail::OperationKind::sync_all:
        ::io_uring_prep_fsync(sqe, prep.fd, 0);
        break;
    }

    ::io_uring_sqe_set_data64(sqe, op_cookie);
    RouterEntry& route = router_[router_slot.value];
    route = RouterEntry{};
    route.cookie = op_cookie;
    route.handle = h;
    route.in_use = true;
    live_cookies_.fetch_add(1, std::memory_order_relaxed);
    const auto& sq = ring_state_->ring.sq;
    const std::uint32_t physical_position =
        static_cast<std::uint32_t>((sq.sqe_tail - 1u) & sq.ring_mask);
    transport_ledger_->append(TransportLedger::Kind::operation, physical_position, op_cookie, h);

    if (!dispatch_->remove_exact(h)) {
        std::fprintf(stderr, "sluice::async::UringAsyncBackend: dispatch_one_locked "
                             "remove_exact miss after claim (invariant violation)\n");
        std::fflush(stderr);
        std::terminate();
    }

    return true;
}

int UringAsyncBackend::submit_transport_locked() noexcept {
    if (fatal_error_.has_value() || transport_ledger_ == nullptr || transport_ledger_->empty())
        return 0;

    submit_flushes_.fetch_add(1, std::memory_order_relaxed);
    int rc = 0;
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
    if (ring_state_->test_hooks.submit != nullptr) {
        rc = ring_state_->test_hooks.submit(ring_state_->test_hooks.context, &ring_state_->ring);
    } else {
        rc = ::io_uring_submit(&ring_state_->ring);
    }
#else
    rc = ::io_uring_submit(&ring_state_->ring);
#endif
    account_transport_result_locked(rc, true);
    return rc;
}

void UringAsyncBackend::account_transport_result_locked(int rc,
                                                        bool had_pending_transport) noexcept {
    if (!had_pending_transport)
        return;

    if (rc > 0) {
        const std::size_t consumed = static_cast<std::size_t>(rc);
        if (consumed > transport_ledger_->size()) {
            std::fprintf(stderr, "sluice::async::UringAsyncBackend: submit consumed more SQEs "
                                 "than the physical ledger contains (invariant violation)\n");
            std::fflush(stderr);
            std::terminate();
        }
        for (std::size_t i = 0; i < consumed; ++i) {
            const TransportLedger::Entry entry = transport_ledger_->pop_front();
            if (entry.kind == TransportLedger::Kind::cancel_control) {
                const std::size_t router_index = find_live_router_cookie_(entry.cookie);
                if (router_index == router_.size() ||
                    router_[router_index].handle.slot.value != entry.handle.slot.value ||
                    router_[router_index].handle.generation.value !=
                        entry.handle.generation.value ||
                    router_[router_index].control_state != RouterEntry::ControlState::prepared) {
                    std::fprintf(stderr, "sluice::async::UringAsyncBackend: consumed control "
                                         "lost its exact prepared router reference "
                                         "(invariant violation)\n");
                    std::fflush(stderr);
                    std::terminate();
                }
                router_[router_index].control_state = RouterEntry::ControlState::submitted;
                live_control_sqes_.fetch_add(1, std::memory_order_relaxed);
            }
        }
        return;
    }
    if (rc == 0)
        return;

    const int err = -rc;
    if (err == EINTR || err == EAGAIN || err == EBUSY)
        return;

    poison_and_recover_locked(IoError{IoError::Code::backend_error, err});
}

void UringAsyncBackend::poison_and_recover_locked(IoError error) noexcept {
    if (fatal_error_.has_value())
        return;
    fatal_error_ = error;
    core_->note_health_failure();

    // Kernel-invisible work converges immediately: dispatch entries never
    // reached an SQE and prepared ledger entries are never submitted again
    // after poison. Submitted operations whose CQEs may still arrive are
    // already outside this ledger and keep their real completion path.
    detail::SlotHandle local{};
    while (dispatch_->pop_front(local)) {
        const detail::RequestKey key{core_->context(), local.slot, local.generation};
        detail::TerminalCandidate candidate;
        candidate.kind = detail::TerminalCandidateKind::physical_outcome;
        candidate.outcome = sluice::detail::IoOutcome::failure(error);
        if (core_->offer_terminal(key, candidate) != detail::TerminalVerdict::chosen) {
            std::fprintf(stderr, "sluice::async::UringAsyncBackend: local poison retirement "
                                 "lost terminal authority (invariant violation)\n");
            std::fflush(stderr);
            std::terminate();
        }
        if (core_->release_execution(key) !=
            detail::ExecutionRelease::borrow_touch_fully_retired) {
            detail::uring_core_handoff_fail_fast();
        }
        publication_pending_->push_back(local);
        bump(stats_, &AsyncStats::completion_errors);
    }

#if defined(SLUICE_B1C_MUTANT_STRAND_POST_ACCEPT_SUBMIT_FAILURE)
    for (std::size_t i = 0; i < 0; ++i)
#else
    for (std::size_t i = 0; i < transport_ledger_->size(); ++i)
#endif
    {
        TransportLedger::Entry& physical = transport_ledger_->at(i);
        if (physical.class_a_recovery_retired) {
            std::fprintf(stderr, "sluice::async::UringAsyncBackend: duplicate Class-A "
                                 "recovery retirement (invariant violation)\n");
            std::fflush(stderr);
            std::terminate();
        }

        const std::size_t router_index = find_live_router_cookie_(physical.cookie);
        if (router_index == router_.size() ||
            router_[router_index].handle.slot.value != physical.handle.slot.value ||
            router_[router_index].handle.generation.value != physical.handle.generation.value) {
            std::fprintf(stderr, "sluice::async::UringAsyncBackend: Class-A "
                                 "recovery lost identity "
                                 "(invariant violation)\n");
            std::fflush(stderr);
            std::terminate();
        }
        RouterEntry& route = router_[router_index];
        const detail::RequestKey key{core_->context(), route.handle.slot, route.handle.generation};

        if (physical.kind == TransportLedger::Kind::operation) {
            if (!route.terminal_delivered) {
                detail::TerminalCandidate candidate;
                candidate.kind = detail::TerminalCandidateKind::physical_outcome;
                candidate.outcome = sluice::detail::IoOutcome::failure(error);
                if (core_->offer_terminal(key, candidate) != detail::TerminalVerdict::chosen) {
                    std::fprintf(stderr, "sluice::async::UringAsyncBackend: Class-A operation "
                                         "recovery lost terminal authority (invariant "
                                         "violation)\n");
                    std::fflush(stderr);
                    std::terminate();
                }
                if (core_->release_execution(key) !=
                    detail::ExecutionRelease::borrow_touch_fully_retired) {
                    detail::uring_core_handoff_fail_fast();
                }
                route.terminal_delivered = true;
                publication_pending_->push_back(route.handle);
                bump(stats_, &AsyncStats::completion_errors);
            }
        } else {
            if (route.control_state == RouterEntry::ControlState::submitted) {
                live_control_sqes_.fetch_sub(1, std::memory_order_relaxed);
            }
            if (route.control_state != RouterEntry::ControlState::none) {
                route.control_state = RouterEntry::ControlState::none;
                if (core_->release_control(key) != detail::ControlRelease::released) {
                    detail::uring_core_handoff_fail_fast();
                }
            }
        }
        // Retire only when the original outcome is already delivered; an
        // entry still waiting for its original CQE must stay routable or
        // that CQE would strand the request.
        if (route.terminal_delivered && route.control_state == RouterEntry::ControlState::none) {
            retire_router_entry_(router_index);
        }
        physical.class_a_recovery_retired = true;
    }

    signal_ready_progress();
}

std::uint64_t UringAsyncBackend::allocate_cookie_() noexcept {
#if defined(SLUICE_B1C_MUTANT_COOKIE_REUSE)
    return 1;
#endif
    if (next_cookie_ == 0 || next_cookie_ >= CONTROL_TAG) {
        std::fprintf(stderr, "sluice::async::UringAsyncBackend: operation-cookie "
                             "domain exhausted (would enter tagged control range / "
                             "wrap; invariant violation)\n");
        std::fflush(stderr);
        std::terminate();
    }
    return next_cookie_++;
}

std::size_t UringAsyncBackend::find_live_router_index_(detail::SlotHandle h) const noexcept {
    for (std::size_t i = 0; i < router_.size(); ++i) {
        const RouterEntry& e = router_[i];
        if (e.in_use && e.handle.slot.value == h.slot.value &&
            e.handle.generation.value == h.generation.value) {
            return i;
        }
    }
    return router_.size();
}

std::size_t UringAsyncBackend::find_live_router_cookie_(std::uint64_t cookie) const noexcept {
    for (std::size_t i = router_.size(); i-- > 0;) {
        if (router_[i].in_use && router_[i].cookie == cookie)
            return i;
    }
    return router_.size();
}

void UringAsyncBackend::retire_router_entry_(std::size_t router_index) noexcept {
    if (router_index >= router_.size() || !router_[router_index].in_use) {
        std::fprintf(stderr, "sluice::async::UringAsyncBackend: invalid router retirement "
                             "(invariant violation)\n");
        std::fflush(stderr);
        std::terminate();
    }
    RouterEntry& entry = router_[router_index];
    if (entry.control_state != RouterEntry::ControlState::none) {
        std::fprintf(stderr, "sluice::async::UringAsyncBackend: router retired with a live "
                             "control reference (invariant violation)\n");
        std::fflush(stderr);
        std::terminate();
    }
    entry = RouterEntry{};
    cookie_free_list_.push_back(detail::SlotIndex{static_cast<std::uint32_t>(router_index)});
    live_cookies_.fetch_sub(1, std::memory_order_relaxed);
}

void UringAsyncBackend::finalize_operation_terminal_(
    RouterEntry& route, std::size_t router_index,
    const detail::TerminalResult& terminal) noexcept {
    if (router_index >= router_.size() || !route.in_use || route.terminal_delivered) {
        std::fprintf(stderr, "sluice::async::UringAsyncBackend: invalid operation terminal "
                             "finalization (invariant violation)\n");
        std::fflush(stderr);
        std::terminate();
    }

    const detail::RequestKey key{core_->context(), route.handle.slot, route.handle.generation};
    const PreparedUringOp& prep = prepared_ops_[route.handle.slot.value];
    const bool is_byte_op =
        prep.kind == detail::OperationKind::read || prep.kind == detail::OperationKind::write;
    detail::TerminalCandidate candidate;
    candidate.kind = detail::TerminalCandidateKind::physical_outcome;
    if (terminal.stored && terminal.is_error) {
        candidate.outcome = sluice::detail::IoOutcome::failure(terminal.error);
    } else if (is_byte_op) {
        candidate.outcome = sluice::detail::IoOutcome::success(terminal.bytes);
    } else {
        candidate.outcome = sluice::detail::IoOutcome::success();
    }
    if (core_->offer_terminal(key, candidate) != detail::TerminalVerdict::chosen) {
        std::fprintf(stderr, "sluice::async::UringAsyncBackend: operation terminal lost "
                             "RequestCore winner authority (invariant violation)\n");
        std::fflush(stderr);
        std::terminate();
    }

    if (cookie_terminal_is_canceled(terminal)) {
        bump(stats_, &AsyncStats::canceled_ops);
    } else if (terminal.stored && terminal.is_error) {
        bump(stats_, &AsyncStats::completion_errors);
    } else if (is_byte_op && terminal.bytes < prep.length) {
        bump(stats_, &AsyncStats::short_completions);
    }

    route.terminal_delivered = true;
#if defined(SLUICE_B1C_MUTANT_PUBLISH_BEFORE_BORROW_RETIREMENT)
    publication_pending_->push_back(route.handle);
#else
    if (core_->release_execution(key) != detail::ExecutionRelease::borrow_touch_fully_retired) {
        detail::uring_core_handoff_fail_fast();
    }
    publication_pending_->push_back(route.handle);
#endif
    if (route.control_state == RouterEntry::ControlState::none) {
        retire_router_entry_(router_index);
    }
}

void UringAsyncBackend::handle_one_cqe(std::uint64_t user_data, int res) noexcept {
    std::lock_guard<std::mutex> lk(dispatch_mtx_);
    if (is_control_cookie(user_data)) {
        const std::uint64_t target_cookie = control_target_cookie(user_data);
        if (target_cookie == 0)
            return;
        const std::size_t router_index = find_live_router_cookie_(target_cookie);
        if (router_index == router_.size())
            return;

        RouterEntry& route = router_[router_index];
        if (route.control_state != RouterEntry::ControlState::submitted ||
            live_control_sqes_.load(std::memory_order_relaxed) == 0) {
            std::fprintf(stderr, "sluice::async::UringAsyncBackend: control CQE without its "
                                 "exact submitted control reference (invariant violation)\n");
            std::fflush(stderr);
            std::terminate();
        }
        route.control_state = RouterEntry::ControlState::none;
        live_control_sqes_.fetch_sub(1, std::memory_order_relaxed);
        const detail::RequestKey key{core_->context(), route.handle.slot, route.handle.generation};
        if (core_->release_control(key) != detail::ControlRelease::released) {
            detail::uring_core_handoff_fail_fast();
        }
#if defined(SLUICE_B1C_MUTANT_CANCEL_CQE_AS_ORIGINAL_TERMINAL)
        if (!route.terminal_delivered) {
            detail::TerminalCandidate fabricated;
            fabricated.kind = detail::TerminalCandidateKind::physical_outcome;
            fabricated.outcome =
                sluice::detail::IoOutcome::failure(IoError{IoError::Code::canceled});
            if (core_->offer_terminal(key, fabricated) == detail::TerminalVerdict::chosen) {
                route.terminal_delivered = true;
                (void)core_->release_execution(key);
                publication_pending_->push_back(route.handle);
            }
        }
#endif
#if defined(SLUICE_B1C_MUTANT_RECLAIM_BEFORE_CONTROL_RETIREMENT)
        retire_router_entry_(router_index);
#else
        if (route.terminal_delivered) {
            retire_router_entry_(router_index);
        }
#endif
        return;
    }
    if (user_data == 0)
        return;

    const std::size_t router_index = find_live_router_cookie_(user_data);
    if (router_index == router_.size())
        return;
    RouterEntry& entry = router_[router_index];

    const PreparedUringOp& prep = prepared_ops_[entry.handle.slot.value];
    const bool is_byte_op =
        (prep.kind == detail::OperationKind::read || prep.kind == detail::OperationKind::write);
    detail::TerminalResult terminal;
    if (res < 0) {
#if defined(SLUICE_B1C_MUTANT_DROP_ORIGINAL_OUTCOME)
        if (is_byte_op)
            return;
#endif
        terminal = detail::TerminalResult::err(sluice::from_errno_value(-res));
    } else if (is_byte_op) {
        terminal = detail::TerminalResult::ok_bytes(static_cast<std::uint64_t>(res));
    } else {
        terminal = detail::TerminalResult::ok_void();
    }

    finalize_operation_terminal_(entry, router_index, terminal);
}

std::size_t UringAsyncBackend::reap_cqes() noexcept {
    std::size_t non_control_observed = 0;
    constexpr unsigned BATCH = 32;
    io_uring_cqe* cqes[BATCH];
    unsigned got = 0;
    while ((got = ::io_uring_peek_batch_cqe(&ring_state_->ring, cqes, BATCH)) > 0) {
        for (unsigned i = 0; i < got; ++i) {
            io_uring_cqe* cqe = cqes[i];

            const std::uint64_t user_data = ::io_uring_cqe_get_data64(cqe);
            const int res = cqe->res;
            ::io_uring_cqe_seen(&ring_state_->ring, cqe);

            const bool is_op = (!is_control_cookie(user_data) && user_data != 0);
            handle_one_cqe(user_data, res);
            if (is_op)
                ++non_control_observed;
        }
        if (got < BATCH)
            break;
    }
    return non_control_observed;
}

int UringAsyncBackend::wait_cqe_without_submit() noexcept {

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
    if (ring_state_->test_hooks.before_poison_wait != nullptr)
        ring_state_->test_hooks.before_poison_wait(ring_state_->test_hooks.context);
#endif
    return ::io_uring_enter(static_cast<unsigned>(ring_state_->ring.ring_fd), 0, 1,
                            IORING_ENTER_GETEVENTS, nullptr);
}

void UringAsyncBackend::publish_zero_op_inline(detail::RequestKey id,
                                               detail::SlotHandle h) noexcept {
    if (!core_->acquire_control(id)) {
        detail::uring_core_handoff_fail_fast();
    }
    detail::PublicationPayload payload;
    if (core_->begin_publication(id, &payload) != detail::PublicationGrant::granted) {
        detail::uring_core_handoff_fail_fast();
    }
    DeliveryRecord& record = delivery_[h.slot.value];
    record.publish(record.completion, payload.outcome);
    if (core_->complete_publication(id) != detail::PublicationCompletion::completed) {
        detail::uring_core_handoff_fail_fast();
    }
    record.event_owed = true;
    record.owed_key = id;
    signal_ready_progress();
}

void UringAsyncBackend::publish_one(detail::SlotHandle h) {
    const detail::RequestKey key{core_->context(), h.slot, h.generation};
    detail::PublicationPayload payload;
    if (core_->begin_publication(key, &payload) != detail::PublicationGrant::granted) {
        detail::uring_core_handoff_fail_fast();
    }
    DeliveryRecord& record = delivery_[h.slot.value];
    record.publish(record.completion, payload.outcome);
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)

    wait_publication_epilogue_pause_();
#endif
    if (core_->complete_publication(key) != detail::PublicationCompletion::completed) {
        detail::uring_core_handoff_fail_fast();
    }
    deliver_event(payload.id, record.kind);
}

void UringAsyncBackend::deliver_event(detail::RequestKey key, detail::OperationKind kind) {
    DeliveryRecord& record = delivery_[key.slot.value];
    detail::OptionalWaiterDelivery waiter = detail::OptionalWaiterDelivery::none();
    if (record.waiter_delivery_present) {
        waiter =
            detail::OptionalWaiterDelivery::of(record.waiter_token, std::move(record.waiter_lease));
        record.waiter_token = {};
        record.waiter_delivery_present = false;
    }
    record.registration = detail::WaiterRegistration::closed;
    (routing_sink_ ? *routing_sink_ : sink_)
        .on_ready(detail::ReadyEvent{key, kind, std::move(waiter)});
}

std::size_t UringAsyncBackend::poll() {
    if (!have_ring_)
        return 0;

    {
        std::lock_guard<std::mutex> lk(dispatch_mtx_);
        if (!fatal_error_.has_value()) {
            while (!dispatch_->empty()) {
                detail::SlotHandle h = dispatch_->front();
                if (!dispatch_one_locked(h))
                    break;
            }
        }

        if (!fatal_error_.has_value())
            (void)submit_transport_locked();
    }

    (void)reap_cqes();

    std::size_t published = 0;
    for (;;) {
        detail::SlotHandle h{};
        bool have = false;
        {
            std::lock_guard<std::mutex> lk(dispatch_mtx_);
            have = publication_pending_->pop_front(h);
        }
        if (!have)
            break;
        publish_one(h);
        ++published;
    }
    for (std::uint32_t i = 0; i < capacity_; ++i) {
        DeliveryRecord& record = delivery_[i];
        if (!record.event_owed)
            continue;
        record.event_owed = false;
        const detail::RequestKey owed = record.owed_key;
        record.owed_key = {};
        deliver_event(owed, record.kind);
        if (core_->release_control(owed) != detail::ControlRelease::released) {
            detail::uring_core_handoff_fail_fast();
        }
        ++published;
    }

    if (published > 0) {
        signal_ready_progress();
    }
    return published;
}

Result<std::size_t> UringAsyncBackend::wait_one() {
    if (!have_ring_)
        return std::size_t{0};

    {
        std::lock_guard<std::mutex> lk(dispatch_mtx_);
        if (!fatal_error_.has_value()) {
            while (!dispatch_->empty()) {
                detail::SlotHandle h = dispatch_->front();
                if (!dispatch_one_locked(h))
                    break;
            }
            if (!fatal_error_.has_value())
                (void)submit_transport_locked();
        }
    }
    (void)reap_cqes();
    std::size_t n = 0;
    {
        detail::SlotHandle h{};
        bool have = false;
        {
            std::lock_guard<std::mutex> lk(dispatch_mtx_);
            have = publication_pending_->pop_front(h);
        }
        if (have) {
            publish_one(h);
            n = 1;
        }
    }
    for (std::uint32_t i = 0; i < capacity_ && n == 0; ++i) {
        DeliveryRecord& record = delivery_[i];
        if (!record.event_owed)
            continue;
        record.event_owed = false;
        const detail::RequestKey owed = record.owed_key;
        record.owed_key = {};
        deliver_event(owed, record.kind);
        if (core_->release_control(owed) != detail::ControlRelease::released) {
            detail::uring_core_handoff_fail_fast();
        }
        n = 1;
    }
    if (n > 0) {
        signal_ready_progress();
        return n;
    }
    if (core_->occupancy().outstanding == 0 &&
        live_control_sqes_.load(std::memory_order_relaxed) == 0 &&
        (fatal_error_.has_value() || transport_ledger_->empty()))
        return std::size_t{0};

    auto submit_and_wait_once = [&]() noexcept {
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
        if (ring_state_->test_hooks.submit_and_wait != nullptr) {
            return ring_state_->test_hooks.submit_and_wait(ring_state_->test_hooks.context,
                                                           &ring_state_->ring, 1);
        }
#endif
        return ::io_uring_submit_and_wait(&ring_state_->ring, 1);
    };

    for (;;) {
        int rc = 0;
        bool poisoned_wait = false;
        {
            std::lock_guard<std::mutex> lk(dispatch_mtx_);
            poisoned_wait = fatal_error_.has_value();
            if (!poisoned_wait) {
                const bool had_pending_transport = !transport_ledger_->empty();
                rc = sluice::detail::retry_uring_wait_on_eintr(submit_and_wait_once);
                account_transport_result_locked(rc, had_pending_transport);
            }
        }
        if (poisoned_wait) {
            rc = sluice::detail::retry_uring_wait_on_eintr(
                [&]() noexcept { return wait_cqe_without_submit(); });
        } else if (fatal_error_.has_value()) {
            rc = 0;
        }

        if (rc < 0 && rc != -EAGAIN && rc != -EBUSY) {
            return make_unexpected<std::size_t>(sluice::from_errno_value(-rc));
        }
        (void)reap_cqes();
        {
            detail::SlotHandle h{};
            bool have = false;
            {
                std::lock_guard<std::mutex> lk(dispatch_mtx_);
                have = publication_pending_->pop_front(h);
            }
            if (have) {
                publish_one(h);
                n = 1;
            }
        }
        for (std::uint32_t i = 0; i < capacity_ && n == 0; ++i) {
            DeliveryRecord& record = delivery_[i];
            if (!record.event_owed)
                continue;
            record.event_owed = false;
            const detail::RequestKey owed = record.owed_key;
            record.owed_key = {};
            deliver_event(owed, record.kind);
            if (core_->release_control(owed) != detail::ControlRelease::released) {
                detail::uring_core_handoff_fail_fast();
            }
            n = 1;
        }
        if (n > 0) {
            signal_ready_progress();
            return n;
        }
        if (core_->occupancy().outstanding == 0 &&
            live_control_sqes_.load(std::memory_order_relaxed) == 0 &&
            (fatal_error_.has_value() || transport_ledger_->empty()))
            return std::size_t{0};
    }
}

detail::PublicCancel UringAsyncBackend::cancel_key(detail::RequestKey key) noexcept {
    detail::PublicCancel disposition;
    {
        std::lock_guard<std::mutex> lk(dispatch_mtx_);

        (void)dispatch_->remove_exact(detail::SlotHandle{key.slot, key.generation});
        disposition = core_->cancel(key);
        if (disposition == detail::PublicCancel::won_before_execution) {
            if (core_->release_execution(key) !=
                detail::ExecutionRelease::borrow_touch_fully_retired) {
                detail::uring_core_handoff_fail_fast();
            }
            publication_pending_->push_back(detail::SlotHandle{key.slot, key.generation});
        } else if (disposition == detail::PublicCancel::requested) {
            issue_running_cancel_locked_(detail::SlotHandle{key.slot, key.generation});
        }
    }
    if (disposition == detail::PublicCancel::won_before_execution) {
        bump(stats_, &AsyncStats::canceled_ops);
        signal_ready_progress();
    }

    return disposition;
}

void UringAsyncBackend::issue_running_cancel_locked_(detail::SlotHandle h) noexcept {
    if (fatal_error_.has_value())
        return;

    const std::size_t idx = find_live_router_index_(h);
    if (idx == router_.size())
        return;
    RouterEntry& route = router_[idx];
    if (route.control_state != RouterEntry::ControlState::none)
        return;

    const std::uint64_t target_cookie = route.cookie;
    if (target_cookie == 0 || target_cookie >= CONTROL_TAG) {
        std::fprintf(stderr, "sluice::async::UringAsyncBackend: issue_running_cancel "
                             "found LIVE router entry with invalid cookie "
                             "(invariant violation)\n");
        std::fflush(stderr);
        std::terminate();
    }

    io_uring_sqe* sqe = ::io_uring_get_sqe(&ring_state_->ring);
    if (sqe == nullptr) {
        (void)submit_transport_locked();
        if (fatal_error_.has_value()) {
            return;
        }
        sqe = ::io_uring_get_sqe(&ring_state_->ring);
    }
    if (sqe == nullptr)
        return;

#if !defined(SLUICE_B1C_MUTANT_REMOVE_CONTROL_PIN)
    const detail::RequestKey key{core_->context(), h.slot, h.generation};
    if (!core_->acquire_control(key)) {
        detail::uring_core_handoff_fail_fast();
    }
#endif
    ::io_uring_prep_cancel64(sqe, target_cookie, 0);
    ::io_uring_sqe_set_data64(sqe, make_control_cookie(target_cookie));
    route.control_state = RouterEntry::ControlState::prepared;
    const auto& sq = ring_state_->ring.sq;
    const std::uint32_t physical_position =
        static_cast<std::uint32_t>((sq.sqe_tail - 1u) & sq.ring_mask);
    transport_ledger_->append(TransportLedger::Kind::cancel_control, physical_position,
                              target_cookie, h);
}

void UringAsyncBackend::close_admission() {
    if (!have_ring_)
        return;
    core_->close_admission();
    if (wait_source_) {
        wait_source_->interrupt_all();
    }
}

std::size_t UringAsyncBackend::outstanding() const noexcept {
    return core_ != nullptr ? core_->occupancy().outstanding : 0;
}

bool UringAsyncBackend::available() const noexcept {
    return available_;
}

Result<RequestHandleState> UringAsyncBackend::resolve_identity_state(
    std::uint64_t ctx, std::uint32_t slot, std::uint64_t gen) const {
    if (core_ == nullptr) {
        return RequestHandleState::not_found;
    }
    const detail::RequestKey key{detail::ContextIdentity{ctx}, detail::SlotIndex{slot},
                                 detail::Generation{gen}};
    switch (core_->lookup(key)) {
    case detail::PublicLookup::outstanding:
        return RequestHandleState::outstanding;
    case detail::PublicLookup::published:
        return RequestHandleState::completion_ready;
    case detail::PublicLookup::not_found:
        return RequestHandleState::not_found;
    }
    return RequestHandleState::not_found;
}

void UringAsyncBackend::cancel(Completion<std::size_t>& c) {
    auto key = core_binding(c);
    if (!key.has_value())
        return;
    (void)cancel_key(*key);
}

void UringAsyncBackend::cancel(Completion<void>& c) {
    auto key = core_binding(c);
    if (!key.has_value())
        return;
    (void)cancel_key(*key);
}

detail::PublicCancel UringAsyncBackend::cancel_identity(detail::RequestKey key) {
    return cancel_key(key);
}

Result<void> UringAsyncBackend::register_waiter(Completion<std::size_t>& c,
                                                detail::WaiterToken token,
                                                detail::RoutingLease lease) {
    auto key = core_binding(c);
    if (!key.has_value()) {
        return make_unexpected<void>(IoError{IoError::Code::invalid_state});
    }
    if (core_->lookup(*key) != detail::PublicLookup::outstanding) {
        return make_unexpected<void>(IoError{IoError::Code::invalid_state});
    }
    DeliveryRecord& record = delivery_[key->slot.value];
    if (record.registration == detail::WaiterRegistration::open_registered) {
        return make_unexpected<void>(IoError{IoError::Code::invalid_state});
    }
    record.registration = detail::WaiterRegistration::open_registered;
    record.waiter_token = token;
    record.waiter_lease = std::move(lease);
    record.waiter_delivery_present = true;
    return {};
}

Result<void> UringAsyncBackend::register_waiter(Completion<void>& c, detail::WaiterToken token,
                                                detail::RoutingLease lease) {
    auto key = core_binding(c);
    if (!key.has_value()) {
        return make_unexpected<void>(IoError{IoError::Code::invalid_state});
    }
    if (core_->lookup(*key) != detail::PublicLookup::outstanding) {
        return make_unexpected<void>(IoError{IoError::Code::invalid_state});
    }
    DeliveryRecord& record = delivery_[key->slot.value];
    if (record.registration == detail::WaiterRegistration::open_registered) {
        return make_unexpected<void>(IoError{IoError::Code::invalid_state});
    }
    record.registration = detail::WaiterRegistration::open_registered;
    record.waiter_token = token;
    record.waiter_lease = std::move(lease);
    record.waiter_delivery_present = true;
    return {};
}

Result<detail::RoutingLease> UringAsyncBackend::cancel_waiter(Completion<std::size_t>& c) {
    auto key = core_binding(c);
    if (!key.has_value()) {
        return make_unexpected<detail::RoutingLease>(IoError{IoError::Code::not_found});
    }
    DeliveryRecord& record = delivery_[key->slot.value];
    if (record.registration != detail::WaiterRegistration::open_registered) {
        return make_unexpected<detail::RoutingLease>(IoError{IoError::Code::not_found});
    }
    detail::RoutingLease lease = std::move(record.waiter_lease);
    record.waiter_token = {};
    record.registration = detail::WaiterRegistration::open_no_waiter;
    record.waiter_delivery_present = false;
    return lease;
}

Result<detail::RoutingLease> UringAsyncBackend::cancel_waiter(Completion<void>& c) {
    auto key = core_binding(c);
    if (!key.has_value()) {
        return make_unexpected<detail::RoutingLease>(IoError{IoError::Code::not_found});
    }
    DeliveryRecord& record = delivery_[key->slot.value];
    if (record.registration != detail::WaiterRegistration::open_registered) {
        return make_unexpected<detail::RoutingLease>(IoError{IoError::Code::not_found});
    }
    detail::RoutingLease lease = std::move(record.waiter_lease);
    record.waiter_token = {};
    record.registration = detail::WaiterRegistration::open_no_waiter;
    record.waiter_delivery_present = false;
    return lease;
}

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)

void UringAsyncBackend::wait_submit_entry_pause_() noexcept {
    auto* g = submit_entry_gate_.load(std::memory_order_acquire);
    if (g == nullptr)
        return;
    g->exited.store(false, std::memory_order_release);
    g->paused.store(true, std::memory_order_release);
    g->paused.notify_all();
    g->resume.wait(false, std::memory_order_acquire);
    g->exited.store(true, std::memory_order_release);
    g->exited.notify_all();
}

void UringAsyncBackend::wait_pre_accept_commit_pause_() noexcept {
    auto* g = pre_accept_commit_gate_.load(std::memory_order_acquire);
    if (g == nullptr)
        return;
    g->exited.store(false, std::memory_order_release);
    g->paused.store(true, std::memory_order_release);
    g->paused.notify_all();
    g->resume.wait(false, std::memory_order_acquire);
    g->exited.store(true, std::memory_order_release);
    g->exited.notify_all();
}

void UringAsyncBackend::wait_accepted_pre_dispatch_pause_() noexcept {
    auto* g = accepted_pre_dispatch_gate_.load(std::memory_order_acquire);
    if (g == nullptr)
        return;
    g->exited.store(false, std::memory_order_release);
    g->paused.store(true, std::memory_order_release);
    g->paused.notify_all();
    g->resume.wait(false, std::memory_order_acquire);
    g->exited.store(true, std::memory_order_release);
    g->exited.notify_all();
}

void UringAsyncBackend::wait_publication_epilogue_pause_() noexcept {
    auto* g = publication_epilogue_gate_.load(std::memory_order_acquire);
    if (g == nullptr)
        return;
    g->exited.store(false, std::memory_order_release);
    g->paused.store(true, std::memory_order_release);
    g->paused.notify_all();
    g->resume.wait(false, std::memory_order_acquire);
    g->exited.store(true, std::memory_order_release);
    g->exited.notify_all();
}

std::optional<IoError>
UringAsyncBackend::injected_precommit_stage_failure_(SubmitStage stage) noexcept {
    auto* inj = submit_stage_failure_injection_.load(std::memory_order_acquire);
    if (inj == nullptr)
        return std::nullopt;
    switch (stage) {
    case SubmitStage::reserve:
        if (inj->fail_reserve.load(std::memory_order_acquire)) {
            inj->reserve_fired.fetch_add(1, std::memory_order_relaxed);

            return IoError{IoError::Code::would_block};
        }
        break;
    case SubmitStage::prepare:
        if (inj->fail_prepare.load(std::memory_order_acquire)) {
            inj->prepare_fired.fetch_add(1, std::memory_order_relaxed);
            return IoError{IoError::Code::invalid_state};
        }
        break;
    case SubmitStage::commit:
        if (inj->fail_commit.load(std::memory_order_acquire)) {
            inj->commit_fired.fetch_add(1, std::memory_order_relaxed);
            return IoError{IoError::Code::invalid_state};
        }
        break;
    }
    return std::nullopt;
}
#endif

#endif

}
