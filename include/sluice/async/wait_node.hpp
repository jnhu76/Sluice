// sluice::async::WaitNode — one canonical wait lifecycle.
//
// WaitNode is the minimal cancellation-safe waiting primitive required by
// deadline/timer integration, async synchronization primitives, and
// multi-wait/select. This is DELIBERATELY NARROW (scope):
//
//   IN scope:  one WaitNode lifecycle, one cancellation-safe WaitQueue
//              protocol, single-wait registration, wake-vs-cancel winner
//              protocol, safe unlink/removal, one canonical terminal seam.
//              The terminal outcomes extend to `expired` (deadline expiry),
//              reached ONLY through the Scheduler expiry seam and the
//              private WaitQueue::expire_locked resolver — still one
//              resolve_ authority, no second winner protocol.
//   OUT scope: mutex/sem/condvar/event/channel/select/multi-wait/wait-any/
//              wait-all, I/O/io_uring cancellation, task cancellation
//              propagation, structured concurrency, high-level sleep/timerfd
//              public APIs, timer-wheel optimization.
//
// Wait cancellation is NOT task cancellation and is NOT I/O operation
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
// destroyed (§10).
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

// The frontend-neutral ResumeTarget token (FE-1b frozen contract, role
// "ResumeTarget"): the opaque datum bound to a wait epoch at admission, which
// the winner's publication tail consults to select the delivery mechanism.
// Plain data: no behavior, no ownership, no allocation, no virtual dispatch.
//
// Kind is the delivery discriminator (FE-1c §13/§14):
//   none     - no continuation bound (pure-protocol/test epochs). The
//              publication tail publishes nothing, exactly like the null
//              Fiber* it replaces.
//   fiber    - the stackful frontend: `ptr` is a Fiber*; publication goes
//              through the canonical make_runnable + worker route seam.
//   deferred - a frontend whose continuation record is discharged outside
//              authoritative locks (experimental stackless frontend; the
//              record is frame-embedded and subject to the SAME
//              address-stability rule as the WaitNode itself).
//
// NOT an ActorIdentity (FE-1b L11): ownership semantics (Mutex/RwLock owner
// comparisons) must never compare this value. The Core never dereferences
// `ptr` for a semantic decision; only the publication tail switches on kind.
class WaitResume {
public:
    enum class Kind : std::uint8_t { none = 0, fiber = 1, deferred = 2 };

    constexpr WaitResume() noexcept = default;

    static constexpr WaitResume none() noexcept { return WaitResume{}; }
    // kind==fiber implies a valid non-null Fiber*: a null token normalizes
    // to `none` at this single construction point (one coherent rule — the
    // publication tails switch on kind and never see a null fiber-kind; the
    // null-skip guards in the legacy expire/cancel tails are exactly the
    // incoherence this removes).
    static constexpr WaitResume fiber(Fiber* f) noexcept {
        return f != nullptr ? WaitResume{f, Kind::fiber} : WaitResume{};
    }
    static constexpr WaitResume deferred(void* delivery_record) noexcept {
        return WaitResume{delivery_record, Kind::deferred};
    }

    constexpr Kind kind() const noexcept { return kind_; }
    // Precondition: kind() == Kind::fiber (the publication tail switches on
    // kind BEFORE calling; no runtime check — the payload is opaque data).
    constexpr Fiber* as_fiber() const noexcept {
        return static_cast<Fiber*>(ptr_);
    }
    // Precondition: kind() == Kind::deferred.
    constexpr void* as_deferred() const noexcept { return ptr_; }

private:
    constexpr WaitResume(void* p, Kind k) noexcept : ptr_(p), kind_(k) {}
    void* ptr_ = nullptr;
    Kind kind_ = Kind::none;
};

// ActorIdentity (FE-1b A1): the stable logical identity of an execution
// ACTOR for ownership / recursive-detection comparisons (e.g. the RwLock
// writer-owner model). Deliberately DISTINCT from WaitResume (the
// ResumeTarget delivery token above): a Fiber frontend's actor and resume
// token coincide (the same Fiber*), while a stackless frontend's actor is
// its own stable token and its resume target is the delivery record —
// ownership semantics must not depend on WHERE control resumes (FE-3 slice
// contract: "same ActorIdentity + different ResumeTarget" and "ownership
// semantics do not depend on ResumeTarget identity"). Never a
// coroutine_handle: identity is an opaque stable token, not a resumable
// capability. The Core compares ActorIds; it never dereferences the token.
class ActorId {
public:
    enum class Kind : std::uint8_t { none = 0, fiber = 1, frontend = 2 };

    constexpr ActorId() noexcept = default;

