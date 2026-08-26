// sluice::async::ThreadPoolBackend — bounded persistent blocking-I/O worker pool.
//
// Bounded persistent blocking-I/O backend driving real POSIX syscalls through
// the RequestArena / RequestSlot lifecycle. See threadpool_backend.hpp and
// docs/history/implementation-plans/phase-e-bounded-threadpool-backend.md for
// the frozen design record.
// This TU is the worked example in docs/architecture/async-request-lifecycle.md
// (submission transaction §4.1 steps 2-3, worker steps 4-5).
//
// Replaces the legacy "one std::thread per op + std::function +
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
#include <system_error>
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
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
            // C2d: injected worker-spawn failure (rows 9-10; finding P1-04
            // "no test injects thread-creation failure"). The injected
            // std::system_error mirrors a real pthread_create EAGAIN; the
            // catch path below stops and joins the already-started workers and
            // rethrows. Compiled out of production builds.
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

// Dispatch the malformed-descriptor probe by op kind (see the declaration).
// Defined next to the validate_* helpers it forwards to.
template <class Op>
Result<void> ThreadPoolBackend::validate_op(const Op& op) noexcept {
    if constexpr (std::is_same_v<Op, ReadOp>) {
        return validate_read(op);
    } else if constexpr (std::is_same_v<Op, WriteOp>) {
        return validate_write(op);
    } else if constexpr (std::is_same_v<Op, SyncDataOp>) {
        return validate_sync(op);
    } else {
        return validate_sync(op);  // SyncAllOp
    }
}

template <class Op>
Result<void> ThreadPoolBackend::submit_size(Op op, Completion<std::size_t>& c,
                                             detail::OperationKind kind) {
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
    // C2e (row 15; B1): deterministic close-wins window — the submit pauses
    // BEFORE taking the admission transaction lock, so close_admission()
    // completes with no contention and the resumed submit must observe
    // admission closed at reserve and reject synchronously (ADR Decision 15;
    // tp_c2e_close_wins_submit_started_before_close_rejected). Compiled out
    // of production builds (no branch, no local, no symbol).
    wait_before_admission_lock_pause_();
#endif
    // ADR §"Commit / accept" (:453-462): the winning submit retains its
    // context/admission lock through Step 5 — the `binding -> outstanding`
    // release-store, the commit/accept linearization point. The admission
    // transaction below serializes the shared Stage 1-3 ladder against
    // close_admission() (which takes the same lock): after close_admission()
    // returns no new acceptance LP can occur (Decision 15), and a submit that
    // enters the protocol before close either completes its LP before close
    // returns (submit wins) or observes admission closed at reserve and
    // rejects synchronously (close wins). The lock is released before
    // enqueue_after_commit: the LP is done and enqueue is no-fail.
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

    // Stage 4: enqueue + dispatch publication under one work_mtx_ critical
    // section. No gap between pin clear and ring visibility.
    enqueue_after_commit(h);
    return {};
}

