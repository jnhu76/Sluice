#include <sluice/async/threadpool_backend.hpp>

#include <sluice/async/detail/fail_fast.hpp>
#include <sluice/detail/io_validation.hpp>
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

} // namespace

void ThreadPoolBackend::BoundedDispatchQueue::push_back(detail::SlotHandle h) noexcept {
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

bool ThreadPoolBackend::BoundedDispatchQueue::pop_front(detail::SlotHandle& out) noexcept {
    if (size_ == 0)
        return false;
    out = storage_[head_];
    head_ = (head_ + 1 == capacity_) ? 0 : head_ + 1;
    --size_;
    return true;
}

bool ThreadPoolBackend::BoundedDispatchQueue::remove_exact(detail::SlotHandle h) noexcept {
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
    : arena_(detail::ContextIdentity::for_testing(next_backend_id()), config.request_capacity),
      prepared_ops_(config.request_capacity), dispatch_(config.request_capacity) {
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
        auto q = arena_.quiescence_snapshot();
        if (!dispatch_.empty() || active_workers_ != 0 || q.slot_in_use != 0 ||
            q.accepted_outstanding != 0 || q.backend_ready != 0) {
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
    if (op.fd < 0)
        return make_unexpected<void>(IoError{IoError::Code::invalid_argument});
    if (op.len > 0 && op.dst == nullptr) {
        return make_unexpected<void>(IoError{IoError::Code::invalid_argument});
    }
    auto off = sluice::detail::checked_posix_offset(op.offset);
    if (!off.has_value()) {
        return make_unexpected<void>(IoError{IoError::Code::invalid_argument});
    }

    if (op.len > static_cast<std::size_t>(std::numeric_limits<ssize_t>::max())) {
        return make_unexpected<void>(IoError{IoError::Code::invalid_argument});
    }
    return {};
}

Result<void> ThreadPoolBackend::validate_write(WriteOp op) {
    if (op.fd < 0)
        return make_unexpected<void>(IoError{IoError::Code::invalid_argument});
    if (op.len > 0 && op.src == nullptr) {
        return make_unexpected<void>(IoError{IoError::Code::invalid_argument});
    }
    auto off = sluice::detail::checked_posix_offset(op.offset);
    if (!off.has_value()) {
        return make_unexpected<void>(IoError{IoError::Code::invalid_argument});
    }
    if (op.len > static_cast<std::size_t>(std::numeric_limits<ssize_t>::max())) {
        return make_unexpected<void>(IoError{IoError::Code::invalid_argument});
    }
    return {};
}

Result<void> ThreadPoolBackend::validate_sync(SyncDataOp op) {
    if (op.fd < 0)
        return make_unexpected<void>(IoError{IoError::Code::invalid_argument});
    return {};
}

Result<void> ThreadPoolBackend::validate_sync(SyncAllOp op) {
    if (op.fd < 0)
        return make_unexpected<void>(IoError{IoError::Code::invalid_argument});
    return {};
}

Result<void> ThreadPoolBackend::submit_read(ReadOp op, Completion<std::size_t>& c) {
    return submit_size(op, c, detail::OperationKind::read);
}

Result<void> ThreadPoolBackend::submit_write(WriteOp op, Completion<std::size_t>& c) {
    return submit_size(op, c, detail::OperationKind::write);
}

Result<void> ThreadPoolBackend::submit_sync_data(SyncDataOp op, Completion<void>& c) {
    return submit_void(op, c, detail::OperationKind::sync_data);
}

Result<void> ThreadPoolBackend::submit_sync_all(SyncAllOp op, Completion<void>& c) {
    return submit_void(op, c, detail::OperationKind::sync_all);
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

template <class Op>
Result<void> ThreadPoolBackend::submit_size(Op op, Completion<std::size_t>& c,
                                            detail::OperationKind kind) {
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)

    wait_before_admission_lock_pause_();
#endif

    detail::SlotHandle h{};
    {
        std::lock_guard<std::mutex> admission_lk(admission_mtx_);
        SubmitPolicy<Op, Completion<std::size_t>> policy{*this, kind};
        auto r = detail::submit_transaction(arena_, c, op, policy);
        if (!r.has_value()) {
            return make_unexpected<void>(r.error());
        }
        h = r.value();
    }

    enqueue_after_commit(h);
    return {};
}

template <class Op>
Result<void> ThreadPoolBackend::submit_void(Op op, Completion<void>& c,
                                            detail::OperationKind kind) {
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)

    wait_before_admission_lock_pause_();
#endif
    detail::SlotHandle h{};
    {
        std::lock_guard<std::mutex> admission_lk(admission_mtx_);
        SubmitPolicy<Op, Completion<void>> policy{*this, kind};
        auto r = detail::submit_transaction(arena_, c, op, policy);
        if (!r.has_value()) {
            return make_unexpected<void>(r.error());
        }
        h = r.value();
    }
    enqueue_after_commit(h);
    return {};
}

void ThreadPoolBackend::enqueue_after_commit(detail::SlotHandle h) noexcept {
    detail::EnqueueOutcome outcome;
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
    bool injected_dispatch_failure = false;

    wait_before_enqueue_lock_pause_();
#endif
    {
        std::lock_guard<std::mutex> lk(work_mtx_);
        outcome = arena_.enqueue(h);
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
        if (outcome == detail::EnqueueOutcome::enqueued) {
            wait_after_enqueue_before_push_pause_(true);

            auto* inj = dispatch_failure_injection_.load(std::memory_order_acquire);
            if (inj != nullptr && inj->armed.load(std::memory_order_acquire)) {
                inj->fired.fetch_add(1, std::memory_order_relaxed);
                (void)arena_.record_terminal(
                    h, detail::TerminalResult::err(IoError{IoError::Code::backend_error}));
                injected_dispatch_failure = true;
            }
        }
#endif
        if (outcome == detail::EnqueueOutcome::enqueued) {
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
            if (!injected_dispatch_failure)
#endif
            {
                dispatch_.push_back(h);
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
                {
                    auto* g = after_enqueue_before_push_gate_.load(std::memory_order_acquire);
                    if (g != nullptr) {
                        g->dispatch_push_completed.store(true, std::memory_order_release);
                    }
                }
#endif
            }
        }
    }
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
    if (injected_dispatch_failure) {
        signal_ready_progress();
    } else
#endif
        if (outcome == detail::EnqueueOutcome::enqueued) {
        work_cv_.notify_one();
    } else {
        signal_ready_progress();
    }
}

void ThreadPoolBackend::publish_size_ready(void* completion,
                                           const detail::TerminalResult& t) noexcept {
    AsyncBackend::publish(*static_cast<Completion<std::size_t>*>(completion), terminal_to_size(t));
}

void ThreadPoolBackend::publish_void_ready(void* completion,
                                           const detail::TerminalResult& t) noexcept {
    AsyncBackend::publish(*static_cast<Completion<void>*>(completion), terminal_to_void(t));
}

Result<std::size_t> ThreadPoolBackend::terminal_to_size(const detail::TerminalResult& t) noexcept {
    if (t.stored && t.is_error)
        return make_unexpected<std::size_t>(t.error);
    return Result<std::size_t>{static_cast<std::size_t>(t.bytes)};
}

Result<void> ThreadPoolBackend::terminal_to_void(const detail::TerminalResult& t) noexcept {
    if (t.stored && t.is_error)
        return make_unexpected<void>(t.error);
    return {};
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
                std::uint64_t dequeue_gate_generation = 0;
                if (before_dequeue_gate_.load(std::memory_order_acquire) != nullptr) {
                    lk.unlock();

                    dequeue_gate_generation = wait_before_dequeue_pause_();

                    wait_post_resume_pre_pop_hold_();
                    lk.lock();
                    if (stopping_ && dispatch_.empty()) {
                        ack_dequeue_gate_generation_(dequeue_gate_generation);
                        return;
                    }
                }

                const bool popped = dispatch_.pop_front(h);
                ack_dequeue_gate_generation_(dequeue_gate_generation);
                if (!popped)
                    continue;
#else
                if (!dispatch_.pop_front(h))
                    continue;
#endif

                bool owns = arena_.mark_running(h);
                if (!owns)
                    continue;
                op = prepared_ops_[h.slot.value];
                ++active_workers_;
                have_op = true;
            }

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
            if (have_op)
                wait_running_pause_();
#endif
            if (have_op) {
                detail::TerminalResult terminal = run_syscall(op);

                syscall_count_.fetch_add(1, std::memory_order_relaxed);
                {
                    std::lock_guard<std::mutex> wl(work_mtx_);
                    --active_workers_;
                }
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)

                wait_terminal_publication_pause_();
#endif

                (void)arena_.record_terminal(h, terminal);

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

detail::TerminalResult ThreadPoolBackend::run_syscall(const PreparedBlockingOp& p) noexcept {
    errno = 0;
    switch (p.kind) {
    case detail::OperationKind::read: {
        ssize_t n = sluice::detail::retry_on_eintr([&] {
            return ::pread(p.fd, const_cast<std::byte*>(p.buffer), p.length,
                           static_cast<off_t>(static_cast<std::int64_t>(p.offset)));
        });
        if (n < 0)
            return detail::TerminalResult::err(sluice::from_errno_value(errno));
        return detail::TerminalResult::ok_bytes(static_cast<std::uint64_t>(n));
    }
    case detail::OperationKind::write: {
        ssize_t n = sluice::detail::retry_on_eintr([&] {
            return ::pwrite(p.fd, p.buffer, p.length,
                            static_cast<off_t>(static_cast<std::int64_t>(p.offset)));
        });
        if (n < 0)
            return detail::TerminalResult::err(sluice::from_errno_value(errno));
        return detail::TerminalResult::ok_bytes(static_cast<std::uint64_t>(n));
    }
    case detail::OperationKind::sync_data: {
        int rc = sluice::detail::retry_on_eintr([&] { return ::fdatasync(p.fd); });
        if (rc < 0)
            return detail::TerminalResult::err(sluice::from_errno_value(errno));
        return detail::TerminalResult::ok_void();
    }
    case detail::OperationKind::sync_all: {
        int rc = sluice::detail::retry_on_eintr([&] { return ::fsync(p.fd); });
        if (rc < 0)
            return detail::TerminalResult::err(sluice::from_errno_value(errno));
        return detail::TerminalResult::ok_void();
    }
    }
    return detail::TerminalResult::err(IoError{IoError::Code::backend_error});
}

std::size_t ThreadPoolBackend::poll() {
    return arena_.reap(routing_sink_ ? *routing_sink_ : sink_);
}

Result<std::size_t> ThreadPoolBackend::wait_one() {
    for (;;) {
        BackendWaitToken token = ready_wait_.snapshot();
        std::size_t n = arena_.reap(routing_sink_ ? *routing_sink_ : sink_);
        if (n > 0)
            return n;
        if (ready_wait_.wait_for_change(token) == BackendWakeReason::interrupted) {
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)

            wait_control_wake_final_reap_pause_();
#endif

            n = arena_.reap(routing_sink_ ? *routing_sink_ : sink_);
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

void ThreadPoolBackend::wait_after_enqueue_before_push_pause_(bool inside_work_mtx) noexcept {
    auto* g = after_enqueue_before_push_gate_.load(std::memory_order_acquire);
    if (g == nullptr)
        return;
    g->exited.store(false, std::memory_order_release);
    g->work_domain_held.store(inside_work_mtx, std::memory_order_release);
    g->dispatch_push_completed.store(false, std::memory_order_release);
    g->paused.store(true, std::memory_order_release);
    g->paused.notify_one();
    g->resume.wait(false, std::memory_order_acquire);
    g->exited.store(true, std::memory_order_release);
    g->exited.notify_one();
}

std::uint64_t ThreadPoolBackend::wait_before_dequeue_pause_() noexcept {
    auto* g = before_dequeue_gate_.load(std::memory_order_acquire);
    if (g == nullptr)
        return 0;
    const std::uint64_t generation = g->armed.load(std::memory_order_acquire);
    if (generation == 0) {
        g->exited.store(false, std::memory_order_release);
        g->paused.store(true, std::memory_order_release);
        g->paused.notify_one();
        g->resume.wait(false, std::memory_order_acquire);
        g->exited.store(true, std::memory_order_release);
        g->exited.notify_one();
        return 0;
    }

    dequeue_gate_detail::publish_max_(g->paused_at, generation);
    g->paused_at.notify_all();
    std::uint64_t seen = g->resumed_at.load(std::memory_order_acquire);
    while (seen < generation) {
        g->resumed_at.wait(seen, std::memory_order_acquire);
        seen = g->resumed_at.load(std::memory_order_acquire);
    }
    return generation;
}

void ThreadPoolBackend::ack_dequeue_gate_generation_(std::uint64_t generation) noexcept {
    if (generation == 0)
        return;
    auto* g = before_dequeue_gate_.load(std::memory_order_acquire);
    if (g == nullptr)
        return;
    dequeue_gate_detail::publish_max_(g->acked_at, generation);
    g->acked_at.notify_all();
}

void ThreadPoolBackend::wait_post_resume_pre_pop_hold_() noexcept {
    auto* g = post_resume_pre_pop_hold_gate_.load(std::memory_order_acquire);
    if (g == nullptr)
        return;
    g->exited.store(false, std::memory_order_release);
    g->paused.store(true, std::memory_order_release);
    g->paused.notify_one();
    g->resume.wait(false, std::memory_order_acquire);
    g->exited.store(true, std::memory_order_release);
    g->exited.notify_one();
}

void ThreadPoolBackend::wait_before_enqueue_lock_pause_() noexcept {
    auto* g = before_enqueue_lock_gate_.load(std::memory_order_acquire);
    if (g == nullptr)
        return;
    g->exited.store(false, std::memory_order_release);
    g->paused.store(true, std::memory_order_release);
    g->paused.notify_one();
    g->resume.wait(false, std::memory_order_acquire);
    g->exited.store(true, std::memory_order_release);
    g->exited.notify_one();
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

void ThreadPoolBackend::wait_running_pause_() noexcept {
    auto* g = running_gate_.load(std::memory_order_acquire);
    if (g == nullptr)
        return;
    g->exited.store(false, std::memory_order_release);
    g->paused.store(true, std::memory_order_release);
    g->paused.notify_one();
    g->resume.wait(false, std::memory_order_acquire);
    g->exited.store(true, std::memory_order_release);
    g->exited.notify_one();
}

void ThreadPoolBackend::wait_terminal_publication_pause_() noexcept {
    auto* g = terminal_publication_gate_.load(std::memory_order_acquire);
    if (g == nullptr)
        return;
    g->exited.store(false, std::memory_order_release);
    g->paused.store(true, std::memory_order_release);
    g->paused.notify_one();
    g->resume.wait(false, std::memory_order_acquire);
    g->exited.store(true, std::memory_order_release);
    g->exited.notify_one();
}

void ThreadPoolBackend::wait_control_wake_final_reap_pause_() noexcept {
    auto* g = control_wake_final_reap_gate_.load(std::memory_order_acquire);
    if (g == nullptr)
        return;
    g->exited.store(false, std::memory_order_release);
    g->paused.store(true, std::memory_order_release);
    g->paused.notify_one();
    g->resume.wait(false, std::memory_order_acquire);
    g->exited.store(true, std::memory_order_release);
    g->exited.notify_one();
}

void ThreadPoolBackend::wait_before_admission_lock_pause_() noexcept {
    auto* g = before_admission_lock_gate_.load(std::memory_order_acquire);
    if (g == nullptr)
        return;
    g->exited.store(false, std::memory_order_release);
    g->paused.store(true, std::memory_order_release);
    g->paused.notify_one();
    g->resume.wait(false, std::memory_order_acquire);
    g->exited.store(true, std::memory_order_release);
    g->exited.notify_one();
}

void ThreadPoolBackend::wait_before_commit_binding_pause_() noexcept {
    auto* g = before_commit_binding_gate_.load(std::memory_order_acquire);
    if (g == nullptr)
        return;
    g->exited.store(false, std::memory_order_release);
    g->paused.store(true, std::memory_order_release);
    g->paused.notify_one();
    g->resume.wait(false, std::memory_order_acquire);
    g->exited.store(true, std::memory_order_release);
    g->exited.notify_one();
}
#endif

void ThreadPoolBackend::cancel(Completion<std::size_t>& c) {
    auto h = arena_.resolve_completion(&c);
    if (!h.has_value())
        return;
    detail::SlotHandle handle = *h;
    detail::CancelDisposition disp;
    {
        std::lock_guard<std::mutex> lk(work_mtx_);

        (void)dispatch_.remove_exact(handle);
        disp = arena_.cancel(handle);
    }
    if (disp == detail::CancelDisposition::terminal_won) {
        tally_canceled();
        signal_ready_progress();
    }
}

void ThreadPoolBackend::cancel(Completion<void>& c) {
    auto h = arena_.resolve_completion(&c);
    if (!h.has_value())
        return;
    detail::SlotHandle handle = *h;
    detail::CancelDisposition disp;
    {
        std::lock_guard<std::mutex> lk(work_mtx_);
        (void)dispatch_.remove_exact(handle);
        disp = arena_.cancel(handle);
    }
    if (disp == detail::CancelDisposition::terminal_won) {
        tally_canceled();
        signal_ready_progress();
    }
}

Result<void> ThreadPoolBackend::register_waiter(Completion<std::size_t>& c,
                                                detail::WaiterToken token,
                                                detail::RoutingLease lease) {
    auto h = arena_.resolve_completion(&c);
    if (!h.has_value()) {
        return make_unexpected<void>(IoError{IoError::Code::invalid_state});
    }
    return arena_.register_waiter(*h, token, std::move(lease));
}

Result<void> ThreadPoolBackend::register_waiter(Completion<void>& c, detail::WaiterToken token,
                                                detail::RoutingLease lease) {
    auto h = arena_.resolve_completion(&c);
    if (!h.has_value()) {
        return make_unexpected<void>(IoError{IoError::Code::invalid_state});
    }
    return arena_.register_waiter(*h, token, std::move(lease));
}

Result<detail::RoutingLease> ThreadPoolBackend::cancel_waiter(Completion<std::size_t>& c) {
    auto h = arena_.resolve_completion(&c);
    if (!h.has_value()) {
        return make_unexpected<detail::RoutingLease>(IoError{IoError::Code::not_found});
    }
    return arena_.cancel_waiter(*h);
}

Result<detail::RoutingLease> ThreadPoolBackend::cancel_waiter(Completion<void>& c) {
    auto h = arena_.resolve_completion(&c);
    if (!h.has_value()) {
        return make_unexpected<detail::RoutingLease>(IoError{IoError::Code::not_found});
    }
    return arena_.cancel_waiter(*h);
}

std::size_t ThreadPoolBackend::outstanding() const noexcept {
    return arena_.accepted_outstanding();
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

void ThreadPoolBackend::close_admission() {
    {
        std::lock_guard<std::mutex> lk(admission_mtx_);
        arena_.close_admission();
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

} // namespace sluice::async
