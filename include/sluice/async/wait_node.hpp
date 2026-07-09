// sluice::async::WaitNode — one canonical wait lifecycle (sluice-CORE-E10).
//
// E10 establishes the minimal cancellation-safe waiting primitive required by
// later deadline/timer integration, async synchronization primitives, and
// multi-wait/select. This is DELIBERATELY NARROW (see docs/e10-waitnode-wait-
// queue.md §1 scope):
//
//   IN scope:  one WaitNode lifecycle, one cancellation-safe WaitQueue
//              protocol, single-wait registration, wake-vs-cancel winner
//              protocol, safe unlink/removal, one canonical terminal seam.
//   OUT scope: timers, deadlines, mutex/sem/condvar/event/channel/select/
//              multi-wait/wait-any/wait-all, I/O/io_uring cancellation, task
//              cancellation propagation, structured concurrency.
//
// E10 wait cancellation is NOT task cancellation and is NOT I/O operation
// cancellation. Cancel here means only: resolve THIS registered wait with the
// Cancelled terminal outcome (§6 cancellation boundary).
//
// ---------------------------------------------------------------------------
// DESIGN LAW (§2 — ONE WINNER TRANSITION). Wake and cancellation do NOT
// implement separate state machines. There is ONE atomic authority for the
// terminal transition: resolve_(outcome) = CAS state_ registered -> outcome.
// That single CAS is the winner authority (§7): the winner CAS-then-unlinks;
// every loser observes the terminal state and performs no second wake.
//
//   Linearization point (§7): the instant resolve_(outcome)'s CAS stores the
//   terminal value. At that point the node is (a) terminally resolved,
//   (b) the unique winner, and (c) the unique owner of the unlink right.
//
// Memory model (§9): state_ is std::atomic. register_ uses acq_rel (publishes
// the membership/Registered state); resolve_ uses acq_rel (release publishes
// the terminal outcome, acquire lets a losing resolver observe the winner's
// outcome). is_terminal()/outcome() read with acquire. No blanket seq_cst
// (§9 forbids it without analysis); acq_rel is the simplest proven ordering
// compatible with the repository's concurrency style (Fiber::state_,
// Completion state).
//
// Two cleanly separated synchronization domains (§9), no overlap:
//   - STRUCTURAL (link fields next_/prev_/home_, queue membership): protected
//     by the owning WaitQueue's mtx_.
//   - WINNER (terminal outcome): the atomic state_ CAS.
// The winner resolver takes the queue mtx_ (to unlink), performs the CAS, and
// unlinks under the same critical section; a losing resolver's CAS fails and
// it performs no unlink. The CAS remains the authority even though both
// resolvers serialize on the queue mtx_, because is_terminal()/outcome() are
// read LOCK-FREE by any thread (the scheduler, the resuming fiber, tests).
//
// Lifetime/ownership (§3): a WaitNode is CALLER-OWNED (mirrors Completion<T>'s
// L7 address-stability discipline). It is constructed in the await frame of
// the waiting fiber/task and destroyed when that frame exits. The node MUST be
// terminal (Woken/Cancelled) — or never registered — before its owning frame
// is destroyed; a debug assert enforces that a Registered node is never
// destroyed (§10/C9).
//
// Layering: BELOW the Scheduler. WaitNode carries NO scheduler reference and
// NO future-specific state (no timer_id/deadline/select_group/...). The Fiber
// handle is an opaque pointer recorded so the scheduler wake seam (which IS
// scheduler code) can route the resumed fiber; WaitNode itself never touches
// the scheduler.
#pragma once

#include <atomic>
#include <cassert>
#include <cstdint>

namespace sluice::async {

class Fiber;      // forward (the scheduler-facing handle)
class WaitQueue;  // forward (friend: link fields + register/detach)

// The terminal outcome of a wait resolution (§2/§6). Repository-native names
// for the two allowed E10 terminal outcomes.
enum class WaitOutcome : std::uint8_t {
    // Not yet terminal (the node is detached or registered but unresolved).
    // Returned by outcome() for a non-terminal node so callers can distinguish.
    unresolved = 0,
    // The wait was resolved by a wake (e.g. WaitQueue::wake_one). The unique
    // winner made the waiting execution runnable through the canonical
    // scheduler seam.
    woken = 1,
    // The wait was resolved by cancellation (WaitQueue::cancel). E10 cancel is
    // wait-cancellation only — see the header banner.
    cancelled = 2,
};

// One registered wait. A WaitNode lives inside a single await frame and is
// registered into at most one WaitQueue at a time. Non-copyable AND
// non-movable: identity is the object address (the intrusive link fields and
// the scheduler's wait map key on it), exactly like Completion<T>.
//
// State machine (§2):
//
//   Detached ──register──> Registered ─┬─resolve(Woken)─────> Woken      [T]
//        ▲                              └─resolve(Cancelled)─> Cancelled [T]
//
//   Detached    : initial; never linked. register_() moves to Registered.
//                 (A node that was never registered may be destroyed.)
//   Registered  : linked in exactly one WaitQueue; resolvable.
//   Woken / Cancelled : absorbing terminal states. The winner unlinks the
//                 node (under the queue mtx_) but the terminal state is kept
//                 forever so outcome() stays queryable.
//
// Who mutates what (§3 ownership):
//   - state_ : register_()/resolve_() via atomic CAS.
//   - link fields (next_/prev_/home_) : the owning WaitQueue, under its mtx_.
//   - fiber_ : immutable after construction.
class WaitNode {
public:
    WaitNode() noexcept = default;

