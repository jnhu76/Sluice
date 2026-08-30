# R2 WAIT-LIFECYCLE FINAL CLOSEOUT REPORT

Status: **UNTRACKED human-review artifact** (per campaign brief §47; do not
commit without repository authority).

---

## VERDICT

**R2 COMPLETE — NO FURTHER CENTRALIZATION EARNED**

- AC-2d-b entered: **NO** (no S3 authority duplication remains; Stage 3
  skipped per brief §27).
- One correctness/liveness defect was found, repaired, and regression-pinned:
  **Q-LIV-1 FIXED** (Queue blocking/timed admit inline-success reconciliation).
- Adversarial review round 2 (2026-08-28, PR #242 at `ed1f1cd`): REQUEST
  CHANGES — P1 timed-admission allocation atomicity (blocker, in R2 scope,
  fixed by the **R2-ALLOC corrective**: prepare-before-register split of the
  ordinary arming authority; regression-pinned ×4, see KNOWN R2 BUGS section);
  P2 evidence wording (PR-body "no new lock" claim and stale DRAFT banner —
  corrected in the PR body); CodeRabbit review was skipped (manual review
  required), GitHub CI green at run #387. The `R2 COMPLETE` verdict above is
  conditional on this corrective; before it, the honest state was
  `R2 NOT COMPLETE — CORRECTNESS BLOCKER`.

BASE: `048ce23` (master, merge of PR #241; verified MERGED
`f4899cd`, 2026-08-28T04:22:21Z)
BRANCH: `fix/r2-wait-lifecycle-closeout`
HEAD (at report writing): `ed1f1cd` + R2-ALLOC corrective commit (review
round 2)
Commits: `dc89c9b` fix(async) Q-LIV-1 · `575c8cd` docs(post-freeze) LOC sync ·
`ed1f1cd` docs(async) living-comment sync · R2-ALLOC corrective (this round)

---

## R2 SCOPE

Included: wait registration, primitive admission, resource/precedence
decision, timer attachment, terminal resolution, cancellation closure,
timeout closure, unlink, wait-registration retirement, primitive-local
reconciliation, permission to suspend, resume/publication boundary.
Excluded (boundary-inspected only): RequestArena lifecycle, Completion
ownership, task/I/O cancellation, Select redesign, coroutine frontends,
runtime adaptation.

---

## PREVIOUS AUTHORITIES (verified on current master)

| Authority | Anchor (mechanically verified) | Status |
|---|---|---|
| AC-2a wait authority matrix | `docs/history/reviews/AC2A-...REPORT.md` (19-row inventory, e10e181 convention) | historical evidence; row conventions superseded by this report's 18-row ordinary-admission census |
| AC-2b ordinary deadline lifecycle | `arm_ordinary_deadline_locked` (scheduler_timer.cpp:317), `consume_...` (:332), `retire_...` (:338); Queue arming intentionally LOCAL (scheduler_queue.cpp timed admits, AC-2b corrective comment), Queue consume/retire route through the authority | intact; WaitNode terminal-winner authority separate; Select timer sibling untouched |
| AC-2c primitive cancellation closure | `cancel_primitive_wait_locked` (scheduler_park_wake.cpp:1235) shared by event/condition/queue/rwlock/mutex/semaphore cancel seams; outside it: `waiting_waitq_count_` bookkeeping, Fiber publication, queue lease/assoc, RwLock fairness all primitive-local | intact |
| DST-PV-1 minimal driver | script seam guarded by `SLUICE_ASYNC_INTERNAL_TESTING` (scheduler.cpp:531-532 `schedule_script_active/pick`); production target carries no seam symbols (mechanical-facts seam-production-exclusion gate green) | intact; single-worker, manually scripted, bounded, fail-closed; Run(X) + logical clock + cancel + fake I/O + Queue ops; NO expansion made |

---

## Q-LIV-1

### Reproduction (pre-fix, deterministic)

`dst_t5_v1_parked_producer_vs_inline_pop` on unfixed master: capacity-1 ring
pre-filled with item 7 (main-thread `try_push`); P parks (`push` onto full
ring); C's BLOCKING `pop` succeeds inline; P remains `FiberState::waiting` at
drain end; `close()` later resolves P closed. Exact path:

1. P: `queue_push_admit` — G+S+producer.mtx; register; ring full → not
   admissible → `commit_suspend_locked` → parks (producer FIFO head).
2. C: `queue_pop_admit` — G+S+consumer.mtx; register; `!ring_empty && head`
   → inline pop: `out=ring_[head]`, head++, `--ring_count_` (1→0),
   `wake_node_locked(self)` → **missing `queue_grant_producer_locked`**.
3. Ring empty+open; P Registered/parked; `active_wait_associations_`=1,
   `waiting_waitq_count_`=1; P's lease custody in P's stack frame.
4. Only a later unrelated op (`try_pop` FastPopCommit / `close` drain) runs
   `queue_grant_producer_locked` and releases P.

### Defect class

**REAL LIVENESS DEFECT** (not a data race — fully G+S+role serialized;
deterministic). The Queue contract grants a parked producer eligibility when
a blocking pop frees capacity; `try_pop` documents and performs exactly this
reconciliation; no documented reason blocking pop should differ. Close merely
masked it.

### Invariant (derived from implementation/contracts)

> When a Queue operation's success changes ring occupancy such that the
> opposite role's FIFO head becomes eligible, the head is reconciled
> (winner-before-publication grant) before the operation returns — subject to:
> exactly-once terminal winner, role FIFO order, lease ownership, close
> semantics, timer retirement, G → S → exactly-one-role lock order.

### Fix (commit dc89c9b, narrow)

The existing Queue-local grant authority is **reused** at the four missing
inline-success sites (`queue_push_admit`, `queue_pop_admit`,
`queue_push_admit_until`, `queue_pop_admit_until`). Shape: inline commit
stays under G+S+own-role.mtx; the own-role mutex is released (nested scope);
the grant runs under G+S only — byte-for-byte the critical-section shape of
`try_push`/`try_pop` FastPush/FastPopCommit; the admit then RETURNS (the
fiber never suspended; the original bug-hunt found the first attempt fell
through to the suspend `context_switch`, stranding C mid-stack — fixed by an
explicit early return). NOT built: generic reconcile callback, admission
framework, Scheduler-wide change, WaitNode winner-law change, unrelated
fast-path changes.

### Symmetric audit

Symmetric defect CONFIRMED and shares exactly one authority fact:
`queue_push_admit` inline filled the ring without
`queue_grant_consumer_locked` (stranded parked consumer). Repaired in the
same slice (v1s witness). Push/pop, untimed/timed: all four sites identical
shape → same-slice repair is in scope per brief §10. No third variant exists
(close's drain loop already reconciled; fast paths always did).

### DST vectors

- `dst_t5_v1` — flipped witness: parked producer + blocking pop inline →
  P commits from the freed slot; no close required.
- `dst_t5_v1s` — symmetric: parked consumer + blocking push inline →
  C consumes the just-committed item; no close required.
- `dst_t5_v1t` — timed producer reconciled by pop inline;
  `active_queue_timers_==0` (grant-side `retire_timer_for_node_locked`
  exactly once); stale `advance_clock(60)` yields no second terminal.
- `dst_t5_v1ts` — symmetric timed consumer; same exactly-once retirement
  proof.
- Existing v2/v3 ladders (expiry / documented reconciler / close /
  already-due) unchanged and green — they now coexist with the repaired
  inline reconcile.

### Accounting proof (mechanical)

- `active_wait_associations_` / `waiting_waitq_count_`: one decrement per
  registration — self (inline block) and winner (grant seam) are distinct
  nodes; cancel and pump paths untouched.
- `active_queue_timers_`: decremented only via the on-resolve thunk at the
  single ACTIVE→terminal CAS; v1t/v1ts assert zero after grant; stale pump
  finds the registration RETIRED (consume CAS fails, inert).
- `granted_not_resumed_`: incremented only on `make_runnable()==true`
  publication; resumed winner decrements; inline path's self-resolution
  (`wake_node_locked`) publishes no ticket and the early return correctly
  skips the post-resume decrement.
- Strongest mechanical witness: `begin_teardown`'s seven-precondition
  fail-fast (`active_port_calls_/active_wait_associations_/
  active_queue_timers_/granted_not_resumed_ == 0`, both role FIFOs empty,
  ring empty) passes in every DST Queue fixture disposal and in
  `async_queue_*` suites.

---

## AC-2d-a

### Exact admission row count (mechanically enumerated on HEAD)

**18 ordinary primitive wait-admission rows** (brief hypothesis confirmed):

| # | Row | Entry seam | file:line |
|---|---|---|---|
| 1 | generic untimed | `await_wait` | scheduler_park_wake.cpp:1145 |
| 2 | generic timed | `await_wait_deadline` | scheduler_timer.cpp:75 |
| 3/4 | mutex lock / until | `mutex_lock(_until)` | scheduler_mutex.cpp:58/144 |
| 5/6 | semaphore acquire / until | `sem_acquire(_until)` | scheduler_semaphore.cpp:56/139 |
| 7/8 | event wait / until | `await_event_wait(_deadline)` | scheduler_event.cpp:226/307 |
| 9/10 | condition prepare / until | `condition_wait_prepare(_until)` | scheduler_condition.cpp:30/116 |
| 11-14 | queue push / pop / push_until / pop_until | `queue_*_admit(_until)` | scheduler_queue.cpp:45/131/204/313 |
| 15-18 | rwlock read / write / read_until / write_until | `rwlock_*_lock(_until)` | scheduler_rwlock.cpp:221/318/479/571 |

Boundary rows (NOT ordinary-admission; sibling/R3 domains, inspected not
absorbed): ready flag (`await_ready_flag`), Completion size/void waits,
Select resolution, request cancellation.

### Actual ladder (as-built, per shape)

All rows: **entry** (F.4/CallGuard for Queue; wrapper fast-path pre-check for
mutex/sem/rwlock) → **register** (`WaitQueue::register_wait_locked`, sole
registration authority, Detached→Registered FIFO-tail link) → `++waiting_waitq_count_`
→ [timed rows: **arm** — AC-2b authority for 8/10 sites; Queue 2 sites
intentionally local (AC-2b corrective; observable historical interleave via
lock-free earliest-deadline cache); Select sibling] → **resource/closed
precedence** (primitive-local predicate under the admit critical section) →
[timed rows: **already-due** Expired inline via `expire_locked` +
`consume_ordinary_deadline_locked`] → [14 rows: defense-in-depth
`node.is_terminal()` recheck] → **`commit_suspend_locked`** (sole suspend
authority: suspend-switch-pending raise + Running→Waiting under G) → locks
release → `test_phase` seam (test builds) → `fiber_ctx::context_switch`.

Queue-specific (post-Q-LIV-1): inline-commit branch resolves SELF via
`wake_node_locked` (no ticket — fiber still Running), decrements counters,
then — after releasing its own role mutex, under G+S — grants the opposite
role FIFO head via the grant seam, then returns without touching the suspend
switch.

### FINAL-RECHECK CENSUS

14 rows carry `node.is_terminal()` defense-in-depth (mutex :120/:215,
sem :122/:211, event :290/:372, condition :91/:188, generic :1173/timer :137,
rwlock :279/:358/:549/:635). **4 Queue rows: none.**

Classification per Queue row (all four): **PROVEN SAFE UNDER LOCK
SERIALIZATION** — the same G-dominance proof that renders the other 14
rechecks unreachable applies verbatim: every resolver of a Scheduler-
integrated wait requires `global_mtx_` (exhaustive inventory: wake_wait_one,
expire/pump, all seven primitive cancels, `cancel_wait`, event set broadcast,
condition notify, mutex/sem/rwlock unlock reconcile, both Queue grant seams,
close drain, begin_teardown), while each admit holds G continuously from
before registration through `commit_suspend_locked`. The Queue rows'
omission is therefore not a semantic divergence but the absence of an
unreachable defensive branch; adding one would be symmetry-for-symmetry
(BRIEF §20 forbids). Note the rechecks are demonstrably non-load-bearing:
the condition rows' dead branches (:91-93/:188-190) return WITHOUT the undo
their own comments promise — unreachable, recorded as S1 census evidence,
intentionally NOT "fixed" (dead-code cleanup out of campaign scope).

### Lock proof & DST falsification of the serialization argument

Actors enumerated: opposite-role op (needs G), close (G), timeout pump (G),
cancel (G+role), publication grants (G held by callers), inline commit (IS
the admitting thread). Each requires G, which the admitting thread holds
across register→precedence→suspend-commit; the DST phase seam
(`scheduler_suspend_before_physical_switch`) fires only AFTER the lock
block — the window is closed by construction, so no legal DST schedule can
even stage a mid-window terminal. Result: **no counterexample found in
enumerated boundary schedules, consistent with the lock-order/state proof**
(v1/v1s/v1t/v1ts + v2/v3 exercise every adjacent boundary: expiry-vs-pop,
close-vs-pop, resource-first, already-due, cancel-after-terminal). No claim
of exhaustive DST proof is made.

### Frontend-neutrality classification (per admission step)

- **FN** (semantic obligations surviving a stackless frontend): registration
  lifetime (caller-owned address-stable node), exactly-one terminal winner
  (resolve_ CAS), deadline/cancel/resource precedence, terminal-state
  validation before suspension permission, exactly-once wait retirement,
  winner-before-publication, primitive-local reconcile obligations, close
  semantics.
- **PP** (primitive policy, primitive-local by design): permit/ownership
  transfer, Queue role FIFO + ring order + lease custody, RwLock
  fairness/prefix claim, Condition mutex release/reacquire, Event
  level-state, closed-state policy, inline resource commit.
- **SF** (current stackful mechanism): `Fiber*`/`FiberState`, `sched_ctx`,
  physical switch, worker routing/`local_runnable`, `waiting_waitq_count_`
  (MW-S3 bookkeeping — classified stackful bookkeeping at
  scheduler_park_wake.cpp:1240-1244), `waiting_ready_`, park/wake handle
  mechanics, `suspend_switch_pending` steal authority.

### S3 search & verdict

Candidate S3s examined and rejected as non-S3:
- per-site rollback (unlink + counter undo): S1 echo — each site's undo is
  enabled only by its own winning CAS; the retirement OBLIGATION is stated
  once (frontend-neutrally) at scheduler_park_wake.cpp:1240-1244.
- `if (>0) --` guarded decrements (~15 sites): S2 defensive redundancy.
- `wake_node_locked` inline shapes (mutex/sem/event): same mechanics,
  different resource facts — PP, not duplicated authority.
- `node.prev_ == nullptr` FIFO-head rule (8 sites): structural fact has ONE
  owner (WaitQueue under mtx_); the no-barging policy is per-primitive by
  design (PP).
- final `is_terminal()` rechecks (14 sites): S1 unreachable echo of a
  G-dominance fact owned by the Scheduler lock topology — not an independent
  decision anywhere.

**AC-2d-a VERDICT: NO AC-2d CENTRALIZATION JUSTIFIED.** Every shared
correctness fact has exactly one owner (see FINAL AUTHORITY MAP). Forcing a
helper would require forbidden shapes (§24 B/C/E/G: resource/close/recheck
callback soup; primitive policy in generic code).

### AC-2d-b

ENTERED: **NO** (Stage 3 skipped per brief §27).

---

## FINAL R2 AUTHORITY MAP

| Correctness fact | Sole authority | file:function | Class |
|---|---|---|---|
| Terminal winner (Woken/Cancelled/Expired) | `WaitNode::resolve_` CAS | wait_node.hpp | FN |
| Unlink (one structural removal seam) | winning resolver under queue mtx | wait_queue.hpp:319 | FN |
| Registration (Detached→Registered, FIFO link) | `WaitQueue::register_wait_locked` (Scheduler sole friend) | wait_queue.hpp:185 | FN |
| Suspend commitment (+steal authority) | `Scheduler::commit_suspend_locked` | scheduler.cpp:1378 | FN decision / SF mechanism |
| Ordinary deadline arm (8/10 sites) | `arm_ordinary_deadline_locked` | scheduler_timer.cpp:317 | FN |
| Ordinary deadline arm (Queue 2 sites) | Queue-local historical order (AC-2b corrective) | scheduler_queue.cpp timed admits | PP (documented divergence) |
| Ordinary deadline consume/retire | `consume_/retire_ordinary_deadline_locked` | scheduler_timer.cpp:332/338 | FN |
| Queue timer counter closure | `queue_timer_on_resolve` thunk at ACTIVE→terminal | scheduler_queue.cpp:32 | FN |
| Cancellation membership+closure (6 primitives) | `cancel_primitive_wait_locked` | scheduler_park_wake.cpp:1235 | FN |
| Generic/Select/Completion/task/I-O cancel | local authorities (out of R2 core) | scheduler_park_wake.cpp / select*.cpp | boundary |
| Admission resource precedence | per-primitive predicate in its admit CS | each scheduler_*.cpp admit | PP |
| Close resolution (Queue) | `QueuePort::close` CL1/CL2 drain | queue_port.cpp:374 | PP |
| Queue opposite-role reconcile | `queue_grant_consumer_locked` / `queue_grant_producer_locked` | scheduler_queue.cpp:435/468 | FN obligation / Queue-local authority |
| Primitive-local reconcile | mutex handoff / sem transfer / rwlock claim+batch / condition handoff / event broadcast | scheduler_mutex.cpp:269 etc. | PP |
| Wait-account retirement (MW bookkeeping) | `waiting_waitq_count_` decrements keyed by exactly-once winner | park_wake + per site | SF bookkeeping |
| Publication (runnable ticket) | `make_runnable` + `route_runnable_locked`, publication LAST | scheduler.cpp:1559 | SF |
| Physical continuation scheduling | Scheduler/worker mechanism (production); DST driver = TEST-ONLY choice among already-runnable legal continuations | scheduler.cpp worker loop | SF (test seam guarded) |

---

## STACKFUL-ONLY RESIDUALS (all mechanism, none unexplained semantic)

`Fiber*` identity (ownership relations keyed by it — semantic but
identity-base is frontend-parameterized), `FiberState`, `sched_ctx`,
`local_runnable`/routing, `waiting_waitq_count_`/`waiting_ready_`
(MW-S3 bookkeeping; no resolution path gates on them), park/wake handles +
`suspend_switch_pending`, `g_worker` entry precondition ("requires a running
Fiber" — stackful-runtime contract), Queue `QueueWaitCtx` stack-lease
delivery (documented as delivery mechanism; the semantic facts — exactly-once
delivery, winner-before-publication — do not require a *stack* frame).

### Future stackless thought experiment (timed wait / cancel / resource wake)

Under `co_await`: WaitNode terminal CAS, deadline arm/consume/retire,
cancellation membership+closure, resource precedence, Queue grant/reconcile,
wait-retirement obligation all reuse UNCHANGED. Replaced: Fiber park/switch,
`waiting_waitq_count_`/`waiting_ready_` bookkeeping (re-keyed to task
records), worker routing, `sched_ctx` switch, stack-lease delivery (node
carries an outcome slot instead). **No unresolved R2↔stackful coupling
found**: no semantic correctness fact requires a Fiber-specific truth.

---

## CONTINUATION AUTHORITY (brief §53)

Production: the Scheduler/worker mechanism owns physical continuation
scheduling. Test harness: the internal deterministic driver may choose among
already-runnable legal continuations (fail-closed, scriptable, single-worker).
Semantic Core: owns wait outcome/resource/deadline/cancel invariants, NOT
physical continuation routing. R2 closeout explicitly keeps `WaitNode`
semantic lifecycle distinct from Fiber continuation representation.

---

## FINAL DUPLICATION CENSUS

- **S0** (textual): admit-ladder comment/statement phrasing across TUs.
- **S1** (structural): the 14 unreachable `is_terminal()` echoes + rollback
  shapes; condition's dead branches (inconsistent, unreachable, recorded).
- **S2** (same semantic process, local policy): the seven ladder shapes
  (register→precedence→suspend) with per-primitive precedence/fairness
  policy — keeping local is SAFER (a shared helper needs per-primitive
  resource/close/recheck callbacks = forbidden shape B/C; Queue's
  role-mutex release point is structurally unique; Condition's mutex
  handoff interleaves mid-ladder).
- **S3**: **NONE.** UNRESOLVED S3: **NONE.**

### REMAINING INTENTIONAL DUPLICATION (kept, with reasons)

1. Queue local timer arming (2 sites) — AC-2b corrective: observable
   historical interleave via lock-free earliest-deadline cache.
2. Per-primitive reconcilers (handoff/transfer/claim/broadcast/grant) —
   different resource facts; single winner law shared.
3. `waiting_waitq_count_` decrement sites — bookkeeping keyed by the single
   winner law (SF; FE-1 re-keys it).
4. Defense rechecks (14) — unreachable belt-and-suspenders; grandfathered
   inventory (issue #144 posture); NOT extended to Queue (§20).

---

## KNOWN R2 BUGS: NONE. KNOWN R2 LIVENESS BUGS: NONE.

(Q-LIV-1 was the campaign's defect; fixed, regression-pinned, mutant-verified.
Adversarial review round 2 (2026-08-28) reclassified one recorded residual as
an in-scope R2 correctness blocker and it is FIXED by the R2-ALLOC corrective
(see the corrective section below): timed admits previously registered the
WaitNode and bumped wait accounting BEFORE the timer arming allocations
(`timer_pool_` node + `deadline_heap_` growth), so a `bad_alloc` escaping the
arming left a Registered+linked node with live accounting behind — exactly the
`registration -> timer attachment -> retirement/rollback` lifecycle R2 exists
to close. The corrective moves every admission allocation into
`prepare_ordinary_deadline_locked` BEFORE `register_wait_locked`
(select.cpp step-(5) reserve pattern); the noexcept publish phase
(`publish_ordinary_deadline_locked` / the preserved Queue LOCAL publish
sequence) commits hooks + ACTIVE count + heap push + cache recompute. Four
regression witnesses: `od_alloc_a1_generic_admission_atomic`,
`od_alloc_a2_event_admission_atomic`, `queue_alloc_pop_admission_atomic`
(one-shot injected `bad_alloc` — node Detached, zero accounting residue, no
pool/heap orphan, immediate healthy re-admission) and
`queue_lifecycle_death_push_until_alloc_fail_fast` (the value-carrying
`push_until` path has no rejection status in the result vocabulary; its
caller-visible response is the pre-existing non-empty-lease fail-fast
boundary, Debug AND Release).
Pre-existing residuals recorded by reviewers, out of R2 scope and unchanged:
`granted_not_resumed_` is per-port masked accounting (benign under G today);
blanket `>0` decrement guards mute future leaks.)

---

## TEST / FORMAL EVIDENCE MAP

| Authority | Witness | Kind |
|---|---|---|
| Terminal winner | wait_queue_resolution_authority_test, wait_queue_test (C12 race: 19296/704 split, 0 double-win) | independent semantic |
| Unlink law | wait_queue_unlink_topology_test | independent semantic |
| Deadline lifecycle | ordinary_deadline_authority_test, timer_wait_test, dst_t5_v2/v3, e11-timer-wait (formal) | independent + formal |
| Cancellation closure | primitive cancel suites ×6, dst_t4, e10-waitnode | independent + formal |
| Queue liveness (Q-LIV-1) | dst_t5_v1/v1s/v1t/v1ts (+ pre-fix failure + reverse mutant) | independent semantic + mutation |
| Queue semantics | async_queue_primitive_test, async_queue_authority_probe, e12-queue (2 models, 19 invariants + liveness) | independent + formal |
| Admission/suspend | primitive suites, dst_t1/t2, e8-suspend-switch, e9-park-wake, e12-* | independent + formal |
| Park/wake routing | e9 suites, issue116 forensics | independent + formal |

Missing: no dedicated formal model of the Queue **inline-admit grant
trigger** (e12-queue models grants as always-enabled transitions — the fix
aligns the implementation WITH that assumption; trigger-site wiring is
implementation detail, recorded as a justified formal-coverage note, no
model rewrite required: no modeled state transition changed).

---

## WHAT MUST NOT BE CENTRALIZED

Resource predicates; Queue role FIFO/ring/lease policy; RwLock fairness;
Condition mutex release/reacquire; Event level state; close semantics; the
role-mutex release point; the two Queue grant seams (already the single
reconcile authority — do NOT merge with mutex handoff); physical
continuation choice; `waiting_waitq_count_` (SF bookkeeping).

## WHAT FE-1 MAY REUSE

WaitNode (terminal CAS + outcome), WaitQueue register/resolve seams,
arm/consume/retire ordinary-deadline authority, cancel_primitive_wait_locked
closure, Queue grant authority (with outcome-slot delivery replacing the
stack lease), the ladder ORDER (registration→precedence→terminal
validation→suspend permission) as the frontend-neutral admission contract.
FE-1 replaces: Fiber park/switch/routing, `waiting_*` bookkeeping, stack
leases.

---

## GATES (actual results)

| Gate | Result |
|---|---|
| Clang Debug full (baseline + post-chain) | 193/193 PASS (baseline 048ce23; post-fix verified twice; re-run green after the R2-ALLOC corrective) |
| Clang Release full | 193/193 PASS (round 1; re-run green after the R2-ALLOC corrective — the lease fail-fast death case is both-mode) |
| TSan full (§16.3 — queue submit/dequeue, cancel, wake, reset/reuse classes) | PASS (see final chain log; evidence includes Queue-admit race classes; re-run after the corrective: 182/182 targets, exit 0) |
| ASan+UBSan full (§16.2 — Queue resource/lifetime) | PASS (round 1; re-run after the corrective — covers the new failure-path unwind and the never-published block erase: 182/182 targets, exit 0) |
| Formal: smoke tier 15 suites | PASS (incl. e9-park-wake, e9-trace-conformance, e10-waitnode, e11-timer-wait, e12-rwlock-liveness) |
| Formal: e12-queue suite | PASS (Model A 12 invariants + Model B 7 invariants + liveness) |
| doc-links (--self-test + full) | PASS (0 broken / 0 stale) |
| verify-architecture-docs | PASS |
| mechanical-facts (--self-test + full) | PASS (after LOC inventory sync commit 575c8cd) |
| assert-hygiene (--self-test + changed-lines) | PASS (no new assert family lines; fix adds none) |
| claim-hygiene | PASS |
| failure-envelope | PASS (49 rows, 46 VERIFIED, 3 honest open) |
| pre-push.sh (manual mode, full) | ALL CHECKS PASSED |
| git diff --check | clean |

Public/installed headers: comment-only touch in `include/sluice/async/
scheduler.hpp` (ed1f1cd) — no API/ABI/layout change; no negative-compile
authority change (headers' contracts untouched; negative-compile probes
green in full suite).

## MUTATION EVIDENCE

1. Pre-fix master + flipped witnesses → both FAIL with the exact stranded
   behavior (events `C:item:7` only; P waiting) — detection power proven.
2. Reverse mutant post-fix (reconcile call removed from `queue_pop_admit`) →
   `dst_t5_v1` FAILS ("must reconcile the parked producer... events:
   C:item:7") → restored byte-clean (verified by rebuild + full green).
   Reviewer A independently reproduced the same mutant.

## ADVERSARIAL REVIEW RESULTS

- **Round 1 Reviewer A (correctness/race/lost-wake/accounting): REPAIR
  SOUND.** 8/8 attack vectors PASS with lock-precise arguments (lost wake,
  double grant, granted_not_resumed_, lock topology incl. no-dual-role-holding,
  exactly-once item/lease, timer exactly-once retirement, early-return
  cleanup, cross-role FIFO fairness incl. cancel-of-head).
- **Round 1 Reviewer B (architecture/frontend-neutrality/over-abstraction):
  CLAIMS 1, 2, 3 ALL UPHELD.** Findings adopted: (a) stale DST test-file
  header — FIXED (ed1f1cd); (b) scheduler.hpp grant-seam caller comment too
  narrow — FIXED (ed1f1cd); (c) no standalone §8 gate artifact — the campaign
  brief's report sections (invariant, lock/atomic table, wake model, resource
  model, shutdown, evidence) constitute the gate record here; the change adds
  call sites to an already-gated authority without altering its wake protocol;
  (d) condition dead-branch inconsistency — recorded (S1, unreachable, not
  repaired: dead-code cleanup out of scope).
- **Disagreements: none** requiring reconciliation; both reviewers
  independently built and ran the tests; Reviewer B additionally verified
  the pre-fix/post-fix test matrix in an isolated worktree.
- **Round 2 (remote-state re-review of PR #242 at `ed1f1cd`, 2026-08-28):
  REQUEST CHANGES.** Findings: (P1, blocker) timed admission not
  allocation-atomic — registration and accounting preceded the arming
  allocations, so an escaping `bad_alloc` left a Registered+linked node with
  live accounting (the report itself still claimed `KNOWN R2 BUGS: NONE`,
  which was unsound); (P2) PR-body performance claim ("No new allocation,
  atomic, lock...") was wrong — every repaired inline commit performs one
  opposite-role `WaitQueue::mtx()` acquisition + an O(1) FIFO-head/empty
  check before the grant authority can observe a waiter (no new mutex
  object, no new lock-order edge — the cost is real but reasonable);
  (state) PR was non-Draft while its body still carried the
  `DRAFT — do not automerge` banner; CodeRabbit's green status was a SKIPPED
  review, not a PASS. Disposition: P1 fixed by the R2-ALLOC corrective
  (prepare-before-register; 4 regression witnesses; Debug full gate green);
  P2 + state fixed in the PR body; no broad re-campaign opened.

---

## R2 COMPLETION CHECKLIST

[x] known Queue liveness drift resolved (Q-LIV-1 FIXED, mutant-verified)
[x] timed-admission allocation atomicity (review round 2 P1 — R2-ALLOC
    corrective: allocations precede registration; publish noexcept; 4
    regression witnesses)
[x] admission surface mechanically enumerated (18 rows, HEAD anchors)
[x] Queue final-recheck question classified (PROVEN SAFE UNDER LOCK
    SERIALIZATION; not added for symmetry)
[x] no unresolved R2 correctness bug
[x] no unresolved S3 authority duplication
[x] ordinary deadline authority remains singular (Queue arming local per
    documented AC-2b corrective; the corrective splits prepare/publish
    WITHOUT centralizing Queue's publish sequence)
[x] cancellation closure remains singular (AC-2c)
[x] WaitNode remains sole terminal winner authority
[x] primitive policy remains primitive-local
[x] stackful mechanisms identified and separated from semantic Core
[x] no generic Wait framework introduced
[x] deterministic driver remains test-only and minimal (no expansion)
[x] independent test evidence exists (incl. mutation sensitivity)
[x] required gates green (Debug/Release/TSan/ASan-UBSan/formal/mechanical;
    Debug re-run green after the R2-ALLOC corrective)

## FINAL VERDICT

**R2 COMPLETE — NO FURTHER CENTRALIZATION EARNED**

Recommended single next task after human merge: **FE-1 pre-study slice** —
freeze the frontend-neutral admission contract (the FN column of the
authority map) as an interface comment / short design note, so the future
stackless frontend consumes WaitNode/deadline/cancel/grant authorities
without re-deriving them (per the closeout's FE-1-reuse list; no coroutine
implementation).
