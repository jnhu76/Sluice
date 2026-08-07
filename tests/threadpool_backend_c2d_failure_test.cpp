// Phase C2d — ThreadPoolBackend failure injection / accepted-terminal under
// allocator failure (Issue #68 rows 9-10).
//
// Scope of this target:
//   1. transactional pre-commit rejection on the REAL backend (binding-CAS
//      loss: submit into a non-idle Completion -> invalid_state, zero residue,
//      capacity immediately recyclable);
//   2. partial worker-startup failure (finding P1-04: no test injected a
//      thread-creation failure; the constructor must stop and join the
//      already-started workers and rethrow synchronously);
//   3. post-commit permanent dispatch failure (ADR Decision 12 "post-commit
//      dispatch failure after execution ownership is proven absent"):
//      injected between enqueue and dispatch push, INSIDE work_mtx_, with no
//      worker ever able to see the handle -> submit returns success, the
//      request reaches exactly one defined backend_error terminal, reap
//      publishes exactly once, the borrow stays valid until reap, no worker
//      or syscall executes, the ring-full invariant path is untouched;
//   4. post-commit no-allocation (ADR Decision 14 / I9): the accepted
//      submit -> enqueue/terminal -> poll/reap -> reset path on the REAL
//      ThreadPool performs ZERO heap allocations under an always-throw
//      operator new, for both the ordinary worker path and the injected
//      dispatch-failure path;
//   5. terminal winner: the injected dispatch-failure terminal vs cancel —
//      exactly one winner, no overwrite, no double publication, no worker
//      execution in any outcome, and at most one tally (canceled_ops == 1 iff
//      cancel won — the injected backend_error terminal contributes no tally
//      because AsyncStats::completion_errors is not wired for ThreadPool,
//      gate §10 residual gap); plus the ADR Gate-4 deterministic commit/enqueue
//      pause in which PENDING cancellation wins while the injection stays armed.
//
// Links sluice_async_internal_testing (the injection seams are guarded by
// SLUICE_ASYNC_INTERNAL_TESTING; production sluice_async carries no seam, no
// branch, and no layout change). All waits are bounded deadlines; a lost
// ready-wake is detected as a bounded timeout (with close_admission used to
// unblock the parked waiter) — never a hang.
#include "harness.hpp"

#include <sluice/async/async_io_context.hpp>
#include <sluice/async/completion.hpp>
#include <sluice/async/threadpool_backend.hpp>
#include <sluice/error.hpp>
#include <sluice/measurement.hpp>
#include <sluice/result.hpp>

#include <atomic>
#include <barrier>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <limits>
#include <new>
#include <string>
#include <system_error>
#include <thread>
#include <unistd.h>

using namespace sluice::async;
using sluice::IoError;
using sluice::Result;

namespace {

constexpr auto kWaitTimeout = std::chrono::seconds(10);

class TempPath {
public:
    explicit TempPath(const char* tag) {
        path_ = (std::filesystem::temp_directory_path() /
                 ("sluice_tp_c2d_" + std::string(tag) + "_" +
                  std::to_string(::getpid()) + "_" +
                  std::to_string(counter_++) + ".tmp"))
                    .string();
    }
    ~TempPath() {
        std::error_code ec;
        std::filesystem::remove(path_, ec);
    }
    TempPath(const TempPath&) = delete;
    TempPath& operator=(const TempPath&) = delete;
    const std::string& path() const { return path_; }
private:
    std::string path_;
    static inline long counter_ = 0;
};

int open_temp(const std::string& path) {
    int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        std::fprintf(stderr, "open_temp failed\n");
        std::exit(1);
    }
    return fd;
}

// Drain outstanding ops through the real reaper with a bounded total time.
// Uses only poll() and yield() — never a blocking wait_one(), which has no
// timeout and could hang the test (and ultimately the parent waitpid) forever
// if a terminal or ready-wake were lost.
bool drain_bounded(ThreadPoolBackend& backend,
                   std::chrono::steady_clock::time_point deadline) {
    while (backend.outstanding() > 0) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        if (backend.poll() == 0) {
            std::this_thread::yield();
        }
    }
    return true;
}

// Wait for a pause gate's paused flag with a bounded deadline.
template <class Gate>
bool wait_paused(Gate& gate, std::chrono::steady_clock::time_point deadline) {
    while (!gate.paused.load(std::memory_order_acquire)) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::yield();
    }
    return true;
}

