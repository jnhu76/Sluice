# S0B — Contract ↔ Correctness Authority ↔ C++ Reality Freeze

**Status:** LIVE — the frozen Safety-correctness-kernel map for #289 Phase S0B
(execution issue #292).

**Baseline:** `master` = `9c13b597` (PR #291 merge, S0A complete). All code
anchors below were read at this SHA. Code anchors are FILE + SYMBOL
(grep-stable); line numbers are avoided except where noted.

**What this document is:** the one-place answer, per Boundary/Safety property
B01–B10, to: what behavior is promised, which internal authority owns it,
where the C++ implements it, what prevents a second authority, which test
independently witnesses it, what formal evidence exists, and what remains
unproved. It replaces the need to reconstruct the truth from historical phase
documents.

**What this document is NOT:** new semantics. Every row cites its governing
authority; where documentation and code disagreed, the pre-fix record and the
adjudication are recorded inline (Case 1–4 per the S0B procedure).

**Method:** Step A extracted each contract from the authority chain (AGENTS.md,
ADR-explicit-io-request-contract + amendments, ADR-explicit-io-completion-
authority, ADR-public-request-handle, ADR-cancel-request-epoch,
architecture-constitution AC-2..AC-15, public headers, `docs/reference/api.md`,
`docs/architecture/async-request-lifecycle.md`). Step B traced every public
entry point to its storage owner in the actual C++. Step C matched existing
regressions/negative controls. Step D classified before any fix. Step E
adjudicated. The full authority/trace evidence per row is below.

## Verdict summary

| Row | Property | Drift | Resolution |
| --- | --- | --- | --- |
| B01 | Request identity / generation / stale-handle exclusion | ALIGNED | none |
| B02 | Admission / submit transaction / rollback | ALIGNED | none |
| B03 | Terminal winner (exactly-once) | ALIGNED | none |
| B04 | Completion publication (reap-only) | ALIGNED | none |
| B05 | Borrowed buffer / storage lifetime | ALIGNED | none |
| B06 | Cancellation vs ordinary completion | ALIGNED | none |
| B07 | Deadline / timeout semantics | ALIGNED | none |
| B08 | Wait / wake / lost-wakeup closure | ALIGNED | none |
| B09 | Resource accounting / retirement | ALIGNED | none |
| B10 | Shutdown / quiescence | ALIGNED | none |
| D1 | api.md reset()-slot-release scope | DOC_TOO_WEAK | docs fixed (this PR) |
| D2 | api.md error-vocabulary emitter scope | DOC_TOO_WEAK | docs fixed (this PR); the register-waiter half was re-adjudicated as W1 below |
| D3 | api.md AsyncIoContext waiter/sink surface | DOC_TOO_WEAK | docs fixed (this PR) |
| W1 | Waiter error vocabulary (register vs cancel split) | IMPLEMENTATION_DRIFT | fixed in S0B-CORRECTIVE-1 (this PR): minimal shared-layer C++ change + regressions |

B01–B10 were ALIGNED as originally frozen (no production change was required
for any B-row's audited property). S0B-CORRECTIVE-1 (adversarial review of
this matrix) then surfaced W1 — a genuine production IMPLEMENTATION_DRIFT in
the waiter error vocabulary that the original pass had mis-classified as the
doc-only finding D2. W1 is adjudicated and corrected in this PR (Case 1:
authority upheld, code corrected, regressions added); D1–D3 remain doc-only
Case-2 fixes. Full W1 record below.

---

## B01 — Request identity / generation / stale-handle exclusion

**Protected property (falsifiable).** For any accepted request with key
`K = (context, slot, generation)`: after the slot is released and reused with
generation `g+1`, any operation carrying the stale `g` — cancel, reap,
dispatch, waiter registration, release, identity query — either returns
`not_found`/fails to act on the new occupant or is an invariant fail-fast; it
can never mutate, terminalize, or observe the new occupant. Generation
increments BEFORE the freed slot is visible to a new reserve, and the counter
never wraps.

**Public/observable contract.** ADR-explicit-io-request-contract Decision 1
(I1/I6); AC-2/AC-14; `docs/reference/api.md` RequestHandle section (stale ⇒
`not_found`; cross-context ⇒ `not_found`); ADR-public-request-handle Decisions
2/6 (non-forgeable, private ctor, sealed resolver).

**Internal authority.** `RequestArena` + `RequestSlot` are the sole identity
authority. `RequestArena::validate_` (generation + non-free + context match)
gates every slot-addressing authority; `free_slot_locked_` increments
generation and clears the key/binding before the slot re-enters the free list;
`request_arena_generation_exhausted_fail_fast` prevents uint64 wrap. Public
identity is minted only by `AsyncBackend::identity_of` (friend) from the
Completion's private binding and resolved only through the private virtual
`resolve_identity_state` → `RequestArena::identity_handle_state`.

**C++ entry points.** `include/sluice/async/detail/request_key.hpp`
(ContextIdentity/SlotIndex/Generation; `for_testing` is the only public ctor),
`request_slot.hpp` (key_ storage), `request_arena.hpp`
(`validate_`, `free_slot_locked_`, `reserve`, `identity_handle_state`,
`resolve_completion`), `request_handle.hpp` (opaque value, private ctor),
`async_io_context.hpp` (`AsyncBackend::identity_of`, private
`resolve_identity_state`, `request_handle_state`).

**State transitions.** `free → reserved` installs `key_ = {ctx, idx,
generation_}`; release path: `completion_ready → free` with `generation_++`
before `free_slots_.push_back`.

**Synchronization.** Arena leaf mutex guards key/generation/free-list;
identity queries take the same leaf (`identity_handle_state` is a lock-guarded
read). Cross-context exclusion is structural (context id per backend arena,
checked in `validate_`).

**Failure behavior.** Stale handle on enqueue/dispatch → named fail-fast
(reuse-before-ack is an invariant, not a recoverable race). Stale cancel /
identity lookup → `not_found`; stale waiter REGISTRATION → `invalid_state`
(provenance misuse — W1 split; waiter-cancel keeps `not_found`). Out-of-range
introspection index → named fail-fast.

**Independent witness.** `tests/request_arena_test.cpp`;
`tests/request_lifecycle_scheme_b_test.cpp ::
generation_reuse_stale_attempts`; `tests/request_handle_test.cpp`
(stale-generation, cross-context, post-reset); negative-compile:
`scripts/verify-request-arena-negative-compile.sh` (6/6),
`scripts/verify-request-handle-authority-negative-compile.sh` (9 cases),
`scripts/verify-completion-authority-negative-compile.sh` (12/12).

**Formal evidence.** `spec/tla/request_arena/` (2 positive / 6 negative
gates; manifest notes residual multi-slot scope as executable-evidence debt —
handed to S1A below).

**Historical evidence.** Phase B closeout (ADR Gate-4 evidence map); finding
#5 (64-bit generation with fail-fast); PR #106 (F3 sealed handle seam).

**Drift classification:** ALIGNED. **Resolution:** none. **Residual:**
TLA+ request_arena suite models the single-slot protocol; multi-slot
interleavings rest on C++ executable evidence only (S1A handoff).

## B02 — Admission / submit transaction / rollback

**Protected property.** A successful `submit_*` is a complete committed
lifecycle: slot bound with stable key, Completion `outstanding`, borrow begun,
accounting incremented, enqueue pin live, and a reliable terminal path with no
post-accept allocation dependency. A failed `submit_*` leaves zero residue:
Completion idle, slot free, no borrow, no accounting, no queue/kernel work.
After the `binding → outstanding` release-store the submit may never return a
rejection.

**Public/observable contract.** AC-3; ADR Decision 5 (five-stage reserve →
prepare → commit → enqueue → dispatch; Step-5 LP); Decision 6 (error
vocabulary); AGENTS.md §3.2; `api.md` submit/L8 clauses.

**Internal authority.** The ONE shared pre-accept ladder
`detail::submit_transaction` (`include/sluice/async/detail/
submit_transaction.hpp`), parameterized per backend by `SubmitPolicy`. The
arena owns the slot half (`reserve/prepare/commit/rollback_reserved_or_
prepared`); the Completion owns the binding half (`begin_binding →
install_binding → commit_binding`, `rollback_binding_before_accept`).

**C++ entry points.** `submit_transaction.hpp` (ladder + full rollback
ladder); per-backend policies: `threadpool_backend.hpp SubmitPolicy`,
`uring_backend.hpp SubmitPolicy` (Stage-0 ring→poison→admission gate),
`fake_backend.hpp`, `sync_backend.hpp`; `request_arena.hpp`
(`reserve`, `prepare`, `install_publication_binding`, `commit`,
`rollback_reserved_or_prepared`); `completion.hpp`
(`begin_binding_for_backend`, `commit_binding_to_outstanding`,
`rollback_binding_before_accept`).

**State transitions.** `free→reserved→prepared→pending` (+ pin, borrow,
accepted_outstanding++) with the LP = Completion `binding→outstanding`
release-store under the backend admission lock; rollback ladder reverses
every stage below the LP.

**Synchronization.** Backend admission transaction locks serialize the ladder
against `close_admission`: `admission_mtx_` (Fake/ThreadPool), `dispatch_mtx_`
(Uring), caller-side `access_mtx_` (Sync). Admission lock → arena leaf only.

**Failure behavior.** Capacity `would_block` / admission-closed
`invalid_state` / malformed descriptor `invalid_argument` (real backends,
Stage 1.5 after reserve; reference backends defer per DIV-14) — all
synchronous, Completion idle. Post-LP failures are terminal-only. The ladder
contains no rejection return past the LP; a throw would hit the noexcept
boundary and terminate.

**Independent witness.** `tests/reference_backend_no_alloc_test.cpp` (3
cases: zero-allocation accepted path + transactional rejection under throwing
operator new); `tests/completion_binding_test.cpp` (two-context CAS
contention); `tests/threadpool_backend_phase_e_test.cpp` (C2d stage-failure
injection); `tests/backend_conformance_test.cpp`
(`capacity_rejects_with_idle_completion`,
`capacity_accepts_exact_limit`, `capacity_rejection_never_completes`,
`capacity_recycles_after_reset`).

**Formal evidence.** `spec/tla/request_arena/` (reserve/commit/rollback
transitions within the modeled protocol).

**Historical evidence.** Phase B round-2 finding C1 (transactional
pre-commit admission), PR #62/#63 closeouts, Phase C2d injection gates.

**Drift classification:** ALIGNED. **Resolution:** none. **Residual:**
`no_space` (ADR Decision 6) has no reserve-path emitter: genuine arena
construction failure surfaces as `std::bad_alloc` from the backend
constructor (before any Result channel exists). The
capacity-vs-initialization-failure distinction is preserved structurally
(constructor throw vs `would_block` Result); vocabulary nuance recorded here,
no code/doc change required.

## B03 — Terminal winner

**Protected property.** Exactly one terminal result per accepted generation.
The first successful transition into `backend_ready` wins; losers never
overwrite result storage, double-push the ready ring, decrement accounting
twice, or mutate generation. Pending enqueue/cancel arbitrate in one
slot-state domain (Scheme B): a pending cancel winner makes enqueue a
successful no-op; neither outcome leaves both pending linkage and ready
linkage live.

**Public/observable contract.** ADR Decision 12 / I10 / I17; AGENTS.md
§3.2 (single terminal-winner arbitration); AC-5; `api.md`
`exactly_once_terminal` conformance clause.

**Internal authority.** `RequestArena::record_terminal` / `RequestArena::
cancel` under the arena leaf mutex — state validated BEFORE the terminal
write (`request_arena_terminal_state_fail_fast` on reserved/prepared);
`terminal_.stored` early-return is the loser no-op; unstored results are
rejected up front (`request_arena_invalid_terminal_fail_fast`).

**C++ entry points.** `request_arena.hpp` (`record_terminal`, `record_
canceled`, `cancel`, `push_ready_locked_`); producers: `threadpool_backend.cpp
worker_loop → run_syscall → record_terminal` (verbatim real result; cancel
intent consumed, never rewritten), `uring_backend.cpp handle_one_cqe →
finalize_operation_terminal_` (deferred until tagged control retires).

**State transitions.** `pending/enqueued/running → backend_ready` (single
winner); `pending → backend_ready(canceled)` competes with `pending →
enqueued` under the same mutex; enqueue observing `backend_ready` acks the
pin and no-ops.

**Synchronization.** Arena leaf mutex only. Workers record terminals with no
backend work lock held (ThreadPool) / on the single-driver CQE domain
(Uring).

**Failure behavior.** Illegal terminal states fail fast in Debug and Release;
a lost `record_terminal` in `finalize_operation_terminal_` (uring) fail-fasts
("operation terminal lost RequestArena winner authority").

**Independent witness.** `tests/request_lifecycle_scheme_b_test.cpp ::
exactly_one_terminal_winner`, `pending_cancel_wins_before_enqueue_then_
enqueue_noop`; `tests/backend_scheme_b_race_test.cpp` (real two-thread
backend-level race); `tests/request_arena_cancel_intent_test.cpp` (6 cases —
ordinary success NOT rewritten to canceled); conformance
`exactly_once_terminal`, `cancel_yields_defined_terminal`,
`submit_reaps_exactly_once`.

**Formal evidence.** `spec/tla/request_arena/` (terminal-winner + Scheme-B
arbitration gates; 6 negative models).

**Historical evidence.** Round-4 findings 1/2 (running cancel verbatim;
unstored-result rejection), Phase B closeout.

**Drift classification:** ALIGNED. **Resolution:** none. **Residual:**
none beyond the multi-slot TLA scope note in B01.

## B04 — Completion publication

**Protected property.** Only the designated reap path may make a
caller-owned `Completion<T>` ready. Workers, CQE handlers, cancel paths, and
Scheduler code only stage `backend_ready`. The Completion-ready
release-store is the single linearization point; an acquire observer of
`ready` sees the installed result, closed registration, decremented
accounting, and ended borrow.

**Public/observable contract.** AC-5 / AC-13 / AC-15; ADR Decisions 9 / I11
/ I18; AGENTS.md §3.3; `api.md` AsyncBackend section (protected
publish helpers, trusted backend-author role).

**Internal authority.** `RequestArena::reap` invokes the slot's
type-erased `CompletionBinding::publish` thunk INSIDE the leaf domain; the
thunk reaches the protected `AsyncBackend::publish` →
`Completion::publish_from_reap` (single-winner CAS `outstanding →
publishing → ready`).

**C++ entry points.** `request_arena.hpp` (`reap`, `install_publication_
binding` — binding validated before any accounting change); `completion.hpp`
(`publish_from_reap`); thunks: `ThreadPoolBackend::publish_size_ready/
publish_void_ready`, `UringAsyncBackend::publish_*_ready`,
`FakeAsyncBackend::publish_*_ready`, `SyncBackend::publish_*_ready`. Grep
audit at this SHA: the ONLY invoker of the thunks is
`RequestArena::reap` via the installed binding; no worker, cancel, or
Scheduler path calls `AsyncBackend::publish` directly.

**State transitions.** `backend_ready → completion_ready` (reap) +
Completion `outstanding → publishing → ready`.

**Synchronization.** Arena leaf mutex held through the release-store; sink
invoked strictly after leaving the domain, with a by-value `ReadyEvent`
carrying no Completion/slot pointer.

**Failure behavior.** Missing publication binding at reap → named fail-fast
(never a silent skip — would strand the Completion outstanding forever);
concurrent publisher loses the CAS and fail-fasts before storage mutation;
reap-without-binding / record-terminal-on-prepared death cases in
`tests/request_arena_death_test.cpp`.

**Independent witness.** `tests/completion_authority_death_test.cpp`
(concurrent publication death); `tests/request_lifecycle_scheme_b_test.cpp ::
ready_sink_event_survives_reset_during_callback`,
`acquire_observer_of_ready_sees_all_effects` (I18 order trace);
`tests/phase_g_backend_progress_wake_test.cpp`; negative-compile 12/12
(ordinary code cannot publish).

**Formal evidence.** `spec/tla/request_arena/` (reap-only publication
within the modeled protocol).

**Historical evidence.** P1-03 resolution (SyncBackend cancel bypass
removed, PR #61); P0-03 (public mutators removed); Phase B round-2 C2/C3
(publication binding in the slot record, reap publishes INSIDE the leaf
domain).

**Drift classification:** ALIGNED. **Resolution:** none. **Residual:**
none.

## B05 — Borrowed buffer / storage lifetime

**Protected property.** fd/buffer borrowing begins at commit (the
`binding → outstanding` LP) and ends only at completion-ready publication by
reap. None of cancel-requested, backend-ready, CQE arrival, syscall return,
or shutdown-requested ends the borrow. The caller may not mutate a write
source, touch a read destination, or close/reuse the fd before the
Completion reports ready.

**Public/observable contract.** ADR Decision 8 / I7; AGENTS.md §3.8
(borrowed buffer address stability); `api.md` op descriptors + L7
address-stability clause.

**Internal authority.** `RequestSlot::borrow_` (fd/address/length/active),
guarded by the arena leaf. `commit` sets `borrow_.active = true` (prepare
stages metadata only); `reap` clears it inside the same critical section
that publishes ready (I18 order trace proves borrow-end precedes the
release-store).

**C++ entry points.** `request_arena.hpp` (`prepare` — `active=false`,
`commit`, `reap`); backend scratch: `threadpool_backend.hpp
PreparedBlockingOp` (worker reads it only after a current-generation
`mark_running`), `uring_backend.hpp PreparedUringOp` (SQE filled at
dispatch; kernel access ends at CQE, strictly before reap).

**State transitions.** commit ⇒ borrow active; reap ⇒ borrow ended +
completion-ready in one critical section; release (reset/ready-destroy)
requires completion_ready.

**Synchronization.** Arena leaf mutex; the worker's prepared-op copy is
handed off under the same backend work-lock critical section as
`mark_running` (no pop-before-running gap).

**Failure behavior.** The borrow outlives every failure path (dispatch
failure, cancel) until reap; ready-ring/terminal machinery never clears it
early.

**Independent witness.** `tests/backend_c2c_waiter_borrow_test.cpp` (C2c
borrow matrix across prepare-inactive / commit-active /
backend_ready-still-active / completion-ready-ended windows, via the
generation-validated `borrow_for_test` seam);
`tests/request_waiter_borrow_lease_test.cpp`.

**Formal evidence.** Partial — `spec/tla/request_arena/` models the
borrow-active flag transitions; no dedicated multi-slot borrow model (S1A
handoff).

**Historical evidence.** Phase C2c closeout.

**Drift classification:** ALIGNED. **Resolution:** none. **Residual:**
Uring fixed-file/registered-buffer borrow interactions remain deferred
(DIV-09), out of S0B scope.

## B06 — Cancellation vs ordinary completion

**Protected property.** Layered cancellation stays layered: task
(`CancelToken` epoch), wait (`cancel_waiter` removes only the waiter), I/O
operation (`AsyncIoContext::cancel`), syscall interruption, admission close,
shutdown. I/O cancel on a pending/enqueued request may win the canceled
terminal (Scheme B); on a running blocking syscall it records intent only —
the real result wins verbatim; on kernel-owned io_uring it appends a bounded
AsyncCancel whose CQE is control-informational and never independently owns
the terminal; stale generation ⇒ `not_found`; terminal ⇒ `already_terminal`.
Exactly-once terminal is preserved on every path.

**Public/observable contract.** AC-9; ADR Decision 11; AGENTS.md §3.4;
`api.md` cancel clauses + RuntimeTaskContext `cancel_waiter` section
(wait-cancel ≠ I/O-cancel); ADR-cancel-request-epoch (token rearm).

**Internal authority.** `RequestArena::cancel` is the single disposition
authority (`terminal_won / intent_recorded / already_terminal / not_found /
not_supported`); cancel intent (`cancel_intent_`) is consumed by the terminal
winner; per-backend local-execution disarm under the backend work/dispatch
lock (`remove_exact` + `arena_.cancel` in one critical section);
`CancelToken` epoch model for the task layer.

**C++ entry points.** `request_arena.hpp` (`cancel`, `cancel_waiter`,
`register_waiter`, `cancel_intent_live`); `threadpool_backend.cpp
ThreadPoolBackend::cancel`; `uring_backend.cpp cancel_handle_`,
`issue_running_cancel` (one per-slot `cancel_queued` bit; cookie resolved
BEFORE `io_uring_get_sqe`), `handle_one_cqe` (control CQE retires the exact
router reference; deferred terminal released only then); `cancel.hpp` /
`src/async/cancel.cpp` (epoch).

**State transitions.** `pending/enqueued → backend_ready(canceled)` vs
`pending → enqueued` arbitration; `running + cancel → cancel_intent_`;
waiter registration state machine (`open_no_waiter → open_registered →
closed`).

**Synchronization.** Backend work/dispatch lock for local disarm + arena
cancel (one critical section); arena leaf for intent/disposition; Uring
cancel-SQE append under `dispatch_mtx_` with CQE retirement on the
single-driver domain.

**Failure behavior.** Dispositions are typed results, never silent;
duplicate control state before AsyncCancel append is an invariant fail-fast;
repeated cancels are idempotent per slot (bounded SQE count).

**Independent witness.** `tests/async_cancel_test.cpp`;
`tests/request_arena_cancel_intent_test.cpp` (6 cases);
`tests/request_lifecycle_scheme_b_test.cpp :: cancel_races_per_state`;
conformance `cancel_yields_defined_terminal` family;
`tests/cancel_token_test.cpp` (T-CANCEL-REARM-1 etc., RED→GREEN);
`tests/uring_backend_c2e_close_drain_test.cpp` (cancel + close/drain);
`tests/runtime_wait_test.cpp` (wait-cancel / I/O independence).

**Formal evidence.** `spec/tla/cancel_token_epoch/` (1 pos / 5 neg);
`spec/tla/e10_waitnode/` (wait cancel); `spec/tla/request_arena/`
(Scheme-B).

**Historical evidence.** Round-4 finding 1; Phase D3 C2b/C2c seams; issue
#262 (open incident; attribution unresolved — see Residual).

**Drift classification:** ALIGNED. **Resolution:** none. **Residual:**
issue #262 is a naturally occurring unresolved incident: a kernel-originated
operation CQE delivered `-ECANCELED` with no repository-local explicit cancel
request identified; the trigger and causal-layer attribution remain
unresolved. A separate historical teardown symptom has only provisional
attribution and is NOT proven to share one root cause. The deterministic
contract↔C++ paths audited here are aligned, but #262 may represent an
unmodeled environment/interleaving/state — it is adversarial input for S1A
formal coverage and the later discrimination/real-incident track (recorded
with this matrix as the contract baseline; no Sluice/kernel/WSL2/
multi-driver/teardown classification is claimed).

## B07 — Deadline / timeout semantics

**Protected property.** Deadlines are absolute monotonic ticks (`expired iff
now >= deadline`); registration, expiry, and retirement have exactly-once
winner arbitration; an already-due deadline at admission resolves Expired
inline through the same resolve authority (no strand); deadline waiting is
distinct from operation cancellation — a timer win resolves the WAIT
(`expired`), never cancels I/O; retirement (RETIRED by a non-timer winner)
closes callback authority so a stale expiry cannot touch a destroyed
WaitNode.

**Public/observable contract.** `api.md` synchronization glossary (absolute
monotonic deadline; already-due; admission precedence resource-first except
Condition deadline-first); ADR-async-primitive-lifetime-failfast; AGENTS.md
§3.6 (wake obligation for every progress-enabling state change);
TimerRegistration header contract.

**Internal authority.** `Scheduler` owns the clock (`monotonic_now` /
test-clock seam), the deadline heap, and `pump_deadlines_locked`;
`TimerRegistration` (ACTIVE/RETIRED/CONSUMED atomic) is the independently
stable retirement identity; `await_wait_deadline` performs the unified
admission critical section with the already-due closure.

**C++ entry points.** `include/sluice/async/timer_registration.hpp`;
`src/async/scheduler_timer.cpp` (`monotonic_now`, `await_wait_deadline`,
`expire_wait`, `pump_deadlines_locked`); bounded-park plumbing:
`async_io_context.hpp/.cpp wait_one(max_park)` + `BackendWaitSource::
supports_bounded_wait` (a source without bounded transport gets a
synchronous `not_supported`, never a silently discarded deadline).

**State transitions.** timer registration `ACTIVE → CONSUMED` (expiry wins)
vs `ACTIVE → RETIRED` (non-timer winner); lazy heap erasure at deadline.

**Synchronization.** Admission and every resolver under `global_mtx_`;
`TimerRegistration::state` is lock-free acquire/release so an expiry checks
retirement BEFORE dereferencing the bound node (the E11 post-destruction
closure).

**Failure behavior.** Duplicate/lost expiry impossible by CAS; lost retire is
a no-op loser; registration admission allocations happen before any mutation
(R2-ALLOC: bad_alloc leaves counters untouched).

**Independent witness.** `tests/ordinary_deadline_authority_test.cpp`;
`tests/select_timer_registration_test.cpp`; `tests/select_timer_pump_death_
test.cpp`; E11 timer suite (`timer_wait_test.cpp` family); issue #229
closeout (deadline seam race fixed + regression).

**Formal evidence.** `spec/tla/e11_timer_wait/` (2 pos / 6 neg);
`spec/tla/e13_select/` (timer arms).

**Historical evidence.** E11/E13 phase gates; issue #229.

**Drift classification:** ALIGNED. **Resolution:** none. **Residual:**
none for the participating surface.

## B08 — Wait / wake / lost-wakeup closure

**Protected property.** Every progress-enabling state change has a
persistent predicate/state + epoch, a producer that publishes state BEFORE
notifying, a commit-to-sleep race closure, bounded worst-case observation
latency where polling is the documented authority, and shutdown behavior.
Backend ready progress (split-phase epoch wait), Scheduler runnable routing
(wake epoch + interrupt bridge), timer expiry (deadline pump), and the
commit-to-park handshake each close their own window.

**Public/observable contract.** AC-6; AGENTS.md §3.6; `api.md`
AsyncIoContext wait_one control-wake theorem block; DIV-04/DIV-05 (2 ms
bounded observation interval is protocol authority for MIXED-WAKE backend
progress).

**Internal authority.** Backend domain: `ReadyWaitSource` /
`UringWaitSource` (progress + control epochs under their own leaf mutex;
`arm_committed_wait`/`consume_committed_wait` handshake). Scheduler domain:
`signal_wake_locked`, `route_runnable_locked`, wake epoch, inbox. Waiter
identity: arena slot registration + `Scheduler::ReadyRoutingSink::on_ready`
(identity-validated, exactly-once delivered marking under
`wait_registry_mtx_`).

**C++ entry points.** `include/sluice/async/detail/ready_wait_source.hpp`,
`uring_wait_source.hpp`; `async_io_context.cpp wait_one` (snapshot → poll →
park loop; invocation-level control baseline); `threadpool_backend.cpp
signal_ready_progress` / `wait_one`; `uring_backend.cpp poll/wait_one`
(state first, then notify); `scheduler.cpp
drain_routed_completion_waits_locked`, `ReadyRoutingSink::on_ready`,
`scheduler_park_wake.cpp` (wake handle lifetime, park/wake).

**State transitions.** progress/control epoch advance; WaitRecord
`registered → delivered → free`; fiber `waiting → runnable` (exactly-once
`make_runnable`).

**Synchronization.** Snapshot-then-poll-then-park order closes both
lost-wake windows; the one-shot control baseline prevents rebaselining;
`interrupt_backend_waiters` is the Scheduler→backend-domain bridge; lease
destruction = acknowledgement.

**Failure behavior.** A final non-blocking reap after an interrupt closes
the interrupt-vs-final-ready race (returns the reaped count; 0 only when
nothing was reaped — never a fabricated completion).

**Independent witness.** `tests/issue115_runnable_publication_wake_test.
cpp`; `tests/issue116_interrupt_reevaluation_regression_test.cpp`;
`tests/issue161_idle_dance_orphan_test.cpp` (+ pub-erase orphan);
`tests/phase_g_backend_progress_wake_test.cpp`;
`tests/async_io_context_split_wait_c2e_test.cpp`;
`scheduler_identity_wake_test.cpp` (+ death);
`tests/scheduler_progress_test.cpp`.

**Formal evidence.** `spec/tla/e9_park_wake/` (4 pos / 8 neg);
`e9_wake_handle_lifetime/`; `spawn_wake_epoch/`; `e7_publication/`;
`e7_multiworker_progress/`; `f1_wait_record/`.

**Historical evidence.** Issues #115/#116/#161 closeouts; Phase G gate.

**Drift classification:** ALIGNED. **Resolution:** none. **Residual:**
none for the participating surface.

## B09 — Resource accounting / retirement

**Protected property.** Each bounded resource has a single admission owner,
an increment point, a retirement point, a distinct full behavior, double-
retire prevention, and a shutdown owner: request capacity (arena slots,
`would_block`), blocking-I/O workers (fixed pool), dispatch ring
(construction-bounded; full push = invariant fail-fast, never
`would_block`), ready ring (pre-reserved linkage), kernel ring depth
(distinct from request capacity; `request_capacity > queue_depth` legal),
wait records (pool + free list, `no_space` on exhaustion), and
caller-owned Completions (caller-bounded). Counters `slot_in_use` /
`accepted_outstanding` / high-water / capacity-rejections are distinct and
never conflated with lifecycle violations.

**Public/observable contract.** AC-7; ADR Decision 13 / I8 / I9;
AGENTS.md §3.5; `api.md` ThreadPoolBackend resource-observation table
(AC-1a).

**Internal authority.** `RequestArena` (slot_in_use, accepted_outstanding,
high_water_mark, capacity_rejections, backend_ready_count — all under the
leaf); `ThreadPoolBackend` (worker_count fixed; dispatch ring + high-water;
active_workers under work_mtx_); `UringAsyncBackend` (queue_depth; router
free-list; transport ledger bounded by `sq.ring_entries`); `Scheduler`
(wait-record pool; deadline pool).

**C++ entry points.** `request_arena.hpp` (counter sites in
reserve/commit/reap/free_slot_locked_); `threadpool_backend.cpp`
(constructor spawn-failure rollback joins started workers and rethrows;
destructor quiescence preflight); `uring_backend.cpp` (cookie no-wrap
`allocate_cookie_`; `retire_router_entry_`; ledger append/consume);
`scheduler_park_wake.cpp` (wait-record free list, `no_space`).

**State transitions.** reserve++/release--; commit++/reap--; router
install/retire; ledger append/drain.

**Synchronization.** Each counter changes only under its owning domain;
per-domain read accessors hold exactly one lock and never nest (no combined
snapshot by design).

**Failure behavior.** Capacity refusal is synchronous `would_block` with
idle Completion; dispatch-ring overflow / ready-ring corruption /
router-double-retire are invariant fail-fasts; spawn failure fails
construction with full worker cleanup.

**Independent witness.** `tests/capacity_validity_test.cpp`; conformance
capacity family + `stats_accounting` + `capacity_stats_are_exact`;
`tests/blocking_io_pool_invariants_test.cpp`;
`tests/application_runtime_resource_test.cpp`;
`tests/async_stats_wait_race_test.cpp` (metric vocabulary:
`queue_full_retries` ≠ `invalid_state_rejections`).

**Formal evidence.** `spec/tla/blocking_io_pool/` (worker/shutdown
protocol); `spec/tla/d1_uring_poison/` (ledger boundedness/quiescence).

**Historical evidence.** Phase E design/closure; TAX-0 resource-seam
additions (#234).

**Drift classification:** ALIGNED. **Resolution:** none. **Residual:**
none for the participating surface.

## B10 — Shutdown / quiescence

**Protected property.** Destruction is quiescent and fail-fast: backend/
context destructors do not implicitly close admission, cancel, drain, wait,
or publish; destroying with bound slots/accepted work is a contract
violation in Debug AND Release; the legal order is close admission →
progress/reap → caller reset/destroy ready Completions → all ownership zero
→ destroy; idle persistent workers may be joined at destruction (worker-pool
teardown, not an I/O drain); `close_admission` atomically prevents new
acceptance (serialized against the Step-5 LP) while accepted work continues
to ordinary terminals; a parked waiter is woken exactly once to re-evaluate.

**Public/observable contract.** ADR Decision 15 / I12 / I14; AGENTS.md
§3.7; AC-4; `api.md` ThreadPoolBackend / FakeAsyncBackend close_admission
clauses + AsyncIoContext L11 note + ApplicationRuntime state table.

**Internal authority.** `AsyncIoContext::~AsyncIoContext`
(`async_context_outstanding_fail_fast`), `RequestArena::
~RequestArena` (`request_arena_destruction_fail_fast` on slot_in_use),
`ThreadPoolBackend::~ThreadPoolBackend` (quiescence preflight under
work_mtx_ then stop/join), `UringAsyncBackend::~UringAsyncBackend`
(preflight: dispatch empty, zero live cookies/controls, ledger quiescent or
proven Class-A retired, arena snapshot zero — BEFORE `io_uring_queue_exit`),
`ApplicationRuntime` (explicit start/stop/drain/join/shutdown state
machine).

**C++ entry points.** the destructors above; `close_admission` in
`threadpool_backend.cpp` / `uring_backend.cpp` / `fake_backend.hpp` /
`sync_backend.hpp`; `request_arena.hpp close_admission` +
`quiescence_snapshot`; `application_runtime.cpp`.

**State transitions.** admission open → closed (idempotent); worker
running → joined; ring teardown only after zero execution references.

**Synchronization.** close takes the admission-transaction lock (same lock
as the submit LP); destructor preflight under work/dispatch lock → arena
leaf (existing frozen order); one-shot control wake after close.

**Failure behavior.** Non-quiescent destruction → named fail-fast in both
modes; post-close submit → synchronous `invalid_state` with idle
Completion.

**Independent witness.** conformance `close_rejects_future_submit`,
`close_preserves_accepted_terminal`, `clean_shutdown_no_ops`,
`drain_then_reset_releases_slot`,
`slot_released_but_admission_stays_closed`; `tests/uring_backend_c2e_
close_drain_test.cpp` (+ death); `tests/fake_backend_c2e_close_drain_test.
cpp`; `tests/async_io_context_death_test.cpp`;
`tests/runtime_wait_death_test.cpp`; D4-RM11 destructor-order probe.

**Formal evidence.** `spec/tla/d1_uring_poison/` (exact control-reference
quiescence; post-poison Class-A); `spec/tla/e16_application_runtime/`;
`spec/tla/blocking_io_pool/` (shutdown drain).

**Historical evidence.** Phase D4 / C2e / E closeouts; sluice-copy Phase-3
drain fix (#258).

**Drift classification:** ALIGNED. **Resolution:** none. **Residual:**
issue #262 (see B06 residual) is an open incident whose possible teardown
intersection is itself unproven; tracked as adversarial input there.

---

## Documentation drift findings (pre-fix records)

Each record follows the required Step-D shape. D1–D3 were fixed in this PR's
original docs commit; the records below preserve the pre-fix state. W1 (next
section) was found by S0B-CORRECTIVE-1 re-adjudicating D2's waiter half and
is preserved in the same format.

### D1 — api.md reset()-slot-release scope (DOC_TOO_WEAK)

```text
EXPECTED CONTRACT: reset()/ready-destruction releases the bound arena slot
  for a request accepted through ANY arena backend.
ACTUAL C++: all four backends (Fake, Sync, ThreadPool, Uring real path)
  accept through detail::submit_transaction, whose policy installs the
  Completion binding (install_binding) unconditionally; Completion::reset()
  / ~Completion call release_completed_binding whenever release_arena_ is
  non-null. Verified: threadpool_backend.cpp submit_size/submit_void,
  uring_backend.cpp submit_size/submit_void, fake_backend.hpp:509/527,
  sync_backend.hpp (same ladder).
WHY THEY DIFFER: api.md sentence predates the Phase D/E completion of the
  four-backend migration and still says "the reference backends
  (FakeAsyncBackend, SyncBackend)".
CLASSIFICATION: DOC_TOO_WEAK (Case 2 — intended behavior proven by DIV-02's
  completed-migration record and the shared ladder).
RESOLUTION: docs — api.md Completion section now names all four arena
  backends; api.zh-CN.md Completion section rewritten to the current
  header surface and states the same four-backend handshake.
```

### D2 — api.md error-vocabulary emitter scope (DOC_TOO_WEAK)

```text
EXPECTED CONTRACT: invalid_argument / not_found / not_supported are emitted
  by every backend that implements the corresponding stage.
ACTUAL C++: ThreadPoolBackend and UringAsyncBackend validate descriptors
  (Stage 1.5 → invalid_argument); all four arena backends return not_found
  for unresolvable waiter registration; not_supported for identity-less
  backends.
WHY THEY DIFFER: same stale "reference backends" phrasing as D1; the
  zh-CN error enum additionally predated the three contract codes
  entirely (missing invalid_argument/not_found/not_supported).
CLASSIFICATION: DOC_TOO_WEAK (Case 2).
RESOLUTION: docs — api.md IoError section now states the full emitter
  set; api.zh-CN.md error model now lists all three contract codes and
  the emitter set.
CORRECTIVE-1 ADDENDUM: the DOC_TOO_WEAK classification above was correct
  only for the cancel-lookup and descriptor-validation halves. The
  "all four arena backends return not_found for unresolvable waiter
  registration" half was NOT a doc understatement: the base
  AsyncBackend contract says invalid_state, the ADR grants not_found
  only to cancel lookups, and all four backends implementing not_found
  was an undocumented production drift. Re-adjudicated as W1 below;
  the original record is preserved verbatim.
```

### D3 — api.md AsyncIoContext waiter/sink surface (DOC_TOO_WEAK)

```text
EXPECTED CONTRACT: the public AsyncIoContext surface participating in
  waiter identity (B08) and shutdown bridging (B10) is documented in the
  public reference.
ACTUAL C++: AsyncIoContext publicly exposes register_waiter ×2,
  cancel_waiter ×2, set_ready_sink, arm_backend_wait_commit
  (include/sluice/async/async_io_context.hpp); they are documented in
  docs/architecture/async-request-lifecycle.md and the headers but absent
  from the api.md class listing (only the RuntimeTaskContext await/cancel
  waiter view was documented).
WHY THEY DIFFER: the api.md listing predates the F1 waiter surface and the
  Phase G park-commit handshake.
CLASSIFICATION: DOC_TOO_WEAK (Case 2).
RESOLUTION: docs — api.md AsyncIoContext section now lists the six methods
  with their contracts and cross-references. The zh-CN companion's async
  section was additionally repaired for the participating surfaces it
  described with removed/nonexistent APIs (context-level cancel-all,
  factory functions, public next_reap_seq(), noexcept result(), the
  pre-epoch CancelState surface, Batch constructor/cancel); its header now
  scopes the async explicit-I/O surface to the English reference as the
  sync authority. Remaining zh-CN↔English correspondence outside the
  participating surfaces is translation debt (C-01 scope rule).
```

### W1 — waiter error-vocabulary authority conflict (S0B-CORRECTIVE-1, IMPLEMENTATION_DRIFT)

The original freeze carried D2's claim — "every arena backend returns
`not_found` for an unresolvable/stale waiter ... lookup" — into api.md as a
pure-doc fix. Adversarial review rejected that adjudication: the three-way
correspondence (Accepted ADR vs base interface header vs derived production
implementation) had never been resolved. The pre-fix record:

```text
EXPECTED CONTRACT (governing authority):
  ADR-explicit-io-request-contract Decision 6: invalid_state covers caller
    lifecycle/provenance misuse INCLUDING "direct use of an invalid/stale
    key"; the not_found exception is granted EXPLICITLY and ONLY to cancel
    lookup ("A cancel lookup may return CancelDisposition::not_found for an
    absent or stale generation").
  Decision 10: duplicate waiter registration -> synchronous invalid_state;
    a failed registration consumes the candidate lease at the by-value
    boundary.
  Registering a waiter is an INSTALL action on a caller-held Completion, not
  a lookup; the ADR grants it no not_found exception.
BASE INTERFACE (include/sluice/async/async_io_context.hpp, AsyncBackend):
  register_waiter: "an unresolvable Completion (unbound / cross-context /
    stale) returns invalid_state (provenance misuse, Decision 6)";
  cancel_waiter: not_found when reap already closed the registration.
ACTUAL BACKENDS (pre-fix, all identical):
  Fake / Sync / ThreadPool / Uring: resolve_completion miss ->
    register_waiter returned not_found — contradicting the base contract
    they implement. The four derived headers even documented not_found,
    while the base header documented invalid_state: the F1 commit
    (41a05a64) introduced both sides of the contradiction at once, so no
    later accepted authority superseded Decision 6 — this is production
    drift, not superseded docs.
  Arena register_waiter also returned not_found for a stale SlotHandle;
  invalid_state for reserved/prepared/completion_ready/duplicate (that
  half matched the ADR).
EXISTING TEST ORACLE: the corpus pinned the DRIFT, not the ADR:
  tp_stale_waiter_authority_harmless, uring_c2c_stale_waiter_authority_
  harmless, fake_waiter_seam_unbound_completion_not_found, and the arena
  stale-registration case all asserted not_found; the F1 closeout recorded
  the Scheduler NORMALIZING not_found back to invalid_state for
  await_completion — evidence the production consumer never relied on the
  drifted code.
WHY THEY DIFFER: implementation shortcut took the cancel-lookup vocabulary
  for the shared resolve miss without an authority decision; the S0B
  original pass mistook the resulting test/comment consensus for intended
  semantics.
CLASSIFICATION: IMPLEMENTATION_DRIFT (Case 1 — code violates the governing
  authority; authority upheld, code corrected).
```

The adjudicated split, pinned by the new shared regression
(`tests/support/waiter_error_vocabulary_cases.hpp`, driven through the PUBLIC
interface on all four arena backends):

| Surface | Fake | Sync | ThreadPool | Uring | Base contract | ADR (D6+D10) |
| --- | --- | --- | --- | --- | --- | --- |
| register_waiter, unbound Completion | invalid_state (was not_found) | invalid_state (was not_found) | invalid_state (was not_found) | invalid_state (was not_found) | invalid_state | invalid_state |
| register_waiter, cross-context | invalid_state (was not_found) | invalid_state (was not_found) | invalid_state (was not_found) | invalid_state (was not_found) | invalid_state | invalid_state |
| register_waiter, stale/released | invalid_state (was not_found) | invalid_state (was not_found) | invalid_state (was not_found) | invalid_state (was not_found) | invalid_state | invalid_state |
| register_waiter, duplicate | invalid_state | invalid_state | invalid_state | invalid_state | invalid_state | invalid_state |
| register_waiter, post-reap (closed) | invalid_state | invalid_state | invalid_state | invalid_state | invalid_state | invalid_state |
| register_waiter, reserved/prepared | invalid_state | invalid_state | invalid_state | invalid_state | (not in header text) | invalid_state |
| cancel_waiter, no registration | not_found | not_found | not_found | not_found | not_found | not_found (cancel lookup) |
| cancel_waiter, unresolvable/stale | not_found | not_found | not_found | not_found | not_found | not_found (cancel lookup) |
| cancel_waiter, reap closed | not_found | not_found | not_found | not_found | not_found | not_found (cancel lookup) |

`RequestHandle::request_state()` keeps `not_found` for stale/wrong-context/
released/invalid handles — a read-only observation surface with its own
granted vocabulary (ADR-public-request-handle); unchanged by W1. The I/O
`cancel` disposition keeps `CancelDisposition::not_found`; unchanged.

RESOLUTION (minimal shared layer, four-backend): `RequestArena::
register_waiter` stale-handle branch → `invalid_state`; the resolve-miss
branch of the eight `register_waiter` forwarders (4 backends × public +
test-seam layers) → `invalid_state`; `cancel_waiter` paths untouched.
Header contracts (base + four derived) now state the split explicitly.
Lease exactly-once on failure is structural (by-value move boundary) and
is asserted by the regression via the move-only type. api.md /
api.zh-CN.md corrected; production consumers audited —
`Scheduler::await_completion_*` treats both codes identically (retire
record → c.ready() check → invalid_state upward), so no runtime behavior
change beyond the code itself.

## Bounded spillover handling (S0-CONTRACT-CANDIDATES)

- **C-01 (api reference drift):** checked ONLY the surfaces participating in
  B01–B10 (submit family, Completion lifecycle, cancel, waiter/deadline,
  borrow clauses, shutdown, capacity-refusal, failure vocabulary). Found
  D1/D2/D3 and fixed them. `CopyStrategy` is present in BOTH api.md and
  `include/sluice/copy_strategy.hpp` (consistent; PR #287 removed only the
  deferred-slot speculation, not the type) and is outside the async
  correctness kernel — remaining api.md↔header correspondence is separate
  API-maintenance debt, not Safety S0B.
- **C-02 (sync durability anchors):** independent of the async correctness
  kernel. Recorded as bounded maintenance debt (re-stamp
  `docs/architecture/sync-io-architecture.md` /
  `sync-durability-model.md` verification anchors against current `src/`
  in a docs-maintenance pass); NOT expanded into this campaign.
- **C-04 (failure model):** audited only the sites participating in B01–B10.
  All use named Debug+Release fail-fasts (`request_arena_*`,
  `completion_*`, the threadpool/uring non-quiescent destruction fail-fasts); the only `assert()`
  on a participating path is the documented L9 `result()` debug check with a
  Release typed fallback. No global assert cleanup performed.
- **C-05 (verification wiring):** verified only the claims this closure
  rests on (conformance suite wiring `xmake/tests/async.lua:119`, arena/
  lifecycle/scheme-B/conformance/death test targets, negative-compile
  gates). No repository-wide CI census.

## Formal evidence → S1A handoff

Property → current model → suspected gap:

```text
B01/B02/B03/B04 → spec/tla/request_arena (2 pos / 6 neg)
    → multi-slot interleavings unmodeled (manifest coverage_gaps:
       request_arena-lifecycle PARTIALLY MODELED); reap-vs-release ordering
       across slots rests on C++ evidence.
B05             → request_arena (borrow flag)
    → no dedicated borrow-end-vs-publication model beyond I18 trace tests.
B06             → cancel_token_epoch, e10_waitnode, request_arena
    → no model of uring control-CQE/deferred-terminal interplay (the
       single-driver domain assumption is the abstraction boundary).
B07             → e11_timer_wait (2 pos / 6 neg)
    → admission-precedence variants (resource-first vs deadline-first per
       primitive) not uniformly modeled.
B08             → e9_park_wake, spawn_wake_epoch, e7_publication,
                  e7_multiworker_progress, f1_wait_record,
                  e9_wake_handle_lifetime
    → backend split-phase epoch protocol (ReadyWaitSource/UringWaitSource
       snapshot→park closure) has no direct model; covered by C++ causal
       regressions (#115/#116/#161).
B09             → blocking_io_pool, d1_uring_poison
    → arena counter invariants unmodeled beyond the lifecycle suite scope.
B10             → d1_uring_poison, e16_application_runtime, blocking_io_pool
    → ThreadPool drain/destruction interleaving with in-flight workers
       relies on executable evidence.
```

#262 as a model adversary (formal-coverage question, NOT a root-cause
claim — S1A should answer it without assuming an answer):

```text
Can the current cancellation / uring models represent:
  no caller-requested cancel
  + an operation CQE returning -ECANCELED
  + the terminal result propagated verbatim?
Does any model (or model invariant) incorrectly assume:
  canceled terminal => caller cancel intent existed?
```

S1A begins from this PR's merge HEAD. S0B adds no new models by scope rule.

## Residual uncertainty (bounded)

1. **Issue #262** — a naturally occurring unresolved incident: a
   kernel-originated operation CQE delivered `-ECANCELED` with no
   repository-local explicit cancel request identified. Unresolved: the
   trigger and the causal-layer attribution (no Sluice/kernel/WSL2/
   multi-driver/teardown classification is claimed; the historical teardown
   symptom's attribution is provisional and not proven to share one root
   cause). Non-reproduction on native Host-0 (880 launches, zero
   #262-family symptoms) is non-reproduction, NOT disproof. The
   deterministic contract↔C++ paths audited here are aligned, but #262 may
   represent an unmodeled environment/interleaving/state. Consumed by S1A
   as a model-adversary question (above) and handed to the later
   discrimination/real-incident track with this matrix as the frozen
   contract baseline.
2. **Multi-slot TLA scope** (see handoff) — executable-evidence debt, S1A.
3. **`no_space` reserve-path vocabulary nuance** (see B02 residual) —
   structural distinction holds; no emitter exists because arena
   construction failure precedes any Result channel.
4. **api.md↔header correspondence outside the B01–B10 surface** — separate
   API-maintenance debt (C-01 scope rule).
5. **zh-CN translation debt beyond the participating surfaces** — the
   English reference remains the sync authority for the async explicit-I/O
   surface (see D3 resolution).
