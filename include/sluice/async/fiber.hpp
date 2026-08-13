// sluice::async::Fiber — minimal task/fiber state model (sluice-CORE-E1).
//
// Source-derived from Zig std.Io Uring.Fiber (Io/Uring.zig:149-248) and the E0
// ADR (docs/adr/ADR-execution-model.md §9 E4 cycle). This is the STATE model
// only — the context-switch (E2) and the scheduler (E4+) come later. A Fiber
// here is the unit of a user task: its state machine, its cancel state, and a
// slot for the context (filled by E2).
//
// The E4 single-worker state-transition proof (the load-bearing cycle):
//   running task -> submit op -> waiting -> switch to scheduler -> run another
//   task -> backend completion -> waiting task runnable -> scheduler selects ->
//   resume at the original call site.
//
// Task states (the runnable/waiting/running/terminal machine). Derived from
// Zig's queue_next/awaiting_group/free_next union + the implicit running state:
// a fiber is on the ready_queue (runnable), being executed (running), waiting
// for a completion (waiting), or finished (done). Terminal is absorbing.
//
// Layering: ABOVE Future/Group/Batch + WaitPolicy. The Evented scheduler (E4+)
// drives Fibers through this state machine. No asm yet (E2). No scheduler yet
// (the seam is here; the driver comes in E4).
#pragma once

#include <sluice/async/cancel.hpp>
#include <sluice/async/fiber_ctx.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <utility>

namespace sluice::async {

// The task lifecycle. Mirrors the E4 cycle and Zig's queue membership:
//   created -> runnable (queued) -> running (a worker executes the entry fn)
//         -> waiting (suspended on a completion/op) -> runnable (requeued on
//   completion) -> ... -> done (terminal, absorbing).
// A fiber may cycle runnable<->running<->waiting many times; `done` is final.
enum class FiberState : std::uint8_t {
    created,    // constructed, not yet made runnable
    runnable,   // on a ready queue; a scheduler may pick it up
    running,    // a worker is executing it
    waiting,    // suspended awaiting a completion/wakeup
    done,       // terminal; result published; never scheduled again
};

// Phase F1 P1-1 (issue #98, PR #105 review): frozen wait-outcome carrier.
// The winning path (reap drain or cancel_waiter) writes the outcome BEFORE
// calling make_runnable; the fiber reads it AFTER resume. This eliminates the
// race where c.ready() may return true even though cancel won the arena race
// (the I/O completed concurrently after cancel set the fiber runnable).
enum class CompletionWaitOutcome : std::uint8_t {
    pending,    // default / not yet resolved by a winner
    completed,  // reap won — Completion is ready
    canceled,   // cancel_waiter won — wait was cancelled
};

// A minimal user task: state + entry + cancel state + a context slot.
//
// The entry function is the task body. On a real scheduler (E4), a worker
// calls entry() when the fiber is runnable; entry() runs the task body, which
// may suspend (transition to waiting) at a cancel/await point and be resumed
// later. For E1 (no scheduler yet), entry() can be invoked directly to test
// the state machine; the context switch + real suspension land in E2/E4.
//
// Lifetime: owned by the scheduler/task layer (E4). Not copyable (identity
// matters — the context is address-stable). Movable only if no context yet
// (before E2 wires the context). For E1 we make it non-movable to keep the
// future context slot address-stable by construction.
class Fiber {
public:
    using Entry = std::function<void(Fiber&)>;

    Fiber() = default;
    explicit Fiber(Entry entry) : entry_(std::move(entry)) {}

    Fiber(const Fiber&) = delete;
    Fiber& operator=(const Fiber&) = delete;
    Fiber(Fiber&&) = delete;
    Fiber& operator=(Fiber&&) = delete;

    // ---- State machine ----

    FiberState state() const noexcept {
        return state_.load(std::memory_order::acquire);
    }

    // Transition to runnable. Lawful from: created, waiting. No-op from
    // runnable/running. Forbidden from done (terminal is absorbing). Called by
    // the scheduler when queueing, or by a completion handler (E4) waking a
    // waiting fiber.
    //
    // Returns true ONLY when the transition actually occurred (created->runnable
    // or waiting->runnable). Returns false if the fiber was already runnable,
    // running, or done. This is the exactly-once-publication invariant (E7-T2):
    // the caller may publish AT MOST ONE runnable ticket, and only when this
    // returns true. A false return MUST NOT be followed by an enqueue — that
    // would create a duplicate live ticket.
    bool make_runnable() noexcept;

