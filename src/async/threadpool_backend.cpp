#include <sluice/async/threadpool_backend.hpp>

#include <sluice/async/detail/fail_fast.hpp>
#include <sluice/async/detail/request_core.hpp>
#include <sluice/detail/file_semantics.hpp>
#include <sluice/detail/posix_retry.hpp>
#include <sluice/error.hpp>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <system_error>
#include <type_traits>
#include <utility>

#include <fcntl.h>
#include <sys/types.h>
#include <unistd.h>

namespace sluice::async {

namespace {

[[noreturn]] void threadpool_dispatch_queue_invariant_fail_fast() noexcept {
    std::fprintf(stderr, "sluice::async::ThreadPoolBackend: post-commit dispatch-ring push "
                         "exceeded capacity (invariant violation)\n");
    std::fflush(stderr);
    std::terminate();
}

[[noreturn]] void threadpool_publication_invariant_fail_fast() noexcept {
    std::fprintf(stderr, "sluice::async::ThreadPoolBackend: publication handoff reached a "
                         "protocol-unreachable state (invariant violation)\n");
    std::fflush(stderr);
    std::terminate();
}

}

void ThreadPoolBackend::BoundedHandleRing::push_back(detail::SlotHandle h) noexcept {
    if (size_ >= capacity_) {
        threadpool_dispatch_queue_invariant_fail_fast();
    }
    std::size_t pos = head_ + size_;
    if (pos >= capacity_)
        pos -= capacity_;
    storage_[pos] = h;
    ++size_;
    if (size_ > high_water_)
        high_water_ = size_;
}

bool ThreadPoolBackend::BoundedHandleRing::pop_front(detail::SlotHandle& out) noexcept {
    if (size_ == 0)
        return false;
    out = storage_[head_];
    head_ = (head_ + 1 == capacity_) ? 0 : head_ + 1;
    --size_;
    return true;
}

bool ThreadPoolBackend::BoundedHandleRing::remove_exact(detail::SlotHandle h) noexcept {
    if (size_ == 0)
        return false;
    for (std::size_t i = 0; i < size_; ++i) {
        std::size_t pos = head_ + i;
        if (pos >= capacity_)
            pos -= capacity_;
        if (storage_[pos].slot.value == h.slot.value &&
            storage_[pos].generation.value == h.generation.value) {
            for (std::size_t j = i; j + 1 < size_; ++j) {
                std::size_t cur = head_ + j;
                if (cur >= capacity_)
                    cur -= capacity_;
                std::size_t nxt = head_ + j + 1;
                if (nxt >= capacity_)
                    nxt -= capacity_;
                storage_[cur] = storage_[nxt];
            }
            --size_;
            return true;
        }
    }
    return false;
}

ThreadPoolBackend::ThreadPoolBackend(ThreadPoolConfig config)
    : capacity_(config.request_capacity), prepared_ops_(config.request_capacity),
      delivery_(config.request_capacity), dispatch_(config.request_capacity),
      publication_pending_(config.request_capacity) {
    if (config.request_capacity == 0 || config.worker_count == 0) {
        throw std::invalid_argument("ThreadPoolConfig fields must be > 0");
    }

    workers_.reserve(config.worker_count);
    stopping_ = false;
    try {
        for (std::size_t i = 0; i < config.worker_count; ++i) {
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)

            if (i == injected_worker_spawn_failure_index()) {
                throw std::system_error(
                    std::make_error_code(std::errc::resource_unavailable_try_again),
                    "injected ThreadPoolBackend worker spawn failure");
            }
#endif
            workers_.emplace_back([this] { worker_loop(); });
        }
    } catch (...) {
        {
            std::lock_guard<std::mutex> lk(work_mtx_);
            stopping_ = true;
        }
        work_cv_.notify_all();
        for (auto& w : workers_) {
            if (w.joinable())
                w.join();
        }
        throw;
    }
}

ThreadPoolBackend::~ThreadPoolBackend() {
    {
        std::lock_guard<std::mutex> lk(work_mtx_);
        const detail::CoreOccupancy occupancy =
            core_ != nullptr ? core_->occupancy() : detail::CoreOccupancy{};
        if (!dispatch_.empty() || active_workers_ != 0 || !publication_pending_.empty() ||
            occupancy.accepted_live != 0) {
            detail::threadpool_non_quiescent_destruction_fail_fast();
        }
        stopping_ = true;
    }
    work_cv_.notify_all();
    for (auto& w : workers_) {
        if (w.joinable())
            w.join();
    }
}

