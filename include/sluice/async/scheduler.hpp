// sluice::async::Scheduler — multi-worker Evented scheduler.
//
// Each Worker owns its own scheduler Context + current Fiber. Fibers are
// pinned to their first-execution Worker; wake routing and multi-worker
// coordination are owned by the seams documented below.
//
// See docs/adr/ADR-execution-model.md §9.2 for the accepted worker contract.
#pragma once

#include <sluice/async/async_io_context.hpp>
#include <sluice/async/completion.hpp>
#include <sluice/async/detail/fail_fast.hpp>
#include <sluice/async/detail/queue_port.hpp>  // detail::QueuePort / QueueRole (Queue seams)
#include <sluice/async/detail/ready_sink.hpp>  // detail::SynchronousReadySink / ReadyEvent
#include <sluice/async/detail/select_registration.hpp>  // detail::DeadlineHeapEntry, SelectTimerRegistration
#include <sluice/async/select_fwd.hpp>  // select() template declaration + forward decls
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
class SelectResult;  // full definition lives in select.hpp (included by select.cpp)
class ApplicationRuntime;  // friend grant for the Fiber identity write seam.

namespace detail {
class SelectGroup;
class SelectPort;
struct SelectArmSlot;
enum class ArmState : std::uint8_t;
class SelectCaseDescriptor;  // full definition in select.hpp (sealed fields)
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
// Deterministic test causal seams (admission, park candidate/commit,
// clock/timer, event set/admission) are realized by a SEPARATE non-installed internal-
// testing runtime (`sluice_async_internal_testing`), compiled from the same
// authoritative sources with the private macro SLUICE_ASYNC_INTERNAL_TESTING
// defined. Only that variant links the test-support objects and the controller
// state; the production `sluice_async` target is hook-free and exports no test
// phase, no test seam state, and no test controller symbol. No binary links
// both runtime variants.
// ----------------------------------------------------------------------------

// External wake handle (ADR §9.4.10). A control-block-backed handle that
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
//     interleave with an in-flight callback.
//   - The producer MUST NOT call any other Scheduler method via this handle.
//   - The producer MUST NOT touch Scheduler queues, registrations, or Fibers.
//
// Refinement map: TLA+ ExternalReadyPublish (the signal half) ->
// SchedulerWakeHandle::notify -> Scheduler::signal_wake_locked. Callback
// lifetime: spec/tla/e9_wake_handle_lifetime/.
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
    // TEST-ONLY. Defined in scheduler_park_wake.cpp (Control is
    // completed in scheduler_internal.hpp).
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
    struct Control;  // shared control block (completed in scheduler_internal.hpp)
    explicit SchedulerWakeHandle(std::shared_ptr<Control> ctrl) noexcept
        : control_(std::move(ctrl)) {}
    std::shared_ptr<Control> control_;
};

// Run invocation lifetime contract (ADR §9.4.0). RunMode is
// an EXPLICIT invocation policy, separate from wake capability. It is the
// ONLY axis along which the idle action differs; the classifier,
// publication protocol, ownership protocol, and backend admission are
// shared (one worker loop, one classifier).
//
//   drain — Drain-mode semantics. MW-S3 returns STALLED; the run invocation
//           MUST NOT park merely because an external-wake-capable wait
//           exists. Existing callers and tests use Drain.
//   live  — explicit Live-mode entry. The run remains resident while an
//           unresolved wait has an effective Scheduler wake source
//           (MW-S3 + external-wake-capable may park). No effective wake
//           source still returns STALLED.
//
// A wake handle NEVER implicitly switches Drain <-> Live.
enum class RunMode : unsigned char {
    drain,
    live,
};

// Per-worker execution state. Each Worker thread owns one of these.
// The scheduler Context + current Fiber are NEVER shared across concurrent
// Workers. Accessed by the owning Worker thread without locks for the
// execution-state fields; local_runnable is concurrency-safe for cross-worker
// publication.
struct WorkerState {
    fiber_ctx::Context sched_ctx{};   // this worker's saved scheduler continuation
    // `current` is read cross-thread ONLY by the
    // internal-testing forensics dump; the atomic closes that data race
    // (Fiber::state() is itself atomic, so the dump's follow-up read is
    // defined). Same-thread uses (run_next_on, the await/select paths) are
    // unchanged by the atomic.
    std::atomic<Fiber*> current{nullptr};  // the Fiber currently running on this worker
    std::deque<Fiber*> local_runnable{};  // owner-local runnable queue
    unsigned id = 0;

    // Synchronization authority for cross-worker local_runnable publication
    // (spawn/spawn_on routing, route_runnable_locked, steal transfers, the
    // retire-rescue move) and for the park predicate's own-queue backstop
    // read (acquired inside wake_mtx_ — that one-way wake_mtx_->inbox_mtx
    // edge is load-bearing). Parked-worker notification authority is the
    // Scheduler wake domain (wake_epoch_/wake_cv_ via signal_wake_locked()),
    // NOT any per-worker condition_variable. The name is historical; renaming
    // is deferred to an on-touch pass.
    std::mutex inbox_mtx;
    std::atomic<bool> active{false};  // this worker is part of a coordinated run

    // Unified suspend-switch authority: closes the wake-before-physical-
    // context-switch window. Raised UNDER global_mtx_ by commit_suspend_locked BEFORE
    // make_waiting() — a resolver, which also needs global_mtx_, can therefore
    // never publish a Runnable ticket onto this worker's local_runnable
    // between Waiting-visibility and protection-publication (it MAY publish
    // later, after G release, while the physical context_switch is still in
    // flight). Cleared AFTER the context_switch returns control to the
    // scheduler continuation (run_next_on; the CPU context is then saved; no
    // lock held). try_steal reads this under global_mtx_ and refuses to steal
    // a ticket from a victim that is mid-suspension: resuming such a ticket on
    // a thief would re-enter a Fiber context whose rsp/rbp/rip have NOT yet
    // been saved by the in-flight switch. The raise happens inside the
    // global_mtx_ critical section, the clear outside any lock; the
    // release/acquire pair on the atomic establishes happens-before between
    // the clear-store and the thief's subsequent load.
    std::atomic<bool> suspend_switch_pending{false};

    // Park-admission per-worker state (ADR §9.4.2 / §9.4.5).
    // observed_epoch is the wake_epoch_ value observed at the instant this
    // worker COMMITTED to park (recorded under wake_mtx_). The cv.wait
    // predicate is "wake_epoch != observed_epoch OR wake-worthy persistent
    // state OR terminate" — this closes the commit-to-physical-wait window.
    // park_domain records which domain this worker is parked in (SCHEDULER
    // vs BACKEND) for diagnostics + the at-most-one-backend-participant
    // rule. Written by the owning worker outside any lock and read
    // cross-thread by the forensics dump — atomic, never a
    // plain field (a "best-effort" racy read is still UB).
    std::uint64_t observed_epoch{0};

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
    // Phase G park-window forensics: which worker_loop break path this
    // worker exited through (written once at exit, read by the dump after
    // the thread is gone). Atomic enum — the watchdog dump reads it from
    // another thread (a plain `const char*` would be a data race).
    enum class LoopExitReason : unsigned char {
        live,  // never written: the loop has not exited
        mw_s1_terminate_observed,
        mw_s2_no_progress_terminate,
        e14f1_last_idle_terminate,
        last_idle_terminate,
        final_park_terminate,
    };
    std::atomic<LoopExitReason> loop_exit_reason{LoopExitReason::live};
    // Causal seam evidence (G1 reproducer): set at worker_loop RETURN —
    // after this store the thread will never touch WorkerState again (the
    // run_impl thread lambda only clears `active`). Cleared when a fresh
    // invocation's thread attaches, so it always describes the CURRENT
    // worker thread, never a prior run's. A test waiting on this flag
    // knows the worker is causally dead, not merely "wrote its exit
    // reason" (the exit-reason write precedes the final Phase-D drain).
    std::atomic<bool> loop_exited{false};
    // Per-worker classify evidence (Phase G review P2a): the LAST
    // classify_locked result this worker observed, with a monotonic
    // sequence. The park ledger reads the parking worker's OWN pair, so a
    // park record attributes the classification the parking worker trusted
    // — a Scheduler-global value would mis-attribute under interleaved
    // classifies from other workers.
    std::atomic<int> last_classify{-1};  // 0 mw_s1, 1 mw_s2, 2 mw_s3, 3 quiescent
    std::atomic<std::uint64_t> classify_seq{0};
#endif
    enum class ParkDomain : unsigned char { None, Scheduler, Backend };
    std::atomic<ParkDomain> park_domain{ParkDomain::None};

    // Idle-dance backstop: 1 while this worker's +1 is
    // (believed to be) counted in Scheduler::idle_workers_ — set at the
    // dance fetch_add, cleared conservatively at each loop-iteration top
    // (an under-clear is safe: the park-commit check then treats the
    // worker's own stale count as another dancer's, refusing once more and
    // re-dancing toward convergence; it can never cause a MISSED refusal).
    // The park commit compares idle_workers_ AGAINST this value so a
    // counted dancer can sleep holding its count (the count's persistence
    // is what damps the not-last wake chain) while a worker that never
    // danced still refuses to park behind an unfinished dance.
    // Plain production atomic; read under global_mtx_ at the
    // park commit alongside idle_workers_.
    std::atomic<unsigned> idle_dance_contributed_{0};

    // Contribution-identity law: the value of
    // Scheduler::dance_epoch_ at the moment this worker's dance contribution
    // was made — loaded BEFORE the idle_workers_ fetch_add (see the
    // refinement argument on Scheduler::dance_epoch_). Meaningful only
    // while idle_dance_contributed_ == 1: the park commit refuses to arm a
    // baseline behind a contribution whose recorded identity is no longer
    // current (the count was orphaned by one of the three genuine erase
    // sites — the two unlocked ticketed paths or the route-publication
    // erase — so the run is one short of the last-idle threshold even
    // though this bit still claims a live contribution). Stale-record
    // false refusals are safe by the same conservative-convergence argument
    // as the R4 under-clear: the refusing worker re-loops, re-dances with a
    // fresh identity, and either reaches the last-idle threshold or parks
    // counted.
    // Plain production atomic; written in the dance under global_mtx_, read
    // at the park commit under global_mtx_ (the generation field itself is
    // never mutex-guarded — it is monotonic and its writers are unlocked by
    // design).
    std::atomic<std::uint64_t> dance_epoch_at_contribution_{0};

    // Owner Scheduler identity. Set exactly once when WorkerState is
    // attached to a Scheduler. Immutable by contract; not used for routing.
    Scheduler* owner_scheduler{nullptr};

    WorkerState() = default;
    WorkerState(const WorkerState&) = delete;
    WorkerState& operator=(const WorkerState&) = delete;
};

// Forward-declared so the AsyncTestAccess death-test accessors (under
// SLUICE_ASYNC_INTERNAL_TESTING) can name AsyncRwLock& without pulling the
// full public header into every Scheduler TU.
class AsyncRwLock;

class Scheduler {
public:
    // wait_capacity bounds the WaitRecord pool.
    // Records are preallocated at construction; acquire_wait_record_locked
    // returns nullptr (no_space) when the free list is exhausted. Default is
    // 256 (a fixed floor). Construction MAY allocate for the
    // pool and is therefore NOT noexcept — a failure is construction failure,
    // not a silent no-space Scheduler.
    explicit Scheduler(AsyncIoContext& ctx,
                       std::size_t wait_capacity = 256);
    ~Scheduler();

    Scheduler(const Scheduler&) = delete;
    Scheduler& operator=(const Scheduler&) = delete;
    Scheduler(Scheduler&&) = delete;
    Scheduler& operator=(Scheduler&&) = delete;

    // Initialize a Fiber's context so the scheduler can run it. The test owns
    // the stack; this wires the fiber's ctx to that stack + the bridge + &fiber.
    bool init_fiber(Fiber& fiber, std::byte* stack_base, std::size_t stack_size);

    // Enqueue a Fiber as runnable. Round-robin assignment to a Worker
    // (single-worker compatible: assigns to worker 0).
    // An active-run publication advances the Scheduler wake
    // epoch (same obligation as every runnable publication) so a worker
    // already parked on the wake domain observes the new ticket even when
    // the assigned target worker is busy inside a Fiber.
    void spawn(Fiber& fiber) noexcept;

    // Test seam: spawn `fiber` directly onto worker `worker_id`'s
    // local_runnable. Narrow deterministic-test hook; exposes no public
    // production contract. Used by deterministic tests to place a victim on
    // a specific worker without the round-robin nondeterminism of spawn().
    // Records the owner. Publishes the same Scheduler wake obligation as
    // spawn().
    void spawn_on(Fiber& fiber, unsigned worker_id) noexcept;

    // Run the scheduler with `worker_count` worker threads until global idle
    // (coordinated run, DRAIN mode). `worker_count` must be >= 1. With 1
    // worker, this is the single-worker path. Drain
    // semantics (ADR §9.4.0): MW-S3 returns STALLED; the run does NOT park
    // merely because an external-wake-capable wait exists.
    void run(unsigned worker_count);