    // Transition to running. Lawful from: runnable only. Called by the worker
    // (E4) when it picks the fiber off the ready queue.
    //
    // Returns true ONLY when the transition actually occurred (runnable->running).
    // Returns false if the fiber was NOT runnable (e.g. already running, done,
    // or still waiting). The caller MUST check this return value: a false return
    // means the ticket is invalid and the Scheduler MUST fail-fast before
    // entering the Fiber context (I47-F3: invalid runnable-ticket guard).
    bool make_running() noexcept;

    // Transition to waiting. Lawful from: running only. Called by the fiber's
    // own entry (E4) when it suspends at an await/cancel point.
    //
    // Returns true ONLY when the transition actually occurred (running->waiting).
    // Returns false if the fiber was NOT running (a contract violation). The
    // caller MUST check this return value and fail-fast on failure (I47-F2:
    // unified suspend authority protocol).
    bool make_waiting() noexcept;

    // Transition to done. Lawful from: running. Called by the entry when the
    // task body returns. Terminal and absorbing: no further transitions.
    void make_done() noexcept;

    // ---- Entry + cancel ----

    Entry& entry() noexcept { return entry_; }
    const Entry& entry() const noexcept { return entry_; }
    void set_entry(Entry e) { entry_ = std::move(e); }

    // The per-fiber cancel state (composes 027's CancelToken + a per-fiber
    // CancelState). Mirrors Zig Fiber.cancel_status + cancel_protection.
    CancelToken& cancel_token() noexcept { return token_; }
    CancelState& cancel_state() noexcept { return cstate_; }

    // ---- Execution-identity tag (E16 P0-1 / C2) ----
    //
    // An opaque tag stored IN Fiber state (not thread_local). The Application
    // Runtime task wrapper sets this to `this` (the Runtime*) around user task
    // code so that is_runtime_task() can detect lifecycle calls from inside a
    // Runtime-owned task. Unlike a thread_local guard, this tag survives Fiber
    // suspend/resume and is correct under Fiber multiplexing (one OS worker
    // runs many Fibers; a TLS guard does not follow Fiber context switches).
    //
    // Authority (C2): the tag is READ-public (narrow introspection) but the
    // WRITE path is private. Only the Scheduler may set it (via its private
    // set_current_fiber_execution_tag, in turn callable only by
    // ApplicationRuntime). This prevents ordinary application task code from
    // clearing, replacing, or forging the Runtime identity tag to bypass
    // is_runtime_task() self-close detection. Do NOT add a public setter.
    void* execution_tag() const noexcept { return execution_tag_; }

    // Phase F1 P1-1: read the frozen wait-outcome after resume. The winner
    // (drain or cancel_waiter) wrote this BEFORE make_runnable; the fiber reads
    // it AFTER context_switch returns. This replaces the racy c.ready() check.
    CompletionWaitOutcome completion_wait_outcome() const noexcept {
        return completion_wait_outcome_;
    }

private:
    friend class Scheduler;  // C2: sole write authority for execution_tag_.
    void set_execution_tag(void* tag) noexcept { execution_tag_ = tag; }

    // Phase F1 P1-1: sole write authority for the frozen wait-outcome.
    // Written by the Scheduler's winning path (drain or cancel_waiter) BEFORE
    // make_runnable; read by the fiber after resume. Never written by user code.
    void set_completion_wait_outcome(CompletionWaitOutcome o) noexcept {
        completion_wait_outcome_ = o;
    }

    std::atomic<FiberState> state_{FiberState::created};
    Entry entry_{};
    CancelToken token_{};
    CancelState cstate_{};
    void* execution_tag_{nullptr};
    CompletionWaitOutcome completion_wait_outcome_{CompletionWaitOutcome::pending};
public:
    // The fiber's saved CPU context (sp/fp/pc). Filled by fiber_ctx::init_context
    // before the first run; updated by context_switch each time the fiber
    // suspends. Public so the Scheduler (E4) can read/write it without friending.
    // Address-stable for the fiber's lifetime (Fiber is non-movable).
    fiber_ctx::Context ctx{};
private:
    // (context_storage_ removed in E4: superseded by the typed ctx above.)
};

}  // namespace sluice::async