    static constexpr ActorId none() noexcept { return ActorId{}; }
    static constexpr ActorId fiber(Fiber* f) noexcept {
        return ActorId{f, Kind::fiber};
    }
    // A frontend-owned stable token (e.g. a frame-embedded actor record).
    static constexpr ActorId frontend(void* token) noexcept {
        return ActorId{token, Kind::frontend};
    }

    constexpr Kind kind() const noexcept { return kind_; }
    constexpr void* token() const noexcept { return ptr_; }

    friend constexpr bool operator==(const ActorId& a,
                                     const ActorId& b) noexcept {
        return a.ptr_ == b.ptr_ && a.kind_ == b.kind_;
    }
    friend constexpr bool operator!=(const ActorId& a,
                                     const ActorId& b) noexcept {
        return !(a == b);
    }

private:
    constexpr ActorId(void* p, Kind k) noexcept : ptr_(p), kind_(k) {}
    void* ptr_ = nullptr;
    Kind kind_ = Kind::none;
};

// The terminal outcome of a wait resolution (§2/§6). Repository-native names
// for the allowed terminal outcomes: woken/cancelled, plus `expired` (a
// monotonic deadline elapsed) as a THIRD terminal outcome that is
// observably distinct from cancellation and competes for the SAME resolve_
// authority (no second winner protocol).
enum class WaitOutcome : std::uint8_t {
    // Not yet terminal (the node is detached or registered but unresolved).
    // Returned by outcome() for a non-terminal node so callers can distinguish.
    unresolved = 0,
    // The wait was resolved by a wake (e.g. WaitQueue::wake_one). The unique
    // winner made the waiting execution runnable through the canonical
    // scheduler seam.
    woken = 1,
    // The wait was resolved by cancellation (WaitQueue::cancel). This cancel
    // is wait-cancellation only — see the header banner.
    cancelled = 2,
    // The wait was resolved by a monotonic deadline elapsing
    // (TIMER_EXPIRE). Distinct from cancellation: the deadline elapsed, the
    // resource did not become ready and the wait was not cancelled. Reached
    // only through the Scheduler expiry seam (expire_wait) which calls the
    // private WaitQueue::expire_locked -> resolve_(expired), exactly mirroring
    // wake/cancel authority. Timeout is NOT cancellation.
    expired = 3,
};

// One registered wait. A WaitNode lives inside a single await frame and is
// registered into at most one WaitQueue at a time. Non-copyable AND
// non-movable: identity is the object address (the intrusive link fields and
// the scheduler's wait map key on it), exactly like Completion<T>.
//
// State machine (§2):
//
//   Detached ──register──> Registered ─┬─resolve(Woken)─────> Woken      [T]
//        ▲                              ├─resolve(Cancelled)─> Cancelled [T]
//        │                              └─resolve(Expired)───> Expired   [T]
//
//   Detached    : initial; never linked. register_() moves to Registered.
//                 (A node that was never registered may be destroyed.)
//   Registered  : linked in exactly one WaitQueue; resolvable.
//   Woken / Cancelled / Expired : absorbing terminal states. The winner
//                 unlinks the node (under the queue mtx_) but the terminal
//                 state is kept forever so outcome() stays queryable.
//
// Who mutates what (§3 ownership):
//   - state_ : register_()/resolve_() via atomic CAS.
//   - link fields (next_/prev_/home_) : the owning WaitQueue, under its mtx_.
//   - resume_ : immutable after registration (bound by register_).
class WaitNode {
public:
    WaitNode() noexcept = default;

    // Construct with a ResumeTarget token bound at registration (FE-1b L2).
    // The token is opaque to WaitNode (never dereferenced); the winner
    // publication tail switches on its kind to select delivery. `none` (the
    // default) is the pure-protocol form and publishes nothing.
    explicit WaitNode(WaitResume resume) noexcept : resume_(resume) {}
    // Stackful convenience form (unchanged public surface): binds a Fiber*
    // ResumeTarget, exactly WaitResume::fiber(fiber).
    explicit WaitNode(Fiber* fiber) noexcept
        : resume_(WaitResume::fiber(fiber)) {}

    // A Registered node may not be destroyed (§10): it is still linked in a
    // queue and destroying it would leave a dangling queue pointer (§3).
    // Debug asserts; release is a no-op. The canonical recovery is to cancel
    // (or wake) the wait first — the winner unlinks it.
    ~WaitNode() {
        assert(!is_registered() &&
               "WaitNode destroyed while Registered (resolve the wait first)");
    }

