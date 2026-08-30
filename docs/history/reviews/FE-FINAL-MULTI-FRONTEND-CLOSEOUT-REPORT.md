# FE — Multi-Frontend Semantic Reuse Campaign: FINAL CLOSEOUT REPORT

# VERDICT

**FE COMPLETE — ONE SEMANTIC CORE, REPRESENTATIVE SECOND FRONTEND PROVEN,
PUBLIC COROUTINE FRONTEND/API DEFERRED.** (Option A of the campaign closeout
contract. FE-4 verdict: **PASS WITH STAGED FRONTEND LIMITATIONS** — see the
DIV-17/DIV-18 sections; no P0/P1.)

Status: **CAMPAIGN COMPLETE** (FE-1 → FE-5), branch `feat/frontend-semantic-reuse`,
Draft PR #243 (kept Draft; NOT merged; NOT marked ready; issue #227 NOT
updated, per campaign contract). This report is UNTRACKED by campaign rule
(a human-review artifact; no tracked document references it — verified by
`scripts/check-doc-links.py` PASS at the final tree).

# BASE / HEAD / PR

| Item | Value |
|---|---|
| Campaign BASE | `5706a6d` (PR #242 merge, master) |
| FE-CORRECTIVE-1 base | `0085626` (docs archival round) |
| Semantic/code-audited HEAD | `e8d6e32` (`feat/frontend-semantic-reuse`) |
| Final PR/evidence HEAD | `19c1bde` (docs-only evidence commit: corrective mutations C1–C4; human review ID 5059658754 approved at this SHA) |
| PR #243 | OPEN / **DRAFT** / MERGEABLE, base `master` @ `4bee61f` |
| Merge state | NOT merged, NOT ready-marked; human final adversarial review is the next gate |

All facts in this report were re-verified against the CURRENT tree at
`e8d6e32` on 2026-08-30; the pre-corrective version of this report
(198/198, evidence ending at `265434b`) is superseded.

# FE CAMPAIGN PURPOSE

Answer exactly one question: does PR #243 demonstrate ONE semantic Core
consumed by TWO execution frontends without duplicating correctness
authority? FE-4 tried to falsify that claim; FE-5 closes the campaign only
if the falsification fails. It is NOT a coroutine-feature campaign (no
public co_await, no Mutex/Semaphore frontends, no sender/receiver, no
scheduler/planner additions — all honored, see SCOPE CONTROL).

# FE-1a

PASS WITH REPRESENTATION COUPLING. Ten findings audited on the frontend-
neutral contract; semantics reusable; **no F3 found**. Re-audited on the
FE-3 tree (equivalence audit §3): rows #2/#5/#7 partially RESOLVED by
FE-2/FE-3/FE-4 (WaitResume token; winner-tail publication incl. the
mutex/semaphore cancel tails; RwLock ActorId), remaining open items are the
declared Mutex-identity slice (DIV-18/row #7) — F1-open, tracked, not F3.

# FE-1b

Frontend-neutral contract FROZEN
(`docs/history/reviews/FE-1B-FRONTEND-NEUTRAL-CONTRACT-FREEZE.md`, archived
to the repo at `0085626`): laws L1–L13 + adversarial corrections A1–A4
(ActorIdentity ≠ ResumeTarget; eligibility commit ≠ physical suspension;
publication commit ≠ continuation execution; binding visibility semantic).

# FE-1c

ONE NARROW TYPE/DELIVERY SEAM EARNED (strategy A: tagged POD token;
`docs/history/reviews/FE-1C-TYPE-IDENTITY-DELIVERY-SEAM-DESIGN.md`,
archived `0085626`).

# FE-2

Minimal stackless Event PoV: `WaitResume {void*, Kind}` registration token
(+8 B/node, DIV-15), the ONE `publish_wait_winner_locked` kind tail, the
deferred-publication transit split (`defer_publication_locked` /
`take_deferred_publications`), the shared `event_wait_admit_locked` ladder,
and the §21 race matrix (A–F) + §22 no-user-code-under-lock witness.

# FE-3

Representative slices over the SAME production laws: Queue
(push/pop ladders + grant tail + `active_port_calls_` entry protocol),
RwLock (read/write ladders + ownership cores; public `ActorId`), Condition
(shared register-before-handoff ladder; bare-WaitQueue presentation),
cross-frontend mixing (one resolver, mixed Fiber+deferred waiter sets).
ActorIdentity separated from ResumeTarget (mechanically: separate classes;
kind-tagged `ActorId` equality; ownership sites never inspect WaitResume).

# FE-CORRECTIVE-1

PASS — THREE P1 BLOCKERS CLOSED (commits `e57c16e`/`f53d7ac`/`c5cb90d`,
plus P2 `0cc738b` and docs `e8d6e32`), each with a pre-fix reproduction and
a sensitive witness:

- **P1-1** deferred-publication storage failure: `defer_publication_locked`
  now `noexcept`; the insertion's failure enters the named process-terminal
  fail-fast (`scheduler_deferred_publication_stranded_fail_fast`, exit 86,
  Debug AND Release — PUB1). Honest posture: transit storage MAY allocate;
  this is a terminal boundary, NOT an allocation-free tail.
- **P1-2** deferred Queue port lifetime: the `active_port_calls_` pin is
  TRANSFERRED to the awaiting frame on every non-throw return and released
  in `await_resume` AFTER result conversion (`queue_release_deferred_pin_
  for_test`, over-release fail-fast); `begin_teardown` cannot pass inside
  the window (QD1 fail-fast; QPIN-1/2 phase witnesses). Both `release_popped`
  and `release_failed` paths covered (QPIN-1/QPIN-2).
- **P1-3** RwLock ActorId synchronization: the recursive-owner check moved
  INSIDE `rwlock_write_admit_locked` (under G) for BOTH frontends; test
  observers take G; death child RW + two-worker turnover stress (TSan
  witness). Full-tree census: every `writer_owner` access is under G or in
  construction/destruction phases (ctor init at `async_rwlock.hpp:79`,
  G-holding death-seam helpers).
- **P2** `WaitResume::fiber(nullptr)` normalizes to `Kind::none` at the
  single construction point (static-assert witness; tails never see a null
  fiber-kind).

The FE-4 final audit (below) re-verified all three closures in the current
tree and did NOT reopen them.

# FINAL SEMANTIC AUTHORITY MAP

Per correctness fact: the ONE semantic owner, and both frontends' relation
to it (full per-primitive matrix: `docs/architecture/fe3-multi-frontend-
equivalence-audit.md` §1).

| Fact | Semantic owner (ONE textual law) | Fiber representation | Deferred representation | Shared authority? | Status |
|---|---|---|---|---|---|
| Wait-epoch registration | `WaitQueue::register_wait_locked` (G + queue mtx) | `WaitResume::fiber` token | `WaitResume::deferred` token | YES | shared |
| Terminal winner | `WaitNode::resolve_` CAS | same | same | YES | shared |
| Terminal outcome | `WaitOutcome` woken/cancelled/expired | same | same | YES | shared |
| Unlink / accounting | winner claim primitive (`rwlock_claim_node_woken_locked` family; ladder tails) | same functions | same functions | YES | shared |
| Deadline prepare/publish/consume/retire | `prepare_/publish_/consume_ordinary_deadline_locked` (AC-2b) + `retire_timer_for_node_locked` | same | same | YES | shared |
| Cancellation | `cancel_primitive_wait_locked` (membership gate → CANCEL CAS → retire) + primitive cancels | same | same | YES | shared |
| Admission precedence | per-primitive `*_admit_locked` ladders (one per direction/mode) | thin entries | thin seam entries | YES | shared |
| Admission rejection / inline law | same ladders (L6: inline publishes nothing) | same | same | YES | shared |
| Publication eligibility (L7) | fiber: `commit_suspend_locked` in the admission CS; deferred: `record.arm()` in the SAME CS | stackful commit | frame-record arm | YES (law), pair (FD2) | shared law, sanctioned pair |
| Publication commit (LAST, L8) | `publish_wait_winner_locked` + the four enumerated return-coupled sites | fiber branch | `defer_publication_locked` | YES (kind tail) | shared |
| Publication delivery / resume (L9) | fiber: `make_runnable` + worker route (scheduler state, no user code); deferred: transit take → consume-CAS → `resume()` with NO lock held | worker routing | drain-discharge | mechanism pair (FD2) | shared law, sanctioned mechanism |
| ActorIdentity | `ActorId` (kind-tagged, separate from WaitResume) | `ActorId::fiber` | `ActorId::frontend` | YES | shared (RwLock); Mutex staged (DIV-18) |
| ResumeTarget | `WaitResume` | fiber ptr | `FeDeferredRecord*` | YES | shared |
| Primitive resource commit/reconciliation | Queue Q-LIV-1 grant; RwLock head-prefix claim; Mutex handoff (staged) | same functions | same functions | YES (Queue/RwLock); Mutex staged | shared |
| Queue lifetime pin | `active_port_calls_` + `begin_teardown` (G+S) | CallGuard on suspended fiber stack | transferred pin released in `await_resume` | YES | shared |
| Teardown balance | `begin_teardown` preconditions + `~Scheduler` transit-empty gate + mixing teardown counters | same gates | same gates | YES | shared |

**SECOND SEMANTIC AUTHORITY: NO.** The only place the delivery law is
textually instantiated more than once is the five sanctioned kind-switch
sites (below); no admission, winner, deadline, cancel, or ownership law has
a second copy.

# F0 / F1 / F2 / F3 FINAL CENSUS

Re-audited at `e8d6e32` (equivalence audit §3 + final mechanical census of
`Fiber*`/`WaitResume`/`ActorId`/`node.fiber()`/`resume()`/`coroutine_handle`
/`publish_wait_winner_locked`/`deferred_publications_`/`active_port_calls_`
/`granted_not_resumed_`/`writer_owner`/`commit_suspend_locked`/
`route_runnable_locked` across `src/async` + `include/sluice/async`):

- **F0** (naming/comment leakage): one recorded P3 note — the event ladder's
  inline-SET CAS-loss posture is silent where rwlock's equivalent fail-fasts
  (unreachable path; audit §7 A4; queued for the ladder's next touch).
- **F1** (representation coupling, semantic authority reusable): the
  sanctioned token/record typings (`WaitResume`, `ActorId`, `FeDeferredRecord`,
  frame-embedded ctx structs) + the ONE open staged residue: Mutex owner is
  still `Fiber*`-typed (`mutex_handoff_one_locked` owner commit — DIV-18,
  the declared Mutex-identity slice). No deferred waiter can reach it (see
  DIV-18).
- **F2** (mechanism coupling requiring a narrow seam): zero OPEN. The
  FE-1a-era direct-fiber cancel/expire publication tails are all migrated
  (condition FE-3; mutex/semaphore FE-4 review round).
- **F3** (second frontend re-derives correctness law): **ZERO.** Every
  deferred entry is a thin CS wrapper that CALLS the production ladder; the
  431-line seam TU contains no admission sequence of its own; M1–M5
  mutations prove the suites fail when the shared authorities break.

# FD0 / FD1 / FD2 / FD3 / FD4 FINAL CENSUS

(equivalence audit §2; re-checked.)

- **FD0**: none material (identical law + shared function).
- **FD1** (representation typing): sanctioned entries — WaitResume/ActorId/
  FeDeferredRecord/frame ctx.
- **FD2** (suspension/delivery mechanism): exactly two sanctioned clusters —
  (a) eligibility-commit pair (`commit_suspend_locked` vs `record.arm()`),
  (b) delivery mechanism pair (worker routing vs transit take/consume/
  resume), instantiated at the FIVE sanctioned kind-switch sites:
  `publish_wait_winner_locked`, `queue_publish_winner_locked`, `cancel_wait`
  (park_wake), `expire_wait` (timer), `pump_deadlines_locked`. Final census
  re-counted at `e8d6e32`: **exactly five sites, no sixth.**
- **FD3** (frontend API/ergonomics): fiber = public API; deferred =
  internal-testing seams only (DIV-16). Declared, not hidden.
- **FD4** (duplicated semantic authority): **NONE** — structural argument
  (seam TU delegates every law) + falsification attempt failed + mutation
  sensitivity.

# EVENT EQUIVALENCE

Fiber and deferred Event paths share `event_wait_admit_locked` (ONE
function): same registration law, same set-first/readiness precedence, same
terminal CAS (`resolve_`), same cancel closure
(`event_cancel_deferred_for_test` over the same closure), same ordinary
deadline authority (LOCAL publish inside the admission CS), same
inline-no-publication law (L6), same winner-before-publication ordering
(publication commit LAST). Dispositions traced in code, not just test names:
already-set (inline, no publication — §21-A), set during the admission
boundary (deterministic phase seam, exactly-once — §21-B/C), set after
suspension (async publication — §21-C family), cancel (§21-D), already-due
deadline (§21-E), deadline after suspension (§21-F).

# QUEUE EQUIVALENCE

Both frontends share `queue_push_admit_locked` / `queue_pop_admit_locked`,
the terminal winner, Q-LIV-1 reconciliation
(`queue_grant_consumer_locked` / `queue_grant_producer_locked` — including
the fiber-blocking-entry grant coverage M5 forced into the suite), Queue
FIFO policy, ring/lease authority, close dispositions, deadline retirement,
and cancellation. **Queue lifetime**: the deferred winner's port obligation
is the TRANSFERRED ordinary-call pin (`active_port_calls_` == 1 through
admission → suspension → terminal winner → publication pending →
continuation resume → port-dependent result conversion; released only
afterward, G+S, over-release fail-fast). Both `release_popped` (QPIN-1) and
`release_failed` (QPIN-2) paths verified. `begin_teardown` cannot pass in
the window (QD1). **The Scheduler deferred transit list does NOT own
QueuePort lifetime — NO.** (It owns only the delivery obligation; the
teardown gates are the transferred pin first, the Scheduler transit-empty
gate second.) FE-CORRECTIVE-1 P1-2 remains CLOSED at `e8d6e32` (verified:
transfer at both cores, catch-release on throw, awaiter-side release seam).

# RWLOCK EQUIVALENCE

`ActorIdentity ≠ ResumeTarget` is mechanically real: separate classes
(`ActorId` at `wait_node.hpp:139`; `WaitResume` above it); `ActorId`
equality includes the kind tag (a `Fiber*` can never equal a frontend token
by address coincidence); the class comment forbids coroutine_handle
identity. Ownership decisions — inline write claim, queued writer grant
(`rwlock_grant_from_head_locked`), try-write, unlock-write, cancel, expiry,
reader-prefix grant — all read/commit `ActorId` and never `WaitResume`,
a coroutine handle, or a delivery-record identity. FE-CORRECTIVE-1 P1-3
remains CLOSED: full-tree census shows every `writer_owner` access under G
(ladders, status seams) or in non-concurrent construction; the test
observers take G. Fairness/grant/cancel/deadline proven per frontend and
mixed (batch grant).

# CONDITION EQUIVALENCE AND LIMIT

What is proven: the frontend-neutral Condition wait-admission / notify /
deadline law — `condition_wait_admit_locked` (register-before-handoff
single-G CS), `ConditionAdmitDisposition` encoding the released_mutex law,
notify/notify_all/cancel/expiry through winner-kind tails, due-inline
retention, pump expiry, cancel loser-exactly-once — for BOTH frontends,
plus the unchanged fiber suites over the SAME ladder covering the full
AsyncCondition choreography.

What is NOT proven: a full stackless AsyncCondition + stackless-owned
AsyncMutex frontend. The staged design presents BARE WaitQueues (empty
bound Mutex queue ⇒ documented UnlockNoWaiter no-op; sentinel/fake owner
slot). Per the campaign contract this is recorded honestly: **Condition
semantic admission contract proven frontend-neutral; full stackless
AsyncMutex composition deferred** (the Mutex-identity slice, DIV-18
trigger). No production-coroutine-Condition claim is made.

# CROSS-FRONTEND EVIDENCE

| Test | Classification |
|---|---|
| `fe3_mix_event_set_resolves_fiber_and_deferred` | TRUE mixed wait-set (ONE `set()` resolves a parked fiber waiter and a parked deferred waiter; each delivered exactly once through its own kind) |
| `fe3_mix_rwlock_batch_grants_fiber_and_deferred_readers` | TRUE mixed wait-set (ONE `unlock_write` batch-grants a fiber reader + a deferred reader as ONE reader prefix; `run_live(2)`) |
| `fe3_q_cross_fiber_waiter_coroutine_resolver` | cross-frontend resolver evidence (deferred resolver → fiber waiter) |
| `fe3_q_cross_coroutine_waiter_fiber_resolver` | cross-frontend resolver evidence (fiber resolver → deferred waiter) |
| `fe3_q_fiber_blocking_pop_grants_deferred_producer` | cross-frontend resolver evidence (fiber BLOCKING admission entry as resolver; added by M5 coverage repair) |

Limitations: mixing is demonstrated on Event + RwLock + Queue directions;
no mixing claim is made for Condition (bare-queue presentation) or for
Mutex/Semaphore (DIV-18). Separate per-frontend smoke tests are NOT counted
as mixing evidence anywhere in this campaign's docs.

# PUBLICATION / DELIVERY PROOF

Every reachable publication tail for FE-covered primitives follows:
terminal winner (`resolve_` CAS) → unlink → timer/resource/accounting
closure → publication commit LAST. Fiber: `make_runnable` → worker route
(scheduler state only). Deferred: transit obligation (`defer_publication_
locked`, under G, `noexcept`) → `take_deferred_publications` (G-scoped
chunk move) → consume-CAS → `resume()` with NO lock held.

Resume-site census (mechanical, `e8d6e32`): production TUs contain ZERO
coroutine `resume()` (the `WaitResume::resume()` accessor name aside — a
token getter). Every continuation `.resume()` lives in the test drain
helpers (`drain_all` in the four slice/mixing tests + PoV), each: take
chunk under G via `AsyncTestAccess`, resume AFTER the G scope, guarded by
`try_consume` (unarmed consume / double consume = loud failure). The §22
witness proves no user code under G deterministically. Fiber
`make_runnable`/`route_runnable_locked` touch scheduler state only — no
user continuation under authoritative locks, either frontend.

# QUEUE LIFETIME PROOF

See QUEUE EQUIVALENCE. Mechanically: `++active_port_calls_` at the F.4
entry → production ladder under G+S+role → catch-release+rethrow on throw →
**no release on non-throw return (transfer)** → awaiter releases via
`queue_release_deferred_pin_for_test` (G+S, over-release fail-fast) in
`await_resume` AFTER `release_popped`/`release_failed`. QPIN-1/QPIN-2
witness the phase-by-phase accounting table; QD1 fail-fasts
`begin_teardown` inside the window; the winner-tail comment now states the
transferred-pin obligation (the pre-corrective "discharge never touches the
port" claim was corrected in `e8d6e32`).

# ACTOR IDENTITY PROOF

See RWLOCK EQUIVALENCE. Slice case `same_actor_different_resume_target`
proves ownership follows the ACTOR across differing ResumeTargets;
`granted_by_fiber_unlock` proves cross-frontend grant; death child RW +
turnover stress prove the synchronization. The FE-CORRECTIVE-1 P1-3 closure
is re-verified at `e8d6e32`.

# DEADLINE / CANCEL REUSE

ONE ordinary deadline authority (AC-2b prepare/publish/consume/retire) and
ONE cancellation closure (`cancel_primitive_wait_locked` + retire) serve
both frontends; FE introduced no second timer or cancel authority. M1–M5 +
C1–C4 mutation sensitivity covers the shared sites. FE changed
REPRESENTATION and DELIVERY only — no semantic transition changed (formal
binding below).

# LIFETIME / FRAME DISCIPLINE

`FeTask` (test-local): `initial_suspend`/`final_suspend` both
`suspend_always`; frame destruction owned by the `FeTask` RAII owner
(`h_.destroy()` in the dtor); no allocation customization, no exception
propagation. A. Frame destruction while WaitNode Registered: not reachable
under the tests' discipline (every case resolves and drains before scope
exit; death children `_Exit` in forked children rather than destroy
frames). B. While Queue pin held: no (pin released in `await_resume`
before the awaiter/frame can complete). C. After terminal winner but
before discharge: the frame MAY be in that window only while the case
deliberately withholds discharge (QD1 child) — in a forked process, never
destroying the frame. D. `final_suspend` = `suspend_always` (an early
resume cannot destroy the frame). E. Frame destruction ownership: the
`FeTask` owner, strictly scoped per case.

Classification: **strict test-only lifetime discipline, mechanically
followed by every current test — NOT a production-safe general coroutine
frontend.** Production framing (scopes, structured lifetime, symmetric
transfer) is future work (DIV-17 revisit trigger).

# FAILURE ATOMICITY

FE-CORRECTIVE-1 P1-1 posture verified at `e8d6e32`: `defer_publication_
locked` declared `noexcept` (`scheduler.hpp:1541`); the ONLY throwing op on
the path (vector growth) is wrapped in try/catch whose handler is the named
process-terminal fail-fast (exit 86, Debug AND Release); the one-shot
injection is `SLUICE_ASYNC_INTERNAL_TESTING`-gated at exactly the insertion
edge; PUB1 (injection → exit 86) + PUBCTL (healthy: depth 1, woken node,
exactly-once take/consume, teardown gate passes) green in Debug, Release,
TSan, ASan+UBSan full suites. No path lets the allocation exception escape
after the winner commit. **P1-1 remains CLOSED.** Honest wording enforced:
"deferred publication transit storage may grow dynamically; failure after a
committed terminal winner is a named process-terminal boundary" — never
"allocation-free publication tail".

# DIV-17

RE-ADJUDICATED: **ACCEPTED / STAGED, unchanged.** The resume-before-
await_suspend-return window is a real standard-level hazard (cppreference,
`coroutine_handle::resume`: "Behavior is undefined if *this does not refer
to a suspended coroutine… A concurrent resumption of the coroutine may
result in a data race"; a handle may legally be resumed before
`await_suspend` returns given suitable synchronization). The v1 discipline
— discharge only from the arming thread, after the `await_suspend` tail —
is mechanically true of EVERY current discharge site: the only take entry
is `AsyncTestAccess::take_deferred_for_test`, called exclusively by the
single-threaded `drain_all` helpers inside case bodies on the arming
thread; no foreign thread discharges in any current test. Statement
required by the campaign: **a production concurrent-drain frontend is NOT
yet proven**; it must adopt symmetric transfer or a suspend-ack gate
(registry revisit trigger). No code change made (the PoV obeys its
declared scope).

# DIV-18

RE-ADJUDICATED: **staged, unchanged, honestly scoped.** Verified at
`e8d6e32`: the deferred admission surface is EXACTLY
Event/Queue/RwLock/Condition — no deferred Mutex or Semaphore entry exists
in `AsyncTestAccess`, so under the current internal frontend construction
NO deferred waiter can reach the `mutex_handoff_one_locked` Fiber-typed
owner commit; the Condition PoV presents bare queues. Closeout wording
(used verbatim in WHAT FE PROVED): FE proves multiple representative
primitive classes share one Core; it does NOT claim every primitive has a
complete stackless frontend. No Mutex frontend was implemented to close
this divergence.

# ABI / LAYOUT

Mechanically measured this session (clang++ -std=c++20, same probe, same
compiler, base = `origin/master` `4bee61f` via a removed temp worktree):

| Type | BASE | FE HEAD `e8d6e32` | Delta |
|---|---|---|---|
| `sizeof(WaitNode)` / alignof | 48 / 8 | **56 / 8** | +8 B (DIV-15 ✓) |
| `sizeof(WaitResume)` | — (absent) | **16 / 8** | new public type |
| `sizeof(AsyncRwLock)` / alignof | 120 / 8 | **128 / 8** | +8 B (DIV-19 ✓, exactly as registered) |

DIV-15/DIV-19 registry values match reality. These are INSTALLED-HEADER
(public) layout changes: the campaign does NOT pretend "test-only frontend
= zero public ABI change". FE-CORRECTIVE-1 itself adds zero layout delta
(`noexcept` + one ternary).

# PERFORMANCE SHAPE

Inspection only, no benchmark campaign, no performance claim (§16.7/§24).

Stackful (production) hot-path tax: no new allocation; no new atomic; no
new lock (transit list lives under existing `global_mtx_`); no virtual
dispatch and no `std::function` anywhere in the added surface (grep: 0);
one kind compare per winner tail (branch); `WaitNode` +8 B; `AsyncRwLock`
+8 B. Deferred (internal frontend) mechanism cost: transit vector may
allocate (grow with CONCURRENT suspended deferred waiters, drained per
discharge; failure = named process-terminal boundary); coroutine-frame
allocation belongs to the compiler/frontend; no per-wait heap object from
the token representation; Queue lifetime pin = counter increments in the
existing G+S domain. The stackful tax and the deferred mechanism cost are
reported separately; neither is hidden under "no allocation".

# FORMAL BINDING

FE changed representation, identity typing, and frontend publication
mechanism — NOT semantic transitions. The R2-era formal semantics
(admission, terminal winner, generation, wake, shutdown) apply unchanged
because the state machines and their transition guards are the SAME
functions the models were derived from (the ladders are moved, not
re-designed; `resolve_` remains the single winner CAS). No new semantic
state was introduced that correctness depends on: the transit list is a
DELIVERY mechanism (persistent state under G + explicit drain), not a new
terminal authority, and its lifecycle is recorded as a JUSTIFIED COVERAGE
GAP (equivalence audit §6) pinned by deterministic boundary cases and
M1/C1 mutations. Per campaign rule, no TLA+ was rewritten to show activity.

# MUTATION EVIDENCE

M1–M5 (FE-3/FE-4, `docs/verification/fe3-multi-frontend-mutation-
evidence.md`): all five single-point mutations RED under the FE suites;
M5's first attempt exposed a coverage gap that was repaired
(`fe3_q_fiber_blocking_pop_grants_deferred_producer`, RED→GREEN). Corrective
C1–C4 (FE-CORRECTIVE-1, now recorded in the same tracked doc): publication
storage-failure escape (child exit 88 vs containment exit 86 Debug AND
Release), Queue pin early release (QD1 exit 87 vs in-window fail-fast),
RwLock pre-G owner read (TSan race report vs G-serialized), null fiber-kind
construction (compile-rejected by static-assert). Provenance recorded: C1–C4
were executed and reverted during the corrective round; their witnesses
(PUB1/PUBCTL, QPIN-1/2, QD1, RW, static-assert) remain green in the current
full suites. No mutation claim exceeds what it proves (bounds stated in the
doc).

# TEST-EVIDENCE STRENGTH

| Suite | Invariant proven | Strength | Deterministic? | Timing tolerance? | Production authority exercised? |
|---|---|---|---|---|---|
| fe2 POV §21 A–F | Event admission/inline/async/cancel/deadline equivalence, exactly-once | STRONG | yes (phase seams) | no | yes (shared ladder) |
| fe2 POV §22 | L9 no-user-code-under-lock | STRONG | yes (G probe) | no | yes |
| fe2 publication death (PUB1/PUBCTL) | P1-1 failure atomicity | STRONG (process boundary) | yes | no | yes |
| fe3 queue slice (12) + QPIN/QD1 | Queue law sharing, lifetime pin | STRONG | yes | no | yes |
| fe3 rwlock slice (7) + RW death + turnover stress | ActorId ownership, sync | STRONG (+ TSan supplementary) | yes | stress supplements | yes |
| fe3 condition slice (5) | Condition law sharing, staged scope | STRONG | yes | no | yes |
| fe3 mixing (2) | joint cross-frontend resolution | MEDIUM-STRONG (semantic assertions deterministic; `waiting_count()` used ONLY as bounded liveness settle) | yes | settle only | yes |
| Queue/RwLock worker mixes | teardown balance under real workers | MEDIUM (cross-domain settle) | yes | bounded settle | yes |

No test claims happens-before/FIFO/absence-of-race from `bounded_wait`,
yields, or stress loops; those appear only as liveness/race-exposure
supplements, per §18/§21.

# STACKFUL REGRESSION

Full Debug/Release/TSan/ASan+UBSan suites (199/199 each, zero sanitizer
reports) run over the SAME stackful suites that predate the campaign — the
fiber entries are thin entries over moved, byte-preserved ladders. Claim
precision: **valid-call stackful semantics preserved.** Some INVALID-call
behavior intentionally changed earlier in the campaign (reachable
caller-contract asserts → named Release-active fail-fasts: recursive write,
unlock-write owner checks; plus the FE-CORRECTIVE-1 fail-fasts), so
"bit-identical behavior" is NOT claimed.

# ADVERSARIAL REVIEW A

(concurrency / lifetime / failure atomicity — findings and dispositions)

Tooling note: the campaign's two independent-subagent passes could not be
dispatched at closeout (API usage cap; reset later the same night). Both
passes were therefore executed by the primary auditor as two separate,
sequentially-focused reviews with independent checklists; this is recorded
honestly as a tooling downgrade from the "independent reviewers" ideal.

Pass A traced at `e8d6e32`: L7 arm-inside-admission-CS for all four
primitives (queue/rwlock/condition cores read directly; event ladder via
the PoV + M4 sensitivity); single terminal winner + subordinate
`try_consume` exactly-once; zero production coroutine-resume sites and all
test discharges outside authoritative locks; Queue pin transfer + QD1/QPIN
witnesses; `defer_publication_locked` noexcept containment + dtor gate;
cancel/expire/pump all on kind-aware tails; FeTask frame discipline.
**Verdict: APPROVE — P0: 0, P1: 0.** P2 recorded: taken-but-never-resumed
transit records leave no Scheduler-visible residue (discharge ownership
after take belongs to the frontend drain caller — a STATED discipline under
DIV-17 v1; unreachable by a foreign thread in the current single-threaded
drain surface). P3 unchanged (event-ladder CAS-loss posture note).

# ADVERSARIAL REVIEW B

(architecture / authority duplication / over-claim)

Pass B re-derived the authority map from code (not docs): every seam entry
delegates to a production `*_locked` law; the kind-switch census found
EXACTLY the five sanctioned sites and no sixth; `ActorId` equality is
kind-tagged and no ownership site inspects `WaitResume`; DIV-17's v1
discipline holds at every take site; DIV-18's surface absence verified (no
deferred Mutex/Semaphore admission exists); DIV-19's 128 B matches the
probe; no tracked document claims a production/public coroutine frontend,
"two production frontends", or "zero-allocation publication" (the PR body's
stale top section was corrected in FE-5). **Verdict: APPROVE — P0: 0, P1: 0,
F3: 0, FD4: 0.** P2 recorded: PR body was stale (fixed this round);
mutation doc lacked the corrective rows (fixed this round). Reconciliation
between passes: no disagreement to average.

# WHAT FE PROVED

1. ONE semantic Core: every admission, terminal-winner, deadline,
   cancellation, ownership, reconciliation, publication-eligibility, and
   teardown law is a single production function consumed by BOTH frontends
   (F3 = 0; FD4 = NONE; five sanctioned kind-switch sites only).
2. Event: full frontend-neutral wait semantics demonstrated (admission,
   inline law, async publication, cancel, ordinary deadline).
3. Queue: resource/FIFO/close/cancel/deadline/reconcile laws shared, plus
   QueuePort lifetime (transferred pin) demonstrated for both frontends.
4. RwLock: ActorIdentity separated from ResumeTarget and G-serialized;
   fairness/grant/cancel/deadline shared.
5. Condition: frontend-neutral wait-admission/notify/deadline choreography
   demonstrated (full stackless AsyncMutex composition staged).
6. Cross-frontend: representative one-resolver mixed wait-sets and
   cross-resolver pairs demonstrated (Event, RwLock, Queue directions).
7. Failure atomicity: the deferred publication failure boundary is
   terminal-safe and witness-verified in Debug AND Release.
8. The stackful frontend's valid-call semantics are preserved through the
   refactor (199/199 across four sanitizer configurations).

# WHAT FE DID NOT PROVE

1. A production coroutine drain architecture (DIV-16; drain points are
   internal-testing seams).
2. Arbitrary cross-thread coroutine discharge (DIV-17 v1 single-thread
   discipline; symmetric transfer / suspend-ack future work).
3. A public coroutine API or public Task type (none added).
4. Generic executor / sender-receiver integration.
5. Complete stackless Mutex ownership (DIV-18 staged) — hence no full
   stackless AsyncCondition either.
6. Every primitive × frontend Cartesian product.
7. Performance superiority of either frontend (no benchmark claim made).
8. Production-safe general coroutine frame lifetime management (test-only
   RAII discipline).

# PUBLIC API DECISION

**DEFERRED** (effectively INTERNAL as staged): the stackless frontend
exists only behind `Scheduler::AsyncTestAccess` in the
`sluice_async_internal_testing` target (DIV-16). Public additions are
exactly the earned representation types (`WaitResume`, `ActorId`) plus the
disposition enums — no co_await surface, no Task, no executor. Rationale:
the second frontend is architecture evidence, not a stabilized contract;
spending public-API/ABI stability now would be unearned. No public API was
added in FE-5.

# REMAINING RISKS

1. DIV-17: a future concurrent drain driver that ignores the v1 discipline
   re-opens the P2426-class hazard (named, registry-gated).
2. DIV-18: until the Mutex-identity slice, any future seam that admits a
   deferred epoch onto a Mutex/Semaphore queue hits the Fiber-typed owner
   commit (surface-absence enforced today; a new seam must re-check).
3. Transit-list take→consume after take is a stated frontend discipline,
   not a Core-enforced gate (Scheduler-scoped gate covers only queued
   entries) — acceptable under the staged single-thread drain; revisit with
   DIV-17.
4. `select_event_registry_test` TSan flake: pre-existing, diagnosed,
   OUTSIDE this branch's paths (compliance-gate addendum); repair tracked
   as a follow-up.
5. Formal coverage of the transit lifecycle is a recorded gap (pinned by
   mutations; smallest-protocol model is a follow-up trigger).

# VALIDATION

All commands executed THIS session at `e8d6e32` (clean tree; actual counts,
not copied):

| Gate | Result |
|---|---|
| Clang Debug full (`xmake f -m debug --toolchain=clang`; build core/async/test; `xmake test -v`) | **199/199 PASSED** |
| Clang Release full (same shape, `-m release`) | **199/199 PASSED** |
| TSan full (`-m tsan`; `xmake run -g test`) | ALL PASSED, exit 0, **0** ThreadSanitizer reports |
| ASan+UBSan full (`-m asanubsan`) | ALL PASSED, exit 0, **0** reports |
| Focused: fe2_pov, fe2_publication_atomicity_death, fe3_queue_slice, fe3_rwlock_slice, fe3_condition_slice, fe3_cross_frontend_mixing | all PASS (inside the 199) |
| mechanical-facts / doc-links / architecture-docs / assert-hygiene / claim-hygiene / failure-envelope | OK / PASS (incl. failure-envelope self-test 15 planted violations) |
| `git diff --check` | clean |
| `bash scripts/gates/pre-push.sh` | **ALL CHECKS PASSED** |
| CI | GitHub Actions Clang Debug + Clang Release green at `e8d6e32`; re-verified on the final docs-commit HEAD after push (below) |

SKIPPED (with reasons): Valgrind (ASan/UBSan covers this campaign's
ownership class; no open leak question); real-liburing (no io_uring surface
touched; stub-default builds unchanged); TLA+/GenMC re-run (no modeled
transition changed — formal binding above).

# FE COMPLETION CHECKLIST

[x] no P0 / [x] no P1 · [x] F3 = 0 · [x] FD4 = 0 for claimed-equivalent
semantics · [x] one semantic Core · [x] representative second frontend
proven · [x] staged limitations explicit (DIV-16/17/18, Condition limit,
frame discipline) · [x] all gates green · [x] valid-call stackful semantics
preserved · [x] layout/ABI delta recorded (probe) · [x] mutation evidence
sensitive (M1–M5, C1–C4) · [x] publication failure cannot escape after
winner commit · [x] QueuePort lifetime mechanically protected · [x] no
unsynchronized `writer_owner` access · [x] user continuation never runs
under authoritative locks · [x] DIV-17 honestly scoped · [x] DIV-18
honestly scoped · [x] PR body current (rewritten FE-5) · [x] exact final
HEAD CI green (Debug + Release; sanitizer coverage local).

# FINAL NEXT STEP

**HUMAN FINAL ADVERSARIAL REVIEW** of PR #243 (kept Draft). Only after
human acceptance and actual merge may #227/roadmap be updated. Follow-up
queue (tracked in the audit/reports, not hidden): Mutex owner-identity
slice (DIV-18 trigger) → production stackless frontend slice (DIV-17) →
select_event_registry flake repair → event-ladder CAS posture alignment
(P3) → transit-lifecycle formal model.

# SCOPE CONTROL (closeout self-audit)

No Mutex/Semaphore frontend added; no public coroutine API added; no
sender/receiver, std::execution, Task/executor framework, second
Scheduler/WaitNode/timer/cancel authority, runtime planner, DST, seeds,
fuzz scheduler, DPOR, or model checker added; PR #243 NOT merged, NOT
ready-marked; #227 untouched; no history rewritten; the only tracked
change of FE-5 is the closeout/evidence documentation (mutation-evidence
corrective rows); the closeout report itself remains UNTRACKED by its own
campaign rule (no tracked doc references it; doc-links gate PASS at the
final tree confirms no dangling authority).