    // Explicit LIVE entry. Same worker loop and classifier as
    // run(); differs ONLY at the idle-action selection boundary. In Live, an
    // unresolved MW-S3 wait with an effective Scheduler wake source may keep
    // the run resident (park). MW-S3 without an effective wake source still
    // returns STALLED. Used by the no-re-entry external-wake
    // proofs. See ADR §9.4.0 / §9.4.3.
    void run_live(unsigned worker_count);

    // Group-scoped Live invocation. Identical to run_live(worker_count)
    // EXCEPT the MW-S3 park decision additionally checks a caller-supplied
    // invocation stop predicate. If the predicate returns true, the run
    // terminates (returns to the caller) instead of parking, even if unrelated
    // Scheduler registrations would otherwise keep it resident.
    //
    // stop_fn(stop_ctx) is called under global_mtx_ at the MW-S3 boundary.
    // It MUST NOT acquire global_mtx_ (already held). It MAY acquire the
    // caller's own lock (e.g. Group::mtx_) — no inversion because the caller
    // never holds its own lock while calling into Scheduler.
    //
    // The predicate's lifetime must span the entire run invocation (it is
    // stored as a raw pointer pair, not copied). Pass nullptr to disable.
    void run_live(unsigned worker_count, bool (*stop_fn)(void*), void* stop_ctx);

    // Legacy single-worker entry point. Delegates to run(1) (Drain).
    void run_until_idle() { run(1); }

    // ---- Suspension primitives (called from Fiber bodies) ----
    // The Completion wait registers a real Scheduler routing record + arena
    // waiter (ADR Decision 10) instead of a Completion*-keyed map entry;
    // the wake arrives through the identity-bearing ReadySink instead of an
    // O(N) ready() re-scan.
    //
    // Returns:
    //   - success        — the Completion reached its terminal result (already
    //                      ready at registration, or the fiber was resumed by
    //                      the identity route); read c.result() then c.reset().
    //   - invalid_state  — synchronous rejection: a second waiter is already
    //                      registered on this Completion, or the
    //                      Completion is not bound to this context's backend
    //                      (cross-context / idle / stale — provenance misuse,
    //                      Decision 6). The fiber is NOT suspended.
    //   - canceled       — the wait was cancelled by Scheduler::cancel_waiter
    //                      while the fiber was suspended; the I/O continues
    //                      (wait-cancel ≠ I/O cancel). The Completion is NOT
    //                      ready; reset it only after the I/O completes and
    //                      reaps (borrow lifetime, Decision 8).
    Result<void> await_completion_size(Completion<std::size_t>& c);
    Result<void> await_completion_void(Completion<void>& c);
    void await_ready_flag(const std::atomic<bool>& ready);

    // ---- Waiter cancellation (ADR Decision 10) ----
    // Removes ONLY the waiter registration for `c` (arena cancel_waiter) and
    // retires the Scheduler routing record. It NEVER cancels the I/O, never
    // ends the fd/buffer borrow, and never chooses a terminal result.
    // Returns:
    //   - true   — this call removed the waiter (cancel won); the suspended
    //              fiber is resumed exactly once with the wait-cancelled
    //              outcome (its await_completion returns IoError::canceled).
    //   - false  — the reap delivery already won (the waiter registration was
    //              already closed); the fiber is (or will be) resumed by the
    //              identity route with the Completion ready. Nothing removed.
    //   - not_found — the Completion is not bound to this context's backend.
    Result<bool> cancel_waiter(Completion<std::size_t>& c);
    Result<bool> cancel_waiter(Completion<void>& c);

    // ---- WaitQueue suspension ----
    // Suspend the calling Fiber on `q`, registering `node`. `node` must be
    // Detached (fresh) and outlive this call's suspend until it is resumed
    // (woken or cancelled). The fiber resumes when EXACTLY ONE resolver wins:
    //   - wake_wait_one(q) resolves the head with Woken, OR
    //   - cancel_wait(q, node) resolves this node with Cancelled.
    // The winner transition is the WaitNode resolve CAS (one authority, §2/§7);
    // the loser performs no second wake. Cancellation here is wait-cancellation
    // ONLY (not task / I/O cancellation).
    //
    // FIBER LIFETIME. `await_wait` is called by the
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
    // Cancelled). A losing call (node already woken) is a no-op. WaitQueue
    // cancel is wait-cancellation only: it does NOT cancel the task/fiber/I/O op.
    bool cancel_wait(WaitQueue& q, WaitNode& node);


    // ---- Deadline / timer wait ----
    // The Deadline type: a monotonic absolute time point. The protocol
    // authority is the absolute deadline (`expired iff now >= deadline`); a
    // relative duration is converted to a deadline by the caller via
    // `monotonic_now() + duration`. Wall-clock time never participates.
    using deadline_t = deadline_tick_t;

    // Read the Scheduler's monotonic clock. Production: steady_clock ticks
    // since process start. Tests: a controllable logical clock advanced by
    // advance_clock() (the deterministic causal seam).
    deadline_t monotonic_now() const noexcept;

    // TEST-ONLY causal seam: advance the controllable logical clock to
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
    // before suspension (the already-due admission closure). The fiber never
    // suspends merely because timer registration happened after the deadline
    // was due.
    //
    // A TimerRegistration control block (independently-stable retirement
    // state) is created for this wait epoch and owned by the Scheduler. A
    // non-timer winner retires it in the SAME critical section that resolves
    // the node, before runnable publication — so a stale expiry cannot
    // dereference the destroyed WaitNode (the timer-lifetime closure).
    void await_wait_deadline(WaitQueue& q, WaitNode& node, deadline_t deadline);

    // Resolve `node` (registered in `q`) with Expired and route the winner's
    // fiber. The third resolution cause, reached when the bound deadline
    // elapses. Returns true iff this call won (the node was Registered and is
    // now Expired). A losing call (node already woken/cancelled, or retired)
    // is a no-op. Mirrors wake_wait_one / cancel_wait exactly:
    // global_mtx_ + q.mtx() -> resolve_(Expired) -> unlink_locked ->
    // --waiting_waitq_count_ -> make_runnable + route_runnable_locked. NEVER
    // a parallel timer-wake publication path.
    bool expire_wait(WaitQueue& q, WaitNode& node);


    // ---- Event wait admission / broadcast ----
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
    // in whatever state the registered waiters left it). Also performs the
    // readiness-offer Select scan on `select_port` inside the same
    // global_mtx_ CS.
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

    // Deadline-aware Event wait. Composes await_event_wait with a
    // deadline TimerRegistration. The wait resolves when EXACTLY ONE cause
    // wins the resolve_ CAS: set() broadcast (Woken), cancel_wait
    // (Cancelled), or the deadline elapsing (Expired). If set_ is observed
    // at admission, resolves Woken inline (no suspend). If the deadline is
    // already due at admission, the already-due path resolves Expired inline
    // (no suspend). The deadline registration is retired by a non-timer
    // winner in the same CS.
    //
    // Deadline precedence: at admission, Event SET readiness
    // is checked BEFORE the already-due deadline predicate. Therefore Event SET
    // + already-due deadline -> Woken inline (the resource is ready; the
    // deadline is moot). This is the accepted production behavior.
    void await_event_wait_deadline(WaitQueue& q, const std::atomic<bool>& set_flag,
                                   WaitNode& node, deadline_t deadline);

    // The narrow Event cancellation authority with
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

    // ---- FE shared wait admission ladder disposition (FE-1c; ONE textual
    // ladder per primitive direction) ----
    // The register → SET-precedence → already-due → terminal-recheck ladder
    // shared by EVERY Event wait frontend. Caller holds G + q.mtx(). Binds the
    // frontend's ResumeTarget token, evaluates Event precedence exactly once,
    // and returns the disposition; the ENTRY (fiber wrapper / deferred
    // awaiter) commits its own PublicationEligibility in the SAME critical
    // section when `authorized` (fiber: commit_suspend_locked; deferred:
    // frontend record arm) and then performs its physical suspension outside
    // the lock. No duplicated admission law exists: this is the only textual
    // ladder (FE-1c verdict).
    enum class WaitAdmitDisposition : std::uint8_t {
        rejected,         // registration contract violation; epoch untouched
        resolved_inline,  // epoch terminal at admission (woken/expired);
                          // consume the outcome inline — publish nothing (L6)
        authorized,       // suspension authorized; commit eligibility in THIS CS
    };
    WaitAdmitDisposition event_wait_admit_locked(WaitQueue& q,
                                                 const std::atomic<bool>& set_flag,
                                                 WaitNode& node,
                                                 const WaitResume& resume,
                                                 bool timed,
                                                 deadline_t deadline)
        SLUICE_REQUIRES(global_mtx_, q.mtx());


    // ---- Semaphore admission / release ----
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
    // admission closure with a deadline TimerRegistration. Mandatory
    // precedence (under global_mtx_ + waiters_.mtx()):
    //   1. authoritative permit admission (if available > 0 AND node is FIFO
    //      head): resolve Woken inline (no suspend). Permit admission wins over
    //      a due deadline.
    //   2. else if the deadline is already due: resolve Expired inline.
    //   3. else: commit suspension.
    // For a registered timed wait, RESOURCE_WAKE (release) / TIMER_EXPIRE /
    // CANCEL compete through the existing exactly-once WaitNode resolution
    // authority. Reuses the deadline timer registration/retirement. No
    // Semaphore-local deadline mechanism.
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


    // ---- AsyncMutex admission / handoff ----
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
    // closure with a deadline TimerRegistration. Mandatory precedence (under
    // global_mtx_ + waiters_.mtx()):
    //   1. authoritative ownership admission (owner free AND node is FIFO head):
    //      resolve Woken inline (no suspend). Ownership admission wins over a
    //      due deadline (resource-first).
    //   2. else if the deadline is already due: resolve Expired inline.
    //   3. else: commit suspension.
    // For a registered timed wait, RESOURCE_WAKE (unlock handoff) /
    // TIMER_EXPIRE / CANCEL compete through the existing exactly-once WaitNode
    // resolution authority. Reuses the deadline timer registration/retirement.
    // No Mutex-local deadline mechanism.
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


    // ---- AsyncCondition ----
    // The Fiber-suspending condition variable substrate. An AsyncCondition owns
    // a private Condition WaitQueue `cond_waiters` and is bound to one AsyncMutex
    // (whose `mutex_waiters` + `owner` are passed BY REFERENCE, exactly as the
    // Mutex's own methods pass them into the Mutex seams above). These NARROW
    // private seams implement the CONDITION-WAIT-PREPARE combined step (register
    // Condition node + release/handoff bound Mutex + make_waiting, under one
    // global_mtx_ CS), the Condition notify_one/notify_all resolution, the
    // queue-identity-gated Condition cancel, and the deadline-aware variant.
    //
    // Synchronization domain: same as the Event/Semaphore/Mutex seams —
    // global_mtx_ (and the
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
    // false on the registration-failure path (the Mutex was NOT released —
    // the caller retains ownership and must run NO reacquire epoch), and true
    // after the Mutex has been released/handed off (the caller MUST run the
    // reacquire epoch). The untimed path has no inline-Expired-at-admission
    // branch (no deadline), so every non-registration-failure path releases
    // the Mutex.
    WaitOutcome condition_wait_prepare(WaitQueue& cond_waiters, WaitNode& cond_node,
                                       WaitQueue& mutex_waiters, Fiber*& owner,
                                       bool& released_mutex);

    // FE-3 shared CONDITION-WAIT-PREPARE admission ladder: the ONE textual
    // admission law for the Condition epoch (blocking+timed), parameterized by
    // the frontend's WaitResume token. Caller MUST hold global_mtx_; the
    // Condition queue mtx is taken and released INSIDE (sequential topology —
    // the Condition queue mtx and the Mutex queue mtx are NEVER held
    // simultaneously), and the Mutex handoff acquires the Mutex queue mtx
    // internally, exactly as the fiber entries did before the extraction.
    //
    // Ladder: [timed: R2-ALLOC prepare -> register -> LOCAL timer publish]
    // -> [untimed: register] -> already-due inline Expired (Mutex RETAINED)
    // -> register-before-handoff phase seam -> Mutex handoff (the ONE accepted
    // mutex_handoff_one_locked; owner committed BEFORE publication) ->
    // terminal recheck -> authorized. The entry commits its own
    // PublicationEligibility in the SAME global_mtx_ CS on `authorized`
    // (fiber: commit_suspend_locked; deferred: record.arm()) and physically
    // suspends outside the lock.
    //
    // The disposition encodes the caller's REACQUIRE obligation (the
    // released_mutex law), so the fiber entries and the deferred seam derive
    // it from one source:
    //   rejected_retain           registration failed; Mutex retained; no
    //                             suspend; the node's outcome is latched.
    //   resolved_inline_retain    already-due deadline resolved Expired
    //                             inline; Mutex retained; no suspend; no
    //                             reacquire epoch (InvDueInlineRetains
    //                             Ownership).
    //   resolved_inline_released  the node was resolved concurrently before
    //                             suspension (post-handoff recheck); Mutex
    //                             RELEASED; no suspend; the caller MUST run
    //                             the reacquire epoch.
    //   authorized                registered (+ timer published when timed)
    //                             and the Mutex released/handed off; the
    //                             caller suspends and MUST run the reacquire
    //                             epoch after resume.
    enum class ConditionAdmitDisposition : std::uint8_t {
        rejected_retain = 0,
        resolved_inline_retain = 1,
        resolved_inline_released = 2,
        authorized = 3,
    };
    ConditionAdmitDisposition condition_wait_admit_locked(
        WaitQueue& cond_waiters, WaitNode& cond_node, const WaitResume& resume,
        WaitQueue& mutex_waiters, Fiber*& owner, bool timed,
        deadline_t deadline)
        SLUICE_REQUIRES(global_mtx_);