// RAII: resume a paused gate on scope exit and wait for the production path to
// leave it. The gate object must outlive the backend (declared before it in
// the test), so no disarm is needed — lexical scope guarantees the gate is
// destroyed after the backend and its workers. The gate can also be resumed
// explicitly (guard.resume()) after the test disarmed the gate pointer.
template <class Gate>
class ScopedGateResume {
public:
    explicit ScopedGateResume(Gate& gate) : gate_(&gate) {}
    void resume() {
        if (resumed_) return;
        gate_->resume.store(true, std::memory_order_release);
        resumed_ = true;
    }
    ~ScopedGateResume() { cleanup(); }
private:
    void cleanup() {
        resume();
        const auto deadline = std::chrono::steady_clock::now() + kWaitTimeout;
        while (!gate_->exited.load(std::memory_order_acquire)) {
            if (std::chrono::steady_clock::now() >= deadline) {
                std::fprintf(stderr,
                             "C2d test gate failed to exit before timeout\n");
                std::abort();
            }
            std::this_thread::yield();
        }
    }
    Gate* gate_;
    bool resumed_ = false;
};

// RAII: arm the static worker-spawn failure seam; ALWAYS restore the
// disarmed sentinel even on failure. The seam is a controlled static (the
// injection point is the constructor, which runs before any instance exists);
// serial isolation holds because the harness runs cases sequentially in one
// process and only the constructing thread reads the seam while armed.
class ScopedSpawnFailureSeam {
public:
    explicit ScopedSpawnFailureSeam(std::size_t fail_at_index) {
        ThreadPoolBackend::set_injected_worker_spawn_failure_index(fail_at_index);
    }
    ~ScopedSpawnFailureSeam() {
        ThreadPoolBackend::set_injected_worker_spawn_failure_index(
            std::numeric_limits<std::size_t>::max());
    }
    ScopedSpawnFailureSeam(const ScopedSpawnFailureSeam&) = delete;
    ScopedSpawnFailureSeam& operator=(const ScopedSpawnFailureSeam&) = delete;
};

}  // namespace

// --- Counting + fault-injection allocation probe -----------------------------
// Same pattern as reference_backend_no_alloc_test.cpp: a malloc-based
// operator new replacement (composes with ASan/TSan interposition) whose
// always-throw flag turns EVERY allocation into std::bad_alloc. Compiled out
// under TSan (the TSan runtime defines the replacements as strong symbols); in
// TSan runs the counter/always-throw assertions are skipped and the lifecycle
// assertions remain — the allocation-freedom proof comes from the
// Debug/Release/ASan runs.
namespace {
// Probe state is declared unconditionally (the TSan-compiled-out branch below
// still names it from `if constexpr (kAllocProbeActive)` discarded branches —
// the sibling reference_backend_no_alloc_test.cpp pattern).
[[maybe_unused]] std::atomic<std::size_t> g_allocations{0};
[[maybe_unused]] std::atomic<bool> g_throw_all{false};
}  // namespace

#if !defined(__has_feature) || !__has_feature(thread_sanitizer)
constexpr bool kAllocProbeActive = true;

