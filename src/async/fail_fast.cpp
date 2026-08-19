// Implementation of the Mutex acquisition fail-fast entry.
//
// Kept deliberately trivial to satisfy ASYNC-MUTEX-NOTHROW-AUTHORITY-1 §D2:
// no allocation, no locking, no I/O, no virtual / function-pointer call, no
// dynamic string, no Scheduler-state recovery. A single std::terminate() is
// the language-standard process terminator and is the most auditable shape.
#include <sluice/async/detail/fail_fast.hpp>

#include <sluice/async/fiber_ctx.hpp>

#include <atomic>
#include <exception>  // std::terminate

namespace sluice::async::detail {

[[noreturn]] void async_mutex_lock_fail_fast() noexcept {
    std::terminate();
}

// E13 P3 stage-boundary fail-fast: a due ACTIVE SelectTimerRegistration is
// unreachable in valid P3 production state. Terminates the process; never
// returns. See include/sluice/async/detail/fail_fast.hpp.
[[noreturn]] void select_timer_pump_active_fail_fast() noexcept {
    std::terminate();
}

// E13 P6 stage-boundary fail-fast: a single Event::set() broadcast observed
// arms belonging to more than one distinct eligible SelectGroup. Multi-group
// shared Event (P8) is DENIED at the P6 boundary; fail fast before any group
// winner CAS. Terminates; never returns. See include/sluice/async/detail/
// fail_fast.hpp.
[[noreturn]] void select_multi_group_event_stage_fail_fast() noexcept {
    std::terminate();
}

// E13 P5 CORRECTIVE: general-purpose Select invariant fail-fast. Called when
// the admission core receives a structurally invalid descriptor/count argument
// or encounters an unknown descriptor kind. Provides defense-in-depth against
// Release-mode memory safety violations. Terminates; never returns.
[[noreturn]] void select_invariant_fail_fast() noexcept {
    std::terminate();
}

// E14 D-E14-F2a: Group lifetime fail-fast. ~Group with pending Evented task.
[[noreturn]] void group_lifetime_fail_fast() noexcept {
    std::terminate();
}

// ADR-async-primitive-lifetime-failfast entries (see fail_fast.hpp).
[[noreturn]] void async_mutex_lifetime_fail_fast() noexcept {
    std::terminate();
}
[[noreturn]] void async_rwlock_lifetime_fail_fast() noexcept {
    std::terminate();
}
[[noreturn]] void async_condition_lifetime_fail_fast() noexcept {
    std::terminate();
}
[[noreturn]] void wait_queue_lifetime_fail_fast() noexcept {
    std::terminate();
}

// E14 D-E14-2: Evented admission fail-fast. Unsupported target.
[[noreturn]] void evented_admission_fail_fast() noexcept {
    std::terminate();
}

// E15-P1-03 / E15-P2-06: AsyncIoContext destroyed or move-assigned over while
// it still owns a backend with outstanding Completions. Per ADR §5 L11 this is
// a contract violation; the truthful deterministic behavior is fail-fast in
// BOTH Debug and Release (a destructor / move-assign has no Result channel for
// invalid_state, and silent abandonment would strand caller-owned Completions
// permanently outstanding). Mirrors group_lifetime_fail_fast.
[[noreturn]] void async_context_outstanding_fail_fast() noexcept {
    std::terminate();
}

// I47-F3: invalid runnable-ticket consumption. run_next_on consumed a ticket
// whose Fiber was NOT Runnable (make_running failed). A thief or owner resumed
// a Fiber whose CPU context was not saved. Process-fatal invariant violation.
[[noreturn]] void scheduler_invalid_runnable_ticket_fail_fast() noexcept {
    std::terminate();
}

// I47-F2: invalid suspend transition. commit_suspend_locked attempted
// make_waiting on a Fiber that was NOT Running. Impossible protocol state.
[[noreturn]] void scheduler_invalid_suspend_transition_fail_fast() noexcept {
    std::terminate();
}

[[noreturn]] void scheduler_missing_fiber_owner_fail_fast() noexcept {
    std::terminate();
}

// Phase F1: wait-registry invariant violation (issue #98). A record state
// transition outside free -> registered -> {delivered | cancelled} -> free,
// or an out-of-range record index from a lease pin. Impossible protocol state.
[[noreturn]] void scheduler_wait_registry_invariant_fail_fast() noexcept {
    std::terminate();
}

// Phase F1: Scheduler destroyed with a non-quiescent wait registry — a
// registered Completion waiter was neither delivered nor cancelled.
[[noreturn]] void scheduler_wait_registry_nonempty_fail_fast() noexcept {
    std::terminate();
}

// Completion publication authority fail-fast. A Completion state transition
// violated the authority model (reset on outstanding/publishing, destroy
// outstanding/publishing, losing publish CAS, rollback on a non-outstanding
// Completion). Process-fatal in BOTH Debug and Release.
[[noreturn]] void completion_authority_fail_fast() noexcept {
    std::terminate();
}

// Phase B (ADR-explicit-io-request-contract, Accepted, Decision 5 / I15): the
// private `binding` transient is an exclusive publication window — only the
// backend that won the idle -> binding CAS may install the RequestKey/context/
// release-capability payload. Destroying or resetting a Completion while it is
// in `binding` observes a half-installed payload, so both are contract
// violations detected in BOTH Debug and Release. Distinct entries (rather than
// reusing completion_authority_fail_fast) keep the failure site attributable to
// the binding transient specifically.
[[noreturn]] void completion_binding_destruction_fail_fast() noexcept {
    std::terminate();
}
[[noreturn]] void completion_binding_reset_fail_fast() noexcept {
    std::terminate();
}

// Phase B (ADR Decision 15 / AC-13 :566-572): slot release via reset/destroy is
// allocation-free and acquires the leaf slot-lifecycle domain after reap has
// left it. Releasing a slot whose enqueue-in-flight pin is still live, whose
// waiter registration is still open, or that still holds a stored waiter
// token/routing-lease is a contract violation — none of those may be silently
// discarded to make teardown pass. Detected in BOTH Debug and Release.
[[noreturn]] void request_slot_release_invariant_fail_fast() noexcept {
    std::terminate();
}

// Phase B (design §9 / ADR Decision 4 :341-349): enqueue from any slot state
// other than pending (-> enqueued) or backend_ready (-> successful no-op) is an
// invariant violation of the Scheme-B arbitration — enqueue before commit,
// double enqueue, or enqueue after reap would silently strand or double-link an
// accepted op. Detected in BOTH Debug and Release.
[[noreturn]] void request_arena_enqueue_state_fail_fast() noexcept {
    std::terminate();
}

// Phase B (ADR Decision 15 / AC-13): arena destruction with slot_in_use != 0 is
// a contract violation — quiescent destruction requires every slot free, and
// the Completion-bound release capability must never dangle after the arena is
// gone. Detected in BOTH Debug and Release (fires from the arena destructor
// during backend/context destruction).
[[noreturn]] void request_arena_destruction_fail_fast() noexcept {
    std::terminate();
}

// Phase B (review C2 / I4 / I5 / I11): reap reached a backend_ready slot whose
// Completion publication binding was never installed before commit. Silently
// skipping would lose an accepted request (AC-4) and strand the Completion
// outstanding forever. Detected in BOTH Debug and Release.
[[noreturn]] void request_arena_missing_binding_fail_fast() noexcept {
    std::terminate();
}

// Phase B (review I2): record_terminal on a slot that is not a legal terminal
// candidate (reserved/prepared = not yet accepted) would strand the op forever
// (the terminal would be stored but the op could never reach backend_ready).
// Detected in BOTH Debug and Release.
[[noreturn]] void request_arena_terminal_state_fail_fast() noexcept {
    std::terminate();
}

// Phase B (review finding #4): enqueue on a stale handle is a reuse-before-ack
// invariant violation (I19), not a successful Scheme-B no-op. Masking it as
// terminal_noop hid the pin/release authority breach. Detected in BOTH Debug
// and Release.
[[noreturn]] void request_arena_enqueue_stale_fail_fast() noexcept {
    std::terminate();
}

// Phase B (review finding #5): a slot whose 64-bit generation reached UINT64_MAX
// cannot increment without wrapping (which would re-introduce ABA, violating I6's
// absolute wording). Fail fast on generation exhaustion instead of silently
// wrapping. Detected in BOTH Debug and Release; practically unreachable
// (~585 years at 1 release/ns).
[[noreturn]] void request_arena_generation_exhausted_fail_fast() noexcept {
    std::terminate();
}

// Phase B (CodeRabbit finding): an out-of-range SlotIndex passed to an
// introspection accessor (key_of/generation_of/state_of/...) would index past
// the fixed slot array. Fail fast in BOTH Debug and Release.
[[noreturn]] void request_arena_slot_index_out_of_range_fail_fast() noexcept {
    std::terminate();
}

// Phase B (review round-4 finding 2): record_terminal was given an unstored
// (default-constructed) TerminalResult. Recording it would publish a phantom
// 0-byte success and risk a double ready-ring push. Fail-fast in BOTH Debug
// and Release.
[[noreturn]] void request_arena_invalid_terminal_fail_fast() noexcept {
    std::terminate();
}

// Phase B (review round-4): the dispatch path reached a slot that is neither
// enqueued nor backend_ready. Invariant violation of the unified state
// machine. Detected in BOTH Debug and Release.
[[noreturn]] void request_arena_dispatch_state_fail_fast() noexcept {
    std::terminate();
}

// Phase B (review round-4): the dispatch path (mark_running) was given a stale
// dispatch identity (stale generation / free / out-of-range / wrong-domain
// handle). Not the legitimate backend_ready backoff — a lifecycle invariant
// violation. Detected in BOTH Debug and Release.
[[noreturn]] void request_arena_dispatch_stale_fail_fast() noexcept {
    std::terminate();
}

// Phase B (review round-4 finding 2; round-5 tail hardening): the ready-ring
// push invariants were violated (slot not backend_ready / no stored terminal /
// already linked, including as the current tail / structurally inconsistent
// head-tail-count triple / ring at capacity). Fail-fast in BOTH Debug and
// Release rather than corrupting the ring silently.
[[noreturn]] void request_arena_ready_ring_invariant_fail_fast() noexcept {
    std::terminate();
}

// Phase E (ThreadPoolBackend): non-quiescent destruction. The caller destroyed
// the backend while accepted work remains (active workers, enqueued ops, backend-
// ready slots, or bound slots). Fail-fast in BOTH Debug and Release.
[[noreturn]] void threadpool_non_quiescent_destruction_fail_fast() noexcept {
    std::terminate();
}

// Phase D1 (UringAsyncBackend): non-quiescent destruction. The caller destroyed
// the backend while accepted work remains (enqueued dispatch entry, live op
// cookie / ring-owned request, backend-ready unreaped terminal, accepted-
// outstanding request, or bound slot). Preflighted BEFORE io_uring_queue_exit().
// Fail-fast in BOTH Debug and Release.
[[noreturn]] void uring_non_quiescent_destruction_fail_fast() noexcept {
    std::terminate();
}

// E14 D-E14-2: Evented admission check. Returns the effective fiber support
// status. Production: fiber_ctx::supported (compile-time constant). Internal-
// testing: may be overridden to simulate unsupported targets on x86_64.
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
namespace {
// -1 = no override (use fiber_ctx::supported); 0 = force unsupported; 1 = force supported.
std::atomic<int> g_evented_admission_override{-1};
}  // namespace

bool evented_admission_check() noexcept {
    int ovr = g_evented_admission_override.load(std::memory_order_acquire);
    if (ovr >= 0) return ovr != 0;
    return fiber_ctx::supported;
}

// AsyncTestAccess entry points (declared in scheduler.hpp under the macro).
// Defined here because the override state lives in this TU.
void set_evented_admission_override_impl(bool supported) noexcept {
    g_evented_admission_override.store(supported ? 1 : 0, std::memory_order_release);
}
void clear_evented_admission_override_impl() noexcept {
    g_evented_admission_override.store(-1, std::memory_order_release);
}
bool get_evented_admission_override_impl() noexcept {
    int ovr = g_evented_admission_override.load(std::memory_order_acquire);
    return ovr >= 0 ? (ovr != 0) : fiber_ctx::supported;
}
#else
bool evented_admission_check() noexcept {
    return fiber_ctx::supported;
}
#endif

}  // namespace sluice::async::detail