Result<void> ThreadPoolBackend::validate_read(ReadOp op) {
    const sluice::detail::DataOpVerdict verdict = sluice::detail::precheck_data_op(
        {op.file.fd < 0, op.file.access, sluice::detail::FileOperation::read, op.offset, op.len});
    if (verdict == sluice::detail::DataOpVerdict::execute && op.dst == nullptr)
        return make_unexpected<void>(IoError{.code = IoError::Code::invalid_argument});
    return sluice::detail::accept_or_reject(verdict);
}

Result<void> ThreadPoolBackend::validate_write(WriteOp op) {
    const sluice::detail::DataOpVerdict verdict = sluice::detail::precheck_data_op(
        {op.file.fd < 0, op.file.access, sluice::detail::FileOperation::write, op.offset, op.len});
    if (verdict == sluice::detail::DataOpVerdict::execute && op.src == nullptr)
        return make_unexpected<void>(IoError{.code = IoError::Code::invalid_argument});
    return sluice::detail::accept_or_reject(verdict);
}

Result<void> ThreadPoolBackend::validate_sync(SyncDataOp op) {
    return sluice::detail::accept_or_reject(sluice::detail::precheck_state_op(
        op.file.fd < 0, op.file.access, sluice::detail::FileOperation::sync_data));
}

Result<void> ThreadPoolBackend::validate_sync(SyncAllOp op) {
    return sluice::detail::accept_or_reject(sluice::detail::precheck_state_op(
        op.file.fd < 0, op.file.access, sluice::detail::FileOperation::sync_all));
}

Result<detail::RequestKey> ThreadPoolBackend::submit_read(ReadOp op, Completion<std::size_t>* c) {
    return submit_request(op, c, detail::OperationKind::read, detail::RequestOp::read);
}

Result<detail::RequestKey> ThreadPoolBackend::submit_write(WriteOp op, Completion<std::size_t>* c) {
    return submit_request(op, c, detail::OperationKind::write, detail::RequestOp::write);
}

Result<detail::RequestKey> ThreadPoolBackend::submit_sync_data(SyncDataOp op, Completion<void>* c) {
    return submit_request(op, c, detail::OperationKind::sync_data, detail::RequestOp::sync_data);
}

Result<detail::RequestKey> ThreadPoolBackend::submit_sync_all(SyncAllOp op, Completion<void>* c) {
    return submit_request(op, c, detail::OperationKind::sync_all, detail::RequestOp::sync_all);
}

template <class Op> Result<void> ThreadPoolBackend::validate_op(const Op& op) noexcept {
    if constexpr (std::is_same_v<Op, ReadOp>) {
        return validate_read(op);
    } else if constexpr (std::is_same_v<Op, WriteOp>) {
        return validate_write(op);
    } else if constexpr (std::is_same_v<Op, SyncDataOp>) {
        return validate_sync(op);
    } else {
        return validate_sync(op);
    }
}

template <class Op, class Comp>
Result<detail::RequestKey> ThreadPoolBackend::submit_request(Op op, Comp* c,
                                                             detail::OperationKind kind,
                                                             detail::RequestOp core_op) {
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)

    wait_submit_entry_pause_();
#endif
    if (auto v = validate_op(op); !v.has_value()) {
        return make_unexpected<detail::RequestKey>(v.error());
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
    prepared_ops_[h.slot.value] =
        PreparedBlockingOp{kind, op.file.fd, buffer_of(op), static_cast<std::size_t>(length),
                           offset};

    DeliveryRecord& record = delivery_[h.slot.value];
    record.completion = c;
    record.publish = c != nullptr ? publish_thunk<Comp>()
                                  : &ThreadPoolBackend::publish_request_ready;
    record.kind = kind;
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

void ThreadPoolBackend::dispatch_after_accept(detail::SlotHandle h) noexcept {
    bool injected_dispatch_failure = false;
    {
        std::lock_guard<std::mutex> lk(work_mtx_);
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
                detail::threadpool_core_handoff_fail_fast();
            }
            if (core_->release_execution(key) != detail::ExecutionRelease::borrow_touch_fully_retired) {
                detail::threadpool_core_handoff_fail_fast();
            }
            publication_pending_.push_back(h);
            injected_dispatch_failure = true;
        } else
#endif
        {
            dispatch_.push_back(h);
        }
    }
    if (injected_dispatch_failure) {
        signal_ready_progress();
    } else {
        work_cv_.notify_one();
    }
}