    // Construct with the scheduler-facing fiber handle. `fiber` is opaque to
    // WaitNode (it never dereferences it); the scheduler wake seam uses it to
    // route the resumed fiber. May be null for pure-protocol tests.
    explicit WaitNode(Fiber* fiber) noexcept : fiber_(fiber) {}

    // A Registered node may not be destroyed (§10/C9): it is still linked in a
    // queue and destroying it would leave a dangling queue pointer (§3 Q7).
    // Debug asserts; release is a no-op. The canonical recovery is to cancel
    // (or wake) the wait first — the winner unlinks it.
    ~WaitNode() {
        assert(!is_registered() &&
               "WaitNode destroyed while Registered (resolve the wait first)");
    }

    WaitNode(const WaitNode&) = delete;
    WaitNode& operator=(const WaitNode&) = delete;
    WaitNode(WaitNode&&) = delete;
    WaitNode& operator=(WaitNode&&) = delete;

    // ---- State queries (acquire: pair with resolve_/register_ release) ----

    bool is_registered() const noexcept {
        return state_.load(std::memory_order::acquire) == State::registered;
    }
    bool is_terminal() const noexcept {
        const auto s = state_.load(std::memory_order::acquire);
        return s == State::woken || s == State::cancelled;
    }
    // The terminal outcome, or WaitOutcome::unresolved if not yet terminal.
    // Acquire so a losing resolver observes the winner's published outcome (§9).
    WaitOutcome outcome() const noexcept {
        const auto s = state_.load(std::memory_order::acquire);
        if (s == State::woken) return WaitOutcome::woken;
        if (s == State::cancelled) return WaitOutcome::cancelled;
        return WaitOutcome::unresolved;
    }
    bool was_woken() const noexcept {
        return state_.load(std::memory_order::acquire) == State::woken;
    }
    bool was_cancelled() const noexcept {
        return state_.load(std::memory_order::acquire) == State::cancelled;
    }

    // The opaque scheduler-facing fiber handle (immutable).
    Fiber* fiber() const noexcept { return fiber_; }

    // Intrusive link pointers (managed by WaitQueue under its mtx_). Public so
    // WaitQueue can touch them without friending; documented as NOT for users.
    WaitNode* next_{nullptr};
    WaitNode* prev_{nullptr};
    WaitQueue* home_{nullptr};  // queue this node is registered in (null iff not registered)

private:
    friend class WaitQueue;

    // Internal lifecycle state (distinct from WaitOutcome, which is the public
    // terminal-outcome projection). detached is the initial/pre-link state.
    enum class State : std::uint8_t {
        detached = 0,    // initial; never linked
        registered = 1,  // linked in exactly one queue; resolvable
        woken = 2,       // terminal: resolved by wake (absorbing)
        cancelled = 3,   // terminal: resolved by cancel (absorbing)
    };

    // Register this node into `q` (Detached -> Registered) and record the
    // scheduler-facing `fiber` handle. Called by WaitQueue under its mtx_
    // during enqueue. Returns false (no transition) if the node is already
    // registered or terminal — register is single-shot per wait, which is the
    // C8 reuse-rejection contract.
    bool register_(WaitQueue* q, Fiber* fiber) noexcept {
        State expected = State::detached;
        if (!state_.compare_exchange_strong(expected, State::registered,
                                            std::memory_order::acq_rel,
                                            std::memory_order::acquire)) {
            return false;  // already registered or terminal
        }
        fiber_ = fiber;
        home_ = q;
        return true;
    }

    // The canonical ONE-WINNER terminal resolver (§2 Design Law, §7 Unlink
    // Law). CAS state_ Registered -> {woken,cancelled}. Returns true ONLY when
    // this call is the unique winner (CAS succeeded). Every losing caller
    // returns false and MUST perform no second wake/unlink.
    bool resolve_(WaitOutcome outcome) noexcept {
        State target;
        if (outcome == WaitOutcome::woken) target = State::woken;
        else if (outcome == WaitOutcome::cancelled) target = State::cancelled;
        else { assert(false && "resolve_ requires a terminal outcome"); return false; }
        State expected = State::registered;
        return state_.compare_exchange_strong(expected, target,
                                              std::memory_order::acq_rel,
                                              std::memory_order::acquire);
    }

    Fiber* fiber_{nullptr};
    std::atomic<State> state_{State::detached};
};

}  // namespace sluice::async
