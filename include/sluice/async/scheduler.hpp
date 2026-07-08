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

// Forward declaration — narrow test hook for E7-T11 deterministic admission
// race. Defined only in test TUs; exposes no public Scheduler contract.
struct SchedulerTestHooks;

// E9 external wake handle (ADR §9.4.10). A generation-validated, weak handle
// that an EXTERNAL producer thread holds so it can wake a parked Scheduler
// Worker without holding a raw Scheduler* (which would be use-after-free
// across Scheduler destruction).
//
// Contract:
//   - Issued by Scheduler::make_wake_handle(); the producer owns the instance.
//   - notify() is safe to call from any thread, including AFTER the issuing
//     Scheduler has been destroyed (it becomes a no-op via the weak control
//     block). It advances the Scheduler wake epoch and notifies wake_cv_.
//   - The producer MUST NOT call any other Scheduler method via this handle.
//   - The producer MUST NOT touch Scheduler queues, registrations, or Fibers.
//
// Refinement map: TLA+ ExternalReadyPublish (the signal half) ->
// SchedulerWakeHandle::notify -> Scheduler::signal_wake_locked.
class SchedulerWakeHandle {
public:
    SchedulerWakeHandle() = default;
    // Not copyable (the control block identity is the wake identity).
    SchedulerWakeHandle(const SchedulerWakeHandle&) = delete;
    SchedulerWakeHandle& operator=(const SchedulerWakeHandle&) = delete;
    SchedulerWakeHandle(SchedulerWakeHandle&&) noexcept = default;
    SchedulerWakeHandle& operator=(SchedulerWakeHandle&&) noexcept = default;

    // Signal the bound Scheduler to wake a parked Worker. No-op if the
    // Scheduler is dead. Returns true if a wake was delivered.
    bool notify() noexcept;

    // Is this handle currently bound to a live Scheduler?
    bool bound() const noexcept;

private:
    friend class Scheduler;
    struct Control;  // shared control block (defined in scheduler.cpp)
    explicit SchedulerWakeHandle(std::shared_ptr<Control> ctrl) noexcept
        : control_(std::move(ctrl)) {}
    std::shared_ptr<Control> control_;
};

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

    // E9 park-admission per-worker state (ADR §9.4.2 / §9.4.5).
    // observed_epoch is the wake_epoch_ value observed at the instant this
    // worker COMMITTED to park (recorded under wake_mtx_). The cv.wait
    // predicate is "wake_epoch != observed_epoch OR wake-worthy persistent
    // state OR terminate" — this closes the commit-to-physical-wait window.
    // park_domain records which domain this worker is parked in (SCHEDULER
    // vs BACKEND) for diagnostics + the at-most-one-backend-participant rule.
    std::uint64_t observed_epoch{0};
    enum class ParkDomain : unsigned char { None, Scheduler, Backend };
    ParkDomain park_domain{ParkDomain::None};

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

    // E8 test seam: spawn `fiber` directly onto worker `worker_id`'s
    // local_runnable. Narrow deterministic-test hook (mirrors the E7-T11
    // admission seam discipline); exposes no public production contract.
    // Used by E8 tests to place a victim on a specific worker without the
    // round-robin nondeterminism of spawn(). Records the owner.
    void spawn_on(Fiber& fiber, unsigned worker_id) noexcept;

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

    // ---- E9 external wake source (ADR §9.4) ----
    // Issue a generation-validated wake handle. The holder may call notify()
    // from an external thread to wake a parked Worker whose wake set includes
    // the Scheduler wake source. Safe across Scheduler destruction. The handle
    // is bound to this Scheduler and invalidated (notify becomes a no-op) on
    // destruction.
    SchedulerWakeHandle make_wake_handle();

    // Attach an external wake handle to the currently-registered ready-flag
    // wait on `ready` (the Fiber that just suspended via await_ready_flag).
    // After this, an external producer's notify() on `wh` will wake a parked
    // Scheduler Worker, which will re-drain and route the now-ready Fiber.
    // Called by the suspending Fiber AFTER the registration is visible.
    // `wh` must outlive the wait registration (caller-owned, ADR §9.4.10).
    void attach_ready_wake(const std::atomic<bool>& ready,
                           SchedulerWakeHandle& wh);

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

    // E8 diagnostic (§12): the id of the Worker the caller is currently
    // running on (TLS g_worker), or -1 if not on a worker thread. Safe to
    // call from a Fiber body. Used by tests to record which worker executed
    // a Fiber without racing a non-atomic std::thread::id across threads.
    static unsigned current_worker_id() {
        WorkerState* w = current_worker();
        return w ? w->id : static_cast<unsigned>(-1);
    }

    // E8 diagnostic (§12): the CURRENT execution owner of `f`, or nullptr if
    // `f` has not been spawned/assigned. Test/DEBUG only — do not make
    // runtime policy depend on this. Returns the WorkerState* that owns `f`
    // at this instant (which may differ from the initial assignment if `f`
    // was stolen).
    WorkerState* owner_of(const Fiber& f) const {
        std::lock_guard<std::mutex> lk(global_mtx_);
        auto it = fiber_owner_.find(const_cast<Fiber*>(&f));
        return it == fiber_owner_.end() ? nullptr : it->second;
    }
    unsigned owner_id_of(const Fiber& f) const {
        WorkerState* o = owner_of(f);
        return o ? o->id : static_cast<unsigned>(-1);
    }