    // Deadline-aware CONDITION-WAIT-PREPARE. Composes condition_wait_prepare
    // with a deadline TimerRegistration on `cond_node`
    // (deadline governs ONLY the Condition epoch). Admission precedence (under
    // global_mtx_):
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

    // ---- Queue wait admission + reconciliation ----
    // The Queue blocking/timed substrate. A QueuePort owns a producer and a
    // consumer WaitQueue (waiters_[2]); the Scheduler is the authoritative
    // resolution + publication executor, exactly as for the other
    // synchronization seams. ALL seams
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

    // Blocking push admission. Registers the producer node on the
    // producer role FIFO under G + S + producer.mtx(); admission recheck
    // commits the item to the ring inline if space opened with no older
    // producer; otherwise suspends. On return, `lease` is:
    //   - empty if committed (the ring slot owns the control now);
    //   - non-empty (original) if closed/expired/cancelled.
    // The caller MUST have detached->producer_operation the control BEFORE
    // calling and pass its lease by reference.
    void queue_push_admit(detail::QueuePort& port, WaitNode& node,
                          detail::QueueItemLease& lease);
    // Blocking pop admission. Symmetric: consumer node on the consumer
    // role FIFO; admission recheck pops an item inline if one arrived with no
    // older consumer; otherwise suspends. On return, `out` is:
    //   - non-empty (the popped item's lease) if an item was granted;
    //   - empty if closed+empty (the caller returns `closed`).
    void queue_pop_admit(detail::QueuePort& port, WaitNode& node,
                         detail::QueueItemLease& out);
    // Deadline-aware variants. Resource-first admission wins over a
    // due deadline; an already-due deadline with no admissible resource
    // resolves Expired inline. On expired, the push caller's `lease`
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

    // ---- FE shared Queue admission ladders (FE-3; ONE textual ladder per
    // direction) ----
    // The prepare(timed) -> register -> counters -> LOCAL timer publish ->
    // resource-precedence -> closed -> already-due admission sequence shared by
    // EVERY push (resp. pop) frontend. Caller holds G + S + the direction's
    // role mtx(). Binds the frontend's ResumeTarget token, evaluates Queue
    // admission precedence exactly once, and returns the disposition; the
    // ENTRY (fiber admit / deferred awaiter) commits its own
    // PublicationEligibility in the SAME critical section when `authorized`
    // (fiber: commit_suspend_locked; deferred: frontend record arm) and then
    // performs its physical suspension outside the lock. No duplicated
    // admission law exists per direction.
    //
    // The LOCAL timer publish below the ladder is the AC-2b review-corrective
    // Queue interleave (active_queue_timers_ bumped BEFORE the ACTIVE count /
    // heap / cache publication; earliest_active_deadline_ is read lock-free by
    // parked workers) and is preserved verbatim inside the ladder.
    enum class QueueAdmitDisposition : std::uint8_t {
        rejected,  // registration contract violation; epoch untouched
        resolved_inline,  // epoch terminal at admission (committed / closed /
                          // expired) with NO ring-occupancy change; consume
                          // the outcome inline — publish nothing (L6)
        resolved_inline_grant,  // ring-occupancy-changing inline success; the
                                // ENTRY must run the Q-LIV-1 opposite-role
                                // grant under G + S AFTER releasing the role
                                // mutex, then consume the outcome inline (L6)
        authorized,  // suspension authorized; commit eligibility in THIS CS
    };
    QueueAdmitDisposition queue_push_admit_locked(detail::QueuePort& port,
                                                  detail::QueueItemLease& lease,
                                                  WaitNode& node,
                                                  const WaitResume& resume,
                                                  bool timed,
                                                  deadline_t deadline)
        SLUICE_REQUIRES(global_mtx_, port.state_mtx_, port.waiters_[0].mtx());
    QueueAdmitDisposition queue_pop_admit_locked(detail::QueuePort& port,
                                                 detail::QueueItemLease& out,
                                                 WaitNode& node,
                                                 const WaitResume& resume,
                                                 bool timed,
                                                 deadline_t deadline)
        SLUICE_REQUIRES(global_mtx_, port.state_mtx_, port.waiters_[1].mtx());

    // FE winner publication for the Queue grant seams: the ONE textual
    // publication tail parameterized by the winner's ResumeTarget kind. Fiber
    // kind keeps the stackful route verbatim (exactly-once make_runnable
    // guard -> the port's granted_not_resumed_ pair -> owner routing).
    // Deferred kind commits the delivery obligation and deliberately does NOT
    // touch granted_not_resumed_: that counter pairs the grant-side increment
    // with the fiber winner's post-resume decrement under G, and a deferred
    // winner's resume (transit-list discharge) never touches the port again —
    // the coroutine frame carries its QueueWaitCtx + lease/out. The in-flight
    // deferred window is owned by the Scheduler-level deferred_publications_
    // teardown gate instead; QueuePort::begin_teardown may legitimately
    // proceed once both role FIFOs drain (the post-resume body reads frame
    // memory only). None kind publishes nothing (the old null-Fiber skip).
    void queue_publish_winner_locked(detail::QueuePort& port, WaitNode& won)
        SLUICE_REQUIRES(global_mtx_);

    // ---- Queue reconciler grant seams (winner-before-publication) ----
    // Called by QueuePort fast paths (try_push / try_pop / close) AND by the
    // blocking/timed admit inline-success paths (scheduler_queue.cpp, Q-LIV-1)
    // under G + S (caller-held; the admit releases its own role mutex first —
    // the two role mutexes are never held together) to grant the
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

    // Queue teardown precondition helper. Reports whether BOTH role FIFOs
    // of `port` are empty (no producer parked, no consumer parked). Called by
    // QueuePort::begin_teardown under global_mtx_; the QueuePort itself is not
    // a friend of WaitQueue (only the Scheduler is), so the emptiness query is
    // the Scheduler's authority. Each role.mtx() is taken sequentially under
    // global_mtx_ (the canonical lock order G -> exactly one role); the two
    // role mutexes are NEVER held together. Returns true iff both FIFOs have
    // no linked WaitNode at the instant of observation.
    bool queue_role_waiters_empty_locked(detail::QueuePort& port)
        SLUICE_REQUIRES(global_mtx_);

    // Queue timer-counter on-resolve thunk. A
    // Queue-bound TimerRegistration installs this as its on_resolve_ hook +
    // `&port` as owner_ctx_ at admit time. The Scheduler fires it exactly once
    // per ACTIVE->terminal timer transition (pump on consume,
    // retire_timer_for_node_locked on retire) under global_mtx_. It is a STATIC
    // MEMBER (not a free function) so it can reach QueuePort's private
    // active_queue_timers_ counter via Scheduler's friend grant. The signature
    // matches TimerRegistration::OnResolveFn.
    static void queue_timer_on_resolve(void* owner_ctx, bool timer_won) noexcept;


    // ---- AsyncRwLock ----
    // Writer-fair, phase-batched Read-Write Lock seams. AsyncRwLock owns a
    // single unified FIFO WaitQueue (readers + writers) and passes its state
    // BY REFERENCE into these Scheduler seams. ALL authoritative RwLock state
    // mutations occur under global_mtx_ -> waiters_.mtx() (same lock order as
    // the other synchronization seams). No RwLock-local lock is introduced.
    //
    // The unified grant helper (rwlock_grant_from_head_locked) dispatches to
    // reader batch grant or single writer grant depending on the queue head
    // mode. Cancel and expiry perform head reconcile after unlink.

    // Attempt inline read acquisition. Under G + W: if writer_active == false
    // AND waiters is empty: active_readers++; return true. Else false.
    [[nodiscard]] bool rwlock_try_read_lock(WaitQueue& waiters,
                                            std::size_t& active_readers,
                                            bool& writer_active);

    // Blocking read admission. Register node at FIFO tail, recheck via
    // grant_from_head_locked. If granted inline: return (node Woken). Otherwise
    // suspend. Internally manages RwWaitCtx{mode=read} on node.user().
    void rwlock_read_lock(WaitQueue& waiters, std::size_t& active_readers,
                          bool& writer_active, WaitNode& node);

    // Deadline-aware read admission (resource-first precedence).
    // `expire_ctx` is stored on the TimerRegistration for pump routing.
    void rwlock_read_lock_until(WaitQueue& waiters, std::size_t& active_readers,
                                bool& writer_active, WaitNode& node,
                                deadline_t deadline, void* expire_ctx);

    // Writer ownership identity (FE-3): `writer_owner` carries the holder's
    // ActorId (FE-1b A1) — NOT the ResumeTarget delivery token. The Fiber
    // frontend binds ActorId::fiber(current Fiber) (coincident with the
    // historical Fiber* identity); a stackless frontend binds its own stable
    // actor token. Recursive detection, non-owner unlock, and the grant-time
    // ownership commit all compare/commit ACTOR identity, so ownership
    // semantics do not depend on where control resumes.

    // Attempt inline write acquisition. Under G + W: if active_readers == 0
    // AND writer_active == false AND waiters empty AND current Fiber exists
    // AND writer_owner != ActorId::fiber(me): commit ownership; return true.
    // Recursive call by the current owner actor returns false. External
    // thread: forbidden.
    [[nodiscard]] bool rwlock_try_write_lock(WaitQueue& waiters,
                                             std::size_t& active_readers,
                                             bool& writer_active,
                                             ActorId& writer_owner);

    // Blocking write admission. Register node at FIFO tail, recheck via
    // grant_from_head_locked. If granted inline: return (node Woken). Otherwise
    // suspend. Internally manages RwWaitCtx{mode=write, actor} on node.user().
    void rwlock_write_lock(WaitQueue& waiters, std::size_t& active_readers,
                           bool& writer_active, ActorId& writer_owner,
                           WaitNode& node);

    // Deadline-aware write admission (resource-first precedence).
    // `expire_ctx` is stored on the TimerRegistration for pump routing.
    void rwlock_write_lock_until(WaitQueue& waiters, std::size_t& active_readers,
                                 bool& writer_active, ActorId& writer_owner,
                                 WaitNode& node, deadline_t deadline,
                                 void* expire_ctx);

    // Release one read share. Under G + W: active_readers--; if reaches 0,
    // reconcile queue head via grant_from_head_locked. Allows external thread.
    void rwlock_unlock_read(WaitQueue& waiters, std::size_t& active_readers,
                            bool& writer_active, ActorId& writer_owner);

    // Release exclusive write ownership. Under G + W: verify writer_active
    // AND writer_owner == ActorId::fiber(current Fiber) (named fail-fast in
    // Debug AND Release on violation — the historical debug-only asserts),
    // clear writer state, reconcile queue head.
    void rwlock_unlock_write(WaitQueue& waiters, std::size_t& active_readers,
                             bool& writer_active, ActorId& writer_owner);

    // Queue-identity-safe RwLock cancellation with head reconcile. Returns true
    // ONLY if node is Registered AND linked in `waiters` AND CANCEL wins. After
    // unlink, calls grant_from_head_locked to advance the queue. Safe from any
    // OS thread.
    [[nodiscard]] bool rwlock_cancel(WaitQueue& waiters,
                                     std::size_t& active_readers,
                                     bool& writer_active, ActorId& writer_owner,
                                     WaitNode& node);

    // RwLock-specific deadline expiry with head reconcile. Called ONLY by
    // pump_deadlines_locked (under G). Resolves the node Expired, unlinks,
    // performs head reconcile (grant acquires W internally), publishes.
    // Returns true iff this call won the resolve CAS (expire_locked succeeded).
    // A losing race (concurrent resolver won) returns false; the caller must
    // NOT increment its won counter in that case.
    bool rwlock_expire_wait(WaitQueue& waiters, std::size_t& active_readers,
                            bool& writer_active, ActorId& writer_owner,
                            WaitNode& node)
        SLUICE_REQUIRES(global_mtx_);

    // ---- FE shared RwLock admission ladders (FE-3; ONE textual ladder per
    // mode) ----
    // The prepare(timed) -> register -> counters -> ordinary-deadline publish
    // -> head-prefix claim -> already-due -> terminal-recheck sequence shared
    // by EVERY RwLock read (resp. write) frontend. Caller holds G + W. The
    // ENTRY commits its own PublicationEligibility in the SAME critical
    // section when `authorized` and performs physical suspension outside the
    // lock. `actor` is the caller's ActorIdentity (stashed by the entry into
    // its frame RwWaitCtx for the grant-time ownership commit; read mode
    // ignores it — v1 has no per-reader identity).
    WaitAdmitDisposition rwlock_read_admit_locked(WaitQueue& waiters,
                                                  std::size_t& active_readers,
                                                  bool& writer_active,
                                                  WaitNode& node,
                                                  const WaitResume& resume,
                                                  bool timed,
                                                  deadline_t deadline,
                                                  void* expire_ctx)
        SLUICE_REQUIRES(global_mtx_, waiters.mtx());
    WaitAdmitDisposition rwlock_write_admit_locked(WaitQueue& waiters,
                                                   std::size_t& active_readers,
                                                   bool& writer_active,
                                                   ActorId& writer_owner,
                                                   WaitNode& node,
                                                   const WaitResume& resume,
                                                   const ActorId& actor,
                                                   bool timed,
                                                   deadline_t deadline,
                                                   void* expire_ctx)
        SLUICE_REQUIRES(global_mtx_, waiters.mtx());

