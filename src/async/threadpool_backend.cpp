// sluice::async::ThreadPoolBackend — Phase E implementation.
//
// Bounded persistent blocking-I/O backend driving real POSIX syscalls through
// the RequestArena / RequestSlot lifecycle. See threadpool_backend.hpp and
// docs/design/phase-e-bounded-threadpool-backend.md for the frozen design.
//
// Phase E replaces the legacy "one std::thread per op + std::function +
// Completion* ready deque" model (DIV-03 / DIV-12) with a fixed worker pool, a
// bounded dispatch ring, and RequestArena as the single request-lifecycle
// authority. Workers consume SlotHandles, run the syscall, and record
// backend-ready ONLY; reap (poll/wait_one) is the sole Completion-ready
// publication authority.
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
#include <type_traits>
#include <utility>

#include <fcntl.h>
#include <sys/types.h>
#include <unistd.h>

namespace sluice::async {

namespace {

// Fail-fast for a post-commit dispatch-ring push that cannot fit. Because the
// dispatch capacity equals the request capacity and a committed request holds
// its slot, a full push means an invariant violation (double-enqueue or ring
// accounting corruption), never legitimate capacity pressure (AGENTS.md §12).
[[noreturn]] void threadpool_dispatch_queue_invariant_fail_fast() noexcept {
    std::fprintf(stderr,
                 "sluice::async::ThreadPoolBackend: post-commit dispatch-ring push "
                 "exceeded capacity (invariant violation)\n");
    std::fflush(stderr);
    std::terminate();
}

}  // namespace

// ---------------------------------------------------------------------------
// BoundedDispatchQueue
// ---------------------------------------------------------------------------

void ThreadPoolBackend::BoundedDispatchQueue::push_back(detail::SlotHandle h) noexcept {
    if (size_ >= capacity_) {
        threadpool_dispatch_queue_invariant_fail_fast();
    }
    std::size_t pos = head_ + size_;
    if (pos >= capacity_) pos -= capacity_;
    storage_[pos] = h;
    ++size_;
    if (size_ > high_water_) high_water_ = size_;
}

bool ThreadPoolBackend::BoundedDispatchQueue::pop_front(detail::SlotHandle& out) noexcept {
    if (size_ == 0) return false;
    out = storage_[head_];
    head_ = (head_ + 1 == capacity_) ? 0 : head_ + 1;
    --size_;
    return true;
}

bool ThreadPoolBackend::BoundedDispatchQueue::remove_exact(detail::SlotHandle h) noexcept {
    // Walk the logical ring; O(capacity) bounded compaction. When found, shift
    // the intervening entries forward to preserve FIFO order, then shrink.
    if (size_ == 0) return false;
    for (std::size_t i = 0; i < size_; ++i) {
        std::size_t pos = head_ + i;
        if (pos >= capacity_) pos -= capacity_;
        if (storage_[pos].slot.value == h.slot.value &&
            storage_[pos].generation.value == h.generation.value) {
            // Shift the tail forward one slot to close the gap.
            for (std::size_t j = i; j + 1 < size_; ++j) {
                std::size_t cur = head_ + j;
                if (cur >= capacity_) cur -= capacity_;
                std::size_t nxt = head_ + j + 1;
                if (nxt >= capacity_) nxt -= capacity_;
                storage_[cur] = storage_[nxt];
            }
            --size_;
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

ThreadPoolBackend::ThreadPoolBackend(ThreadPoolConfig config)
    : arena_(detail::ContextIdentity::for_testing(next_backend_id()), config.request_capacity),
      prepared_ops_(config.request_capacity),
      dispatch_(config.request_capacity) {
    if (config.request_capacity == 0 || config.worker_count == 0) {
        throw std::invalid_argument("ThreadPoolConfig fields must be > 0");
    }
    // Launch the fixed persistent worker pool. If a thread fails to spawn, set
    // stopping_, wake the already-started workers, join them, and rethrow —
    // never let a joinable thread vector terminate the process on destruction.
    workers_.reserve(config.worker_count);
    stopping_ = false;
    try {
        for (std::size_t i = 0; i < config.worker_count; ++i) {
            workers_.emplace_back([this] { worker_loop(); });
        }
    } catch (...) {
        {
            std::lock_guard<std::mutex> lk(work_mtx_);
            stopping_ = true;
        }
        work_cv_.notify_all();
        for (auto& w : workers_) {
            if (w.joinable()) w.join();
        }
        throw;
    }
}

ThreadPoolBackend::~ThreadPoolBackend() {
    // Quiescent persistent-worker teardown only (AGENTS.md §14): verify no
    // accepted work, active worker, or ring entry remains, then set stop,
    // notify all idle workers, join the fixed pool. This join is worker-pool
    // teardown, NOT an I/O drain. Non-quiescent destruction fail-fasts in BOTH
    // Debug and Release (ADR Decision 15).
    {
        std::lock_guard<std::mutex> lk(work_mtx_);
        auto q = arena_.quiescence_snapshot();  // arena leaf lock under work_mtx_
        if (!dispatch_.empty() || active_workers_ != 0 ||
            q.slot_in_use != 0 || q.accepted_outstanding != 0 ||
            q.backend_ready != 0) {
            detail::threadpool_non_quiescent_destruction_fail_fast();
        }
        stopping_ = true;
    }
    work_cv_.notify_all();
    for (auto& w : workers_) {
        if (w.joinable()) w.join();
    }
}

// ---------------------------------------------------------------------------
// Descriptor validation (real syscall backend — DIV-14 does NOT apply)
// ---------------------------------------------------------------------------

Result<void> ThreadPoolBackend::validate_read(ReadOp op) {
    if (op.fd < 0) return make_unexpected<void>(IoError{IoError::Code::invalid_argument});
    if (op.len > 0 && op.dst == nullptr) {
        return make_unexpected<void>(IoError{IoError::Code::invalid_argument});
    }
    auto off = sluice::detail::checked_posix_offset(op.offset);
    if (!off.has_value()) {
        return make_unexpected<void>(IoError{IoError::Code::invalid_argument});
    }
    // pread returns ssize_t; a length beyond SSIZE_MAX cannot be represented as
    // a byte count and may be misread as an error by the syscall.
    if (op.len > static_cast<std::size_t>(std::numeric_limits<ssize_t>::max())) {
        return make_unexpected<void>(IoError{IoError::Code::invalid_argument});
    }
    return {};
}

Result<void> ThreadPoolBackend::validate_write(WriteOp op) {
    if (op.fd < 0) return make_unexpected<void>(IoError{IoError::Code::invalid_argument});
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
    if (op.fd < 0) return make_unexpected<void>(IoError{IoError::Code::invalid_argument});
    return {};
}

Result<void> ThreadPoolBackend::validate_sync(SyncAllOp op) {
    if (op.fd < 0) return make_unexpected<void>(IoError{IoError::Code::invalid_argument});
    return {};
}

// ---------------------------------------------------------------------------
// Five-stage admission
// ---------------------------------------------------------------------------

Result<void> ThreadPoolBackend::submit_read(ReadOp op, Completion<std::size_t>& c) {
    auto v = validate_read(op);
    if (!v.has_value()) return v;
    return submit_size(op, c, detail::OperationKind::read, op.len);
}

Result<void> ThreadPoolBackend::submit_write(WriteOp op, Completion<std::size_t>& c) {
    auto v = validate_write(op);
    if (!v.has_value()) return v;
    return submit_size(op, c, detail::OperationKind::write, op.len);
}

Result<void> ThreadPoolBackend::submit_sync_data(SyncDataOp op, Completion<void>& c) {
    auto v = validate_sync(op);
    if (!v.has_value()) return v;
    return submit_void(op, c, detail::OperationKind::sync_data);
}

Result<void> ThreadPoolBackend::submit_sync_all(SyncAllOp op, Completion<void>& c) {
    auto v = validate_sync(op);
    if (!v.has_value()) return v;
    return submit_void(op, c, detail::OperationKind::sync_all);
}

template <class Op>
Result<void> ThreadPoolBackend::submit_size(Op op, Completion<std::size_t>& c,
                                             detail::OperationKind kind, std::size_t len) {
    // Stage 1: reserve. Arena full -> would_block; admission closed ->
    // invalid_state (ADR Decision 6/13).
    auto rh = arena_.reserve();
    if (!rh.has_value()) return make_unexpected<void>(rh.error());
    detail::SlotHandle h = rh.value();

    // Stage 2: prepare (op kind + fd/buffer borrow metadata).
    auto ph = arena_.prepare(h, kind, borrow_of(op));
    if (!ph.has_value()) {
        (void)arena_.rollback_reserved_or_prepared(h);
        return make_unexpected<void>(ph.error());
    }

    // Record the fixed prepared op into per-slot scratch (Scheme B). The worker
    // reads this only after mark_running(h) succeeds (current-generation running).
    prepared_ops_[h.slot.value] =
        PreparedBlockingOp{kind, op.fd,
                           static_cast<const std::byte*>(borrow_of(op).address),
                           op.len, op.offset};

    // Stage 2.5: install the slot's Completion publication binding.
    auto bh = arena_.install_publication_binding(h, &c, len, &publish_size_ready);
    if (!bh.has_value()) {
        (void)arena_.rollback_reserved_or_prepared(h);
        return make_unexpected<void>(bh.error());
    }

    // Stage 3a: Completion CAS idle -> binding elects ONE submitter.
    if (!begin_binding(c)) {
        (void)arena_.rollback_reserved_or_prepared(h);
        return make_unexpected<void>(IoError{IoError::Code::invalid_state});
    }
    // Stage 3b: commit (pending + pin + accepted++ + borrow begins).
    auto ch = arena_.commit(h);
    if (!ch.has_value()) {
        rollback_binding_before_accept(c);
        (void)arena_.rollback_reserved_or_prepared(h);
        return make_unexpected<void>(IoError{IoError::Code::invalid_state});
    }
    // Stage 3c: install the slot-release capability, then publish outstanding.
    // AFTER commit_binding nothing may throw (I9).
    install_binding(c, &arena_, h);
    commit_binding(c);

    // Stage 4: enqueue + dispatch publication under one work_mtx_ critical
    // section (P0). No gap between pin clear and ring visibility.
    enqueue_after_commit(h);
    return {};
}

template <class Op>
Result<void> ThreadPoolBackend::submit_void(Op op, Completion<void>& c,
                                            detail::OperationKind kind) {
    auto rh = arena_.reserve();
    if (!rh.has_value()) return make_unexpected<void>(rh.error());
    detail::SlotHandle h = rh.value();

    auto ph = arena_.prepare(h, kind, detail::BorrowMetadata{op.fd, nullptr, 0});
    if (!ph.has_value()) {
        (void)arena_.rollback_reserved_or_prepared(h);
        return make_unexpected<void>(ph.error());
    }

    prepared_ops_[h.slot.value] =
        PreparedBlockingOp{kind, op.fd, nullptr, std::size_t{0}, std::uint64_t{0}};

    auto bh = arena_.install_publication_binding(h, &c, 0, &publish_void_ready);
    if (!bh.has_value()) {
        (void)arena_.rollback_reserved_or_prepared(h);
        return make_unexpected<void>(bh.error());
    }

    if (!begin_binding(c)) {
        (void)arena_.rollback_reserved_or_prepared(h);
        return make_unexpected<void>(IoError{IoError::Code::invalid_state});
    }
    auto ch = arena_.commit(h);
    if (!ch.has_value()) {
        rollback_binding_before_accept(c);
        (void)arena_.rollback_reserved_or_prepared(h);
        return make_unexpected<void>(IoError{IoError::Code::invalid_state});
    }
    install_binding(c, &arena_, h);
    commit_binding(c);

    enqueue_after_commit(h);
    return {};
}

// ---------------------------------------------------------------------------
// Unified enqueue + dispatch publication (P0)
// ---------------------------------------------------------------------------

void ThreadPoolBackend::enqueue_after_commit(detail::SlotHandle h) noexcept {
    detail::EnqueueOutcome outcome;
    {
        std::lock_guard<std::mutex> lk(work_mtx_);
        outcome = arena_.enqueue(h);  // pending -> enqueued OR terminal_noop
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
        if (outcome == detail::EnqueueOutcome::enqueued) {
            // Post-fix placement: the gate fires INSIDE work_mtx_, so the
            // structural lock-domain probe sees work_domain_held == true.
            wait_after_enqueue_before_push_pause_(/*inside_work_mtx=*/true);
        }
#endif
        if (outcome == detail::EnqueueOutcome::enqueued) {
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
    if (outcome == detail::EnqueueOutcome::enqueued) {
        work_cv_.notify_one();
    } else {
        // terminal_noop: a pending cancel/terminal won first (Scheme B). That
        // winner owns readiness; re-arm the ready condition so the wake is not
        // lost (ADR Decision 4 / I19; design §4.5).
        signal_ready_progress();
    }
}

// ---------------------------------------------------------------------------
// Publication thunks + terminal conversion
// ---------------------------------------------------------------------------

void ThreadPoolBackend::publish_size_ready(void* completion,
                                            const detail::TerminalResult& t) noexcept {
    AsyncBackend::publish(*static_cast<Completion<std::size_t>*>(completion), terminal_to_size(t));
}

void ThreadPoolBackend::publish_void_ready(void* completion,
                                           const detail::TerminalResult& t) noexcept {
    AsyncBackend::publish(*static_cast<Completion<void>*>(completion), terminal_to_void(t));
}

Result<std::size_t> ThreadPoolBackend::terminal_to_size(const detail::TerminalResult& t) noexcept {
    if (t.stored && t.is_error) return make_unexpected<std::size_t>(t.error);
    return Result<std::size_t>{static_cast<std::size_t>(t.bytes)};
}

Result<void> ThreadPoolBackend::terminal_to_void(const detail::TerminalResult& t) noexcept {
    if (t.stored && t.is_error) return make_unexpected<void>(t.error);
    return {};
}

// ---------------------------------------------------------------------------
// Worker loop — dequeue + mark_running as one transfer; run the syscall
// ---------------------------------------------------------------------------

void ThreadPoolBackend::worker_loop() {
    // The outermost guard: an exception must never escape a worker thread
    // (AGENTS.md §9, ADR Decision 16). A syscall failure is a terminal result,
    // not an exception. If the unexpected happens, terminate deterministically
    // rather than corrupt shared state.
    try {
        for (;;) {
            detail::SlotHandle h{};
            PreparedBlockingOp op{};
            bool have_op = false;
            {
                std::unique_lock<std::mutex> lk(work_mtx_);
                work_cv_.wait(lk, [&] { return stopping_ || !dispatch_.empty(); });
                if (stopping_ && dispatch_.empty()) return;
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
                if (before_dequeue_gate_.load(std::memory_order_acquire) != nullptr) {
                    // Gate B: release work_mtx_ while paused so the test can
                    // safely call cancel/dispatch_size_for_test. The request
                    // stays on the ring; pop_front happens only after resume.
                    lk.unlock();
                    wait_before_dequeue_pause_();
                    lk.lock();
                    if (stopping_ && dispatch_.empty()) return;
                }
#endif
                if (!dispatch_.pop_front(h)) continue;  // spurious / raced
                // Dequeue + mark_running form ONE coordinated ownership transfer
                // under work_mtx_: there is no external window where the request
                // is popped but not running (design §4.2; ADR §10.4). A stale
                // handle fails fast; a backend_ready slot backs off (cancel won).
                bool owns = arena_.mark_running(h);
                if (!owns) continue;  // terminal winner already won; nothing to run
                op = prepared_ops_[h.slot.value];
                ++active_workers_;
                have_op = true;
            }  // work_mtx_ released BEFORE the blocking syscall (lock order §5)

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
            if (have_op) wait_running_pause_();  // Gate C: slot is `running`
#endif
            if (have_op) {
                detail::TerminalResult terminal = run_syscall(op);
                // Bookkeeping BEFORE terminal publication (P2): an observer must
                // not see a ready Completion with stale worker accounting.
                syscall_count_.fetch_add(1, std::memory_order_relaxed);
                {
                    std::lock_guard<std::mutex> wl(work_mtx_);
                    --active_workers_;
                }
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
                // Gate D post-fix: bookkeeping is done, terminal not yet stored.
                wait_terminal_publication_pause_();
#endif
                // record_terminal takes the arena leaf lock alone (no work_mtx_
                // held); the first caller wins, losers no-op (ADR Decision 12).
                (void)arena_.record_terminal(h, terminal);
                // Wake the ready domain so a blocked wait_one() observes the new
                // backend-ready (AC-6; design §4.5).
                signal_ready_progress();
            }
        }
    } catch (...) {
        // Invariant-safe: a worker must not swallow an unexpected exception and
        // continue corrupting state. Fail fast so the process dies deterministically.
        std::fprintf(stderr,
                     "sluice::async::ThreadPoolBackend: worker exception escaped "
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
        if (n < 0) return detail::TerminalResult::err(sluice::from_errno_value(errno));
        return detail::TerminalResult::ok_bytes(static_cast<std::uint64_t>(n));
    }
    case detail::OperationKind::write: {
        ssize_t n = sluice::detail::retry_on_eintr([&] {
            return ::pwrite(p.fd, p.buffer, p.length,
                            static_cast<off_t>(static_cast<std::int64_t>(p.offset)));
        });
        if (n < 0) return detail::TerminalResult::err(sluice::from_errno_value(errno));
        return detail::TerminalResult::ok_bytes(static_cast<std::uint64_t>(n));
    }
    case detail::OperationKind::sync_data: {
        int rc = sluice::detail::retry_on_eintr([&] { return ::fdatasync(p.fd); });
        if (rc < 0) return detail::TerminalResult::err(sluice::from_errno_value(errno));
        return detail::TerminalResult::ok_void();
    }
    case detail::OperationKind::sync_all: {
        int rc = sluice::detail::retry_on_eintr([&] { return ::fsync(p.fd); });
        if (rc < 0) return detail::TerminalResult::err(sluice::from_errno_value(errno));
        return detail::TerminalResult::ok_void();
    }
    }
    return detail::TerminalResult::err(IoError{IoError::Code::backend_error});
}

// ---------------------------------------------------------------------------
// poll / wait_one — reap is the SOLE Completion-ready publication authority
// ---------------------------------------------------------------------------

std::size_t ThreadPoolBackend::poll() {
    // Reap publishes Completion-ready through the slot binding inside the leaf
    // domain (ADR Decision 9 / I11). The worker only recorded backend-ready.
    return arena_.reap(sink_);
}

Result<std::size_t> ThreadPoolBackend::wait_one() {
    // Split-phase ready-epoch protocol (design §4.5; AC-6; issue #67): snapshot
    // the epochs, reap, and if nothing was reaped, park in the OBSERVE-ONLY
    // ready wait (no backend lock held across the park — the context-level
    // caller keeps access_mtx_ only across this reap). Returns only the reaped
    // count; 0 means a control-plane interruption (close_admission /
    // interrupt_all) with no completion reaped. The caller decides when to
    // stop waiting by tracking outstanding() or using close_admission().
    for (;;) {
        BackendWaitToken token = ready_wait_.snapshot();
        std::size_t n = arena_.reap(sink_);
        if (n > 0) return n;
        if (ready_wait_.wait_for_change(token) == BackendWakeReason::interrupted) {
            // One final non-blocking reap closes the interrupt-vs-final-ready
            // race, then report the interruption (0 = no completion reaped).
            n = arena_.reap(sink_);
            if (n > 0) return n;
            return std::size_t{0};
        }
        // progress: re-loop and reap the newly backend_ready request(s).
    }
}

void ThreadPoolBackend::signal_ready_progress() noexcept {
    // Publish the progress epoch under the ready mutex, then notify ALL
    // observers (the split wait allows concurrent parkers; notify_one could
    // strand a second waiter on a stale token).
    ready_wait_.signal_progress();
}

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
// Deterministic pause helpers. Each is a no-op when the corresponding gate is
// disarmed. The gate itself is a simple paused/resume atomic handshake; the
// test is responsible for setting resume only after observing paused.

void ThreadPoolBackend::wait_after_enqueue_before_push_pause_(
    bool inside_work_mtx) noexcept {
    auto* g = after_enqueue_before_push_gate_.load(std::memory_order_acquire);
    if (g == nullptr) return;
    g->exited.store(false, std::memory_order_release);
    g->work_domain_held.store(inside_work_mtx, std::memory_order_release);
    g->dispatch_push_completed.store(false, std::memory_order_release);
    g->paused.store(true, std::memory_order_release);
    while (!g->resume.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    g->exited.store(true, std::memory_order_release);
}

void ThreadPoolBackend::wait_before_dequeue_pause_() noexcept {
    auto* g = before_dequeue_gate_.load(std::memory_order_acquire);
    if (g == nullptr) return;
    g->exited.store(false, std::memory_order_release);
    g->paused.store(true, std::memory_order_release);
    while (!g->resume.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    g->exited.store(true, std::memory_order_release);
}

void ThreadPoolBackend::wait_running_pause_() noexcept {
    auto* g = running_gate_.load(std::memory_order_acquire);
    if (g == nullptr) return;
    g->exited.store(false, std::memory_order_release);
    g->paused.store(true, std::memory_order_release);
    while (!g->resume.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    g->exited.store(true, std::memory_order_release);
}

void ThreadPoolBackend::wait_terminal_publication_pause_() noexcept {
    auto* g = terminal_publication_gate_.load(std::memory_order_acquire);
    if (g == nullptr) return;
    g->exited.store(false, std::memory_order_release);
    g->paused.store(true, std::memory_order_release);
    while (!g->resume.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    g->exited.store(true, std::memory_order_release);
}
#endif

// ---------------------------------------------------------------------------
// cancel — Completion-keyed, drives the shared state machine
// ---------------------------------------------------------------------------

void ThreadPoolBackend::cancel(Completion<std::size_t>& c) {
    auto h = arena_.resolve_completion(&c);
    if (!h.has_value()) return;
    detail::SlotHandle handle = *h;
    detail::CancelDisposition disp;
    {
        std::lock_guard<std::mutex> lk(work_mtx_);
        // remove_exact + cancel under one work_mtx_ so a request cannot be both
        // off the ring and being dispatched (design §4.3; ADR §10.3).
        (void)dispatch_.remove_exact(handle);
        disp = arena_.cancel(handle);
    }
    if (disp == detail::CancelDisposition::terminal_won) {
        tally_canceled();
        signal_ready_progress();
    }
    // intent_recorded: no tally, no terminal; the syscall's real result wins
    // verbatim. already_terminal / not_found: no-op.
}

void ThreadPoolBackend::cancel(Completion<void>& c) {
    auto h = arena_.resolve_completion(&c);
    if (!h.has_value()) return;
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

std::size_t ThreadPoolBackend::outstanding() const noexcept {
    return arena_.accepted_outstanding();
}

void ThreadPoolBackend::close_admission() {
    // Close admission, THEN wake any participant parked in the ready wait so
    // it re-evaluates (issue #67: the frozen design's "close does not signal"
    // constraint starved a parked wait_one and deadlocked drain). The wake is
    // a one-shot control generation advance — a re-evaluation signal, not a
    // fabricated completion and not a persistent "never park again" state:
    // future waits snapshot the advanced generation and park normally, so an
    // admission-closed runtime with outstanding work never busy-spins.
    arena_.close_admission();
    ready_wait_.interrupt_all();
}

}  // namespace sluice::async
