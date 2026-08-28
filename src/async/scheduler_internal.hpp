// scheduler_internal.hpp — non-installed internals shared by the Scheduler
// implementation TUs (split from scheduler.cpp; see
// docs/post-freeze/structural-audit.md §6).
//
// NOT public API: never install, never include from an installed header.
// Holds the two entities that must be ONE program-wide object across the
// scheduler_*.cpp TUs:
//   * SchedulerWakeHandle::Control — the pimpl control block, complete here so
//     both the constructing/destructing TU (scheduler.cpp) and the
//     notify/bound TU (scheduler_park_wake.cpp) see the same definition;
//   * g_worker — the per-thread WorkerState* TLS.
#ifndef SLUICE_ASYNC_SCHEDULER_INTERNAL_HPP
#define SLUICE_ASYNC_SCHEDULER_INTERNAL_HPP

#include <sluice/async/scheduler.hpp>

#include <condition_variable>
#include <mutex>

namespace sluice::async {

// TLS: the current Worker's WorkerState. Set by worker_loop before any Fiber
// runs; used by await_completion_*/await_ready_flag to find the current
// Fiber and scheduler context. This is genuine Worker-local state (one Worker
// per OS thread) — NOT a process-global slot shared across Workers.
// inline thread_local: ONE per-thread entity shared by every Scheduler
// implementation TU.
inline thread_local WorkerState* g_worker = nullptr;

// Per-operation context stored on WaitNode::user_ for RwLock waiters.
// Stack-local for the Fiber frontend (alive for the entire suspension epoch
// because the fiber stack persists); COROUTINE-FRAME-EMBEDDED for the
// deferred frontend (FE-1a lifetime rule — the grant winner's ownership
// commit reads ctx->actor after suspension, so the address must be stable).
// Shared here (non-installed) so scheduler_rwlock.cpp and the internal-testing
// seam TU read ONE type (no duplicated wait-node context).
//
// `actor` is the waiter's ActorIdentity (FE-1b A1): committed into
// writer_owner by the writer-grant authority. The Fiber entry binds
// ActorId::fiber(me); a frontend binds its own stable token. Read mode
// ignores it (v1 has no per-reader identity).
struct RwWaitCtx {
    enum class Mode : std::uint8_t { read, write };
    Mode mode;
    ActorId actor;
};

// ---- SchedulerWakeHandle::Control (full definition; forward-declared in
// the header so the shared_ptr member is pimpl-friendly) ----
struct SchedulerWakeHandle::Control {
    // Control::mtx is the CALLBACK LEASE.
    //
    // notify() holds this mutex from the validity check through the ENTIRE
    // Scheduler wake callback (notify_external_wake -> signal_wake_locked).
    // ~Scheduler acquires the SAME mutex before invalidating the control
    // block. Therefore destruction cannot invalidate or destroy Scheduler
    // wake members while a notify callback holding the lease is in flight:
    //   - Notify wins:  N holds the lease through the callback; D's mutex
    //                   acquisition BLOCKS until the callback returns.
    //   - D wins:       D invalidates + releases; N then observes dead/null
    //                   and returns false without any Scheduler dereference.
    //
    // This is a mutex-serialized callback lease, NOT shared ownership and
    // NOT reference counting. Control::mtx does not extend Scheduler object
    // ownership. A stale handle may survive Scheduler destruction; its later
    // notify() observes alive=false and returns false (a safe no-op). See
    // spec/tla/e9_wake_handle_lifetime/ for the TLA+ proof.
    Mutex mtx;
    Scheduler* scheduler SLUICE_GUARDED_BY(mtx){nullptr};
    bool alive SLUICE_GUARDED_BY(mtx){false};

    // Deterministic lifetime test seam (spec 13). TEST-ONLY.
    // When armed, notify() pauses at the exact causal boundary - AFTER it
    // has validated alive under Control::mtx and BEFORE notify_external_wake
    // - while STILL HOLDING the lease. This forces the notifier-wins
    // interleaving deterministically: the destructor cannot complete
    // invalidation while the notifier is paused. It does NOT alter
    // production Scheduler state; it only blocks the notifier thread.
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
    bool lifetime_seam_armed{false};
    bool lifetime_seam_paused{false};
    std::mutex lifetime_seam_mtx;
    std::condition_variable lifetime_seam_cv;
#endif
};

}  // namespace sluice::async

#endif
