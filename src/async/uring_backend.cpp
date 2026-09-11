#include <sluice/async/uring_backend.hpp>

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
#include "tax0_ablation_seams.hpp"
#endif

#include <sluice/detail/io_validation.hpp>
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

namespace {
Result<void> unsupported_stub() {
    return make_unexpected<void>(IoError{IoError::Code::backend_error});
}
} // namespace

Result<void> UringAsyncBackend::submit_read(ReadOp, Completion<std::size_t>&) {
    return unsupported_stub();
}
Result<void> UringAsyncBackend::submit_write(WriteOp, Completion<std::size_t>&) {
    return unsupported_stub();
}
Result<void> UringAsyncBackend::submit_sync_data(SyncDataOp, Completion<void>&) {
    return unsupported_stub();
}
Result<void> UringAsyncBackend::submit_sync_all(SyncAllOp, Completion<void>&) {
    return unsupported_stub();
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

} // namespace

class UringAsyncBackend::BoundedDispatchQueue {
  public:
    explicit BoundedDispatchQueue(std::size_t capacity) : storage_(capacity), capacity_(capacity) {}
    bool empty() const noexcept { return size_ == 0; }
    std::size_t size() const noexcept { return size_; }

    detail::SlotHandle front() const noexcept {
        if (size_ == 0) {
            std::fprintf(stderr, "sluice::async::UringAsyncBackend: dispatch ring "
                                 "front() on empty queue (invariant violation)\n");
            std::fflush(stderr);
            std::terminate();
        }
        return storage_[head_];
    }

