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
#include <sluice/async/lock_guard.hpp>
#include <sluice/async/mutex.hpp>
#include <sluice/async/thread_annotations.hpp>
#include <sluice/async/timer_registration.hpp>
#include <sluice/async/wait_node.hpp>
#include <sluice/async/wait_queue.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <list>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

namespace sluice::async {

// Forward declaration — narrow test hook for E7-T11 deterministic admission
// race. Defined only in test TUs; exposes no public Scheduler contract.
struct SchedulerTestHooks;

// Forward declaration — E9-CORRECTIVE deterministic park seams (ADR §9.4.15).
// Defined only in the E9 test TU; accesses private park-seam members.
struct E9ParkSeamHooks;

// Forward declaration — E11 deterministic clock/timer test seams (M7). Defined
// only in the E11 test TU; enables the controllable monotonic clock so race
// proofs never use sleep_for as causal proof.
struct E11TimerTestHooks;

// E9 external wake handle (ADR §9.4.10). A control-block-backed handle that
// an EXTERNAL producer thread holds so it can wake a parked Scheduler Worker
// without holding a raw Scheduler* (which would be use-after-free across
// Scheduler destruction).
//
// Contract:
//   - Issued by Scheduler::make_wake_handle(); the producer owns the instance.
//   - notify() is safe to call from any thread, including AFTER the issuing
//     Scheduler has been destroyed (it becomes a no-op). The shared Control
//     block outlives the Scheduler (the Scheduler and every handle hold a
//     shared_ptr); the destructor flips alive=false under Control::mtx.
//   - notify() holds Control::mtx (the CALLBACK LEASE) from the validity
//     check through the Scheduler wake callback, so destruction cannot
//     interleave with an in-flight callback (E9-LIFETIME-CORRECTIVE).
//   - The producer MUST NOT call any other Scheduler method via this handle.
//   - The producer MUST NOT touch Scheduler queues, registrations, or Fibers.
//
// Refinement map: TLA+ ExternalReadyPublish (the signal half) ->
// SchedulerWakeHandle::notify -> Scheduler::signal_wake_locked. Callback
// lifetime: docs/spec/e9_wake_handle_lifetime/.
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

    // ---- E9-LIFETIME-CORRECTIVE deterministic test seam (spec 13) ----
    // TEST-ONLY. Defined in scheduler.cpp (where Control is complete).
    // Arm/pause/release the notify callback at the exact boundary: validated
    // + lease held, immediately before notify_external_wake. The seam does
    // NOT modify Scheduler state; it only blocks the notifier thread so the
    // test can prove the destructor cannot progress while the lease is held.
    void lifetime_seam_arm() noexcept;
    void lifetime_seam_wait_paused() noexcept;
    bool lifetime_seam_is_paused() const noexcept;
    void lifetime_seam_release() noexcept;

private:
    friend class Scheduler;
    struct Control;  // shared control block (defined in scheduler.cpp)
    explicit SchedulerWakeHandle(std::shared_ptr<Control> ctrl) noexcept
        : control_(std::move(ctrl)) {}
    std::shared_ptr<Control> control_;
};

// E9-CORRECTIVE: Run invocation lifetime contract (ADR §9.4.0). RunMode is
// an EXPLICIT invocation policy, separate from wake capability. It is the
// ONLY axis along which the idle action differs; the classifier,
// publication protocol, ownership protocol, and backend admission are
// shared (one worker loop, one classifier).
//
//   drain — E7/E8 compatibility. MW-S3 returns STALLED; the run invocation
//           MUST NOT park merely because an external-wake-capable wait
//           exists. Existing callers and E7/E8 tests use Drain.
//   live  — explicit E9 entry. The run remains resident while an
//           unresolved wait has an effective Scheduler wake source
//           (MW-S3 + external-wake-capable may park). No effective wake
//           source still returns STALLED.
//
// A wake handle NEVER implicitly switches Drain <-> Live (E9-LIFE-6).
enum class RunMode : unsigned char {
    drain,
    live,
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
    // (E7 coordinated run, DRAIN mode). `worker_count` must be >= 1. With 1
    // worker, this is the single-worker path (E4-E6 compatible). Drain
    // semantics (ADR §9.4.0): MW-S3 returns STALLED; the run does NOT park
    // merely because an external-wake-capable wait exists.
    void run(unsigned worker_count);