template <class Op>
Result<void> ThreadPoolBackend::submit_void(Op op, Completion<void>& c,
                                             detail::OperationKind kind) {
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
    // C2e (row 15; B1): deterministic close-wins window — see submit_size.
    wait_before_admission_lock_pause_();
#endif
    detail::SlotHandle h{};
    {
        // Backend admission transaction domain (ADR :453-462) — see
        // submit_size.
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

// ---------------------------------------------------------------------------
// Unified enqueue + dispatch publication
// ---------------------------------------------------------------------------

void ThreadPoolBackend::enqueue_after_commit(detail::SlotHandle h) noexcept {
    detail::EnqueueOutcome outcome;
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
    bool injected_dispatch_failure = false;
    // C2d (ADR Gate 4): deterministic commit/enqueue pause. The request is
    // committed (Completion outstanding, slot `pending`, enqueue pin set) but
    // work_mtx_ is not yet held, so a test-issued pending cancellation wins
    // the canceled terminal here (Scheme B); the resumed enqueue then observes
    // backend_ready and acknowledges the pin as a terminal no-op with no
    // dispatch linkage (ADR Gate 4; I17/I19). Entirely compiled out of
    // production builds (no branch, no local, no symbol).
    wait_before_enqueue_lock_pause_();
#endif
    {
        std::lock_guard<std::mutex> lk(work_mtx_);
        outcome = arena_.enqueue(h);  // pending -> enqueued OR terminal_noop
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
        if (outcome == detail::EnqueueOutcome::enqueued) {
            // Post-fix placement: the gate fires INSIDE work_mtx_, so the
            // structural lock-domain probe sees work_domain_held == true.
            wait_after_enqueue_before_push_pause_(/*inside_work_mtx=*/true);
            // C2d: post-commit dispatch-failure injection (rows 9-10). The
            // enqueue already won (slot `enqueued`, pin acknowledged) but the
            // handle was NOT yet pushed: no worker can pop it (workers dequeue
            // only under work_mtx_, which we hold), so no worker, ring, kernel,
            // or other executor holds execution ownership. Record the defined
            // `backend_error` terminal through the arena's terminal-winner
            // authority instead of pushing — the ADR Decision-12
            // "post-commit dispatch failure after execution ownership is
            // proven absent" winner candidate (AGENTS.md §10.5). reap
            // publishes it exactly once; the borrow stays active until reap.
            // TEST-ONLY probe (AGENTS.md §15): production dispatch cannot fail
            // by construction (bounded ring, allocation-free push; a full push
            // is the §12 invariant fail-fast), so this branch proves the
            // SHARED arena's terminal-winner/reap machinery under a simulated
            // post-commit terminal event — not a production failure-handling
            // path. Entirely compiled out of production builds (no branch, no
            // local, no symbol).
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
            if (!injected_dispatch_failure)  // injection won: no dispatch linkage
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
        // The dispatch-failure terminal won (ADR Decision 12); no worker will
        // run or signal, so the READY domain must observe the new backend_ready
        // (a blocked wait_one must not lose the wake — AC-6 / design §4.5).
        signal_ready_progress();
    } else
#endif
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
                std::uint64_t dequeue_gate_generation = 0;
                if (before_dequeue_gate_.load(std::memory_order_acquire) != nullptr) {
                    // Gate B: release work_mtx_ while paused so the test can
                    // safely call cancel/dispatch_size_for_test. The request
                    // stays on the ring; pop_front happens only after resume.
                    lk.unlock();
                    // Issue #110: the visit's generation (0 = legacy
                    // single-visit bool protocol) is acked only AFTER this
                    // cycle's dequeue decision below.
                    dequeue_gate_generation = wait_before_dequeue_pause_();
                    // Issue #110 regression seam: optional single-visit hold
                    // in the exact post-resume / pre-pop window (the old
                    // protocol published `exited` here). No-op when disarmed.
                    wait_post_resume_pre_pop_hold_();
                    lk.lock();
                    if (stopping_ && dispatch_.empty()) {
                        ack_dequeue_gate_generation_(dequeue_gate_generation);
                        return;
                    }
                }
                // Issue #110 ACK linearization point: publish the ACK after
                // pop_front has decided this cycle's consume (entry popped, or
                // ring observed empty — e.g. cancel won and removed it). After
                // this decision the worker continuation can consume another
                // entry only by re-entering work_cv_ wait -> gate check ->
                // pop, so a test that waits for the ACK before arming
                // generation N+1 cannot have N+1's dispatch entry stolen
                // without its own gate observation.
                const bool popped = dispatch_.pop_front(h);
                ack_dequeue_gate_generation_(dequeue_gate_generation);
                if (!popped) continue;  // spurious / raced
#else
                if (!dispatch_.pop_front(h)) continue;  // spurious / raced
#endif
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
                // Bookkeeping BEFORE terminal publication: an observer must
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
    // domain (ADR Decision 9). The worker only recorded backend-ready.
    // Deliver identity events to the attached Scheduler-owned
    // routing sink when one is set; otherwise the no-op reference sink.
    return arena_.reap(routing_sink_ ? *routing_sink_ : sink_);
}

Result<std::size_t> ThreadPoolBackend::wait_one() {
    // Split-phase ready-epoch protocol (design §4.5; AC-6): snapshot
    // the epochs, reap, and if nothing was reaped, park in the OBSERVE-ONLY
    // ready wait (no backend lock held across the park — the context-level
    // caller keeps access_mtx_ only across this reap). Returns only the reaped
    // count; 0 means a control-plane interruption (close_admission /
    // interrupt_all) with no completion reaped. The caller decides when to
    // stop waiting by tracking outstanding() or using close_admission().
    for (;;) {
        BackendWaitToken token = ready_wait_.snapshot();
        std::size_t n = arena_.reap(routing_sink_ ? *routing_sink_ : sink_);
        if (n > 0) return n;
        if (ready_wait_.wait_for_change(token) == BackendWakeReason::interrupted) {
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
            // C2e (row 15): deterministic interrupt-vs-final-ready window. The
            // pause lets a test record the final terminal in the exact window
            // between the control wake and the final reap, proving the final
            // reap returns it — the control interrupt never swallows the last
            // ready (tp_c2e_interrupt_final_reap_closes_ready_race; mutant M4
            // detector). Compiled out of production builds (no branch, no
            // local, no symbol).
            wait_control_wake_final_reap_pause_();
#endif
            // One final non-blocking reap closes the interrupt-vs-final-ready
            // race, then report the interruption (0 = no completion reaped).
            n = arena_.reap(routing_sink_ ? *routing_sink_ : sink_);
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
// disarmed. The gate is a paused/resume/exited atomic handshake.
//
// Issue #92 (completing #86-B): the handshake is now fully bidirectional.
// paused and exited publish with notify_one(); resume BLOCKS on
// resume.wait(false, acquire) instead of yield-busy-spinning. Under parallel
// Debug oversubscription the old resume yield-spin could starve the very test
// thread that publishes resume (the worker burned its time slice re-checking
// resume while the resumer waited for CPU), producing the intermittent 30s
// case-level watchdog abort with no phase attribution. The test side now
// resumes ONLY through resume_threadpool_gate() (release-store + notify_all),
// so every resume publisher provably pairs its store with a notification. A
// missing notify would leave the production thread blocked in resume.wait and
// is caught by the case-level watchdog instead of hanging forever. This is
// SLUICE_ASYNC_INTERNAL_TESTING behavior only; production ThreadPool semantics
// are unchanged.

void ThreadPoolBackend::wait_after_enqueue_before_push_pause_(
    bool inside_work_mtx) noexcept {
    auto* g = after_enqueue_before_push_gate_.load(std::memory_order_acquire);
    if (g == nullptr) return;
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
    if (g == nullptr) return 0;
    const std::uint64_t generation = g->armed.load(std::memory_order_acquire);
    if (generation == 0) {
        // Legacy single-visit bool protocol (single-shot test cases; never
        // rearmed — see the struct comment in the header). Behavior is
        // byte-identical to the pre-#110 seam.
        g->exited.store(false, std::memory_order_release);
        g->paused.store(true, std::memory_order_release);
        g->paused.notify_one();
        g->resume.wait(false, std::memory_order_acquire);
        g->exited.store(true, std::memory_order_release);
        g->exited.notify_one();
        return 0;
    }
    // Issue #110 generation handshake: publish the pause for THIS generation
    // (monotonic max + notify — atomic::wait permits spurious wakes, so the
    // consumer re-checks in a predicate loop), then block until the test
    // resumes THIS generation or later (a resume of an older generation must
    // not release this visit; the pre-#110 boolean carried no such identity).
    dequeue_gate_detail::publish_max_(g->paused_at, generation);
    g->paused_at.notify_all();
    std::uint64_t seen = g->resumed_at.load(std::memory_order_acquire);
    while (seen < generation) {
        g->resumed_at.wait(seen, std::memory_order_acquire);
        seen = g->resumed_at.load(std::memory_order_acquire);
    }
    return generation;
}

void ThreadPoolBackend::ack_dequeue_gate_generation_(
    std::uint64_t generation) noexcept {
    if (generation == 0) return;  // legacy single-visit protocol: no ACK
    auto* g = before_dequeue_gate_.load(std::memory_order_acquire);
    if (g == nullptr) return;
    dequeue_gate_detail::publish_max_(g->acked_at, generation);
    g->acked_at.notify_all();
}

void ThreadPoolBackend::wait_post_resume_pre_pop_hold_() noexcept {
    auto* g = post_resume_pre_pop_hold_gate_.load(std::memory_order_acquire);
    if (g == nullptr) return;
    g->exited.store(false, std::memory_order_release);
    g->paused.store(true, std::memory_order_release);
    g->paused.notify_one();
    g->resume.wait(false, std::memory_order_acquire);
    g->exited.store(true, std::memory_order_release);
    g->exited.notify_one();
}

void ThreadPoolBackend::wait_before_enqueue_lock_pause_() noexcept {
    auto* g = before_enqueue_lock_gate_.load(std::memory_order_acquire);
    if (g == nullptr) return;
    g->exited.store(false, std::memory_order_release);
    g->paused.store(true, std::memory_order_release);
    g->paused.notify_one();
    g->resume.wait(false, std::memory_order_acquire);
    g->exited.store(true, std::memory_order_release);
    g->exited.notify_one();
}

// C2d (ADR Gate 4): pre-commit stage-failure injection. Returns the stage's
// natural synchronous rejection (and increments its `fired` counter) when the
// seam is armed; std::nullopt when disarmed. The submit paths consult this
// immediately before the stage's arena call and return the rejection through
// their OWN rollback code — the injected branch never duplicates the arena
// call, only the rollback of a stage that never executed.
std::optional<IoError> ThreadPoolBackend::injected_precommit_stage_failure_(
    SubmitStage stage) noexcept {
    auto* inj = submit_stage_failure_injection_.load(std::memory_order_acquire);
    if (inj == nullptr) return std::nullopt;
    switch (stage) {
    case SubmitStage::reserve:
        if (inj->fail_reserve.load(std::memory_order_acquire)) {
            inj->reserve_fired.fetch_add(1, std::memory_order_relaxed);
            // The capacity-full form (ADR Decision 6): the only natural
            // reserve rejection on a well-formed context.
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
    if (g == nullptr) return;
    g->exited.store(false, std::memory_order_release);
    g->paused.store(true, std::memory_order_release);
    g->paused.notify_one();
    g->resume.wait(false, std::memory_order_acquire);
    g->exited.store(true, std::memory_order_release);
    g->exited.notify_one();
}

void ThreadPoolBackend::wait_terminal_publication_pause_() noexcept {
    auto* g = terminal_publication_gate_.load(std::memory_order_acquire);
    if (g == nullptr) return;
    g->exited.store(false, std::memory_order_release);
    g->paused.store(true, std::memory_order_release);
    g->paused.notify_one();
    g->resume.wait(false, std::memory_order_acquire);
    g->exited.store(true, std::memory_order_release);
    g->exited.notify_one();
}

// C2e: pause between the interrupted control wake and wait_one's final reap
// (see the public gate struct's comment). No-op when disarmed.
void ThreadPoolBackend::wait_control_wake_final_reap_pause_() noexcept {
    auto* g = control_wake_final_reap_gate_.load(std::memory_order_acquire);
    if (g == nullptr) return;
    g->exited.store(false, std::memory_order_release);
    g->paused.store(true, std::memory_order_release);
    g->paused.notify_one();
    g->resume.wait(false, std::memory_order_acquire);
    g->exited.store(true, std::memory_order_release);
    g->exited.notify_one();
}

// C2e (B1): pause BEFORE taking the admission transaction lock (see the
// public gate struct's comment). No-op when disarmed.
void ThreadPoolBackend::wait_before_admission_lock_pause_() noexcept {
    auto* g = before_admission_lock_gate_.load(std::memory_order_acquire);
    if (g == nullptr) return;
    g->exited.store(false, std::memory_order_release);
    g->paused.store(true, std::memory_order_release);
    g->paused.notify_one();
    g->resume.wait(false, std::memory_order_acquire);
    g->exited.store(true, std::memory_order_release);
    g->exited.notify_one();
}

// C2e (B1): pause between arena_.commit() and the `binding -> outstanding`
// release-store, INSIDE the admission transaction (see the public gate
// struct's comment). No-op when disarmed.
void ThreadPoolBackend::wait_before_commit_binding_pause_() noexcept {
    auto* g = before_commit_binding_gate_.load(std::memory_order_acquire);
    if (g == nullptr) return;
    g->exited.store(false, std::memory_order_release);
    g->paused.store(true, std::memory_order_release);
    g->paused.notify_one();
    g->resume.wait(false, std::memory_order_acquire);
    g->exited.store(true, std::memory_order_release);
    g->exited.notify_one();
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

// ---------------------------------------------------------------------------
// Production waiter registration / cancellation (ADR Decision 10)
// ---------------------------------------------------------------------------
// The waiter registration is ORTHOGONAL to the execution state, so no
// work_mtx_/dispatch interaction is needed: the arena leaf serializes
// registration against reap extraction and cancel_waiter against reap, and
// the I/O path never reads waiter state. The candidate lease is consumed at
// the by-value boundary on any failure (never transferred to the slot).

Result<void> ThreadPoolBackend::register_waiter(Completion<std::size_t>& c,
                                                detail::WaiterToken token,
                                                detail::RoutingLease lease) {
    auto h = arena_.resolve_completion(&c);
    if (!h.has_value()) {
        return make_unexpected<void>(IoError{IoError::Code::not_found});
    }
    return arena_.register_waiter(*h, token, std::move(lease));
}

Result<void> ThreadPoolBackend::register_waiter(Completion<void>& c,
                                                detail::WaiterToken token,
                                                detail::RoutingLease lease) {
    auto h = arena_.resolve_completion(&c);
    if (!h.has_value()) {
        return make_unexpected<void>(IoError{IoError::Code::not_found});
    }
    return arena_.register_waiter(*h, token, std::move(lease));
}

Result<detail::RoutingLease> ThreadPoolBackend::cancel_waiter(
    Completion<std::size_t>& c) {
    auto h = arena_.resolve_completion(&c);
    if (!h.has_value()) {
        return make_unexpected<detail::RoutingLease>(
            IoError{IoError::Code::not_found});
    }
    return arena_.cancel_waiter(*h);
}

Result<detail::RoutingLease> ThreadPoolBackend::cancel_waiter(
    Completion<void>& c) {
    auto h = arena_.resolve_completion(&c);
    if (!h.has_value()) {
        return make_unexpected<detail::RoutingLease>(
            IoError{IoError::Code::not_found});
    }
    return arena_.cancel_waiter(*h);
}

std::size_t ThreadPoolBackend::outstanding() const noexcept {
    return arena_.accepted_outstanding();
}

// --- AC-1a production resource observations (read-only; no new authority) ---

// Dispatch occupancy/high-water read under the work domain's own lock —
// BoundedDispatchQueue lives entirely under work_mtx_ (push/pop/remove are
// called only with it held). No arena lock is taken here, and no caller of
// these accessors holds work_mtx_, so no new lock-order edge is introduced.
// Lock order observed: work_mtx_ -> (nothing); the snapshot below never holds
// two locks at once.
std::size_t ThreadPoolBackend::dispatch_occupancy() const {
    std::lock_guard<std::mutex> lk(work_mtx_);
    return dispatch_.size();
}

std::size_t ThreadPoolBackend::dispatch_high_water_mark() const {
    std::lock_guard<std::mutex> lk(work_mtx_);
    return dispatch_.high_water();
}

// active_workers_ is written only under work_mtx_ (worker_loop ++ on successful
// mark_running, -- after the syscall before terminal publication). Same single-
// domain discipline as dispatch_occupancy.
std::size_t ThreadPoolBackend::active_workers() const {
    std::lock_guard<std::mutex> lk(work_mtx_);
    return active_workers_;
}

// Component-wise coherent snapshot: SEPARATE critical sections per resource
// domain (see the header comment). The declaration is noexcept even though the
// member accessors take locks: every critical section is a non-blocking leaf
// acquisition whose body cannot throw, matching quiescence_snapshot()'s
// precedent. Fields are read in a fixed order (arena -> dispatch/workers) to
// keep the documented staleness pattern deterministic for tests.
ThreadPoolBackend::ResourceSnapshot ThreadPoolBackend::resource_snapshot() const noexcept {
    ResourceSnapshot s{};
    // arena leaf mutex_ ...
    const auto q = arena_.quiescence_snapshot();
    // ... then the work domain. Never two locks at once: quiescence_snapshot()
    // has already released the arena mutex before work_mtx_ is taken.
    std::lock_guard<std::mutex> lk(work_mtx_);
    s.arena_capacity = arena_.capacity();
    s.arena_slot_in_use = q.slot_in_use;
    s.arena_high_water_mark = arena_.high_water_mark();
    s.arena_capacity_rejections = arena_.capacity_rejections();
    s.accepted_outstanding = q.accepted_outstanding;
    s.dispatch_capacity = dispatch_.capacity();
    s.dispatch_occupancy = dispatch_.size();
    s.dispatch_high_water_mark = dispatch_.high_water();
    s.configured_workers = workers_.size();
    s.active_workers = active_workers_;
    return s;
}

void ThreadPoolBackend::close_admission() {
    // ADR §"Commit / accept" (:453-462) + Decision 15: close_admission()
    // takes the backend admission transaction lock so it serializes against an
    // in-flight submit's Stage 1-5 acceptance protocol (the `binding ->
    // outstanding` release-store — Step 5 — is the commit/accept linearization
    // point). After this returns, no new acceptance LP can occur: an in-flight
    // submit either completed its LP first (submit wins) or a later submit
    // observes admission closed at reserve and rejects synchronously (close
    // wins). THEN wake any participant parked in the ready wait so it
    // re-evaluates (the frozen design's "close does not signal"
    // constraint starved a parked wait_one and deadlocked drain). The wake is
    // a one-shot control generation advance — a re-evaluation signal, not a
    // fabricated completion and not a persistent "never park again" state:
    // future waits snapshot the advanced generation and park normally, so an
    // admission-closed runtime with outstanding work never busy-spins.
    {
        std::lock_guard<std::mutex> lk(admission_mtx_);
        arena_.close_admission();
    }
    ready_wait_.interrupt_all();
}

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
namespace {
// C2d: worker-spawn failure injection state (see the guarded class setters in
// threadpool_backend.hpp). SIZE_MAX = disarmed. A static seam is required
// because the injection point is the constructor, which runs before any
// instance exists; the tests guarantee serial isolation (only the constructing
// thread reads it while armed; the harness runs cases sequentially in one
// process) and restore SIZE_MAX via RAII. Compiled out of production builds.
std::atomic<std::size_t> g_injected_worker_spawn_failure_index{
    std::numeric_limits<std::size_t>::max()};
}  // namespace

std::size_t ThreadPoolBackend::injected_worker_spawn_failure_index() noexcept {
    return g_injected_worker_spawn_failure_index.load(std::memory_order_acquire);
}

void ThreadPoolBackend::set_injected_worker_spawn_failure_index(
    std::size_t index) noexcept {
    g_injected_worker_spawn_failure_index.store(index, std::memory_order_release);
}
#endif

}  // namespace sluice::async
