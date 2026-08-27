# AC-2a — Frontend-Neutral Wait Authority Matrix — REVIEW AUTHOR REPORT

**Type:** architecture review / authority audit (read-only; no production code touched)
**Campaign:** #227 Phase 6 — AC-2 / R2 Wait authority campaign, step 1 (authority matrix)
**Governing inputs:** AGENTS.md; architecture constitution (note: constitution's "AC-2" = Explicit Operation Identity — *different* namespace from campaign code "AC-2a"; this review is the R2-Wait matrix demanded by #226 A2 / #227 Phase 6 / #225 R2); issues #221/#225/#226/#227/#234/#237 (read-only).
**Deliverable status:** UNTRACKED by design, pending adversarial human review. No issue, branch, or commit was created or mutated.

---

## VERDICT — one screen

**READY FOR ONE NARROW AC-2 SLICE.**

- **Selected first slice: a single Deadline/Timer-Lifecycle Authority inside the Scheduler** (candidate **B**) — one owner decides when a deadline registration is armed (`count++`, heap push, earliest-cache recompute) and when it leaves ACTIVE (`consumed` vs `retired` CAS + conditional `active_deadline_count_--` + per-owner hook + cache recompute). Today those decisions exist independently at ≥10 arming sites and ~15 closing sites across 8 TUs, plus a structurally parallel second implementation for Select arms.
- This is real S3: two places can currently disagree about whether one ACTIVE→terminal transition has been accounted — the exact fact whose drift breaks MW classification (`any_active_deadline_locked`) and park-bounding (`earliest_active_deadline_locked`).
- The **nine-block count survives re-audit**: exactly **9 timed admission blocks** on current master carry the full ladder (register → count → arm timer → precedence-1 resource → precedence-2 already-due → terminal-recheck → suspend tail). The AsyncQueue pair **still lacks the terminal-recheck defense** every other family carries — the #234 drift specimen is alive at `e10e181`.
- Rejected: whole-admission-core extraction (too wide for one Draft PR), wake-publication mega-helper (would become forbidden callback soup), anything touching fiber switch/routing (SF, out of scope), and any GenericWait framework shape.
- The one-CAS winner substrate (`WaitNode::resolve_`) is already the strongest FN authority in the tree — it is NOT duplicated and must not be re-litigated. The remaining compression target is what surrounds it.

---

## BASE / SCOPE

```
BASE_SHA          : e10e18119fb65c539a593559910b81a5283b1f1f (master, PR #236 merge)
git status --short: ?? docs/history/reviews/ARCH-FUNDAMENTAL-POSTRX1-REVIEW-AUTHOR-REPORT.md
                    (known human-owned untracked review artifact — untouched)
branch            : master
platform          : WSL2 linux 6.18 (evidence = source reading; no new measurements needed)
```

Scope: every wait-capable path under `sluice_async`: Mutex, Semaphore, Event, Condition, RwLock, AsyncQueue push/pop (+timed), generic WaitQueue awaits (+deadline), Scheduler ready-flag wait, Completion await (size/void), Select (event/timer arms), park/wake mechanism, deadline heap/pump. I/O-request cancellation layers were read for boundary confirmation only (out of scope).

Method (per #226 discipline): no name-based summarization — every cell below was traced to concrete source lines at BASE_SHA.

---

## WAIT INVENTORY (as-built, e10e181)

Registration-suspension capable paths (each traced end-to-end):

| # | Primitive / op | Entry seam(s) | File | Queue | Resource predicate |
|---|---|---|---|---|---|
| 1 | Generic wait | `Scheduler::await_wait` | scheduler_park_wake.cpp:1145 | caller WaitQueue | NONE |
| 2 | Generic timed wait | `Scheduler::await_wait_deadline` | scheduler_timer.cpp:75 | caller WaitQueue | NONE |
| 3 | Mutex lock | `mutex_lock` | scheduler_mutex.cpp:58 | mutex waiters_ | `owner==nullptr && node.prev_==nullptr` |
| 4 | Mutex lock_until | `mutex_lock_until` | scheduler_mutex.cpp:144 | ditto | ditto |
| 5 | Semaphore acquire | `sem_acquire` | scheduler_semaphore.cpp:56 | sem waiters_ | `available>0 && node.prev_==nullptr` |
| 6 | Semaphore acquire_until | `sem_acquire_until` | scheduler_semaphore.cpp:139 | ditto | ditto |
| 7 | Event wait | `await_event_wait` | scheduler_event.cpp:228 | event waiters_ | `set_==true` |
| 8 | Event wait_until | `await_event_wait_deadline` | scheduler_event.cpp:309 | ditto | ditto |
| 9 | Cond wait (prepare) | `condition_wait_prepare` | scheduler_condition.cpp:30 | cond waiters_ | none (releases bound Mutex) |
| 10 | Cond wait_until | `condition_wait_prepare_until` | scheduler_condition.cpp:116 | ditto | ditto |
| 11 | Queue push | `queue_push_admit` | scheduler_queue.cpp:45 | port.waiters_[0] | `!closed && !ring_full && head` |
| 12 | Queue push_until | `queue_push_admit_until` | scheduler_queue.cpp:167 | ditto | ditto |
| 13 | Queue pop | `queue_pop_admit` | scheduler_queue.cpp:112 | port.waiters_[1] | `!ring_empty && head`; else `closed` |
| 14 | Queue pop_until | `queue_pop_admit_until` | scheduler_queue.cpp:253 | ditto | ditto |
| 15 | RwLock read | `rwlock_read_lock(_until)` | scheduler_rwlock.cpp:221/481 | rwlock waiters_ | `!writer_active && head` (single-node prefix claim) |
| 16 | RwLock write | `rwlock_write_lock(_until)` | scheduler_rwlock.cpp:318/571 | ditto | `active_readers==0 && !writer_active && head` |
| 17 | Ready flag | `await_ready_flag` | scheduler_park_wake.cpp:1119 | `waiting_ready_` map | `ready.load()` |
| 18 | Completion (I/O) | `await_completion_size` | scheduler_park_wake.cpp:838 | arena waiter + WaitRecord | `c.ready()` (reap/cancel registry, F1) |
| 19 | Completion void | `await_completion_void` | scheduler_park_wake.cpp:927 | same | same |

Resolution/wake-capable paths:

| Cause | Seams |
|---|---|
| Wake (generic head) | `wake_wait_one` / `wake_wait_one_locked` (park_wake.cpp:1190–1233) |
| Wake (specific node) | `WaitQueue::wake_node_locked` (wait_queue.hpp:254) |
| Handoff (owner-commit variant) | `mutex_handoff_one_locked` (scheduler_mutex.cpp:269) |
| Grant (queue reconcilers) | `queue_grant_consumer_locked` / `queue_grant_producer_locked` (scheduler_queue.cpp:356/389) |
| Grant (rwlock head reconcile + reader batch) | `rwlock_grant_from_head_locked` + shared claim primitive `rwlock_claim_node_woken_locked` (scheduler_rwlock.cpp:49/73) |
| Cancel | `cancel_wait`; primitive-gated variants: `event_cancel_wait`, `mutex_cancel`, `sem_cancel`, `condition_cancel_wait`, `queue_cancel`, `rwlock_cancel` |
| Expire (timer) | `expire_wait` + `pump_deadlines_locked` + `retire_timer_for_node_locked` (scheduler_timer.cpp:164/193/317) |
| Set broadcast | `event_set_broadcast` drain loop (scheduler_event.cpp:30) |
| Notify | `condition_notify_one/_all` (scheduler_condition.cpp:205/216) |
| Release transfer | `sem_release` (scheduler_semaphore.cpp:264) |
| Unlock reconcile | `mutex_unlock`; `rwlock_unlock_read/write` |
| Select resolution | `select_process_group_locked` driver + per-kind finalizers (`select_finalize_timer_{winner,loser}_locked`, event analogs in select.cpp/select_timer.cpp) |
| I/O reap wake | ReadySink `on_ready` → `route_runnable` (scheduler.cpp:1410–1460); non-arena legacy fallback via `waiting_size_/void_` maps + `wake_ready_completions` scan |

Suspend-committal sites (`commit_suspend_locked`): **23** across 9 TUs (condition 2, semaphore 2, park_wake 6, timer 1, queue 4, rwlock 4, event 2, mutex 2, select 1). Physical context-switch site is uniformly `fiber_ctx::context_switch` outside the G critical section; the sole exception structurally is Fiber main-entry bootstrap (out of wait-domain scope).

---

## AUTHORITY MATRIX

Columns condensed where a value is uniform; "all paths" means rows 1–19 above. Full-disclosure rules followed: NONE/N/A used rather than padding.

| Lifecycle step | Where implemented today | Uniform? | Domain notes |
|---|---|---|---|
| Initial predicate test (pre-lock fast path) | each primitive wrapper (`AsyncMutex::lock` etc.) before seam; QueuePort try_push/try_pop | PP per primitive | FIFO/barging policy shapes this |
| Admission locks | `global_mtx_` (G) everywhere; + `port.state_mtx_` (S) for rows 11–14; + role `WaitQueue::mtx_` (W); Condition takes its two W's sequentially, never nested (condition cpp:53–66, 139–155) | yes except queue trio | edge set fixed, see LOCK-ORDER MAP |
| `register_wait_locked` (Detached→Registered CAS + FIFO tail link) | ONE implementation, `wait_queue.hpp:183` — called at 21 production sites + 5 test-forge sites | yes (single authority) | C8 reuse-rejection contract |
| Accounting acquire | `++waiting_waitq_count_` hand-written at every admission; rows 11–14 additionally `++port.active_wait_associations_` | obligation uniform, sites duplicated | see S-classification |
| Timer registration (timed rows) | `timer_pool_.emplace_back(++count, heap_push, recompute)` verbatim ×9 production + ×1 test-only (scheduler_timer.cpp:451–457) | yes | see DEADLINE/TIMER MAP |
| Precedence-1 resource admission (inline Woken) | rows 3/5/7/11–16: predicate check + `wake_node_locked`/claim + resource commit + (timer retire) + count decrement; rows 1/2: N/A (no resource); rows 9/10: N/A (release-not-acquire semantics) | shape uniform, predicate PP | never bypasses resolve_ CAS |
| Precedence-2 already-due closure | rows 2,4,6,8,10,12,14,15u,16u: `clock_now_unlocked() >= deadline → expire_locked + try_claim_expiry + --count` ×9 | yes | untimed rows N/A |
| Terminal recheck (undo-if-resolved defense) | present in ALL WaitQueue families **except rows 11–14** (Queue admits rely solely on held-G serialization argument) | 15 of 19 have it; Queue omits | **the live drift specimen** |
| Suspend commitment | `commit_suspend_locked` single authority (suspend authority raised BEFORE make_waiting, both under G; scheduler.cpp:1347) | yes | closes suspend-before-switch race for all paths |
| Physical suspend | `fiber_ctx::context_switch` outside G | yes | SF |
| Waiter ownership | caller-owned WaitNode in awaiting frame; node holds opaque `fiber_`; NO per-node scheduler map (E10 design) | yes | dtor asserts !Registered |
| Per-op context hook | `node.user()`: Queue=QueueWaitCtx, RwLock=RwWaitCtx; others null | PP option | read only by winner path under G+W |
| Winner arbitration | ONE atomic: `WaitNode::resolve_(outcome)` acq_rel CAS Registered→{woken,cancelled,expired} (wait_node.hpp:240); loser does nothing | yes — single authority, sealed | THE load-bearing FN fact |
| Unlink right | winner-only, same critical section (`unlink_locked`; §7 Unlink Law) | yes | no competing destructor-unlink |
| Accounting retirement | `--waiting_waitq_count_` guarded (`>0`) at ~30 resolver-side sites; Queue adds `--active_wait_associations_` at 7 manual sites **and** inside pump via `owner_ctx_` | sites heavily duplicated | highest-density duplication found |
| Timer retirement (non-timer winner) | `retire_timer_for_node_locked` O(pool) scan-by-active-node; 12 call sites (wake/handoff/claim×2/grants/cancel-family×6) | yes w/ caveats | see DEADLINE/TIMER MAP |
| Timer consumption (timer wins) | `try_claim_expiry` then `--active_deadline_count_`: inline-due ×9, pump (timer.cpp:233–242); Select runs a parallel type+pair of helpers | mostly | drift toward dual systems |
| Wake publication | `publish_waiting_fiber_runnable_locked` (make_runnable exactly-once guard → route to recorded owner) OR inline branch: `f->make_runnable()` + `route_runnable_locked(f, owner_for_fiber_locked(f))` | two idioms, one rule | exactly-once + freeze-before-visible |
| Resume accounting | Queue post-switch `granted_not_resumed_--` under G ×4 | Queue-only | SF tail |
| Close/shutdown interplay | Queue closed_ mapping onto Woken-with-retained-lease; Event reset pure flip (no resolve); Scheduler does NOT auto-cancel waits at teardown (`role_waiters_empty_locked` precondition) | PP | verified against E10-CORRECTIVE C3 |
| Fairness | strict FIFO tail-link everywhere; mutex/sem anti-barging via `empty_locked()` try gates; rwlock writer-bounded reader prefix; single-group lowest-index tie-break (select) | PP flavor | FIFO default is an FN decision point |

### FRONTEND-NEUTRALITY MATRIX (litmus: stackful fiber replaced by `co_await io.wait(...)` callers)

FN rules below hold regardless of whether resume = fiber switch or coroutine handle resume:

| Step | Verdict | Reasoning |
|---|---|---|
| Terminal outcome vocabulary {woken, cancelled, expired(+primitive close policies)} | **STILL REQUIRED** | any frontend exposes identical outcomes to its caller |
| Single resolve_ CAS winner + loser-no-op | **STILL REQUIRED** | wake vs cancel vs timeout race exists under any frontend |
| Register-then-recheck-then-may-suspend ordering (lost-wake defense) | **STILL REQUIRED** | the between-register-and-park window is frontend-independent |
| Admission precedence ladder (resource > already-due > terminal-recheck > suspend) | **STILL REQUIRED** | a co_await `lock_until` must answer grant-vs-expire identically |
| Deadline arming / state-before-pointer expiry gate / exactly-once retirement accounting | **STILL REQUIRED** | timers outlive representation choice |
| Membership-gated cancel identity safety | **STILL REQUIRED** | wrong-primitive cancel must stay inert |
| Exactly-once publication of resumption (make_runnable guard semantics) | **STILL REQUIRED** (rule) | "resume-at-most-once after unique winner" is representation-free |
| `waiting_waitq_count_` as an SF MW-classification counter | STACKFUL-ONLY (this counter's purpose) | but the underlying obligation "every registration counted once, retired once" is STILL REQUIRED — this split motivates moving the pairing into one authority, leaving MW bookkeeping downstream |
| Fiber/WorkerState routing, idle-dance epochs, steal, MW classify | STACKFUL-ONLY | scheduler execution mechanism |
| `fiber_ctx::context_switch` + `commit_suspend_locked` mechanics | STACKFUL-ONLY | keep OUT of semantic Core |
| Primitive predicates & ownership transfer shapes | PP (parameterized) | permit transfer / owner commit / ring commit / reader-prefix / SET-reset remain local |
| Near-identical cancel seam bodies differing only by an extra counter line | TS-ish shells around one FN obligation | textual family, one semantic rule |

No step classified UNCERTAIN beyond the deliberately split count-obligation row above.

---

## LOCK-ORDER MAP (verified at e10e181)

Established edges (no reversals found):

```
G = Scheduler::global_mtx_
W = WaitQueue::mtx_        (per queue)
S = QueuePort::state_mtx_
R = WorkerState::inbox_mtx
M = Scheduler::wait_registry_mtx_   (Completion wait records; standalone leaf)
L = ApplicationRuntime::lifecycle_mtx_ (standalone; never nests downward)

G → W            (every WaitQueue-integrated path: rows 1–10,15–19, resolvers, cancels, pump)
G → S → W(role)  (rows 11–14 + reconciler fast paths, queue_port.cpp:115/185/…)
                 NOTE: the two role queues' W's are NEVER held together
                 (sequential acquisition inside queue_role_waiters_empty_locked;
                  Condition likewise sequential across its two distinct queues)
G → R            (route_runnable_locked pushes local_runnable under inbox lock, then signals)
wake_mtx_        STANDALONE: notified-under-G but never acquired while holding G (as-built doc §7 agrees)
wait_registry_mtx_ leaf under G?  → acquired via *_locked helpers while caller holds G (acquire_wait_record_locked from await_completion_* under G) ⇒ implicit edge G→M, single-direction
```

Extraction-risk findings:
- A helper extracting any admission/prelude step changes NOTHING about ownership iff it runs strictly within an existing G(-holding) frame; every candidate below satisfies this constraint. No proposed move invents a new lock, flips an edge, executes user callbacks under locks (none do today except the two registered fn-pointer thunks, which run under G by documented law and are accounted, idempotent, non-blocking), or creates a second synchronization authority.
- `retire_timer_for_node_locked`'s O(active-timers) pool scan happens under G on every non-timer win; a keyed structure would change allocation profile — flagged as a future option only, NOT part of the slice.

## WINNER / PRECEDENCE MATRIX (terminal outcomes & order)

Shared arbitration mechanism (uniform): first valid `resolve_(outcome)` CAS wins; losers perform zero mutations. Primitive-specific precedence POLICY (deliberately not globalized):

| Operation pair | Event | Sem | Mutex | Cond | Queue prod | Queue cons | RwLock | Select group | Generic timed |
|---|---|---|---|---|---|---|---|---|---|
| resource vs already-due | resource wins (SET checked first, event cpp:347→363) | permit first (sem cpp:179→200) | owner-first (mutex cpp:186→204) | N/A release-shape (due-inline retains Mutex, cond cpp:162) | space first (queue cpp:198→223) | item first (queue cpp:280→304) | claim first (rwlock cpp:518→535 / 608→621) | lowest-ready-index snapshot (select.cpp:1074+) | N/A no resource |
| cancel vs resource | cancel can't beat SET observed pre-registration (CAS) | same | same | same | same | same | same | loser finalizer retires arms (SN-9 ordering) | cancel beats pending timer (retire-then-publish) |
| timeout vs wake (post-suspend) | expire races set-drain through same G+CAS; winner publishes | symmetric | symmetric | symmetric | symmetric; timer exhausts lease retained | symmetric | pump routes through `rwlock_expire_wait` + head reconcile | timer consume AFTER group CAS (select_timer.cpp:229+) | expire_wait mirrors wake/cancel exactly |
| close vs resource | N/A | N/A | N/A | N/A | closed wins only when no space AND open-check fails… producer: closed outcome only via closed_ branch after space-fail; consumer: items drained BEFORE closed mapping (resource-first consumer, queue cpp:295) | ← | N/A | N/A | N/A |
| Queue close encoding | — | — | — | — | **maps "closed" onto Woken + retained lease** (caller decodes via ctx) — PP, unique among primitives | ← | | | |

Conclusion honored: precedence is NOT globally identical; the shared part is the *mechanism* (CAS + admission-time ladder skeleton), the per-cell answers are PP.

## DEADLINE / TIMER MAP (duplication census)

One deadline epoch today requires these decisions, with their repetition counts:

| Decision | Independent implementations |
|---|---|
| create control block + `++active_deadline_count_` + heap push + recompute cache | **10** sites (9 production admission blocks + 1 TEST-ONLY coordinator helper timer cpp:451) |
| non-timer winner MUST retire registration in same CS BEFORE publication (TimerLifetimeClosure) | **12** call sites of `retire_timer_for_node_locked` (wake_locked, cancel_wait, handoff, rwlock claim ×internal callers, 2 queue grants, 6 cancel seams …) |
| timer WON: `try_claim_expiry` + `--active_deadline_count_` (+ thunk/recompute) | 9 inline-due copies + pump copy (timer cpp:124, 202, 225, 233ff, 305, 365…, queue ×4, rwlock ×2 gated asserts) ≈ **13** |
| timer LOST (stale) MUST NOT decrement / MUST NOT deref node | pump guard (timer cpp:233), select pump guard (select_timer.cpp:87), erase-by-address helpers ×2 |
| recompute-after-close duty | sprinkled through ~20 sites manually |
| SELECT parallel universe | `SelectTimerRegistration` type + splice/consume/retire helpers implementing the SAME ACTIVE/RETIRED/CONSUMED + count-exactly-once pattern over a second storage family (select_timer.cpp:46–192) — separate API, same invariant |
| per-port timer counters (Queue) | `active_queue_timers_` via OnResolveFn thunk (queue cpp:32–42) + pump manual `--active_wait_associations_` (timer cpp:286–299) — the ONLY cross-module reach-in of its kind |

Drift consequences already realized: Queue lacks recheck; Queue decrements pump-path counters by hand while others route through thunks/helpers; Select forked a whole parallel control block. None is a *bug* today (guards assert), but three different shapes of one accounting fact is textbook S3 materialization.

## LOST-WAKE / TERMINAL-RECHECK MAP

Uniform defense (verified in all 23 suspend paths): register + (predicate/admission rechecks) + `commit_suspend_locked` inside ONE continuous G(-scoped) CS; every resolver requires G before publishing a runnable ticket/resolving; physical switch occurs only after CS exit. Therefore "resolved between initial check and suspension" cannot strand: either the resolver completed before the CS (observed by the recheck) or it serializes after publication.

Where encoded:
- `await_wait/cpp:1173`, `await_wait_deadline/timer cpp:143`, `mutex_lock/120`, `mutex_lock_until/217`, `sem_acquire/122`, `sem_acquire_until/213`, `await_event_wait/292`, `await_event_wait_deadline/376`, `condition_prepare/91`, `condition_prepare_until/189`, `rwlock_read_lock/279`, `write_lock/358`, `read_lock_until/550`, `write_lock_until/635`, `await_completion_size/880&893` (two rechecks: fallback-map + arena-loss paths), `await_completion_void/954/961`, `await_ready_flag/1129`, select armed-group snapshot CS (:1074ff).
- **Missing (only place): Queue admit rows 11–14.** Their comment trail argues unreachability under held G+S+W. That argument is the same one every other family makes *in addition* to carrying the recheck. Classification: unreachable-today divergence (S1/S2), proof that nine hand-copies do not converge — exactly the regulatory evidence the specimen was supposed to provide.

Additional per-primitive lost-wakeup subtleties confirmed intact: sem stable-state invariant (eligible queued waiter ⇔ available==0), mutex transient `owner==nullptr with earlier waiter` refusal, Event admission-before-final-set-check phase seams, condition register-before-handoff (InvNoLostNotifyWindow), Completion Race-A closure (record+arena-register+recheck+suspend atomic).

---

## ASYNCQUEUE DRIFT RE-AUDIT (from current master; #234 conclusion NOT assumed)

Compared `push`/`timed_push`/`pop`/`timed_pop` + fast paths + reconcilers (`queue_port.cpp`):

1. **Exactly what logic is duplicated?** The entire register→counts(2)→[arm timer]→precedence ladder→close branch→suspend-tail scaffolding, ×4; plus grant/cancel epilogues mirroring the other primitives'.
2. **Classification?**
   - register/CAS/unlink/publication-rule machinery: **FN** shared authority (already centralized).
   - precedence answers (space/item vs closed; producer vs consumer resource-first asymmetry): **PP**, intentional and divergent-per-role.
   - close-mapped-to-Woken with retained lease: **PP** encoding choice.
   - the missing terminal-recheck + hand-managed pump counters + post-switch `granted_not_resumed_` decoupling: **drift-prone deltas with no semantic mandate — accidental** (nothing documents them as desired absence; contrast transfer-policy divergences in #234 which ARE documented bilateral contracts).
3. **Intentional differences:** three-lock topology G→S→W (needed: ring mutation serialization independent of waiter lists), per-port association counters, inline ring commit before resolve returns true.
4. **Accidental differences:** recheck omission; `fire_on_resolve_locked` invoked at inline branches but active-association decrement done manually in four places while pump reaches it via `owner_ctx_`; thunk-signature asymmetry (`OnResolveFn(bool)` vs RwLock's null hook).
5. **Can ONE authority centralize without hiding queue policy?** Yes — the scheduling-skeleton core (register/count/[timer]/ladder-halting predicate hooks/recheck/suspend) parameterized by primitive-provided {locks-to-hold, predicate fns, commit thunks}; Queue passes three locks + closed-mapping policy. HOWEVER this is candidate A, not the selected first slice (blast radius).
6. **Still the strongest first slice?** **No — demoted.** It remains the best *specimen* (proves drift is real), but the strongest *authority move* now measurable is the deadline/timer lifecycle (below): smaller interface, crosses the most TUs where the fact is genuinely re-decided, and carries the crash-critical lifetime closure. Queue-local dedup would fix ¼ of one family while the same scaffold persists in five others.

## CURRENT ADMISSION-BLOCK COUNT

`EXACT_COUNT_CURRENT_MASTER = 9`

(timed/full-ladder admission blocks with register→count→arm→precedence-1→already-due→terminal-recheck→suspend: await_wait_deadline, mutex_lock_until, sem_acquire_until, await_event_wait_deadline, condition_wait_prepare_until, queue_push_admit_until, queue_pop_admit_until, rwlock_read_lock_until, rwlock_write_lock_until.)

Supporting counts: 9 additional UNTIMED WaitQueue admissions (same minus timer/already-due steps); 3 registry-based waits outside WaitQueue (ready flag, completion size, completion void); total suspend commits 23.

## S0 / S1 / S2 / S3 CLASSIFICATION (only S2/S3 justify work)

| Pattern | Class | Justification |
|---|---|---|
| `WaitNode::resolve_` + Unlink Law + sealed WaitQueue | (already single) | no duplication — protected |
| `commit_suspend_locked`, `publish_waiting_fiber_runnable_locked`, route/idle-dance | single authorities | n/a |
| Nine/ten ladder scaffolds including hand-rolled recheck defenses | **S3** (skeleton) + S1 (per-site text) | the suspend-decision & defense is the same correctness fact re-decided 19×; Queue proves divergence |
| Deadline arming triple (++count/push/recompute) ×10 | **S3** | arming without count ⇒ park stops honoring deadlines; sites differ already (test-helper skips q registration) |
| Retirement/accounting pairing (ACTIVE→terminal ⟺ count-- exactly once + thunk + recompute) | **S3** | pump-vs-thunk-vs-assert-gated three shapes; wrong at any one site ⇒ classification stalls or `recompute` misses wake bound |
| State-before-deref expiry gate | **S3** (fact) duplicated across pump + select-pump + erase helpers | UAF-critical; two storage families decide it separately |
| Six membership-gated cancel seams | **S2** | one rule (identity-safe cancel) — real semantic compression available, small payoff |
| Untimed/timed sibling bodies within one primitive | S1 | text shadowing, resolved by whichever skeleton authority lands |
| Select parallel TimerRegistration system | **S2 leaning S3** | two types enforce one lifetime invariant; candidates B may leave as documented sibling initially |
| Reconciler grant epilogues (handoff/queue-grant/rwlock) differing mid-sequence | PP + TS | genuine inter-positioned commits; do NOT unify (mega-helper risk) |
| awaiting-wrapper loops (RuntimeTaskContext submit+await_take) | TS | no authority content |

## TEST / FORMAL WITNESS MAP (independent witnesses preserved — none to be DRY'd)

| Invariant | Witnesses | Class |
|---|---|---|
| resolve_ one-winner / unlink / reuse rejection | TLA+ e10-waitnode suite; async_mutex_nothrow_authority_probe, select_claim_death_test | independent |
| Timer lifetime closure (state-before-deref; lazy reclaim) | TLA+ e11-timer-wait (t17/t18 cited in source); timer_wait_test | independent |
| Per-primitive admission/ladder correctness | TLA+ e12-{event,semaphore,async-mutex,async-condition,queue,rwlock,rwlock-scheduler-liveness}; primitive tests + authority probes + death tests per primitive | independent |
| Select group/finalizers | TLA+ e13-select-core/-safety; select_claim_death_test | independent |
| Park/wake epochs, retire-rescue, spawn epoch | e9-park-wake, spawn-wake-epoch, worker-retire-rescue suites + trace-conformance bridge | independent |
| Completion registry/lease | f1-wait-record suite; request_waiter_borrow_lease_test; backend C2 rows | independent |
| Runtime epoch | e16-application-runtime suite | independent |
| Missing today | no formal model states "ACTIVE count == heap/pool live entries at quiescence" as an explicit invariant (it is exercised implicitly by t18) | gap → slice's added assertion test becomes first witness; consider manifest note, not new suite |

## AI / MAINTAINER REASONING ASSESSMENT

Metric = semantic ambiguity reduced, not LOC. Today a reviewer answering *"when is this timer retired?"* must know all 12+ call-site conventions and both thunk styles; *"did this ACTIVE transition decrement?"* requires checking up to four co-located lines per site. After the selected slice those collapse to ONE function each with typed parameters (`deadline_owner`, `won`) — the six questions in §15 of the task become greppable at one site. Queue's omitted recheck demonstrates the cost of the status quo precisely because no maintainer could tell omission-from-policy; an authority boundary forces the distinction to be written down. Framework-shaped alternatives score lower on this metric: they relocate the question behind template metapolicy.

---

## CANDIDATE FIRST SLICES (ranked)

| Criterion | **B. Deadline/timer lifecycle authority** | D. Unified cancel seam | A. Common admission skeleton | C. Publication-prelude merger | E. Queue-local admission core |
|---|---|---|---|---|---|
| S3 strength | high (accounting+lifetime facts re-decided ~25×) | medium (one rule, 6 copies, already near-identical) | high but composite | low–medium (prelude bodies differ mid-CS by genuine PP commits) | medium-high but intra-family only |
| Drift evidence | Select parallel system; pump-vs-thunk counter asymmetry; recheck omission adjacent | member-gate order drift historically fixed per-site (E12-A corrective chain) | Queue recheck omission | handoff owner-before-publish + queue retire-before-commit prove forced variance | recheck omission; pump hand-reach |
| Frontend-neutral | yes (pure) | yes | yes | yes (rule) | yes |
| Call sites moved | arming 9→1 fn; closing ~13+12 routed through 1–2 fns; semantic line count −≈250 | 6→1 thin seam | 19 admissions→core | aborted | 4→1 core |
| Differences to preserve | predicate/resource commits, Queue double-counters (as parameters), Select sibling (documented) | Queue's extra counter; rwlock's head-reconcile continuation | every predicate + Condition release-shape + Queue closed encoding | all interleavings (that's why rejected) | three-lock topology, closed mapping |
| Lock risk | none (G-held helpers) | none | none mechanical; function-parameter surface invites future misuse | high (callback soup) | none |
| Test coverage | full (e11/e12 suites, probes) | full (cancel death tests per primitive) | full but wide rewrite | partial | focused queue tests |
| Expected semantic compression | high | low-medium | high | negative | medium |
| Blast radius | contained (new private Scheduler members + body rewrites of arming/close sequences; no signature changes) | small | large (whole ladder restructure; touches test-only helper too) | large | small |
| **Recommendation** | **SELECTED — first slice** | defer to second slice | long-term destination after B/D land | reject | fold into later A |

## SELECTED FIRST SLICE

**AC-2 FIRST SLICE = Scheduler-owned Deadline/Timer-Lifecycle Authority ("one place owns ACTIVE")**

CURRENT AUTHORITY (today):
- Arming: `scheduler_timer.cpp:109–113`, `scheduler_mutex.cpp:176–180`, `scheduler_semaphore.cpp:169–173`, `scheduler_event.cpp:339–343`, `scheduler_condition.cpp:150–154`, `scheduler_queue.cpp:189–196` & `272–279`, `scheduler_rwlock.cpp:504–510` & `596–602`, test-only `scheduler_timer.cpp:451–457`.
- Closing (consume/retire + count + thunk + recompute): inline-due paths ×9 (see files above), `pump_deadlines_locked` claims/branches (`scheduler_timer.cpp:222–313`), `retire_timer_for_node_locked` (`scheduler_timer.cpp:317–350`), assert-gated rwlock due-paths (`scheduler_rwlock.cpp:535–548`, `621–633`), queue thunk `queue_timer_on_resolve` (`scheduler_queue.cpp:32–42`).
- Parallel sibling: Select pool + `select_timer_retire_locked` / `select_timer_consume_locked` (`select_timer.cpp:122–161`) — same invariant, second implementation.

DUPLICATED DECISION (the semantic facts decided in multiple places):
1. "An ACTIVE registration leaves ACTIVE exactly once, as CONSUMED (timer won) or RETIRED (lost), and *if and only if* that CAS succeeded, `active_deadline_count_` is decremented and the earliest-deadline cache is recomputed."
2. "Arming a deadline atomically pairs pool entry, heap entry, and active count."
3. "No dereference of node/arm before the controlling state is observed ACTIVE."

NEW AUTHORITY BOUNDARY:
Two (plus lookup) private `Scheduler::` methods, G-required, existing today-identical semantics:
```cpp
TimerRegistration* arm_deadline_locked(WaitNode&, WaitQueue*, deadline_t,
                                       OnResolveFn /*default null*/, void* ctx);
bool close_deadline_locked(TimerRegistration&, bool timer_won); // CAS->CONSUMED|RETIRED + gated --count + thunk + recompute
// retire_timer_for_node_locked(node) stays as ACTIVE-scoped lookup routing into close(won=false)
```
Select sibling: NOT merged in slice 1; add a header-note cross-reference + optionally share `#`-style unit test. (Recorded as POSSIBLE FUTURE CONSEQUENCE: eventual common `DeadlineEpochBase`.)

PARAMETERS/POLICIES REMAINING OUTSIDE: every resource predicate; queue's dual per-port counters (passed via the existing `on_resolve_/owner_ctx_` mechanism — unchanged); rwlock's ExpireCtx routing in pump; close-mapped-to-Woken encoding; precedence ladder order; SelectTimerRegistration; heap container choice.

LOCK EFFECT: unchanged. Both helpers require-and-run-under caller-held G (leaf calls into existing G-held frames only); no new lock, no edge change, no callback executed outside today's G-law.

FRONTEND EFFECT: validity argument identical under a future co_await frontend — deadlines and their exactly-once accounting do not reference fibers, workers, or the switch mechanism anywhere in the new boundary (unit-tested with plain nodes already).

PUBLIC API EFFECT: expected NONE (all additions are Scheduler-private; WaitNode/WaitQueue/TimerRegistration headers untouched except comments).

FORMAL EFFECT: e11-timer-wait suite covers the transition semantics — behavior-preserving refactor ⇒ models unchanged; update implementation-binding comments and the manifest note only if the audit asks. A new executable witness test asserting count/pool/heap coherence at boundaries accompanies the slice.

TEST EFFECT (must continue passing, independent): timer_wait_test (T15/T17/T18 regions), event/mutex/semaphore/condition/rwlock/queue primitive tests + authority probes + death tests, select_claim/safety tests, async_stats_wait_race_test, full Clang Debug gate.

PERFORMANCE EFFECT: neutral by construction (callers already hold G; helpers are direct-callable, trivially inlinable; no new allocation). Not a performance change under §16.7 ⇒ no E1 regression gate triggered; a smoke E1 round is optional, not required.

REJECTION-TEST (task §19): ten questions all pass: moves one invariant ✔; kills duplicate authority not syntax ✔; preserves policy (parameters) ✔; preserves lock order ✔; co_await-valid ✔; keeps SF mechanics out ✔; independent witnesses named ✔; narrow enough for one Draft PR (≈2 helper functions + ~22 rewrite points, mechanically checkable diff classes) ✔; rollback = revert single commit ✔; negative review can conclude "leave duplicated" — accepted outcome since no behavior change ships unless approved ✔.

## REJECTED ALTERNATIVES
- A (full admission skeleton) now — composite blast radius (19 paths + test helper), better sequenced after B and D normalize its ingredients; framework pressure highest here.
- C (publication prelude) — would need a twenty-callback mega-helper; inter-CAS commits are genuine PP; explicitly forbidden shape per task §16.
- "Unify Select timers into TimerRegistration now" — desirable endpoint but couples two storage lifetimes in one slice; kept as documented follow-up consequence.
- Any WaitFramework/GenericWait/policy-DSL shape — forbidden; not needed by the evidence.
- Splitting worker_loop/park_on_wake_source — contradicts #226 sequencing guidance; no S3 identified there (their duplication is textual tails, SF mechanism).

## IMPLEMENTATION BOUNDARY (for the future PR)
Slice = one commit-train Draft PR: (1) authority methods + unit witness; (2) arming-site rewires (10); (3) closing-site rewires (~15) incl. thunk unification for Queue pump path; (4) select-timer cross-reference note. Each step green under full Debug gate; TSan class gate included (shared-counter/synchronization change class). Negative-compile/docs gates per changed-lines manifest. No public-header edits.

## REQUIRED GATES FOR FUTURE PR
Clang Debug full build/test (baseline); Clang Release (public-header touched? none expected — only if that changes); TSan mandatory (§16.3 — wait/synchronization class; include wake-vs-expiry and cancel-vs-expiry focused filters); architecture compliance mini-gate filling Gate 0–4 fields with this report linked; docs touchpoints: `as-built-async-architecture.md` §8 authority table row + e12-cross-primitive-terminal-audit appendix note; pre-push mechanical gate. Real-liburing/ASan unaffected. Formal: no re-run required; manifest annotation.

## WHAT MUST NOT CHANGE
Single resolve_ CAS winner law and sealed WaitQueue privacy; G→W/G→S→W/R edges; suspend-before-switch closure mechanics in `commit_suspend_locked`; caller-owned WaitNode address-stability; primitive predicates & fairness policies; Queue closed→Woken encoding and producer/consumer resource-first asymmetry; SyncBackend completing-at-poll topology; the current one-publication authority (`publish_waiting_fiber_runnable_locked` semantics incl. make_runnable exactly-once); test-only seam isolation (`SLUICE_ASYNC_INTERNAL_TESTING` guards); all independent witnesses listed above.

---

*Evidence base: direct source reading at e10e181 (file:line citations above); mechanical counts reproduced via grep over `src/async/*.cpp`; historical specimens read from `docs/history/closeout/e12-cross-primitive-terminal-audit.md` and #234 summary text. No measurements were taken; none were required for this verdict.*
