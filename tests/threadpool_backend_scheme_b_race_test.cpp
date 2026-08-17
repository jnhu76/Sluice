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
// Issue #110: the multi-iteration race case
// (tp_cancel_races_worker_terminal_exactly_one) uses the generation-scoped
// BeforeWorkerDequeuePauseGate handshake — arm(N) -> paused(N) -> resume(N) ->
// ACK(N) published only after the worker's pop_front decision — so a
// generation-N worker continuation can never consume iteration N+1's dispatch
// entry without generation N+1's gate observation (the pre-#110 `exited` bool
// fired pre-pop and licensed exactly that theft).
// tp_dequeue_gate_generation_blocks_cross_iteration_theft is the deterministic
// issue-#110 regression (PostResumePrePopHoldGate holds the exact pre-pop
// window).
//
// Links sluice_async_internal_testing (the seams are guarded by
// SLUICE_ASYNC_INTERNAL_TESTING; production sluice_async has no seams).
#include "death_test_runner_posix.hpp"
#include "harness.hpp"

#include <sluice/async/completion.hpp>
#include <sluice/async/threadpool_backend.hpp>
#include <sluice/detail/posix_retry.hpp>
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
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <mutex>
#include <poll.h>
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
    resume,      wait_exited, wait_ack,  barrier,
    join_canceler, drain,   reset,       teardown,
    done,
};