    void push_back(detail::SlotHandle h) noexcept {
        if (size_ >= capacity_) {
            std::fprintf(stderr, "sluice::async::UringAsyncBackend: dispatch ring overflow "
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
    if (op.fd < 0)
        return make_unexpected<void>(IoError{IoError::Code::invalid_argument});
    if (op.len > 0 && op.dst == nullptr) {
        return make_unexpected<void>(IoError{IoError::Code::invalid_argument});
    }
    auto off = sluice::detail::checked_posix_offset(op.offset);
    if (!off.has_value()) {
        return make_unexpected<void>(IoError{IoError::Code::invalid_argument});
    }
    return {};
}
Result<void> UringAsyncBackend::validate_write(WriteOp op) {
    if (op.fd < 0)
        return make_unexpected<void>(IoError{IoError::Code::invalid_argument});
    if (op.len > 0 && op.src == nullptr) {
        return make_unexpected<void>(IoError{IoError::Code::invalid_argument});
    }
    auto off = sluice::detail::checked_posix_offset(op.offset);
    if (!off.has_value()) {
        return make_unexpected<void>(IoError{IoError::Code::invalid_argument});
    }
    return {};
}
Result<void> UringAsyncBackend::validate_sync(SyncDataOp op) {
    if (op.fd < 0)
        return make_unexpected<void>(IoError{IoError::Code::invalid_argument});
    return {};
}
Result<void> UringAsyncBackend::validate_sync(SyncAllOp op) {
    if (op.fd < 0)
        return make_unexpected<void>(IoError{IoError::Code::invalid_argument});
    return {};
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
                                           const detail::TerminalResult& t) noexcept {
    AsyncBackend::publish(*static_cast<Completion<std::size_t>*>(completion), terminal_to_size(t));
}

void UringAsyncBackend::publish_void_ready(void* completion,
                                           const detail::TerminalResult& t) noexcept {
    AsyncBackend::publish(*static_cast<Completion<void>*>(completion), terminal_to_void(t));
}

Result<std::size_t> UringAsyncBackend::terminal_to_size(const detail::TerminalResult& t) noexcept {
    if (t.stored && t.is_error)
        return make_unexpected<std::size_t>(t.error);
    return Result<std::size_t>{static_cast<std::size_t>(t.bytes)};
}

Result<void> UringAsyncBackend::terminal_to_void(const detail::TerminalResult& t) noexcept {
    if (t.stored && t.is_error)
        return make_unexpected<void>(t.error);
    return {};
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
    : arena_(detail::ContextIdentity::for_testing(next_backend_id()), config.request_capacity),
      prepared_ops_(config.request_capacity), router_(config.request_capacity),
      cancel_scratch_(config.request_capacity), cookie_free_list_(config.request_capacity),
      queue_depth_(config.queue_depth), ring_state_(std::make_unique<UringRingState>()) {
    for (std::uint32_t i = 0; i < config.request_capacity; ++i) {
        cookie_free_list_[i] = detail::SlotIndex{i};
    }
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)

    cookie_table_for_test_ = std::make_unique<RouterCookieTableForTest>(config.request_capacity);

    router_extent_cached_for_test_ = router_.size();
#endif
    dispatch_ = std::make_unique<BoundedDispatchQueue>(config.request_capacity);
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
        const auto q = arena_.quiescence_snapshot();
        const bool ledger_quiescent =
            transport_ledger_ == nullptr || transport_ledger_->empty() ||
            (fatal_error_.has_value() && transport_ledger_->all_class_a_recovery_retired());
        if (!dispatch_->empty() || live_cookies_.load(std::memory_order_relaxed) != 0 ||
            live_control_sqes_.load(std::memory_order_relaxed) != 0 || !ledger_quiescent ||
            q.slot_in_use != 0 || q.accepted_outstanding != 0 || q.backend_ready != 0) {
            detail::uring_non_quiescent_destruction_fail_fast();
        }
    }
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)

    if (auto* fn = before_queue_exit_fn_.load(std::memory_order_acquire)) {
        fn(before_queue_exit_ctx_.load(std::memory_order_acquire));
    }
#endif
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

std::size_t UringAsyncBackend::live_control_entries_for_test() const noexcept {
    std::lock_guard<std::mutex> lk(dispatch_mtx_);
    std::size_t live = 0;
    for (const RouterEntry& entry : router_) {
        if (entry.in_use && entry.control_state != RouterEntry::ControlState::none)
            ++live;
    }
    return live;
}
#endif

template <class Op>
Result<void> UringAsyncBackend::submit_size(Op op, Completion<std::size_t>& c,
                                            detail::OperationKind kind) {
    detail::SlotHandle h{};
    {
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)

        wait_before_admission_lock_pause_();
#endif

        std::lock_guard<std::mutex> lk(dispatch_mtx_);
        SubmitPolicy<Op, Completion<std::size_t>> policy{*this, kind};
        auto r = detail::submit_transaction(arena_, c, op, policy);
        if (!r.has_value()) {
            return make_unexpected<void>(r.error());
        }
        h = r.value();
    }

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)

    wait_after_commit_before_enqueue_pause_();
#endif

    enqueue_after_commit(h);
    return {};
}

template <class Op>
Result<void> UringAsyncBackend::submit_void(Op op, Completion<void>& c,
                                            detail::OperationKind kind) {
    detail::SlotHandle h{};
    {
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
        wait_before_admission_lock_pause_();
#endif
        std::lock_guard<std::mutex> lk(dispatch_mtx_);
        SubmitPolicy<Op, Completion<void>> policy{*this, kind};
        auto r = detail::submit_transaction(arena_, c, op, policy);
        if (!r.has_value()) {
            return make_unexpected<void>(r.error());
        }
        h = r.value();
    }

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
    wait_after_commit_before_enqueue_pause_();
#endif

    enqueue_after_commit(h);
    return {};
}

Result<void> UringAsyncBackend::submit_read(ReadOp op, Completion<std::size_t>& c) {
    return submit_size(op, c, detail::OperationKind::read);
}
Result<void> UringAsyncBackend::submit_write(WriteOp op, Completion<std::size_t>& c) {
    return submit_size(op, c, detail::OperationKind::write);
}
Result<void> UringAsyncBackend::submit_sync_data(SyncDataOp op, Completion<void>& c) {
    return submit_void(op, c, detail::OperationKind::sync_data);
}
Result<void> UringAsyncBackend::submit_sync_all(SyncAllOp op, Completion<void>& c) {
    return submit_void(op, c, detail::OperationKind::sync_all);
}