    // E9-CORRECTIVE: explicit LIVE entry. Same worker loop and classifier as
    // run(); differs ONLY at the idle-action selection boundary. In Live, an
    // unresolved MW-S3 wait with an effective Scheduler wake source may keep
    // the run resident (park). MW-S3 without an effective wake source still
    // returns STALLED. Used by E9-T1..T14 (the no-re-entry external-wake
    // proof). See ADR §9.4.0 / §9.4.3.
    void run_live(unsigned worker_count);

    // Legacy single-worker entry point (E4-E6 compatibility). Delegates to
    // run(1) (Drain).
    void run_until_idle() { run(1); }

    // ---- E5/E6 suspension primitives (called from Fiber bodies) ----
    void await_completion_size(Completion<std::size_t>& c);
    void await_completion_void(Completion<void>& c);
    void await_ready_flag(const std::atomic<bool>& ready);

    // ---- E10 WaitQueue suspension (sluice-CORE-E10) ----
    // Suspend the calling Fiber on `q`, registering `node`. `node` must be
    // Detached (fresh) and outlive this call's suspend until it is resumed
    // (woken or cancelled). The fiber resumes when EXACTLY ONE resolver wins:
    //   - wake_wait_one(q) resolves the head with Woken, OR
    //   - cancel_wait(q, node) resolves this node with Cancelled.
    // The winner transition is the WaitNode resolve CAS (one authority, §2/§7);
    // the loser performs no second wake. Cancellation here is wait-cancellation
    // ONLY (not task / I/O cancellation).
    //
    // FIBER LIFETIME (E10-CORRECTIVE Sec.6). `await_wait` is called by the
    // running fiber itself (me = ws->current); the node is registered with that
    // fiber as its handle, and the fiber then suspends INSIDE this call
    // (make_waiting + context_switch). The fiber cannot return from await_wait
    // — and therefore cannot reach done, and its caller cannot destroy it —
    // until a resolver wins, which resolves+unlinks the node in the SAME
    // critical section before routing the fiber runnable. Hence a Fiber
    // registered via await_wait can never be destroyed while its WaitNode is
    // Registered: the Scheduler owns no Fiber objects (they are caller-owned
    // raw pointers), never destroys a Fiber on run termination, and a fiber may
    // be destroyed by its caller only after it reaches done (by which point the
    // node is already terminal+unlinked). [FIBER_WAIT_LIFETIME:
    //  PROTECTED_BY_EXISTING_OWNERSHIP]
    //
    // Registration protocol (§4 lost-wake window): mirrors await_ready_flag —
    // register + recheck + make_waiting are ONE atomic transition w.r.t. the
    // wake path (wake_wait_one_locked runs under global_mtx_); only
    // context_switch is outside. If the wait is already resolvable at register
    // time (e.g. a wake was already delivered), the registration is undone and
    // the fiber does NOT suspend.
    void await_wait(WaitQueue& q, WaitNode& node);

    // Resolve the head of `q` with Woken and route the winner's fiber through
    // the canonical wake seam (route_runnable_locked). Returns true iff a wait
    // was actually woken by THIS call (exactly one runnable enqueue). Called by
    // a waker (typically a Fiber or external producer). Safe to call any time.
    bool wake_wait_one(WaitQueue& q);

    // Resolve `node` (registered in `q`) with Cancelled and route the winner's
    // fiber. Returns true iff this call won (the node was Registered and is now
    // Cancelled). A losing call (node already woken) is a no-op. E10 cancel is
    // wait-cancellation only: it does NOT cancel the task/fiber/I/O op.
    bool cancel_wait(WaitQueue& q, WaitNode& node);


