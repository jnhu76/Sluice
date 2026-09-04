> **HISTORICAL / EVIDENCE — NOT CURRENT AUTHORITY.** Archived from `docs/architecture/` by S0-DOCS (#290, 2026-09-04). Point-in-time record; do not cite as authority for new decisions.

# FE-4 multi-frontend equivalence audit (Event / Queue / RwLock / Condition)

Scope: the FE-4 campaign stage — the frontend-equivalence matrix, the
frontend-divergence census (FD classes; FD4 MUST be NONE), the FE-1a F0–F3
re-audit against the FE-3 tree, the deferred-resume-site audit (L9), and the
control-plane allocation audit. Baseline: `feat/frontend-semantic-reuse` @
FE-3 closeout. Evidence cited by test case name (all passing at Clang Debug
198/198 and TSan 198/198).

## 1. Equivalence matrix (per primitive, per disposition)

For each primitive: the ONE shared admission law (ladder) and the two
frontend entries over it. "Inline" = resolved without suspension;
"parked" = `authorized` + frontend PublicationEligibility commit.

| Primitive | Shared law (production) | Dispositions / outcomes | Fiber-side evidence (unchanged suites) | Deferred-side evidence (slice cases) | Mixed evidence |
|---|---|---|---|---|---|
| Event | `event_wait_admit_locked` (FE-2) | rejected / resolved_inline (set-at-admission) / authorized | `event_primitive_test` (unchanged) | `fe2_pov_inline_already_set`, §21-B/C boundary cases, `event_cancel_deferred_for_test` cases | `fe3_mix_event_set_resolves_fiber_and_deferred` |
| Queue push | `queue_push_admit_locked` | rejected / resolved_inline / resolved_inline_grant (Q-LIV-1) / authorized / closed | `async_queue_primitive_test` + fiber entries over the ladder | `fe3_q_push_inline_admissible`, closed/cancel/expiry cases, `fe3_q_fiber_blocking_pop_grants_deferred_producer` (entry-run grant) | `fe3_q_cross_fiber_waiter_coroutine_resolver`, `fe3_q_cross_coroutine_waiter_fiber_resolver` |
| Queue pop | `queue_pop_admit_locked` | same set + item/closed/empty lease custody | same | `fe3_q_pop_inline_admissible`, granted-by-resolver cases | same two cross cases |
| RwLock read | `rwlock_read_admit_locked` | rejected / resolved_inline (head-prefix claim) / authorized | `async_rwlock` fiber suites (unchanged) | `fe3_rwlock_reader_batch_deferred` (prefix batch), fairness cases | `fe3_mix_rwlock_batch_grants_fiber_and_deferred_readers` |
| RwLock write | `rwlock_write_admit_locked` + shared cores `rwlock_try_write_admission_locked` / `rwlock_unlock_write_core_locked` | rejected / resolved_inline (claim commits `writer_owner = actor`) / authorized; recursive refusal; checked unlock | fiber suites | `fe3_rwlock_deferred_writer_owns_releases`, `same_actor_different_resume_target`, `granted_by_fiber_unlock`, `cancel`, `expire` | (writer grant via fiber unlock covered in-slice) |
| Condition | `condition_wait_admit_locked` | rejected_retain / resolved_inline_retain (due-inline) / resolved_inline_released / authorized | `async_condition_primitive_test` (unchanged, incl. register-before-release closure `cond_t3` and FIFO handoff `cond_t5`) | `fe3_condition_deferred_notify_one_own_reacquire`, `due_inline_retains_mutex`, `pump_expiry_reacquire`, `cancel_loser_exactly_once` (+terminal re-wait rejected_retain), `notify_all_own_reacquire` | (composition covered by the unchanged fiber suites over the SAME ladder; Mutex identity re-typing is the declared later slice) |

Equivalence claim shape: for every row, the deferred entry executes the SAME
admission sequence (register → counters → [timed: LOCAL publish] → precedence
→ authorized) as the fiber entry because both CALL one function; the entries
differ ONLY in (a) the `WaitResume` token, (b) the PublicationEligibility
commit (`commit_suspend_locked` vs `record.arm()`), (c) the physical
suspension mechanism. Winner delivery differs ONLY at the publication tail's
kind switch. Both are proven exactly-once by the resumed/guard counters in
every cited case.

## 2. Frontend-divergence census (FD classes; FD4 MUST be NONE)

Classes (operationalized for this campaign):
- FD0 — no divergence: identical law, identical admission text, shared
  function.
- FD1 — representation typing: same law, frontend-specific token/record
  types (accepted by the frozen contract).
- FD2 — narrow mechanism cluster: the suspension/publication mechanism
  differs (commit_suspend+switch vs arm+transit) with the law shared.
- FD3 — structural force: a stackful invariant FORCES a second authority.
- FD4 — duplicated semantic authority: a second textual/behavioral copy of a
  semantic law (admission, winner, ownership, deadline, cancel).

Census result: **FD4 = NONE.** Structural argument (checkable): every
deferred entry is a thin CS wrapper that CALLS the production law —
`scheduler_fe2_test_seam.cpp` (380 lines) contains NO admission sequence of
its own; each `*_core_` body is: acquire G (+ the role mutex the fiber entry
also takes) → call `Scheduler::<primitive>_admit_locked` (or the shared
ownership cores) → commit the record on `authorized`. The one textual law
per direction lives in exactly one production function per row of §1.
FD1 entries: `WaitResume` / `ActorId` / `FeDeferredRecord` / frame-embedded
ctx structs (accepted typing). FD2 entries: the publication tail kind switch
and the eligibility-commit pair — both sanctioned by the frozen contract
(FE-1b L6–L10). FD3: NONE (no stackful invariant forced a copy; the Mutex
identity boundary is a deferred SLICE, not a duplicated authority).

### 2b. Sanctioned winner-delivery kind-switch copies (checkable census)

The delivery law (ResumeTarget kind → mechanism) is TEXTUALLY PRESENT in
exactly five production sites, each sanctioned and enumerated here so the
count stays checkable: (1) `publish_wait_winner_locked` — the canonical
tail; (2) `queue_publish_winner_locked` — the Queue grant tail whose fiber
branch additionally pairs `granted_not_resumed_` (divergence documented at
the declaration site); (3) `cancel_wait` (park_wake) — folds the
fiber-branch publication bool into its return value; (4) `expire_wait`
(timer) — same return-value coupling; (5) `pump_deadlines_locked` — the
pump's won-counter variant. Sites (3)–(5) are deferred-correct but cannot be
mechanically collapsed onto the void tail without changing their return
contracts; they are tracked as P3 hygiene, not divergence. The FE-4 review
round migrated the two UNSANCTIONED copies (mutex/semaphore cancel tails —
they had no coupling justification) onto the canonical tail (DIV-18).

## 3. FE-1a F0–F3 re-audit on the FE-3 tree

| FE-1a finding | FE-3 tree state | Class |
|---|---|---|
| #1 `resolve_` token-independent | unchanged (wait_node.hpp CAS touches `state_` only) | F0 |
| #2 registration token `Fiber*`-typed | RESOLVED at FE-2: `register_wait_locked(WaitNode&, const WaitResume&)` | F1 (accepted) |
| #3 deadline authority token-independent | unchanged; the FE-3 ladders reuse the SAME prepare/publish/consume authority (LOCAL publish inside the admission CS) | F0 |
| #4 cancellation closure token-independent | unchanged; condition cancel now publishes via the winner-kind tail (was fiber()-direct — a residual F2 site, CLOSED by FE-3) | F0/F2-closed |
| #5 publication `Fiber*`-typed | RESOLVED at FE-2 (tail kind switch); FE-3 migrated the remaining direct-fiber CANCEL tails onto it (queue: FE-3 slice; condition: this slice; mutex/semaphore cancel: FE-4 review round — see DIV-18). The one staged residue is `mutex_handoff_one_locked`'s OWNER COMMIT (`owner = won->fiber()` before publication) — inherently Fiber-typed until the Mutex-identity slice; migrating its publication alone would NOT close the hazard (DIV-18) | F2-closed for cancel/expire tails; handoff owner-commit = F1-open (DIV-18) |
| #6 `commit_suspend_locked` stackful coupling | accepted mechanism pair (FD2): deferred counterpart is `record.arm()` inside the same resolver-excluded CS | F1/F2 (accepted) |
| #7 Mutex/RwLock owner `Fiber*`-typed | RwLock RESOLVED (ActorId, FE-3). Mutex STILL `Fiber*& owner` — the declared later slice (Mutex choreography; Condition PoV presents bare queues) | F1-open (tracked) |
| #8 QueueWaitCtx caller-frame result storage | unchanged and REQUIRED (winner writes through `won->user()`); frame-embedded by the awaiter (FE-1a property) | F1 (accepted) |
| #9 `granted_not_resumed_` stackful bookkeeping | unchanged for the fiber branch; the deferred branch documents WHY it must NOT pair (no post-resume port access); the teardown window is owned by the deferred-transit gate | F1 (accepted) |
| #10 registration→commitment one resolver-excluded CS | preserved verbatim in ALL four ladders (the FE-3 extraction moved, did not split, the CS) | F0 |

No NEW F3-class finding. The open items are the two declared later slices
(Mutex identity re-typing) — tracked, not hidden.

## 4. Deferred-resume-site audit (L9: no user code under authoritative locks)

The Core's entire deferred-delivery surface is two functions:
`defer_publication_locked` (producer half: persistent transit write UNDER G,
no continuation executed) and `take_deferred_publications` (chunked move-out
UNDER G, returns raw record pointers). The `.resume()` of a parked
continuation is executed by the FRONTEND with NO Core lock held — in this
campaign, the drain helpers (`drain_all`) in the slice tests, which take the
chunk first and resume outside the G scope. FE-3 added NO new take or resume
site and NO new lock held across a discharge (grep-audited: the only G-guarded
regions in the seam TU end before any `resume()`; the production TU contains
none). The mixing/slice cases assert delivery exactly-once through the
`try_consume` guard (armed-once, consumed-once).

## 5. Control-plane allocation audit

Deferred admission adds ZERO control-plane allocation: the epoch storage
(`WaitNode`, `RwWaitCtx`/`QueueWaitCtx`, `FeDeferredRecord`, lease/out) is
COROUTINE-FRAME-EMBEDDED — allocated once at coroutine creation, before any
admission (the FE-1a address-stability property under test). The admission
path itself allocates only where the fiber path already did (timed:
`prepare_ordinary_deadline_locked`'s R2-ALLOC reserve, unchanged). The
deferred transit list is a construction-bounded, reusable-node pool bounded
by CONCURRENT suspended deferred waiters (FE-2 Gate 2), not by cumulative
submissions.

## 6. Formal-model statement (§17)

No TLA+/GenMC model in `spec/tla/` currently models the deferred
frontend-publication transit protocol. Per §17 this is recorded as a
JUSTIFIED COVERAGE GAP rather than silently skipped: the load-bearing
properties (no lost wake across the admission CS — L7; exactly-once
transit commit + consume; no user code under locks — L9) are pinned by the
deterministic FE-2 boundary cases (§21-B/C phase-controlled set-across-
admission), the FE-3 slice/mixing exactly-once counters, and the M1/M4
mutations (dropping the transit commit or the arm fails the suites loudly).
The smallest protocol worth modeling later is the transit
produce/take/consume lifecycle with the teardown gate — a follow-up trigger,
not a blocker for this campaign's claim (which is authority SHARING, not a
new protocol).

## 7. FE-4 adversarial review round (recorded outcomes)

Two independent adversarial reviews (A: concurrency; B: architecture/falsify-
the-census) returned REQUEST-CHANGES with converging findings; dispositions:

| Finding | Disposition |
|---|---|
| A1/B3: queue seam's hand-rolled F.4 entry interval is not exception-safe against the timed ladder's MAY-THROW prepare (a throw would strand `begin_teardown`); text drifts from QueuePort's CallGuard | FIXED: exception-safe interval close (catch/decrement/rethrow) + NOTE-DRIFT-COUPLING cross-reference at both cores |
| A2: discharge can legally race the admitting thread's await_suspend tail (P2426 class); no rule forbade it | REGISTERED: DIV-17 (v1 discipline: discharge only from the arming thread after the suspend tail; production frontend needs symmetric transfer or a suspend-ack gate) |
| A3/B1: `mutex_cancel`/`sem_cancel` still published via `node.fiber()` (the exact hazard condition_cancel was migrated away from); audit row #5 overclaimed closure | FIXED + REGISTERED: both cancel tails migrated onto the winner-kind tail (behavior-equal for fibers); `mutex_handoff_one_locked`'s owner commit registered as the staged DIV-18 residue with the Mutex-identity slice as the trigger; audit row #5 corrected |
| A4 [P3]: event ladder's inline-SET CAS-loss posture is silent while rwlock's equivalent is a Category B fail-fast | RECORDED here as an inconsistency note; the path is unreachable under the admission CS; alignment is queued with the next touch of that ladder, not repaired out-of-scope in this campaign |
| A5 [P3]: deferred rwlock awaiters never cleared `node.user()` (set/clear asymmetry vs fiber entries) | FIXED: `await_resume` clears `node.user()` in all rwlock slice/mixing awaiters |
| B2 [P3]: the delivery kind-switch has five textual sites; count was implicit | FIXED: §2b enumerates the sanctioned copies (checkable census) |
| B4 [P3]: audit cited an uncommitted test case | RESOLVED by commit ordering (the M5 coverage case + this audit land in the same FE-4 commit) |

Both reviewers explicitly verified (with runs): no P1 correctness finding;
arming windows closed; exactly-once holds; lock order holds (G → S → one
role mutex; never two queue mutexes; nothing holds G across a resume); the
ActorId/WaitResume separation is collision-free; no over-abstraction
(no std::function/virtual/coroutine_handle/registry); mechanical +
assert-hygiene gates pass; Reviewer A additionally ran the full Debug suite
(198/198) and TSan on all four FE-3 targets (clean).