void ThreadPoolBackend::publish_zero_op_inline(detail::RequestKey id, detail::SlotHandle h) noexcept {
    if (!core_->acquire_control(id)) {
        detail::threadpool_core_handoff_fail_fast();
    }
    detail::PublicationPayload payload;
    if (core_->begin_publication(id, &payload) != detail::PublicationGrant::granted) {
        detail::threadpool_core_handoff_fail_fast();
    }
    DeliveryRecord& record = delivery_[h.slot.value];
    record.publish(record.completion, payload.outcome);
    if (core_->complete_publication(id) != detail::PublicationCompletion::completed) {
        detail::threadpool_core_handoff_fail_fast();
    }
    record.event_owed = true;
    record.owed_key = id;
    signal_ready_progress();
}

void ThreadPoolBackend::publish_one(detail::SlotHandle h) {
    const detail::RequestKey key{core_->context(), h.slot, h.generation};
    detail::PublicationPayload payload;
    if (core_->begin_publication(key, &payload) != detail::PublicationGrant::granted) {
        threadpool_publication_invariant_fail_fast();
    }
    DeliveryRecord& record = delivery_[h.slot.value];
    record.publish(record.completion, payload.outcome);
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)

    wait_publication_epilogue_pause_();
#endif
    if (core_->complete_publication(key) != detail::PublicationCompletion::completed) {
        threadpool_publication_invariant_fail_fast();
    }
    deliver_event(payload.id, record.kind);
}

void ThreadPoolBackend::deliver_event(detail::RequestKey key, detail::OperationKind kind) {
    (void)core_->claim_observer_delivery(key);
    (routing_sink_ ? *routing_sink_ : sink_).on_ready(detail::ReadyEvent{key, kind});
}

void ThreadPoolBackend::publish_size_ready(void* completion,
                                           const sluice::detail::IoOutcome& outcome) noexcept {
    Result<std::size_t> result = outcome.succeeded
                                     ? Result<std::size_t>{static_cast<std::size_t>(
                                           outcome.effect.confirmed_bytes)}
                                     : make_unexpected<std::size_t>(outcome.error);
    AsyncBackend::publish(*static_cast<Completion<std::size_t>*>(completion), std::move(result));
}

void ThreadPoolBackend::publish_void_ready(void* completion,
                                           const sluice::detail::IoOutcome& outcome) noexcept {
    Result<void> result = outcome.succeeded
                              ? Result<void>{}
                              : make_unexpected<void>(outcome.error);
    AsyncBackend::publish(*static_cast<Completion<void>*>(completion), std::move(result));
}

void ThreadPoolBackend::publish_request_ready(void* completion,
                                              const sluice::detail::IoOutcome& outcome) noexcept {
    (void)completion;
    (void)outcome;
}

void ThreadPoolBackend::worker_loop() {
    try {
        for (;;) {
            detail::SlotHandle h{};
            PreparedBlockingOp op{};
            bool have_op = false;
            {
                std::unique_lock<std::mutex> lk(work_mtx_);
                work_cv_.wait(lk, [&] { return stopping_ || !dispatch_.empty(); });
                if (stopping_ && dispatch_.empty())
                    return;
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)

                wait_before_dequeue_pause_();
#endif
                if (!dispatch_.pop_front(h))
                    continue;

                const detail::RequestKey key{core_->context(), h.slot, h.generation};
                if (core_->claim_execution(key) != detail::ExecutionClaim::claimed) {
                    continue;
                }
                op = prepared_ops_[h.slot.value];
                ++active_workers_;
                have_op = true;
            }

            if (have_op) {
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)

                wait_worker_claimed_pause_();
#endif
                const sluice::detail::IoOutcome outcome = run_syscall(op);

                syscall_count_.fetch_add(1, std::memory_order_relaxed);
                {
                    std::lock_guard<std::mutex> wl(work_mtx_);
                    --active_workers_;
                }
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)

                wait_worker_outcome_pre_terminal_pause_();
#endif

                const detail::RequestKey key{core_->context(), h.slot, h.generation};
                detail::TerminalCandidate candidate;
                candidate.kind = detail::TerminalCandidateKind::physical_outcome;
                candidate.outcome = outcome;
                if (core_->offer_terminal(key, candidate) != detail::TerminalVerdict::chosen) {
                    detail::threadpool_core_handoff_fail_fast();
                }
                if (core_->release_execution(key) !=
                    detail::ExecutionRelease::borrow_touch_fully_retired) {
                    detail::threadpool_core_handoff_fail_fast();
                }
                {
                    std::lock_guard<std::mutex> wl(work_mtx_);
                    publication_pending_.push_back(h);
                }

                signal_ready_progress();
            }
        }
    } catch (...) {
        std::fprintf(stderr, "sluice::async::ThreadPoolBackend: worker exception escaped "
                             "(invariant violation)\n");
        std::fflush(stderr);
        std::terminate();
    }
}