    // Shared inline try-write admission core (ONE textual decision: recursive
    // owner-actor detection + free-resource commit). Caller holds G + W.
    // Both the Fiber entry (actor = current Fiber) and the test-only deferred
    // seam (actor = frontend token) consume it — no duplicated ownership law.
    bool rwlock_try_write_admission_locked(WaitQueue& waiters,
                                           std::size_t& active_readers,
                                           bool& writer_active,
                                           ActorId& writer_owner,
                                           const ActorId& caller)
        SLUICE_REQUIRES(global_mtx_, waiters.mtx());

    // Shared checked write-release core (ONE textual ownership check +
    // release + head reconcile). Caller holds G. The ownership checks are
    // named fail-fasts (Debug AND Release): not write-active, or caller
    // actor != writer_owner. Used by the Fiber entry and the test-only
    // deferred seam.
    void rwlock_unlock_write_core_locked(WaitQueue& waiters,
                                         std::size_t& active_readers,
                                         bool& writer_active,
                                         ActorId& writer_owner,
                                         const ActorId& caller)
        SLUICE_REQUIRES(global_mtx_);


    // ---- External wake source (ADR §9.4) ----
    // Issue a generation-validated wake handle. The holder may call notify()
    // from an external thread to wake a parked Worker whose wake set includes
    // the Scheduler wake source. Safe across Scheduler destruction. The handle
    // is bound to this Scheduler and invalidated (notify becomes a no-op) on
    // destruction.
    SchedulerWakeHandle make_wake_handle() noexcept;

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
        // Completion waits live in the wait registry (live records);
        // flag/WaitQueue waits remain counted here. G -> registry is the
        // documented lock order.
        LockGuard rlk(wait_registry_mtx_);
        return wait_record_live_count_ + waiting_size_.size() + waiting_void_.size() +
               waiting_ready_.size() + waiting_waitq_count_;
    }
    std::size_t waiting_ready_count() const {
        LockGuard lk(global_mtx_);
        return waiting_ready_.size();
    }

    // Diagnostic: the id of the Worker the caller is currently
    // running on (TLS g_worker), or -1 if not on a worker thread. Safe to
    // call from a Fiber body. Used by tests to record which worker executed
    // a Fiber without racing a non-atomic std::thread::id across threads.
    static unsigned current_worker_id() {
        WorkerState* w = current_worker();
        return w ? w->id : static_cast<unsigned>(-1);
    }

    // READ the execution-identity tag from the current Fiber
    // (or nullptr if not inside a Fiber body). Unlike thread_local, this tag
    // survives Fiber suspend/resume. The ApplicationRuntime task wrapper stores
    // its `this` pointer here to detect worker-call detection. The read path is
    // public for narrow introspection (is_runtime_task).
    static void* current_fiber_execution_tag() {
        WorkerState* w = current_worker();
        Fiber* cur = w ? w->current.load(std::memory_order_acquire) : nullptr;
        return cur ? cur->execution_tag() : nullptr;
    }

private:
    // WRITE authority for the Fiber execution-identity tag. This is the
    // ONLY public-reachable write path, and it is private: callable solely by
    // ApplicationRuntime (the friend grant above), which sets/restores the tag
    // around user task code. Ordinary application code cannot reach it, so it
    // cannot clear, replace, or forge the Runtime identity to bypass
    // is_runtime_task() self-close detection. Do NOT make this public.
    friend class ApplicationRuntime;
    static void set_current_fiber_execution_tag(void* tag) {
        WorkerState* w = current_worker();
        if (w) {
            if (Fiber* cur = w->current.load(std::memory_order_acquire)) {
                cur->set_execution_tag(tag);
            }
        }
    }

