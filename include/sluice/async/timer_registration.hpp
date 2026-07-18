// sluice::async::TimerRegistration — E11 timer registration control block.
//
// A Scheduler-owned, independently-stable control block that represents one
// active deadline wait. It is the load-bearing E11 difference from E10:
//
//   E10 logical loser-safety holds ONLY while the WaitNode object is still
//   alive (its absorbing terminal state rejects a straggling resolve_ CAS).
//
//   E11 must additionally be safe AFTER the WaitNode storage is destroyed
//   (the fiber resumes and its caller-owned node goes out of scope), because
//   a physically-retained/lazy timer entry can outlive the node. A stale
//   expiry that captured only a raw WaitNode* would dereference freed memory
//   when the node's stack slot was reused by the next wait epoch.
//
// TimerRegistration solves this by decoupling LOGICAL timer authority from the
// WaitNode ADDRESS. Its atomic `state` (ACTIVE / RETIRED / CONSUMED) is the
// independently-stable retirement identity:
//
//   ACTIVE   : registered; an expiry may claim it (CAS ACTIVE->CONSUMED).
//   RETIRED  : a NON-timer winner (wake/cancel) closed callback authority. A
//              later expiry observes RETIRED and MUST NOT dereference the
//              bound WaitNode — regardless of whether that node's storage
//              is still alive.
//   CONSUMED : a timer expiry won; callback authority closed.
//
// The decisive check (I4 / TimerLifetimeClosure): an expiry loads `state`
// BEFORE it dereferences `node`. If the load is not ACTIVE, the expiry returns
// without touching the node. Because retirement is established by the
// non-timer winner in the SAME global_mtx_ critical section that publishes the
// runnable ticket (and hence strictly before the fiber can resume and destroy
// its node), a stale expiry can never observe ACTIVE for a destroyed node.
//
// Lifetime/ownership: the Scheduler owns the set of TimerRegistration objects
// (stored by value in a pointer-stable container: std::list). Each block lives
// exactly for one wait epoch. Logical retirement (ACTIVE->RETIRED) is immediate
// (the non-timer winner performs it in the same CS as the resolve CAS); but
// PHYSICAL erasure from the heap+pool is LAZY-AT-DEADLINE — the pump erases a
// block only when now >= its deadline, regardless of its state. So a far-future
// RETIRED block remains physically in the pool until its original deadline is
// reached (proven by e11_t18). It MUST outlive any WaitNode it is bound to (it
// is heap-allocated by the Scheduler, while the node is a fiber-frame local),
// satisfying the post-destruction window.
//
// Synchronization: `state` is std::atomic (lock-free acquire/release) so an
// expiry path can observe retirement without taking global_mtx_. The bound
// {node, queue} pointers are only read AFTER an ACTIVE observation, and only
// then under global_mtx_ + queue->mtx() (the expiry enters the canonical
// resolution critical section). This matches E9's SchedulerWakeHandle
// callback-lease discipline.
#pragma once

#include <atomic>
#include <cstdint>

namespace sluice::async {

class WaitNode;   // forward (the bound wait epoch identity)
class WaitQueue;  // forward (the resolution target's queue)
class Scheduler;  // forward (the pool that owns this block)

// A monotonic absolute time point (E11 Deadline). Stored as a 64-bit tick of
// the project/compiler monotonic clock; `expired iff now >= deadline`. The
// Scheduler owns the clock (a controllable one for tests, steady_clock for
// production) so this is a value type, not a clock-coupled type.
using deadline_tick_t = std::uint64_t;

// One timer registration control block. Non-copyable, non-movable (its address
// is its identity for the deadline-heap ordering back-reference). The atomic
// `state` is the retirement authority; the raw {node, queue} pointers are the
// live-wait binding, read only while ACTIVE is observed.
//
// E12-E Queue hook (Corrective-2 §8 supersession): a Queue-bound registration
// carries an optional `owner_ctx_` + `on_resolve_` thunk so the Scheduler can
// perform per-port counter bookkeeping at retire/consume. Non-Queue waits
// (Event/Semaphore/Mutex/Condition) leave both null and the Scheduler's
// default `--waiting_waitq_count_` accounting applies unchanged. The thunk is
// a plain fn ptr (no allocation, no capture); the Scheduler calls it under
// global_mtx_ exactly once per registration resolution.
class TimerRegistration {
public:
    // Per-resolution bookkeeping callback. Invoked by the Scheduler under
    // global_mtx_ when this registration transitions out of ACTIVE (either
    // via retire, by a non-timer winner, or via consume, by an expiry). The
    // `owner_ctx` is opaque to the Scheduler; the registrant (QueuePort)
    // interprets it. The bool argument is true if the timer WON the resolution
    // (consumed) and false if it lost (retired by another resolver).
    using OnResolveFn = void (*)(void* owner_ctx, bool timer_won) noexcept;

