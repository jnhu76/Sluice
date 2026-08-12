// Phase E ThreadPoolBackend Scheme-B race regressions.
//
// Four deterministic, timing-independent cases that drive the real
// ThreadPoolBackend through the SLUICE_ASYNC_INTERNAL_TESTING pause gates.
// Each case arms a gate, submits a real syscall, waits for the exact pause
// point, manipulates or observes the state, resumes the gate, and then drains.
// All waits are bounded; on timeout or assertion failure the test resumes every
// armed gate and joins every created thread before reporting failure.
//
// Pre-fix / post-fix behavior (honest labels):
//   A  structural lock-domain proof: the Gate-A pause fires INSIDE work_mtx_.
//      The test asserts work_domain_held==true while paused. Pre-fix code
//      pauses outside work_mtx_, so this case FAILS pre-fix and passes after
//      the enqueue/dispatch atomicity fix.
//   B  conformance: enqueued cancel wins before dequeue; the syscall does not
//      run. Likely passes pre-fix; proves the legal cancel/dequeue protocol.
//   C  conformance: running cancel records intent only; the real syscall result
//      wins verbatim. Likely passes pre-fix; proves Decision 11 semantics.
//   D  terminal publication order: while paused, bookkeeping is already done
//      (active_workers==0, syscall_count==1) but poll()==0. Pre-fix code pauses
//      BEFORE bookkeeping and AFTER record_terminal, so this case FAILS pre-fix
//      and passes after the bookkeeping reorder.
//
// Links sluice_async_internal_testing (the seams are guarded by
// SLUICE_ASYNC_INTERNAL_TESTING; production sluice_async has no seams).
#include "harness.hpp"

#include <sluice/async/completion.hpp>
#include <sluice/async/threadpool_backend.hpp>
#include <sluice/error.hpp>
#include <sluice/measurement.hpp>
#include <sluice/result.hpp>

#include <atomic>
#include <barrier>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <mutex>
#include <string>
#include <sys/types.h>
#include <sys/wait.h>
#include <thread>
#include <type_traits>
#include <unistd.h>

using namespace sluice::async;
using sluice::IoError;
using sluice::Result;

namespace {

// Drain deadline for drain_bounded (the production reap path). This is NOT a
// gate-scheduling deadline: during the drain the gate is already resumed, so
// the worker is free to run and the syscall completes promptly. 5s is generous
// (the expected completion is sub-millisecond). The case-level Watchdog below
// is the sole deadlock guard for the gate handshakes.
constexpr auto kDrainDeadline = std::chrono::seconds(5);

// Issue #92: case phase attribution. Each case owns a PhaseProbe (stack-local)
// and advances `phase` across its transitions; on watchdog expiry the watchdog
// thread prints case + phase + the bound gate's lock-free atomics. The watchdog
// thread reads ONLY atomics (and the immutable name pointer) — it MUST NOT call
// any backend diagnostic that takes work_mtx_/arena, because a watchdog cannot
// block behind the defect it is diagnosing. The gate atomics are bound once the
// gate is armed; a case with no gate leaves them null and only case/phase print.
enum class CasePhase : std::uint8_t {
    setup,       submit,    wait_paused, inspect,
    resume,      wait_exited, barrier,   join_canceler,
    drain,       reset,     teardown,    done,
};

inline const char* phase_name(CasePhase p) noexcept {
    switch (p) {
    case CasePhase::setup:        return "setup";
    case CasePhase::submit:       return "submit";
    case CasePhase::wait_paused:  return "wait_paused";
    case CasePhase::inspect:      return "inspect";
    case CasePhase::resume:       return "resume";
    case CasePhase::wait_exited:  return "wait_exited";
    case CasePhase::barrier:      return "barrier";
    case CasePhase::join_canceler:return "join_canceler";
    case CasePhase::drain:        return "drain";
    case CasePhase::reset:        return "reset";
    case CasePhase::teardown:     return "teardown";
    case CasePhase::done:         return "done";
    }
    return "?";
}

struct PhaseProbe {
    const char* name = nullptr;
    std::atomic<CasePhase> phase{CasePhase::setup};
    const std::atomic<bool>* gate_paused = nullptr;
    const std::atomic<bool>* gate_resume = nullptr;
    const std::atomic<bool>* gate_exited = nullptr;

    void set(CasePhase p) noexcept { phase.store(p, std::memory_order_release); }
    void bind_gate(const std::atomic<bool>& paused,
                   const std::atomic<bool>& resume,
                   const std::atomic<bool>& exited) noexcept {
        gate_paused = &paused;
        gate_resume = &resume;
        gate_exited = &exited;
    }
};

// Issue #86-B / #92 case-level last-resort watchdog. The gate handshakes are
// fully bidirectional blocking atomic::wait (zero-CPU), so under correct
// protocol every case completes in seconds — even the 64-iteration race loop
// takes well under 15s under TSan. The deadline is NOT a correctness deadline
// and NOT a root-cause classifier: a wall-clock bound cannot distinguish a
// genuine protocol deadlock from complete host-scheduler starvation. It is a
// last-resort boundedness guard that converts an unbounded hang into a bounded
// abort WITH phase attribution so the next failure points at the stalled phase
// instead of reporting only "30s elapsed". It ABORTs (not FAILs) because a stuck
// protocol is a catastrophic defect, not a Scheme-B correctness assertion.
class Watchdog {
public:
    explicit Watchdog(std::chrono::seconds timeout, const PhaseProbe& probe)
        : probe_(&probe) {
        deadline_ = std::chrono::steady_clock::now() + timeout;
        try {
            thread_ = std::thread([this] {
                std::unique_lock<std::mutex> lk(mtx_);
                (void)cv_.wait_until(lk, deadline_, [this] {
                    return done_.load(std::memory_order_acquire);
                });
                if (!done_.load(std::memory_order_acquire)) {
                    diagnose_and_abort();
                }
            });
        } catch (...) {
            // Fail-closed: the watchdog is the only bound on the blocking
            // atomic::wait handshakes, so a thread-creation failure must fail
            // the test. Continuing without a guard would let a genuinely
            // stalled protocol hang the test in unbounded atomic::wait instead
            // of failing loudly.
            std::fprintf(stderr,
                         "ThreadPool test watchdog: thread creation failed; "
                         "aborting (fail-closed: no unbounded atomic::wait)\n");
            std::abort();
        }
    }
    ~Watchdog() {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            done_.store(true, std::memory_order_release);
        }
        cv_.notify_one();
        if (thread_.joinable()) thread_.join();
    }
    Watchdog(const Watchdog&) = delete;
    Watchdog& operator=(const Watchdog&) = delete;
private:
    // Reads ONLY atomics and the immutable name pointer; never touches
    // work_mtx_/arena, so the watchdog cannot deadlock behind the defect.
    void diagnose_and_abort() noexcept {
        const CasePhase ph = probe_->phase.load(std::memory_order_acquire);
        std::fprintf(stderr,
                     "ThreadPool test watchdog: case exceeded the last-resort "
                     "boundedness deadline; aborting for diagnostics\n");
        std::fprintf(stderr, "  case=%s\n  phase=%s\n",
                     probe_->name ? probe_->name : "?", phase_name(ph));
        if (probe_->gate_resume != nullptr) {
            std::fprintf(stderr, "  gate: paused=%d resume=%d exited=%d\n",
                         probe_->gate_paused->load(std::memory_order_acquire),
                         probe_->gate_resume->load(std::memory_order_acquire),
                         probe_->gate_exited->load(std::memory_order_acquire));
        }
        std::abort();
    }
    std::mutex mtx_;
    std::condition_variable cv_;
    std::chrono::steady_clock::time_point deadline_;
    std::atomic<bool> done_{false};
    std::thread thread_;
    const PhaseProbe* probe_;
};