    // ---- Per-operation context hook (AsyncQueue / AsyncRwLock) ----
    // An optional, caller-owned opaque pointer stashed on the node BEFORE
    // registration, so a reconciler that resolves THIS node (via
    // wake_one_locked, which returns the winning WaitNode*) can reach the
    // per-operation context it needs to finalize atomically. Authorized
    // production users: AsyncQueue and AsyncRwLock; it is never
    // dereferenced by WaitQueue or by the generic Scheduler wake path. Null by
    // default; each primitive sets it to its wait-node context (QueueWaitCtx*
    // or RwWaitCtx*) before registration and clears it after terminal
    // resolution. Kept trivial (no ownership) so the node remains trivially
    // relocatable in spirit and zero-cost when unused. The linked node's user_
    // is read ONLY by the owning Scheduler seam under G + W while linked — it
    // is NOT a general-purpose user payload.
    void* user() const noexcept { return user_; }
    void set_user(void* p) noexcept { user_ = p; }

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
        return s == State::woken || s == State::cancelled || s == State::expired;
    }
    // The terminal outcome, or WaitOutcome::unresolved if not yet terminal.
    // Acquire so a losing resolver observes the winner's published outcome (§9).
    WaitOutcome outcome() const noexcept {
        const auto s = state_.load(std::memory_order::acquire);
        if (s == State::woken) return WaitOutcome::woken;
        if (s == State::cancelled) return WaitOutcome::cancelled;
        if (s == State::expired) return WaitOutcome::expired;
        return WaitOutcome::unresolved;
    }
    bool was_woken() const noexcept {
        return state_.load(std::memory_order::acquire) == State::woken;
    }
    bool was_cancelled() const noexcept {
        return state_.load(std::memory_order::acquire) == State::cancelled;
    }
    // The wait resolved by a monotonic deadline elapsing (distinct from
    // cancellation). Lock-free acquire, like was_woken/was_cancelled.
    bool was_expired() const noexcept {
        return state_.load(std::memory_order::acquire) == State::expired;
    }

    // The frontend-neutral ResumeTarget bound at registration (immutable
    // after register_ succeeds). FE-1b L2: fully bound before the epoch is
    // resolver-observable — the binding happens under the authoritative
    // admission critical section, so the semantic rule is satisfied by the
    // CS, not by the textual order relative to the state CAS.
    const WaitResume& resume() const noexcept { return resume_; }
    // Stackful view of the token (existing consumer convenience). For a
    // non-fiber token this is the raw payload reinterpreted — callers must
    // switch on resume().kind() first. Publication tails do.
    Fiber* fiber() const noexcept { return resume_.as_fiber(); }

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
        expired = 4,     // terminal: resolved by deadline expiry (absorbing)
    };

    // Register this node into `q` (Detached -> Registered) and bind the
    // ResumeTarget token. Called by WaitQueue under its mtx_ during enqueue.
    // Returns false (no transition) if the node is already registered or
    // terminal (including expired) — register is single-shot per wait, which
    // is the reuse-rejection contract.
    bool register_(WaitQueue* q, const WaitResume& resume) noexcept {
        State expected = State::detached;
        if (!state_.compare_exchange_strong(expected, State::registered,
                                            std::memory_order::acq_rel,
                                            std::memory_order::acquire)) {
            return false;  // already registered or terminal
        }
        resume_ = resume;
        home_ = q;
        return true;
    }

    // The canonical ONE-WINNER terminal resolver (§2 Design Law, §7 Unlink
    // Law). CAS state_ Registered -> {woken,cancelled,expired}. Returns true
    // ONLY when this call is the unique winner (CAS succeeded). Every losing
    // caller returns false and MUST perform no second wake/unlink.
    //
    // `expired` is a third terminal outcome reached only via the Scheduler
    // expiry seam. It is outcome-agnostic to the CAS: the same Registered
    // guard + acq_rel CAS is the single authority. A losing timer expiry (a
    // node already woken/cancelled/expired) sees the CAS fail and does nothing
    // — exactly the loser semantic (§6 truth table).
    bool resolve_(WaitOutcome outcome) noexcept {
        State target;
        if (outcome == WaitOutcome::woken) target = State::woken;
        else if (outcome == WaitOutcome::cancelled) target = State::cancelled;
        else if (outcome == WaitOutcome::expired) target = State::expired;
        else { assert(false && "resolve_ requires a terminal outcome"); return false; }
        State expected = State::registered;
        return state_.compare_exchange_strong(expected, target,
                                              std::memory_order::acq_rel,
                                              std::memory_order::acquire);
    }

    WaitResume resume_{};  // ResumeTarget token (FE-1b L2/L11); bound at register
    std::atomic<State> state_{State::detached};
    void* user_{nullptr};  // AsyncQueue / AsyncRwLock per-op context; else null
};

}  // namespace sluice::async