    // ---- E10/E11 boundary: deadline / timer wait (sluice-CORE-E11) ----
    // The E11 Deadline type: a monotonic absolute time point. The protocol
    // authority is the absolute deadline (`expired iff now >= deadline`); a
    // relative duration is converted to a deadline by the caller via
    // `monotonic_now() + duration`. Wall-clock time never participates.
    using deadline_t = deadline_tick_t;

    // Read the Scheduler's monotonic clock. Production: steady_clock ticks
    // since process start. Tests: a controllable logical clock advanced by
    // advance_clock() (M7 deterministic causal seam).
    deadline_t monotonic_now() const noexcept;

    // TEST-ONLY causal seam (M7): advance the controllable logical clock to
    // `t` and pump any due timers. No-op in production mode (the clock runs
    // on steady_clock). In test mode this is the deterministic timer driver:
    // it advances time and resolves due deadlines through expire_wait, WITHOUT
    // sleep_for.
    void advance_clock(deadline_t t);

    // Suspend the calling Fiber on `q`, registering `node` with a monotonic
    // absolute `deadline`. The wait resolves when EXACTLY ONE cause wins the
    // resolve_ CAS:
    //   - wake_wait_one(q)         -> Woken   (RESOURCE_WAKE)
    //   - cancel_wait(q, node)     -> Cancelled (CANCEL)
    //   - the deadline elapsing     -> Expired  (TIMER_EXPIRE)
    // The winner is the WaitNode resolve CAS (one authority); a losing cause
    // performs no second wake. `node` must be Detached (fresh) and outlive
    // this call's suspend until it is resumed.
    //
    // A deadline already due at admission MUST NOT strand the fiber: under the
    // admission critical section, after registration the deadline is rechecked
    // and, if due, resolved as Expired through the same resolve_ authority
    // before suspension (I5 admission closure). The fiber never suspends
    // merely because timer registration happened after the deadline was due.
    //
    // A TimerRegistration control block (independently-stable retirement
    // state) is created for this wait epoch and owned by the Scheduler. A
    // non-timer winner retires it in the SAME critical section that resolves
    // the node, before runnable publication — so a stale expiry cannot
    // dereference the destroyed WaitNode (I4 lifetime closure).
    void await_wait_deadline(WaitQueue& q, WaitNode& node, deadline_t deadline);

    // Resolve `node` (registered in `q`) with Expired and route the winner's
    // fiber. The E11 third resolution cause, reached when the bound deadline
    // elapses. Returns true iff this call won (the node was Registered and is
    // now Expired). A losing call (node already woken/cancelled, or retired)
    // is a no-op. Mirrors wake_wait_one / cancel_wait exactly:
    // global_mtx_ + q.mtx() -> resolve_(Expired) -> unlink_locked ->
    // --waiting_waitq_count_ -> make_runnable + route_runnable_locked. NEVER
    // a parallel timer-wake publication path.
    bool expire_wait(WaitQueue& q, WaitNode& node);


    // ---- E12-A Event wait admission / broadcast (sluice-CORE-E12-A) ----
    // The Event persistent-readiness substrate. Event owns a persistent
    // std::atomic<bool> set_ + a WaitQueue waiters_; these seams implement the
    // lost-set admission closure and set-all broadcast under global_mtx_ (the
    // existing serialization domain). No second ready-flag map, no Event-private
    // timer, no Event-private cancellation winner, no direct Fiber manipulation.
    //
    // Synchronization domain: ALL Event seams take global_mtx_ (and q.mtx()
    // inside it), the same domain await_wait / wake_wait_one / cancel_wait /
    // expire_wait / pump_deadlines_locked use. This serializes set linearization,
    // reset linearization, wait admission, and set's drain boundary, making
    // OLD_SET_WAKES_POST_RESET_WAITER mechanically impossible (no generation
    // counter needed).