inline const char* phase_name(CasePhase p) noexcept {
    switch (p) {
    case CasePhase::setup:        return "setup";
    case CasePhase::submit:       return "submit";
    case CasePhase::wait_paused:  return "wait_paused";
    case CasePhase::inspect:      return "inspect";
    case CasePhase::resume:       return "resume";
    case CasePhase::wait_exited:  return "wait_exited";
    case CasePhase::wait_ack:     return "wait_ack";
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
    // Issue #101 corrective (2026-08-13): progress checkpoints. `progress_epoch`
    // is bumped at every phase transition and every race-loop iteration;
    // `iteration` is the current race-loop index. The watchdog polls these
    // lock-free counters and uses them to distinguish State A (progress
    // continued until the case-total deadline — budget exhaustion / slow
    // execution) from State B (no progress for a bounded interval — a genuine
    // stall). Diagnostic-only: relaxed stores; the watchdog reads with acquire.
    std::atomic<std::uint64_t> progress_epoch{0};
    std::atomic<std::uint64_t> iteration{0};

    void set(CasePhase p) noexcept {
        phase.store(p, std::memory_order_release);
        progress_epoch.fetch_add(1, std::memory_order_relaxed);
    }
    void note_iteration(std::uint64_t iter) noexcept {
        iteration.store(iter, std::memory_order_relaxed);
        progress_epoch.fetch_add(1, std::memory_order_relaxed);
    }
    void bind_gate(const std::atomic<bool>& paused,
                   const std::atomic<bool>& resume,
                   const std::atomic<bool>& exited) noexcept {
        gate_paused = &paused;
        gate_resume = &resume;
        gate_exited = &exited;
    }
    // Issue #110: generation-handshake binding (the BeforeWorkerDequeuePauseGate
    // seq triple) for watchdog diagnostics of generation-protocol cases. A case
    // may bind BOTH this and the bool triple (e.g. the hold gate's bools); the
    // watchdog prints whichever slots are bound. Reads ONLY atomics.
    const std::atomic<std::uint64_t>* gate_paused_at = nullptr;
    const std::atomic<std::uint64_t>* gate_resumed_at = nullptr;
    const std::atomic<std::uint64_t>* gate_acked_at = nullptr;
    void bind_dequeue_generation_gate(
        const std::atomic<std::uint64_t>& paused_at,
        const std::atomic<std::uint64_t>& resumed_at,
        const std::atomic<std::uint64_t>& acked_at) noexcept {
        gate_paused_at = &paused_at;
        gate_resumed_at = &resumed_at;
        gate_acked_at = &acked_at;
    }
};

// Issue #86-B / #92 / #101 case-level last-resort watchdog. The gate handshakes
// are fully bidirectional blocking atomic::wait (zero-CPU), so under correct
// protocol every case completes in seconds — even the 64-iteration race loop
// takes well under 15s under TSan. The deadline is NOT a correctness deadline:
// a wall-clock bound cannot distinguish a genuine protocol deadlock from
// complete host-scheduler starvation. It is a last-resort boundedness guard
// that converts an unbounded hang into a bounded abort.
//
// Issue #101 model defect (2026-08-14): a CASE-TOTAL wall-clock deadline is NOT
// a liveness oracle. The watchdog polls the probe's progress checkpoints
// (lock-free atomics only) every kPollInterval and records when the progress
// epoch last moved. The ONLY abort trigger is a GENUINE no-progress stall — the
// progress epoch frozen for >= the full case budget — checked CONTINUOUSLY so a
// real stall is caught ~threshold after it happens, not at the case-total
// budget. Case-total budget exhaustion with progress continuing (State A —
// host-scheduler slowdown under load) is NOT stall evidence and is reported
// non-fatally with a re-armed budget window; the race cases are straight-line /
// bounded-iteration, so continued progress always converges, and a real
// deadlock freezes progress and is caught by the stall check. This does NOT
// weaken the gate: a true stall still ABORTs (a stuck protocol is a catastrophic
// defect, not a Scheme-B correctness assertion), no duration is increased, and
// the abort path adds no retry/sleep.
//
// Evidence boundary (audit review, 2026-08-14): the defect class above is
// PROVEN by the controlled-timestamp policy test and the synthetic progressing
// child below. The historical 2026-08-13 serial 1/5 firing at 5e5ec36 is NOT
// retroactively classified — no progress telemetry existed for it, so it is
// neither STALLED nor PROGRESS CONTINUED. Removing the defect class does not
// prove that historical firing was a progressing case; its classification
// remains unresolved unless a future capture on instrumented historical code
// says otherwise. "Watchdog model defect" and "historical residual root cause"
// are therefore two separate statements.
//
// The abort decision is a PURE function of explicit time inputs (no threads,
// no sleeps, no I/O): watchdog_decide() below is proven deterministically with
// controlled timestamps in tp_watchdog_decision_policy_controlled_timestamps.
// AGENTS.md §13.3: sleep_for must not prove liveness — the policy is proven
// with injected time; the fork-based cases below are end-to-end wiring checks,
// and their sleeps are pacing/diagnosis only.
enum class WatchdogDecision : std::uint8_t {
    Continue,                 // keep observing; nothing to report
    ReportProgressContinued,  // budget window expired, progress recent — NOT a stall
    AbortStalled,             // progress frozen >= threshold — genuine stall
};

struct WatchdogInputs {
    std::chrono::steady_clock::time_point now;
    std::chrono::steady_clock::time_point last_progress_at;
    std::chrono::steady_clock::time_point budget_deadline;  // current window end
    std::chrono::milliseconds no_progress_threshold;        // full case budget
};

struct WatchdogDecisionResult {
    WatchdogDecision action;
    // Re-armed window deadline; meaningful only when action ==
    // ReportProgressContinued. The window is re-armed to one full budget from
    // `now` so a real stall beginning after a re-arm is still caught (the
    // stall check runs continuously, independent of the window).
    std::chrono::steady_clock::time_point next_budget_deadline{};
};

inline WatchdogDecisionResult watchdog_decide(const WatchdogInputs& in) noexcept {
    // A genuine stall is the ONLY abort trigger (issue #101). Checked BEFORE
    // the budget window so a real deadlock is caught as soon as the freeze
    // reaches the threshold, regardless of the current (possibly re-armed)
    // window.
    if (in.now - in.last_progress_at >= in.no_progress_threshold) {
        return {WatchdogDecision::AbortStalled, in.budget_deadline};
    }
    // Budget window expired with progress recent: budget exhaustion, NOT stall
    // evidence. Non-fatal — the caller re-arms the window and keeps observing.
    if (in.now >= in.budget_deadline) {
        return {WatchdogDecision::ReportProgressContinued,
                in.now + in.no_progress_threshold};
    }
    return {WatchdogDecision::Continue, in.budget_deadline};
}

class Watchdog {
public:
    explicit Watchdog(std::chrono::seconds timeout, const PhaseProbe& probe)
        : probe_(&probe),
          timeout_ms_(std::chrono::duration_cast<std::chrono::milliseconds>(timeout)),
          start_(std::chrono::steady_clock::now()) {
        deadline_ = start_ + timeout;
        // The no-progress threshold is the FULL case budget: a genuine stall must
        // freeze progress for the entire budget before the watchdog aborts. This
        // is what distinguishes a real deadlock (progress never resumes) from
        // host-scheduler starvation under parallel-suite load (progress pauses for
        // a few seconds, then resumes). Starvation never reaches a full-budget
        // freeze, so a progressing or briefly-starved case is never aborted,
        // while a real deadlock is (issue #101).
        no_progress_threshold_ = timeout_ms_;
        try {
            thread_ = std::thread([this] { run(); });
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
    // Poll cadence for the progress checkpoints. The no-progress threshold
    // equals the full case budget (set in the constructor): a genuine stall must
    // freeze progress for the ENTIRE budget before the watchdog aborts. Brief
    // host-scheduler starvation under parallel-suite load (seconds, not a full
    // 30s budget) therefore never aborts; a real deadlock (progress never
    // resumes) reaches a full-budget freeze and does.
    static constexpr auto kPollInterval = std::chrono::milliseconds(100);

    void run() noexcept {
        std::unique_lock<std::mutex> lk(mtx_);
        auto last_progress_at = start_;
        auto last_epoch = probe_->progress_epoch.load(std::memory_order_acquire);
        while (true) {
            const bool done = cv_.wait_until(
                lk, std::chrono::steady_clock::now() + kPollInterval, [this] {
                    return done_.load(std::memory_order_acquire);
                });
            if (done) return;
            const auto now = std::chrono::steady_clock::now();
            const auto epoch =
                probe_->progress_epoch.load(std::memory_order_acquire);
            if (epoch != last_epoch) {
                last_epoch = epoch;
                last_progress_at = now;
            }
            // Thin driver over the pure watchdog_decide policy (defined above;
            // proven deterministically in
            // tp_watchdog_decision_policy_controlled_timestamps). A genuine
            // stall (progress frozen >= the full budget) is the ONLY abort
            // trigger; budget exhaustion with progress recent is non-fatal.
            const auto decision = watchdog_decide(
                {now, last_progress_at, deadline_, no_progress_threshold_});
            switch (decision.action) {
            case WatchdogDecision::AbortStalled:
                diagnose_stalled_and_abort(now, last_progress_at);
                // Unreachable: diagnose_stalled_and_abort is [[noreturn]]. The
                // explicit break keeps this case non-fallthrough even if the
                // attribute is ever dropped (a silent fallthrough into
                // ReportProgressContinued would be a policy bug).
                break;
            case WatchdogDecision::ReportProgressContinued:
                // Non-fatal: budget exhausted while progress continued. Emit a
                // diagnostic and re-arm the window (one full budget from now)
                // so the watchdog keeps guarding against a future genuine
                // stall. No abort, no duration increase, no retry, no sleep.
                report_progress_continued(now, last_progress_at);
                deadline_ = decision.next_budget_deadline;
                break;
            case WatchdogDecision::Continue:
                break;
            }
        }
    }

    // Reads ONLY atomics and the immutable name pointer; never touches
    // work_mtx_/arena, so the watchdog cannot deadlock behind the defect. Called
    // ONLY for a genuine no-progress stall (progress frozen >= the no-progress
    // interval). Budget exhaustion under continued progress is reported
    // non-fatally by report_progress_continued (issue #101).
    [[noreturn]] void diagnose_stalled_and_abort(
        std::chrono::steady_clock::time_point now,
        std::chrono::steady_clock::time_point last_progress_at) noexcept {
        const CasePhase ph = probe_->phase.load(std::memory_order_acquire);
        const auto elapsed_since_progress = std::chrono::duration_cast<
            std::chrono::milliseconds>(now - last_progress_at);
        const auto total_elapsed = std::chrono::duration_cast<
            std::chrono::milliseconds>(now - start_);
        std::fprintf(stderr,
                     "ThreadPool test watchdog: GENUINE NO-PROGRESS STALL "
                     "(progress epoch frozen for >= the no-progress interval); "
                     "aborting for diagnostics\n");
        std::fprintf(stderr,
                     "  classification=STALLED (true no-progress evidence)\n");
        std::fprintf(stderr, "  case=%s\n  phase=%s\n",
                     probe_->name ? probe_->name : "?", phase_name(ph));
        std::fprintf(stderr, "  iteration=%llu\n  progress_epoch=%llu\n",
                     static_cast<unsigned long long>(
                         probe_->iteration.load(std::memory_order_acquire)),
                     static_cast<unsigned long long>(
                         probe_->progress_epoch.load(std::memory_order_acquire)));
        std::fprintf(stderr,
                     "  elapsed_since_last_progress=%lldms\n"
                     "  no_progress_interval=%lldms\n"
                     "  total_elapsed=%lldms\n",
                     static_cast<long long>(elapsed_since_progress.count()),
                     static_cast<long long>(no_progress_threshold_.count()),
                     static_cast<long long>(total_elapsed.count()));
        if (probe_->gate_resume != nullptr) {
            std::fprintf(stderr, "  gate: paused=%d resume=%d exited=%d\n",
                         probe_->gate_paused->load(std::memory_order_acquire),
                         probe_->gate_resume->load(std::memory_order_acquire),
                         probe_->gate_exited->load(std::memory_order_acquire));
        }
        if (probe_->gate_paused_at != nullptr) {
            // Issue #110 generation handshake state: a stall with
            // resumed_at >= paused_at > acked_at is a worker parked between
            // its resume and the dequeue-boundary ACK — the exact window the
            // old bool protocol mis-labeled as `exited`.
            std::fprintf(stderr,
                         "  gate: paused_at=%llu resumed_at=%llu acked_at=%llu\n",
                         static_cast<unsigned long long>(
                             probe_->gate_paused_at->load(std::memory_order_acquire)),
                         static_cast<unsigned long long>(
                             probe_->gate_resumed_at->load(std::memory_order_acquire)),
                         static_cast<unsigned long long>(
                             probe_->gate_acked_at->load(std::memory_order_acquire)));
        }
        std::abort();
    }

    // Non-fatal: the case-total budget window expired while progress continued.
    // This is NOT stall evidence and MUST NOT abort (issue #101 model defect).
    // The diagnostic is informational; the budget window is re-armed by the
    // caller so the watchdog keeps observing for a genuine stall.
    void report_progress_continued(
        std::chrono::steady_clock::time_point now,
        std::chrono::steady_clock::time_point last_progress_at) noexcept {
        const CasePhase ph = probe_->phase.load(std::memory_order_acquire);
        const auto elapsed_since_progress = std::chrono::duration_cast<
            std::chrono::milliseconds>(now - last_progress_at);
        const auto total_elapsed = std::chrono::duration_cast<
            std::chrono::milliseconds>(now - start_);
        std::fprintf(stderr,
                     "ThreadPool test watchdog: case-total budget exceeded while "
                     "progress continued (NOT a stall — no abort); re-arming "
                     "budget window (issue #101: case-total deadline is not a "
                     "liveness oracle)\n");
        std::fprintf(stderr,
                     "  classification=PROGRESS CONTINUED (budget exhaustion, "
                     "NOT stall evidence)\n");
        std::fprintf(stderr,
                     "  case=%s phase=%s iteration=%llu progress_epoch=%llu\n",
                     probe_->name ? probe_->name : "?", phase_name(ph),
                     static_cast<unsigned long long>(
                         probe_->iteration.load(std::memory_order_acquire)),
                     static_cast<unsigned long long>(
                         probe_->progress_epoch.load(std::memory_order_acquire)));
        std::fprintf(stderr,
                     "  elapsed_since_last_progress=%lldms total_elapsed=%lldms\n",
                     static_cast<long long>(elapsed_since_progress.count()),
                     static_cast<long long>(total_elapsed.count()));
    }
    std::mutex mtx_;
    std::condition_variable cv_;
    std::chrono::steady_clock::time_point deadline_;
    std::atomic<bool> done_{false};
    std::thread thread_;
    const PhaseProbe* probe_;
    std::chrono::milliseconds timeout_ms_;
    std::chrono::steady_clock::time_point start_;
    std::chrono::milliseconds no_progress_threshold_{};
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

// Issue #110: phase-attributed generation-handshake waits for the
// BeforeWorkerDequeuePauseGate (the blocking predicate loops live in the
// internal-testing header; these wrappers only advance the probe so a
// watchdog stall is attributed to wait_paused vs wait_ack).
void wait_dequeue_paused_gen(ThreadPoolBackend::BeforeWorkerDequeuePauseGate& gate,
                             std::uint64_t generation, PhaseProbe& probe) {
    probe.set(CasePhase::wait_paused);
    wait_dequeue_gate_paused(gate, generation);
    probe.set(CasePhase::inspect);
}

void wait_dequeue_ack_gen(ThreadPoolBackend::BeforeWorkerDequeuePauseGate& gate,
                          std::uint64_t generation, PhaseProbe& probe) {
    probe.set(CasePhase::wait_ack);
    wait_dequeue_gate_ack(gate, generation);
}

// Drain outstanding ops through the real reaper with a bounded total time.
// Uses only poll() and yield() — never a blocking wait_one(), which has no
// timeout and could hang the test (and ultimately the parent waitpid) forever
// if a terminal or ready-wake were lost. Attributes the drain phase so a
// watchdog stall while blocked here is reported as `drain`, not as whatever
// phase preceded it (e.g. inspect).
bool drain_bounded(ThreadPoolBackend& backend,
                   std::chrono::steady_clock::time_point deadline,
                   PhaseProbe& probe) {
    probe.set(CasePhase::drain);
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
// assertion fails. Attributes `resume` before the resume+join and `wait_exited`
// before the (possibly blocking) exited.wait so a watchdog stall is reported by
// the transition that is actually blocked, never as a stale `inspect`.
template <class Gate>
class ScopedGateAndThread {
public:
    ScopedGateAndThread(Gate& gate, std::thread& t, PhaseProbe& probe)
        : gate_(&gate), thread_(&t), probe_(&probe) {}
    void join() {
        if (joined_) return;
        probe_->set(CasePhase::resume);
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
        // Only attribute wait_exited when the wait may actually block: if the
        // production path already left (exited==true) the wait returns
        // immediately and we keep the caller's current phase (e.g. teardown)
        // instead of clobbering it.
        if (!gate_->exited.load(std::memory_order_acquire)) {
            probe_->set(CasePhase::wait_exited);
        }
        gate_->exited.wait(false, std::memory_order_acquire);
    }
    Gate* gate_;
    std::thread* thread_;
    PhaseProbe* probe_;
    bool joined_ = false;
};

// RAII: resume a paused gate on scope exit (for tests without a submitter
// thread) and wait for the production path to leave the gate.  The gate object
// must outlive the backend (declared before it in the test), so no disarm is
// needed — lexical scope guarantees the gate is destroyed after the backend
// and its workers. Attributes `resume` / `wait_exited` like the helper above.
template <class Gate>
class ScopedGateResume {
public:
    ScopedGateResume(Gate& gate, PhaseProbe& probe)
        : gate_(&gate), probe_(&probe) {}
    void resume() {
        if (resumed_) return;
        probe_->set(CasePhase::resume);
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
        // Only attribute wait_exited when the wait may actually block (see
        // ScopedGateAndThread::wait_for_exit).
        if (!gate_->exited.load(std::memory_order_acquire)) {
            probe_->set(CasePhase::wait_exited);
        }
        gate_->exited.wait(false, std::memory_order_acquire);
    }
    Gate* gate_;
    PhaseProbe* probe_;
    bool resumed_ = false;
};

// #93 review follow-up: the watchdog diagnostic-path coverage now runs in a
// FRESH EXEC IMAGE. The parent fork()s and performs ONLY async-signal-safe
// work (close/dup2/execv) before execv; main() dispatches here when argv[1]
// == "--watchdog-diagnostic-child". The old implementation constructed
// PhaseProbe + gate + Watchdog (which spawns a std::thread) as post-fork C++
// work inside the forked image of the multithreaded parent — unsafe under a
// multithreaded/sanitizer runtime. The fresh exec image is single-threaded at
// origin, so constructing the Watchdog here is ordinary program startup. The
// body is otherwise identical to the prior in-child body: declare the gate,
// bind it, start a short-timeout Watchdog, then block without resuming. The
// watchdog fires, reads ONLY the bound gate atomics, prints the
// `gate: paused=...` diagnostic to stderr, and aborts (SIGABRT). It never
// returns; main guards the call with a fallback _Exit.
[[noreturn]] void run_watchdog_diagnostic_child() {
    PhaseProbe probe;
    probe.name = "tp_watchdog_diagnostic_path_reads_bound_gate";
    ThreadPoolBackend::BeforeWorkerDequeuePauseGate gate;
    probe.bind_gate(gate.paused, gate.resume, gate.exited);
    // Deliberately short timeout: this child is constructed to FIRE the
    // diagnostic path. The case-level 30s kWatchdog is intentionally not used
    // — a normal run never reaches diagnose_stalled_and_abort, so a plain TSan
    // run instruments none of its gate-atomic reads; this is the only run that
    // does.
    Watchdog wd(std::chrono::seconds(1), probe);
    // Never resume: the watchdog must fire, read the bound gate atomics, print
    // the diagnostic, and abort. Block until that happens.
    for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(60));
    }
}

// Issue #101 regression child. Constructs a 1s-budget Watchdog and then makes
// STEADY progress — bumping the probe's progress epoch every ~100ms — forever.
// The no-progress threshold equals the FULL 1s budget, so a stall (epoch frozen
// for >= 1s) is never reached; the budget expires repeatedly with progress
// continuing. Pre-fix the watchdog ABORTED at the first budget expiry (a
// case-total wall-clock deadline used as a liveness oracle — the proven #101
// model defect, exercised on this synthetic child); post-fix budget exhaustion
// under continued progress is non-fatal, so this child stays alive indefinitely
// and the parent regression kills it after confirming it did not abort. The
// sleep below is pacing only — it manufactures steady progress for the child;
// the policy property itself is proven with injected timestamps in
// tp_watchdog_decision_policy_controlled_timestamps, not by this wall-clock
// loop (a shorter pacing interval would not help against >1s scheduler
// starvation anyway; only the deterministic policy test is authoritative).
// Fresh exec image (single-threaded at origin), same safe self-exec idiom
// as run_watchdog_diagnostic_child.
[[noreturn]] void run_watchdog_progress_child() {
    PhaseProbe probe;
    probe.name = "tp_watchdog_progress_continued_child";
    Watchdog wd(std::chrono::seconds(1), probe);
    for (;;) {
        probe.progress_epoch.fetch_add(1, std::memory_order_relaxed);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

// Single cleanup authority for the watchdog progress child: SIGKILL it and
// reap it BOUNDED (5s) so a stray child can never outlive the test. Returns
// true only when the reap is confirmed; on failure prints a diagnostic and
// returns false so the caller can fail. Used by every path that ends the
// child — fcntl setup failure, observation error, and the normal success
// cleanup — so no path can leave run_watchdog_progress_child() alive.
bool kill_and_reap_child(pid_t pid) {
    ::kill(pid, SIGKILL);
    int st = 0;
    pid_t w = 0;
    int reap_errno = 0;
    const auto reap_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    for (;;) {
        w = sluice::detail::retry_on_eintr(
            [&] { return ::waitpid(pid, &st, WNOHANG); });
        if (w == pid) return true;
        if (w < 0) {
            reap_errno = errno;
            break;
        }
        if (std::chrono::steady_clock::now() >= reap_deadline) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    std::fprintf(stderr,
                 "failed to reap the killed watchdog progress child "
                 "(waitpid=%ld errno=%d); stray child possible\n",
                 static_cast<long>(w), reap_errno);
    return false;
}

}  // namespace

// Custom main (not SLUICE_MAIN) so the binary can re-exec itself in watchdog
// diagnostic-child mode (see tp_watchdog_diagnostic_path_reads_bound_gate and
// run_watchdog_diagnostic_child). This mirrors the established death-test
// self-exec convention (tests/death_test_runner_posix.hpp): argv selects child
// dispatch; otherwise the normal registered cases run via sluice_test::run_all.
// SLUICE_TEST_FILTER / case registration are unaffected.
int main(int argc, char** argv) {
    if (argc > 1 &&
        std::strcmp(argv[1], "--watchdog-diagnostic-child") == 0) {
        // Fresh exec image: construct the Watchdog and block until it fires.
        run_watchdog_diagnostic_child();  // never returns (aborts via watchdog)
        std::fputs("watchdog diagnostic child: unexpected return from "
                   "run_watchdog_diagnostic_child\n",
                   stderr);
        std::_Exit(1);
    }
    if (argc > 1 &&
        std::strcmp(argv[1], "--watchdog-progress-child") == 0) {
        // Fresh exec image: construct the Watchdog and progress forever. The
        // parent regression observes that this child is NOT aborted.
        run_watchdog_progress_child();  // never returns (loops forever)
        std::_Exit(1);
    }
    return sluice_test::run_all();
}

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
    ScopedGateAndThread arm(gate, submitter, probe);

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
                                   std::chrono::steady_clock::now() + kDrainDeadline,
                                   probe));
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
                            std::chrono::steady_clock::now() + kDrainDeadline,
                            probe);
        if (c.ready()) c.reset();
    }

    probe.set(CasePhase::teardown);
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
    ScopedGateResume guard(gate, probe);

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
                               std::chrono::steady_clock::now() + kDrainDeadline,
                               probe)) {
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
                            std::chrono::steady_clock::now() + kDrainDeadline,
                            probe);
        if (c.ready()) c.reset();
    }

    probe.set(CasePhase::teardown);
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
    ScopedGateResume guard(gate, probe);

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
                               std::chrono::steady_clock::now() + kDrainDeadline,
                               probe)) {
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
                            std::chrono::steady_clock::now() + kDrainDeadline,
                            probe);
        if (c.ready()) c.reset();
    }

    probe.set(CasePhase::teardown);
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
    ScopedGateResume guard(gate, probe);

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
                               std::chrono::steady_clock::now() + kDrainDeadline,
                               probe)) {
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
                            std::chrono::steady_clock::now() + kDrainDeadline,
                            probe);
        if (c.ready()) c.reset();
    }

    probe.set(CasePhase::teardown);
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
    ScopedGateResume guard(gate, probe);
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
                                   std::chrono::steady_clock::now() + kDrainDeadline,
                                   probe)) {
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
                            std::chrono::steady_clock::now() + kDrainDeadline,
                            probe);
        if (c.ready()) c.reset();
    }

    probe.set(CasePhase::teardown);
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
    ScopedGateResume guard(gate, probe);
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
                               std::chrono::steady_clock::now() + kDrainDeadline,
                               probe)) {
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
                            std::chrono::steady_clock::now() + kDrainDeadline,
                            probe);
        if (c.ready()) c.reset();
    }

    probe.set(CasePhase::teardown);
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
                              std::chrono::steady_clock::now() + kDrainDeadline,
                              probe)) {
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
        probe.set(CasePhase::resume);
        resume_threadpool_gate(gate);
        // Wait for the production path to leave the gate before unbinding it.
        probe.set(CasePhase::wait_exited);
        gate.exited.wait(false, std::memory_order_acquire);
    }

    if (fail_msg == nullptr) {
        if (!drain_bounded(backend,
                           std::chrono::steady_clock::now() + kDrainDeadline,
                           probe)) {
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
        probe.set(CasePhase::resume);
        resume_threadpool_gate(gate);
        probe.set(CasePhase::wait_exited);
        gate.exited.wait(false, std::memory_order_acquire);
        (void)drain_bounded(backend,
                            std::chrono::steady_clock::now() + kDrainDeadline,
                            probe);
        if (c.ready()) c.reset();
    }

    probe.set(CasePhase::teardown);
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

    probe.set(CasePhase::submit);
    SLUICE_CHECK(backend.submit_read(ReadOp{fd, buf, 1, 0}, c).has_value());

    const char* fail_msg = nullptr;
    const auto deadline = std::chrono::steady_clock::now() + kDrainDeadline;
    // Wait for the worker to record the terminal (backend_ready), BEFORE any
    // reap runs. This case has no pause gate, so the bounded yield loop is the
    // observation mechanism (AGENTS.md §13.2 permits bounded observation, just
    // not as a correctness proof); inspect attributes it for the watchdog.
    probe.set(CasePhase::inspect);
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
                            std::chrono::steady_clock::now() + kDrainDeadline,
                            probe);
        if (c.ready()) c.reset();
    }

    probe.set(CasePhase::teardown);
    ::close(fd);
    if (fail_msg != nullptr) SLUICE_FAIL(fail_msg);
}