void UringAsyncBackend::enqueue_after_commit(detail::SlotHandle h) noexcept {
    bool newly_poisoned = false;
    detail::EnqueueOutcome outcome;
    {
        std::unique_lock<std::mutex> lk(dispatch_mtx_);

        const bool poisoned_before = fatal_error_.has_value();
        outcome = arena_.enqueue(h);
        if (outcome == detail::EnqueueOutcome::enqueued) {
            dispatch_->push_back(h);

            for (;;) {
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
                if (before_dispatch_transfer_gate_.load(std::memory_order_acquire) != nullptr) {
                    lk.unlock();
                    wait_before_dispatch_transfer_pause_();
                    lk.lock();
                }
#endif
                if (dispatch_->empty())
                    break;
                const detail::SlotHandle front = dispatch_->front();
                if (!dispatch_one_locked(front))
                    break;
            }
        }

        newly_poisoned = !poisoned_before && fatal_error_.has_value();
    }
    if (outcome != detail::EnqueueOutcome::enqueued) {
        signal_ready_progress();
    }

    if (newly_poisoned) {
        signal_ready_progress();
    }
}

bool UringAsyncBackend::dispatch_one(detail::SlotHandle h) noexcept {
    std::lock_guard<std::mutex> lk(dispatch_mtx_);
    return dispatch_one_locked(h);
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
    detail::SlotIndex router_slot = cookie_free_list_.back();
    cookie_free_list_.pop_back();

    io_uring_sqe* sqe = ::io_uring_get_sqe(&ring_state_->ring);
    if (sqe == nullptr) {
        (void)submit_transport_locked();
        if (fatal_error_.has_value()) {
            cookie_free_list_.push_back(router_slot);
            return false;
        }
        sqe = ::io_uring_get_sqe(&ring_state_->ring);
        if (sqe == nullptr) {
            cookie_free_list_.push_back(router_slot);
            return false;
        }
    }

    const std::uint64_t op_cookie = allocate_cookie_();
    const PreparedUringOp& prep = prepared_ops_[h.slot.value];
    switch (prep.kind) {
    case detail::OperationKind::read:
        ::io_uring_prep_read(sqe, prep.fd, const_cast<std::byte*>(prep.buffer), prep.native_length,
                             static_cast<off_t>(static_cast<std::int64_t>(prep.offset)));
        break;
    case detail::OperationKind::write:
        ::io_uring_prep_write(sqe, prep.fd, prep.buffer, prep.native_length,
                              static_cast<off_t>(static_cast<int64_t>(prep.offset)));
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
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)

    router_table_insert_(op_cookie, router_slot.value);
#endif
    const auto& sq = ring_state_->ring.sq;
    const std::uint32_t physical_position =
        static_cast<std::uint32_t>((sq.sqe_tail - 1u) & sq.ring_mask);
    transport_ledger_->append(TransportLedger::Kind::operation, physical_position, op_cookie, h);

    if (!arena_.mark_running(h)) {
        std::fprintf(stderr, "sluice::async::UringAsyncBackend: mark_running false "
                             "after get_sqe (invariant violation — cancel cannot "
                             "have won under the dispatch_mtx_ discipline)\n");
        std::fflush(stderr);
        std::terminate();
    }

    if (!dispatch_->remove_exact(h)) {
        std::fprintf(stderr, "sluice::async::UringAsyncBackend: dispatch_one_locked "
                             "remove_exact miss after mark_running (invariant "
                             "violation)\n");
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
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
                fold_router_lookup_diag_for_test(RouterLookupKindForTest::transport);
#endif
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
    admission_closed_ = true;

    for (std::size_t i = 0; i < transport_ledger_->size(); ++i) {
        TransportLedger::Entry& physical = transport_ledger_->at(i);
        if (physical.class_a_recovery_retired) {
            std::fprintf(stderr, "sluice::async::UringAsyncBackend: duplicate Class-A "
                                 "recovery retirement (invariant violation)\n");
            std::fflush(stderr);
            std::terminate();
        }

        if (physical.kind == TransportLedger::Kind::operation) {
            const std::size_t router_index = find_live_router_cookie_(physical.cookie);
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
            fold_router_lookup_diag_for_test(RouterLookupKindForTest::transport);
#endif
            if (router_index == router_.size() ||
                router_[router_index].handle.slot.value != physical.handle.slot.value ||
                router_[router_index].handle.generation.value != physical.handle.generation.value) {
                std::fprintf(stderr, "sluice::async::UringAsyncBackend: Class-A operation "
                                     "recovery lost identity "
                                     "(invariant violation)\n");
                std::fflush(stderr);
                std::terminate();
            }
            RouterEntry& route = router_[router_index];
            const detail::TerminalResult terminal = detail::TerminalResult::err(error);
            if (route.control_state == RouterEntry::ControlState::none) {
                finalize_operation_terminal_(router_index, terminal);
            } else {
                route.deferred_terminal = terminal;
                route.deferred_terminal_stored = true;
            }
        } else {
            const std::size_t router_index = find_live_router_cookie_(physical.cookie);
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
            fold_router_lookup_diag_for_test(RouterLookupKindForTest::transport);
#endif
            if (router_index == router_.size() ||
                router_[router_index].handle.slot.value != physical.handle.slot.value ||
                router_[router_index].handle.generation.value != physical.handle.generation.value ||
                router_[router_index].control_state != RouterEntry::ControlState::prepared) {
                std::fprintf(stderr, "sluice::async::UringAsyncBackend: Class-A control "
                                     "recovery lost its exact prepared router reference "
                                     "(invariant violation)\n");
                std::fflush(stderr);
                std::terminate();
            }
            RouterEntry& route = router_[router_index];
            route.control_state = RouterEntry::ControlState::none;
            cancel_scratch_[physical.handle.slot.value].cancel_queued = false;
            if (route.deferred_terminal_stored)
                finalize_operation_terminal_(router_index, route.deferred_terminal);
        }
        physical.class_a_recovery_retired = true;
    }

    detail::SlotHandle local{};
    while (dispatch_->pop_front(local)) {
        if (!arena_.record_terminal(local, detail::TerminalResult::err(error))) {
            std::fprintf(stderr, "sluice::async::UringAsyncBackend: local poison retirement "
                                 "lost terminal authority (invariant violation)\n");
            std::fflush(stderr);
            std::terminate();
        }
        cancel_scratch_[local.slot.value].cancel_queued = false;
        bump(stats_, &AsyncStats::completion_errors);
    }
}

std::uint64_t UringAsyncBackend::allocate_cookie_() noexcept {
    if (next_cookie_ == 0 || next_cookie_ >= CONTROL_TAG) {
        std::fprintf(stderr, "sluice::async::UringAsyncBackend: operation-cookie "
                             "domain exhausted (would enter tagged control range / "
                             "wrap; invariant violation)\n");
        std::fflush(stderr);
        std::terminate();
    }
    return next_cookie_++;
}

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
std::size_t UringAsyncBackend::router_extent_() const noexcept {
    return detail::tax0_f07_skip_extent_reprobes() ? router_extent_cached_for_test_
                                                   : router_.size();
}
#endif

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
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)

    std::size_t examined = 0;

    const std::size_t extent = router_extent_();
    std::size_t found = extent;
    const bool reverse = router_fix_mode_for_test_ == RouterFixModeForTest::reverse_scan ||
                         (router_fix_mode_for_test_ == RouterFixModeForTest::production_baseline &&
                          router_scan_mode_for_test_ != RouterScanModeForTest::forward_ablation);
    if (router_fix_mode_for_test_ == RouterFixModeForTest::bounded_cookie_table &&
        cookie_table_for_test_ != nullptr) {
        const std::size_t idx = cookie_table_for_test_->lookup(cookie);
        examined = static_cast<std::size_t>(cookie_table_for_test_->last_probes);
        if (idx != RouterCookieTableForTest::kMiss) {
            if (idx >= extent || !router_[idx].in_use || router_[idx].cookie != cookie) {
                std::fprintf(stderr, "sluice::async::UringAsyncBackend: router cookie "
                                     "table resolved a stale/non-matching router entry "
                                     "(invariant violation)\n");
                std::fflush(stderr);
                std::terminate();
            }
            found = idx;
        }
    } else if (reverse) {
        for (std::size_t i = extent; i-- > 0;) {
            ++examined;
            if (router_[i].in_use && router_[i].cookie == cookie) {
                found = i;
                break;
            }
        }
    } else {
        for (std::size_t i = 0; i < extent; ++i) {
            ++examined;
            if (router_[i].in_use && router_[i].cookie == cookie) {
                found = i;
                break;
            }
        }
    }
    RouterScanDiagnosticsForTest& diag = router_diag_for_test_;
    diag.last_call_iterations = examined;
    diag.lookup_calls += 1;
    if (reverse)
        diag.reverse_mode_calls += 1;
    if (found != extent) {
        diag.lookup_hits += 1;
        diag.matched_router_index_sum += found;
        if (found > diag.matched_router_index_max)
            diag.matched_router_index_max = found;
    } else {
        diag.lookup_misses += 1;
    }
    if (router_fix_mode_for_test_ == RouterFixModeForTest::bounded_cookie_table) {
        diag.table_lookup_probes_total += examined;
        if (examined > diag.table_lookup_probes_max)
            diag.table_lookup_probes_max = examined;
    }
    return found;