    // Transition `set_flag` to SET and attempt RESOURCE_WAKE resolution for
    // every currently registered Event wait epoch in `waiters`. Each winner is
    // resolved through the canonical path (wake_wait_one_locked: resolve_(Woken)
    // + unlink + retire timer + dec count + make_runnable + route). Idempotent:
    // set() on SET is a no-op (the store is a no-op; the drain finds the queue
    // in whatever state the registered waiters left it). Returns the number of
    // waiters resolved by THIS call. Safe to call from an external OS thread.
    std::size_t event_set_broadcast(WaitQueue& waiters, std::atomic<bool>& set_flag);

    // Transition `set_flag` to UNSET. Does NOT resolve, cancel, expire, unlink,
    // or publish any WaitNode. A waiter already registered remains governed by
    // future set(), deadline, or cancellation. Linearized under global_mtx_.
    void event_reset(std::atomic<bool>& set_flag);

    // Suspend the calling Fiber on `q`, registering `node`, with the Event
    // readiness predicate `set_flag`. The admission closure: register + check
    // SET + (if SET) resolve Woken inline through wake_node_locked, OR commit
    // suspension. Mirrors await_wait_deadline's I5 already-due path. `node` must
    // be Detached (fresh) and outlive this call's suspend until it is resumed.
    // Result via node.outcome() (woken / cancelled).
    void await_event_wait(WaitQueue& q, const std::atomic<bool>& set_flag,
                          WaitNode& node);