private:
    friend struct SchedulerTestHooks;  // E7-T11 deterministic admission seam
    friend class SchedulerWakeHandle;  // E9: notify() -> notify_external_wake

    // Wait registration with owner Worker (E7-B will use owner; E7-A stores
    // the Fiber only).
    struct WaitReg {
        Fiber* fiber;
        WorkerState* owner;
    };

    // E7-C fixup: explicit global MW-state classification (ADR §9.2.6).
    // Physical run termination may occur for both MW_S3_UNRESOLVED and
    // QUIESCENT, but they are LOGICALLY distinct: MW_S3 retains unresolved
    // wait registrations and must never be reported as quiescence.
    enum class MwState {
        mw_s1,              // runnable/running work exists
        mw_s2,              // no runnable/running; ≥1 backend op outstanding
        mw_s3_unresolved,   // no runnable/running; no backend op outstanding; wait registration(s) remain
        quiescent,          // nothing remains
    };

    // E7-C fixup: two-phase MW-S2 admission state machine. An atomic bool is
    // insufficient (check-to-block race). NONE → CANDIDATE (under global_mtx_,
    // after Phase-A drain+classify) → COMMITTED (after Phase-B re-drain+
    // reclassify still shows MW-S2). route_runnable_locked demotes COMMITTED
    // or CANDIDATE back to NONE (new runnable work cancels admission).
    enum class AdmissionState : unsigned {
        none,
        candidate,
        committed,
    };

    bool wake_ready_completions_locked();
    bool wake_ready_flags_locked();
    void route_runnable(Fiber* f, WorkerState* owner);
    void route_runnable_locked(Fiber* f, WorkerState* owner);
    void worker_loop(WorkerState* ws);
    void run_next_on(WorkerState* ws, Fiber* fiber);

    // E8: try to steal one runnable Fiber from another worker's local_runnable
    // to `thief`. Returns true if a Fiber was stolen (and now sits on
    // thief->local_runnable with ownership transferred). Called WITHOUT
    // global_mtx_ held; acquires it internally. Steal is MOVE + OWNER
    // TRANSFER — it never calls make_runnable (the fiber is already Runnable)
    // and never publishes a second ticket. See ADR §9.3 + the TLA+ model.
    bool try_steal(WorkerState* thief);

    // Classify global MW state. Must be called with global_mtx_ held.
    // Uses the AUTHORITATIVE backend outstanding count (ctx_.outstanding()),
    // NOT scheduler wait-map size — a backend op may be outstanding without
    // a current Scheduler wait registration, and a wait registration may
    // exist with no outstanding backend op (MW-S3). ctx_.outstanding()
    // acquires access_mtx_ internally; global_mtx_→access_mtx_ is the
    // accepted lock order.
    MwState classify_locked() const;

    // Test seam for E7-T11 (deterministic MW-S2 admission race). When set,
    // the worker that reaches Phase-B commit pauses BEFORE the final
    // reclassify+commit decision until the seam is released by the test.
    // Exposes no public contract; private, used by a friend test.
    bool admission_seam_armed_ = false;
    std::mutex admission_seam_mtx_;
    std::condition_variable admission_seam_cv_;
    bool admission_seam_paused_ = false;

    // Get the current Worker's WorkerState (worker-local via TLS).
    static WorkerState* current_worker();

    AsyncIoContext& ctx_;

    // Global coordination state (protected by global_mtx_).
    mutable std::mutex global_mtx_;
    std::unordered_map<void*, WaitReg> waiting_size_{};
    std::unordered_map<void*, WaitReg> waiting_void_{};
    std::unordered_map<const std::atomic<bool>*, WaitReg> waiting_ready_{};

    // E8: the RUNNABLE ownership / steal-consistency record for each Fiber
    // that has been spawned (protected by global_mtx_). It records which
    // Worker's local_runnable queue currently holds the Fiber's runnable
    // ticket. Writers: spawn / spawn_on / run() distribute (initial owner),
    // and try_steal (victim -> thief). Readers: the steal eligibility check
    // in try_steal (verify the victim still owns the stealable ticket) and
    // the owner_of/owner_id_of test diagnostics.
    //
    // It is NOT read by any wake/route path: wake_ready_*_locked route by
    // WaitReg.owner (captured as g_worker at suspend time). It is NOT
    // updated by await_* (the wait-epoch resume owner is captured in
    // WaitReg.owner, not here). See ADR §9.3.5.1 + docs/e8-formal-corrective.
    // For E7-pinned Fibers this is write-once (spawn sets it; no steal
    // occurs) and is behaviorally identical to E7; E8 only diverges on steal.
    std::unordered_map<Fiber*, WorkerState*> fiber_owner_{};

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
    std::atomic<unsigned> idle_workers_{0};
    std::atomic<bool> global_terminate_{false};
    std::condition_variable global_idle_cv_;
    bool in_coordinated_run_ = false;

    // E7-C fixup: MW-S2 admission coordination state (protected by
    // global_mtx_). admission_ transitions NONE→CANDIDATE→COMMITTED under
    // global_mtx_; route_runnable_locked demotes to NONE. Only the COMMITTED
    // participant may enter ctx_.wait_one(), and only AFTER releasing
    // global_mtx_.
    AdmissionState admission_{AdmissionState::none};
    unsigned admission_owner_ = static_cast<unsigned>(-1);  // worker id of candidate/committed

    // ---- E9 wake source (ADR §9.4.5) ----
    // The unified Scheduler wake source. wake_epoch_ is a monotonically
    // non-decreasing counter advanced by every wake-relevant producer
    // (route_runnable_locked, notify_external_wake, termination). wake_cv_ is
    // the physical delivery. Persistent state (runnable/ready) is the
    // authority; the epoch closes the commit-to-physical-wait window.
    // wake_mtx_ is the cv mutex; it is acquired by signal_wake_locked and by
    // the SCHEDULER-domain park. Lock order: global_mtx_ may be held while
    // acquiring wake_mtx_ (signal_wake_locked does so); the reverse is never
    // done.
    std::mutex wake_mtx_;
    std::condition_variable wake_cv_;
    std::uint64_t wake_epoch_{0};

    // The wake control block, weak-referenced by every issued
    // SchedulerWakeHandle so a post-destruction notify() is a safe no-op.
    std::shared_ptr<SchedulerWakeHandle::Control> wake_control_;

    // Issue / deliver a wake. signal_wake_locked acquires wake_mtx_
    // internally; safe to call under global_mtx_. Advances the epoch and
    // notifies wake_cv_. Idempotent + coalescing-safe (multiple signals collapse).
    void signal_wake_locked();

    // Wake entry for external producers (via SchedulerWakeHandle). Acquires
    // wake_mtx_ internally. No-op if the Scheduler is terminating.
    void notify_external_wake() noexcept;

    // Park the current Worker on the SCHEDULER wake domain (wake_cv_ with a
    // bounded timeout, defense-in-depth). Records observed_epoch_ under
    // wake_mtx_ before sleeping. Returns when wake-worthy state is observed.
    // ws is the calling worker. Called with global_mtx_ RELEASED.
    void park_on_wake_source(WorkerState* ws);

    // Predicate: is there currently an external-wake-capable wait registered
    // (a ready-flag wait)? When true, the MW-S2 participant must NOT enter a
    // backend-only ctx_.wait_one() — it parks on the SCHEDULER domain instead
    // (the MIXED-WAKE fix, ADR §9.4.7). Call with global_mtx_ held.
    bool external_wake_possible_locked() const {
        return !waiting_ready_.empty();
    }
};

}  // namespace sluice::async