#else

    for (std::size_t i = router_.size(); i-- > 0;) {
        if (router_[i].in_use && router_[i].cookie == cookie)
            return i;
    }
    return router_.size();
#endif
}

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)

void UringAsyncBackend::router_table_insert_(std::uint64_t cookie,
                                             std::size_t router_index) noexcept {
    if (router_fix_mode_for_test_ != RouterFixModeForTest::bounded_cookie_table ||
        cookie_table_for_test_ == nullptr)
        return;
    cookie_table_for_test_->insert(cookie, router_index);
    fold_router_table_probes_for_test_('i', cookie_table_for_test_->last_probes);
}

void UringAsyncBackend::router_table_erase_(std::uint64_t cookie) noexcept {
    if (router_fix_mode_for_test_ != RouterFixModeForTest::bounded_cookie_table ||
        cookie_table_for_test_ == nullptr)
        return;
    cookie_table_for_test_->erase(cookie);
    fold_router_table_probes_for_test_('e', cookie_table_for_test_->last_probes);
}

void UringAsyncBackend::fold_router_table_probes_for_test_(char which,
                                                           std::uint64_t probes) const noexcept {
    RouterScanDiagnosticsForTest& diag = router_diag_for_test_;
    switch (which) {
    case 'i':
        diag.table_insert_calls += 1;
        diag.table_insert_probes_total += probes;
        if (probes > diag.table_insert_probes_max)
            diag.table_insert_probes_max = probes;
        break;
    case 'e':
        diag.table_erase_calls += 1;
        diag.table_erase_probes_total += probes;
        if (probes > diag.table_erase_probes_max)
            diag.table_erase_probes_max = probes;
        break;
    default:
        break;
    }
}
#endif

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)

