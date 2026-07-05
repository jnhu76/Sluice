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
    void make_runnable() noexcept;

    // Transition to running. Lawful from: runnable only. Called by the worker
    // (E4) when it picks the fiber off the ready queue.
    void make_running() noexcept;

    // Transition to waiting. Lawful from: running only. Called by the fiber's
    // own entry (E4) when it suspends at an await/cancel point.
    void make_waiting() noexcept;

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

private:
    std::atomic<FiberState> state_{FiberState::created};
    Entry entry_{};
    CancelToken token_{};
    CancelState cstate_{};
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