sluice::detail::IoOutcome ThreadPoolBackend::run_syscall(const PreparedBlockingOp& p) noexcept {
    errno = 0;
    switch (p.kind) {
    case detail::OperationKind::read: {
        ssize_t n = sluice::detail::retry_on_eintr([&] {
            return ::pread(p.fd, const_cast<std::byte*>(p.buffer), p.length,
                           static_cast<off_t>(static_cast<std::int64_t>(p.offset)));
        });
        if (n < 0)
            return sluice::detail::failed_dispatched_attempt(sluice::from_errno_value(errno));
        return sluice::detail::IoOutcome::success(static_cast<std::uint64_t>(n));
    }
    case detail::OperationKind::write: {
        ssize_t n = sluice::detail::retry_on_eintr([&] {
            return ::pwrite(p.fd, p.buffer, p.length,
                            static_cast<off_t>(static_cast<std::int64_t>(p.offset)));
        });
        if (n < 0)
            return sluice::detail::failed_dispatched_attempt(sluice::from_errno_value(errno));
        return sluice::detail::IoOutcome::success(static_cast<std::uint64_t>(n));
    }
    case detail::OperationKind::sync_data: {
        int rc = sluice::detail::retry_on_eintr([&] { return ::fdatasync(p.fd); });
        if (rc < 0)
            return sluice::detail::failed_dispatched_attempt(sluice::from_errno_value(errno));
        return sluice::detail::IoOutcome::success();
    }
    case detail::OperationKind::sync_all: {
        int rc = sluice::detail::retry_on_eintr([&] { return ::fsync(p.fd); });
        if (rc < 0)
            return sluice::detail::failed_dispatched_attempt(sluice::from_errno_value(errno));
        return sluice::detail::IoOutcome::success();
    }
    }
    return sluice::detail::IoOutcome::failure(IoError{IoError::Code::backend_error});
}

std::size_t ThreadPoolBackend::poll() {
    std::size_t published = 0;
    for (;;) {
        detail::SlotHandle h{};
        bool have = false;
        {
            std::lock_guard<std::mutex> lk(work_mtx_);
            have = publication_pending_.pop_front(h);
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
            detail::threadpool_core_handoff_fail_fast();
        }
        ++published;
    }
    return published;
}

Result<std::size_t> ThreadPoolBackend::wait_one() {
    for (;;) {
        BackendWaitToken token = ready_wait_.snapshot();
        std::size_t n = poll();
        if (n > 0)
            return n;
        if (ready_wait_.wait_for_change(token) == BackendWakeReason::interrupted) {
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)

            wait_control_wake_final_reap_pause_();
#endif

            n = poll();
            if (n > 0)
                return n;
            return std::size_t{0};
        }
    }
}

void ThreadPoolBackend::signal_ready_progress() noexcept {
    ready_wait_.signal_progress();
}

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)