void UringAsyncBackend::fold_router_lookup_diag_for_test(
    RouterLookupKindForTest kind) const noexcept {
    RouterScanDiagnosticsForTest& diag = router_diag_for_test_;
    const std::uint64_t it = diag.last_call_iterations;
    switch (kind) {
    case RouterLookupKindForTest::operation_cqe:
        diag.operation_cookie_lookup_calls += 1;
        diag.operation_lookup_iterations_total += it;
        if (it > diag.operation_lookup_iterations_max)
            diag.operation_lookup_iterations_max = it;
        break;
    case RouterLookupKindForTest::control_cqe:
        diag.control_cookie_lookup_calls += 1;
        diag.control_lookup_iterations_total += it;
        if (it > diag.control_lookup_iterations_max)
            diag.control_lookup_iterations_max = it;
        break;
    case RouterLookupKindForTest::transport:
        diag.transport_cookie_lookup_calls += 1;
        diag.transport_lookup_iterations_total += it;
        if (it > diag.transport_lookup_iterations_max)
            diag.transport_lookup_iterations_max = it;
        break;
    }
}
#endif

void UringAsyncBackend::retire_router_entry_(std::size_t router_index) noexcept {
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
    if (router_index >= router_extent_() || !router_[router_index].in_use) {
#else
    if (router_index >= router_.size() || !router_[router_index].in_use) {
#endif
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
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)

    router_table_erase_(entry.cookie);
#endif
    entry = RouterEntry{};
    cookie_free_list_.push_back(detail::SlotIndex{static_cast<std::uint32_t>(router_index)});
    live_cookies_.fetch_sub(1, std::memory_order_relaxed);
}

void UringAsyncBackend::finalize_operation_terminal_(
    std::size_t router_index, const detail::TerminalResult& terminal) noexcept {
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
    if (router_index >= router_extent_() || !router_[router_index].in_use ||
        router_[router_index].control_state != RouterEntry::ControlState::none) {
#else
    if (router_index >= router_.size() || !router_[router_index].in_use ||
        router_[router_index].control_state != RouterEntry::ControlState::none) {
#endif
        std::fprintf(stderr, "sluice::async::UringAsyncBackend: invalid operation terminal "
                             "finalization (invariant violation)\n");
        std::fflush(stderr);
        std::terminate();
    }

    const detail::SlotHandle h = router_[router_index].handle;
    if (!arena_.record_terminal(h, terminal)) {
        std::fprintf(stderr, "sluice::async::UringAsyncBackend: operation terminal lost "
                             "RequestArena winner authority (invariant violation)\n");
        std::fflush(stderr);
        std::terminate();
    }

    const PreparedUringOp& prep = prepared_ops_[h.slot.value];
    const bool is_byte_op =
        prep.kind == detail::OperationKind::read || prep.kind == detail::OperationKind::write;
    if (terminal.stored && terminal.is_error && terminal.error.code == IoError::Code::canceled) {
        bump(stats_, &AsyncStats::canceled_ops);
    } else if (terminal.stored && terminal.is_error) {
        bump(stats_, &AsyncStats::completion_errors);
    } else if (is_byte_op && terminal.bytes < prep.length) {
        bump(stats_, &AsyncStats::short_completions);
    }

    cancel_scratch_[h.slot.value].cancel_queued = false;
    retire_router_entry_(router_index);
}

void UringAsyncBackend::handle_one_cqe(std::uint64_t user_data, int res) noexcept {
    if (is_control_cookie(user_data)) {
        const std::uint64_t target_cookie = control_target_cookie(user_data);
        if (target_cookie == 0)
            return;
        const std::size_t router_index = find_live_router_cookie_(target_cookie);
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
        fold_router_lookup_diag_for_test(RouterLookupKindForTest::control_cqe);
        if (router_index == router_extent_())
#else
        if (router_index == router_.size())
#endif
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
        cancel_scratch_[route.handle.slot.value].cancel_queued = false;
        if (route.deferred_terminal_stored)
            finalize_operation_terminal_(router_index, route.deferred_terminal);
        return;
    }
    if (user_data == 0)
        return;

    const std::size_t router_index = find_live_router_cookie_(user_data);
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
    fold_router_lookup_diag_for_test(RouterLookupKindForTest::operation_cqe);
    if (router_index == router_extent_())
#else
    if (router_index == router_.size())
#endif
        return;
    RouterEntry& entry = router_[router_index];

    const detail::SlotHandle h = entry.handle;

    const PreparedUringOp& prep = prepared_ops_[h.slot.value];
    const bool is_byte_op =
        (prep.kind == detail::OperationKind::read || prep.kind == detail::OperationKind::write);
    detail::TerminalResult terminal;
    if (res < 0) {
        terminal = detail::TerminalResult::err(sluice::from_errno_value(-res));
    } else if (is_byte_op) {
        terminal = detail::TerminalResult::ok_bytes(static_cast<std::uint64_t>(res));
    } else {
        terminal = detail::TerminalResult::ok_void();
    }

    if (entry.control_state != RouterEntry::ControlState::none) {
        if (!entry.deferred_terminal_stored) {
            entry.deferred_terminal = terminal;
            entry.deferred_terminal_stored = true;
        }
        return;
    }
    finalize_operation_terminal_(router_index, terminal);
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
    const std::size_t n = arena_.reap(routing_sink_ ? *routing_sink_ : sink_);

    if (n > 0) {
        signal_ready_progress();
    }
    return n;
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
    std::size_t n = arena_.reap(routing_sink_ ? *routing_sink_ : sink_);
    if (n > 0) {
        signal_ready_progress();
        return n;
    }
    if (arena_.accepted_outstanding() == 0 &&
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
        n = arena_.reap(routing_sink_ ? *routing_sink_ : sink_);
        if (n > 0) {
            signal_ready_progress();
            return n;
        }
        if (arena_.accepted_outstanding() == 0 &&
            live_control_sqes_.load(std::memory_order_relaxed) == 0 &&
            (fatal_error_.has_value() || transport_ledger_->empty()))
            return std::size_t{0};
    }
}

detail::CancelDisposition UringAsyncBackend::cancel_handle_(detail::SlotHandle handle) noexcept {
    detail::CancelDisposition disp;
    {
        std::lock_guard<std::mutex> lk(dispatch_mtx_);
        (void)dispatch_->remove_exact(handle);
        disp = arena_.cancel(handle);
    }
    if (disp == detail::CancelDisposition::terminal_won) {
        bump(stats_, &AsyncStats::canceled_ops);
        signal_ready_progress();
    } else if (disp == detail::CancelDisposition::intent_recorded) {
        issue_running_cancel(handle);
    }

    return disp;
}

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
void UringAsyncBackend::wait_after_commit_before_enqueue_pause_() noexcept {
    auto* g = after_commit_before_enqueue_gate_.load(std::memory_order_acquire);
    if (g == nullptr)
        return;
    g->exited.store(false, std::memory_order_release);
    g->paused.store(true, std::memory_order_release);
    while (!g->resume.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    g->exited.store(true, std::memory_order_release);
}

void UringAsyncBackend::wait_before_dispatch_transfer_pause_() noexcept {
    auto* g = before_dispatch_transfer_gate_.load(std::memory_order_acquire);
    if (g == nullptr)
        return;
    g->exited.store(false, std::memory_order_release);
    g->dispatch_domain_released.store(true, std::memory_order_release);
    g->paused.store(true, std::memory_order_release);
    while (!g->resume.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    g->exited.store(true, std::memory_order_release);
}

void UringAsyncBackend::wait_before_commit_binding_pause_() noexcept {
    auto* g = before_commit_binding_gate_.load(std::memory_order_acquire);
    if (g == nullptr)
        return;
    g->exited.store(false, std::memory_order_release);
    g->admission_domain_held.store(true, std::memory_order_release);
    g->paused.store(true, std::memory_order_release);
    while (!g->resume.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    g->exited.store(true, std::memory_order_release);
}

void UringAsyncBackend::wait_before_admission_lock_pause_() noexcept {
    auto* g = before_admission_lock_gate_.load(std::memory_order_acquire);
    if (g == nullptr)
        return;
    g->exited.store(false, std::memory_order_release);
    g->paused.store(true, std::memory_order_release);
    while (!g->resume.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    g->exited.store(true, std::memory_order_release);
}
#endif

void UringAsyncBackend::cancel(Completion<std::size_t>& c) {
    if (!have_ring_)
        return;
    auto h = arena_.resolve_completion(&c);
    if (!h.has_value())
        return;
    (void)cancel_handle_(*h);
}

void UringAsyncBackend::cancel(Completion<void>& c) {
    if (!have_ring_)
        return;
    auto h = arena_.resolve_completion(&c);
    if (!h.has_value())
        return;
    (void)cancel_handle_(*h);
}

Result<void> UringAsyncBackend::register_waiter(Completion<std::size_t>& c,
                                                detail::WaiterToken token,
                                                detail::RoutingLease lease) {
    auto h = arena_.resolve_completion(&c);
    if (!h.has_value()) {
        return make_unexpected<void>(IoError{IoError::Code::invalid_state});
    }
    return arena_.register_waiter(*h, token, std::move(lease));
}

Result<void> UringAsyncBackend::register_waiter(Completion<void>& c, detail::WaiterToken token,
                                                detail::RoutingLease lease) {
    auto h = arena_.resolve_completion(&c);
    if (!h.has_value()) {
        return make_unexpected<void>(IoError{IoError::Code::invalid_state});
    }
    return arena_.register_waiter(*h, token, std::move(lease));
}

Result<detail::RoutingLease> UringAsyncBackend::cancel_waiter(Completion<std::size_t>& c) {
    auto h = arena_.resolve_completion(&c);
    if (!h.has_value()) {
        return make_unexpected<detail::RoutingLease>(IoError{IoError::Code::not_found});
    }
    return arena_.cancel_waiter(*h);
}

Result<detail::RoutingLease> UringAsyncBackend::cancel_waiter(Completion<void>& c) {
    auto h = arena_.resolve_completion(&c);
    if (!h.has_value()) {
        return make_unexpected<detail::RoutingLease>(IoError{IoError::Code::not_found});
    }
    return arena_.cancel_waiter(*h);
}

void UringAsyncBackend::issue_running_cancel(detail::SlotHandle h) noexcept {
    bool newly_poisoned = false;
    std::uint64_t target_cookie = 0;
    {
        std::lock_guard<std::mutex> lk(dispatch_mtx_);

        if (fatal_error_.has_value())
            return;
        CancelScratch& scratch = cancel_scratch_[h.slot.value];
        if (scratch.cancel_queued)
            return;

        const std::size_t idx = find_live_router_index_(h);
        if (idx == router_.size())
            return;
        target_cookie = router_[idx].cookie;
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
#if defined(SLUICE_TV1_C012_MUTANT)

                return;
#endif

                newly_poisoned = true;
            } else {
                sqe = ::io_uring_get_sqe(&ring_state_->ring);
            }
        }
        if (sqe != nullptr) {
            ::io_uring_prep_cancel64(sqe, target_cookie, 0);
            ::io_uring_sqe_set_data64(sqe, make_control_cookie(target_cookie));
            scratch.cancel_queued = true;
            RouterEntry& route = router_[idx];
            if (route.control_state != RouterEntry::ControlState::none) {
                std::fprintf(stderr, "sluice::async::UringAsyncBackend: duplicate control state "
                                     "before AsyncCancel append (invariant violation)\n");
                std::fflush(stderr);
                std::terminate();
            }
            route.control_state = RouterEntry::ControlState::prepared;
            const auto& sq = ring_state_->ring.sq;
            const std::uint32_t physical_position =
                static_cast<std::uint32_t>((sq.sqe_tail - 1u) & sq.ring_mask);
            transport_ledger_->append(TransportLedger::Kind::cancel_control, physical_position,
                                      target_cookie, h);
        }
    }

    if (newly_poisoned) {
        signal_ready_progress();
    }
}

void UringAsyncBackend::close_admission() {
    if (!have_ring_)
        return;
    {
        std::lock_guard<std::mutex> lk(dispatch_mtx_);
        arena_.close_admission();
        admission_closed_ = true;
    }
    if (wait_source_) {
        wait_source_->interrupt_all();
    }
}

std::size_t UringAsyncBackend::outstanding() const noexcept {
    return arena_.accepted_outstanding();
}

bool UringAsyncBackend::available() const noexcept {
    return available_;
}

#endif

} // namespace sluice::async