public:

    // Diagnostic: the CURRENT execution owner of `f`, or nullptr if
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
    friend class SchedulerWakeHandle;  // notify() -> notify_external_wake
    // QueuePort reaches global_mtx_, wake_wait_one_locked, and the
    // queue admit/cancel seams to reconcile the OTHER role on fast-path
    // success. Mirrors how Scheduler friended nothing for Mutex/Semaphore
    // (those pass their private waiters_ by reference); the Queue needs
    //Scheduler-internal wake + global-mtx access for its reconciler.
    friend class ::sluice::async::detail::QueuePort;

    // Friend the pre-declared constrained public select()
    // template (declared in select_fwd.hpp, defined in select.hpp). By
    // friending a concrete function-template entity (not a concrete struct
    // name), an ordinary production TU cannot forge the friend grant: the
    // template is uniquely identified by its template-head + requires clause +
    // signature, and no other definition can match that entity.
    //
    // The template calls select_admit directly (no intermediate SelectBridge).
    // select_admit stays private to all other code.
    template <class... Cases>
        requires (
            sizeof...(Cases) >= 1 &&
            sizeof...(Cases) <= kSelectMaxArms &&
            (SelectCaseType<Cases> && ...)
        )
    friend SelectResult select(Scheduler& scheduler, Cases&&... cases);

    // ---- Select registry operations (private Scheduler authority) ----
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
    // Returns the number of arms marked. Readiness-offer only; no winner
    // claim, no finalization, no publication.
    std::size_t select_event_scan_locked(Event& event)
        SLUICE_REQUIRES(global_mtx_);

    // Wait registration with owner Worker. Used by the ready-FLAG waits
    // (waiting_ready_); the Completion waits live in the identity-bearing
    // WaitRecord registry.
    struct WaitReg {
        Fiber* fiber;
        WorkerState* owner;
    };

    // ---- Identity-bearing waiter routing (ADR Decisions 9/10) ----
    // One routing record per registered Scheduler Completion-waiter. Records
    // are address-stable for the Scheduler lifetime (never destroyed; reused
    // from a free list with a generation bump so a stale token cannot match a
    // new occupant). The routing lease pins the record: the record is
    // NOT retired while a request slot or the synchronous ReadySink owns its
    // lease, and it is consumed exactly once by the winning delivery path
    // (reap sink -> delivered -> drain, or waiter cancel -> cancelled).
    //
    // State machine (guarded by wait_registry_mtx_):
    //   free -> registered        registration (under global_mtx_)
    //   registered -> delivered   ReadyRoutingSink (during poll/wait_one)
    //   registered -> cancelled   cancel_waiter winner (under global_mtx_)
    //   delivered -> free         drain consume (under global_mtx_)
    enum class WaitRecordState : std::uint8_t {
        free,
        registered,
        delivered,
        cancelled,
    };

    struct WaitRecord {
        std::uint32_t index = 0;         // self index (free-list return)
        std::uint32_t generation = 0;    // bumped on reuse before visibility
        WaitRecordState state = WaitRecordState::free;
        Fiber* fiber = nullptr;          // caller-owned; valid while suspended
        WorkerState* owner = nullptr;    // address-stable across runs
        const void* completion = nullptr;  // diagnostics / cancel identity
        WaitRecord* next_delivered = nullptr;  // intrusive delivered-list link
        WaitRecord* next_free = nullptr;       // intrusive free-list link
    };

    // The Scheduler-owned identity-bearing ReadySink. The arena
    // invokes it during backend poll/wait_one with NO slot/backend lock held;
    // it acquires NO Scheduler lock — it marks the matching WaitRecord
    // delivered under the registry leaf and links it into the delivered list
    // for the drain (under global_mtx_) to route. It consumes (acknowledges)
    // the routing lease in every path. Stale/cross-Scheduler tokens and
    // already-cancelled records drop the lease without routing.
    class ReadyRoutingSink final : public detail::SynchronousReadySink {
    public:
        explicit ReadyRoutingSink(Scheduler* scheduler) noexcept
            : scheduler_(scheduler) {}
        void on_ready(detail::ReadyEvent event) noexcept override;

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
        // Phase F1 diagnostics (layout cost accepted, AGENTS.md §15): the
        // identity-route counters. deliveries = events carrying a waiter;
        // routed = records marked delivered (the fiber will be woken by the
        // drain); stale_dropped = generation/scheduler-identity mismatches;
        // cancel_lost = the waiter was already cancelled. Tests use these to
        // prove the identity path fired and no legacy scan was involved.
        std::size_t deliveries() const noexcept { return deliveries_; }
        std::size_t routed() const noexcept { return routed_; }
        std::size_t stale_dropped() const noexcept { return stale_dropped_; }
        std::size_t cancel_lost() const noexcept { return cancel_lost_; }
#endif

    private:
        Scheduler* scheduler_;
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
        std::size_t deliveries_ = 0;
        std::size_t routed_ = 0;
        std::size_t stale_dropped_ = 0;
        std::size_t cancel_lost_ = 0;
#endif
    };

    // Wake-path drain: polls the backend — the Scheduler-owned
    // sink marks delivered records during reap — then pops the delivered list
    // and routes each waiter exactly once (make_runnable + owner
    // routing). Replaces the O(N) Completion::ready() re-scan. Caller MUST
    // hold global_mtx_ (the drain's poll runs under G; the sink takes only
    // the registry leaf, so no deadlock).
    bool drain_routed_completion_waits_locked() SLUICE_REQUIRES(global_mtx_);

    // Registry helpers. `wait_registry_mtx_` is a leaf domain: code holding
    // it MUST NOT acquire global_mtx_, access_mtx_, a worker inbox mutex,
    // the wake mutex, or a backend-progress lock. All callers
    // hold G.
    WaitRecord* acquire_wait_record_locked(Fiber* fiber, WorkerState* owner,
                                           const void* completion,
                                           std::uint64_t& lease_id_out)
        SLUICE_REQUIRES(global_mtx_);
    void retire_wait_record_locked(std::uint32_t index) SLUICE_REQUIRES(global_mtx_);
    std::size_t wait_record_live_count_locked() const SLUICE_REQUIRES(global_mtx_);

    // Explicit global MW-state classification (ADR §9.2.6).
    // Physical run termination may occur for both MW_S3_UNRESOLVED and
    // QUIESCENT, but they are LOGICALLY distinct: MW_S3 retains unresolved
    // wait registrations and must never be reported as quiescence.
    enum class MwState {
        mw_s1,              // runnable/running work exists
        mw_s2,              // no runnable/running; ≥1 backend op outstanding
        mw_s3_unresolved,   // no runnable/running; no backend op outstanding; wait registration(s) remain
        quiescent,          // nothing remains
    };

    // Two-phase MW-S2 admission state machine. An atomic bool is
    // insufficient (check-to-block race). NONE → CANDIDATE (under global_mtx_,
    // after the first drain+classify) → COMMITTED (after a re-drain+
    // reclassify still shows MW-S2). route_runnable_locked demotes COMMITTED
    // or CANDIDATE back to NONE (new runnable work cancels admission).
    enum class AdmissionState : unsigned {
        none,
        candidate,
        committed,
    };

    bool wake_ready_flags_locked() SLUICE_REQUIRES(global_mtx_);
    void route_runnable(Fiber* f, WorkerState* owner) SLUICE_REQUIRES(global_mtx_);
    void route_runnable_locked(Fiber* f, WorkerState* owner) SLUICE_REQUIRES(global_mtx_);

    // Owner-authoritative Fiber lookup. Returns the recorded owner
    // WorkerState for a Fiber that has already run and suspended. A missing
    // owner is a fatal Scheduler invariant violation (fail-fast). MUST NOT
    // fall back to g_worker, pending_spawn_, or an arbitrary resolver Worker.
    WorkerState* owner_for_fiber_locked(Fiber* fiber) SLUICE_REQUIRES(global_mtx_);

    // Canonical waiting-Fiber publication helper. Looks up the Fiber's
    // recorded owner, transitions Waiting->Runnable (exactly-once guard), and
    // routes the runnable ticket through the current topology. A retained owner
    // outside this invocation's first-N participants is rebound under
    // global_mtx_; with no accepting invocation the ticket is deferred. Returns
    // true iff the transition succeeded and the ticket was published. Preserves
    // winner resolution, timer retirement, and wait accounting (caller's
    // responsibility).
    bool publish_waiting_fiber_runnable_locked(Fiber* fiber) SLUICE_REQUIRES(global_mtx_);

    // ---- FE frontend-neutral publication edge (FE-1b contract L8/L9) ----
    // Canonical winner publication: switches on the winning node's ResumeTarget
    // kind. `fiber` -> publish_waiting_fiber_runnable_locked (unchanged stackful
    // path); `deferred` -> defer_publication_locked (commit the delivery
    // obligation under G; the frontend discharges it outside authoritative
    // locks); `none` -> publish nothing (pure-protocol epochs, exactly like the
    // null Fiber* previously guarded at each tail). The terminal winner remains
    // the node's resolve_ CAS; this seam only delivers an already-decided
    // terminal. Caller MUST hold global_mtx_.
    void publish_wait_winner_locked(WaitNode& won) SLUICE_REQUIRES(global_mtx_);

    // Commit a deferred delivery obligation: move the frontend record address
    // onto the transient transit list under G. NO user continuation executes
    // here (FE-1b L9); discharge happens in take_deferred_publications chunks
    // with no lock held. Allocation: possible vector growth bounded by
    // CONCURRENT suspended deferred-kind waiters (transient, drained per
    // discharge — the same resource class as worker inboxes; grows with
    // outstanding, never with historical submissions).
    //
    // FAILURE POSTURE (FE-CORRECTIVE-1 P1-1): the insertion runs after the
    // terminal winner is irreversible; an allocation failure here is NOT
    // recoverable (the suspended continuation's delivery obligation would be
    // lost with no residue the teardown gate can observe). A storage failure
    // therefore enters the NAMED process-terminal fail-fast
    // (scheduler_deferred_publication_stranded_fail_fast — the same boundary
    // ~Scheduler uses for a stranded entry). This is a process-terminal
    // failure boundary, deliberately NOT the R2-style allocation-free
    // publication tail: the stackless frontend is experimental/test-gated
    // and its evidence documents this posture. noexcept: no exception
    // escapes (the only throwing op is contained by the boundary).
    void defer_publication_locked(void* delivery_record) noexcept
        SLUICE_REQUIRES(global_mtx_);

    // Drain chunk: move out up to `cap` pending delivery records (FIFO) under
    // G. The caller discharges each record (frontend resume) with NO
    // Scheduler/primitive lock held, then may call again; an empty take means
    // the TRANSIT LIST is drained (records already taken but not yet
    // discharged are the discharging caller's obligation — take ownership;
    // only the ~Scheduler-empty precondition covers the never-taken
    // remainder). ~Scheduler requires this list EMPTY (a stranded
    // entry is a published-but-unconsumed winner: fail-fast teardown
    // precondition, mirrors the QueuePort granted_not_resumed_ gate).
    std::size_t take_deferred_publications(void** out, std::size_t cap);

    // Transient deferred-publication transit (FE-2; compliance gate
    // docs/architecture/fe2-frontend-seam-compliance-gate.md Gate 1/2).
    // Entries are raw frontend delivery records (experimental stackless
    // frontend). Producer: winner tails under G. Consumer: frontend drain.
    std::vector<void*> deferred_publications_ SLUICE_GUARDED_BY(global_mtx_){};

    // Frontend-neutral primitive cancellation terminal closure. The caller
    // MUST hold global_mtx_ + this exact WaitQueue's mtx(). Membership is
    // checked before mutation; a winner resolves Cancelled and unlinks through
    // WaitQueue authority and retires any ordinary deadline through AC-2b.
    // Returns true iff this call won that terminal transition. The closure is
    // frontend-neutral ONLY through those steps: the winning CALLER still
    // decrements waiting_waitq_count_ itself — the exactly-once wait-epoch
    // retirement obligation is semantic, but that concrete counter is current
    // stackful-frontend bookkeeping (MW classification), not shared authority
    // (AC-2a frontend-neutrality separation). Does NOT inspect/publish a
    // Fiber, directly mutate primitive-local resource policy, reconcile a
    // queue head, acquire a lock, allocate, or touch Scheduler wait
    // accounting. AC-2b retirement may fire an already-installed
    // TimerRegistration on_resolve hook (Queue timer accounting); this helper
    // adds no callback authority.
    bool cancel_primitive_wait_locked(WaitQueue& waiters, WaitNode& node)
        SLUICE_REQUIRES(global_mtx_, waiters.mtx());

    // The wake_wait_one body with global_mtx_ already held. Resolves the
    // FIFO head with Woken (wake_one_locked), retires any bound timer, decrements
    // waiting_waitq_count_, and routes the winner runnable. Returns the winning
    // node (nullptr if empty or head lost). Used by the public wake_wait_one AND
    // event_set_broadcast's drain loop. Caller MUST hold global_mtx_.
    WaitNode* wake_wait_one_locked(WaitQueue& q) SLUICE_REQUIRES(global_mtx_);

    // ---- RwLock private helpers ----
    // Unified head-driven grant reconcile. Caller MUST hold G; this function
    // acquires W internally (like mutex_handoff_one_locked). Dispatches to
    // reader batch or writer grant based on head mode. Publishes granted
    // winners under G after W release.
    void rwlock_grant_from_head_locked(WaitQueue& waiters,
                                       std::size_t& active_readers,
                                       bool& writer_active,
                                       ActorId& writer_owner)
        SLUICE_REQUIRES(global_mtx_);

    // No-publication head-claim primitive. Caller MUST hold G + W. Resolves the node Woken (the
    // single winner CAS — fail-fast Category B if it loses for a valid linked
    // eligible node), unlinks it from the queue, retires any bound
    // TimerRegistration, decrements waiting_waitq_count_, and clears the
    // node's temporary next_/prev_ linkage. Does NOT publish the winner
    // (route_runnable_locked is the caller's responsibility). Used by both the
    // unified head reconcile (rwlock_grant_from_head_locked) and the inline
    // admission recheck in the registration paths so the two share ONE claim
    // authority — no drift in resolve/unlink/timer/accounting semantics.
    bool rwlock_claim_node_woken_locked(WaitQueue& waiters, WaitNode& node)
        SLUICE_REQUIRES(global_mtx_, waiters.mtx_);

    // RwLock timer expiry reconcile hook (installed on TimerRegistration).
    // The pump identifies RwLock timers by this function pointer. The hook
    // itself is a no-op; the pump calls rwlock_expire_wait directly.
    static void rwlock_timer_expire_reconcile(void* owner_ctx,
                                              bool timer_won) noexcept;

    using WorkerSnapshot = std::vector<WorkerState*>;

    // Worker-topology authority. Grow the Scheduler-owned topology
    // monotonically and capture exactly the first worker_count stable pointees
    // participating in this invocation. The vector structure is touched only
    // under global_mtx_; captured WorkerState* values remain valid for the
    // Scheduler lifetime and may be used after the lock is released.
    void ensure_workers_locked(unsigned worker_count, WorkerSnapshot& run_workers)
        SLUICE_REQUIRES(global_mtx_);

    void worker_loop(WorkerState* ws, const WorkerSnapshot& run_workers);
    // One internal run implementation parameterized by
    // RunMode. The worker loop reads run_mode_ to select the idle action
    // (ADR §9.4.0). Drain and Live share the SAME loop, classifier, and
    // publication/ownership protocols.
    void run_impl(unsigned worker_count, RunMode mode);
    void run_next_on(WorkerState* ws, Fiber* fiber);

    // Unified suspend-switch authority protocol. Centralizes the
    // suspend authority raise + Fiber Running->Waiting transition that EVERY
    // suspension path MUST perform under global_mtx_ BEFORE releasing it.
    //
    // Precondition: global_mtx_ is held; fiber is ws->current (Running); the
    // wait registration is committed; inline-ready/terminal recheck has ruled
    // out early return.
    //
    // Required ordering (closes the suspend-before-switch race):
    //   1. ws->suspend_switch_pending.store(true, release)  — raise authority
    //   2. fiber->make_waiting()                            — Running->Waiting
    //
    // Because resolver paths require global_mtx_, no resolver may publish a
    // Runnable ticket between Waiting publication and suspend authority
    // publication. A thief observing the routed ticket under global_mtx_ sees
    // suspend_switch_pending==true and refuses the steal until run_next_on
    // clears it (scheduler-side, after the physical context switch saves the
    // Fiber CPU context).
    //
    // Fails fast (scheduler_invalid_suspend_transition_fail_fast) if
    // make_waiting() returns false (impossible protocol state).
    void commit_suspend_locked(WorkerState* ws, Fiber* fiber)
        SLUICE_REQUIRES(global_mtx_);

    // Try to steal one runnable Fiber from another worker's local_runnable
    // to `thief`. Returns true if a Fiber was stolen (and now sits on
    // thief->local_runnable with ownership transferred). Called WITHOUT
    // global_mtx_ held; acquires it internally. Steal is MOVE + OWNER
    // TRANSFER — it never calls make_runnable (the fiber is already Runnable)
    // and never publishes a second ticket. See ADR §9.3 + the TLA+ model.
    bool try_steal(WorkerState* thief, const WorkerSnapshot& run_workers);

    // Classify global MW state. Must be called with global_mtx_ held.
    // Uses the AUTHORITATIVE backend outstanding count (ctx_.outstanding()),
    // NOT scheduler wait-map size — a backend op may be outstanding without
    // a current Scheduler wait registration, and a wait registration may
    // exist with no outstanding backend op (MW-S3). ctx_.outstanding()
    // acquires access_mtx_ internally; global_mtx_→access_mtx_ is the
    // accepted lock order.
    // `classify_ws` (the calling worker, when known) receives the per-worker
    // classify evidence (WorkerState::last_classify / classify_seq) used by
    // the internal-testing park ledger — the ledger must attribute the
    // classification the PARKING worker trusted, not a Scheduler-global
    // last-writer value.
    MwState classify_locked(const WorkerSnapshot& run_workers,
                            WorkerState* classify_ws = nullptr) const SLUICE_REQUIRES(global_mtx_);
    // classify_locked body. classify_locked wraps it to record the per-worker
    // classification (internal-testing park-ledger forensics only); in
    // production the wrapper is a pure forward.
    MwState classify_locked_impl(const WorkerSnapshot& run_workers) const SLUICE_REQUIRES(global_mtx_);

    // ASYNC-TEST-SEAM-AUTHORITY-CORRECTIVE-1: the deterministic causal pause
    // seams (admission, park candidate/commit, event set-store/
    // admission-before-final-check) NO LONGER live as Scheduler fields. They
    // were pure test-coordination state (mtx/cv/bool) with zero production
    // readers, and the forgeable namespace-level friends that gated them have
    // been removed. The seams are now driven by a non-installed test controller
    // keyed on `Scheduler*`, reached ONLY through phase call sites compiled into
    // the `sluice_async_internal_testing` variant (guarded by
    // SLUICE_ASYNC_INTERNAL_TESTING in the scheduler_*.cpp TUs). The production
    // `sluice_async` target has no such fields, no such call sites, and no such
    // symbols. See tests/async_test_control_internal.hpp for the controller.

    // True when persistent progress exists in the
    // run domain but NO active observer would remain if the caller parked.
    // Called under global_mtx_ at the wake-domain park commit (the
    // arm→recheck closure in park_on_wake_source). Progress = a runnable
    // ticket on ANY active participant's local queue or in pending_spawn_,
    // or accepted backend work (ctx_.outstanding() > 0, including
    // backend-ready-unreaped — the reap is progress owed to a waiter).
    // NOT external-wake-capable waits: those are externally resolved by
    // contract (their producers signal the wake domain) and a resident park
    // while only they remain is the Live design. Observer = a fiber
    // currently executing, the MW-S2 participant parked in / committed to
    // the backend-domain wait, or an admission in flight (the admission
    // holder is cycling; if it abandons, it re-evaluates rather than park).
    // CHECK PRIORITY: the runnable-ticket checks
    // (pending_spawn_ / any local queue) run BEFORE the observer exemption.
    // A runnable ticket is never delegatable to a running Fiber: a running
    // Fiber is an observer for ITSELF, not for another ticket queued behind
    // it, and a publication landing entirely before the park's G-section is
    // invisible to the wake epoch (its signal is absorbed by the baseline
    // the park records under the same G-section) — this recheck is that
    // publication's only transport. The observer exemption therefore covers
    // only accepted backend work and resident waits, whose designated
    // observer is the MW-S2 participant (its bridge wakes on backend
    // progress).
    bool unguarded_progress_pending_locked() const SLUICE_REQUIRES(global_mtx_);

    // Get the current Worker's WorkerState (worker-local via TLS).
    static WorkerState* current_worker();

    AsyncIoContext& ctx_;

    // Global coordination state (protected by global_mtx_).
    mutable Mutex global_mtx_;
    // Completion waits are identity-bearing on RequestArena-backed
    // backends (the wait registry below, routed via the ReadySink). For a
    // NON-arena backend (custom AsyncBackend that returns not_supported from
    // register_waiter — e.g. test-support backends), the Scheduler falls back
    // to the legacy Completion*-keyed maps + ready() re-scan. The fallback is
    // provably non-dual: each registration chooses exactly ONE path by the
    // register_waiter result, a fiber lives in exactly one registry, and the
    // sink never touches map entries while the scan never touches records.
    std::unordered_map<void*, WaitReg> waiting_size_ SLUICE_GUARDED_BY(global_mtx_){};
    std::unordered_map<void*, WaitReg> waiting_void_ SLUICE_GUARDED_BY(global_mtx_){};
    std::unordered_map<const std::atomic<bool>*, WaitReg> waiting_ready_ SLUICE_GUARDED_BY(global_mtx_){};

    // Count of fibers suspended on a WaitQueue via await_wait (protected
    // by global_mtx_). Incremented on registration, decremented on resolution
    // (wake_wait_one winner OR cancel_wait winner). Counted by classify_locked
    // exactly like the other wait maps so MW-S3 (unresolved waits) is correct.
    // We track a COUNT (not a per-node map) because a WaitQueue owns its own
    // intrusive node list; the scheduler only needs to know SOME WaitQueue
    // wait is
    // unresolved for MW classification, and the per-node fiber is recovered
    // from the winning node at resolution time.
    std::size_t waiting_waitq_count_ SLUICE_GUARDED_BY(global_mtx_){0};

    // ---- Wait registry (ADR Decision 10) ----
    // The registry is a LEAF domain: wait_registry_mtx_ is never held while
    // acquiring global_mtx_, access_mtx_, a worker inbox mutex, the wake
    // mutex, or a backend-progress lock. Inbound edges only: {global_mtx_, access_mtx_}
    // -> registry (registration/drain/cancel under G; ReadyRoutingSink under
    // access_mtx_ during poll/wait_one). Records are address-stable
    // unique_ptrs; the pool is preallocated at construction to exactly
    // wait_capacity_ records and NEVER grows beyond that bound.
    // Reuse bumps generation before the new occupant is visible.
    mutable Mutex wait_registry_mtx_;
    // Configured maximum number of WaitRecords. Once
    // constructed, wait_records_.size() == wait_capacity_ and never changes.
    const std::size_t wait_capacity_ SLUICE_GUARDED_BY(wait_registry_mtx_){0};
    std::vector<std::unique_ptr<WaitRecord>> wait_records_
        SLUICE_GUARDED_BY(wait_registry_mtx_);
    // Intrusive free list through the records (a vector free-list would
    // allocate on the drain/route path).
    WaitRecord* wait_record_free_head_ SLUICE_GUARDED_BY(wait_registry_mtx_) = nullptr;
    WaitRecord* wait_delivered_head_ SLUICE_GUARDED_BY(wait_registry_mtx_) = nullptr;
    // Live records = registered + delivered (diagnostics / destructor assert).
    std::size_t wait_record_live_count_ SLUICE_GUARDED_BY(wait_registry_mtx_){0};
    // Monotonic lease-id serial (0 is the invalid lease id). Guarded by the
    // registry leaf (lease creation happens under G -> registry).
    std::uint64_t wait_lease_serial_ SLUICE_GUARDED_BY(wait_registry_mtx_){1};
    // Per-Scheduler identity stamped into every WaiterToken; distinguishes
    // Schedulers so a token from another Scheduler can never route here.
    const std::uint64_t scheduler_identity_;
    // The Scheduler-owned sink, installed on the context at construction and
    // detached at destruction.
    ReadyRoutingSink ready_sink_{this};

    // Count of SelectGroups whose callers have committed Waiting and
    // whose phase is Armed, but which are not yet Completed. Counted BY GROUP
    // (not by arm),
    // protected by global_mtx_. Incremented once at the no-ready suspension
    // commit; decremented once at suspended publication. An Event-only
    // suspended Select has no active Timer, no ordinary WaitQueue, and no
    // backend op, yet an external Event::set can still resolve the Fiber — so
    // this count participates in MW classification + park liveness exactly
    // like waiting_waitq_count_ (a Worker in run_live must NOT park/terminate
    // as if quiescent while an Armed Select can still be resolved externally).
    std::size_t waiting_select_count_ SLUICE_GUARDED_BY(global_mtx_){0};


    // The RUNNABLE ownership / steal-consistency record for each Fiber
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
    // WaitReg.owner, not here). See ADR §9.3.5.1.
    // For pinned Fibers this is write-once (spawn sets it; no steal
    // occurs); only steal diverges.
    std::unordered_map<Fiber*, WorkerState*> fiber_owner_ SLUICE_GUARDED_BY(global_mtx_){};

    // Global runnable queue for pre-start assignment. Fibers assigned here
    // are picked up by workers in FIFO order.
    std::deque<Fiber*> pending_spawn_ SLUICE_GUARDED_BY(global_mtx_){};
    unsigned next_spawn_worker_ SLUICE_GUARDED_BY(global_mtx_) = 0;

    // Worker topology authority:
    //   - the workers_ vector structure is protected by global_mtx_;
    //   - each published WorkerState pointee is address-stable until Scheduler
    //     destruction (wait registrations and inboxes retain these pointers);
    //   - a pointer captured under global_mtx_ may be used after unlock only
    //     through the pointee field's existing authority;
    //   - lock order remains global_mtx_ -> WorkerState::inbox_mtx.
    std::vector<std::unique_ptr<WorkerState>> workers_ SLUICE_GUARDED_BY(global_mtx_);

    // Run coordination state.
    std::atomic<unsigned> active_worker_count_{0};
    std::atomic<unsigned> running_fiber_count_{0};
    std::atomic<unsigned> idle_workers_{0};
    // Contribution-identity law: generation of the CURRENT
    // idle-dance contribution epoch. idle_workers_ is erased OUTSIDE any
    // lock by two ticketed paths (the popped-ticket branch and the MW-S1
    // fall-through in worker_loop), and such an erase can orphan a peer's
    // just-made not-last contribution: the count is gone while the peer's
    // 1-bit idle_dance_contributed_ still claims a live contribution, so
    // the park commit's bare idle>own comparison cannot tell the eraser's
    // fresh count from the peer's stale one (1 == 1) and both workers park
    // with the run permanently one short of the last-idle threshold —
    // work complete, terminate false, no remaining producer (the
    // contribution-orphan stall; TLC counterexample M4,
    // spec/tla/e12_rwlock_scheduler_liveness).
    //
    // LAW (model-proven): any idle-count reset that CAN orphan a live
    // contribution invalidates the identity of ALL live contributions; a
    // dancer may arm a park baseline only while its recorded identity is
    // still current, else it refuses, signals, and re-dances (toward the
    // last-idle threshold or a re-counted park). This field is that
    // generation. It is advanced at exactly THREE erase sites
    // (re-proven by the split-window model round): the two
    // unlocked ticketed paths above, and the route-publication erase in
    // route_runnable_locked — the split-window model round falsified its
    // old "self-guarded" verdict (a delayed orphan commit can absorb the
    // eraser's final not-last signal with the generation still matching).
    // Every other reset site is self-guarded — and CONDITIONED on the route
    // bump: the same-G-critical-section forced re-loop/continue sites in
    // the worker loop are reachable only after work appeared, i.e.
    // after a route publication whose bump already invalidated whatever it
    // orphaned; the run boundary resets every worker's identity together;
    // the Live-resident reset keeps its bounded-timeout re-drain backstop;
    // and the two MW-S2 sites are carried by the same publication backbone
    // plus the no-progress terminate flag (model-out-of-scope, documented
    // in the suite README).
    //
    // C++ refinement argument (why the unlocked RMW pair refines the
    // model's split EraseIdle/BumpGen — the honest dichotomy, NOT a
    // visibility claim): the dancer RECORDS this epoch strictly BEFORE its
    // idle fetch_add; the eraser bumps strictly AFTER its exchange(0).
    // These are two distinct unlocked atomics — no acquire load can
    // "guarantee" seeing the later bump — so a stale dancer's park commit
    // CAN read the erased count together with the still-current
    // generation (the split window; reachable in the model, witness gate
    // E12SchedLivenessSplitWindow). The window is nevertheless safe by a
    // two-case split on what the commit observes:
    //   (1) generation != recorded (bump visible): the identity term
    //       refuses; the worker signals and re-dances; convergence by the
    //       R4 conservative argument.
    //   (2) generation == recorded with the count already erased (the
    //       split window): the eraser has NOT completed its protocol. Its
    //       re-dance not-last signal — the only signal the dancer could
    //       sleep through — is emitted under global_mtx_, and the commit
    //       holds global_mtx_ across its count load, generation load, and
    //       baseline arming; observing the erased count additionally
    //       proves no re-dance contribution is G-visible yet. The signal
    //       is therefore sequenced strictly AFTER the arming: the cv
    //       predicate fires, the park is transient, and the dancer
    //       re-loops. The window reorders who wakes; it cannot rebuild
    //       the terminal M4 stuck state, whose mechanism needs the
    //       absorbed signal to precede the arming.
    // NOT mutex-guarded (its writers are the unlocked erase sites by
    // design); never resets — a run boundary needs no reset because every
    // worker re-dances from a fresh loop top and records the current value.
    std::atomic<std::uint64_t> dance_epoch_{0};
    std::atomic<bool> global_terminate_{false};
    std::condition_variable global_idle_cv_;
    bool in_coordinated_run_ = false;
    // "Participant disappearance" guard: participants of
    // the CURRENT invocation whose worker_loop is still live. The idle-dance
    // termination converges against THIS count, not the invocation snapshot
    // size — a worker that exits its loop early (e.g. the MW-S2 no-progress
    // terminate) can never join the dance, and counting it would leave the
    // survivors permanently short of the "last idle worker": the run never
    // terminates, run_live never returns, the driver never reaches its
    // drain-complete evaluation, and drain() waits forever (the deterministic
    // reproducer's post-steal residual stall). Guarded by global_mtx_; set at
    // run_impl entry, decremented in the worker_loop exit retire block.
    unsigned live_loop_workers_ SLUICE_GUARDED_BY(global_mtx_) = 0;

    // The current run invocation's lifetime policy. Set by
    // run_impl before worker_loop starts; read by worker_loop at the idle-
    // action selection boundary. Stable for the duration of a run (a run
    // invocation has ONE mode). Plain bool-storage is safe: it is written
    // once before any worker thread starts and only read thereafter.
    RunMode run_mode_{RunMode::drain};

    // Invocation-scoped stop predicate for Group-scoped Live runs.
    // Set by run_impl before worker_loop; null for ordinary run/run_live.
    // Checked at the MW-S3+Live+external_wake boundary under global_mtx_.
    // If stop_fn(stop_ctx) returns true, the run terminates instead of parking.
    bool (*invocation_stop_fn_)(void*){nullptr};
    void* invocation_stop_ctx_{nullptr};

    // Internal-testing seam: one-shot init_fiber failure injection.
    // When true, the next init_fiber() returns false and resets the flag.
    // Production (SLUICE_ASYNC_INTERNAL_TESTING undefined): never written.
    std::atomic<bool> force_init_fiber_fail_{false};

    // MW-S2 admission coordination state (protected by
    // global_mtx_). admission_ transitions NONE→CANDIDATE→COMMITTED under
    // global_mtx_; route_runnable_locked demotes to NONE. Only the COMMITTED
    // participant may enter ctx_.wait_one(), and only AFTER releasing
    // global_mtx_.
    AdmissionState admission_ SLUICE_GUARDED_BY(global_mtx_){AdmissionState::none};
    unsigned admission_owner_ SLUICE_GUARDED_BY(global_mtx_) = static_cast<unsigned>(-1);  // worker id of candidate/committed

    // ---- Wake source (ADR §9.4.5) ----
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

    // Backend-progress bridge gate: true while the MW-S2 progress
    // participant is parked (or committed to park) in the BACKEND domain
    // (ctx_.wait_one()). Scheduler::signal_wake_locked checks this gate before
    // interrupting the backend waiters, so wake publications cost one atomic
    // load when no backend participant is parked. Set at the MW-S2 admission
    // commit (inside the same critical section that arms the committed-wait
    // baseline, which closes the commit-to-park window independently of this
    // gate), cleared when wait_one() returns. The gate is
    // an optimization only: the epoch protocols are the lost-wake authority.
    std::atomic<bool> backend_wait_active_{false};

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
    // ---- Phase G park-window forensics (G1 BLOCKED instrumentation) ----
    // One bounded ring record per WAKE-DOMAIN park commit (the observed_epoch
    // baseline point in park_on_wake_source). Purpose: when a run stalls with
    // workers parked unguarded, state EXACTLY which persistent state the
    // worker trusted at park time (classification, backend outstanding,
    // backend ready generation, waiting sets) versus which publications
    // happened after the baseline — distinguishing a stale classification
    // from a publication that never crosses the park boundary (AGENTS.md
    // §13.2 wake-obligation audit; design doc
    // docs/history/implementation-plans/phase-g-backend-progress-wake.md
    // park-window finding).
    // `last_classify` is the PARKING worker's own per-worker classify pair
    // (WorkerState::last_classify / classify_seq) captured at park time —
    // never a Scheduler-global last-writer value (Phase G review P2a: a
    // global field mis-attributes when another worker classifies between
    // this worker's classify and its park commit).
    // Layout cost accepted for the internal-testing build only
    // (AGENTS.md §15); absent from production.
    struct ParkLedgerRecord {
        std::uint64_t park_seq = 0;         // global monotonic park id
        unsigned worker_id = 0;
        std::uint64_t epoch_at_commit = 0;  // wake_epoch_ baseline == observed_epoch
        std::uint64_t ready_generation = 0; // backend wait source: last ready publication
        std::uint64_t control_generation = 0;  // backend wait source: interrupt count
        std::size_t backend_outstanding = 0;   // ctx accepted-unreaped count
        std::size_t waiting_registered = 0;    // size_+void_+ready_+waitq+select
        unsigned idle_workers = 0;
        bool backend_wait_active = false;
        bool ready_flag_bounded = false;    // park bounded by ready-flag observation
        bool global_terminate = false;
        bool external_wake_possible = false;
        int last_classify = -1;  // 0 mw_s1, 1 mw_s2, 2 mw_s3_unresolved, 3 quiescent
        std::uint64_t classify_seq = 0;  // parking worker's classify_seq at park time
    };
    static constexpr std::size_t kParkLedgerCapacity = 64;
    // Off by default: the park-commit snapshot takes global_mtx_ /
    // access_mtx_ / the wait-source mutex, which shifts park-path timing —
    // observation must not perturb seam-driven tests. Only the forensics
    // case enables it (one acquire load here otherwise).
    mutable std::atomic<bool> park_forensics_enabled_{false};
    // Leaf test-only mutex. Acquired while holding wake_mtx_ (record side)
    // and standalone (dump side); never acquires a Scheduler lock.
    mutable std::mutex park_ledger_mtx_;
    ParkLedgerRecord park_ledger_[kParkLedgerCapacity]{};
    std::size_t park_ledger_count_{0};
    std::size_t park_ledger_next_{0};
    std::uint64_t park_ledger_total_{0};  // monotonic park sequence (ring wraps)

    // Stall forensics dump (stderr): live scheduler state (wake epoch, wait
    // token, outstanding, admission, waiting sets, per-worker park domain)
    // plus the park ledger ring. Safe to call from a watchdog thread while
    // workers are parked: each domain lock is taken separately, no nesting.
    void dump_park_forensics_for_test(const char* tag);