    // Deadline-aware Event wait. Composes await_event_wait with E11
    // TimerRegistration. The wait resolves when EXACTLY ONE cause wins the
    // resolve_ CAS: set() broadcast (Woken), cancel_wait (Cancelled), or the
    // deadline elapsing (Expired). If set_ is observed at admission, resolves
    // Woken inline (no suspend). If the deadline is already due at admission,
    // the E11 I5 path resolves Expired inline (no suspend). The deadline
    // registration is retired by a non-timer winner in the same CS (E11 I4).
    void await_event_wait_deadline(WaitQueue& q, const std::atomic<bool>& set_flag,
                                   WaitNode& node, deadline_t deadline);


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
        LockGuard lk(global_mtx_);
        return waiting_size_.size() + waiting_void_.size() + waiting_ready_.size() +
               waiting_waitq_count_;
    }
    std::size_t waiting_ready_count() const {
        LockGuard lk(global_mtx_);
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
        LockGuard lk(global_mtx_);
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
    friend struct E9ParkSeamHooks;     // E9-CORRECTIVE park seams (T3/T4)
    friend struct E11TimerTestHooks;   // E11 deterministic clock/timer seams

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

    bool wake_ready_completions_locked() SLUICE_REQUIRES(global_mtx_);
    bool wake_ready_flags_locked() SLUICE_REQUIRES(global_mtx_);
    void route_runnable(Fiber* f, WorkerState* owner) SLUICE_REQUIRES(global_mtx_);
    void route_runnable_locked(Fiber* f, WorkerState* owner) SLUICE_REQUIRES(global_mtx_);
    // E12-A: the wake_wait_one body with global_mtx_ already held. Resolves the
    // FIFO head with Woken (wake_one_locked), retires any bound timer, decrements
    // waiting_waitq_count_, and routes the winner runnable. Returns the winning
    // node (nullptr if empty or head lost). Used by the public wake_wait_one AND
    // event_set_broadcast's drain loop. Caller MUST hold global_mtx_.
    WaitNode* wake_wait_one_locked(WaitQueue& q) SLUICE_REQUIRES(global_mtx_);
    void worker_loop(WorkerState* ws);
    // E9-CORRECTIVE: one internal run implementation parameterized by
    // RunMode. The worker loop reads run_mode_ to select the idle action
    // (ADR §9.4.0). Drain and Live share the SAME loop, classifier, and
    // publication/ownership protocols.
    void run_impl(unsigned worker_count, RunMode mode);
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
    MwState classify_locked() const SLUICE_REQUIRES(global_mtx_);

    // Test seam for E7-T11 (deterministic MW-S2 admission race). When set,
    // the worker that reaches Phase-B commit pauses BEFORE the final
    // reclassify+commit decision until the seam is released by the test.
    // Exposes no public contract; private, used by a friend test.
    bool admission_seam_armed_ = false;
    std::mutex admission_seam_mtx_;
    std::condition_variable admission_seam_cv_;
    bool admission_seam_paused_ = false;

    // E9-CORRECTIVE deterministic park seams (ADR §9.4.15). Two narrow
    // TEST-only hooks at the load-bearing causal boundaries of park
    // admission, replacing sleep-based race proofs (M7):
    //   park_candidate_seam_  — pauses a Worker right after it reaches
    //                           ParkCandidate (Phase-B recheck boundary).
    //   park_commit_seam_     — pauses a Worker immediately before the
    //                           physical wait (commit boundary).
    // They only pause the Worker at the exact boundary; they do NOT
    // modify Scheduler state. The test releases the seam only after the
    // producer has published readiness + signaled the wake epoch.
    bool park_candidate_seam_armed_ = false;
    bool park_commit_seam_armed_ = false;
    std::mutex park_seam_mtx_;
    std::condition_variable park_seam_cv_;
    bool park_seam_candidate_paused_ = false;
    bool park_seam_commit_paused_ = false;

    // Get the current Worker's WorkerState (worker-local via TLS).
    static WorkerState* current_worker();

    AsyncIoContext& ctx_;

    // Global coordination state (protected by global_mtx_).
    mutable Mutex global_mtx_;
    std::unordered_map<void*, WaitReg> waiting_size_ SLUICE_GUARDED_BY(global_mtx_){};
    std::unordered_map<void*, WaitReg> waiting_void_ SLUICE_GUARDED_BY(global_mtx_){};
    std::unordered_map<const std::atomic<bool>*, WaitReg> waiting_ready_ SLUICE_GUARDED_BY(global_mtx_){};

    // E10: count of fibers suspended on a WaitQueue via await_wait (protected
    // by global_mtx_). Incremented on registration, decremented on resolution
    // (wake_wait_one winner OR cancel_wait winner). Counted by classify_locked
    // exactly like the other wait maps so MW-S3 (unresolved waits) is correct.
    // We track a COUNT (not a per-node map) because a WaitQueue owns its own
    // intrusive node list; the scheduler only needs to know SOME E10 wait is
    // unresolved for MW classification, and the per-node fiber is recovered
    // from the winning node at resolution time.
    std::size_t waiting_waitq_count_ SLUICE_GUARDED_BY(global_mtx_){0};


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
    std::unordered_map<Fiber*, WorkerState*> fiber_owner_ SLUICE_GUARDED_BY(global_mtx_){};

    // Global runnable queue for pre-start assignment (E7-A; E7-B will make this
    // per-worker). Fibers assigned here are picked up by workers in FIFO order.
    std::deque<Fiber*> pending_spawn_ SLUICE_GUARDED_BY(global_mtx_){};
    unsigned next_spawn_worker_ SLUICE_GUARDED_BY(global_mtx_) = 0;

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

    // E9-CORRECTIVE: the current run invocation's lifetime policy. Set by
    // run_impl before worker_loop starts; read by worker_loop at the idle-
    // action selection boundary. Stable for the duration of a run (a run
    // invocation has ONE mode). Plain bool-storage is safe: it is written
    // once before any worker thread starts and only read thereafter.
    RunMode run_mode_{RunMode::drain};

    // E7-C fixup: MW-S2 admission coordination state (protected by
    // global_mtx_). admission_ transitions NONE→CANDIDATE→COMMITTED under
    // global_mtx_; route_runnable_locked demotes to NONE. Only the COMMITTED
    // participant may enter ctx_.wait_one(), and only AFTER releasing
    // global_mtx_.
    AdmissionState admission_ SLUICE_GUARDED_BY(global_mtx_){AdmissionState::none};
    unsigned admission_owner_ SLUICE_GUARDED_BY(global_mtx_) = static_cast<unsigned>(-1);  // worker id of candidate/committed

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
    Mutex wake_mtx_;
    std::condition_variable_any wake_cv_;
    std::uint64_t wake_epoch_ SLUICE_GUARDED_BY(wake_mtx_){0};

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
    // E11: the bounded timeout is min(default wake poll, earliest-deadline-now)
    // so an active deadline cannot park a Worker indefinitely past it (I6).
    // TSA-SUPPRESS-001: uses std::unique_lock + cv.wait (release-wait-reacquire
    // pattern).  Clang TSA cannot track unique_lock capability semantics through
    // condition_variable::wait.  The production lock fact (wake_epoch_ is
    // protected by wake_mtx_) is already independently accepted and proven in E9.
    void park_on_wake_source(WorkerState* ws) SLUICE_NO_THREAD_SAFETY_ANALYSIS;

    // ---- E11 timer / deadline subsystem (sluice-CORE-E11) ----
    // The deadline container is a binary min-heap over TimerRegistration*,
    // keyed by deadline. Pointer-stable storage (std::list) so a registration's
    // identity survives heap sift operations and outlives any bound WaitNode.
    //
    // Reclamation contract (proven by e11_t18, F3-corrected): logical
    // retirement (ACTIVE->RETIRED) is IMMEDIATE (the non-timer winner performs
    // it in the same CS as the resolve CAS); lifetime safety is IMMEDIATE (the
    // atomic `state` gate makes try_claim_expiry/retire observe RETIRED/CONSUMED
    // WITHOUT dereferencing a possibly-destroyed node — I4); but PHYSICAL
    // reclamation (erase from heap + pool) is LAZY-AT-DEADLINE — the pump pops
    // an entry only when `now >= its deadline`, regardless of state, and erases
    // the pool block then. Consequently a far-future RETIRED entry remains
    // physically in the heap+pool until its original deadline is reached. The
    // pool size is therefore bounded by (concurrent ACTIVE deadline waits) +
    // (retired/consumed entries whose deadlines have not yet been reached); it
    // is NOT bounded solely by concurrent waits, and "unbounded growth fixed"
    // must not be claimed absolutely. No inert block lingers past its OWN
    // deadline. Wheel-compaction / eager removal is deferred to E15 and is NOT
    // added unless a reviewer proves a mandatory resource-bound violation.
    //
    // All heap state is protected by global_mtx_ (the Scheduler coordination
    // domain): the heap is mutated only by await_wait_deadline (register),
    // pump_deadlines_locked (expiry), and the retire path (state flip under
    // the same CS as the resolve CAS). The clock is atomic so a worker can read
    // it outside the lock for the park-timeout computation.
    std::list<TimerRegistration> timer_pool_ SLUICE_GUARDED_BY(global_mtx_){};
    std::vector<TimerRegistration*> deadline_heap_ SLUICE_GUARDED_BY(global_mtx_){};

    // O(1) count of ACTIVE timer registrations. Incremented/decremented
    // alongside every Active ↔ {Retired, Consumed} state transition under
    // global_mtx_. Replaces an O(pool) scan in any_active_deadline_locked().
    std::size_t active_deadline_count_ SLUICE_GUARDED_BY(global_mtx_){0};

    // The monotonic clock. Production: ticks since process start on
    // steady_clock. Tests: a logical clock advanced deterministically by
    // advance_clock() (the timer driver). test_clock_mode_ selects the source.
    //
    // Both are std::atomic and carry NO GUARDED_BY: the clock is read LOCK-FREE
    // by clock_now_unlocked() / pump_deadlines_locked() (the park-timeout calc
    // and the timer driver), which intentionally do not always hold global_mtx_.
    // This matches the established pattern for the other atomic run-coordination
    // fields (global_terminate_, active_worker_count_): atomic storage is the
    // synchronization authority, not a mutex capability. Writes still serialize
    // under global_mtx_ in practice (advance_clock / the worker loop), but the
    // reads are lock-free and acquire/release-ordered on the atomic itself.
    std::atomic<deadline_t> clock_{0};
    std::atomic<bool> test_clock_mode_{false};

    // Atomic cache of the earliest ACTIVE deadline, or kNoDeadline if none.
    // Maintained under global_mtx_ by recompute_earliest_deadline_locked()
    // (called after every heap/pool mutation), but read LOCK-FREE by
    // park_on_wake_source to bound the wake wait WITHOUT taking global_mtx_
    // (avoids a wake_mtx_->global_mtx_ lock-order inversion — I6 without
    // re-opening a park-side global_mtx_ acquisition). A stale read is safe: it
    // only makes the park timeout slightly too long/short; the worker loop's
    // pump_deadlines_locked re-establishes the authoritative deadline set under
    // global_mtx_ on every iteration, so liveness (I6) is preserved even if the
    // cache briefly lags.
    static constexpr deadline_t kNoDeadline =
        static_cast<deadline_t>(-1);
    std::atomic<deadline_t> earliest_active_deadline_{kNoDeadline};

    // Helper: read the clock WITHOUT global_mtx_ (for the park-timeout calc).
    // In production returns steady_clock ticks; in test mode returns clock_.
    deadline_t clock_now_unlocked() const noexcept;

    // Recompute earliest_active_deadline_ from the pool (under global_mtx_) and
    // publish it to the atomic cache. Called after every heap/pool mutation
    // (register, pump-pop/erase, retire). O(pool); the pool is small.
    void recompute_earliest_deadline_locked() SLUICE_REQUIRES(global_mtx_);

    // Helper: the earliest ACTIVE deadline, or false if none. Called under
    // global_mtx_. Used to classify the deadline dimension. (park_on_wake_source
    // reads the atomic cache instead, to avoid the lock-order inversion.)
    bool earliest_active_deadline_locked(deadline_t& out) const SLUICE_REQUIRES(global_mtx_);

    // Pump due timers: for every heap-min whose deadline <= now and whose
    // registration is still ACTIVE, claim it (try_claim_expiry) and resolve
    // its bound node through expire_wait. Lazy-skips retired/consumed entries
    // (removes them from the heap without dereferencing the node). Returns the
    // number of expiries that attempted resolution. Called under global_mtx_
    // by the worker loop and by advance_clock(). Acquires q.mtx() internally
    // via expire_wait (lock order: global_mtx_ -> q.mtx()).
    std::size_t pump_deadlines_locked() SLUICE_REQUIRES(global_mtx_);

    // Heap helpers (min-heap on deadline). Called under global_mtx_.
    static bool heap_less(const TimerRegistration* a, const TimerRegistration* b) noexcept;
    void heap_push_locked(TimerRegistration* r) SLUICE_REQUIRES(global_mtx_);
    void heap_pop_min_locked() SLUICE_REQUIRES(global_mtx_);
    void heap_sift_up_locked(std::size_t i) SLUICE_REQUIRES(global_mtx_);
    void heap_sift_down_locked(std::size_t i) SLUICE_REQUIRES(global_mtx_);

    // O(1) erase of a registration's pool block by its pool_self_ self-link.
    // SAFE ONLY when the block has ALREADY been popped from the deadline heap
    // (so no live heap slot still holds its pointer). The pump is the sole
    // legitimate caller: it pops the min (removing it from the heap), then
    // erases the pool block. Other paths (admission-expired, non-timer retire)
    // do NOT erase — they leave the block in the heap to be popped+erased by
    // the pump at its deadline (lazy removal). This keeps the pool bounded by
    // live+pending deadline waits with no UAF. Called under global_mtx_.
    // NEVER reads node()/queue() (I4-safe by construction).
    void erase_popped_registration_locked(TimerRegistration* r) SLUICE_REQUIRES(global_mtx_);

    // E11-T17 (F2) narrow test hook: register a TimerRegistration for {node,q,
    // deadline} from a NON-worker thread (the test coordinator). Mirrors the
    // full await_wait_deadline admission MINUS the fiber-suspend path: it
    // registers `node` into `q` (Detached->Registered), increments the wait
    // count, creates the ACTIVE registration, pushes it into the deadline heap,
    // and refreshes the earliest-deadline park cache — but does NOT suspend a
    // fiber. This lets the coordinator install a NEW deadline while the worker
    // is held at the park-commit seam (global_mtx_ is released at that seam).
    // TEST-ONLY. Returns the created registration pointer (nullptr if the
    // deadline was already due or the node was not Detached).
    TimerRegistration* register_test_deadline_locked(WaitNode* node, WaitQueue* q,
                                                     deadline_t deadline)
        SLUICE_REQUIRES(global_mtx_);

    // Predicate: is there currently an active timer registration? An active
    // deadline is an externally-resolvable wait (its expiry reaches the
    // Scheduler worker loop), so it participates in MW classification + park
    // liveness exactly like a WaitQueue wait. Called under global_mtx_.
    bool any_active_deadline_locked() const SLUICE_REQUIRES(global_mtx_);

    // Retire the timer registration (if any) bound to `node`. Called by the
    // non-timer winner path (wake_wait_one / cancel_wait) in the SAME
    // global_mtx_ critical section as the resolve CAS, BEFORE runnable
    // publication (E11 Phase 5). Performs ACTIVE->RETIRED on the
    // registration's independently-stable state, closing callback authority so
    // a stale/lazy expiry cannot dereference the node after the fiber resumes
    // and destroys its caller-owned WaitNode (I4).
    //
    // I4-safe scan discipline: the scan considers ONLY ACTIVE registrations.
    // An ACTIVE registration is provably bound to a live, still-Registered
    // node (the node is destroyed only after its wait epoch resolves, which
    // resolves in the SAME global_mtx_ CS that retires/consumes the registration
    // — so while a registration is ACTIVE its node has not been destroyed).
    // Inert (retired/consumed) entries are skipped WITHOUT reading their node()
    // pointer, so a block bound to an already-destroyed node is never
    // dereferenced here. The ACTIVE block matching `&node` (the live winner
    // node) is retired. Called under global_mtx_.
    void retire_timer_for_node_locked(WaitNode& node) SLUICE_REQUIRES(global_mtx_);

    // Predicate: is there currently an externally-resolvable wait registered —
    // one whose resolution arrives from OUTSIDE the worker loop (a ready-flag
    // wait, OR a WaitQueue wait resolved via wake_wait_one/cancel_wait, OR an
    // E11 deadline wait whose expiry is driven by the timer pump)? Such
    // resolutions reach signal_wake_locked (the unified Scheduler wake source),
    // NOT a backend reap, so the worker MUST park on the SCHEDULER domain to
    // observe them. When true, the MW-S2 participant must NOT enter a backend-
    // only ctx_.wait_one() — it parks on the SCHEDULER domain instead (the
    // MIXED-WAKE fix, ADR §9.4.7). Call with global_mtx_ held.
    //
    // E10-CORRECTIVE C1: a WaitQueue wait is externally resolvable exactly like
    // a ready-flag wait (wake_wait_one/cancel_wait run under global_mtx_ +
    // q.mtx() and reach signal_wake_locked via route_runnable_locked). It MUST
    // participate in this classification, or a Live run could park/terminate on
    // a source that cannot observe the wait's resolution (C1 park-domain gap).
    //
    // E11: an ACTIVE deadline registration is likewise externally resolvable —
    // its expiry is driven by the worker loop's timer pump + the bounded park
    // timeout, not by a backend reap. It participates in this classification
    // for the same reason as the WaitQueue wait.
    bool external_wake_possible_locked() const SLUICE_REQUIRES(global_mtx_) {
        return !waiting_ready_.empty() || waiting_waitq_count_ > 0 ||
               any_active_deadline_locked();
    }
};

}  // namespace sluice::async