void* operator new(std::size_t n) {
    if (g_throw_all.load(std::memory_order_acquire)) {
        throw std::bad_alloc{};
    }
    g_allocations.fetch_add(1, std::memory_order_relaxed);
    if (void* p = std::malloc(n)) return p;
    throw std::bad_alloc{};
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
// Aligned overloads preserve the requested alignment (CodeRabbit finding in
// the sibling probe): forward to std::aligned_alloc with a size rounded up to
// a multiple of the alignment.
void* operator new(std::size_t n, std::align_val_t a) {
    if (g_throw_all.load(std::memory_order_acquire)) {
        throw std::bad_alloc{};
    }
    g_allocations.fetch_add(1, std::memory_order_relaxed);
    const std::size_t align = static_cast<std::size_t>(a);
    const std::size_t size = ((n + align - 1) / align) * align;
    if (void* p = std::aligned_alloc(align, size)) return p;
    throw std::bad_alloc{};
}
void* operator new[](std::size_t n, std::align_val_t a) { return ::operator new(n, a); }
void operator delete(void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete(void* p, std::align_val_t, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::align_val_t, std::size_t) noexcept { std::free(p); }
#else
constexpr bool kAllocProbeActive = false;
#endif

SLUICE_MAIN()

// ---- C2d row 9: pre-commit rejection on the REAL backend is transactional --
// Submit into a NON-IDLE Completion loses the binding CAS after reserve/
// prepare/binding-install: the backend must roll back its candidate slot and
// return invalid_state with ZERO residue — the rejected Completion untouched,
// slot_in_use/outstanding/dispatch unchanged, no ready residue, and the
// capacity immediately reusable by a fresh submit.
SLUICE_TEST_CASE(tp_c2d_cas_loss_rejection_zero_side_effects) {
    // Capacity 3: two accepted ops fill two slots, leaving a FREE slot for the
    // third submit — so the rejection happens at the Completion binding CAS
    // (non-idle Completion), NOT at reserve (would_block).
    //
    // DETERMINISM: the worker runs concurrently, so ring-occupancy and
    // syscall-count observations after submit are racy. The worker is paused
    // at Gate B (before its first dequeue) so every residue assertion sees the
    // exact post-submit state; the gate is then disarmed (the worker's
    // resume-acquire makes the disarm visible) and the drain proceeds.
    ThreadPoolBackend::BeforeWorkerDequeuePauseGate gate;
    ThreadPoolBackend backend(ThreadPoolConfig{/*capacity=*/3, /*workers=*/1});
    backend.set_before_dequeue_pause_gate(&gate);
    ScopedGateResume guard(gate);  // RAII resume even on failure
    TempPath tp("cas");
    int fd = open_temp(tp.path());
    const std::byte seed[2] = {std::byte{0x11}, std::byte{0x22}};
    SLUICE_CHECK(::pwrite(fd, seed, 2, 0) == 2);

    Completion<std::size_t> c1, c2;
    std::byte b1[1]{}, b2[1]{}, b3[1]{};
    SLUICE_CHECK(backend.submit_read(ReadOp{fd, b1, 1, 0}, c1).has_value());
    SLUICE_CHECK(backend.submit_read(ReadOp{fd, b2, 1, 0}, c2).has_value());
    SLUICE_CHECK(backend.outstanding() == 2);

    // The worker wakes for c1 and pauses BEFORE its first dequeue: both ops
    // are still on the dispatch ring, nothing has executed.
    const auto deadline = std::chrono::steady_clock::now() + kWaitTimeout;
    SLUICE_CHECK_MSG(wait_paused(gate, deadline),
                     "worker must pause before dequeue");

    // Submit into c1 AGAIN while it is outstanding: reserve succeeds (free
    // slot), prepare + binding install succeed, the binding CAS loses.
    auto rej = backend.submit_read(ReadOp{fd, b3, 1, 0}, c1);
    SLUICE_CHECK(!rej.has_value());
    SLUICE_CHECK(rej.error().code == IoError::Code::invalid_state);

    // Zero side effects (deterministic — the worker is still paused): the
    // rejected submit touched nothing.
    SLUICE_CHECK(c1.outstanding());          // original op untouched
    SLUICE_CHECK(c2.outstanding());
    SLUICE_CHECK(backend.outstanding() == 2);
    SLUICE_CHECK(backend.arena_slot_in_use() == 2);  // candidate rolled back
    SLUICE_CHECK(backend.dispatch_size_for_test() == 2);  // only the real ops
    SLUICE_CHECK(backend.syscall_count_for_test() == 0);  // nothing executed
    SLUICE_CHECK(backend.backend_ready_count_for_test() == 0);  // no ghost

    // Disarm the gate while the worker is paused, then resume: the worker's
    // resume-acquire makes the disarm visible, so it dequeues and runs both
    // ops without pausing again.
    backend.set_before_dequeue_pause_gate(nullptr);
    guard.resume();

    // Both accepted ops reach exactly one terminal with the real result.
    SLUICE_CHECK(drain_bounded(backend, deadline));
    SLUICE_CHECK(c1.ready() && c1.result().has_value() && c1.result().value() == 1);
    SLUICE_CHECK(c2.ready() && c2.result().has_value() && c2.result().value() == 1);

    // The capacity is immediately recyclable: a fresh submit succeeds.
    c1.reset();
    c2.reset();
    Completion<std::size_t> c3;
    SLUICE_CHECK(backend.submit_read(ReadOp{fd, b3, 1, 0}, c3).has_value());
    SLUICE_CHECK(drain_bounded(backend, deadline));
    SLUICE_CHECK(c3.ready());
    c3.reset();
    SLUICE_CHECK(backend.outstanding() == 0);
    SLUICE_CHECK(backend.arena_slot_in_use() == 0);
    ::close(fd);
}

// ---- C2d row 9: partial worker-startup failure (finding P1-04) -------------
// The constructor launches the fixed worker pool; an injected spawn failure
// must propagate synchronously and the already-started workers must exit and
// be joined. Surviving the failed construction (no std::terminate from an
// unjoined thread vector) IS the join proof: if the catch path left a worker
// joinable, unwinding the constructor's members would terminate the process.
// The seam is restored by RAII, and a normal construction afterwards succeeds
// with the full worker count.
SLUICE_TEST_CASE(tp_c2d_partial_worker_startup_failure) {
    // (a) fail BEFORE the first worker: zero workers started; the injected
    // std::system_error (mirroring pthread_create EAGAIN) propagates.
    {
        ScopedSpawnFailureSeam seam(/*fail_at_index=*/0);
        bool threw_expected = false;
        try {
            ThreadPoolBackend backend(ThreadPoolConfig{/*capacity=*/2, /*workers=*/3});
            (void)backend;
        } catch (const std::system_error& e) {
            threw_expected =
                e.code() == std::errc::resource_unavailable_try_again;
        }
        SLUICE_CHECK_MSG(threw_expected,
                         "constructor must propagate the injected spawn failure");
    }
    // (b) fail AFTER worker 1 started (partial startup): the started worker
    // must stop and join; the injected exception propagates unchanged.
    {
        ScopedSpawnFailureSeam seam(/*fail_at_index=*/1);
        bool threw_expected = false;
        try {
            ThreadPoolBackend backend(ThreadPoolConfig{/*capacity=*/2, /*workers=*/4});
            (void)backend;
        } catch (const std::system_error& e) {
            threw_expected =
                e.code() == std::errc::resource_unavailable_try_again;
        }
        SLUICE_CHECK_MSG(threw_expected,
                         "partial worker startup must throw the injected failure");
    }
    // (c) the RAII seam restored the disarmed sentinel: a normal construction
    // succeeds with the full worker count and destroys quiescently.
    ThreadPoolBackend backend(ThreadPoolConfig{/*capacity=*/2, /*workers=*/2});
    SLUICE_CHECK(backend.configured_worker_count() == 2);
}

// ---- C2d row 9: post-commit permanent dispatch failure (size op) -----------
// Injected between arena_.enqueue(h) and dispatch_.push_back(h), INSIDE
// work_mtx_, with the handle never visible to any worker (workers dequeue only
// under work_mtx_): submit returns success; the request reaches exactly ONE
// defined backend_error terminal; reap publishes exactly once; the borrow
// stays active until reap; no worker or syscall ever runs; the ready-domain
// wake reaches a waiter parked BEFORE the submit (a lost wake would hang and
// trip the bounded timeout, not pass).
SLUICE_TEST_CASE(tp_c2d_dispatch_failure_injection_size_op) {
    ThreadPoolBackend::DispatchFailureInjection injection;
    ThreadPoolBackend backend(ThreadPoolConfig{/*capacity=*/2, /*workers=*/1});
    backend.set_dispatch_failure_injection(&injection);
    sluice::AsyncStats stats;
    backend.attach_stats(&stats);
    TempPath tp("ds");
    int fd = open_temp(tp.path());
    const std::byte seed[1] = {std::byte{0x22}};
    SLUICE_CHECK(::pwrite(fd, seed, 1, 0) == 1);

    Completion<std::size_t> c;
    std::byte buf[1]{};
    const std::uint64_t syscalls_before = backend.syscall_count_for_test();
    const auto deadline = std::chrono::steady_clock::now() + kWaitTimeout;

    // A waiter parks BEFORE the submit: the dispatch-failure terminal is
    // recorded synchronously inside submit, so a wait that starts after submit
    // would never exercise the ready-domain wake. The issue #67 wait-phase
    // seam pins "parked in the ready cv wait"; the injected path's
    // signal_ready_progress must wake it.
    std::atomic<bool> parked{false};
    backend.set_wait_phase_flag_for_test(&parked);
    std::atomic<bool> wait_done{false};
    std::size_t reaped = 0;
    std::thread waiter([&] {
        auto wr = backend.wait_one();
        if (wr.has_value()) reaped = wr.value();
        wait_done.store(true, std::memory_order_release);
    });

    while (!parked.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    if (!parked.load(std::memory_order_acquire)) {
        backend.close_admission();  // interrupt the (possibly parked) wait
        waiter.join();
        SLUICE_FAIL("waiter did not park in the ready wait");
    }

    injection.armed.store(true);
    auto r = backend.submit_read(ReadOp{fd, buf, 1, 0}, c);
    injection.armed.store(false);

    // The waiter must wake and reap exactly once (bounded; a lost wake is a
    // defect — see the C2d gate mutation evidence).
    while (!wait_done.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    if (!wait_done.load(std::memory_order_acquire)) {
        backend.close_admission();
        waiter.join();
        // Reap the stored-but-unwoken terminal and reset the ready Completion
        // so the backend destructor stays quiescent: the case's OWN message
        // below must be the observable failure, not the destructor's
        // non-quiescent fail-fast abort (which would fire before the harness
        // prints the recorded failure).
        (void)backend.poll();
        if (c.ready()) c.reset();
        SLUICE_FAIL("wait_one lost the dispatch-failure ready wake");
    }
    waiter.join();
    // Disarm the wait-phase flag: the canonical hygiene for the issue #67
    // seam (the backend must not retain a pointer to this stack flag after
    // the waiter has left the ready wait).
    backend.set_wait_phase_flag_for_test(nullptr);

    // submit succeeded despite the permanent dispatch failure.
    SLUICE_CHECK(r.has_value());
    SLUICE_CHECK(injection.fired == 1);
    SLUICE_CHECK(reaped == 1);
    SLUICE_CHECK(c.ready());
    // Collect the terminal facts and reset the ready Completion BEFORE the
    // assertions so any violation leaves the backend quiescent — the case's
    // own message is the observable failure, not the destructor's
    // non-quiescent fail-fast abort.
    bool value_is_error = c.ready() && !c.result().has_value();
    bool error_is_backend =
        value_is_error && c.result().error().code == IoError::Code::backend_error;
    c.reset();
    SLUICE_CHECK_MSG(value_is_error,
                     "the dispatch-failure terminal must be a backend_error result");
    SLUICE_CHECK_MSG(error_is_backend,
                     "the dispatch-failure terminal must report IoError::backend_error");
    // The request never entered a worker or a syscall.
    SLUICE_CHECK(backend.syscall_count_for_test() == syscalls_before);
    SLUICE_CHECK(backend.dispatch_size_for_test() == 0);
    // Exactly one terminal; no cancel tally; stats accurate.
    SLUICE_CHECK(stats.canceled_ops == 0);
    SLUICE_CHECK(backend.outstanding() == 0);
    SLUICE_CHECK(backend.arena_slot_in_use() == 0);
    ::close(fd);
}

// ---- C2d row 9: post-commit permanent dispatch failure (void op) -----------
// The void (sync) ops go through the separate submit_void template path but
// share the same enqueue/dispatch injection seam; prove the defined
// backend_error terminal and exactly-once publication there too. This case
// ALSO pins the borrow-before-reap window deterministically (no concurrent
// waiter): the injected terminal does NOT end the borrow — only reap does
// (I7), and reap publishes exactly once.
SLUICE_TEST_CASE(tp_c2d_dispatch_failure_injection_void_op) {
    ThreadPoolBackend::DispatchFailureInjection injection;
    ThreadPoolBackend backend(ThreadPoolConfig{/*capacity=*/2, /*workers=*/1});
    backend.set_dispatch_failure_injection(&injection);
    sluice::AsyncStats stats;
    backend.attach_stats(&stats);
    TempPath tp("dv");
    int fd = open_temp(tp.path());

    Completion<void> c;
    const std::uint64_t syscalls_before = backend.syscall_count_for_test();

    injection.armed.store(true);
    auto r = backend.submit_sync_data(SyncDataOp{fd}, c);
    injection.armed.store(false);

    SLUICE_CHECK(r.has_value());
    SLUICE_CHECK(injection.fired == 1);
    SLUICE_CHECK(backend.outstanding() == 1);
    SLUICE_CHECK(backend.dispatch_size_for_test() == 0);
    SLUICE_CHECK(backend.syscall_count_for_test() == syscalls_before);

    // The fd borrow began at commit and stays ACTIVE while the injected
    // terminal is stored but not yet reaped — a dispatch failure does not end
    // the borrow; reap does (ADR Decision 8 / I7).
    auto h = backend.handle_for_completion_for_test(&c);
    SLUICE_CHECK(h.has_value());
    auto b = backend.borrow_for_test(*h);
    SLUICE_CHECK(b.has_value() && b->active && b->fd == fd);

    const auto deadline = std::chrono::steady_clock::now() + kWaitTimeout;
    while (!c.ready() && std::chrono::steady_clock::now() < deadline) {
        (void)backend.poll();
        std::this_thread::yield();
    }
    SLUICE_CHECK(c.ready());
    SLUICE_CHECK(!c.result().has_value());
    SLUICE_CHECK(c.result().error().code == IoError::Code::backend_error);
    // Borrow ended at completion-ready; a second reap publishes nothing.
    auto b2 = backend.borrow_for_test(*h);
    SLUICE_CHECK(b2.has_value() && !b2->active);
    SLUICE_CHECK(backend.poll() == 0);
    SLUICE_CHECK(backend.outstanding() == 0);
    SLUICE_CHECK(stats.canceled_ops == 0);
    c.reset();
    SLUICE_CHECK(backend.arena_slot_in_use() == 0);
    ::close(fd);
}

// ---- C2d row 10: real ThreadPool post-commit no-allocation -----------------
// The accepted submit -> worker terminal -> reap -> reset path performs ZERO
// heap allocations under an always-throw operator new (ADR Decision 14 / I9),
// and the accepted request reaches exactly one terminal — the real syscall
// result wins verbatim. Backend, fd, buffer, Completion, and stats are all
// constructed BEFORE the window is armed.
SLUICE_TEST_CASE(tp_c2d_real_worker_post_commit_no_allocation) {
    ThreadPoolBackend backend(ThreadPoolConfig{/*capacity=*/4, /*workers=*/1});
    sluice::AsyncStats stats;
    backend.attach_stats(&stats);
    TempPath tp("na");
    int fd = open_temp(tp.path());
    const std::byte seed[1] = {std::byte{0x33}};
    SLUICE_CHECK(::pwrite(fd, seed, 1, 0) == 1);

    Completion<std::size_t> c;
    std::byte buf[1]{};

    if constexpr (kAllocProbeActive) {
        g_allocations.store(0, std::memory_order_relaxed);
        g_throw_all.store(true, std::memory_order_relaxed);
    }
    bool submit_ok = backend.submit_read(ReadOp{fd, buf, 1, 0}, c).has_value();
    const auto deadline = std::chrono::steady_clock::now() + kWaitTimeout;
    while (!c.ready() && std::chrono::steady_clock::now() < deadline) {
        (void)backend.poll();
        std::this_thread::yield();
    }
    bool ready = c.ready();
    bool value_ok = ready && c.result().has_value();
    c.reset();
    std::size_t outstanding = backend.outstanding();
    std::size_t slot_in_use = backend.arena_slot_in_use();
    if constexpr (kAllocProbeActive) {
        g_throw_all.store(false, std::memory_order_relaxed);
    }

    SLUICE_CHECK_MSG(submit_ok,
                     "submit must succeed under always-throw operator new");
    SLUICE_CHECK_MSG(ready,
                     "the accepted request must reach its terminal under always-throw operator new");
    SLUICE_CHECK_MSG(value_ok,
                     "the real syscall result must win verbatim");
    if constexpr (kAllocProbeActive) {
        std::size_t allocs = g_allocations.load(std::memory_order_relaxed);
        SLUICE_CHECK_MSG(allocs == 0,
                         "the accepted submit/enqueue/terminal/reap/reset path must allocate nothing");
    }
    SLUICE_CHECK_MSG(outstanding == 0, "arena drained");
    SLUICE_CHECK_MSG(slot_in_use == 0, "slot released");
    ::close(fd);
}

// ---- C2d row 10: post-commit no-allocation on the injected failure path ----
// The SAME zero-allocation window with the dispatch-failure injection armed:
// the defined backend_error terminal path (record_terminal + ready wake +
// reap + reset) must also be allocation-free and reach exactly one terminal.
SLUICE_TEST_CASE(tp_c2d_dispatch_failure_post_commit_no_allocation) {
    ThreadPoolBackend::DispatchFailureInjection injection;
    ThreadPoolBackend backend(ThreadPoolConfig{/*capacity=*/4, /*workers=*/1});
    backend.set_dispatch_failure_injection(&injection);
    sluice::AsyncStats stats;
    backend.attach_stats(&stats);
    TempPath tp("nf");
    int fd = open_temp(tp.path());

    Completion<std::size_t> c;
    std::byte buf[1]{};
    const std::uint64_t syscalls_before = backend.syscall_count_for_test();

    injection.armed.store(true);
    if constexpr (kAllocProbeActive) {
        g_allocations.store(0, std::memory_order_relaxed);
        g_throw_all.store(true, std::memory_order_relaxed);
    }
    bool submit_ok = backend.submit_read(ReadOp{fd, buf, 1, 0}, c).has_value();
    const auto deadline = std::chrono::steady_clock::now() + kWaitTimeout;
    while (!c.ready() && std::chrono::steady_clock::now() < deadline) {
        (void)backend.poll();
        std::this_thread::yield();
    }
    bool ready = c.ready();
    bool error_ok =
        ready && !c.result().has_value() &&
        c.result().error().code == IoError::Code::backend_error;
    c.reset();
    std::size_t outstanding = backend.outstanding();
    std::size_t slot_in_use = backend.arena_slot_in_use();
    if constexpr (kAllocProbeActive) {
        g_throw_all.store(false, std::memory_order_relaxed);
    }
    injection.armed.store(false);

    SLUICE_CHECK_MSG(submit_ok,
                     "submit must succeed under always-throw operator new");
    SLUICE_CHECK_MSG(ready,
                     "the accepted request must reach its terminal under always-throw operator new");
    SLUICE_CHECK_MSG(error_ok,
                     "the dispatch-failure terminal must be the defined backend_error");
    SLUICE_CHECK_MSG(injection.fired == 1, "injection must fire exactly once");
    SLUICE_CHECK_MSG(backend.syscall_count_for_test() == syscalls_before,
                     "no worker/syscall execution on the injected path");
    if constexpr (kAllocProbeActive) {
        std::size_t allocs = g_allocations.load(std::memory_order_relaxed);
        SLUICE_CHECK_MSG(allocs == 0,
                         "the injected terminal path must allocate nothing");
    }
    SLUICE_CHECK_MSG(outstanding == 0, "arena drained");
    SLUICE_CHECK_MSG(slot_in_use == 0, "slot released");
    ::close(fd);
}

// ---- C2d row 9: deterministic cancel-wins ordering (ADR Gate 4) -------------
// The ADR Gate-4 obligation "a deterministic commit/enqueue pause in which
// pending cancellation wins": the submit thread pauses after commit
// (Completion outstanding, slot `pending`, enqueue pin set) and before taking
// work_mtx_; the test's cancel wins the canceled terminal from `pending`
// (Scheme B); an intervening reap publishes nothing and the Completion stays
// non-ready (I17/I19 — the pin is still live); the resumed enqueue observes
// backend_ready, acknowledges the pin as a terminal no-op, and links NOTHING —
// no worker runs, submit still succeeds. The injection stays ARMED throughout:
// the seam is gated on the enqueued outcome, so it cannot fire on a
// terminal_noop (fired == 0), and the request reaches exactly one canceled
// terminal with one canceled_ops tally.
SLUICE_TEST_CASE(tp_c2d_cancel_wins_before_enqueue_injection_armed) {
    ThreadPoolBackend::DispatchFailureInjection injection;
    ThreadPoolBackend::BeforeEnqueueLockPauseGate gate;
    ThreadPoolBackend backend(ThreadPoolConfig{/*capacity=*/2, /*workers=*/1});
    backend.set_dispatch_failure_injection(&injection);
    backend.set_before_enqueue_lock_pause_gate(&gate);
    sluice::AsyncStats stats;
    backend.attach_stats(&stats);
    TempPath tp("cw");
    int fd = open_temp(tp.path());
    const std::byte seed[1] = {std::byte{0x55}};
    SLUICE_CHECK(::pwrite(fd, seed, 1, 0) == 1);

    Completion<std::size_t> c;
    std::byte buf[1]{};
    const std::uint64_t syscalls_before = backend.syscall_count_for_test();

    injection.armed.store(true);
    std::atomic<bool> submit_ok{false};
    std::thread submitter([&] {
        submit_ok.store(backend.submit_read(ReadOp{fd, buf, 1, 0}, c).has_value(),
                        std::memory_order_release);
    });
    ScopedGateResume guard(gate);  // RAII resume even on failure
    const auto deadline = std::chrono::steady_clock::now() + kWaitTimeout;
    SLUICE_CHECK_MSG(wait_paused(gate, deadline),
                     "submit must pause after commit, before the enqueue lock");

    // Deterministic pending-cancel win while the injection is still armed.
    backend.cancel(c);
    SLUICE_CHECK(stats.canceled_ops == 1);
    // Intervening reap publishes nothing: backend_ready with a live enqueue
    // pin is reap-ineligible (I17/I19) — the Completion stays non-ready and
    // the slot stays in use (ADR Gate 4).
    SLUICE_CHECK(backend.poll() == 0);
    SLUICE_CHECK(!c.ready());
    SLUICE_CHECK(backend.arena_slot_in_use() == 1);

    guard.resume();
    submitter.join();
    SLUICE_CHECK_MSG(submit_ok.load(std::memory_order_acquire),
                     "submit must succeed — the cancel is a terminal event, not a rejection");

    // Exactly one terminal: the canceled winner, published by reap; nothing
    // was pushed, so no worker or syscall executed. Collect the terminal facts
    // and reset the ready Completion BEFORE the assertions (see the size-op
    // case for the quiescent-diagnostic rationale).
    while (!c.ready() && std::chrono::steady_clock::now() < deadline) {
        (void)backend.poll();
        std::this_thread::yield();
    }
    bool ready = c.ready();
    bool canceled =
        ready && !c.result().has_value() &&
        c.result().error().code == IoError::Code::canceled;
    bool one_publication = backend.poll() == 0;
    c.reset();
    SLUICE_CHECK_MSG(ready, "the canceled terminal must be published");
    SLUICE_CHECK_MSG(canceled, "the sole terminal must be IoError::canceled");
    SLUICE_CHECK_MSG(one_publication, "exactly one publication");
    SLUICE_CHECK(backend.syscall_count_for_test() == syscalls_before);
    SLUICE_CHECK(backend.dispatch_size_for_test() == 0);
    SLUICE_CHECK(backend.outstanding() == 0);
    SLUICE_CHECK(backend.arena_slot_in_use() == 0);
    // Last (post-quiescence): the seam must not fire on a terminal_noop
    // enqueue — the assertion that catches the M10 mutation.
    SLUICE_CHECK_MSG(injection.fired == 0,
                     "the injection must not fire on a terminal_noop enqueue");
    ::close(fd);
}

// ---- C2d row 9: dispatch-failure terminal vs cancel — exactly one winner ---
// Cancel and the injected dispatch failure serialize on work_mtx_; the winner
// is whoever reaches the work domain first. BOTH outcomes are legal (cancel
// wins -> canceled terminal; injection wins -> backend_error terminal). The
// DETERMINISTIC orderings are covered separately: injection-wins by
// `tp_c2d_cancel_after_dispatch_failure_terminal_no_overwrite` (and the
// size/void injection cases), cancel-wins by
// `tp_c2d_cancel_wins_before_enqueue_injection_armed` (ADR Gate 4). This case
// covers the genuinely racy interleaving and asserts the INVARIANT that holds
// in every interleaving: exactly one terminal, exactly one publication, no
// worker execution, and at most one tally (canceled_ops == 1 iff cancel won —
// the injected backend_error terminal contributes no tally; completion_errors
// is unwired for ThreadPool, gate §10 residual gap).
SLUICE_TEST_CASE(tp_c2d_dispatch_failure_races_cancel_exactly_one) {
    ThreadPoolBackend::DispatchFailureInjection injection;
    ThreadPoolBackend backend(ThreadPoolConfig{/*capacity=*/4, /*workers=*/1});
    backend.set_dispatch_failure_injection(&injection);
    sluice::AsyncStats stats;
    backend.attach_stats(&stats);
    TempPath tp("rc");
    int fd = open_temp(tp.path());
    const std::byte seed[1] = {std::byte{0x44}};
    SLUICE_CHECK(::pwrite(fd, seed, 1, 0) == 1);

    Completion<std::size_t> c;
    std::byte buf[1]{};
    const std::uint64_t syscalls_before = backend.syscall_count_for_test();

    injection.armed.store(true);
    std::barrier start(2);
    std::atomic<bool> submit_done{false};
    std::thread submitter([&] {
        start.arrive_and_wait();
        auto r = backend.submit_read(ReadOp{fd, buf, 1, 0}, c);
        submit_done.store(r.has_value(), std::memory_order_release);
    });

    start.arrive_and_wait();
    const auto deadline = std::chrono::steady_clock::now() + kWaitTimeout;
    // Cancel races the enqueue/dispatch decision. Canceling a not-yet-bound
    // Completion is a not_found no-op; canceling before the injection is a
    // legal enqueued/pending terminal winner; canceling after it is an
    // already_terminal no-op. Exactly one terminal in every interleaving.
    while (!submit_done.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline) {
        backend.cancel(c);
        std::this_thread::yield();
    }
    submitter.join();
    injection.armed.store(false);

    SLUICE_CHECK_MSG(submit_done.load(std::memory_order_acquire),
                     "submit must succeed in the race");
    SLUICE_CHECK(injection.fired <= 1);
    SLUICE_CHECK(backend.outstanding() == 1);

    while (!c.ready() && std::chrono::steady_clock::now() < deadline) {
        (void)backend.poll();
        std::this_thread::yield();
    }
    SLUICE_CHECK_MSG(c.ready(), "exactly one terminal must be published");
    SLUICE_CHECK(!c.result().has_value());
    const IoError::Code code = c.result().error().code;
    SLUICE_CHECK_MSG(code == IoError::Code::canceled ||
                         code == IoError::Code::backend_error,
                     "the sole terminal must be canceled (cancel won) or backend_error (dispatch failure won)");
    // At most one tally: canceled_ops == 1 iff cancel won. The injected
    // backend_error terminal contributes NO tally — AsyncStats::completion_errors
    // is not wired for ThreadPoolBackend (gate §10 residual gap), so an
    // injection-wins interleaving records zero tallies.
    if (code == IoError::Code::canceled) {
        SLUICE_CHECK(stats.canceled_ops == 1);
    } else {
        SLUICE_CHECK(stats.canceled_ops == 0);
    }
    // No worker or syscall execution in ANY outcome.
    SLUICE_CHECK(backend.syscall_count_for_test() == syscalls_before);
    SLUICE_CHECK(backend.dispatch_size_for_test() == 0);
    c.reset();
    SLUICE_CHECK(backend.outstanding() == 0);
    SLUICE_CHECK(backend.arena_slot_in_use() == 0);
    ::close(fd);
}

// ---- C2d row 9: cancel after the dispatch-failure terminal is a no-op ------
// The injected terminal fires synchronously inside submit; a later cancel
// observes already_terminal: no overwrite, no second tally, no double
// ready-ring push — reap publishes exactly once.
SLUICE_TEST_CASE(tp_c2d_cancel_after_dispatch_failure_terminal_no_overwrite) {
    ThreadPoolBackend::DispatchFailureInjection injection;
    ThreadPoolBackend backend(ThreadPoolConfig{/*capacity=*/2, /*workers=*/1});
    backend.set_dispatch_failure_injection(&injection);
    sluice::AsyncStats stats;
    backend.attach_stats(&stats);
    TempPath tp("co");
    int fd = open_temp(tp.path());

    Completion<std::size_t> c;
    std::byte buf[1]{};

    injection.armed.store(true);
    SLUICE_CHECK(backend.submit_read(ReadOp{fd, buf, 1, 0}, c).has_value());
    injection.armed.store(false);
    SLUICE_CHECK(injection.fired == 1);

    backend.cancel(c);  // already_terminal: no overwrite, no tally
    backend.cancel(c);  // still a no-op — the terminal persists

    const auto deadline = std::chrono::steady_clock::now() + kWaitTimeout;
    while (!c.ready() && std::chrono::steady_clock::now() < deadline) {
        (void)backend.poll();
        std::this_thread::yield();
    }
    SLUICE_CHECK(c.ready());
    SLUICE_CHECK(!c.result().has_value());
    SLUICE_CHECK(c.result().error().code == IoError::Code::backend_error);
    SLUICE_CHECK(stats.canceled_ops == 0);  // cancel never tallied
    c.reset();
    SLUICE_CHECK(backend.outstanding() == 0);
    SLUICE_CHECK(backend.arena_slot_in_use() == 0);
    ::close(fd);
}