#endif

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
    // The bounded timeout is min(default wake poll, earliest-deadline-now)
    // so an active deadline cannot park a Worker indefinitely past it.
    // The timeout is DEADLINE-DRIVEN (uncapped) except when
    // `bounded_backend_observation` is set — the MW-S2 MIXED-WAKE park for a
    // non-split-wait (reference/legacy) backend, whose poll-driven readiness
    // is observed through the 2ms bounded observation interval (retained
    // only there). Split-wait backends park their
    // progress participant in ctx_.wait_one() and never reach this function
    // with backend progress pending.
    // Clang TSA cannot track unique_lock capability semantics through
    // condition_variable::wait (release-wait-reacquire pattern), so this
    // function opts out of analysis. The production lock fact (wake_epoch_ is
    // protected by wake_mtx_) is independently accepted and proven.
    void park_on_wake_source(WorkerState* ws,
                             bool bounded_backend_observation) SLUICE_NO_THREAD_SAFETY_ANALYSIS;

    // ---- Timer / deadline subsystem ----
    // The deadline container is a binary min-heap over TimerRegistration*,
    // keyed by deadline. Pointer-stable storage (std::list) so a registration's
    // identity survives heap sift operations and outlives any bound WaitNode.
    //
    // Reclamation contract: logical
    // retirement (ACTIVE->RETIRED) is IMMEDIATE (the non-timer winner performs
    // it in the same CS as the resolve CAS); lifetime safety is IMMEDIATE (the
    // atomic `state` gate makes try_claim_expiry/retire observe RETIRED/CONSUMED
    // WITHOUT dereferencing a possibly-destroyed node); but PHYSICAL
    // reclamation (erase from heap + pool) is LAZY-AT-DEADLINE — the pump pops
    // an entry only when `now >= its deadline`, regardless of state, and erases
    // the pool block then. Consequently a far-future RETIRED entry remains
    // physically in the heap+pool until its original deadline is reached. The
    // pool size is therefore bounded by (concurrent ACTIVE deadline waits) +
    // (retired/consumed entries whose deadlines have not yet been reached); it
    // is NOT bounded solely by concurrent waits, and "unbounded growth fixed"
    // must not be claimed absolutely. No inert block lingers past its OWN
    // deadline. Wheel-compaction / eager removal is deliberately NOT
    // added unless a reviewer proves a mandatory resource-bound violation.
    //
    // All heap state is protected by global_mtx_ (the Scheduler coordination
    // domain): the heap is mutated only by await_wait_deadline (register),
    // pump_deadlines_locked (expiry), and the retire path (state flip under
    // the same CS as the resolve CAS). The clock is atomic so a worker can read
    // it outside the lock for the park-timeout computation.
    std::list<TimerRegistration> timer_pool_ SLUICE_GUARDED_BY(global_mtx_){};
    // The heap stores unified tagged
    // DeadlineHeapEntry values (Ordinary | Select) instead of raw
    // TimerRegistration*. Both kinds share one min-heap keyed by the cached
    // deadline; the ordinary branch is byte-for-byte identical in logic (it
    // reads the same deadline, pops the same min, and processes the same
    // TimerRegistration* via entry.target.ordinary). Internal-only type; no
    // public API exposure.
    std::vector<detail::DeadlineHeapEntry> deadline_heap_ SLUICE_GUARDED_BY(global_mtx_){};

    // Scheduler-owned stable pool of SelectTimerRegistration blocks,
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
    // The heap stores unified DeadlineHeapEntry values; the comparator
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
    // SAFE ONLY when no live heap slot holds the block's pointer: either it
    // has ALREADY been popped from the deadline heap (the pump path: pop the
    // min, then erase the pool block), or it was NEVER published to the heap
    // (the R2-ALLOC admission path: register_wait_locked failed after
    // prepare_ordinary_deadline_locked, so the caller discards the
    // prepared-but-never-published block before returning). Other paths
    // (admission-expired, non-timer retire) do NOT erase — they leave the
    // block in the heap to be popped+erased by the pump at its deadline
    // (lazy removal). This keeps the pool bounded by live+pending deadline
    // waits with no UAF. Called under global_mtx_.
    // NEVER reads node()/queue() (lifetime-safe by construction).
    void erase_popped_registration_locked(TimerRegistration* r) SLUICE_REQUIRES(global_mtx_);

    // ---- Ordinary deadline lifecycle authority (AC-2b) ----
    // Single authority for ordinary ACTIVE->{CONSUMED,RETIRED} transitions
    // and their exactly-once active_deadline_count_ decrements; SHARED arming
    // authority for NON-Queue registrations only (Queue-local arming stays
    // local — see below).
    //
    // Authority boundary (distinct from WaitNode::resolve_): the resolve CAS on
    // the bound WaitNode remains the ONE wait-outcome winner authority; these
    // helpers own ONLY timer callback/lifetime state + active_deadline_count_
    // accounting. A timer consume never publishes a Completion/runnable, and a
    // WaitNode winner never fabricates timer accounting outside these helpers.
    //
    // Deliberately NOT centralized here:
    //   - recompute_earliest_deadline_locked() timing: pump_deadlines_locked
    //     amortizes one recompute AFTER its whole loop; inline already-due paths
    //     recompute immediately. Keep each caller's existing horizon.
    //   - fire_on_resolve_locked(): Queue/RwLock per-port hook ordering differs
    //     between retire and consume sites; it stays at each call site.
    //   - primitive-specific bookkeeping (resource commit, waiting_waitq_count_,
    //     per-port counters): not uniform across arming sites.
    //
    // SelectTimerRegistration is a sibling implementation of the same
    // high-level invariant with different admission/reclamation mechanics
    // (spliced caller-frame blocks, ownership validation); it keeps its own
    // select_timer_{retire,consume}_locked helpers. No shared hierarchy.
    //
    // All three require global_mtx_ held (the existing G-required timer CS).

    // R2-ALLOC corrective: split arming into a MAY-THROW prepare phase and a
    // noexcept publish phase so a timed admission can perform every
    // allocation BEFORE it mutates any admission state (node registration,
    // waiting_waitq_count_, per-primitive counters). The admission order at
    // every production timed site is:
    //
    //   prepare_ordinary_deadline_locked(...)   // may throw; NOTHING mutated
    //   register_wait_locked(node, ...)         // noexcept pointer linking
    //   ++waiting_waitq_count_ (+ per-site)     // noexcept
    //   publish (or the local Queue publish)    // noexcept
    //
    // A std::bad_alloc / std::length_error escaping prepare therefore leaves:
    // node Detached, wait accounting unchanged, timer accounting unchanged,
    // no pool block, no heap entry, no earliest-deadline cache change, no
    // publication — the strong admission-atomicity contract. This is the
    // ordinary-path mirror of the Select admission's reserve-before-register
    // (select.cpp step (5)); a catch-based rollback alternative was rejected
    // because arm's two internal allocation points (heap growth + pool node)
    // would each need partial-failure rollback under the lock.
    //
    // Prepare performs, in order: a deadline_heap_ capacity-growth reserve
    // and the timer_pool_.emplace_back node allocation. The heap reserve
    // fires only when size == capacity (heap exactly full) and takes the
    // next geometric doubling step, checked against max_size, so the
    // amortized-O(1) append curve of vector::push_back is preserved — an
    // unconditional one-slot reserve would degrade N concurrent admissions
    // into O(N) reallocations and O(N^2) element moves. Once prepare
    // returns, both containers' subsequent admission mutations are then
    // allocation-free. The block is constructed
    // with the {node, q, deadline} binding and default-null hooks; it is NOT
    // ACTIVE-published (no count, no heap entry, no cache recompute) until
    // publish. Between prepare and publish the caller holds global_mtx_ for
    // the whole admission, so no pump/scan can observe the intermediate
    // block; if register_wait_locked fails (registration contract violation),
    // the caller MUST erase the never-published block via
    // erase_popped_registration_locked (safe: no heap slot holds its pointer).
    TimerRegistration* prepare_ordinary_deadline_locked(
        WaitNode* node, WaitQueue* q, deadline_t deadline)
        SLUICE_REQUIRES(global_mtx_);

    // Publish a prepared block: install {on_resolve, owner_ctx}, increment
    // active_deadline_count_ exactly once, push the block into the deadline
    // heap (within the capacity reserved by prepare), and refresh the
    // earliest-deadline park cache at this same critical-section boundary —
    // the exact externally-observable sequence every pre-existing NON-Queue
    // arming site performed. noexcept PROOF: the heap entry type is trivially
    // copyable (static_assert in scheduler_timer.cpp), push_back within
    // reserved capacity cannot throw for it, sift swaps are nothrow, and
    // recompute only scans and stores an atomic. After publication the
    // admission tail is exception-free by construction — mirroring the §10.2
    // rule that a committed transaction must not gain a throwing path.
    void publish_ordinary_deadline_locked(
        TimerRegistration* reg,
        TimerRegistration::OnResolveFn on_resolve = nullptr,
        void* owner_ctx = nullptr) noexcept SLUICE_REQUIRES(global_mtx_);

    // Arm one ordinary deadline registration for wait epoch {node, q,
    // deadline}: prepare + publish in one call. Kept as the composed
    // historical form for callers whose registration happens outside this
    // helper and cannot interleave an allocation failure with admission state
    // (the narrow test-only hook). PRODUCTION timed admissions MUST NOT use
    // the composed form across their registration boundary — they call
    // prepare BEFORE register_wait_locked and publish after their counters,
    // per the R2-ALLOC split above.
    //
    // Raw-pointer parameters are deliberate (AC-2b review corrective): the
    // narrow test-only hook register_test_deadline_locked historically admits
    // a null `node` binding — stored unevaluated and never dereferenced until
    // an expiry gates on ACTIVE — and this authority MUST NOT tighten that
    // seam contract as a side effect of centralization. Production callers
    // always pass live pointers.
    //
    // Optional {on_resolve, owner_ctx} install the per-port resolution hook
    // atomically with publication, all within the caller's global_mtx_
    // critical section.
    //
    // INTENTIONALLY NOT MIGRATED (AC-2b review corrective): Queue arming in
    // queue_push/pop_admit_until stays local. Its historical order bumps
    // active_queue_timers_ BETWEEN hook binding and ACTIVE-count publication,
    // and the arming-time recompute publishes earliest_active_deadline_ to an
    // atomic that parked workers read WITHOUT global_mtx_; ordering against
    // the per-port counter is therefore externally observable in principle
    // and is preserved verbatim at those call sites (they use the shared
    // prepare for the allocation phase and keep their local publish
    // sequence). Semantic compression over completeness — see
    // queue_push_admit_until for the local sequence.
    TimerRegistration* arm_ordinary_deadline_locked(
        WaitNode* node, WaitQueue* q, deadline_t deadline,
        TimerRegistration::OnResolveFn on_resolve = nullptr,
        void* owner_ctx = nullptr) SLUICE_REQUIRES(global_mtx_);

    // Consume an ordinary registration as the timer-expiry winner: CAS
    // ACTIVE->CONSUMED; on success decrement active_deadline_count_ exactly
    // once. Returns true iff THIS call won. On failed CAS: no count mutation;
    // the caller MUST NOT dereference node()/queue() (timer-lifetime closure).
    // Does NOT recompute the earliest-deadline cache: callers keep their own
    // horizon (pump amortizes once per drain; inline paths recompute once).
    bool consume_ordinary_deadline_locked(TimerRegistration& reg)
        SLUICE_REQUIRES(global_mtx_);

    // Retire an ordinary registration as a non-timer winner closing timer
    // callback authority: CAS ACTIVE->RETIRED; on success decrement
    // active_deadline_count_ exactly once. Returns true iff THIS call retired
    // an active registration. On failed CAS (already terminal): no count
    // mutation. Does NOT fire on_resolve hooks or recompute the cache — both
    // stay at the call site where their ordering is proven per-primitive.
    bool retire_ordinary_deadline_locked(TimerRegistration& reg)
        SLUICE_REQUIRES(global_mtx_);

    // ---- Select timer registration (private Scheduler authority) ----
    // All require global_mtx_ held. These own the registered-state accounting
    // authority for Select Timer arms: every ACTIVE->terminal
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
    // (the retire/consume helper already did, exactly once). Lifetime-safe: matches
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

    // Select timer pump branch body. PRE: global_mtx_ held; the pump
    // has ALREADY popped `reg` from the deadline heap and observed
    // now >= reg.deadline(). State-before-arm rule: load state
    // first; non-ACTIVE -> PumpSkip seam + return stale (do NOT read arm_).
    // ACTIVE -> TimerPumpActive seam, instrument the exact production arm
    // dereference site, then fail fast (a due ACTIVE Select entry is
    // unreachable in a valid registration). The caller (pump) performs the
    // physical reclamation via erase_popped_select_registration_locked
    // regardless of the branch. Returns true if the entry was stale (skipped),
    // false if it was ACTIVE (the call does not return in the ACTIVE case).
    bool select_timer_pump_entry_locked(
        detail::SelectTimerRegistration& reg) SLUICE_REQUIRES(global_mtx_);

    // ---- Select central claim + winner/loser finalization core ----
    // All require global_mtx_ held continuously. This is the single loser
    // processor and the single claim+finalize orchestrator. It does NOT
    // publish a result, route a runnable, transition the group to
    // Completed/Consumed, or
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

    // Validate the entire group BEFORE the irreversible winner CAS.
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
    // ACTIVE->RETIRED via select_timer_retire_locked (arm classification
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

    // The reusable all-authority-closed invariant predicate. Returns
    // true iff: winner != kNoWinner, winner < arm_count, AND for every arm
    //   - Event: state == Retired, home_ == nullptr, next_/prev_ == nullptr
    //   - Timer: state == Retired, stable_reg_ terminal (CONSUMED for the
    //            winner, RETIRED for losers)
    // select_process_group_locked asserts this before returning true on a win.
    // A future select_publish_locked must call this at its entry as its
    // publication precondition. Pure read; no mutation.
    bool select_all_authority_closed_locked(const detail::SelectGroup& group) const
        SLUICE_REQUIRES(global_mtx_);

    // ---- Select registration + admission core (inline + suspended) ----
    // The single non-template admission core, reached ONLY via the friended
    // public variadic select() template (declared in select_fwd.hpp, defined
    // in select.hpp). PRIVATE: ordinary code cannot name it (the friend grant
    // is to the exact constrained template entity, not to a forgeable struct
    // name). Owns every centralized admission step: caller + case-Scheduler
    // validation (BEFORE any allocation), caller-frame group+ arms
    // materialization, Timer stable-block construction (before global_mtx_),
    // deadline-heap reserve (the only allocation under the lock), the
    // registration loop, FinishRegistration, the immutable readiness snapshot,
    // lowest-index tie-break, the single claim+finalize processor call, all-authority-
    // closed verification, and result completion. NOT a template — compiles
    // once.
    //
    // The admission name is NEUTRAL (select_admit) because BOTH outcomes
    // route through this one core: an inline-ready
    // admission returns a SelectResult without suspending; a no-ready admission
    // commits the caller Fiber to Waiting + phase Armed, suspends, and is later
    // resumed to consume the published result (Completed -> Consumed). There is
    // exactly ONE admission core for both paths. `descs` points at
    // `count` SelectCaseDescriptor values (caller-frame array); the function
    // does not retain the pointer past the call.
    //
    // Not noexcept: may throw std::logic_error (caller validation) or
    // std::invalid_argument (case Scheduler mismatch) BEFORE any allocation, or
    // std::bad_alloc (Timer block / heap reserve) before the first registration
    // mutation. After the heap reserve, the registration loop contains no
    // ordinary throwing operation; the no-ready suspension + resume paths are
    // noexcept (or fail-fast, which terminates).
    SelectResult select_admit(detail::SelectCaseDescriptor* descs,
                              std::size_t count);

    // ---- Select registration-rollback authority (private) ----
    // Refines the formal actions
    // ContractBeginRollback / ContractRollbackRelease(i) /
    // ContractCloseAuthority(i) / ContractFinishRollback of the closed
    // E13SelectContract.tla model. Rollback is permitted ONLY in the Building
    // domain (phase==Building, winner==kNoWinner, completion_mode==None, caller
    // still Running, FinishRegistration NOT yet occurred). All helpers are
    // noexcept: rollback must never throw and replace the original admission
    // exception. They may assert/fail-fast on impossible
    // corruption. PRE: global_mtx_ held continuously across the whole rollback.
    //
    // registered_count is the authoritative size of the fully committed
    // registered prefix [0, registered_count); the catch transaction passes it.
    // The orchestrator processes the registered prefix in reverse registration
    // order (registered_count-1 .. 0), normalizes the never-registered suffix
    // [registered_count, arm_count) to Detached, then finishes to Aborted.

    // Preflight: validate the rollback domain mechanically and set
    // group.phase = Rollback. Rejects Selecting/Armed/Completed/Consumed/
    // Aborted/Rollback re-entry (fail-fast). No arm authority is closed here.
    void select_begin_rollback_locked(detail::SelectGroup& group) noexcept
        SLUICE_REQUIRES(global_mtx_);

    // Roll back ONE fully-registered arm. Order (normative, matches
    // E13SelectEventTimer.tla RollbackCancelTimer then RollbackRetireTimer):
    // classify the arm Retired FIRST, then
    // close its external callback authority.
    //   Event: arm.state = Retired; then select_event_unlink_locked (the single
    //          canonical Event unlink path); Event::set_ is never mutated.
    //   Timer: arm.state = Retired; then select_timer_retire_locked (the single
    //          canonical Timer retirement authority — ACTIVE->RETIRED CAS +
    //          --active_deadline_count_ + recompute, exactly once). A false
    //          return from retire is an invariant violation -> fail-fast.
    // arm_count/registered_count validate the index is a registered arm.
    void select_rollback_arm_locked(detail::SelectGroup& group,
                                    detail::SelectArmSlot& arm) noexcept
        SLUICE_REQUIRES(global_mtx_);

    // Prove every registered authority is closed + every never-registered arm
    // is Detached, then set group.phase = Aborted. Does NOT clear admitted_
    // (admitted_ && phase==Aborted is the intended destruction proof). Does NOT
    // publish (no result/runnable). Sets phase ONLY after the postconditions
    // hold; an open authority fails fast.
    void select_finish_rollback_locked(detail::SelectGroup& group,
                                       detail::SelectArmSlot* arms,
                                       std::size_t arm_count,
                                       std::size_t registered_count) noexcept
        SLUICE_REQUIRES(global_mtx_);

    // The rollback orchestrator called by select_admit's catch. Calls
    // begin -> reverse per-arm rollback over [0, registered_count) -> suffix
    // normalization -> finish. noexcept.
    void select_rollback_registration_locked(
        detail::SelectGroup& group, detail::SelectArmSlot* arms,
        std::size_t arm_count, std::size_t registered_count) noexcept
        SLUICE_REQUIRES(global_mtx_);

    // ---- Select unified publication authority ----
    // THE single result/runnable publication function. PRE:
    // global_mtx_ held continuously; the winner has already been claimed and
    // every winner+loser arm finalized (authority closed) in the SAME critical
    // section that calls this. select_publish_locked:
    //   - mechanically validates the publication-entry shape,
    //     fail-fasting on any violation,
    //   - constructs exactly ONE SelectResult from the winner arm and writes it
    //     to group.result_ (written exactly once),
    //   - branches on entry phase:
    //       * Selecting (inline): completion_mode_=Inline, phase=Completed.
    //         NO make_runnable, NO route_runnable_locked, NO counter mutation.
    //       * Armed (suspended): completion_mode_=Suspended, phase=Completed,
    //         decrement waiting_select_count_ (underflow -> fail-fast),
    //         make_runnable (false -> fail-fast), route_runnable_locked via
    //         group.caller_owner_ (NEVER the resolver-thread g_worker).
    //
    // Required source order: all authority closed < group.result_
    // write < completion mode < phase Completed < runnable publication. There
    // is exactly ONE call site each for Fiber::make_runnable and
    // Scheduler::route_runnable_locked inside Select, both here, both only on
    // the suspended branch. No Event/Timer finalizer/scan/pump reaches them.
    void select_publish_locked(detail::SelectGroup& group)
        SLUICE_REQUIRES(global_mtx_);

    // ---- Suspended Event/Timer resolution ----
    // The post-suspension resolvers, reached ONLY under
    // global_mtx_ from event_set_broadcast (Event) and the timer pump (Timer).
    // Each performs the single-group gate (Event) or ACTIVE validation
    // (Timer), offers CandidateReady to the relevant arm(s), then drives the
    // single claim+finalize group processor exactly once followed by select_publish_locked
    // exactly once. No arm finalizer calls make_runnable / route_runnable.
    //
    // select_resolve_event_locked: walks `event`'s SelectPort for THIS Event's
    // eligible (kind==Event, state==Registered, group.phase==Armed, home points
    // here) arms; applies the single-group gate (multi-group DENIED ->
    // fail-fast before any CAS); marks every eligible arm CandidateReady;
    // chooses the lowest INDEX (NOT intrusive-list order) ready arm; processes
    // + publishes the group once. Returns true iff a group was published.
    bool select_resolve_event_locked(Event& event)
        SLUICE_REQUIRES(global_mtx_);

    // select_resolve_timer_locked: the ACTIVE path replacement for the due-
    // ACTIVE stage fail-fast in select_timer_pump_entry_locked. Validates the
    // ACTIVE block (arm kind/state/group/scheduler/phase/pool/reg back-pointer)
    // under the state-before-arm rule, finds the exact arm index by address
    // scan of group.arms_, marks it CandidateReady, processes + publishes the
    // group once. PRE: `reg` is ACTIVE and the pump has popped it. The Timer
    // registration is NOT consumed before the group winner CAS (the claim+finalize
    // core owns the winner/loser finalizer). Returns true iff a group was published.
    bool select_resolve_timer_locked(detail::SelectTimerRegistration& reg)
        SLUICE_REQUIRES(global_mtx_);

    // Narrow test hook: register a TimerRegistration for {node,q,
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
    // publication. Performs ACTIVE->RETIRED on the
    // registration's independently-stable state, closing callback authority so
    // a stale/lazy expiry cannot dereference the node after the fiber resumes
    // and destroys its caller-owned WaitNode.
    //
    // Lifetime-safe scan discipline: the scan considers ONLY ACTIVE registrations.
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
    // wait, OR a WaitQueue wait resolved via wake_wait_one/cancel_wait, OR a
    // deadline wait whose expiry is driven by the timer pump)? Such
    // resolutions reach signal_wake_locked (the unified Scheduler wake source),
    // NOT a backend reap, so the worker MUST park on the SCHEDULER domain to
    // observe them. When true, the MW-S2 participant must NOT enter a backend-
    // only ctx_.wait_one() — it parks on the SCHEDULER domain instead (the
    // MIXED-WAKE fix, ADR §9.4.7). Call with global_mtx_ held.
    //
    // A WaitQueue wait is externally resolvable exactly like
    // a ready-flag wait (wake_wait_one/cancel_wait run under global_mtx_ +
    // q.mtx() and reach signal_wake_locked via route_runnable_locked). It MUST
    // participate in this classification, or a Live run could park/terminate on
    // a source that cannot observe the wait's resolution.
    //
    // An ACTIVE deadline registration is likewise externally resolvable —
    // its expiry is driven by the worker loop's timer pump + the bounded park
    // timeout, not by a backend reap. It participates in this classification
    // for the same reason as the WaitQueue wait.
    bool external_wake_possible_locked() const SLUICE_REQUIRES(global_mtx_) {
        return !waiting_ready_.empty() || waiting_waitq_count_ > 0 ||
               any_active_deadline_locked() || waiting_select_count_ > 0;
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

    // Internal-testing access surface (static observation/mutation helpers).
    // The 600+ line definition lives OUT-OF-LINE in the
    // NON-INSTALLED seam header src/async/scheduler_test_access.hpp, which
    // the bottom of this file includes under the internal-testing macro.
    // Production TUs compile neither the completion of this declaration nor
    // any seam symbol; internal-testing TUs see the complete nested class.
    struct AsyncTestAccess;
#endif  // defined(SLUICE_ASYNC_INTERNAL_TESTING)
};

}  // namespace sluice::async

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
// The complete AsyncTestAccess definition lives in the
// NON-INSTALLED seam header src/async/scheduler_test_access.hpp (resolved
// via the internal-testing-only include path). Including it here keeps every
// internal-testing TU that names Scheduler::AsyncTestAccess working without
// per-test changes; production TUs never compile this include.
#include "scheduler_test_access.hpp"
#endif  // defined(SLUICE_ASYNC_INTERNAL_TESTING)
