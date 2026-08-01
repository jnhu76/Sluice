// sluice::async::ThreadPoolBackend (sluice-CORE-020A, ADR §4 Option 5).
//
// The portable, always-buildable REAL async backend: runs the existing blocking
// pread/pwrite/fdatasync/fsync on worker threads. This is the FALLBACK where
// io_uring is absent (ADR §4) and provides the ThreadPool row that 022's bench
// compares. Reuses Result<T>/IoError verbatim.
//
// Threading model: one worker thread per outstanding op (simple, correct; the
// high-concurrency path is UringAsyncBackend, job 020B). Each submitted op
// spawns a worker that performs the blocking syscall and pushes a terminal
// result into a ready queue. poll() drains the ready queue (marking
// Completions ready) and JOINS the worker that produced each drained result;
// wait_one() blocks on a condition variable until >=1 ready and then polls.
// Joining at reap time keeps the number of unreaped (zombie) kernel threads
// bounded by the number of outstanding ops instead of growing with the total
// op count — a high-op-count copy (e.g. ~200k ops per buffer-size round) must
// not accumulate ~200k unreaped threads, which exhausts kernel task-count
// limits (RLIMIT_NPROC / threads-max) on standard Linux kernels. The
// destructor joins any remaining in-flight workers, so the captured `this`
// stays valid for every worker (workers never outlive the backend).
//
// Buffer lifetime (L1-L3c): the worker reads/writes the caller's buffer; the
// caller MUST keep it alive + address-stable until the Completion is ready
// (same rule as the rest of async). The backend does NOT copy buffers.
//
// Cancel (ADR §7 X2): best-effort and asynchronous. This backend spawns one
// worker thread per submitted op, so an op is "started" essentially at submit.
// Portable cancel of an in-flight blocking syscall is deferred (it would need
// pthread_kill/tgkill — a portability hazard). Therefore cancel here records a
// cancel request and the op completes with its real result when the syscall
// returns (exactly-once, ADR X3). The terminal result after cancel is one of
// {success, error, canceled} — defined, never stuck outstanding. Cancellation
// that actually interrupts the syscall is the Uring backend's job (020B/026).
//
// Shutdown gate: once destruction begins (or shutting_down_for_test flips the
// gate), submit_* returns invalid_state synchronously instead of spawning a
// worker that could not be joined (was dead state before 025 B2; now enforced).
//
// Spawn-failure safety: if a worker thread cannot be spawned (resource
// exhaustion — std::thread construction throws), the op is resolved as a
// terminal backend_error instead of letting the exception escape submit_*.
// The reaper (poll/wait_one) completes the Completion with that error and
// the outstanding_ accounting stays balanced, so a failed spawn is an op
// error — never a hang, a leaked outstanding op, or an exception crossing a
// public submit boundary.
//
// No new dependency (std::thread/mutex/condition_variable only — ADR §11 D4).
// State is instance-owned (no globals, gate item 6).
#pragma once

#include <sluice/async/async_io_context.hpp>
#include <sluice/async/completion.hpp>
#include <sluice/detail/posix_retry.hpp>
#include <sluice/error.hpp>
#include <sluice/result.hpp>

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace sluice::async {

class ThreadPoolBackend : public AsyncBackend {
public:
    ThreadPoolBackend() = default;
    ~ThreadPoolBackend() override;

    Result<void> submit_read(ReadOp op, Completion<std::size_t>& c) override;
    Result<void> submit_write(WriteOp op, Completion<std::size_t>& c) override;
    Result<void> submit_sync_data(SyncDataOp op, Completion<void>& c) override;
    Result<void> submit_sync_all(SyncAllOp op, Completion<void>& c) override;

    std::size_t poll() override;
    Result<std::size_t> wait_one() override;

    void cancel(Completion<std::size_t>& c) override;
    void cancel(Completion<void>& c) override;

    std::size_t outstanding() const noexcept override;

    // Test-only hook: flips the shutdown gate WITHOUT running the destructor
    // (the destructor path is unsafe to test directly: use-after-free on the
    // backend object). Used by the 025 B2 contract tests to verify submit-
    // after-shutdown-begun returns invalid_state. The leading underscore marks
    // it as internal; production code never calls it.
    void shutting_down_for_test();

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
    // Test-only: count of spawned worker threads that have not yet been
    // joined (reaped) by poll()/wait_one(). A method-only seam — no member
    // data, so production object layout is unchanged. Regression for the
    // Version B CI failure: unreaped workers must stay bounded by outstanding
    // ops, not grow with total ops.
    std::size_t unjoined_workers_for_test() const {
        std::lock_guard<std::mutex> lk(mtx_);
        std::size_t n = 0;
        for (const auto& w : workers_)
            if (w.joinable())
                ++n;
        return n;
    }
#endif

private:
    // A pending op's ready form. The work callable runs on a worker thread; on
    // return it pushes a terminal result into the matching ready_ deque. `worker`
    // is the index of the std::thread object in workers_ that produced this
    // result, so the reaper can join exactly that worker once the result is
    // drained; spawn-failure entries (no thread was created) carry kNoWorker.
    struct ReadySize {
        Completion<std::size_t>* c;
        Result<std::size_t> r;
        std::size_t worker;
    };
    struct ReadyVoid {
        Completion<void>* c;
        Result<void> r;
        std::size_t worker;
    };

    // Sentinel worker index: the op never spawned a thread (spawn failure).
    static constexpr std::size_t kNoWorker = static_cast<std::size_t>(-1);

    // Enqueue a job: record outstanding + spawn worker. Caller has already
    // verified accepting_new_work() and c.idle().
    void enqueue_size(Completion<std::size_t>& c, std::function<Result<std::size_t>()> work);
    void enqueue_void(Completion<void>& c, std::function<Result<void>()> work);

    // Resolve an op whose worker-thread spawn failed (resource exhaustion):
    // push a terminal error into the ready queue so the reaper completes the
    // Completion and balances the outstanding_ accounting. The alternative —
    // letting the exception escape submit_* — would be swallowed by the
    // Runtime task boundary and hang the caller (the copy task's done_cv
    // wait), so a failed spawn must surface as an op ERROR, not an exception.
    void fail_spawn_size(Completion<std::size_t>* c, const IoError& err);
    void fail_spawn_void(Completion<void>* c, const IoError& err);

    // True if the backend will accept a new submitted op (not shutting down).
    // Centralizes the destroying_ gate so every submit_* enforces it. Reads
    // destroying_ under mtx_; the lock provides the happens-before edge to the
    // destructor's write (CP.20).
    bool accepting_new_work() const;

    // Take the worker thread at `index` out of workers_ so the caller can
    // join it OUTSIDE mtx_ (joining inside the lock would stall every other
    // submit and worker publication for the whole thread teardown and invites
    // lock-ordering bugs). MUST be called with mtx_ held. Returns a non-
    // joinable thread when the index is out of range or the slot was already
    // taken — each worker is moved out exactly once, right after its result
    // is reaped. The moved-out slot becomes a non-joinable placeholder, so
    // vector indices stay stable and the destructor's join loop skips it.
    std::thread take_worker_for_join_locked(std::size_t index);

    mutable std::mutex mtx_;
    std::condition_variable cv_;
    std::deque<ReadySize> ready_size_;
    std::deque<ReadyVoid> ready_void_;
    std::vector<std::thread> workers_;     // joined in destructor
    std::size_t outstanding_ = 0;
    bool destroying_ = false;
};

}  // namespace sluice::async
