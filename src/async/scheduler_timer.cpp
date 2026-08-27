// Scheduler timer/deadline domain — implementation TU split from
// scheduler.cpp (docs/post-freeze/structural-audit.md §6).
//
// The class declaration, lock domains, atomic orderings, and wake contracts
// remain in include/sluice/async/scheduler.hpp.
#include <sluice/async/scheduler.hpp>

#include <sluice/async/async_rwlock.hpp>
#include <sluice/async/select.hpp>
#include <sluice/async/event.hpp>
#include <sluice/async/fiber_ctx.hpp>
#include <sluice/async/detail/fail_fast.hpp>
#include <sluice/async/detail/select_port.hpp>

#include "scheduler_internal.hpp"

#include <utility>
#include <cstdio>
#include <cstdlib>

// ASYNC-TEST-SEAM-AUTHORITY-CORRECTIVE-1: the internal-testing variant pulls in
// the non-installed test-control header so the phase call sites below resolve to
// the controller. In the production build this include is absent and the call
// sites compile to nothing.
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
#include "async_test_control_internal.hpp"
#endif

namespace sluice::async {
Scheduler::deadline_t Scheduler::monotonic_now() const noexcept {
    // Production clock: steady_clock ticks since process start. Rebased to a
    // small origin so deadline_t values stay manageable. Test mode returns the
    // logical clock_ (advanced deterministically by advance_clock()).
    // Lock-free: clock_ + test_clock_mode_ are atomics (no GUARDED_BY), read
    // here exactly as in clock_now_unlocked().
    if (test_clock_mode_.load(std::memory_order::acquire)) {
        return clock_.load(std::memory_order::acquire);
    }
    auto since_epoch = std::chrono::steady_clock::now().time_since_epoch();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(since_epoch);
    return static_cast<deadline_t>(ms.count());
}

Scheduler::deadline_t Scheduler::clock_now_unlocked() const noexcept {
    // Lock-free read for the park-timeout computation. In production reads
    // steady_clock; in test mode reads the atomic clock_.
    if (test_clock_mode_) {
        return clock_.load(std::memory_order::acquire);
    }
    auto since_epoch = std::chrono::steady_clock::now().time_since_epoch();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(since_epoch);
    return static_cast<deadline_t>(ms.count());
}

void Scheduler::advance_clock(deadline_t t) {
    // TEST-ONLY deterministic timer driver (M7). Advance the logical clock and
    // pump any now-due timers through expire_wait. Requires test-clock mode
    // (set by E11TimerTestHooks::enable_test_clock). In production this is a
    // no-op: real time advances on its own and the worker loop's pump drives
    // expiry. NEVER used as causal proof with sleep_for.
    {
        LockGuard lk(global_mtx_);
        if (!test_clock_mode_) return;  // production mode: no-op
        deadline_t cur = clock_.load(std::memory_order::acquire);
        if (t > cur) clock_.store(t, std::memory_order::release);
        // Pump due timers under the lock (expire_wait re-acquires the per-queue
        // mtx inside this global_mtx_ CS — lock order preserved).
        (void)pump_deadlines_locked();
    }
    // A timer expiry published a runnable: signal the wake source so a parked
    // worker (if any) re-loops.
    signal_wake_locked();
}

void Scheduler::await_wait_deadline(WaitQueue& q, WaitNode& node, deadline_t deadline) {
    // Deadline wait admission. Unified suspend protocol.
    // Extends await_wait with: (1) a TimerRegistration control block bound to
    // this wait epoch, and (2) an already-due-deadline recheck that resolves
    // Expired immediately through the SAME resolve_ authority (the
    // already-due admission closure — the fiber is never stranded by a due
    // deadline).
    //
    // The admission critical section establishes, atomically w.r.t. every
    // resolver (wake_wait_one / cancel_wait / expire_wait / pump_deadlines all
    // run under global_mtx_):
    //   1. register node into q               (Detached -> Registered)
    //   2. ++waiting_waitq_count_             (MW-S3 accounting)
    //   3. create TimerRegistration R_E       (ACTIVE, bound {node,q,deadline})
    //   4. push R_E into the deadline heap
    //   5. recheck: if node already terminal -> undo + return (defense-in-depth)
    //   6. recheck: if deadline already due  -> resolve Expired + return (the
    //      already-due closure)
    //   7. commit_suspend_locked(ws, me)      (authority + Waiting)
    // Only context_switch is outside the lock.
    WorkerState* ws = g_worker;
    Fiber* me = ws->current;
    TimerRegistration* reg = nullptr;
    {
        LockGuard lk(global_mtx_);
        LockGuard qlk(q.mtx());
        if (!q.register_wait_locked(node, me)) {
            // Node already registered or terminal: contract violation.
            return;
        }
        ++waiting_waitq_count_;
        // Arm the timer registration control block for this wait epoch (pool
        // construction + ACTIVE count + heap push + park-cache refresh).
        reg = arm_ordinary_deadline_locked(&node, &q, deadline);

        // Already-due admission closure: if the deadline is ALREADY due, resolve
        // Expired
        // through the same resolve_ authority NOW — the fiber must NOT suspend
        // and wait for a future timer scan merely because registration happened
        // after the deadline was due. expire_locked is the winner CAS; on win,
        // perform the winner path (unlink already done by expire_locked,
        // retire the registration, dec count) and return WITHOUT suspending.
        if (clock_now_unlocked() >= deadline) {
            if (q.expire_locked(node)) {
                (void)consume_ordinary_deadline_locked(*reg);  // ACTIVE->CONSUMED
                recompute_earliest_deadline_locked();  // reg no longer Active
                if (waiting_waitq_count_ > 0) --waiting_waitq_count_;
                // The current Fiber is RUNNING and continues inline; no
                // publication.
                // The consumed block stays in the heap/pool for lazy removal:
                // its deadline is already due, so the next pump_deadlines_locked
                // (worker-loop drain, or the test driver's advance_clock) pops
                // + erases it. No UAF: the block is not dereferenced while inert.
                return;  // resolved at admission; do NOT suspend
            }
            // If expire_locked lost, a concurrent resolver won; fall through to
            // the terminal recheck (the node is terminal -> undo + return).
        }

        // Recheck: if the node was resolved concurrently (it cannot be, since
        // register_wait_locked just moved it to Registered under both locks and
        // every resolver takes global_mtx_), undo and do not suspend.
        if (node.is_terminal()) {
            q.unlink_locked(node);
            --waiting_waitq_count_;
            (void)retire_ordinary_deadline_locked(*reg);  // ACTIVE->RETIRED
            recompute_earliest_deadline_locked();
            return;
        }
        commit_suspend_locked(ws, me);
    }
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
    sluice_async_test::test_phase(*this,
        sluice_async_test::PhaseTag::scheduler_suspend_before_physical_switch);
#endif
    fiber_ctx::Switch s;
    s.old = &me->ctx;
    s.new_ = &ws->sched_ctx;
    (void)fiber_ctx::context_switch(&s);
}

bool Scheduler::expire_wait(WaitQueue& q, WaitNode& node) {
    // The third resolution cause: resolve `node` with Expired and route the
    // winner's fiber. Mirrors wake_wait_one / cancel_wait EXACTLY:
    // global_mtx_ + q.mtx() -> resolve_(Expired) -> unlink_locked ->
    // --waiting_waitq_count_ -> make_runnable + route_runnable_locked.
    //
    // Called by pump_deadlines_locked for a due, ACTIVE registration. The
    // registration's try_claim_expiry() (ACTIVE->CONSUMED) has ALREADY been
    // performed by the caller, so THIS call owns the timer authority. The
    // resolve_ CAS is still the publication guard: if a concurrent wake/cancel
    // won the node, resolve_ fails here and this returns false (loser) — but
    // the concurrent winner retired the registration in its own CS, so there is
    // no double-claim. NEVER a parallel timer-wake publication path.
    LockGuard lk(global_mtx_);
    LockGuard qlk(q.mtx());
    if (!q.expire_locked(node)) return false;  // already terminal (loser)
    Fiber* f = node.fiber();
    if (waiting_waitq_count_ > 0) --waiting_waitq_count_;
    // Route to the Fiber's recorded owner (NOT g_worker).
    if (f != nullptr) {
        if (publish_waiting_fiber_runnable_locked(f)) {
            return true;
        }
    }
    return false;
}



std::size_t Scheduler::pump_deadlines_locked() {
    // Drive due timers: for every heap-min whose deadline <= now and whose
    // registration is still ACTIVE, claim it (try_claim_expiry) and resolve its
    // bound node via expire_wait. Lazy-skips retired/consumed entries (removes
    // them from the heap without dereferencing the node). Returns the number of
    // expiries that won the resolve_ CAS.
    //
    // The timer-lifetime closure is the load-bearing property here: a retired
    // registration is observed via its atomic state BEFORE its node pointer is
    // touched. The heap may retain a stale physical entry for a retired timer
    // whose WaitNode has since been destroyed; pump skips it inertly.
    std::size_t won = 0;
    const deadline_t now = clock_now_unlocked();
    while (!deadline_heap_.empty()) {
        const detail::DeadlineHeapEntry front = deadline_heap_.front();
        if (front.deadline > now) break;  // earliest not yet due
        // The deadline heap holds tagged entries (Ordinary | Select).
        // Pop the min regardless of kind (lazy removal: inert entries leave
        // the heap here without their target ever being dereferenced for a
        // non-ACTIVE state). Copy `front` because pop invalidates the ref.
        heap_pop_min_locked();
        if (front.kind == detail::DeadlineHeapEntry::Kind::select) {
            // Select timer branch (Addendum D/E). State-before-arm: the branch
            // body loads state first; non-ACTIVE skips (PumpSkip), ACTIVE
            // fails fast (a due ACTIVE Select entry is unreachable in valid
            // production state — no admission path). Physical reclamation only
            // here: the
            // retire/consume helper already decremented active_deadline_count_
            // exactly once; the stale-pop path MUST NOT decrement again.
            select_timer_pump_entry_locked(*front.target.select);
            erase_popped_select_registration_locked(front.target.select);
            continue;
        }
        TimerRegistration* top = front.target.ordinary;
        // Timer-lifetime gate: claim the timer authority BEFORE dereferencing the
        // node. If
        // the registration is RETIRED (non-timer winner closed it) or already
        // CONSUMED (an earlier expiry won), skip — do NOT touch node/queue.
        // The active count was already decremented when the registration was
        // retired/consumed by the non-timer winner path.
        if (!consume_ordinary_deadline_locked(*top)) {
            // Inert stale entry, now dropped from the heap. Erase its pool block
            // too so the pool never accumulates dead registrations (the block's
            // node may already be destroyed; erase_popped_registration_locked
            // matches by ADDRESS without reading node/queue — timer-lifetime
            // safe).
            erase_popped_registration_locked(top);
            continue;
        }
        // ACTIVE->CONSUMED won (count decremented inside the helper).
        // ACTIVE->CONSUMED won: this expiry owns the timer authority. Resolve
        // the bound node through the canonical seam. The {node, queue} pointers
        // are valid: retirement happens only in the non-timer winner's
        // global_mtx_ CS, and we hold global_mtx_ here, so a concurrent retire
        // cannot have flipped the state after our winning claim.
        WaitNode* n = top->node();
        WaitQueue* q = top->queue();
        // Resolve the wait and, regardless of whether the CAS won, erase the
        // consumed block: it is no longer active, and the resolve_ CAS is the
        // publication guard. Erasing keeps the pool bounded by live deadline
        // waits (no accumulation across epochs).
        if (n != nullptr && q != nullptr) {
            // RwLock timer: route to rwlock_expire_wait (expire + head
            // reconcile + publish). Identified by the on_resolve function ptr.
            // rwlock_expire_wait acquires W internally, so we must NOT hold it.
            if (top->on_resolve_ == &rwlock_timer_expire_reconcile) {
                auto* ctx = static_cast<AsyncRwLock::ExpireCtx*>(top->owner_ctx_);
                if (ctx == nullptr) {
                    // A RwLock timer without a valid ExpireCtx is internal state
                    // corruption (Category B): the context is address-stable for
                    // the AsyncRwLock lifetime and set unconditionally at
                    // construction. Silently erasing here would leave the bound
                    // node unresolved in its queue. Fail fast instead.
                    assert(false && "E12-F pump: RwLock timer with null ExpireCtx "
                                    "(Category B internal invariant violation)");
                    std::abort();
                }
                if (rwlock_expire_wait(*ctx->waiters, *ctx->active_readers,
                                       *ctx->writer_active, *ctx->writer_owner,
                                       *n)) {
                    ++won;
                }
                erase_popped_registration_locked(top);
                continue;
            }
            // Generic / Queue path: expire inline under qlk.
            // expire_wait re-acquires global_mtx_ + q.mtx(); but we ALREADY hold
            // global_mtx_. Call the resolve path inline (no re-acquire) to avoid
            // a self-deadlock. The registration is already consumed; the resolve
            // CAS is the publication guard.
            LockGuard qlk(q->mtx());
            if (q->expire_locked(*n)) {
                Fiber* f = n->fiber();
                // Queue-bound registration: the
                // pump performs the per-port `--active_wait_associations_`
                // (via owner_ctx) and `--active_queue_timers_` (via the
                // on-resolve thunk) decrements. Non-Queue registrations have
                // no thunk and no owner_ctx; the `--waiting_waitq_count_`
                // Scheduler-wide accounting below applies unchanged.
                if (top->has_on_resolve()) {
                    auto* port = static_cast<detail::QueuePort*>(top->owner_ctx_);
                    if (port != nullptr &&
                        port->active_wait_associations_ > 0) {
                        --port->active_wait_associations_;
                    }
                    top->fire_on_resolve_locked(/*timer_won=*/true);
                }
                if (waiting_waitq_count_ > 0) --waiting_waitq_count_;
                // Route to the Fiber's recorded owner (NOT g_worker).
                if (f != nullptr && f->make_runnable()) {
                    WorkerState* owner = owner_for_fiber_locked(f);
                    route_runnable_locked(f, owner);
                    ++won;
                }
            }
        }
        erase_popped_registration_locked(top);
    }
    // The pump may have retired/consumed/erased entries; refresh the park-
    // timeout cache so park_on_wake_source sees the new earliest obligation.
    recompute_earliest_deadline_locked();
    return won;
}

// ---- Ordinary deadline lifecycle authority (AC-2b) ----
// Implementations of the three helpers declared in scheduler.hpp. See the
// header block for the authority boundary: these own ONLY the uniform
// ordinary-TimerRegistration facts (pool publication + ACTIVE count, exactly-
// once ACTIVE->terminal with count decrement). Cache-recompute timing and
// on_resolve hook firing stay at the call sites where their ordering is
// proven per-primitive.

TimerRegistration* Scheduler::arm_ordinary_deadline_locked(WaitNode* node,
                                                           WaitQueue* q,
                                                           deadline_t deadline,
                                                           TimerRegistration::OnResolveFn on_resolve,
                                                           void* owner_ctx) {
    timer_pool_.emplace_back(node, q, deadline);
    TimerRegistration* reg = &timer_pool_.back();
    reg->on_resolve_ = on_resolve;
    reg->owner_ctx_ = owner_ctx;
    ++active_deadline_count_;
    heap_push_ordinary_locked(reg);
    recompute_earliest_deadline_locked();
    return reg;
}

bool Scheduler::consume_ordinary_deadline_locked(TimerRegistration& reg) {
    if (!reg.try_claim_expiry()) return false;  // lost: no count mutation
    --active_deadline_count_;
    return true;
}

bool Scheduler::retire_ordinary_deadline_locked(TimerRegistration& reg) {
    if (!reg.retire()) return false;  // already terminal: no count mutation
    --active_deadline_count_;
    return true;
}

void Scheduler::retire_timer_for_node_locked(WaitNode& node) {
    // Timer Lifetime Closure. Called by the non-timer winner
    // (wake_wait_one / cancel_wait) in the SAME global_mtx_ CS as the resolve
    // CAS, BEFORE runnable publication. Performs ACTIVE->RETIRED on the bound
    // registration's independently-stable state. A later stale expiry then
    // observes RETIRED in pump_deadlines_locked / try_claim_expiry and MUST NOT
    // dereference the node (which may have been destroyed after the fiber
    // resumed).
    //
    // Timer-lifetime-safe scan: only ACTIVE registrations are inspected for a node match.
    // An ACTIVE registration is provably bound to a LIVE, still-Registered node
    // (a node is destroyed only after its wait epoch resolves, and resolving
    // retires/consumes the registration in the SAME CS — so while a block is
    // ACTIVE its bound node has not been destroyed). Inert blocks (bound to a
    // possibly-destroyed node) are skipped WITHOUT reading node(). This is the
    // load-bearing difference: we never read the node() of a block whose node
    // may be gone. The ACTIVE block matching the live `node` is retired.
    for (auto& r : timer_pool_) {
        if (!r.is_active()) continue;  // inert: node may be destroyed; skip
        if (r.node() == &node) {
            // ACTIVE->RETIRED (closes callback authority). The helper owns the
            // count decrement; the per-port hook fires here because its
            // timer_won=false ordering is retire-specific.
            if (retire_ordinary_deadline_locked(r)) {
                // Queue-bound timer: a retire decrements
                // the per-port active_queue_timers_ counter via the on-resolve
                // thunk. timer_won=false (the timer LOST — a non-timer winner
                // retired it). Idempotent: the CAS above is the single
                // ACTIVE->terminal transition.
                r.fire_on_resolve_locked(/*timer_won=*/false);
            }
            recompute_earliest_deadline_locked();  // refresh park-timeout cache
            return;
        }
    }
}

bool Scheduler::any_active_deadline_locked() const {
    // True if any registration is still ACTIVE (an unresolved deadline wait).
    // Uses the O(1) active_deadline_count_ maintained across all state
    // transitions. Called by external_wake_possible_locked so a Live run with
    // an active deadline parks and MW classification treats the deadline
    // as an external-wake source.
    return active_deadline_count_ > 0;
}

bool Scheduler::earliest_active_deadline_locked(deadline_t& out) const {
    // Return the earliest ACTIVE deadline (min-heap front, skipping inert
    // entries). Used to bound park_on_wake_source. The heap is lazily
    // cleaned by pump_deadlines_locked; here we just scan for the min ACTIVE.
    // Select ACTIVE deadlines participate exactly like ordinary ones,
    // so both pools are scanned.
    bool found = false;
    deadline_t best = 0;
    for (const auto& r : timer_pool_) {
        if (!r.is_active()) continue;
        if (!found || r.deadline() < best) {
            best = r.deadline();
            found = true;
        }
    }
    for (const auto& r : select_timer_pool_) {
        if (!r.is_active()) continue;
        if (!found || r.deadline() < best) {
            best = r.deadline();
            found = true;
        }
    }
    if (found) out = best;
    return found;
}

void Scheduler::recompute_earliest_deadline_locked() {
    // Recompute the earliest ACTIVE deadline from the pool and publish it to the
    // atomic cache. Called under global_mtx_ after every heap/pool mutation so
    // park_on_wake_source can read it LOCK-FREE (avoiding a wake_mtx_ ->
    // global_mtx_ lock-order inversion). O(pool); the pool holds at most one
    // entry per concurrent deadline wait.
    // Select ACTIVE deadlines participate exactly like ordinary ones,
    // so both pools are scanned.
    deadline_t best = kNoDeadline;
    bool found = false;
    for (const auto& r : timer_pool_) {
        if (!r.is_active()) continue;
        if (!found || r.deadline() < best) {
            best = r.deadline();
            found = true;
        }
    }
    for (const auto& r : select_timer_pool_) {
        if (!r.is_active()) continue;
        if (!found || r.deadline() < best) {
            best = r.deadline();
            found = true;
        }
    }
    earliest_active_deadline_.store(found ? best : kNoDeadline,
                                    std::memory_order::release);
}

void Scheduler::erase_popped_registration_locked(TimerRegistration* r) {
    // Erase a registration's pool block. SAFE only because the caller
    // (pump_deadlines_locked) has ALREADY popped the block from the deadline
    // heap, so no live heap slot still holds its pointer. The block is
    // non-ACTIVE (retired/consumed) and is erased by ADDRESS match (no
    // node()/queue() read) — timer-lifetime safe. O(pool size): the pool holds
    // at most one
    // entry per concurrent deadline wait, so this scan is small.
    if (r == nullptr) return;
    for (auto it = timer_pool_.begin(); it != timer_pool_.end(); ++it) {
        if (&*it == r) {
            timer_pool_.erase(it);
            return;  // r is now dangling; caller must not touch it
        }
    }
}

TimerRegistration* Scheduler::register_test_deadline_locked(WaitNode* node,
                                                            WaitQueue* q,
                                                            deadline_t deadline) {
    // Narrow test hook. Creates an ACTIVE TimerRegistration for
    // {node, q, deadline}, pushes it into the deadline heap, refreshes the
    // earliest-deadline park cache, AND registers `node` into `q` (Detached ->
    // Registered) so the pump's expire path can resolve it. This mirrors the
    // full await_wait_deadline admission MINUS the fiber-suspend path: it
    // increments waiting_waitq_count_ (so the pump's decrement on win balances)
    // and registers the node, but does NOT suspend a fiber, so the coordinator
    // can install a NEW deadline from a NON-worker thread while the worker is
    // held at the park-commit seam (global_mtx_ is released at that seam).
    // Called by the test coordinator. See tests/timer_wait_test.cpp T17.
    // TEST-ONLY; no production caller.
    if (clock_now_unlocked() >= deadline) return nullptr;  // already due: skip
    if (q != nullptr) {
        LockGuard qlk(q->mtx());
        if (!q->register_wait_locked(*node, nullptr)) return nullptr;  // not Detached
    }
    ++waiting_waitq_count_;  // mirror admission accounting (pump decrements on win)
    // Arm through the ordinary deadline authority. The raw-pointer form
    // preserves this seam's historical contract exactly — including the
    // never-dereferenced null-node binding the old emplace_back accepted.
    return arm_ordinary_deadline_locked(node, q, deadline);
}

// ---- deadline heap helpers (min-heap on deadline) ----
// The heap stores unified DeadlineHeapEntry values (Ordinary | Select).
// The comparator (detail::heap_less_entry) compares cached deadlines only;
// equal-deadline order is unspecified. sift/pop operate on vector entries and
// no longer touch any registration's heap_index (the entry's vector position
// is the sole position authority — Addendum G).

void Scheduler::heap_push_entry_locked(const detail::DeadlineHeapEntry& e) {
    deadline_heap_.push_back(e);
    heap_sift_up_locked(deadline_heap_.size() - 1);
}

void Scheduler::heap_push_ordinary_locked(TimerRegistration* r) {
    heap_push_entry_locked(detail::DeadlineHeapEntry::for_ordinary(*r));
}

void Scheduler::heap_pop_min_locked() {
    if (deadline_heap_.empty()) return;
    detail::DeadlineHeapEntry last = deadline_heap_.back();
    deadline_heap_.pop_back();
    if (!deadline_heap_.empty()) {
        deadline_heap_[0] = last;
        heap_sift_down_locked(0);
    }
}

void Scheduler::heap_sift_up_locked(std::size_t i) {
    while (i > 0) {
        std::size_t parent = (i - 1) / 2;
        if (!detail::heap_less_entry(deadline_heap_[i], deadline_heap_[parent])) break;
        std::swap(deadline_heap_[i], deadline_heap_[parent]);
        i = parent;
    }
}

void Scheduler::heap_sift_down_locked(std::size_t i) {
    const std::size_t n = deadline_heap_.size();
    while (true) {
        std::size_t l = 2 * i + 1;
        std::size_t r = 2 * i + 2;
        std::size_t best = i;
        if (l < n && detail::heap_less_entry(deadline_heap_[l], deadline_heap_[best])) best = l;
        if (r < n && detail::heap_less_entry(deadline_heap_[r], deadline_heap_[best])) best = r;
        if (best == i) break;
        std::swap(deadline_heap_[i], deadline_heap_[best]);
        i = best;
    }
}

}  // namespace sluice::async