// Watchdog timeout: generous enough that it does not fire under correct
// protocol (even under TSan + full-suite oversubscription), short enough that
// a CI deadlock is caught promptly. A last-resort boundedness guard, not a
// correctness deadline.
constexpr auto kWatchdog = std::chrono::seconds(30);

class TempPath {
public:
    TempPath(const char* tag) {
        path_ = (std::filesystem::temp_directory_path() /
                 ("sluice_tp_scheme_b_" + std::string(tag) + "_" +
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
    if (fd < 0) { std::fprintf(stderr, "open_temp failed\n"); std::exit(1); }
    return fd;
}

// Deterministic, zero-CPU gate handshake (issue #86-B / #92): block on
// atomic::wait until the production path reaches the exact pause point and calls
// paused.notify_one(). The probe is advanced across the blocking transition so a
// watchdog expiry attributes the stall to wait_paused (worker never reached the
// pause) vs inspect (past the pause). The case-level Watchdog is the last-resort
// boundedness guard; correctness is the deterministic atomic::wait handshake.
template <class Gate>
void wait_paused(Gate& gate, PhaseProbe& probe) {
    probe.set(CasePhase::wait_paused);
    gate.paused.wait(false, std::memory_order_acquire);
    probe.set(CasePhase::inspect);
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

// RAII: resume a paused gate and join a thread on scope exit, then wait for the
// production path to leave the gate.  The gate object must outlive the backend
// (declared before it in the test), so no disarm is needed — lexical scope
// guarantees the gate is destroyed after the backend and its workers.
// Guarantees the test never leaves a gate armed or a thread joinable when an
// assertion fails.
template <class Gate>
class ScopedGateAndThread {
public:
    ScopedGateAndThread(Gate& gate, std::thread& t)
        : gate_(&gate), thread_(&t) {}
    void join() {
        if (joined_) return;
        resume_threadpool_gate(*gate_);
        thread_->join();
        joined_ = true;
    }
    ~ScopedGateAndThread() { cleanup(); }
private:
    void cleanup() {
        join();
        wait_for_exit();
    }
    void wait_for_exit() noexcept {
        // Issue #86-B: block (zero-CPU) on atomic::wait instead of
        // yield-spinning. exited.notify_one is called by the production path.
        gate_->exited.wait(false, std::memory_order_acquire);
    }
    Gate* gate_;
    std::thread* thread_;
    bool joined_ = false;
};

// RAII: resume a paused gate on scope exit (for tests without a submitter
// thread) and wait for the production path to leave the gate.  The gate object
// must outlive the backend (declared before it in the test), so no disarm is
// needed — lexical scope guarantees the gate is destroyed after the backend
// and its workers.
template <class Gate>
class ScopedGateResume {
public:
    ScopedGateResume(Gate& gate)
        : gate_(&gate) {}
    void resume() {
        if (resumed_) return;
        resume_threadpool_gate(*gate_);
        resumed_ = true;
    }
    ~ScopedGateResume() { cleanup(); }
private:
    void cleanup() {
        resume();
        wait_for_exit();
    }
    void wait_for_exit() noexcept {
        // Issue #86-B: block (zero-CPU) on atomic::wait instead of
        // yield-spinning. exited.notify_one is called by the production path.
        gate_->exited.wait(false, std::memory_order_acquire);
    }
    Gate* gate_;
    bool resumed_ = false;
};

}  // namespace

SLUICE_MAIN()

// Gate A: the pause between enqueue and dispatch push fires INSIDE work_mtx_.
// No cancel is issued in this case: a canceled terminal would be a backend
// defect, so only a real success (value==1, exactly one syscall) is accepted.
SLUICE_TEST_CASE(tp_enqueue_push_share_one_work_domain) {
    PhaseProbe probe;
    probe.name = "tp_enqueue_push_share_one_work_domain";
    // #93 review: gate before watchdog, bind before thread start. Thread
    // creation publishes the non-atomic probe pointer writes; reverse
    // destruction (backend -> watchdog joined -> gate -> probe) keeps the gate
    // alive across the watchdog's diagnostic atomic reads.
    ThreadPoolBackend::AfterArenaEnqueueBeforeDispatchPushPauseGate gate;
    probe.bind_gate(gate.paused, gate.resume, gate.exited);
    Watchdog wd(kWatchdog, probe);
    ThreadPoolBackend backend(ThreadPoolConfig{/*capacity=*/1, /*workers=*/1});
    backend.set_after_enqueue_before_push_pause_gate(&gate);

    TempPath tp("A");
    int fd = open_temp(tp.path());
    const std::byte seed[1] = {std::byte{0x11}};
    SLUICE_CHECK(::pwrite(fd, seed, 1, 0) == 1);

    std::byte buf[1]{};
    Completion<std::size_t> c;

    Result<void> submit_result;
    std::thread submitter([&] {
        submit_result = backend.submit_read(ReadOp{fd, buf, 1, 0}, c);
    });
    ScopedGateAndThread arm(gate, submitter);

    const char* fail_msg = nullptr;
    std::uint64_t syscalls_before = 0;

    wait_paused(gate, probe);
    if (!gate.work_domain_held.load(std::memory_order_acquire)) {
        fail_msg = "Gate A must fire inside the work_mtx_ critical section";
    } else if (gate.dispatch_push_completed.load(std::memory_order_acquire)) {
        fail_msg = "dispatch push must not have completed while paused";
    } else {
        syscalls_before = backend.syscall_count_for_test();
        auto handle = backend.handle_for_completion_for_test(&c);
        if (!handle.has_value()) {
            fail_msg = "handle_for_completion_for_test must find the bound Completion";
        } else {
            auto obs = backend.observe_for_test(*handle);
            if (!obs.has_value()) {
                fail_msg = "observe_for_test must validate the live handle";
            } else if (obs->state != detail::RequestState::enqueued) {
                fail_msg = "slot must be enqueued while paused";
            } else if (obs->enqueue_pin_live) {
                fail_msg = "enqueue pin must be cleared";
            }
        }
    }

    arm.join();

    if (fail_msg == nullptr) {
        SLUICE_CHECK_MSG(submit_result.has_value(),
                         "submit must succeed (commit already accepted)");
        SLUICE_CHECK(drain_bounded(backend,
                                   std::chrono::steady_clock::now() + kDrainDeadline));
        SLUICE_CHECK(c.ready());
        // No cancel was issued — only a real success is legal; a spurious
        // canceled terminal would be a backend defect.
        SLUICE_CHECK(c.result().has_value());
        SLUICE_CHECK(c.result().value() == 1);
        SLUICE_CHECK(backend.syscall_count_for_test() == syscalls_before + 1);
        SLUICE_CHECK(backend.outstanding() == 0);
        c.reset();
        SLUICE_CHECK(backend.arena_slot_in_use() == 0);
    } else {
        // Cleanup on failure so the bound Completion can be reset without
        // triggering the Completion authority fail-fast.
        (void)drain_bounded(backend,
                            std::chrono::steady_clock::now() + kDrainDeadline);
        if (c.ready()) c.reset();
    }

    ::close(fd);
    if (fail_msg != nullptr) SLUICE_FAIL(fail_msg);
}

// Gate B: enqueued cancel wins before the worker dequeues; no syscall runs.
SLUICE_TEST_CASE(tp_enqueued_cancel_wins_no_syscall) {
    PhaseProbe probe;
    probe.name = "tp_enqueued_cancel_wins_no_syscall";
    // #93 review: gate before watchdog, bind before thread start. Thread
    // creation publishes the non-atomic probe pointer writes; reverse
    // destruction (backend -> watchdog joined -> gate -> probe) keeps the gate
    // alive across the watchdog's diagnostic atomic reads.
    ThreadPoolBackend::BeforeWorkerDequeuePauseGate gate;
    probe.bind_gate(gate.paused, gate.resume, gate.exited);
    Watchdog wd(kWatchdog, probe);
    ThreadPoolBackend backend(ThreadPoolConfig{/*capacity=*/1, /*workers=*/1});
    backend.set_before_dequeue_pause_gate(&gate);
    ScopedGateResume guard(gate);

    TempPath tp("B");
    int fd = open_temp(tp.path());
    const std::byte seed[1] = {std::byte{0x22}};
    SLUICE_CHECK(::pwrite(fd, seed, 1, 0) == 1);

    std::byte buf[1]{};
    Completion<std::size_t> c;

    SLUICE_CHECK(backend.submit_read(ReadOp{fd, buf, 1, 0}, c).has_value());

    const char* fail_msg = nullptr;

    wait_paused(gate, probe);
    {
        const std::uint64_t syscalls_before = backend.syscall_count_for_test();
        if (backend.dispatch_size_for_test() != 1) {
            fail_msg = "dispatch ring must hold exactly one enqueued op";
        } else {
            backend.cancel(c);
            guard.resume();
            if (!drain_bounded(backend,
                               std::chrono::steady_clock::now() + kDrainDeadline)) {
                fail_msg = "drain did not complete in time";
            } else if (!c.ready()) {
                fail_msg = "cancel must leave the Completion ready";
            } else if (c.result().has_value()) {
                fail_msg = "canceled op must report an error";
            } else if (c.result().error().code != IoError::Code::canceled) {
                fail_msg = "canceled op must report IoError::canceled";
            } else if (backend.syscall_count_for_test() != syscalls_before) {
                fail_msg = "canceled enqueued op must not execute a syscall";
            } else if (backend.outstanding() != 0) {
                fail_msg = "outstanding must be zero after drain";
            }
        }
    }

    if (fail_msg == nullptr) {
        c.reset();
        SLUICE_CHECK(backend.arena_slot_in_use() == 0);
    } else {
        guard.resume();
        (void)drain_bounded(backend,
                            std::chrono::steady_clock::now() + kDrainDeadline);
        if (c.ready()) c.reset();
    }

    ::close(fd);
    if (fail_msg != nullptr) SLUICE_FAIL(fail_msg);
}

// Gate C: running cancel records intent only; the real syscall result wins verbatim.
SLUICE_TEST_CASE(tp_running_cancel_intent_real_result_verbatim) {
    PhaseProbe probe;
    probe.name = "tp_running_cancel_intent_real_result_verbatim";
    // #93 review: gate before watchdog, bind before thread start. Thread
    // creation publishes the non-atomic probe pointer writes; reverse
    // destruction (backend -> watchdog joined -> gate -> probe) keeps the gate
    // alive across the watchdog's diagnostic atomic reads.
    ThreadPoolBackend::WorkerRunningPauseGate gate;
    probe.bind_gate(gate.paused, gate.resume, gate.exited);
    Watchdog wd(kWatchdog, probe);
    ThreadPoolBackend backend(ThreadPoolConfig{/*capacity=*/1, /*workers=*/1});
    backend.set_running_pause_gate(&gate);
    ScopedGateResume guard(gate);

    TempPath tp("C");
    int fd = open_temp(tp.path());
    const std::byte seed[1] = {std::byte{0x33}};
    SLUICE_CHECK(::pwrite(fd, seed, 1, 0) == 1);

    std::byte buf[1]{};
    Completion<std::size_t> c;

    SLUICE_CHECK(backend.submit_read(ReadOp{fd, buf, 1, 0}, c).has_value());

    const char* fail_msg = nullptr;

    wait_paused(gate, probe);
    {
        const std::uint64_t syscalls_before = backend.syscall_count_for_test();
        if (backend.active_workers_for_test() != 1) {
            fail_msg = "exactly one worker must be running";
        } else {
            backend.cancel(c);
            guard.resume();
            if (!drain_bounded(backend,
                               std::chrono::steady_clock::now() + kDrainDeadline)) {
                fail_msg = "drain did not complete in time";
            } else if (!c.ready()) {
                fail_msg = "running-cancel op must still complete";
            } else if (!c.result().has_value()) {
                fail_msg = "real result must win verbatim; cancel must not rewrite";
            } else if (c.result().value() != 1) {
                fail_msg = "read must return the 1 seeded byte";
            } else if (backend.syscall_count_for_test() != syscalls_before + 1) {
                fail_msg = "exactly one syscall must have executed";
            } else if (backend.outstanding() != 0) {
                fail_msg = "outstanding must be zero after drain";
            }
        }
    }

    if (fail_msg == nullptr) {
        c.reset();
        SLUICE_CHECK(backend.arena_slot_in_use() == 0);
    } else {
        guard.resume();
        (void)drain_bounded(backend,
                            std::chrono::steady_clock::now() + kDrainDeadline);
        if (c.ready()) c.reset();
    }

    ::close(fd);
    if (fail_msg != nullptr) SLUICE_FAIL(fail_msg);
}

// Gate D: terminal publication happens after worker bookkeeping is complete.
SLUICE_TEST_CASE(tp_terminal_publication_after_bookkeeping) {
    PhaseProbe probe;
    probe.name = "tp_terminal_publication_after_bookkeeping";
    // #93 review: gate before watchdog, bind before thread start. Thread
    // creation publishes the non-atomic probe pointer writes; reverse
    // destruction (backend -> watchdog joined -> gate -> probe) keeps the gate
    // alive across the watchdog's diagnostic atomic reads.
    ThreadPoolBackend::TerminalPublicationPauseGate gate;
    probe.bind_gate(gate.paused, gate.resume, gate.exited);
    Watchdog wd(kWatchdog, probe);
    ThreadPoolBackend backend(ThreadPoolConfig{/*capacity=*/1, /*workers=*/1});
    backend.set_terminal_publication_pause_gate(&gate);
    ScopedGateResume guard(gate);

    TempPath tp("D");
    int fd = open_temp(tp.path());
    const std::byte seed[1] = {std::byte{0x44}};
    SLUICE_CHECK(::pwrite(fd, seed, 1, 0) == 1);

    std::byte buf[1]{};
    Completion<std::size_t> c;

    SLUICE_CHECK(backend.submit_read(ReadOp{fd, buf, 1, 0}, c).has_value());

    const char* fail_msg = nullptr;

    wait_paused(gate, probe);
    {
        // Post-fix invariant: bookkeeping is done, but reap has not yet published.
        if (backend.active_workers_for_test() != 0) {
            fail_msg = "active_workers must be zero before publication";
        } else if (backend.syscall_count_for_test() != 1) {
            fail_msg = "exactly one syscall must have executed before publication";
        } else if (backend.poll() != 0) {
            fail_msg = "poll must see nothing ready before publication";
        } else if (c.ready()) {
            fail_msg = "Completion must not be ready before reap publishes";
        } else {
            guard.resume();
            if (!drain_bounded(backend,
                               std::chrono::steady_clock::now() + kDrainDeadline)) {
                fail_msg = "drain did not complete in time";
            } else if (!c.ready()) {
                fail_msg = "op must complete after resume";
            } else if (!c.result().has_value()) {
                fail_msg = "read must succeed";
            } else if (c.result().value() != 1) {
                fail_msg = "read must return the 1 seeded byte";
            } else if (backend.outstanding() != 0) {
                fail_msg = "outstanding must be zero after drain";
            }
        }
    }

    if (fail_msg == nullptr) {
        c.reset();
        SLUICE_CHECK(backend.arena_slot_in_use() == 0);
    } else {
        guard.resume();
        (void)drain_bounded(backend,
                            std::chrono::steady_clock::now() + kDrainDeadline);
        if (c.ready()) c.reset();
    }

    ::close(fd);
    if (fail_msg != nullptr) SLUICE_FAIL(fail_msg);
}

// ---- C2b row 5 (ThreadPool): canceled_ops tallies ONLY on terminal_won ------
// Enqueued cancel wins the terminal (Gate B): exactly one canceled_ops. A late
// cancel on the already-terminal (still bound) Completion is already_terminal
// and never tallies again; the worker runs no syscall.
SLUICE_TEST_CASE(tp_canceled_ops_tallied_only_on_terminal_won) {
    PhaseProbe probe;
    probe.name = "tp_canceled_ops_tallied_only_on_terminal_won";
    // #93 review: gate before watchdog, bind before thread start. Thread
    // creation publishes the non-atomic probe pointer writes; reverse
    // destruction (backend -> watchdog joined -> gate -> probe) keeps the gate
    // alive across the watchdog's diagnostic atomic reads.
    ThreadPoolBackend::BeforeWorkerDequeuePauseGate gate;
    probe.bind_gate(gate.paused, gate.resume, gate.exited);
    Watchdog wd(kWatchdog, probe);
    ThreadPoolBackend backend(ThreadPoolConfig{/*capacity=*/1, /*workers=*/1});
    backend.set_before_dequeue_pause_gate(&gate);
    ScopedGateResume guard(gate);
    sluice::AsyncStats stats;
    backend.attach_stats(&stats);

    TempPath tp("E");
    int fd = open_temp(tp.path());
    const std::byte seed[1] = {std::byte{0x55}};
    SLUICE_CHECK(::pwrite(fd, seed, 1, 0) == 1);

    std::byte buf[1]{};
    Completion<std::size_t> c;

    SLUICE_CHECK(backend.submit_read(ReadOp{fd, buf, 1, 0}, c).has_value());

    const char* fail_msg = nullptr;

    wait_paused(gate, probe);
    {
        const std::uint64_t syscalls_before = backend.syscall_count_for_test();
        backend.cancel(c);  // enqueued -> terminal_won
        if (stats.canceled_ops != 1) {
            fail_msg = "terminal_won must tally exactly one canceled op";
        } else if (stats.completion_errors != 0) {
            fail_msg = "a canceled winner is not a completion error";
        } else {
            // Late cancel while the canceled terminal is bound:
            // already_terminal -> no second tally.
            backend.cancel(c);
            if (stats.canceled_ops != 1) {
                fail_msg = "late cancel must not tally again";
            } else {
                guard.resume();
                if (!drain_bounded(backend,
                                   std::chrono::steady_clock::now() + kDrainDeadline)) {
                    fail_msg = "drain did not complete in time";
                } else if (!c.ready()) {
                    fail_msg = "canceled op must be ready after drain";
                } else if (c.result().has_value()) {
                    fail_msg = "canceled op must report an error";
                } else if (c.result().error().code != IoError::Code::canceled) {
                    fail_msg = "canceled op must report IoError::canceled";
                } else if (backend.syscall_count_for_test() != syscalls_before) {
                    fail_msg = "canceled enqueued op must not execute a syscall";
                }
            }
        }
    }

    if (fail_msg == nullptr) {
        // Cancel AFTER ready (still bound until reset): already_terminal.
        backend.cancel(c);
        SLUICE_CHECK(stats.canceled_ops == 1);
        c.reset();
        SLUICE_CHECK(backend.arena_slot_in_use() == 0);
    } else {
        guard.resume();
        (void)drain_bounded(backend,
                            std::chrono::steady_clock::now() + kDrainDeadline);
        if (c.ready()) c.reset();
    }

    ::close(fd);
    if (fail_msg != nullptr) SLUICE_FAIL(fail_msg);
}

// ---- C2b row 5/6 (ThreadPool): running-cancel intent never tallies ---------
// Gate C: cancel on a RUNNING op records intent only — no canceled terminal,
// no canceled_ops tally; the real syscall result wins VERBATIM (never rewritten
// to canceled). A cancel after that ordinary winner is already_terminal.
SLUICE_TEST_CASE(tp_running_cancel_intent_does_not_tally) {
    PhaseProbe probe;
    probe.name = "tp_running_cancel_intent_does_not_tally";
    // #93 review: gate before watchdog, bind before thread start. Thread
    // creation publishes the non-atomic probe pointer writes; reverse
    // destruction (backend -> watchdog joined -> gate -> probe) keeps the gate
    // alive across the watchdog's diagnostic atomic reads.
    ThreadPoolBackend::WorkerRunningPauseGate gate;
    probe.bind_gate(gate.paused, gate.resume, gate.exited);
    Watchdog wd(kWatchdog, probe);
    ThreadPoolBackend backend(ThreadPoolConfig{/*capacity=*/1, /*workers=*/1});
    backend.set_running_pause_gate(&gate);
    ScopedGateResume guard(gate);
    sluice::AsyncStats stats;
    backend.attach_stats(&stats);

    TempPath tp("F");
    int fd = open_temp(tp.path());
    const std::byte seed[1] = {std::byte{0x66}};
    SLUICE_CHECK(::pwrite(fd, seed, 1, 0) == 1);

    std::byte buf[1]{};
    Completion<std::size_t> c;

    SLUICE_CHECK(backend.submit_read(ReadOp{fd, buf, 1, 0}, c).has_value());

    const char* fail_msg = nullptr;

    wait_paused(gate, probe);
    {
        backend.cancel(c);  // running -> intent_recorded
        if (stats.canceled_ops != 0) {
            fail_msg = "intent_recorded must NOT tally canceled_ops";
        } else if (backend.backend_ready_count_for_test() != 0) {
            fail_msg = "intent must not store a terminal or push the ready ring";
        } else if (c.ready()) {
            fail_msg = "the Completion must stay outstanding on intent";
        } else {
            guard.resume();
            if (!drain_bounded(backend,
                               std::chrono::steady_clock::now() + kDrainDeadline)) {
                fail_msg = "drain did not complete in time";
            } else if (!c.ready()) {
                fail_msg = "running-cancel op must still complete";
            } else if (!c.result().has_value()) {
                fail_msg = "real result must win verbatim; intent must not rewrite";
            } else if (c.result().value() != 1) {
                fail_msg = "read must return the 1 seeded byte";
            } else if (stats.canceled_ops != 0) {
                fail_msg = "an ordinary winner must never tally canceled_ops";
            }
        }
    }

    if (fail_msg == nullptr) {
        // Cancel after the ordinary winner: already_terminal, still no tally.
        backend.cancel(c);
        SLUICE_CHECK(stats.canceled_ops == 0);
        c.reset();
        SLUICE_CHECK(backend.arena_slot_in_use() == 0);
    } else {
        guard.resume();
        (void)drain_bounded(backend,
                            std::chrono::steady_clock::now() + kDrainDeadline);
        if (c.ready()) c.reset();
    }

    ::close(fd);
    if (fail_msg != nullptr) SLUICE_FAIL(fail_msg);
}

// ---- C2b row 4 (ThreadPool integration): stale-generation events harmless --
// Issue #68 row 4 requires: after a slot is released and the SAME physical slot
// is reused by a NEW request (generation N+1), a stale-generation event (the
// N-handle) must NOT act on the live N+1 occupant. The stale handle is injected
// through cancel_handle_for_test, which routes it through the REAL cancel
// authority path (remove_exact + arena_.cancel under work_mtx_, tally on
// terminal_won) — the same path the public cancel(Completion&) takes after
// resolving the pointer. The BeforeWorkerDequeuePauseGate holds the N+1
// occupant in the `enqueued` state so the stale event targets a genuinely LIVE
// occupant (not a free slot). The new occupant's result, counters, syscall
// count, and state all stay exactly intact; the stale handle resolves to
// not_found. All identity is pointer-free (SlotHandle/RequestKey) — no
// Completion reverse map.
SLUICE_TEST_CASE(tp_stale_generation_event_harmless) {
    PhaseProbe probe;
    probe.name = "tp_stale_generation_event_harmless";
    // #93 review: gate before watchdog, bind before thread start. Thread
    // creation publishes the non-atomic probe pointer writes; reverse
    // destruction (backend -> watchdog joined -> gate -> probe) keeps the gate
    // alive across the watchdog's diagnostic atomic reads.
    ThreadPoolBackend::BeforeWorkerDequeuePauseGate gate;
    probe.bind_gate(gate.paused, gate.resume, gate.exited);
    Watchdog wd(kWatchdog, probe);
    ThreadPoolBackend backend(ThreadPoolConfig{/*capacity=*/1, /*workers=*/1});
    // The gate is NOT armed for the gen-N lifecycle (it must drain freely); it
    // is armed only for the gen-N+1 submit so that occupant stays enqueued.
    sluice::AsyncStats stats;
    backend.attach_stats(&stats);

    TempPath tp("G");
    int fd = open_temp(tp.path());
    const std::byte seed[1] = {std::byte{0x77}};
    SLUICE_CHECK(::pwrite(fd, seed, 1, 0) == 1);

    std::byte buf[1]{};
    Completion<std::size_t> c;

    const char* fail_msg = nullptr;
    std::optional<detail::SlotHandle> h0;

    // Generation N: full lifecycle; capture the slot+generation identity BEFORE
    // the release. The handle becomes stale once the slot is freed.
    if (!backend.submit_read(ReadOp{fd, buf, 1, 0}, c).has_value()) {
        fail_msg = "first submit must succeed";
    } else if (!drain_bounded(backend,
                              std::chrono::steady_clock::now() + kDrainDeadline)) {
        fail_msg = "first drain did not complete in time";
    } else if (!c.ready() || !c.result().has_value() || c.result().value() != 1) {
        fail_msg = "first op must complete with the seeded byte";
    } else {
        h0 = backend.handle_for_completion_for_test(&c);
        if (!h0.has_value()) {
            fail_msg = "the bound Completion must resolve to a slot handle";
        }
    }

    std::optional<detail::SlotHandle> h1;
    if (fail_msg == nullptr) {
        c.reset();  // release handshake: slot freed, generation advances to N+1
        // Arm the gate NOW so the gen-N+1 occupant stays enqueued (the worker
        // pauses before dequeue), making it a LIVE target for the stale event.
        backend.set_before_dequeue_pause_gate(&gate);
        // The SAME physical slot is reused by a NEW request (generation N+1)
        // BEFORE the stale event is injected.
        if (!backend.submit_read(ReadOp{fd, buf, 1, 0}, c).has_value()) {
            fail_msg = "second submit must reuse the released slot";
        } else {
            wait_paused(gate, probe);
            h1 = backend.handle_for_completion_for_test(&c);
            if (!h1.has_value()) {
                fail_msg = "new occupant must resolve to a slot handle";
            } else if (h1->slot.value != h0->slot.value ||
                       h1->generation.value != h0->generation.value + 1) {
                fail_msg = "reuse must keep the slot and advance generation by one";
            }
        }
    }

    const std::uint64_t syscalls_before_inject =
        (fail_msg == nullptr) ? backend.syscall_count_for_test() : 0;
    if (fail_msg == nullptr) {
        // NOW inject the stale N-handle through the REAL cancel authority path
        // while the N+1 occupant is LIVE (enqueued, worker paused pre-dequeue).
        detail::CancelDisposition disp = backend.cancel_handle_for_test(*h0);
        if (disp != detail::CancelDisposition::not_found) {
            fail_msg = "stale handle must resolve to not_found against a live N+1";
        } else if (stats.canceled_ops != 0) {
            fail_msg = "stale cancel must not tally canceled_ops";
        } else if (backend.outstanding() != 1) {
            fail_msg = "live N+1 occupant must remain outstanding";
        } else {
            // Observe exactly ONCE: if the stale event destroyed the N+1
            // occupant, observe_for_test returns nullopt — that MUST fail (a
            // missing occupant is the worst regression this case can catch).
            auto obs = backend.observe_for_test(*h1);
            if (!obs.has_value()) {
                fail_msg = "live N+1 occupant must still be observable";
            } else if (obs->state != detail::RequestState::enqueued) {
                fail_msg = "live N+1 occupant must stay enqueued";
            }
        }
    }

    if (fail_msg == nullptr) {
        // Resume the worker; the live N+1 occupant completes with ITS OWN
        // result. The stale injection left no residue.
        resume_threadpool_gate(gate);
        // Wait for the production path to leave the gate before unbinding it.
        gate.exited.wait(false, std::memory_order_acquire);
    }

    if (fail_msg == nullptr) {
        if (!drain_bounded(backend,
                           std::chrono::steady_clock::now() + kDrainDeadline)) {
            fail_msg = "second drain did not complete in time";
        } else if (!c.ready() || !c.result().has_value() ||
                   c.result().value() != 1) {
            fail_msg = "new occupant must complete with ITS OWN result";
        } else if (backend.syscall_count_for_test() != syscalls_before_inject + 1) {
            fail_msg = "exactly one new syscall for the new occupant";
        } else if (stats.canceled_ops != 0 || stats.completion_errors != 0) {
            fail_msg = "stale attempts must leave counters intact";
        }
    }

    if (fail_msg == nullptr) {
        c.reset();
        SLUICE_CHECK(backend.arena_slot_in_use() == 0);
    } else {
        resume_threadpool_gate(gate);
        gate.exited.wait(false, std::memory_order_acquire);
        (void)drain_bounded(backend,
                            std::chrono::steady_clock::now() + kDrainDeadline);
        if (c.ready()) c.reset();
    }

    ::close(fd);
    if (fail_msg != nullptr) SLUICE_FAIL(fail_msg);
}

// ---- C2b row 8 (ThreadPool): publication boundary — reap gates ready -------
// Runtime evidence that a worker NEVER publishes: once the worker's syscall
// finished and record_terminal stored the backend_ready terminal, the
// Completion is STILL not ready — only poll()/wait_one() reap publishes
// through the slot binding. No gate is needed: the test catches the exact
// backend_ready window (terminal stored, not yet reaped) because only the
// main thread reaps.
SLUICE_TEST_CASE(tp_publication_boundary_reap_gates_ready) {
    PhaseProbe probe;
    probe.name = "tp_publication_boundary_reap_gates_ready";
    Watchdog wd(kWatchdog, probe);
    ThreadPoolBackend backend(ThreadPoolConfig{/*capacity=*/1, /*workers=*/1});

    TempPath tp("H");
    int fd = open_temp(tp.path());
    const std::byte seed[1] = {std::byte{0x88}};
    SLUICE_CHECK(::pwrite(fd, seed, 1, 0) == 1);

    std::byte buf[1]{};
    Completion<std::size_t> c;

    SLUICE_CHECK(backend.submit_read(ReadOp{fd, buf, 1, 0}, c).has_value());

    const char* fail_msg = nullptr;
    const auto deadline = std::chrono::steady_clock::now() + kDrainDeadline;
    // Wait for the worker to record the terminal (backend_ready), BEFORE any
    // reap runs.
    while (backend.backend_ready_count_for_test() == 0) {
        if (std::chrono::steady_clock::now() >= deadline) {
            fail_msg = "terminal was not recorded before timeout";
            break;
        }
        std::this_thread::yield();
    }

    if (fail_msg == nullptr) {
        if (c.ready()) {
            fail_msg = "Completion must NOT be ready before poll/reap publishes";
        } else if (backend.syscall_count_for_test() != 1) {
            fail_msg = "exactly one syscall must have executed";
        } else if (backend.poll() != 1) {
            fail_msg = "the reap must publish exactly one Completion";
        } else if (!c.ready()) {
            fail_msg = "reap must publish the Completion ready";
        } else if (!c.result().has_value() || c.result().value() != 1) {
            fail_msg = "read must return the 1 seeded byte";
        } else if (backend.poll() != 0) {
            fail_msg = "a second poll must publish nothing (exactly-once)";
        }
    }

    if (fail_msg == nullptr) {
        c.reset();
        SLUICE_CHECK(backend.arena_slot_in_use() == 0);
    } else {
        (void)drain_bounded(backend,
                            std::chrono::steady_clock::now() + kDrainDeadline);
        if (c.ready()) c.reset();
    }

    ::close(fd);
    if (fail_msg != nullptr) SLUICE_FAIL(fail_msg);
}

// ---- C2b rows 6/7 (ThreadPool): cancel races the worker terminal winner ----
// Genuine causal two-thread TSan evidence: BeforeWorkerDequeuePauseGate holds
// the worker in the pre-dequeue window on EVERY iteration, so the op is
// provably `enqueued` when the barrier releases. The barrier then releases the
// canceler and the worker-gate resume together, so cancel and dequeue race for
// the single terminal transition under the backend's work_mtx_ arbitration.
// This closes the "worker already finished before the canceler started" hole:
// the race is forced, not probabilistic. Each iteration asserts the exactly-one
// winner contract end to end:
//   - exactly one publication (poll total == 1), one ready Completion
//   - the result is EITHER canceled OR the real 1-byte success
//   - canceled_ops tallies exactly the canceled winners (never intent/losers)
//   - syscall_count tallies exactly the syscall winners (cancel-won iterations
//     run no syscall)
// No sleep_for, no timing assumptions.
SLUICE_TEST_CASE(tp_cancel_races_worker_terminal_exactly_one) {
    PhaseProbe probe;
    probe.name = "tp_cancel_races_worker_terminal_exactly_one";
    // #93 review: gate before watchdog, bind before thread start. Thread
    // creation publishes the non-atomic probe pointer writes; reverse
    // destruction (backend -> watchdog joined -> gate -> probe) keeps the gate
    // alive across the watchdog's diagnostic atomic reads.
    ThreadPoolBackend::BeforeWorkerDequeuePauseGate gate;
    probe.bind_gate(gate.paused, gate.resume, gate.exited);
    Watchdog wd(kWatchdog, probe);
    constexpr std::size_t kIters = 64;
    ThreadPoolBackend backend(ThreadPoolConfig{/*capacity=*/1, /*workers=*/1});
    backend.set_before_dequeue_pause_gate(&gate);
    sluice::AsyncStats stats;
    backend.attach_stats(&stats);

    TempPath tp("I");
    int fd = open_temp(tp.path());
    const std::byte seed[1] = {std::byte{0x99}};
    SLUICE_CHECK(::pwrite(fd, seed, 1, 0) == 1);

    std::byte buf[1]{};
    std::uint64_t canceled_total = 0;
    std::uint64_t success_total = 0;
    const char* fail_msg = nullptr;

    // Per-iteration cleanup. The Completion is a loop-body local; destroying it
    // while outstanding/publishing is a Completion-authority fail-fast that
    // would mask the real failure attribution (the test's own SLUICE_FAIL would
    // never run). So EVERY fail path must drain the backend and reset/abort the
    // Completion before breaking out of the iteration scope. The gate is also
    // resumed so the worker is never stranded, and the gate-exit is awaited so
    // the next iteration (or the final backend destruction) sees a stable gate.
    auto cleanup_iteration = [&](Completion<std::size_t>& c) noexcept {
        resume_threadpool_gate(gate);
        gate.exited.wait(false, std::memory_order_acquire);
        if (!drain_bounded(backend,
                           std::chrono::steady_clock::now() + kDrainDeadline)) {
            // The harness itself cannot safely recover from a stuck backend;
            // abort so the failure cause is explicit, not a destructor race.
            std::abort();
        }
        if (c.ready()) {
            c.reset();
        } else if (!c.idle()) {
            // Never allow an outstanding/publishing Completion to destruct.
            std::abort();
        }
    };
    // Sets the failure message and cleans up the iteration. The caller MUST
    // follow with an explicit `break` (or an `if (fail_msg != nullptr) break`
    // after an inner loop) — no control-flow is hidden in the helper.
    auto fail_iteration = [&](const char* msg, Completion<std::size_t>& c) {
        fail_msg = msg;
        cleanup_iteration(c);
    };

    for (std::size_t iter = 0; iter < kIters && fail_msg == nullptr; ++iter) {
        Completion<std::size_t> c;
        if (!backend.submit_read(ReadOp{fd, buf, 1, 0}, c).has_value()) {
            // Submit rejected: c is idle, nothing to clean up.
            fail_msg = "submit must succeed on a drained backend";
            break;
        }
        // Force the worker into the pre-dequeue window so the op is provably
        // `enqueued` before the race is released.
        wait_paused(gate, probe);
        // Barrier synchronizes TWO threads: the main thread (which resumes the
        // worker gate) and the canceler. Both release together AFTER the worker
        // is confirmed paused, so cancel and dequeue genuinely race. The
        // canceler executes cancel immediately (it cannot run before the barrier
        // because the worker was already paused BEFORE the barrier released),
        // while the main thread resumes the worker gate at the same instant.
        // The two then contend for work_mtx_: cancel either wins the enqueued
        // terminal (terminal_won -> canceled winner, no syscall) or loses to the
        // worker's dequeue (running/ordinary winner). Either outcome is valid;
        // the exactly-one assertions below are the contract. The canceler is
        // created inside a try block and joined before scope exit so a
        // thread-creation failure under load cannot leave a joinable thread
        // (which would std::terminate the process at scope end).
        probe.set(CasePhase::barrier);
        std::barrier sync{2};
        std::thread canceler;
        try {
            canceler = std::thread([&] {
                sync.arrive_and_wait();
                backend.cancel(c);
            });
        } catch (...) {
            // Thread creation can fail under heavy concurrency load. The gate
            // is resumed inside cleanup_iteration so the worker is not stranded.
            fail_iteration("canceler thread creation failed under load", c);
            break;
        }
        sync.arrive_and_wait();
        resume_threadpool_gate(gate);
        probe.set(CasePhase::join_canceler);
        canceler.join();
        // Wait for the worker to leave the gate before the next iteration arms
        // it again (the gate struct is reused).
        gate.exited.wait(false, std::memory_order_acquire);
        // Re-arm the gate for the next iteration. Safe only AFTER exited was
        // observed true: a production thread reaches resume.wait() only after
        // setting exited=false, so resetting resume=false here cannot drop a
        // wake under a still-waiting epoch.
        probe.set(CasePhase::reset);
        rearm_threadpool_gate(gate);

        // Drain through the real reaper, counting publications. The loop
        // condition is `!c.ready()` (not just `outstanding() > 0`) so the
        // `c.ready()`/`result()` assertions below are never racy: the Completion
        // is published exactly by the reap inside poll(), and we only proceed
        // once that publication is observed.
        probe.set(CasePhase::drain);
        std::size_t published = 0;
        const auto deadline = std::chrono::steady_clock::now() + kDrainDeadline;
        while (!c.ready()) {
            if (std::chrono::steady_clock::now() >= deadline) {
                fail_iteration("drain must complete within the bounded deadline",
                               c);
                break;
            }
            published += backend.poll();
            if (!c.ready()) std::this_thread::yield();
        }
        if (fail_msg != nullptr) break;

        if (published != 1) {
            fail_iteration("exactly one publication per iteration", c);
            break;
        }
        if (!c.ready()) {
            fail_iteration("Completion must be ready after drain", c);
            break;
        }
        if (c.result().has_value()) {
            if (c.result().value() != 1) {
                fail_iteration("real result must be the 1 seeded byte", c);
                break;
            }
            ++success_total;
        } else {
            if (c.result().error().code != IoError::Code::canceled) {
                fail_iteration("non-success result must be canceled", c);
                break;
            }
            ++canceled_total;
        }
        c.reset();

        // Exactly-one accounting: a canceled winner tallied one canceled op and
        // ran no syscall; a syscall winner tallied neither. (c is now idle, so a
        // fail here needs no Completion cleanup.)
        if (stats.canceled_ops != canceled_total) {
            fail_msg = "canceled_ops must tally exactly the canceled winners";
            break;
        }
        if (backend.syscall_count_for_test() != success_total) {
            fail_msg = "syscall_count must tally exactly the syscall winners";
            break;
        }
        if (backend.arena_slot_in_use() != 0) {
            fail_msg = "slot must be released after reset";
            break;
        }
    }

    if (fail_msg == nullptr) {
        if (canceled_total + success_total != kIters) {
            fail_msg = "every iteration must produce exactly one winner";
        }
    }

    // Safety: ensure the gate is resumed and the worker has exited it before
    // the backend (and its worker thread) is destroyed. The case-level Watchdog
    // catches a genuinely stuck worker.
    resume_threadpool_gate(gate);
    gate.exited.wait(false, std::memory_order_acquire);

    ::close(fd);
    if (fail_msg != nullptr) SLUICE_FAIL(fail_msg);
}

// #93 review follow-up — TSan diagnostic-path coverage. A normal (green) run
// never fires the case-level Watchdog, so diagnose_and_abort() — and its reads
// of the bound gate atomics — are never executed; a plain TSan run therefore
// instruments none of those reads. This case FORCES the diagnostic path: a
// forked child constructs probe + gate + bind + Watchdog in the review-mandated
// order (gate before watchdog; bind before thread start) with a SHORT timeout,
// then blocks without resuming. The watchdog fires, reads the bound gate
// atomics, prints the `gate: paused=…` line, and aborts. The parent asserts the
// child was terminated by SIGABRT AND that the `gate:` line was printed (proving
// the diagnostic branch actually dereferenced the bound gate atomics). Under
// TSan this is the only run that exercises those reads, so a race on them would
// be reported here rather than hidden behind a green normal run. POSIX only.
SLUICE_TEST_CASE(tp_watchdog_diagnostic_path_reads_bound_gate) {
    int pipefd[2];
    if (::pipe(pipefd) != 0) {
        SLUICE_FAIL("pipe() failed for watchdog diagnostic-path test");
    }

    pid_t pid = ::fork();
    if (pid < 0) {
        ::close(pipefd[0]);
        ::close(pipefd[1]);
        SLUICE_FAIL("fork() failed for watchdog diagnostic-path test");
    }

    if (pid == 0) {
        // Child: send stderr (the watchdog diagnostic output) down the pipe.
        ::close(pipefd[0]);
        ::dup2(pipefd[1], STDERR_FILENO);
        ::close(pipefd[1]);

        // Review-mandated order: declare the gate and bind it BEFORE starting
        // the watchdog thread. std::thread creation publishes the non-atomic
        // probe pointer writes; reverse-declaration destruction
        // (watchdog joined -> gate -> probe) keeps the gate alive across the
        // diagnostic reads.
        PhaseProbe probe;
        probe.name = "tp_watchdog_diagnostic_path_reads_bound_gate";
        ThreadPoolBackend::BeforeWorkerDequeuePauseGate gate;
        probe.bind_gate(gate.paused, gate.resume, gate.exited);
        // Deliberately short timeout: this child is constructed to FIRE the
        // diagnostic path. The case-level 30s kWatchdog is intentionally not
        // used — a normal run never reaches diagnose_and_abort, so a plain TSan
        // run instruments none of its gate-atomic reads; this is the only run
        // that does.
        Watchdog wd(std::chrono::seconds(1), probe);

        // Never resume: the watchdog must fire, read the bound gate atomics,
        // print the diagnostic, and abort. Block until that happens.
        for (;;) {
            std::this_thread::sleep_for(std::chrono::seconds(60));
        }
    }

    // Parent: close the write end so read() sees EOF when the child aborts.
    ::close(pipefd[1]);
    std::string captured;
    for (;;) {
        char buf[256];
        ssize_t n = ::read(pipefd[0], buf, sizeof(buf));
        if (n > 0) {
            captured.append(buf, static_cast<std::size_t>(n));
        } else if (n == 0) {
            break;
        } else if (errno == EINTR) {
            continue;
        } else {
            break;
        }
    }
    ::close(pipefd[0]);

    int status = 0;
    pid_t w;
    do {
        w = ::waitpid(pid, &status, 0);
    } while (w < 0 && errno == EINTR);

    const char* fail_msg = nullptr;
    if (w != pid) {
        fail_msg = "waitpid for the watchdog diagnostic child failed";
    } else if (!WIFSIGNALED(status) || WTERMSIG(status) != SIGABRT) {
        fail_msg = "watchdog diagnostic child must abort via SIGABRT";
    } else if (captured.find("gate: paused=") == std::string::npos) {
        fail_msg = "watchdog must read+print the bound gate atomics";
    } else if (captured.find("tp_watchdog_diagnostic_path_reads_bound_gate") ==
               std::string::npos) {
        fail_msg = "watchdog must print the bound case name";
    }
    if (fail_msg != nullptr) SLUICE_FAIL(fail_msg);
}
