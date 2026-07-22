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
#include <sluice/async/detail/queue_port.hpp>  // detail::QueuePort / QueueRole (E12-E Queue seams)
#include <sluice/async/detail/select_registration.hpp>  // detail::DeadlineHeapEntry, SelectTimerRegistration (E13 P3)
#include <sluice/async/select_fwd.hpp>  // E13 P5 CORRECTIVE: select() template declaration + forward decls
#include <sluice/async/fiber.hpp>
#include <sluice/async/fiber_ctx.hpp>
#include <sluice/async/lock_guard.hpp>
#include <sluice/async/mutex.hpp>
#include <sluice/async/thread_annotations.hpp>
#include <sluice/async/timer_registration.hpp>
#include <sluice/async/wait_node.hpp>
#include <sluice/async/wait_queue.hpp>

#include <algorithm>
#include <array>
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

class Event;
class SelectResult;  // P5: full definition lives in select.hpp (included by select.cpp)

namespace detail {
class SelectGroup;
class SelectPort;
struct SelectArmSlot;
enum class ArmState : std::uint8_t;
class SelectCaseDescriptor;  // P5 CORRECTIVE: full definition in select.hpp (sealed fields)
}  // namespace detail

// ----------------------------------------------------------------------------
// ASYNC-TEST-SEAM-AUTHORITY-CORRECTIVE-1.
//
// This installed header declares NO test-hook type and grants NO test friend.
// Previously, namespace-level forward declarations of SchedulerTestHooks /
// E9ParkSeamHooks / E11TimerTestHooks / E12EventTestHooks lived here, each with
// a matching `friend struct X;` inside Scheduler/Event. An ordinary production
// TU could supply its own definition of any of those types and forge the friend
// grant, because the type NAME was published by this installed header. That
// forgeable authority is now REMOVED.
//
// Deterministic test causal seams (E7 admission, E9 park, E11 clock/timer, E12
// event set/admission) are realized by a SEPARATE non-installed internal-
// testing runtime (`sluice_async_internal_testing`), compiled from the same
// authoritative sources with the private macro SLUICE_ASYNC_INTERNAL_TESTING
// defined. Only that variant links the test-support objects and the controller
// state; the production `sluice_async` target is hook-free and exports no test
// phase, no test seam state, and no test controller symbol. No binary links
// both runtime variants.
// ----------------------------------------------------------------------------

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

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
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
#endif  // defined(SLUICE_ASYNC_INTERNAL_TESTING)

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

    // E13: owner Scheduler identity. Set exactly once when WorkerState is
    // attached to a Scheduler. Immutable by contract; not used for routing.
    Scheduler* owner_scheduler{nullptr};

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
    // in whatever state the registered waiters left it). P2: also performs
    // Phase-1 Select scan on `select_port` inside the same global_mtx_ CS.
    // Returns the number of waiters resolved by THIS call.
    // Safe to call from an external OS thread.
    std::size_t event_set_broadcast(Event& event);

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
    //
    // Deadline precedence (F-EVENT-DEADLINE): at admission, Event SET readiness
    // is checked BEFORE the already-due deadline predicate. Therefore Event SET
    // + already-due deadline -> Woken inline (the resource is ready; the
    // deadline is moot). This is the accepted production behavior.
    void await_event_wait_deadline(WaitQueue& q, const std::atomic<bool>& set_flag,
                                   WaitNode& node, deadline_t deadline);

    // E12-A-EVENT-CORRECTIVE-2: the narrow Event cancellation authority with
    // EXACT queue-membership validation. Resolves `node` with Cancelled only if
    // it is currently linked in `q` (scanned under global_mtx_ + q.mtx()). Does
    // NOT expose `q` to the caller (Event::cancel passes its private waiters_).
    // Returns true iff node is Registered AND linked in `q` AND CANCEL wins;
    // otherwise returns false without mutation. A foreign node (registered in a
    // different queue, or detached/terminal) fails the membership gate and
    // returns false -- it is NOT resolved and NOT unlinked. This is the
    // per-wait-epoch CANCEL cause; it cannot synthesize a RESOURCE_WAKE. Generic
    // cancel_wait is unchanged (Event-specific membership gate only).
    bool event_cancel_wait(WaitQueue& q, WaitNode& node);


    // ---- E12-B Semaphore admission / release (sluice-CORE-E12-B) ----
    // The Semaphore counting-permit substrate. A Semaphore owns an
    // std::atomic<permit_count_t> available_ + a private WaitQueue waiters_ +
    // a const max_permits_. These NARROW private seams implement the
    // admission closure, release transfer/store/overflow, timed acquire, and
    // queue-identity-safe cancellation under global_mtx_ (the existing
    // serialization domain). No Semaphore-private wake channel, no permit
    // ownership tracking, no grant-in-flight/refund state, no direct Fiber
    // manipulation.
    //
    // Synchronization domain: ALL Semaphore seams take global_mtx_ (and
    // waiters_.mtx() inside it), the same domain await_wait /
    // wake_wait_one / cancel_wait / expire_wait / the Event seams use. This
    // serializes admission, release disposition, and cancellation so the
    // lost-wake / barging / overflow-mutation windows are mechanically closed.
    //
    // Lock order: global_mtx_ -> waiters_.mtx() (unchanged). `available` is
    // passed by reference (the Semaphore's atomic); it is read/written ONLY
    // under the authoritative locks here. It does NOT authorize lock-free
    // acquisition.

    // Attempt to acquire one permit WITHOUT suspending. Under global_mtx_ +
    // waiters_.mtx(): if `available > 0` AND the queue is empty (no eligible
    // queued waiter has FIFO priority), decrement `available` and return true;
    // otherwise return false with no mutation. Safe to call from any thread.
    // No barging.
    [[nodiscard]] bool sem_try_acquire(WaitQueue& waiters,
                                       std::atomic<std::uint32_t>& available);

    // Acquire admission closure. Register `node` on `waiters`, then recheck
    // resource admission under the authoritative locks:
    //   - if `available > 0` AND `node` is now the FIFO head (admissible): consume
    //     one stored permit (available--), resolve `node` Woken inline via
    //     wake_node_locked, do NOT suspend.
    //   - otherwise: commit suspension.
    // The admission window is closed: a permit observed during the admission
    // critical section leads to inline Woken, not a sleeping registered waiter
    // with stored supply. Mirrors await_event_wait's lost-set closure. `node`
    // must be Detached (fresh) and outlive this call's suspend until resumed.
    void sem_acquire(WaitQueue& waiters,
                     std::atomic<std::uint32_t>& available, WaitNode& node);

    // Deadline-aware acquire admission closure. Composes sem_acquire's
    // admission closure with E11 TimerRegistration. Mandatory precedence
    // (under global_mtx_ + waiters_.mtx()):
    //   1. authoritative permit admission (if available > 0 AND node is FIFO
    //      head): resolve Woken inline (no suspend). Permit admission wins over
    //      a due deadline.
    //   2. else if the deadline is already due: resolve Expired inline (E11 I5).
    //   3. else: commit suspension.
    // For a registered timed wait, RESOURCE_WAKE (release) / TIMER_EXPIRE /
    // CANCEL compete through the existing exactly-once WaitNode resolution
    // authority. Reuses E11 timer registration/retirement. No Semaphore-local
    // deadline mechanism.
    void sem_acquire_until(WaitQueue& waiters,
                           std::atomic<std::uint32_t>& available, WaitNode& node,
                           deadline_t deadline);

    // Queue-identity-safe cancellation (mirrors event_cancel_wait). Resolves
    // `node` with Cancelled only if it is currently linked in `waiters` (scanned
    // under global_mtx_ + waiters_.mtx()). Returns true iff node is Registered
    // AND linked in `waiters` AND CANCEL wins; otherwise returns false WITHOUT
    // mutation. Does NOT expose `waiters`. Does NOT change `available`. Safe
    // against wrong-Semaphore (same/different Scheduler), detached, and terminal
    // nodes.
    [[nodiscard]] bool sem_cancel(WaitQueue& waiters, WaitNode& node);

    // Release disposition (mirrors wake_wait_one_locked for the transfer branch
    // and the permit store for the empty-queue branch). Under global_mtx_ +
    // waiters_.mtx(), this release call contributes exactly one pending permit:
    //   - if `waiters` is non-empty: wake exactly the FIFO head via
    //     wake_wait_one_locked, transferring this release-created permit
    //     directly to that waiter. `available` is UNCHANGED. Return true. (A
    //     null return from wake_wait_one_locked happens ONLY when the queue is
    //     empty — Conclusion A — so the transfer branch never falls through.)
    //   - otherwise (queue empty): if `available == max_permits`: return false
    //     (overflow; no mutation). Otherwise `available++` and return true.
    // One release never both wakes a waiter AND stores. Safe to call from an
    // external OS thread (mirrors event_set_broadcast's external-thread path:
    // g_worker is null -> pending_spawn_ routing).
    [[nodiscard]] bool sem_release(WaitQueue& waiters,
                                   std::atomic<std::uint32_t>& available,
                                   std::uint32_t max_permits);


    // ---- E12-C AsyncMutex admission / handoff (sluice-CORE-E12-C) ----
    // The Fiber-suspending Mutex substrate. An AsyncMutex owns a Fiber* owner_
    // (the SOLE ownership authority — no redundant locked_) + a private
    // WaitQueue waiters_. These NARROW private seams implement the admission
    // closure, direct ownership handoff (MUTEX-HANDOFF-ONE), timed lock,
    // queue-identity-safe cancellation, and try_lock under global_mtx_ (the
    // existing serialization domain). No Mutex-private wake channel, no
    // grant-in-flight / reserved-owner state, no generic winner callback.
    //
    // Synchronization domain: ALL Mutex seams take global_mtx_ (and
    // waiters_.mtx() inside it), the same domain the sem_* / await_wait /
    // wake_wait_one / cancel_wait / expire_wait seams use. This serializes
    // admission, handoff, and cancellation so the lost-wake / barging /
    // owner-before-publication windows are mechanically closed.
    //
    // Lock order: global_mtx_ -> waiters_.mtx() (unchanged). `owner` is passed
    // by reference (the AsyncMutex's Fiber* owner_); it is read/written ONLY
    // under the authoritative locks here. It does NOT authorize lock-free
    // observation (there is no public is_locked()).

    // Attempt to acquire ownership WITHOUT suspending. Under global_mtx_ +
    // waiters_.mtx(): if `owner == nullptr` AND the queue is empty (no eligible
    // queued waiter has FIFO priority), set `owner = current Fiber` and return
    // true; otherwise return false with no mutation. No barging: a queued
    // waiter with FIFO priority cannot be bypassed. Requires a running Fiber
    // (g_worker->current); a null Fiber is a caller-precondition debug assert.
    [[nodiscard]] bool mutex_try_lock(WaitQueue& waiters, Fiber*& owner);

    // Lock admission closure. Register `node` on `waiters`, then recheck
    // ownership admission under the authoritative locks:
    //   - if `owner == nullptr` AND `node` is now the FIFO head (admissible):
    //     resolve `node` Woken inline via wake_node_locked, set
    //     `owner = current Fiber`, do NOT suspend.
    //   - otherwise: commit suspension.
    // The admission window is closed: an owner-release observed during the
    // admission critical section leads to inline Woken or to this registered
    // node, never to a stranded waiter. Mirrors sem_acquire's lost-set closure
    // but the admission predicate is "owner is free AND this node is FIFO head".
    // `node` must be Detached (fresh) and outlive this call's suspend until
    // resumed. Recursive lock by the current owner is a debug assert (caller
    // precondition violation), not a successful acquisition.
    void mutex_lock(WaitQueue& waiters, Fiber*& owner, WaitNode& node);

    // Deadline-aware lock admission closure. Composes mutex_lock's admission
    // closure with E11 TimerRegistration. Mandatory precedence (under
    // global_mtx_ + waiters_.mtx()):
    //   1. authoritative ownership admission (owner free AND node is FIFO head):
    //      resolve Woken inline (no suspend). Ownership admission wins over a
    //      due deadline (resource-first).
    //   2. else if the deadline is already due: resolve Expired inline (E11 I5).
    //   3. else: commit suspension.
    // For a registered timed wait, RESOURCE_WAKE (unlock handoff) /
    // TIMER_EXPIRE / CANCEL compete through the existing exactly-once WaitNode
    // resolution authority. Reuses E11 timer registration/retirement. No
    // Mutex-local deadline mechanism.
    void mutex_lock_until(WaitQueue& waiters, Fiber*& owner, WaitNode& node,
                          deadline_t deadline);

    // Queue-identity-safe cancellation (mirrors sem_cancel). Resolves `node`
    // with Cancelled only if it is currently linked in `waiters` (scanned under
    // global_mtx_ + waiters_.mtx()). Returns true iff node is Registered AND
    // linked in `waiters` AND CANCEL wins; otherwise returns false WITHOUT
    // mutation. Does NOT expose `waiters`. Does NOT change `owner`. Safe to
    // call from any OS thread (g_worker may be null). Safe against wrong-Mutex
    // (same/different Scheduler), detached, and terminal nodes.
    [[nodiscard]] bool mutex_cancel(WaitQueue& waiters, WaitNode& node);

    // Unlock with direct ownership handoff (MUTEX-HANDOFF-ONE). Under
    // global_mtx_, the calling (owner) Fiber releases ownership:
    //   - if `waiters` has an eligible FIFO head: resolve it Woken, commit
    //     `owner = winner Fiber` (BEFORE runnable publication — see
    //     mutex_handoff_one_locked), retire any bound timer, and publish the
    //     winner runnable exactly once. `owner` transitions Owned(F_old) ->
    //     Owned(F_new) with NO intermediate owner = nullptr.
    //   - otherwise (queue empty): `owner = nullptr` (UnlockNoWaiter).
    // The owner-commit-before-publication source order is the load-bearing
    // refinement obligation (docs §10.5 / §15.4). Recursive/non-owner unlock
    // is a debug assert (caller precondition violation), no mutation.
    void mutex_unlock(WaitQueue& waiters, Fiber*& owner);

    // MUTEX-HANDOFF-ONE: the narrow private seam that resolves the eligible FIFO
    // head Woken, commits `owner = winner Fiber`, retires the winner's timer,
    // and publishes the winner runnable — in THAT source order (owner commit
    // BEFORE make_runnable / route_runnable_locked). Mirrors wake_wait_one_locked
    // EXCEPT it writes the winner's fiber() into the caller's `owner` reference
    // before publication (the Semaphore has no ownership to commit). Returns the
    // winning node (nullptr if the queue is empty or the head lost to a
    // concurrent resolver). The caller MUST hold global_mtx_; waiters_.mtx() is
    // taken here (under global_mtx_, consistent lock order). A winning linked
    // node with null Fiber is an internal-invariant debug assert, NOT an
    // empty-queue result.
    WaitNode* mutex_handoff_one_locked(WaitQueue& waiters, Fiber*& owner)
        SLUICE_REQUIRES(global_mtx_);


    // ---- E12-D AsyncCondition (sluice-CORE-E12-D) ----
    // The Fiber-suspending condition variable substrate. An AsyncCondition owns
    // a private Condition WaitQueue `cond_waiters` and is bound to one AsyncMutex
    // (whose `mutex_waiters` + `owner` are passed BY REFERENCE, exactly as the
    // Mutex's own methods pass them into the Mutex seams above). These NARROW
    // private seams implement the CONDITION-WAIT-PREPARE combined step (register
    // Condition node + release/handoff bound Mutex + make_waiting, under one
    // global_mtx_ CS), the Condition notify_one/notify_all resolution, the
    // queue-identity-gated Condition cancel, and the deadline-aware variant.
    //
    // Synchronization domain: same as the E12-A/B/C seams — global_mtx_ (and the
    // relevant queue mtx() inside it). The Condition queue mtx and the Mutex
    // queue mtx are taken SEQUENTIALLY under global_mtx_, NEVER simultaneously
    // (docs §6.3): the prepare seam locks cond_waiters.mtx() to register the
    // node, UNLOCKS it, then calls mutex_handoff_one_locked which takes
    // mutex_waiters.mtx() internally. There is no Condition-queue-to-Mutex-
    // queue lock edge.
    //
    // Mutex authority (construction authorization §1.1): the prepare seam
    // releases the bound Mutex by reusing the ONE accepted
    // mutex_handoff_one_locked (owner-before-publication already load-bearing).
    // It does NOT write `owner` directly, does NOT register on mutex_waiters,
    // and does NOT implement a second handoff. The Scheduler is the sole Mutex
    // state-machine executor.
    //
    // notify_one/notify_all resolve Condition nodes Woken via the existing
    // wake_one_locked / drain pattern and publish the winner runnable
    // (Condition-epoch publication). They do NOT mutate Mutex state: the winner
    // Fiber resumes and runs its OWN reacquire body (mutex_.lock(reacquire_node))
    // — the reacquire epoch is NOT the notifier's concern.

    // CONDITION-WAIT-PREPARE combined seam (docs §7). Under one global_mtx_ CS:
    //   1. register `cond_node` into `cond_waiters` (Detached -> Registered);
    //   2. release the bound Mutex: if `mutex_waiters` has an eligible FIFO head,
    //      mutex_handoff_one_locked resolves it Woken + commits `owner = winner`
    //      (BEFORE publication); else `owner = nullptr` (no waiter). NO
    //      intermediate owner = nullptr window during handoff;
    //   3. make the calling Fiber Waiting.
    // Then context_switch. The calling Fiber MUST currently own the bound Mutex
    // (caller precondition; non-owner wait is a debug assert). Returns the
    // latched Condition-node terminal outcome (Woken/Expired/Cancelled) read
    // from `cond_node` after the resume. The reacquire epoch is run by the
    // CALLER (AsyncCondition::wait) after this returns, NOT by this seam.
    //
    // `released_mutex` mirrors the timed seam (condition_wait_prepare_until):
    // false on the C8 registration-failure path (the Mutex was NOT released —
    // the caller retains ownership and must run NO reacquire epoch), and true
    // after the Mutex has been released/handed off (the caller MUST run the
    // reacquire epoch). The untimed path has no inline-Expired-at-admission
    // branch (no deadline), so every non-registration-failure path releases
    // the Mutex.
    WaitOutcome condition_wait_prepare(WaitQueue& cond_waiters, WaitNode& cond_node,
                                       WaitQueue& mutex_waiters, Fiber*& owner,
                                       bool& released_mutex);

    // Deadline-aware CONDITION-WAIT-PREPARE. Composes condition_wait_prepare
    // with an E11 TimerRegistration on `cond_node` (C-H4: deadline governs ONLY
    // the Condition epoch). Admission precedence (under global_mtx_):
    //   1. if the deadline is ALREADY due at admission: resolve `cond_node`
    //      Expired INLINE — do NOT release the Mutex, do NOT suspend, do NOT
    //      create a reacquire epoch. Return Expired; the caller RETAINS
    //      ownership (WaitDueInline / InvDueInlineRetainsOwnership). Sets
    //      `released_mutex = false`;
    //   2. else: register the node + timer, release/handoff the Mutex,
    //      make_waiting, context_switch; return the latched outcome and set
    //      `released_mutex = true` (the caller MUST run the reacquire epoch).
    // `released_mutex` distinguishes the inline-Expired path (no reacquire) from
    // a suspended resolution (Woken/Cancelled/suspended-Expired, all reacquire),
    // since both an inline-Expired and a suspended-Expired return the SAME
    // outcome (Expired) but have opposite reacquire obligations.
    WaitOutcome condition_wait_prepare_until(WaitQueue& cond_waiters,
                                             WaitNode& cond_node,
                                             WaitQueue& mutex_waiters,
                                             Fiber*& owner, deadline_t deadline,
                                             bool& released_mutex);

    // notify_one: under global_mtx_ + cond_waiters.mtx(), resolve the eligible
    // FIFO head of the Condition queue with Woken, retire any bound timer,
    // decrement waiting_waitq_count_, and publish the winner runnable. Empty
    // queue is a no-op. Does NOT mutate Mutex state. Safe from any OS thread.
    void condition_notify_one(WaitQueue& cond_waiters);

    // notify_all: under one continuous global_mtx_ CS, loop
    // wake_wait_one_locked(cond_waiters) until nullptr (mirrors
    // event_set_broadcast's drain). Each winner resolves Woken exactly once,
    // retires its timer, and is published runnable. Waiters registered after the
    // snapshot linearization point are excluded (admission needs global_mtx_).
    // Returns the count of resolved winners. Does NOT mutate Mutex state.
    std::size_t condition_notify_all(WaitQueue& cond_waiters);

    // Queue-identity-safe Condition cancel (mirrors event_cancel_wait /
    // mutex_cancel). Resolves `cond_node` with Cancelled ONLY if it is currently
    // Registered AND linked in `cond_waiters` (scanned under global_mtx_ +
    // cond_waiters.mtx()) AND the CANCEL CAS wins. Otherwise returns false
    // WITHOUT mutation. Does NOT expose `cond_waiters`. Does NOT change Mutex
    // `owner`. Safe from any OS thread. Safe against wrong-Condition, detached,
    // and terminal nodes.
    [[nodiscard]] bool condition_cancel_wait(WaitQueue& cond_waiters,
                                             WaitNode& cond_node);

    // ---- E12-E Queue wait admission + reconciliation (sluice-CORE-E12-E) ----
    // The Queue blocking/timed substrate. A QueuePort owns a producer and a
    // consumer WaitQueue (waiters_[2]); the Scheduler is the authoritative
    // resolution + publication executor, exactly as for E12-A/B/C/D. ALL seams
    // take global_mtx_ (and the role queue mtx() inside it); the QueuePort
    // passes its private waiters_[role] BY REFERENCE so the Scheduler can
    // resolve under the canonical locks without exposing them. No public
    // wait_queue() accessor exists on QueuePort (sealed authority).
    //
    // Lock order: global_mtx_ (G) -> QueuePort::state_mtx_ (S) -> exactly one
    // of producer/consumer role mtx(). The two role mutexes are NEVER held
    // together. The reconciler takes one role queue per iteration under G+S
    // and loops to a fixed point.
    //
    // The `role` argument selects producer (0) vs consumer (1) admission. The
    // untimed admit closures mirror sem_acquire; the timed variants mirror
    // sem_acquire_until (resource-first admission, then already-due Expired).
    // A Queue wait resolves when EXACTLY ONE cause wins the resolve_ CAS:
    //   - the reconciler grants (a producer/consumer arrives)  -> Woken
    //   - queue_cancel                                          -> Cancelled
    //   - the deadline elapses (push_until/pop_until only)      -> Expired

    // Blocking push admission (P5). Registers the producer node on the
    // producer role FIFO under G + S + producer.mtx(); admission recheck
    // commits the item to the ring inline if space opened with no older
    // producer; otherwise suspends. On return, `lease` is:
    //   - empty if committed (the ring slot owns the control now);
    //   - non-empty (original) if closed/expired/cancelled.
    // The caller MUST have detached->producer_operation the control BEFORE
    // calling and pass its lease by reference.
    void queue_push_admit(detail::QueuePort& port, WaitNode& node,
                          detail::QueueItemLease& lease);
    // Blocking pop admission (P5). Symmetric: consumer node on the consumer
    // role FIFO; admission recheck pops an item inline if one arrived with no
    // older consumer; otherwise suspends. On return, `out` is:
    //   - non-empty (the popped item's lease) if an item was granted;
    //   - empty if closed+empty (the caller returns `closed`).
    void queue_pop_admit(detail::QueuePort& port, WaitNode& node,
                         detail::QueueItemLease& out);
    // Deadline-aware variants (P4-timed). Resource-first admission wins over a
    // due deadline; an already-due deadline with no admissible resource
    // resolves Expired inline (E11 I5). On expired, the push caller's `lease`
    // remains non-empty (original); the pop caller's `out` remains empty.
    void queue_push_admit_until(detail::QueuePort& port, WaitNode& node,
                                detail::QueueItemLease& lease,
                                deadline_t deadline);
    void queue_pop_admit_until(detail::QueuePort& port, WaitNode& node,
                               detail::QueueItemLease& out, deadline_t deadline);

    // Queue-identity-safe cancellation. Mirrors mutex_cancel. Returns true ONLY
    // if the node is Registered AND linked in this QueuePort's role FIFO AND the
    // CANCEL CAS wins. Safe from any OS thread; does not change ring state.
    [[nodiscard]] bool queue_cancel(detail::QueuePort& port, detail::QueueRole role,
                                    WaitNode& node);

    // ---- E12-E reconciler grant seams (winner-before-publication) ----
    // Called by QueuePort fast paths under G + S (caller-held) to grant the
    // role FIFO head atomically: resolve_(Woken) + per-winner resource commit
    // (read from won->user()) + retire any bound timer + make_runnable /
    // route_runnable_locked (publication LAST). These mirror
    // mutex_handoff_one_locked's ordering: the commit happens BETWEEN resolve
    // and publication. Each takes the role mtx internally (under G). Returns
    // the winning WaitNode (nullptr if the role FIFO is empty / head lost the
    // CAS). The caller (try_push / try_pop / close) decides how many times to
    // loop (e.g. close drains until nullptr).
    //
    // queue_grant_consumer_locked: a producer just committed an item OR close
    // is draining. Moves the ring FIFO HEAD item into the winner's out-lease.
    // Precondition: ring non-empty (the caller ensured it). If the ring is
    // somehow empty (close race), resolves the winner Woken with no commit
    // (the caller's close path then sees closed+empty).
    WaitNode* queue_grant_consumer_locked(detail::QueuePort& port);
    // queue_grant_producer_locked: a consumer just freed a slot OR close is
    // draining. Moves the winner's lease (ctx->prod_control) into the freed
    // ring slot. Precondition: ring not full. If the Queue is Closed, resolves
    // the winner Woken with the lease RETAINED (the producer returns closed).
    WaitNode* queue_grant_producer_locked(detail::QueuePort& port);

    // E12-E P7 teardown precondition helper. Reports whether BOTH role FIFOs
    // of `port` are empty (no producer parked, no consumer parked). Called by
    // QueuePort::begin_teardown under global_mtx_; the QueuePort itself is not
    // a friend of WaitQueue (only the Scheduler is), so the emptiness query is
    // the Scheduler's authority. Each role.mtx() is taken sequentially under
    // global_mtx_ (the canonical lock order G -> exactly one role); the two
    // role mutexes are NEVER held together. Returns true iff both FIFOs have
    // no linked WaitNode at the instant of observation.
    bool queue_role_waiters_empty_locked(detail::QueuePort& port)
        SLUICE_REQUIRES(global_mtx_);

    // E12-E Queue timer-counter on-resolve thunk (F.1/F.2 corrective). A
    // Queue-bound TimerRegistration installs this as its on_resolve_ hook +
    // `&port` as owner_ctx_ at admit time. The Scheduler fires it exactly once
    // per ACTIVE->terminal timer transition (pump on consume,
    // retire_timer_for_node_locked on retire) under global_mtx_. It is a STATIC
    // MEMBER (not a free function) so it can reach QueuePort's private
    // active_queue_timers_ counter via Scheduler's friend grant. The signature
    // matches TimerRegistration::OnResolveFn.
    static void queue_timer_on_resolve(void* owner_ctx, bool timer_won) noexcept;


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
    friend class SchedulerWakeHandle;  // E9: notify() -> notify_external_wake
    // E12-E: QueuePort reaches global_mtx_, wake_wait_one_locked, and the
    // queue admit/cancel seams to reconcile the OTHER role on fast-path
    // success. Mirrors how Scheduler friended nothing for Mutex/Semaphore
    // (those pass their private waiters_ by reference); the Queue needs
    //Scheduler-internal wake + global-mtx access for its reconciler.
    friend class ::sluice::async::detail::QueuePort;

    // E13 P5 CORRECTIVE: friend the pre-declared constrained public select()
    // template (declared in select_fwd.hpp, defined in select.hpp). By
    // friending a concrete function-template entity (not a concrete struct
    // name), an ordinary production TU cannot forge the friend grant: the
    // template is uniquely identified by its template-head + requires clause +
    // signature, and no other definition can match that entity.
    //
    // The template calls select_admit_inline directly (no intermediate
    // SelectBridge). select_admit_inline stays private to all other code.
    template <class... Cases>
        requires (
            sizeof...(Cases) >= 1 &&
            sizeof...(Cases) <= kSelectMaxArms &&
            (SelectCaseType<std::remove_cvref_t<Cases>> && ...)
        )
    friend SelectResult select(Scheduler& scheduler, Cases&&... cases);

    // ---- E13 Select registry operations (private Scheduler authority) ----
    // All three require global_mtx_ held. Event must belong to this Scheduler.
    //
    // Link `arm` into `event`'s private SelectPort. Precondition: arm is
    // Prepared/Detached, home_ is null, not already linked. Establishes:
    // arm.home_ == &event.select_port_, arm.state == Registered,
    // arm.kind == Event, arm.group != nullptr.
    void select_event_link_locked(Event& event, detail::SelectArmSlot& arm)
        SLUICE_REQUIRES(global_mtx_);

    // Remove `arm` from `event`'s private SelectPort. Repairs links and
    // clears arm.next_, arm.prev_, arm.home_. Does NOT claim winner, set
    // result, publish caller, or retire Timer. Assertion-fails on mismatch.
    void select_event_unlink_locked(Event& event, detail::SelectArmSlot& arm)
        SLUICE_REQUIRES(global_mtx_);

    // Walk `event`'s SelectPort, marking eligible Event arms CandidateReady.
    // Eligible: kind==Event, state==Registered, group!=nullptr, group.phase==Armed.
    // Returns the number of arms marked. P2: readiness-offer only; no winner
    // claim, no finalization, no publication.
    std::size_t select_event_scan_locked(Event& event)
        SLUICE_REQUIRES(global_mtx_);

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

    // ASYNC-TEST-SEAM-AUTHORITY-CORRECTIVE-1: the deterministic causal pause
    // seams (E7 admission, E9 park candidate/commit, E12 event set-store/
    // admission-before-final-check) NO LONGER live as Scheduler fields. They
    // were pure test-coordination state (mtx/cv/bool) with zero production
    // readers, and the forgeable namespace-level friends that gated them have
    // been removed. The seams are now driven by a non-installed test controller
    // keyed on `Scheduler*`, reached ONLY through phase call sites compiled into
    // the `sluice_async_internal_testing` variant (guarded by
    // SLUICE_ASYNC_INTERNAL_TESTING in scheduler.cpp). The production
    // `sluice_async` target has no such fields, no such call sites, and no such
    // symbols. See tests/async_test_control_internal.hpp for the controller.

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
    // E13 P3 (deadline-heap migration): the heap now stores unified tagged
    // DeadlineHeapEntry values (Ordinary | Select) instead of raw
    // TimerRegistration*. Both kinds share one min-heap keyed by the cached
    // deadline; the ordinary branch is byte-for-byte identical in logic (it
    // reads the same deadline, pops the same min, and processes the same
    // TimerRegistration* via entry.target.ordinary). Internal-only type; no
    // public API exposure. See docs/e13-select-timer-adapter.md §4.
    std::vector<detail::DeadlineHeapEntry> deadline_heap_ SLUICE_GUARDED_BY(global_mtx_){};

    // E13 P3: Scheduler-owned stable pool of SelectTimerRegistration blocks,
    // mirroring timer_pool_. Blocks are constructed in a caller-frame
    // std::list outside G and spliced in ONE NODE AT A TIME under G during
    // registration (std::list::splice is O(1), allocation-free). Pointer-
    // stable; the heap entry references a block by &pool.back() after splice.
    // Lazy-at-deadline reclamation: a retired/consumed block remains here
    // until the pump pops its deadline entry (mirrors timer_pool_).
    std::list<detail::SelectTimerRegistration> select_timer_pool_
        SLUICE_GUARDED_BY(global_mtx_){};

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
    // E13 P3: the heap stores unified DeadlineHeapEntry values; the comparator
    // compares cached deadlines (equal-deadline order is unspecified). sift/pop
    // no longer touch any registration's heap_index (the entry's vector
    // position is the sole position authority).
    void heap_push_entry_locked(const detail::DeadlineHeapEntry& e)
        SLUICE_REQUIRES(global_mtx_);
    // Thin wrapper: build an Ordinary entry from a TimerRegistration and push.
    // Kept so ordinary call sites read as `heap_push_ordinary_locked(reg)`.
    void heap_push_ordinary_locked(TimerRegistration* r) SLUICE_REQUIRES(global_mtx_);
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

    // ---- E13 P3 Select timer registration (private Scheduler authority) ----
    // All require global_mtx_ held. These own the registered-state accounting
    // authority for Select Timer arms (Addendum C): every ACTIVE->terminal
    // transition of a registered Select block routes through the two helpers
    // below so active_deadline_count_ is decremented exactly once and the
    // earliest-deadline cache is recomputed. The stale pump-pop path does
    // physical reclamation only and does NOT decrement again.

    // Splice ONE SelectTimerRegistration node from a caller-frame temporary
    // pool into select_timer_pool_, push its DeadlineHeapEntry {Select}, and
    // return a stable pointer to the now-Scheduler-owned block. O(1),
    // allocation-free under G (single-node std::list::splice + push within
    // reserved capacity — reserve is the caller's responsibility). The
    // returned pointer is valid until the pump reclaims the block.
    detail::SelectTimerRegistration* select_timer_splice_one_locked(
        std::list<detail::SelectTimerRegistration>& tmp_pool,
        std::list<detail::SelectTimerRegistration>::iterator it)
        SLUICE_REQUIRES(global_mtx_);

    // Retire a registered Select block: validate ownership + pool membership,
    // CAS ACTIVE->RETIRED; on success decrement active_deadline_count_ once
    // and recompute earliest_active_deadline_. Returns true iff THIS call
    // retired an active registration. On failed CAS: no counter mutation.
    bool select_timer_retire_locked(detail::SelectTimerRegistration& reg)
        SLUICE_REQUIRES(global_mtx_);

    // Consume a registered Select block: validate ownership + pool membership,
    // CAS ACTIVE->CONSUMED; on success decrement active_deadline_count_ once
    // and recompute earliest_active_deadline_. Returns true iff THIS call
    // consumed an active registration. On failed CAS: no counter mutation.
    bool select_timer_consume_locked(detail::SelectTimerRegistration& reg)
        SLUICE_REQUIRES(global_mtx_);

    // O(1)-by-address erase of a Select pool block, SAFE only because the
    // caller (pump) has ALREADY popped the block from the deadline heap.
    // Physical reclamation only: does NOT decrement active_deadline_count_
    // (the retire/consume helper already did, exactly once). I4-safe: matches
    // by address, never reads arm_. Mirrors erase_popped_registration_locked.
    void erase_popped_select_registration_locked(
        detail::SelectTimerRegistration* r) SLUICE_REQUIRES(global_mtx_);

    // Predicate: does this Scheduler's select_timer_pool_ own `reg` (by
    // address)? Used by the accounting helpers to validate that a registered
    // transition is being applied to a Scheduler-owned block (defends against
    // a stray/detached local object or a cross-Scheduler mistake). O(pool).
    bool pool_owns_select_block_locked(
        const detail::SelectTimerRegistration& reg) const noexcept
        SLUICE_REQUIRES(global_mtx_);

    // E13 P3 Select timer pump branch body. PRE: global_mtx_ held; the pump
    // has ALREADY popped `reg` from the deadline heap and observed
    // now >= reg.deadline(). State-before-arm rule (Addendum E): load state
    // first; non-ACTIVE -> PumpSkip seam + return stale (do NOT read arm_).
    // ACTIVE -> TimerPumpActive seam, instrument the exact production arm
    // dereference site, then fail fast (a due ACTIVE Select entry is
    // unreachable in valid P3 — Addendum D). The caller (pump) performs the
    // physical reclamation via erase_popped_select_registration_locked
    // regardless of the branch. Returns true if the entry was stale (skipped),
    // false if it was ACTIVE (the call does not return in the ACTIVE case).
    bool select_timer_pump_entry_locked(
        detail::SelectTimerRegistration& reg) SLUICE_REQUIRES(global_mtx_);

    // ---- E13 P4 Select central claim + winner/loser finalization core ----
    // All require global_mtx_ held continuously. This is the single loser
    // processor and the single claim+finalize orchestrator
    // (docs/e13-select-locking-and-publication.md §1.5,
    //  docs/e13-select-formal-production-mapping.md §2). P4 does NOT publish a
    // result, route a runnable, transition the group to Completed/Consumed, or
    // choose a candidate from a readiness snapshot (the caller supplies the
    // candidate index). After a successful process the group is "claimed and
    // finalized but unpublished" — an internal transient state under
    // global_mtx_ that a later stage will follow with publication before the
    // lock is released.

    // Receive one CandidateReady arm and claim+finalize the whole group. PRE:
    // global_mtx_ held. Validates the entire group BEFORE the irreversible
    // winner CAS, then attempts the single group winner CAS
    // (SelectGroup::claim_winner_locked). On win: commits the winner, finalizes
    // every loser, closes every Event/Timer authority, asserts all-authority-
    // closed, returns true. On loss (another invocation already owns the
    // winner): performs no arm/adapter/accounting/phase mutation, returns
    // false. No allocation in this path. Returns whether THIS invocation won
    // the claim.
    bool select_process_group_locked(detail::SelectGroup& group,
                                     std::uint32_t candidate_index)
        SLUICE_REQUIRES(global_mtx_);

    // Validate the entire group BEFORE the irreversible winner CAS (P4 §6).
    // Split into shape (asserts for every call) and claim (asserts for a fresh
    // claim only, after the winner-existence check returns claim-lost). Every
    // check is a debug assertion that fires before any mutation, so an invalid
    // group cannot become permanently claimed and then fail halfway through
    // finalization. No allocation. Members (not free functions) because they
    // read Event private fields (scheduler_, select_port_) under the friend
    // grant.
    void select_preflight_shape_locked(detail::SelectGroup& group,
                                       std::uint32_t candidate_index) const
        SLUICE_REQUIRES(global_mtx_);
    void select_preflight_claim_locked(detail::SelectGroup& group,
                                       std::uint32_t candidate_index) const
        SLUICE_REQUIRES(global_mtx_);

    // Commit the winner arm (post-CAS). Timer winner: ACTIVE->CONSUMED via
    // select_timer_consume_locked (CAS after group claim), then arm.state=
    // Retired. Event winner: targeted handling, unlink from Event SelectPort,
    // then arm.state = Retired. Event::set_ is NEVER mutated. No publication.
    void select_commit_winner_locked(detail::SelectGroup& group,
                                     std::uint32_t winner_index)
        SLUICE_REQUIRES(global_mtx_);

    // Finalize one loser arm. Timer loser: arm.state = Retired FIRST, then
    // ACTIVE->RETIRED via select_timer_retire_locked (SN-9: arm classification
    // precedes the registration retirement CAS). Event loser: arm.state =
    // Retired, then unlink. No publication; never writes result/runnable.
    void select_finalize_loser_locked(detail::SelectGroup& group,
                                      std::uint32_t loser_index)
        SLUICE_REQUIRES(global_mtx_);

    // Per-kind finalizer halves (called by the two drivers above under G).
    void select_finalize_event_winner_locked(detail::SelectGroup& group,
                                             detail::SelectArmSlot& arm)
        SLUICE_REQUIRES(global_mtx_);
    void select_finalize_event_loser_locked(detail::SelectGroup& group,
                                            detail::SelectArmSlot& arm)
        SLUICE_REQUIRES(global_mtx_);
    void select_finalize_timer_winner_locked(detail::SelectGroup& group,
                                             detail::SelectArmSlot& arm)
        SLUICE_REQUIRES(global_mtx_);
    void select_finalize_timer_loser_locked(detail::SelectGroup& group,
                                            detail::SelectArmSlot& arm)
        SLUICE_REQUIRES(global_mtx_);

    // The reusable all-authority-closed invariant predicate (SN-10). Returns
    // true iff: winner != kNoWinner, winner < arm_count, AND for every arm
    //   - Event: state == Retired, home_ == nullptr, next_/prev_ == nullptr
    //   - Timer: state == Retired, stable_reg_ terminal (CONSUMED for the
    //            winner, RETIRED for losers)
    // select_process_group_locked asserts this before returning true on a win.
    // A future select_publish_locked must call this at its entry as its
    // publication precondition. Pure read; no mutation.
    bool select_all_authority_closed_locked(const detail::SelectGroup& group) const
        SLUICE_REQUIRES(global_mtx_);

    // ---- E13 P5 Select registration + inline admission ----
    // (docs/e13-select-production-test-plan.md §7.5,
    //  docs/e13-select-locking-and-publication.md §3,
    //  docs/e13-select-public-api.md §3/§4/§5/§7).
    //
    // The single non-template admission core, reached ONLY via the friended
    // public variadic select() template (declared in select_fwd.hpp, defined
    // in select.hpp). PRIVATE: ordinary code cannot name it (the friend grant
    // is to the exact constrained template entity, not to a forgeable struct
    // name). Owns every centralized admission step: caller + case-Scheduler
    // validation (BEFORE any allocation), caller-frame group+ arms
    // materialization, Timer stable-block construction (before global_mtx_),
    // deadline-heap reserve (the only allocation under the lock), the
    // registration loop, FinishRegistration, the immutable readiness snapshot,
    // lowest-index tie-break, the single P4 processor call, all-authority-
    // closed verification, and inline result completion (Completed -> Consumed).
    // NOT a template — compiles once.
    //
    // P5 implements the INLINE-READY case only: a successful admission returns a
    // SelectResult without suspending the caller or publishing a runnable. The
    // no-ready branch fails fast (suspended completion is P6, denied here).
    // `descs` points at `count` SelectCaseDescriptor values (caller-frame array);
    // the function does not retain the pointer past the call.
    //
    // Not noexcept: may throw std::logic_error (caller validation) or
    // std::invalid_argument (case Scheduler mismatch) BEFORE any allocation, or
    // std::bad_alloc (Timer block / heap reserve) before the first registration
    // mutation. After the heap reserve, the registration loop contains no
    // ordinary throwing operation.
    SelectResult select_admit_inline(detail::SelectCaseDescriptor* descs,
                                     std::size_t count);

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

    // ------------------------------------------------------------------------
    // ASYNC-TEST-SEAM-AUTHORITY-CORRECTIVE-1: internal-testing access surface.
    //
    // This block is compiled ONLY when SLUICE_ASYNC_INTERNAL_TESTING is defined
    // (the `sluice_async_internal_testing` variant). In the production
    // `sluice_async` target the macro is undefined, so these declarations do not
    // exist: an ordinary production TU cannot name `AsyncTestAccess`, cannot
    // drive the test clock, cannot register a test deadline, and cannot observe
    // timer-pool internals. There is no forgeable namespace-level friend; the
    // accessors are ordinary private members gated by the preprocessor.
    //
    // `AsyncTestAccess` is a thin pass-through to the dual-use production state
    // (clock_, test_clock_mode_, timer_pool_, deadline_heap_,
    // register_test_deadline_locked). The production fields themselves remain
    // declared unconditionally above (production reads clock_/
    // test_clock_mode_); only the WRITE/observation accessors are test-only.
    // The non-installed test-support controller (tests/async_test_control.hpp)
    // is the sole consumer.
    // ------------------------------------------------------------------------
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
public:
    // E13 P3 test-only instrumentation (Addendum E): counts the number of
    // times the Select timer pump branch reads reg.arm_ on an ACTIVE entry
    // — i.e. the exact production dereference site, instrumented immediately
    // before the load. Stale (RETIRED/CONSUMED) pops must observe a delta of
    // 0. Reachable only via AsyncTestAccess; absent in production.
    std::size_t select_timer_arm_load_count_{0};

    // Internal-testing access surface. Reached only via the non-installed
    // test-support controller; not part of the public API.
    struct AsyncTestAccess {
        // Enable the deterministic logical clock (test mode).
        static void enable_test_clock(Scheduler& s) noexcept {
            s.test_clock_mode_.store(true, std::memory_order::release);
            s.clock_.store(0, std::memory_order::release);
        }
        static void set_clock(Scheduler& s, deadline_t t) noexcept {
            s.clock_.store(t, std::memory_order::release);
        }
        static deadline_t clock_now(const Scheduler& s) noexcept {
            return s.clock_.load(std::memory_order::acquire);
        }

        // Register a test deadline from a non-worker thread (the coordinator).
        // Mirrors await_wait_deadline admission MINUS the fiber-suspend path.
        // Acquires global_mtx_ internally (the caller does NOT hold it).
        static TimerRegistration* register_test_deadline(Scheduler& s,
                                                         WaitNode* node,
                                                         WaitQueue* q,
                                                         deadline_t deadline);

        // Timer-pool observation. These read GUARDED_BY fields from a test
        // coordinator thread for diagnostics; defined out-of-line with a TSA
        // suppression (the pool sizes are not load-bearing for correctness).
        static std::size_t timer_pool_size(const Scheduler& s) noexcept;
        static std::size_t deadline_heap_size(const Scheduler& s) noexcept;
        static std::size_t active_deadline_count(const Scheduler& s) noexcept;
        static std::size_t timer_pool_count_in_state(const Scheduler& s,
                                                     TimerRegistration::State st) noexcept;
        static bool earliest_active_deadline(Scheduler& s, deadline_t& out);

        // ---- E13 Select registry test accessors ----
        // Link an Event Select arm into the Event's private SelectPort.
        // Acquires global_mtx_ internally. The arm must be Prepared/Detached
        // with kind==Event and group set.
        static void select_event_link(Scheduler& s, Event& event,
                                      detail::SelectArmSlot& arm) {
            LockGuard lk(s.global_mtx_);
            s.select_event_link_locked(event, arm);
        }

        // Unlink an Event Select arm from the Event's private SelectPort.
        // Acquires global_mtx_ internally.
        static void select_event_unlink(Scheduler& s, Event& event,
                                        detail::SelectArmSlot& arm) {
            LockGuard lk(s.global_mtx_);
            s.select_event_unlink_locked(event, arm);
        }

        // Phase-1 scan: walk the Event's SelectPort and mark eligible arms
        // CandidateReady. Acquires global_mtx_ internally. Returns marked count.
        static std::size_t select_event_scan(Scheduler& s, Event& event) {
            LockGuard lk(s.global_mtx_);
            return s.select_event_scan_locked(event);
        }

        // Set an arm's state under global_mtx_. Used by tests to prepare
        // specific arm states before a scan.
        static void set_arm_state(Scheduler& s, detail::SelectArmSlot& arm,
                                  detail::ArmState st);

        // P4 EH corrective: forge a stale-but-equality-passing Event home_ for
        // the event-membership death test. PRE: `arm` is NOT linked in `event`'s
        // SelectPort intrusive list and its home_/next_/prev_ are null (e.g. it
        // was unlinked through select_event_unlink). After this call
        // arm.home_ == &event.select_port_ (so the preflight home_ equality
        // check passes), but the arm remains ABSENT from the intrusive list
        // (so the mechanical `found` scan fails and the preflight asserts).
        // This is the exact shape the EH case must reach: a home_ that looks
        // right but cannot be mechanically confirmed, proving the intrusive-
        // membership scan is load-bearing. Acquires global_mtx_ internally.
        static void select_event_forge_stale_home(Scheduler& s, Event& event,
                                                  detail::SelectArmSlot& arm);

        // ---- E13 P3 Select timer test accessors ----
        // All route through Scheduler authority. No forgeable test hook; the
        // production target has none of these symbols.
        //
        // Synthetic Select timer registration from a non-worker thread (the
        // test coordinator). Builds a one-node temporary list and splices it
        // into select_timer_pool_ under G, pushing its tagged heap entry. The
        // block starts ACTIVE. Returns a stable pointer to the Scheduler-owned
        // block. `deadline` must be in the future (now < deadline) or the
        // entry is immediately due — tests keep clocks before deadlines or
        // transition to terminal before advancing.
        static detail::SelectTimerRegistration* register_synthetic_select_timer(
            Scheduler& s, detail::SelectArmSlot* arm, deadline_t deadline) {
            std::list<detail::SelectTimerRegistration> tmp;
            tmp.emplace_back(arm, &s, deadline);
            // Reserve heap capacity BEFORE mutation (the admission protocol's
            // only allocation-under-G). Synthetic single-entry registration.
            LockGuard lk(s.global_mtx_);
            s.deadline_heap_.reserve(s.deadline_heap_.size() + 1);
            return s.select_timer_splice_one_locked(tmp, tmp.begin());
        }

        // Transition a registered synthetic ACTIVE block via the Scheduler
        // accounting helpers (NOT the registration CAS directly — Addendum C).
        // Acquire global_mtx_ internally. Return the helper's CAS result.
        static bool retire_synthetic_select_timer(
            Scheduler& s, detail::SelectTimerRegistration& reg) {
            LockGuard lk(s.global_mtx_);
            return s.select_timer_retire_locked(reg);
        }
        static bool consume_synthetic_select_timer(
            Scheduler& s, detail::SelectTimerRegistration& reg) {
            LockGuard lk(s.global_mtx_);
            return s.select_timer_consume_locked(reg);
        }

        // Splice ONE caller-owned temporary node into select_timer_pool_ via the
        // REAL production helper (select_timer_splice_one_locked), returning the
        // stable Scheduler-owned address. For T2's pre/post-splice address-
        // identity proof only: a test captures &*it before the call, splices,
        // then asserts the returned pointer equals the captured address, the
        // temporary pool is empty, and the heap entry's Select target is that
        // same address. Mirrors the future admission protocol (caller-frame tmp
        // -> Scheduler pool) exactly. Acquires global_mtx_ internally and
        // reserves heap capacity before mutation.
        static detail::SelectTimerRegistration* splice_one_for_test(
            Scheduler& s,
            std::list<detail::SelectTimerRegistration>& tmp_pool,
            std::list<detail::SelectTimerRegistration>::iterator it) {
            LockGuard lk(s.global_mtx_);
            s.deadline_heap_.reserve(s.deadline_heap_.size() + 1);
            return s.select_timer_splice_one_locked(tmp_pool, it);
        }

        // Detached-object CAS authority for T1 (E13 P3 Corrective closure 3).
        // PRE: `reg` is NOT Scheduler-owned (never spliced into any pool) — it
        // is a stack-local SelectTimerRegistration exercising the registration's
        // own CAS state machine. The CAS methods are private; this guarded
        // entry is the only non-Scheduler way to reach them, and it exists
        // solely so T1 can test ACTIVE->{RETIRED,CONSUMED} + failed-CAS
        // transitions on detached locals without exposing the CASes in the
        // production target. Registered blocks MUST go through
        // retire_synthetic_select_timer / consume_synthetic_select_timer (the
        // Scheduler accounting helpers).
        static bool detached_try_claim_expiry(
            detail::SelectTimerRegistration& reg) noexcept {
            assert(reg.scheduler() == nullptr &&
                   "detached CAS accessor requires a never-registered registration");
            return reg.try_claim_expiry();
        }
        static bool detached_retire(
            detail::SelectTimerRegistration& reg) noexcept {
            assert(reg.scheduler() == nullptr &&
                   "detached CAS accessor requires a never-registered registration");
            return reg.retire();
        }

        // E13 P4 detached-group winner-CAS test entry. PRE: `group` is a
        // structural object that was never admitted/registered with any
        // Scheduler (scheduler_ == nullptr, arms_ == nullptr, arm_count_ == 0).
        // The winner CAS (SelectGroup::claim_winner_locked) is PRIVATE so a
        // registered group cannot bypass Scheduler::select_process_group_locked;
        // this guarded entry is the only non-Scheduler way to reach the CAS,
        // and it exists solely so P1 structural tests can prove first-claim-
        // wins / second-claim-loses on detached objects. The mechanical
        // detached precondition is ENFORCED here (not merely documented): a
        // group carrying arms or a scheduler binding is rejected before the CAS
        // (P4 §5.1: "Do not repeat the P3 mistake of documenting a test-only
        // precondition without enforcing it"). Defined out-of-line: the body
        // touches SelectGroup's complete definition (select_port.hpp), which is
        // NOT included by this installed header.
        static bool detached_claim_winner(detail::SelectGroup& group,
                                         std::uint32_t arm_index) noexcept;

        // ---- E13 P4 Select central claim + finalization test accessors ----
        // The P4 core is test-driven via direct seam calls (production-test-
        // plan.md §4 / §7.4). These acquire global_mtx_ internally and dispatch
        // to the Scheduler-locked core. No forgeable test authority; absent in
        // the production target.

        // Drive select_process_group_locked: validate + claim + finalize the
        // whole group for `candidate_index` without publication. Returns
        // whether THIS call won the claim. `group` must be a registered group
        // (scheduler_ == &s, arms_/arm_count_ set, arms linked/registered by
        // the test harness exactly as a future admission would).
        static bool select_process_group(Scheduler& s,
                                         detail::SelectGroup& group,
                                         std::uint32_t candidate_index);

        // Read the all-authority-closed invariant predicate (SN-10). False
        // before processing (no winner / open authority); true after a
        // successful process. A guarded test may also invoke it directly to
        // prove an open authority is rejected (OA death test).
        static bool select_all_authority_closed(const Scheduler& s,
                                                const detail::SelectGroup& group);

        // P4 OA corrective: invoke the all-authority-closed invariant as a
        // fail-fast assert (the mechanical precondition a future P6 publication
        // entry will gate on). Acquires global_mtx_ and asserts
        // select_all_authority_closed_locked(group). Used by the OA death case:
        // after a valid process (all authority closed) the test re-opens one
        // winner authority, then this assert must terminate the program,
        // proving the publication precondition is mechanically enforced — not
        // merely a bool predicate that could be ignored.
        static void assert_select_all_authority_closed(
            const Scheduler& s, const detail::SelectGroup& group);

        // Advance the test clock deterministically (drives the timer pump).
        static void advance_clock(Scheduler& s, deadline_t t);

        // Observation (diagnostics; defined out-of-line with a TSA suppression).
        static std::size_t select_timer_pool_size(
            const Scheduler& s) noexcept;
        static std::size_t select_timer_count_in_state(
            const Scheduler& s,
            detail::SelectTimerRegistration::State st) noexcept;
        // Tagged heap counts by kind: returns {ordinary_count, select_count}.
        static std::array<std::size_t, 2> tagged_heap_counts_by_kind(
            const Scheduler& s) noexcept;

        // Does any Select-kind heap entry target `target` (by address)? For
        // T2: proves the heap stores exactly the spliced block's address as
        // its stable Select pointer (the heap-by-stable-address contract).
        // Reads GUARDED_BY fields from a test coordinator for diagnostics;
        // not load-bearing for correctness.
        static bool deadline_heap_has_select_target(
            const Scheduler& s,
            const detail::SelectTimerRegistration* target) noexcept;

        // arm-load instrumentation (Addendum E): the count of times the
        // pump branch read reg.arm_ on an ACTIVE entry (the exact production
        // dereference site). Stale pops observe delta 0.
        static std::size_t select_timer_arm_load_count(
            const Scheduler& s) noexcept {
            return s.select_timer_arm_load_count_;
        }
        static void reset_select_timer_arm_load_count(Scheduler& s) noexcept {
            s.select_timer_arm_load_count_ = 0;
        }

        // Drain the Select timer pool to empty: retire every still-ACTIVE
        // block (via the Scheduler accounting helper, so counters stay
        // consistent), then advance the clock far past every deadline so the
        // pump physically reclaims each block. Used by test fixtures to honor
        // the ~Scheduler quiescence contract. Acquires/releases global_mtx_.
        static void drain_select_pool(Scheduler& s) {
            // Retire ACTIVE blocks under G (so advancing the clock later hits
            // the stale-skip path, not the ACTIVE fail-fast).
            {
                LockGuard lk(s.global_mtx_);
                for (auto& reg : s.select_timer_pool_) {
                    if (reg.is_active()) s.select_timer_retire_locked(reg);
                }
            }
            // Pump every Select entry to physical reclamation. A large deadline
            // covers all test fixtures' deadlines.
            s.advance_clock(static_cast<deadline_t>(1) << 62);
        }
    };
#endif  // defined(SLUICE_ASYNC_INTERNAL_TESTING)
};

}  // namespace sluice::async
