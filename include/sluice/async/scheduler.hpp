// sluice::async::Scheduler — multi-worker Evented scheduler (sluice-CORE-E7).
//
// E7-A: worker-local execution state + multi-worker run skeleton.
// Each Worker owns its own scheduler Context + current Fiber (E7-C1). Fibers
// are pinned to their first-execution Worker (E7-C2). Wake routing + MW
// coordination are added in E7-B/E7-C.
//
// See docs/adr/ADR-execution-model.md §9.2 for the accepted E7 contract.
#pragma once

#include <sluice/async/async_io_context.hpp>
#include <sluice/async/completion.hpp>
#include <sluice/async/fiber.hpp>
#include <sluice/async/fiber_ctx.hpp>

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

namespace sluice::async {

// Per-worker execution state (E7-C1). Each Worker thread owns one of these.
// The scheduler Context + current Fiber are NEVER shared across concurrent
// Workers. Accessed by the owning Worker thread without locks for the
// execution-state fields; the inbox is concurrency-safe for cross-worker
// publication (E7-B).
struct WorkerState {
    fiber_ctx::Context sched_ctx{};   // this worker's saved scheduler continuation
    Fiber* current = nullptr;          // the Fiber currently running on this worker
    std::deque<Fiber*> local_runnable{};  // owner-local runnable queue
    unsigned id = 0;

    // Cross-worker routed inbox (E7-B will populate; E7-A leaves empty).
    // Protected by inbox_mtx for cross-worker publication.
    std::mutex inbox_mtx;
    std::deque<Fiber*> inbox;
    std::condition_variable inbox_cv;
    std::atomic<bool> active{false};  // this worker is part of a coordinated run

    WorkerState() = default;
    WorkerState(const WorkerState&) = delete;
    WorkerState& operator=(const WorkerState&) = delete;
};

class Scheduler {
public:
    explicit Scheduler(AsyncIoContext& ctx) noexcept;
    ~Scheduler();

    Scheduler(const Scheduler&) = delete;
    Scheduler& operator=(const Scheduler&) = delete;
    Scheduler(Scheduler&&) = delete;
    Scheduler& operator=(Scheduler&&) = delete;

    // Initialize a Fiber's context so the scheduler can run it. The test owns
    // the stack; this wires the fiber's ctx to that stack + the bridge + &fiber.
    bool init_fiber(Fiber& fiber, std::byte* stack_base, std::size_t stack_size);

    // Enqueue a Fiber as runnable. Round-robin assignment to a Worker (E7-B
    // refines). E7-A: assigns to worker 0 (single-worker compatible).
    void spawn(Fiber& fiber) noexcept;

    // Run the scheduler with `worker_count` worker threads until global idle
    // (E7 coordinated run). `worker_count` must be >= 1. With 1 worker, this
    // is the single-worker path (E4-E6 compatible).
    void run(unsigned worker_count);

    // Legacy single-worker entry point (E4-E6 compatibility). Delegates to
    // run(1).
    void run_until_idle() { run(1); }

    // ---- E5/E6 suspension primitives (called from Fiber bodies) ----
    void await_completion_size(Completion<std::size_t>& c);
    void await_completion_void(Completion<void>& c);
    void await_ready_flag(const std::atomic<bool>& ready);

    // ---- Diagnostics ----
    std::size_t runnable_count() const;
    std::size_t waiting_count() const {
        std::lock_guard<std::mutex> lk(global_mtx_);
        return waiting_size_.size() + waiting_void_.size() + waiting_ready_.size();
    }
    std::size_t waiting_ready_count() const {
        std::lock_guard<std::mutex> lk(global_mtx_);
        return waiting_ready_.size();
    }

private:
    // Wait registration with owner Worker (E7-B will use owner; E7-A stores
    // the Fiber only).
    struct WaitReg {
        Fiber* fiber;
        WorkerState* owner;
    };

    bool wake_ready_completions_locked();
    bool wake_ready_flags_locked();
    void route_runnable(Fiber* f, WorkerState* owner);
    void route_runnable_locked(Fiber* f, WorkerState* owner);
    void worker_loop(WorkerState* ws);
    void run_next_on(WorkerState* ws, Fiber* fiber);

    // Get the current Worker's WorkerState (worker-local via TLS).
    static WorkerState* current_worker();

    AsyncIoContext& ctx_;

    // Global coordination state (protected by global_mtx_).
    mutable std::mutex global_mtx_;
    std::unordered_map<void*, WaitReg> waiting_size_{};
    std::unordered_map<void*, WaitReg> waiting_void_{};
    std::unordered_map<const std::atomic<bool>*, WaitReg> waiting_ready_{};

    // Global runnable queue for pre-start assignment (E7-A; E7-B will make this
    // per-worker). Fibers assigned here are picked up by workers in FIFO order.
    std::deque<Fiber*> pending_spawn_{};
    unsigned next_spawn_worker_ = 0;

    // Worker state storage. Owned by the Scheduler (address-stable for the
    // Scheduler's lifetime — wait registrations and inboxes reference these).
    std::vector<std::unique_ptr<WorkerState>> workers_;

    // Run coordination state.
    std::atomic<unsigned> active_worker_count_{0};
    std::atomic<unsigned> running_fiber_count_{0};
    std::condition_variable global_idle_cv_;
    bool in_coordinated_run_ = false;
};

}  // namespace sluice::async