void ThreadPoolBackend::wait_submit_entry_pause_() noexcept {
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

void ThreadPoolBackend::wait_pre_accept_commit_pause_() noexcept {
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

void ThreadPoolBackend::wait_accepted_pre_dispatch_pause_() noexcept {
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

void ThreadPoolBackend::wait_before_dequeue_pause_() noexcept {
    auto* g = before_dequeue_gate_.load(std::memory_order_acquire);
    if (g == nullptr)
        return;
    g->exited.store(false, std::memory_order_release);
    g->paused.store(true, std::memory_order_release);
    g->paused.notify_all();
    g->resume.wait(false, std::memory_order_acquire);
    g->exited.store(true, std::memory_order_release);
    g->exited.notify_all();
}

void ThreadPoolBackend::wait_worker_claimed_pause_() noexcept {
    auto* g = worker_claimed_gate_.load(std::memory_order_acquire);
    if (g == nullptr)
        return;
    g->exited.store(false, std::memory_order_release);
    g->paused.store(true, std::memory_order_release);
    g->paused.notify_all();
    g->resume.wait(false, std::memory_order_acquire);
    g->exited.store(true, std::memory_order_release);
    g->exited.notify_all();
}

void ThreadPoolBackend::wait_worker_outcome_pre_terminal_pause_() noexcept {
    auto* g = worker_outcome_pre_terminal_gate_.load(std::memory_order_acquire);
    if (g == nullptr)
        return;
    g->exited.store(false, std::memory_order_release);
    g->paused.store(true, std::memory_order_release);
    g->paused.notify_all();
    g->resume.wait(false, std::memory_order_acquire);
    g->exited.store(true, std::memory_order_release);
    g->exited.notify_all();
}

void ThreadPoolBackend::wait_publication_epilogue_pause_() noexcept {
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

void ThreadPoolBackend::wait_control_wake_final_reap_pause_() noexcept {
    auto* g = control_wake_final_reap_gate_.load(std::memory_order_acquire);
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
ThreadPoolBackend::injected_precommit_stage_failure_(SubmitStage stage) noexcept {
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

detail::PublicCancel ThreadPoolBackend::cancel_key(detail::RequestKey key) {
    detail::PublicCancel disposition;
    {
        std::lock_guard<std::mutex> lk(work_mtx_);

        (void)dispatch_.remove_exact(detail::SlotHandle{key.slot, key.generation});
        disposition = core_->cancel(key);
        if (disposition == detail::PublicCancel::won_before_execution) {
            if (core_->release_execution(key) !=
                detail::ExecutionRelease::borrow_touch_fully_retired) {
                detail::threadpool_core_handoff_fail_fast();
            }
            publication_pending_.push_back(detail::SlotHandle{key.slot, key.generation});
        }
    }
    if (disposition == detail::PublicCancel::won_before_execution) {
        tally_canceled();
        signal_ready_progress();
    }
    return disposition;
}

void ThreadPoolBackend::cancel(Completion<std::size_t>& c) {
    auto key = core_binding(c);
    if (!key.has_value())
        return;
    (void)cancel_key(*key);
}

void ThreadPoolBackend::cancel(Completion<void>& c) {
    auto key = core_binding(c);
    if (!key.has_value())
        return;
    (void)cancel_key(*key);
}

detail::PublicCancel ThreadPoolBackend::cancel_identity(detail::RequestKey key) {
    return cancel_key(key);
}

std::size_t ThreadPoolBackend::outstanding() const noexcept {
    return core_ != nullptr ? core_->occupancy().outstanding : 0;
}

std::size_t ThreadPoolBackend::dispatch_occupancy() const {
    std::lock_guard<std::mutex> lk(work_mtx_);
    return dispatch_.size();
}

std::size_t ThreadPoolBackend::dispatch_high_water_mark() const {
    std::lock_guard<std::mutex> lk(work_mtx_);
    return dispatch_.high_water();
}

std::size_t ThreadPoolBackend::active_workers() const {
    std::lock_guard<std::mutex> lk(work_mtx_);
    return active_workers_;
}

Result<RequestHandleState> ThreadPoolBackend::resolve_identity_state(
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

void ThreadPoolBackend::close_admission() {
    if (core_ != nullptr) {
        core_->close_admission();
    }
    ready_wait_.interrupt_all();
}

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
namespace {

std::atomic<std::size_t> g_injected_worker_spawn_failure_index{
    std::numeric_limits<std::size_t>::max()};
}

std::size_t ThreadPoolBackend::injected_worker_spawn_failure_index() noexcept {
    return g_injected_worker_spawn_failure_index.load(std::memory_order_acquire);
}

void ThreadPoolBackend::set_injected_worker_spawn_failure_index(std::size_t index) noexcept {
    g_injected_worker_spawn_failure_index.store(index, std::memory_order_release);
}
#endif

}