    // Independently-stable timer callback authority (distinct from WaitNode
    // terminal state). See file banner for the lifetime law.
    enum class State : std::uint8_t {
        active = 0,    // registered; an expiry may claim it
        retired = 1,   // non-timer winner closed callback authority
        consumed = 2,  // a timer expiry won; callback authority closed
    };

    TimerRegistration() = default;
    TimerRegistration(WaitNode* node, WaitQueue* queue, deadline_tick_t deadline) noexcept
        : node_(node), queue_(queue), deadline_(deadline) {}

    TimerRegistration(const TimerRegistration&) = delete;
    TimerRegistration& operator=(const TimerRegistration&) = delete;
    TimerRegistration(TimerRegistration&&) = delete;
    TimerRegistration& operator=(TimerRegistration&&) = delete;

    // The load-bearing expiry gate (I4). Returns true ONLY when this call is
    // the unique expiry authority: it CAS ACTIVE->CONSUMED. Any other state
    // (RETIRED by a non-timer winner, or CONSUMED by an earlier expiry) makes
    // this return false, and the caller MUST NOT dereference node_/queue_.
    // Acq_rel: success publishes CONSUMED to a racing non-timer retire; failure
    // (acquire) lets the loser observe the winner's terminal state.
    bool try_claim_expiry() noexcept {
        State expected = State::active;
        return state_.compare_exchange_strong(expected, State::consumed,
                                              std::memory_order::acq_rel,
                                              std::memory_order::acquire);
    }

    // Retire the registration (close callback authority). Called by the
    // non-timer winner path (wake_wait_one / cancel_wait) in the same
    // global_mtx_ critical section that resolves the node, BEFORE runnable
    // publication. CAS ACTIVE->RETIRED. If the state is already CONSUMED (a
    // timer expiry concurrently won) this is a no-op loser — the timer winner
    // owns publication. Returns true if THIS call retired an active timer.
    bool retire() noexcept {
        State expected = State::active;
        return state_.compare_exchange_strong(expected, State::retired,
                                              std::memory_order::acq_rel,
                                              std::memory_order::acquire);
    }

    // Queries (for the Scheduler heap + tests). Acquire pairs with the
    // claim/retire release.
    bool is_active() const noexcept {
        return state_.load(std::memory_order::acquire) == State::active;
    }
    bool is_retired() const noexcept {
        return state_.load(std::memory_order::acquire) == State::retired;
    }
    bool is_consumed() const noexcept {
        return state_.load(std::memory_order::acquire) == State::consumed;
    }
    State state() const noexcept { return state_.load(std::memory_order::acquire); }

    // The bound wait-epoch identity + resolution target. Read by an expiry
    // ONLY AFTER try_claim_expiry() returned true (ACTIVE->CONSUMED), under
    // global_mtx_ + queue_->mtx(). The node pointer is NEVER dereferenced by
    // a retired/consumed registration — that is the post-destruction safety
    // boundary (I4).
    WaitNode* node() const noexcept { return node_; }
    WaitQueue* queue() const noexcept { return queue_; }
    deadline_tick_t deadline() const noexcept { return deadline_; }

    // E12-E Queue per-port resolution hook. Returns true iff this registration
    // is bound to a Queue wait (an on_resolve_ thunk + owner_ctx_ were
    // installed at admit time). The Scheduler calls fire_on_resolve_locked()
    // exactly once per ACTIVE->terminal transition under global_mtx_.
    bool has_on_resolve() const noexcept { return on_resolve_ != nullptr; }
    void fire_on_resolve_locked(bool timer_won) noexcept {
        // Caller MUST hold global_mtx_ AND have just performed the ACTIVE->
        // terminal CAS that won/lost this registration. The thunk is
        // idempotent under that serialisation (it is invoked exactly once per
        // registration lifetime).
        if (on_resolve_ != nullptr) {
            on_resolve_(owner_ctx_, timer_won);
        }
    }

    // Heap/pool linkage used by the Scheduler's deadline container. The
    // Scheduler stores TimerRegistration in a pointer-stable container
    // (std::list), so these index fields are for the binary-heap position
    // (lazy removal: a retired entry may remain physically present but inert).
    std::size_t heap_index = static_cast<std::size_t>(-1);

private:
    friend class Scheduler;  // owns the pool; erases inert blocks

    std::atomic<State> state_{State::active};
    WaitNode* node_{nullptr};
    WaitQueue* queue_{nullptr};
    deadline_tick_t deadline_{0};
    // E12-E Queue per-port bookkeeping. Both null for non-Queue waits.
    OnResolveFn on_resolve_{nullptr};
    void* owner_ctx_{nullptr};
};

}  // namespace sluice::async
