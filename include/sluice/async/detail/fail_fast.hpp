// sluice::async::detail::async_mutex_lock_fail_fast
//
// Named fail-fast entry for the Mutex acquisition boundary
// (ASYNC-MUTEX-NOTHROW-AUTHORITY-1 §2/§3).
//
// An internal Mutex acquisition that participates in an authoritative
// Scheduler transition (or, by the Queue design, a winner CommitGap) cannot
// resume user execution after an underlying lock failure while preserving
// winner / ownership / queue-membership / publication invariants. Such a
// failure is therefore process-fatal.
//
// Contract (authority §D2):
//   * [[noreturn]] noexcept;
//   * no allocation, no locking, no I/O;
//   * no virtual dispatch, no function-pointer call, no dynamic string;
//   * does not attempt to recover Scheduler state;
//   * ultimately calls std::terminate() (or an equivalent process terminator).
//
// The winner path must not format or emit complex log output.
//
// This function takes no operation parameter: the operation is known only to
// the (internal-testing-only) failure-injection seam, never to the production
// fail-fast path. Adding a parameter here would invite future formatting /
// logging / allocation on the winner path and is deliberately rejected.
#pragma once

namespace sluice::async::detail {

// Terminates the process. Called only from the Mutex::lock()/try_lock()
// catch (...) boundaries; never returns.
[[noreturn]] void async_mutex_lock_fail_fast() noexcept;

// E13 P3 stage-boundary fail-fast (docs/e13-select-timer-adapter.md §5,
// Mandatory Addendum D). A due ACTIVE SelectTimerRegistration is UNREACHABLE
// in valid P3 production state: there is no admission path, so no ACTIVE
// Select heap entry should ever be observed by the pump. If the pump pops an
// ACTIVE Select entry, that is an invariant violation (either a stale entry
// was observed before a CAS completed, the registration protocol has a bug,
// or a test advanced the clock past an ACTIVE synthetic entry). The pump
// MUST NOT claim a winner, mark CandidateReady, retire/consume, erase, or
// busy-loop; it fails fast instead. This is NOT supported production Select
// behavior — P4 (claim/finalize) is denied pending independent P3 review.
//
// Same contract as async_mutex_lock_fail_fast: [[noreturn]] noexcept, no
// allocation / locking / I/O / dynamic string, no state recovery, ultimately
// std::terminate(). Takes no parameter (the operation is known only to the
// caller; adding one would invite logging on the pump hot path).
[[noreturn]] void select_timer_pump_active_fail_fast() noexcept;

// E13 P6 stage-boundary fail-fast: multi-group shared Event (P8, DENIED).
// docs/e13-select-locking-and-publication.md §6 / production-test-plan.md §7.8.
// One Event::set() broadcast may observe arms belonging to MORE THAN ONE
// distinct eligible SelectGroup (phase==Armed). P8 (multi-group Event
// intrusive worklist + per-group iteration) is not implemented at the P6
// boundary. P6 must therefore detect >1 distinct eligible group BEFORE any
// group winner CAS / candidate mutation / authority close and fail fast,
// rather than silently resolving only one group (a lost resolution) or
// attempting an unsupported multi-group publish. The same Event appearing
// twice in ONE group is NOT this case and is supported (P6-D1).
//
// Same contract as the other fail-fast entries: [[noreturn]] noexcept, no
// allocation / locking / I/O / dynamic string, no state recovery, ultimately
// std::terminate(). No parameter (the operation is known only to the caller).
[[noreturn]] void select_multi_group_event_stage_fail_fast() noexcept;

// E13 P5 CORRECTIVE: general-purpose Select invariant fail-fast. Called when
// the admission core receives a structurally invalid descriptor/count argument
// (descs==nullptr, count==0, count>kSelectMaxArms) or encounters an unknown
// descriptor kind in Release builds. Provides defense-in-depth against
// Release-mode memory safety violations even when a non-friend caller bypasses
// the public select() template's compile-time requires clause gate.
//
// Same contract as the other fail-fast entries.
[[noreturn]] void select_invariant_fail_fast() noexcept;

// E14 D-E14-F2a: Group lifetime fail-fast. Called from ~Group when an Evented
// task Future is still pending at destruction time. This is a caller-contract
// violation (the caller must await or cancel before destroying an Evented
// Group). The destructor MUST NOT call Evented Future::await from a non-Fiber
// context (g_worker is null on an ordinary caller thread, causing a null
// dereference in Scheduler::await_ready_flag). Failing fast surfaces the
// violation deterministically instead of allowing UB.
//
// Same contract as the other fail-fast entries: [[noreturn]] noexcept, no
// allocation / locking / I/O / dynamic string, no state recovery, ultimately
// std::terminate().
[[noreturn]] void group_lifetime_fail_fast() noexcept;

// E14 D-E14-2: Evented admission fail-fast. Called when an Evented public
// admission boundary is reached on a target where fiber_ctx::supported is
// false. Deterministically testable via the bool parameter (production passes
// fiber_ctx::supported; death tests pass false).
//
// Same contract as the other fail-fast entries.
[[noreturn]] void evented_admission_fail_fast() noexcept;

// E15-P1-03 / E15-P2-06: AsyncIoContext outstanding-Completion fail-fast.
// Called from AsyncIoContext::~AsyncIoContext() and operator=(AsyncIoContext&&)
// when the context still owns a backend with >0 outstanding Completions. Per
// ADR §5 L11 this is a caller-contract violation: outstanding Completions are
// address-stable and CALLER-OWNED; silently discarding the backend that
// publishes them would strand them permanently outstanding (no Result channel,
// no path to ready). A destructor / move-assignment has no Result channel to
// surface invalid_state, so the truthful contract is deterministic fail-fast
// in BOTH Debug and Release (no silent abandonment, no claimed-but-unreturnable
// invalid_state). Mirrors group_lifetime_fail_fast.
//
// Same contract as the other fail-fast entries: [[noreturn]] noexcept, no
// allocation / locking / I/O / dynamic string, no state recovery, ultimately
// std::terminate(). No parameter (the operation is known only to the caller).
[[noreturn]] void async_context_outstanding_fail_fast() noexcept;

// E14 D-E14-2: internal testable guard. Production code calls
// require_evented_supported(fiber_ctx::supported). On supported targets this
// is an optimized no-op (the parameter is a compile-time true constant). On
// unsupported targets or when called with false (death test), it calls
// evented_admission_fail_fast().
inline void require_evented_supported(bool supported) noexcept {
    if (!supported) {
        evented_admission_fail_fast();
    }
}

// E14 D-E14-2: Evented admission check. Returns the effective fiber support
// status. Production: returns fiber_ctx::supported (compile-time constant on
// the target). Internal-testing: may be overridden via
// AsyncTestAccess::set_evented_admission_override to simulate unsupported
// targets on x86_64. Defined out-of-line in fail_fast.cpp.
bool evented_admission_check() noexcept;

// I47-F3: invalid runnable-ticket consumption fail-fast. Called from
// run_next_on when make_running() fails (the Fiber is NOT in Runnable state).
// A worker that consumes a ticket whose Fiber is not Runnable would enter an
// invalid context (rsp/rbp/rip not saved). This is process-fatal: the
// invariant violation means the suspend-switch authority protocol was breached.
//
// Same contract as the other fail-fast entries: [[noreturn]] noexcept, no
// allocation / locking / I/O / dynamic string, no state recovery, ultimately
// std::terminate(). No parameter (the operation is known only to the caller).
[[noreturn]] void scheduler_invalid_runnable_ticket_fail_fast() noexcept;

// I47-F2: invalid suspend transition fail-fast. Called from
// commit_suspend_locked when make_waiting() fails (the Fiber is NOT Running).
// A Fiber that cannot transition Running->Waiting is in an impossible protocol
// state: the caller believed it was the current Running Fiber, but the state
// machine disagrees. Process-fatal.
//
// Same contract as the other fail-fast entries.
[[noreturn]] void scheduler_invalid_suspend_transition_fail_fast() noexcept;

// I47-F1: missing Fiber owner fail-fast. Called from owner_for_fiber_locked
// when a previously-running Fiber has no recorded owner in fiber_owner_. A
// Fiber that has run and entered Waiting MUST have an owner entry (set at
// spawn/spawn_on/steal). A missing entry is a fatal Scheduler invariant
// violation: routing would fall back to an arbitrary Worker, breaking the
// suspend-switch authority guarantee.
//
// Same contract as the other fail-fast entries.
[[noreturn]] void scheduler_missing_fiber_owner_fail_fast() noexcept;

// Phase F1: wait-registry invariant fail-fast (issue #98). Called when the
// Scheduler wait registry's record state machine is violated (a retire on a
// non-registered record, an out-of-range record index from a lease pin, ...).
// The registry is a leaf domain whose state transitions are exactly
// free -> registered -> {delivered | cancelled} -> free; any other transition
// is an impossible protocol state. Process-fatal in Debug AND Release.
//
// Same contract as the other fail-fast entries.
[[noreturn]] void scheduler_wait_registry_invariant_fail_fast() noexcept;

// Phase F1: non-quiescent wait-registry fail-fast. Called from ~Scheduler
// when wait_record_live_count_ != 0 — a registered Completion waiter was
// neither delivered (drained) nor cancelled, i.e. a suspended Fiber's wake
// obligation was abandoned before the Scheduler was destroyed. Process-fatal
// in Debug AND Release (mirrors select_invariant_fail_fast).
//
// Same contract as the other fail-fast entries.
[[noreturn]] void scheduler_wait_registry_nonempty_fail_fast() noexcept;

// Completion publication authority fail-fast. Called when a Completion state
// transition violates the authority model (ADR-explicit-io-completion-authority):
//   - reset() on an outstanding or publishing Completion
//   - destruction of an outstanding or publishing Completion
//   - publish_from_reap() when the CAS outstanding → publishing loses
//     (double-publish or publish from idle/ready)
//   - rollback_claim_before_accept() on a Completion that is not outstanding
// Note: reset() from idle is a deliberate IDEMPOTENT NO-OP (AC-13 as amended),
// NOT a fail-fast. These are contract violations that MUST be detected in BOTH
// Debug and Release.
//
// Same contract as the other fail-fast entries.
[[noreturn]] void completion_authority_fail_fast() noexcept;

// Phase B (ADR-explicit-io-request-contract, Accepted, Decision 5): the Completion
// claim path gains a private `binding` transient between `idle` and `outstanding`.
// Only the backend that wins the idle → binding CAS may install the binding
// payload (RequestKey, ContextIdentity, slot-release capability); a second
// submitting context, a cancel path, a waiter-registration path, reset, and the
// destructor MUST NOT observe or act on a half-installed binding (I15). These
// entries fire on the binding-state violations:
//   - completion_binding_destruction_fail_fast: ~Completion while state==binding
//   - completion_binding_reset_fail_fast:       reset() while state==binding
// They are distinct from completion_authority_fail_fast so the failure site is
// attributable to the binding transient specifically. Detected in BOTH Debug and
// Release.
//
// Same contract as the other fail-fast entries.
[[noreturn]] void completion_binding_destruction_fail_fast() noexcept;
[[noreturn]] void completion_binding_reset_fail_fast() noexcept;

// Phase B (ADR Decision 15 / AC-13 :566-572): slot release (via Completion reset
// or ready-Completion destruction) is allocation-free and acquires the leaf
// slot-lifecycle domain after reap has left it. Release is a contract violation
// (fail-fast in BOTH Debug and Release) when the slot is not in a releasable
// state — specifically when the enqueue-in-flight pin is still live, or the
// waiter registration is still open, or a stored waiter token/routing-lease has
// not been consumed. None of those may be silently discarded to make teardown
// pass. (Commit 2 declares the entry; commit 3 wires it into the release path
// once the pin and waiter-registration fields exist on the slot.)
//
// Same contract as the other fail-fast entries.
[[noreturn]] void request_slot_release_invariant_fail_fast() noexcept;

// Phase B (design §9 pending -> enqueued failure row): enqueue has exactly two
// legal outcomes (pending -> enqueued, or observing backend_ready -> successful
// no-op). Entering enqueue from any other slot state (reserved/prepared =
// enqueue before commit, enqueued/running = double enqueue, completion_ready =
// enqueue after reap) is an invariant violation of the Scheme-B arbitration and
// fails fast in BOTH Debug and Release rather than silently stranding the op.
[[noreturn]] void request_arena_enqueue_state_fail_fast() noexcept;

// Phase B (ADR Decision 15 / AC-13): quiescent arena destruction requires every
// slot free (slot_in_use == 0). Destroying the arena — via backend/context
// destruction — while slots are still bound (e.g. the caller holds ready
// Completions it never reset) is a contract violation in BOTH Debug and Release:
// no implicit drain or abandonment, and the Completion-bound release capability
// must never dangle.
[[noreturn]] void request_arena_destruction_fail_fast() noexcept;

// Phase B (review C2 / I4 / I5 / I11): reap reached a backend_ready slot whose
// Completion publication binding was never installed. The binding is installed
// before commit; a missing binding means the accepted op cannot be published —
// silently skipping it would lose an accepted request (AC-4) and strand the
// Completion outstanding forever. Invariant violation: fail-fast in BOTH Debug
// and Release instead of a silent drop.
[[noreturn]] void request_arena_missing_binding_fail_fast() noexcept;

// Phase B (review I2): record_terminal/cancel reached a slot that is not a
// legal terminal candidate (reserved/prepared = not yet accepted; free =
// never reserved). Storing a terminal before acceptance would strand the op
// forever (dispatch's later record_terminal would see the terminal already
// stored and the op could never reach backend_ready). Invariant violation:
// fail-fast in BOTH Debug and Release.
[[noreturn]] void request_arena_terminal_state_fail_fast() noexcept;

// Phase B (review finding #4 — stale enqueue masked as a successful no-op).
// enqueue() has exactly two LEGAL outcomes (ADR Decision 5 / I17): it wins and
// publishes pending linkage, or it observes backend_ready from a LEGITIMATE
// prior terminal winner and completes as a successful no-op. A STALE handle
// (generation mismatch) is neither: it means the committed submit path's slot
// moved on (released/reused) while its identity-bound enqueue pin was still
// live — an I19 reuse-before-acknowledgement disaster, not a normal race.
// Treating stale as terminal_noop (the prior behavior) silently masqueraded as
// a Scheme-B success and masked the pin/reuse violation. It now fails fast in
// BOTH Debug and Release.
[[noreturn]] void request_arena_enqueue_stale_fail_fast() noexcept;

// Phase B (review finding #5 — generation wrap re-introduces ABA). A 64-bit
// generation makes ABA practically impossible, but a silent wrap at UINT64_MAX
// would still violate I6's absolute wording ("a stale key can never mutate the
// new occupant"). The arena instead fail-fasts on generation EXHAUSTION when a
// slot's generation reaches UINT64_MAX on release (~585 years at 1 release/ns
// is unreachable in practice), so the ABA guard can never actually wrap.
// Detected in BOTH Debug and Release.
[[noreturn]] void request_arena_generation_exhausted_fail_fast() noexcept;

// Phase B (CodeRabbit finding): the read-only introspection accessors
// (key_of/generation_of/state_of/...) index the fixed slot array without the
// bounds check validate_ applies to handle-taking methods. An out-of-range
// SlotIndex would be an out-of-bounds read. They are a deliberate test surface
// AND a backend dispatch path, so a stale/checked index is an invariant
// violation, not a recoverable error — fail-fast in BOTH Debug and Release.
[[noreturn]] void request_arena_slot_index_out_of_range_fail_fast() noexcept;

// Phase B (review round-4 finding 2): record_terminal was given a default-
// constructed TerminalResult (stored == false). Recording it would publish a
// phantom 0-byte success (terminal_to_size treats an unstored result as
// success) and would leave cancel() unable to recognize the existing terminal
// (it keys the already-terminal check on stored), risking a second ready-ring
// push of the same slot. An unstored terminal is a caller bug — the production
// callers always pass a stored result via ok_bytes/ok_void/err. Invariant
// violation: fail-fast in BOTH Debug and Release.
[[noreturn]] void request_arena_invalid_terminal_fail_fast() noexcept;

// Phase B (review round-4): the dispatch path (mark_running, enqueued ->
// running) reached a slot that is neither enqueued nor backend_ready. Entering
// dispatch from free/reserved/prepared/pending (dispatch before enqueue),
// running (double dispatch), or completion_ready (dispatch after reap) is an
// invariant violation of the unified state machine (design §9: "only
// unknown/illegal state is an invariant violation (fail-fast)"). The Phase B
// reference backends never call mark_running; it makes the shared arena correct
// for the later ThreadPool/Uring migration. Detected in BOTH Debug and Release.
[[noreturn]] void request_arena_dispatch_state_fail_fast() noexcept;

// Phase B (review round-4): the dispatch path (mark_running) was given a STALE
// dispatch identity — validate_ rejected the handle (stale generation, free
// slot, out-of-range index, or wrong context provenance). `mark_running`
// returning false is RESERVED for the legitimate dispatch backoff: a
// current-generation slot already backend_ready because a terminal winner won
// before dispatch. A stale handle is none of the legal outcomes — the backend
// holds a dispatch identity whose slot moved on (released/reused) — a
// lifecycle invariant violation, not a normal cancel/dispatch race. Fail-fast
// in BOTH Debug and Release rather than masquerading as a backoff.
[[noreturn]] void request_arena_dispatch_stale_fail_fast() noexcept;

// Phase B (review round-4 finding 2; round-5 tail hardening): the ready-ring
// push invariants were violated — a push landed on a slot that is not
// backend_ready, has no stored terminal, is already linked onto the ring
// (ready_next_ != kNotOnReadyRing for a non-tail node, or the slot IS the
// current tail, whose ready_next_ legitimately carries the kNotOnReadyRing
// terminator), onto a ring whose head/tail/count triple is structurally
// inconsistent, or onto a ring already at capacity (which would exceed the
// one-entry-per-in-flight-op guarantee). Any of these would corrupt the ring
// or let reap publish a phantom. The only push sites (record_terminal /
// pending-or-enqueued cancel) guarantee the preconditions; reaching this entry
// is an invariant violation. Fail-fast in BOTH Debug and Release.
[[noreturn]] void request_arena_ready_ring_invariant_fail_fast() noexcept;

// Phase E (ThreadPoolBackend): non-quiescent destruction. The backend's
// destructor is called while accepted work remains (active workers, enqueued ops,
// backend-ready slots, or bound slots). Quiescent teardown requires the caller
// to close admission, drain all outstanding Completions, and reset them so that
// slot_in_use == 0 and accepted_outstanding == 0. Silent implicit drain/cancel
// would violate the explicit lifecycle and could strand caller-owned Completions.
// Fail-fast in BOTH Debug and Release.
[[noreturn]] void threadpool_non_quiescent_destruction_fail_fast() noexcept;

// ADR-async-primitive-lifetime-failfast: async synchronization primitive
// destruction fail-fast. Destroying AsyncMutex (while owned), AsyncRwLock
// (with active readers or writer), AsyncCondition (while a wait() is in
// flight), or WaitQueue (with registered waiters) is a caller contract
// violation with no recovery semantics: registered waiters and Scheduler
// routing records would refer to freed memory. Silent continuation is
// silent use-after-free, not graceful degradation, and a destructor has no
// Result channel — deterministic named termination is the truthful option.
// Debug keeps the descriptive assert ahead of these entries; Release
// enforcement is these entries. Quiescent destruction is unchanged and
// side-effect-free (no cancel-all, no wake-all, no force-release, no
// synthesized results — AGENTS.md §14).
//
// Same contract as the other fail-fast entries: [[noreturn]] noexcept, no
// allocation / locking / I/O / dynamic string, no state recovery,
// ultimately std::terminate(). No parameter (naming the authority is the
// function name's job).
[[noreturn]] void async_mutex_lifetime_fail_fast() noexcept;
[[noreturn]] void async_rwlock_lifetime_fail_fast() noexcept;
[[noreturn]] void async_condition_lifetime_fail_fast() noexcept;
[[noreturn]] void wait_queue_lifetime_fail_fast() noexcept;

// Phase D1 (UringAsyncBackend): non-quiescent destruction. The backend's
// destructor is called while accepted work remains (an enqueued local dispatch
// entry, a live operation cookie / ring-owned request, a backend-ready unreaped
// terminal, an accepted-outstanding request, or a bound slot). Quiescent
// teardown requires the caller to reap accepted requests and reset/destroy the
// ready Completions so that slot_in_use == 0 and accepted_outstanding == 0.
// Silent implicit cancel/drain/wait/reap would violate the explicit lifecycle
// (AGENTS.md §14) and could strand caller-owned Completions or race a live
// kernel request. The destructor preflights this BEFORE io_uring_queue_exit()
// so a contract violation is reported as such, not masked by ring teardown.
// Fail-fast in BOTH Debug and Release.
[[noreturn]] void uring_non_quiescent_destruction_fail_fast() noexcept;

}  // namespace sluice::async::detail