// ---- C2b rows 6/7 (ThreadPool): cancel races the worker terminal winner ----
// Genuine causal two-thread TSan evidence: the generation-scoped
// BeforeWorkerDequeuePauseGate handshake (issue #110) holds the worker in the
// pre-dequeue window on EVERY iteration, so the op is provably `enqueued`
// when the barrier releases. The barrier then releases the canceler and the
// worker-gate resume together, so cancel and dequeue race for the single
// terminal transition under the backend's work_mtx_ arbitration.
// This closes the "worker already finished before the canceler started" hole:
// the race is forced, not probabilistic. Each iteration owns an unambiguous
// causal generation (1..kIters, monotonic, never reset):
//   arm(N) -> submit N -> paused(N) -> [barrier] cancel ‖ resume(N)
//   -> ACK(N) (published only after the worker's pop_front decision for the
//      pausing cycle) -> ONLY THEN arm(N+1)
// The ACK wait closes the #110 cross-iteration hole: the pre-#110 protocol
// treated the gate's `exited` bool as iteration completion, but `exited`
// fires BEFORE pop_front — so after a cancel-won iteration the descheduled
// worker continuation could consume iteration N+1's freshly pushed dispatch
// entry without visiting the re-armed gate, stalling wait_paused forever.
// After ACK(N) the worker continuation can consume another entry only by
// re-entering work_cv_ wait -> gate check -> pop, so iteration N+1 always
// gets its own gate observation. Each iteration asserts the exactly-one
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
    // alive across the watchdog's diagnostic atomic reads. The bool binding
    // stays for legacy diagnostics; the seq triple is the live protocol.
    ThreadPoolBackend::BeforeWorkerDequeuePauseGate gate;
    probe.bind_dequeue_generation_gate(gate.paused_at, gate.resumed_at,
                                       gate.acked_at);
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
    auto cleanup_iteration = [&](Completion<std::size_t>& c,
                                 std::uint64_t gen) noexcept {
        probe.set(CasePhase::resume);
        resume_dequeue_gate_generation(gate, gen);
        // Wait for the ACK only when the worker actually visited this
        // generation: a submit-reject path created no dispatch entry, so no
        // visit — and no ACK — can ever occur for it. When a visit did occur,
        // the resume above guarantees the worker reaches its dequeue decision
        // and publishes the ACK.
        if (gate.paused_at.load(std::memory_order_acquire) >= gen) {
            probe.set(CasePhase::wait_ack);
            wait_dequeue_gate_ack(gate, gen);
        }
        if (!drain_bounded(backend,
                           std::chrono::steady_clock::now() + kDrainDeadline,
                           probe)) {
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
    auto fail_iteration = [&](const char* msg, Completion<std::size_t>& c,
                              std::uint64_t gen) {
        fail_msg = msg;
        cleanup_iteration(c, gen);
    };

    // Highest generation whose pause this test observed; the final safety
    // block waits for ITS ack (a submit-reject iteration never visits).
    std::uint64_t last_paused_gen = 0;
    for (std::size_t iter = 0; iter < kIters && fail_msg == nullptr; ++iter) {
        // Iteration N's causal generation: strictly increasing, never reset.
        const std::uint64_t gen = static_cast<std::uint64_t>(iter) + 1;
        probe.note_iteration(iter);  // progress checkpoint for the watchdog
        Completion<std::size_t> c;
        // Arm the gate for THIS iteration before creating its dispatch entry,
        // so the entry can only be consumed through a visit that observes
        // this generation (the #110 invariant).
        arm_dequeue_gate_generation(gate, gen);
        if (!backend.submit_read(ReadOp{fd, buf, 1, 0}, c).has_value()) {
            // Submit rejected: c is idle, nothing to clean up. The worker
            // never visits gen, so no ACK is expected for it.
            fail_msg = "submit must succeed on a drained backend";
            break;
        }
        // Force the worker into the pre-dequeue window so the op is provably
        // `enqueued` before the race is released.
        wait_dequeue_paused_gen(gate, gen, probe);
        last_paused_gen = gen;
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
            fail_iteration("canceler thread creation failed under load", c, gen);
            break;
        }
        sync.arrive_and_wait();
        probe.set(CasePhase::resume);
        resume_dequeue_gate_generation(gate, gen);
        probe.set(CasePhase::join_canceler);
        canceler.join();
        // #110: wait for the dequeue-boundary ACK of THIS generation before
        // the next iteration may exist. The pre-#110 protocol waited only for
        // the gate `exited` bool (published BEFORE pop_front) and then
        // rearmed — the exact hole that let the N continuation steal N+1's
        // dispatch entry. No rearm step exists anymore: arming N+1 above IS
        // the arm, and it is reachable only after this ACK.
        wait_dequeue_ack_gen(gate, gen, probe);

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
                               c, gen);
                break;
            }
            published += backend.poll();
            if (!c.ready()) std::this_thread::yield();
        }
        if (fail_msg != nullptr) break;

        if (published != 1) {
            fail_iteration("exactly one publication per iteration", c, gen);
            break;
        }
        if (!c.ready()) {
            fail_iteration("Completion must be ready after drain", c, gen);
            break;
        }
        if (c.result().has_value()) {
            if (c.result().value() != 1) {
                fail_iteration("real result must be the 1 seeded byte", c, gen);
                break;
            }
            ++success_total;
        } else {
            if (c.result().error().code != IoError::Code::canceled) {
                fail_iteration("non-success result must be canceled", c, gen);
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

    // Safety: idempotently release any gate visit (kIters is the high-water
    // generation) and wait for the ACK of the highest generation whose pause
    // was observed, so no worker is stranded in a pause before the backend
    // (and its worker thread) is destroyed. The case-level Watchdog catches a
    // genuinely stuck worker.
    //
    // resume(kIters) may exceed the highest generation actually armed (an
    // early break leaves later generations never armed). Safe here and ONLY
    // here, because this is TERMINAL cleanup: no generation is armed after
    // this point, a never-armed generation has no parked worker to release,
    // and the shutdown path returns before the gate check. Do NOT copy this
    // tail-resume into a loop that submits afterwards — resumed_at >= N
    // pre-releases a future gen-N pause (the worker would publish paused_at
    // and proceed without holding), silently destroying the gate's forced
    // pre-dequeue window (see the pre-release constraint on
    // resume_dequeue_gate_generation).
    probe.set(CasePhase::resume);
    resume_dequeue_gate_generation(gate, static_cast<std::uint64_t>(kIters));
    if (last_paused_gen != 0) {
        wait_dequeue_ack_gen(gate, last_paused_gen, probe);
    }

    probe.set(CasePhase::teardown);
    ::close(fd);
    if (fail_msg != nullptr) SLUICE_FAIL(fail_msg);
}

// ---- Issue #110: cross-iteration pause-gate protocol regression -------------
// Deterministic proof of the #110 invariant:
//
//   a generation-N worker continuation cannot consume generation N+1's
//   dispatch entry without generation N+1's gate observation
//
// via the two causal properties of the fixed protocol:
//   1. the dequeue-boundary ACK is HONEST — it is unpublished while the
//      worker is provably pre-pop (held by PostResumePrePopHoldGate in the
//      exact window where the pre-#110 protocol already showed
//      `exited == true`, which is what licensed the theft);
//   2. after ACK(1), arming + submitting generation 2 forces the SAME worker
//      continuation through the gen-2 gate: paused_at reaches 2 while the
//      gen-2 entry is still on the dispatch ring (not consumed pre-gate).
//
// Pre-fix shape (the old bool protocol): after resume the test observed
// exited==true while the worker was still pre-pop, rearmed, and submitted
// N+1; the descheduled continuation then popped N+1 directly (this case's
// dispatch_size would read 0 / wait_paused(gen 2) would stall forever). The
// post-fix protocol makes that shape inexpressible: arming N+1 requires
// wait_dequeue_gate_ack(N) first, and the ACK is published only after the
// pop decision. Every wait below is a blocking atomic wait — no sleep, no
// timeout, no yield loop, no timing assumption. (This libstdc++ exposes
// atomic::wait/notify but not atomic::wait_for; the negative probe therefore
// uses a helper thread blocked in the ACK wait, whose completion is causally
// — not temporally — bounded.)
SLUICE_TEST_CASE(tp_dequeue_gate_generation_blocks_cross_iteration_theft) {
    PhaseProbe probe;
    probe.name = "tp_dequeue_gate_generation_blocks_cross_iteration_theft";
    ThreadPoolBackend::BeforeWorkerDequeuePauseGate gate;
    ThreadPoolBackend::PostResumePrePopHoldGate hold;
    probe.bind_dequeue_generation_gate(gate.paused_at, gate.resumed_at,
                                       gate.acked_at);
    probe.bind_gate(hold.paused, hold.resume, hold.exited);
    Watchdog wd(kWatchdog, probe);
    ThreadPoolBackend backend(ThreadPoolConfig{/*capacity=*/1, /*workers=*/1});
    backend.set_before_dequeue_pause_gate(&gate);
    backend.set_post_resume_pre_pop_hold_gate(&hold);
    sluice::AsyncStats stats;
    backend.attach_stats(&stats);

    TempPath tp("J");
    int fd = open_temp(tp.path());
    const std::byte seed[1] = {std::byte{0xAA}};
    SLUICE_CHECK(::pwrite(fd, seed, 1, 0) == 1);

    std::byte buf[1]{};
    Completion<std::size_t> c1;
    Completion<std::size_t> c2;
    const char* fail_msg = nullptr;
    std::uint64_t syscalls_before = 0;
    // CORE 1b helper: blocked in the gen-1 ACK wait. Its completion is
    // causally bounded — it can finish only after ACK(1), and ACK(1) requires
    // the hold release this thread alone performs — so "not finished before
    // the release" is deterministic, with no timeout and no timing read.
    std::atomic<bool> ack_waiter_done{false};
    std::thread ack_waiter;

    // --- generation 1: cancel wins; the worker is held pre-pop --------------
    arm_dequeue_gate_generation(gate, 1);
    if (!backend.submit_read(ReadOp{fd, buf, 1, 0}, c1).has_value()) {
        fail_msg = "gen-1 submit must succeed";
    } else {
        wait_dequeue_paused_gen(gate, 1, probe);
        syscalls_before = backend.syscall_count_for_test();
        backend.cancel(c1);  // worker held in the gate: enqueued cancel wins
        if (backend.dispatch_size_for_test() != 0) {
            fail_msg = "cancel must remove the gen-1 dispatch entry";
        } else {
            resume_dequeue_gate_generation(gate, 1);
            // The worker is now deterministically parked in the hold — the
            // exact post-resume/pre-pop window (old protocol: exited==true).
            wait_paused(hold, probe);
            // CORE 1 (deterministic): no ACK may exist while the worker is
            // provably pre-pop. Single worker; it is parked in hold.resume;
            // only this thread releases it.
            if (gate.acked_at.load(std::memory_order_acquire) != 0) {
                fail_msg = "no ACK may be published before the dequeue decision";
            } else {
                // CORE 1b: the would-be iteration-2 publisher (a consumer
                // blocked in the ACK wait) cannot proceed while the worker is
                // held. Deterministic by construction: the helper sets its
                // done flag strictly after ACK(1) is observable, and no ACK
                // can exist before the hold release.
                try {
                    ack_waiter = std::thread([&] {
                        wait_dequeue_gate_ack(gate, 1);
                        ack_waiter_done.store(true, std::memory_order_release);
                    });
                } catch (...) {
                    fail_msg = "ack-waiter thread creation failed";
                }
                if (fail_msg == nullptr &&
                    (gate.acked_at.load(std::memory_order_acquire) != 0 ||
                     ack_waiter_done.load(std::memory_order_acquire))) {
                    fail_msg = "the ACK waiter may not finish before the hold release";
                }
            }
        }
    }

    // Release the hold: the worker crosses the pop boundary (ring is empty —
    // cancel removed the gen-1 entry) and publishes ACK(1); only now can the
    // blocked helper finish. The join is causal, not timed.
    probe.set(CasePhase::resume);
    resume_threadpool_gate(hold);
    if (ack_waiter.joinable()) {
        probe.set(CasePhase::wait_ack);
        ack_waiter.join();
    }

    if (fail_msg == nullptr) {
        // The helper's completed join above IS the gen-1 ACK wait.
        // Gen-1 cleanup through the real reaper; exactly one publication.
        probe.set(CasePhase::drain);
        std::size_t published = 0;
        const auto deadline = std::chrono::steady_clock::now() + kDrainDeadline;
        while (!c1.ready()) {
            if (std::chrono::steady_clock::now() >= deadline) {
                fail_msg = "gen-1 drain must complete within the deadline";
                break;
            }
            published += backend.poll();
            if (!c1.ready()) std::this_thread::yield();
        }
        if (fail_msg == nullptr && published != 1) {
            fail_msg = "exactly one publication for gen 1";
        } else if (fail_msg == nullptr && !c1.ready()) {
            fail_msg = "gen-1 Completion must be ready after drain";
        } else if (fail_msg == nullptr &&
                   (c1.result().has_value() ||
                    c1.result().error().code != IoError::Code::canceled)) {
            fail_msg = "gen-1 winner must be canceled";
        } else if (fail_msg == nullptr &&
                   backend.syscall_count_for_test() != syscalls_before) {
            fail_msg = "canceled gen-1 op must not execute a syscall";
        } else if (fail_msg == nullptr && stats.canceled_ops != 1) {
            fail_msg = "gen-1 cancel must tally exactly one canceled op";
        }
        if (fail_msg == nullptr) {
            c1.reset();
        }
    }

    // --- generation 2: the SAME continuation must pass the gen-2 gate -------
    if (fail_msg == nullptr) {
        backend.set_post_resume_pre_pop_hold_gate(nullptr);  // single-use seam
        probe.set(CasePhase::submit);
        arm_dequeue_gate_generation(gate, 2);
        if (!backend.submit_read(ReadOp{fd, buf, 1, 0}, c2).has_value()) {
            fail_msg = "gen-2 submit must succeed";
        } else {
            wait_dequeue_paused_gen(gate, 2, probe);
            // CORE 2 (deterministic): the gen-2 entry is still on the ring AT
            // the gen-2 gate — it was not consumed by the gen-1 continuation
            // (the theft). Under the old protocol this wait_paused stalled
            // forever because no worker would visit the re-armed gate.
            if (backend.dispatch_size_for_test() != 1) {
                fail_msg = "gen-1 continuation must not consume the gen-2 entry";
            } else if (backend.syscall_count_for_test() != syscalls_before) {
                fail_msg = "no syscall may run before the gen-2 release";
            } else if (backend.outstanding() != 1) {
                fail_msg = "the gen-2 op must be the one outstanding request";
            } else {
                resume_dequeue_gate_generation(gate, 2);
                wait_dequeue_ack_gen(gate, 2, probe);
                // Ordinary worker winner from here: the real result verbatim.
                probe.set(CasePhase::drain);
                std::size_t published = 0;
                const auto deadline =
                    std::chrono::steady_clock::now() + kDrainDeadline;
                while (!c2.ready()) {
                    if (std::chrono::steady_clock::now() >= deadline) {
                        fail_msg = "gen-2 drain must complete within the deadline";
                        break;
                    }
                    published += backend.poll();
                    if (!c2.ready()) std::this_thread::yield();
                }
                if (fail_msg == nullptr && published != 1) {
                    fail_msg = "exactly one publication for gen 2";
                } else if (fail_msg == nullptr && !c2.ready()) {
                    fail_msg = "gen-2 Completion must be ready after drain";
                } else if (fail_msg == nullptr &&
                           (!c2.result().has_value() || c2.result().value() != 1)) {
                    fail_msg = "gen-2 real result must win verbatim (1 byte)";
                } else if (fail_msg == nullptr &&
                           backend.syscall_count_for_test() !=
                               syscalls_before + 1) {
                    fail_msg = "exactly one syscall for the gen-2 winner";
                } else if (fail_msg == nullptr && stats.canceled_ops != 1) {
                    fail_msg = "the gen-2 ordinary winner must not tally a cancel";
                } else if (fail_msg == nullptr && backend.outstanding() != 0) {
                    fail_msg = "outstanding must be zero after the gen-2 drain";
                }
                if (fail_msg == nullptr) {
                    c2.reset();
                    if (backend.arena_slot_in_use() != 0) {
                        fail_msg = "slots must be released after both resets";
                    }
                }
            }
        }
    }

    // Tail cleanup (every path): release both gates idempotently, wait for
    // the ACK of the highest generation whose pause was observed, drain, and
    // reset whatever became ready so no Completion destructs outstanding.
    // resume(gate, 2) may exceed the highest armed generation on an early
    // failure path — terminal cleanup only; nothing is armed afterwards (see
    // the pre-release constraint on resume_dequeue_gate_generation).
    probe.set(CasePhase::resume);
    resume_threadpool_gate(hold);
    resume_dequeue_gate_generation(gate, 2);
    const std::uint64_t observed =
        gate.paused_at.load(std::memory_order_acquire);
    if (observed >= 1) {
        wait_dequeue_ack_gen(gate, observed, probe);
    }
    (void)drain_bounded(backend,
                        std::chrono::steady_clock::now() + kDrainDeadline,
                        probe);
    if (c1.ready()) c1.reset();
    if (c2.ready()) c2.reset();

    probe.set(CasePhase::teardown);
    ::close(fd);
    if (fail_msg != nullptr) SLUICE_FAIL(fail_msg);
}

// #93 review follow-up — TSan diagnostic-path coverage. A normal (green) run
// never fires the case-level Watchdog, so diagnose_stalled_and_abort() — and its
// reads of the bound gate atomics — are never executed; a plain TSan run
// therefore instruments none of those reads. This case FORCES the diagnostic
// path: the parent fork()s and re-execs THIS binary in child mode
// (--watchdog-diagnostic-child; see run_watchdog_diagnostic_child), so the
// Watchdog is constructed in a FRESH EXEC IMAGE (single-threaded at origin) —
// never as post-fork C++ work in the multithreaded parent image. The child
// blocks without resuming; the watchdog fires, reads the bound gate atomics,
// prints the `gate: paused=` line, and aborts. The parent asserts the child was
// terminated by SIGABRT AND that the `gate:` line was printed (proving the
// diagnostic branch actually dereferenced the bound gate atomics). Under TSan
// this is the only run that exercises those reads, so a race on them would be
// reported here rather than hidden behind a green normal run. The parent
// captures the child's stderr with a BOUNDED poll()/read() (read via the
// repository retry_on_eintr helper; a permanent read/poll error is recorded and
// fails the test rather than accepting partial output) and reaps with a bounded
// WNOHANG loop; on timeout or capture failure it kills + reaps the child and
// fails, so a stuck child can never hang the test on an unbounded
// read()/waitpid(). POSIX only.
SLUICE_TEST_CASE(tp_watchdog_diagnostic_path_reads_bound_gate) {
    // Resolve this binary's path in the PARENT, BEFORE fork, via the shared
    // portable self-exec resolver (Linux: readlink /proc/self/exe; macOS:
    // _NSGetExecutablePath). readlink/_NSGetExecutablePath are not guaranteed
    // async-signal-safe, so the post-fork child performs ONLY async-signal-safe
    // calls (close/dup2/execv/_Exit). Matches the self-exec idiom in
    // tests/death_test_runner_posix.hpp (which also resolves before fork).
    std::string self_path = sluice_death_test::resolve_self_executable_path();
    if (self_path.empty()) {
        SLUICE_FAIL("self-executable path resolution failed; cannot exec child");
    }

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
        // Child: ONLY async-signal-safe work before execv — no C++ runtime, no
        // object construction, no thread creation in this forked image of the
        // multithreaded parent (#93). Redirect stderr to the pipe (dup2 survives
        // exec; fd 2 is not close-on-exec), close the rest, then re-exec THIS
        // binary in child mode. The fresh exec image (run_watchdog_diagnostic_
        // child, dispatched from main) constructs the Watchdog and blocks; the
        // watchdog fires, reads the bound gate atomics, prints the `gate:`
        // diagnostic to stderr (the pipe), and aborts. _Exit(127) is the exec
        // convention for "exec failed".
        ::close(pipefd[0]);
        if (::dup2(pipefd[1], STDERR_FILENO) < 0) std::_Exit(127);
        ::close(pipefd[1]);
        char* child_argv[] = {self_path.data(),
                              const_cast<char*>("--watchdog-diagnostic-child"),
                              nullptr};
        ::execv(child_argv[0], child_argv);
        std::_Exit(127);  // execv failed
    }

    // Parent: close the write end so a poll/read sees EOF when the child
    // aborts (its dup'd stderr is closed on SIGABRT).
    ::close(pipefd[1]);

    // BOUNDED diagnostic capture. poll() uses a deadline; its own EINTR is
    // handled by `continue` so remaining_ms is recomputed each iteration
    // (wrapping poll in retry_on_eintr would re-pass a stale timeout). read()
    // goes through the repository sluice::detail::retry_on_eintr helper. A
    // permanent (non-EINTR) poll or read error is RECORDED and fails the test —
    // partial captured output is never silently accepted. The child is built to
    // fire its 1s watchdog and abort, so this generous bound is a safety net,
    // not a correctness proof.
    constexpr auto kChildDeadline = std::chrono::seconds(10);
    const auto read_deadline =
        std::chrono::steady_clock::now() + kChildDeadline;
    std::string captured;
    bool pipe_timed_out = false;
    bool capture_failed = false;
    int capture_errno = 0;
    for (;;) {
        struct ::pollfd pfd;
        pfd.fd = pipefd[0];
        pfd.events = POLLIN;
        pfd.revents = 0;
        const auto remaining_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                read_deadline - std::chrono::steady_clock::now())
                .count();
        if (remaining_ms <= 0) {
            pipe_timed_out = true;
            break;
        }
        int pr = ::poll(&pfd, 1, static_cast<int>(remaining_ms));
        if (pr < 0) {
            if (errno == EINTR) continue;   // recompute remaining_ms at top
            capture_failed = true;          // poll blew up: record, do not hide
            capture_errno = errno;
            break;
        }
        if (pr == 0) {
            pipe_timed_out = true;
            break;                          // deadline exceeded without EOF
        }
        char buf[256];
        ssize_t rd = sluice::detail::retry_on_eintr(
            [&] { return ::read(pipefd[0], buf, sizeof(buf)); });
        if (rd > 0) {
            captured.append(buf, static_cast<std::size_t>(rd));
        } else if (rd == 0) {
            break;                          // EOF: child closed/aborted stderr
        } else {
            capture_failed = true;          // permanent read error: record + fail
            capture_errno = errno;
            break;
        }
    }
    ::close(pipefd[0]);

    // BOUNDED reap. The child either already aborted (SIGABRT) or, if capture
    // timed out or failed, is about to be killed (SIGKILL). A WNOHANG poll loop
    // with a deadline ensures even a pathologically stuck child cannot hang the
    // parent on an unbounded blocking waitpid. waitpid's own EINTR is handled by
    // retry_on_eintr. If the reap deadline itself expires with the child still
    // uncollectable, SIGKILL and perform a final bounded reap so no diagnostic
    // child can escape cleanup.
    if (pipe_timed_out || capture_failed) {
        ::kill(pid, SIGKILL);
    }
    int status = 0;
    pid_t w = 0;
    const auto reap_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    for (;;) {
        w = sluice::detail::retry_on_eintr(
            [&] { return ::waitpid(pid, &status, WNOHANG); });
        if (w == pid) break;
        if (w < 0) break;                   // error (e.g. ECHILD); fail below
        // w == 0: child not yet collectable.
        if (std::chrono::steady_clock::now() >= reap_deadline) {
            // Deadline expired with the child still uncollectable: SIGKILL
            // (idempotent; harmless if already dead) and a final bounded reap so
            // no diagnostic child can escape cleanup.
            ::kill(pid, SIGKILL);
            const auto kill_reap_deadline =
                std::chrono::steady_clock::now() + std::chrono::seconds(5);
            for (;;) {
                w = sluice::detail::retry_on_eintr(
                    [&] { return ::waitpid(pid, &status, WNOHANG); });
                if (w == pid || w < 0) break;
                if (std::chrono::steady_clock::now() >= kill_reap_deadline) {
                    w = 0;
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    const char* fail_msg = nullptr;
    if (capture_failed) {
        std::fprintf(stderr,
                     "watchdog diagnostic pipe capture failed: errno=%d (%s)\n",
                     capture_errno, std::strerror(capture_errno));
        fail_msg = "watchdog diagnostic pipe capture failed (non-EINTR error)";
    } else if (pipe_timed_out) {
        fail_msg =
            "watchdog diagnostic child did not close stderr within the bound";
    } else if (w != pid) {
        fail_msg = "waitpid for the watchdog diagnostic child failed/timed out";
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

// Issue #101 regression — end-to-end wiring of the pure decision policy.
// tp_watchdog_decision_policy_controlled_timestamps proves the POLICY itself
// with controlled timestamps; this case proves the real loop (thread + poll +
// fork + exec wiring) does not abort a progressing child. The forked child
// (--watchdog-progress-child) bumps its progress epoch every 100ms under a 1s
// watchdog budget, so the budget expires repeatedly with progress continuing.
// This case observes the child for ~3s (3x the child's budget): if the
// watchdog model is correct the child STAYS ALIVE (budget exhaustion under
// continued progress is non-fatal) and this case passes; pre-fix the watchdog
// aborted the child by SIGABRT at the first (~1s) budget expiry and this case
// FAILED. Fail-closed: a waitpid error during observation, an un-reaped child
// after SIGKILL, or a failed pipe drain setup is a test FAILURE, never a false
// green. The parent's sleeps pace the WNOHANG poll only (diagnosis, AGENTS.md
// §13.3); child liveness is established by waitpid, and the policy property is
// proven with injected time, not by this wall-clock window. The child's
// non-fatal progress-continued diagnostics are routed to a pipe and drained
// (expected output, not a failure). POSIX only.
SLUICE_TEST_CASE(tp_watchdog_does_not_abort_on_continued_progress) {
    std::string self_path = sluice_death_test::resolve_self_executable_path();
    if (self_path.empty()) {
        SLUICE_FAIL("self-executable path resolution failed; cannot exec child");
    }

    int pipefd[2];
    if (::pipe(pipefd) != 0) {
        SLUICE_FAIL("pipe() failed for watchdog progress-child regression");
    }

    pid_t pid = ::fork();
    if (pid < 0) {
        ::close(pipefd[0]);
        ::close(pipefd[1]);
        SLUICE_FAIL("fork() failed for watchdog progress-child regression");
    }

    if (pid == 0) {
        // Child: async-signal-safe only before execv. Redirect stderr to the
        // pipe (dup2 survives exec) and re-exec in progress-child mode.
        ::close(pipefd[0]);
        if (::dup2(pipefd[1], STDERR_FILENO) < 0) std::_Exit(127);
        ::close(pipefd[1]);
        char* child_argv[] = {self_path.data(),
                              const_cast<char*>("--watchdog-progress-child"),
                              nullptr};
        ::execv(child_argv[0], child_argv);
        std::_Exit(127);  // execv failed
    }

    // Parent: close the write end, then observe the child for ~3s (3x its 1s
    // budget). Drain the pipe so the child's expected progress-continued
    // diagnostics cannot fill the pipe buffer and block it. The observed
    // property is child LIVENESS via waitpid (an OS observation); the sleep
    // only paces the WNOHANG poll.
    ::close(pipefd[1]);
    // Non-blocking read end: drain the child's expected progress-continued
    // diagnostics without ever blocking on an open pipe (the child is alive by
    // design, so a blocking read would never see EOF and would stall the loop).
    int rd_flags = ::fcntl(pipefd[0], F_GETFL);
    if (rd_flags < 0 ||
        ::fcntl(pipefd[0], F_SETFL, rd_flags | O_NONBLOCK) < 0) {
        // Fail-closed: without a reliable non-blocking drain we cannot keep
        // observing. Kill + reap the child before reporting the failure.
        const int save_errno = errno;
        ::close(pipefd[0]);
        kill_and_reap_child(pid);
        std::fprintf(stderr,
                     "fcntl(O_NONBLOCK) failed (errno=%d) while setting up the "
                     "watchdog progress-child pipe drain\n",
                     save_errno);
        SLUICE_FAIL("pipe drain setup failed for watchdog progress-child "
                    "regression");
    }
    constexpr auto kObserve = std::chrono::seconds(3);
    const auto observe_deadline =
        std::chrono::steady_clock::now() + kObserve;
    int status = 0;
    bool died_early = false;
    bool observe_error = false;
    int observe_errno = 0;
    int early_termsig = 0;
    while (std::chrono::steady_clock::now() < observe_deadline) {
        // Drain any pending diagnostic bytes (discard). Fail-closed: only
        // EAGAIN/EWOULDBLOCK (non-blocking pipe drained) and EOF (child closed
        // its stderr — the waitpid check below classifies an early exit) end
        // the drain; any OTHER read error makes the observation untrustworthy
        // and fails the case (never a false green).
        char drain[256];
        for (;;) {
            const ssize_t n = sluice::detail::retry_on_eintr(
                [&] { return ::read(pipefd[0], drain, sizeof(drain)); });
            if (n > 0) continue;
            if (n == 0) break;  // EOF: drained; child's stderr closed
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;  // drained
            observe_error = true;
            observe_errno = errno;
            break;
        }
        if (observe_error) break;
        pid_t w = sluice::detail::retry_on_eintr(
            [&] { return ::waitpid(pid, &status, WNOHANG); });
        if (w == pid) {
            died_early = true;
            if (WIFSIGNALED(status)) early_termsig = WTERMSIG(status);
            break;
        }
        if (w < 0) {
            // Fail-closed: an observation error is NOT a pass. We can no longer
            // trust the child's state, so report the failure instead of falling
            // through to the kill/reap success branch (a false green).
            observe_error = true;
            observe_errno = errno;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    ::close(pipefd[0]);

    const char* fail_msg = nullptr;
    if (observe_error) {
        // Fail-closed: the observation error already means the child's state
        // cannot be verified; kill + reap it so the progress child cannot
        // remain alive and hang the test.
        kill_and_reap_child(pid);
        std::fprintf(stderr,
                     "watchdog progress-child observation failed (errno=%d); "
                     "child state cannot be verified\n",
                     observe_errno);
        fail_msg = "watchdog progress-child observation error";
    } else if (died_early) {
        std::fprintf(stderr,
                     "watchdog progress child died early (termsig=%d); the "
                     "watchdog must NOT abort a case making steady progress "
                     "(issue #101)\n",
                     early_termsig);
        fail_msg =
            "watchdog must not abort on continued progress (issue #101)";
    } else {
        // Success: child survived the whole observation window (budget
        // exhaustion under continued progress is non-fatal). Kill + reap it
        // bounded so a stray child cannot hang the test — and FAIL if the reap
        // cannot be confirmed.
        if (!kill_and_reap_child(pid)) {
            fail_msg = "watchdog progress child not reaped after SIGKILL";
        }
    }
    if (fail_msg != nullptr) SLUICE_FAIL(fail_msg);
}

// Issue #101 regression — the watchdog decision policy, proven DETERMINISTICALLY
// with controlled timestamps (no sleeps, no processes, no threads). Directly
// proves the two load-bearing policy properties the model-defect fix must have:
//   budget expires + progress recent      -> ReportProgressContinued (NO abort)
//   no progress >= full-budget threshold  -> AbortStalled (the ONLY abort)
// plus the continuous-stall check (frozen progress is caught before any window
// expires), the inclusive threshold boundary, and that a re-armed window
// (ReportProgressContinued) does not lose the stall guard. AGENTS.md §13.3:
// sleep_for must not prove liveness — here nothing is slept; every input is an
// explicit timestamp. The fork-based regression above only wires this policy
// through the real loop.
SLUICE_TEST_CASE(tp_watchdog_decision_policy_controlled_timestamps) {
    using clock = std::chrono::steady_clock;
    using ms = std::chrono::milliseconds;
    const auto t0 = clock::time_point{};  // arbitrary origin; pure arithmetic
    const auto kBudget = ms(1000);
    const auto kThreshold = kBudget;  // no-progress threshold == full case budget

    auto dec = [&](auto now, auto last, auto deadline) {
        return watchdog_decide({now, last, deadline, kThreshold});
    };

    // (1) Progress recent, budget not expired -> Continue.
    auto r = dec(t0 + ms(500), t0 + ms(400), t0 + ms(1000));
    if (r.action != WatchdogDecision::Continue) {
        SLUICE_FAIL("budget not expired + progress recent must Continue");
    }

    // (2) Budget expires with progress recent -> ReportProgressContinued (the
    //     #101 defect class: MUST NOT abort), and the window re-arms to one
    //     full budget from `now`.
    r = dec(t0 + ms(1000), t0 + ms(900), t0 + ms(1000));
    if (r.action != WatchdogDecision::ReportProgressContinued) {
        SLUICE_FAIL("budget expired + progress recent must NOT be a stall");
    }
    if (r.next_budget_deadline != t0 + ms(2000)) {
        SLUICE_FAIL("re-arm must extend the window by one full budget");
    }

    // (3) Zero progress for the full budget (window expired) -> AbortStalled.
    r = dec(t0 + ms(1000), t0, t0 + ms(1000));
    if (r.action != WatchdogDecision::AbortStalled) {
        SLUICE_FAIL("full-budget freeze at window expiry must abort");
    }

    // (4) Frozen progress reaches the threshold BEFORE the window expires —
    //     the stall check runs continuously, not at the window end.
    r = dec(t0 + ms(1500), t0 + ms(500), t0 + ms(3000));
    if (r.action != WatchdogDecision::AbortStalled) {
        SLUICE_FAIL("frozen progress must abort before the window expires");
    }

    // (5) Threshold boundary is inclusive: exactly one budget elapsed -> stall.
    r = dec(t0 + ms(1000), t0, t0 + ms(3000));
    if (r.action != WatchdogDecision::AbortStalled) {
        SLUICE_FAIL("exactly one budget of frozen progress must abort");
    }

    // (6) Just below the threshold -> not a stall yet.
    r = dec(t0 + ms(999), t0, t0 + ms(3000));
    if (r.action != WatchdogDecision::Continue) {
        SLUICE_FAIL("sub-threshold freeze must not abort");
    }

    // (7) After a re-arm (window now ends at t0+2000ms), progress freezes at
    //     t0+1000ms: at the new window end the freeze has reached the full
    //     budget -> AbortStalled. The re-arm must not lose the stall guard.
    r = dec(t0 + ms(2000), t0 + ms(1000), t0 + ms(2000));
    if (r.action != WatchdogDecision::AbortStalled) {
        SLUICE_FAIL("re-armed window must still catch a genuine stall");
    }
}
