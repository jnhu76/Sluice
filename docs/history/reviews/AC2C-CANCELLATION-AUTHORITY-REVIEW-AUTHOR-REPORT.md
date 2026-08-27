# AC-2c Cancellation Authority Review — Author Report

## VERDICT

`READY FOR ONE NARROW AC-2c-b SLICE`

The repository has one frontend-neutral S3 duplication worth centralizing: the
membership-gated primitive cancellation terminal closure. Six Scheduler seams
independently decide the same correctness facts while holding the same
`G -> W` lock domain:

```text
target queue contains this WaitNode
-> Cancelled CAS wins
-> winner-owned unlink completes in the same W critical section
-> ordinary timer authority is retired through AC-2b
```

The slice MUST stop there. The exactly-once retirement of
`waiting_waitq_count_` is an obligation each winning CALLER discharges itself
(one guarded decrement at each call site): that concrete counter is current
stackful Scheduler bookkeeping (MW classification), not frontend-neutral
authority. Runnable publication, Fiber/Worker routing, Queue local
accounting, RwLock head reconciliation, public return-value policy, and all
Completion/Select semantics also remain outside the shared authority.

## REVIEW CORRECTION (2026-08-27, PR #240 review 5041406830)

The adversarial review of the AC-2c-b implementation rejected one boundary in
this report's original selected slice (P1) plus three evidence/doc issues
(P2). The production correction on this branch narrows the helper:

- `cancel_primitive_wait_locked` owns ONLY exact-queue membership, the
  Cancelled winner CAS + unlink, and AC-2b timer retirement.
- The guarded `waiting_waitq_count_` decrement returned to each of the six
  call sites, at its original pre-slice position (after the `Fiber*` capture
  at Event/Semaphore/Mutex/Condition, after `--active_wait_associations_` at
  Queue, before the capture at RwLock). Six identical one-line decrements are
  accepted deliberately: semantic compression is not DRY, and no accounting
  framework may be invented to remove them.
- Root cause: this report's original FRONTEND-NEUTRALITY MATRIX classified
  `waiting_waitq_count_` retirement as frontend-neutral. The exactly-once
  wait-epoch retirement OBLIGATION is semantic, but that concrete counter is
  current stackful-frontend bookkeeping (MW classification), per the AC-2a
  wait-authority matrix (issue #237). The affected sections below are
  corrected in place; this section governs wherever they still narrate the
  audit-time proposal.
- Evidence-layer correction: the GitHub CodeRabbit bot skipped review (Draft
  PR reports "Draft PRs are not automatically reviewed by default"); no
  GitHub CodeRabbit findings exist, so none may be cited. An independent
  adversarial subagent review of `d4f1a66..8105f0c` found zero behavior
  bugs; review 5041406830 is the human adversarial round.

## BASE

- Repository: `jnhu76/Sluice`
- Branch audited: `master`
- `HEAD`: `d4f1a66467a58bcdcc36addb749ca5a1921c7265`
- `origin/master`: `d4f1a66467a58bcdcc36addb749ca5a1921c7265`
- Worktree before audit: clean
- AC-2b prerequisite: **VERIFIED**. PR #238 is merged at this exact base.
- AC-2b mechanical proof:
  - ordinary arm sites call `arm_ordinary_deadline_locked`;
  - ordinary consume/retire paths call the shared helpers;
  - Queue's two direct arm mutations remain the documented cache-publication
    exception;
  - Select timer accounting remains a separate sibling authority;
  - `WaitNode::resolve_` is unchanged.
- Pre-change Clang Debug baseline:
  - `xmake f -m debug --toolchain=clang -y`: PASS
  - `xmake build sluice_core`: PASS
  - `xmake build sluice_async`: PASS
  - `xmake build -g test`: PASS
  - `xmake test -v`: PASS, 192/192 tests
- External build-tool contract: Context7 `/xmake-io/xmake-docs` confirms that
  `xmake f -m debug` persists Debug configuration, `--toolchain` selects the
  toolchain, named targets are runnable/buildable, and `xmake test` builds and
  runs configured tests.

## INVENTORY

WaitNode cancellation paths inspected:

1. `Scheduler::cancel_wait` — generic raw WaitQueue wait.
2. `Scheduler::event_cancel_wait` — Event private queue.
3. `Scheduler::sem_cancel` — Semaphore private queue.
4. `Scheduler::mutex_cancel` — AsyncMutex private queue.
5. `Scheduler::condition_cancel_wait` — condition-epoch private queue.
6. `Scheduler::queue_cancel` — QueuePort role FIFO; production-private and not
   reachable from the public AsyncQueue v1 API.
7. `Scheduler::rwlock_cancel` — AsyncRwLock private queue plus head reconcile.

Boundary paths inspected but excluded from this authority:

- `Scheduler::cancel_waiter(Completion<T>&)` / RequestArena waiter delivery;
- I/O operation cancellation and RequestKey terminal arbitration;
- cooperative task cancellation (`CancelToken` / `CancelState`);
- Select group winner/loser finalization and rollback;
- Queue `close()` and teardown;
- timer expiry and ordinary wake/grant paths.

## AUTHORITY MATRIX

Abbreviations: `G` = `Scheduler::global_mtx_`; `W` = the target WaitQueue
mutex; `S` = QueuePort state mutex; `FN` = frontend-neutral; `PP` =
primitive policy; `SF` = stackful mechanism.

| Primitive/path | Entry | Identity/membership gate | Locks/order | Pre-state | Winner/CAS + unlink | Timer/cache | Global/local accounting | Context/reconcile | Publication | Close/fairness | Return/no-op | Witness | Formal | FN? | Class |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| Generic WaitQueue | public `Scheduler::cancel_wait(q,node)` | Caller contract requires node registered in `q`; no scan | `G -> W`, W held through publication | Registered in supplied q | `q.cancel_locked`: `resolve_(Cancelled)` CAS, then winner unlink | `retire_timer_for_node_locked`; AC-2b recomputes when an active timer retires | `waiting_waitq_count_--`; no local counter | captures `Fiber*`; no resource reconcile | `publish_waiting_fiber_runnable_locked` | no close/fairness change | `true` only if runnable publication succeeds; terminal win with null Fiber returns `false`; loser `false` | `od_w2`, `od_w3`, WaitQueue race suites | E10, E11 | closure FN; return/publication SF | S0 boundary, not selected |
| Event | public `Event::cancel` -> internal seam | exact `contains_locked` in Event queue | `G -> W`, W held through publication | Registered member | same CAS + unlink | same AC-2b retire/cache | global only | `Fiber*`; no SET mutation/reconcile | canonical Fiber publication | `reset`/SET unchanged; FIFO unchanged | `true` iff member + cancel terminal winner; otherwise false/no mutation | Event T31/T32 family, parity D4 | E10/E11/E12 Event | closure FN, publication SF | S3 selected family |
| Semaphore | public `Semaphore::cancel` -> internal seam | exact `contains_locked` in Semaphore queue | `G -> W`, W held through publication | Registered member | same | same | global only; permit count unchanged | `Fiber*`; no permit grant/reconcile | canonical Fiber publication | permit fairness unchanged | same primitive bool | sem T19–T25, parity D4 | E10/E11/E12 Semaphore | closure FN, permit rules PP, publication SF | S3 selected family |
| AsyncMutex | public `AsyncMutex::cancel` -> internal seam | exact `contains_locked` in Mutex queue | `G -> W`, W held through publication | Registered member | same | same | global only; owner unchanged | `Fiber*`; no ownership transfer/reconcile | canonical Fiber publication | handoff/FIFO unchanged | same primitive bool | mtx T7–T10/T15/T16/T22, parity D4 | E10/E11/E12 Mutex | closure FN, ownership PP, publication SF | S3 selected family |
| AsyncCondition | public `AsyncCondition::cancel` -> internal seam | exact `contains_locked` in condition queue | `G -> W`, W held through publication | Registered condition epoch | same | same | global only; `active_waits_` is closed by the outer wait protocol, not this seam | cancels condition epoch only; mandatory Mutex reacquire remains caller protocol | canonical Fiber publication | notify/reacquire ordering unchanged | same primitive bool | cond T12/T18/T30/T31 + 500-race | E10/E11/E12 Condition | closure FN, reacquire PP, publication SF | S3 selected family |
| AsyncQueue role | internal `queue_cancel`; no public v1 caller | exact `contains_locked` in selected role FIFO | `G -> role W`; no `S` acquired here | Registered role member | same | same, including Queue timer hook through existing retirement authority | global plus `active_wait_associations_--`; `active_queue_timers_` remains hook-owned; `granted_not_resumed_` unchanged | lease/ring untouched; no cross-role reconcile | canonical Fiber publication | close distinct; FIFO/ring policy unchanged | same primitive bool internally | Queue formal/tests cover other terminal causes; no public cancel witness | E10/E11/E12 Queue, but public cancel N/A | common closure FN; local count/policy PP | S3 closure, local tail excluded |
| AsyncRwLock | public `AsyncRwLock::cancel` -> internal seam | exact `contains_locked` in RwLock queue | `G -> W`; release W; reconcile reacquires W; publish under G | Registered member | same | same | global only | capture Fiber/owner under W; after unlink reconcile new head; grant publications precede cancel publication | manual owner-preserving publication after reconcile | writer-fair reader-prefix/writer policy unchanged | same primitive bool | T6/T13/R1–R5/MW cases | E10/E11/E12 RwLock + scheduler-liveness | common closure FN; reconcile PP; publication SF | S3 closure, reconcile excluded |
| Completion waiter | public `cancel_waiter(Completion<T>&)` | context binding + RequestKey/generation + slot registration | G, context access, arena leaf, wait registry according to F1 contract | RequestSlot waiter registered | RequestArena moves delivery/lease; no WaitNode CAS/unlink | N/A to WaitNode timer | different slot/routing accounting | Scheduler WaitRecord/RoutingLease | canceled waiter routing, not I/O terminal publication | I/O remains live | `Result<bool>`: removed vs reap won; `not_found`/fallback possible | F1/C2c suites | F1 wait-record | distinct FN authority | S0 boundary |
| Select | public `select` internal group finalizer | group/arm identity, not WaitQueue cancellation | G plus Select registry domains | armed group/registered arm | group first-claim-wins + loser finalization | Select timer sibling authority | Select counters | group rollback/finalize | group winner publication | Select fairness/policy | `SelectResult`, not cancel bool | E13 suites | E13 | distinct FN authority | S0 boundary |

## FRONTEND-NEUTRALITY MATRIX

| Step | Class | Reason |
|---|---|---|
| Validate exact target-queue membership before mutation | FN | A coroutine waiter would still need object/epoch identity safety. |
| Registered -> Cancelled exactly-one winner | FN | Terminal arbitration is independent of stack representation. |
| Winning resolver owns unlink in the same W critical section | FN | Intrusive-queue integrity and one removal are frontend-independent. |
| Retire active ordinary TimerRegistration through AC-2b | FN | Prevents stale expiry authority regardless of resume mechanism. |
| Retire `waiting_waitq_count_` with the winning terminal transition | SF (corrected; originally mis-filed FN) | The exactly-once retirement obligation is semantic, but the concrete counter is current stackful-frontend bookkeeping (MW classification). Each winning call site decrements it itself (review 5041406830 P1). |
| Return `true` for primitive member + terminal winner | FN at primitive contract | The primitive API reports terminal ownership, not routing success. |
| `Fiber::make_runnable`, owner lookup, Worker routing | SF | Specific to the current stackful Scheduler frontend. |
| Queue local association/grant counters | PP | Queue lifecycle and lease policy. |
| Queue timer counter retirement hook | Existing AC-2b boundary | `retire_timer_for_node_locked` may fire the already-installed hook; AC-2c adds no hook or policy. |
| RwLock exposed-head grant/reconcile | PP | Writer-fair reader-prefix policy. |
| Condition mandatory Mutex reacquire | PP | Condition two-epoch semantics. |
| Select loser finalization | PP / separate FN core | Different multi-arm identity/state machine. |

## LOCK MAP

| Path | Existing lock horizon | Required after slice |
|---|---|---|
| generic cancel | acquire G, acquire W, closure + publication, release W, release G | unchanged; does not call selected helper |
| Event/Semaphore/Mutex/Condition | acquire G, acquire W, membership + closure + publication, release W, release G | caller retains identical G and W guards; helper requires both and adds no lock |
| Queue cancel | acquire G, acquire selected role W, closure + local count + publication, release W/G | identical; no S edge added or removed |
| RwLock cancel | acquire G/W, closure + capture, release W, reconcile (reacquires W), publish under G | identical; helper is invoked only inside first G/W region |

There is no new lock, lock-order edge, blocking point, callback authority,
allocation, or unlock/relock horizon. Existing AC-2b timer retirement may fire
an already-installed Queue `on_resolve` hook. The slot-lifecycle/I/O arena leaf
is never entered.

## MEMBERSHIP / IDENTITY MAP

- Primitive cancellation accepts an untrusted `WaitNode&` relative to a
  primitive instance. The only safe proof is a scan of that primitive's exact
  private WaitQueue under its W lock: `contains_locked(node)`.
- The scan deliberately does not trust `node.home_` or take a foreign queue
  lock. Wrong primitive on the same or a different Scheduler returns false.
- `cancel_locked` alone is insufficient for primitive APIs: given the wrong
  queue and a node registered elsewhere, its CAS could win before unlink is
  attempted against the wrong queue. Therefore membership must remain part of
  the selected atomic semantic closure.
- Generic `cancel_wait(q,node)` has a stronger caller precondition: membership
  is guaranteed by its caller. Changing that contract or return semantics is
  outside this slice.
- WaitNode is fresh-per-epoch and terminal states are absorbing. It has no
  reset/generation; stale/reused terminal nodes fail membership and CAS.
- Completion waiting uses `(ContextIdentity, SlotIndex, Generation)` plus a
  routing lease. It is not WaitNode identity and is excluded.

## RETURN-VALUE MATRIX

| Entry family | Meaning of success | Meaning of false/error |
|---|---|---|
| `Scheduler::cancel_wait` | Cancel won **and** a waiting Fiber was made runnable/published | terminal loser or no routable Fiber; a fiberless synthetic epoch may be Cancelled while returning false |
| Event/Sem/Mutex/Condition/RwLock public `cancel` | supplied node belongs to this primitive queue and this call won Cancelled | wrong object/detached/terminal/concurrent loser; no mutation |
| internal Queue cancel | same terminal-winner meaning as primitive family | same no-op classes; no public v1 entry |
| Completion `cancel_waiter` | waiter delivery removal won | `false` means reap delivery won; typed errors cover foreign/unbound/unsupported cases |
| I/O operation cancel | disposition/terminal arbitration defined by backend/RequestArena | not a WaitNode bool contract |

The selected helper returns only the primitive-family terminal-winner bool. It
does not replace or call generic `cancel_wait`, and it does not publish.

## TIMER-CLOSURE MAP

- Every successful WaitNode cancel first wins `resolve_(Cancelled)`/unlink,
  then calls `retire_timer_for_node_locked(node)` while G and the target W are
  still held, before any local reconcile or runnable publication.
- That function reuses AC-2b's `retire_ordinary_deadline_locked`; an ACTIVE
  registration transitions to RETIRED, decrements ordinary active count once,
  invokes any already-installed owner hook, and refreshes the earliest active
  deadline as required by the existing authority.
- Queue `active_queue_timers_` remains owned by its existing timer owner hook.
  The selected helper does not touch it directly.
- RwLock cancellation still performs head reconcile only after ordinary timer
  retirement and W release.
- Select timer consume/retire is a separate group-finalization authority.
- Mechanical search found no primitive cancel path directly mutating
  `active_deadline_count_`; no AC-2b blocker exists.

## UNLINK LAW

Current law is uniform:

```text
holds G + target W
contains target (primitive paths only)
resolve_(Cancelled) CAS
  loser -> return false; zero unlink/timer/accounting/publication
  winner -> unlink_locked in the same W critical section
         -> timer/accounting closure
         -> local policy
         -> optional publication
```

`WaitQueue::cancel_locked` binds CAS success to unlink. The selected helper
must not split those operations, invoke unlink separately, or change
`WaitNode::resolve_`.

## ACCOUNTING MATRIX

| Counter | Increment authority | Cancel decrement | Other terminal paths | Selected slice |
|---|---|---|---|---|
| `waiting_waitq_count_` | successful WaitQueue registration under G/W | every WaitNode cancel winner, guarded against underflow, AT THE CALL SITE | wake/grant/expire/admission rollback | stays at each of the six primitive call sites; the helper does not own it (review 5041406830 P1) |
| ordinary `active_deadline_count_` | AC-2b arm helper | AC-2b retire reached by timer closure | AC-2b consume/retire | reused, not changed |
| Queue `active_wait_associations_` | Queue successful role registration | Queue-local tail after shared closure | grant/expire/inline/close paths | remains Queue-local |
| Queue `active_queue_timers_` | Queue timer hook/arm | existing owner hook through timer retire | consume/retire paths | unchanged |
| Queue `granted_not_resumed_` | Queue grant publication | N/A; cancel grants no resource | resumed grant path | untouched |
| Condition `active_waits_` | outer Condition wait epoch | outer wait/reacquire completion | all Condition outcomes | untouched |
| RwLock reader/writer state | grant/unlock authority | no direct decrement; head reconcile may grant | wake/expire/unlock | untouched |
| Select counters | Select registration/finalizer | N/A | Select winner/rollback | untouched |

## PRIMITIVE RECONCILIATION

- Event: none; cancel does not set/reset the Event.
- Semaphore: none; cancel neither creates nor consumes a permit. A later
  `release` owns the next grant.
- AsyncMutex: none; cancel never transfers ownership. The owner/unlock handoff
  authority remains separate.
- AsyncCondition: none inside cancellation; the resumed waiter must execute
  the existing mandatory Mutex reacquire epoch.
- AsyncQueue: no public wait cancellation in v1; the internal seam preserves
  leases/ring state and its Queue-local accounting. No new cross-role callback.
- AsyncRwLock: **required**. Removing a head may expose an admissible writer or
  maximal reader prefix. Existing `rwlock_grant_from_head_locked` runs after W
  release and publishes grants before the cancel winner. This remains local.
- Generic WaitQueue: none.

## PUBLICATION MAP

- Terminal visibility begins at the successful WaitNode CAS under W.
- Structural detachment, timer retirement, and global wait retirement happen
  before any runnable publication.
- Event/Semaphore/Mutex/Condition/Queue call the existing canonical
  `publish_waiting_fiber_runnable_locked` while retaining their current lock
  horizon.
- RwLock captures Fiber and owner under W, releases W, reconciles/publishes
  grants, then publishes the canceled Fiber manually under G.
- A null Fiber is valid for the internal synthetic deadline seam. Primitive
  APIs still report a terminal win; generic cancel may report false because it
  couples its bool to publication. The helper performs no publication and
  therefore cannot conflate these contracts.
- Completion publication and Select result publication are unrelated.

## DRIFT EVIDENCE

- Six functions repeat the same five semantic decisions; comments explicitly
  say they “mirror” Event/Semaphore/Mutex cancellation.
- Event's corrective history introduced the exact membership gate, after
  which later primitive seams copied the same proof. Today each copy can drift
  on membership ordering, timer closure, or global accounting.
- The generic path already demonstrates return-value drift: `od_w3` records a
  fiberless terminal winner with `false` return. Treating all bools alike would
  be a regression.
- Queue adds `active_wait_associations_`; centralizing that local counter would
  leak primitive policy.
- RwLock adds owner capture, W release, head reconciliation, grant-before-cancel
  publication, and writer-fair policy. Centralizing those steps would create a
  callback framework and obscure the load-bearing order.
- Select uses a different group identity and finalizer, while Completion uses
  RequestKey/generation and delivery leases. Textual similarity is not shared
  authority.

## S0/S1/S2/S3 CLASSIFICATION

| Finding | Class | Rationale |
|---|---|---|
| Similar `Fiber*` null checks and publication calls | S1 | Existing publication helper already owns routing; call-site placement differs. |
| Generic vs primitive cancel function shape | S0 | Caller contract and bool semantics differ; no shared new authority justified. |
| Queue/RwLock/local reconcile tails | S2 policy similarity at most | Correctness is real but primitive-specific, not one decision. |
| Primitive exact membership gate | S3 | Six locations decide whether this primitive owns the supplied epoch. |
| Primitive CAS/unlink + timer/global accounting closure | S3 | Six locations independently decide the same winner-only terminal obligations under identical locks. |
| Completion/Select cancellation/finalization | S0 boundary | Different identities, state machines, and results. |

## TEST / FORMAL WITNESSES

Independent runtime witnesses already present:

- Generic single-winner/absorbing/unlink: `wait_queue_test`,
  `wait_queue_resolution_authority_test`, `wait_queue_unlink_topology_test`.
- Timer closure: `timer_cancel_wins_timer_loses`, `od_w2`, `od_w3`.
- Cross-primitive membership: `parity_d4_event_*`, `parity_d4_semaphore_*`,
  `parity_d4_mutex_*`; Condition T30/T31; Semaphore T19–T25; Mutex T7–T10;
  Event corrective cases; RwLock T13.
- Primitive policy: Mutex T15/T16/T22; Condition T12/T18; RwLock T6,
  head-reader-prefix/FIFO/R1–R5/MW cases.
- TSan sensitivity: cross-primitive races and RwLock multi-worker cases run in
  the repository's TSan test group.
- Death/negative compile: no new death condition or public reachability is
  introduced; existing primitive lifetime and authority probes remain gates.

Formal witnesses:

- `e10-waitnode`: exactly one terminal winner, winner-owned unlink, absorbing
  terminal, no duplicate wake; includes a required negative model.
- `e11-timer-wait`: active timer consume/retire race closure.
- `e12-event`, `e12-semaphore`, `e12-async-mutex`,
  `e12-async-condition`, `e12-queue`, `e12-rwlock`, and
  `e12-rwlock-scheduler-liveness`: primitive refinements and policy tails.
- E13 Select and F1 wait-record models establish exclusion boundaries.

The selected refactor changes no modeled state or transition. It changes the
production location implementing an already-modeled transition. Relevant E10,
E11, and E12 suites must nevertheless run after implementation. A new model is
not justified.

Stage B will add/extend an independent public-behavior characterization only
if the exact selected conjunction (membership loser has zero timer/accounting
effect; correct-object cancel closes the timed epoch) is not already directly
witnessed in one case. Such a behavior-preserving characterization is expected
to pass before and after; manufacturing a RED test for helper existence would
test implementation preference and violate AC-11.

## AI / MAINTAINER REASONING

After the selected slice, one private declaration/definition answers six
frontend-neutral questions: membership proof, winning CAS, unlink ownership,
and timer closure. Each call site then answers only its local questions:
which private queue is targeted, the one-line `waiting_waitq_count_`
retirement (stackful bookkeeping, review 5041406830 P1), which local counters
close, whether head policy reconciles, and how/when the current stackful
frontend is published. This reduces the semantic search space without hiding
policy behind callbacks or normalizing distinct return contracts.

## RANKED CANDIDATE SLICES

Scores: 1 = low, 5 = high. For leakage/risk/pressure/blast/performance, lower
is better.

| Rank/candidate | S3 evidence | FN | compression | policy leakage | lock risk | callback pressure | sites | coverage | blast | perf effect | Recommendation |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| 1. Membership-gated primitive terminal closure: contains + cancel/unlink + AC-2b retire (global-count retirement removed by review 5041406830 P1) | 5 | 5 | 5 | 1 | 1 | 1 | 6 | 5 | 2 | one out-of-line call; no new work | **SELECT** |
| 2. Membership gate only | 5 | 5 | 2 | 1 | 1 | 1 | 6 | 5 | 1 | neutral | Reject as under-compression; leaves winner closure independently decided six times |
| 3. Timer-retire + global count pair | 4 | 5 | 3 | 1 | 1 | 1 | 7 | 4 | 2 | neutral | Reject: separates ownership proof/CAS/unlink from its winner-only consequences |
| 4. Cancellation publication prelude | 2 | 1 | 2 | 4 | 3 | 4 | 7 | 4 | 4 | possible extra routing abstraction | Reject: stackful and RwLock ordering differs |
| 5. Whole cancel normalization with callbacks | 2 | 2 | 3 | 5 | 5 | 5 | 7 | 4 | 5 | indirection on hot path | Reject: callback soup and primitive-policy leakage |

## SELECTED SLICE

Add exactly one private Scheduler helper, tentatively named
`cancel_primitive_wait_locked(WaitQueue&, WaitNode&)`, with the contract:

```text
PRE: caller holds G and the supplied queue's W
if node is not a member: false, no mutation
if Cancelled CAS loses: false, no mutation
if it wins:
    cancel_locked performs winner-owned unlink
    retire_timer_for_node_locked reuses AC-2b
    return true
CALLER, after a true return and before publication:
    if (waiting_waitq_count_ > 0) --waiting_waitq_count_   [stackful bookkeeping]
NO publication, Fiber/Worker lookup, direct primitive-policy mutation,
reconcile, new callback, allocation, locking, Scheduler wait accounting, or
public return-policy conversion. AC-2b timer retirement may fire an
already-installed Queue on_resolve hook and thereby retire
active_queue_timers_.
```

Use it in Event, Semaphore, AsyncMutex, AsyncCondition, internal Queue, and
AsyncRwLock cancellation seams. Keep each caller's current G/W guards so the
lock horizon is byte-for-byte conceptually unchanged. Do not use it in generic
`cancel_wait` because that API intentionally has no membership scan and has a
different bool contract.

## REJECTED ALTERNATIVES

- Change `WaitNode::resolve_` or `WaitQueue::cancel_locked`: rejected; the
  single-winner/unlink core is already authoritative and is a hard boundary.
- Make generic `cancel_wait` the common helper: rejected; its caller
  precondition and publication-coupled bool differ.
- Add a callback/template framework for local counter/reconcile/publication:
  rejected; obscures source order and leaks primitive policy.
- Centralize Queue counters or close: rejected; public Queue v1 has no cancel
  API and close is not cancellation.
- Centralize RwLock reconcile: rejected; writer fairness and grant-before-
  cancel publication are primitive authority.
- Merge Completion/I/O cancellation or Select: rejected; identity and state
  machines are distinct under AC-9.

## IMPLEMENTATION BOUNDARY

Allowed files:

- private Scheduler declaration/comment in `include/sluice/async/scheduler.hpp`;
- one production definition in the most local Scheduler source TU;
- replacement of the five-step duplicated closure in exactly six Scheduler
  cancel seams;
- one focused public-behavior characterization in an existing test TU if
  required by the witness audit;
- governing current architecture documentation only if a comment/reference
  would otherwise become false.

Not allowed: public API or ABI/layout change; new state; callback; dynamic
allocation; new lock; moving lock boundaries; fairness/resource/reacquire/
close/Select/Completion changes; formatting unrelated code.

## ARCHITECTURE COMPLIANCE GATE 0–4

This record links and instantiates
`docs/architecture/design-compliance-gate.md` before production work.

### Gate 0 — classification

```text
Affected capability:    primitive / Scheduler WaitQueue integration
Affected layer:         E10-E12 scheduler
Classification:         Corrective semantic-authority compression; behavior preserving
Governing ADR:          ADR-execution-model; current async-synchronization;
                        ADR-async-primitive-lifetime-failfast (destruction unchanged)
Conformance map change: no
Constitution rules:     AC-1, AC-6, AC-9, AC-10, AC-11, AC-12
```

Zig classification: the conformance row remains `P` semantic / `I` mechanism
for cancelable sync waits (no uncancelable twin) and `F` semantic / `I`
mechanism for I/O-aware primitives. The source-derived `zig/` tree is not
present or built, by repository contract. This refactor neither improves nor
worsens conformance; no divergence-registry update is required.

### Gate 1 — state machine and authority

```text
Detached/foreign/terminal
  -- primitive cancel --> unchanged; false

Registered + member
  -- contains under G/W --> eligible
  -- WaitNode CAS Registered -> Cancelled --> unique winner
  -- unlink under same W CS --> detached structurally
  -- timer ACTIVE -> RETIRED if bound --> AC-2b
  -- global wait count -1 (caller, one guarded decrement) --> logical wait retired
  -- local primitive reconcile --> unchanged caller authority
  -- optional Fiber publication --> unchanged caller authority
```

All transitions are allocation-free and non-throwing. Losing paths are no-op.
No new failure class or shutdown transition exists. Destruction remains
quiescence-only named fail-fast; no implicit cancel/drain is introduced.

Lock/atomic authority table:

| Fact | Authority | Domain |
|---|---|---|
| target membership | target WaitQueue list | G + target W |
| terminal winner | `WaitNode::state_` CAS | target W, acq_rel atomic |
| unlink | winning `WaitQueue::cancel_locked` | target W |
| timer retire/cache | AC-2b deadline helpers | G (W retained by caller) |
| global wait retirement | Scheduler caller after the helper returns true | G |
| Queue association retirement | Queue caller | existing G + role W |
| Queue timer retirement hook | existing AC-2b timer retirement | existing G + role W |
| RwLock head reconcile | RwLock caller/helper | G, then its existing W acquisition |
| runnable publication | existing Scheduler routing helper/caller | G; existing W horizon per caller |

### Gate 2 — resources and failures

- Construction/submit/completion resources: none added.
- Hot-path allocation: none before or after.
- WaitQueue capacity/lifetime: caller-owned intrusive nodes, bounded by active
  admitted waits; unchanged.
- Membership remains the existing O(N) scan. No second traversal or cache.
- Full/OOM/reclamation behavior: N/A; no container or allocation changes.
- Typed/no-op failure: nonmember or losing CAS returns false before any
  terminal/accounting side effect.

### Gate 3 — progress and wake

- The helper does not block, suspend, route, notify, or publish.
- Persistent state is written in the existing order before caller publication:
  Cancelled + unlinked + timer/global accounting closed.
- Existing callers remain the signal producers via the existing canonical
  runnable route. The waiting Fiber remains the consumer; `make_runnable` is
  the exactly-once guard.
- Commit-to-sleep closure, wake epoch, worst-case latency, one-worker
  liveness, shutdown wake, and polling dependencies are unchanged.

### Gate 4 — evidence results

- Focused characterization: primitive membership loser has zero mutation and
  correct-object timed cancel closes terminal/timer/global accounting — PASS.
- Focused existing primitive tests and full Clang Debug suite — PASS (192/192).
- Production `sluice_async` explicit build — PASS.
- Release build/tests because a public installed header is touched — PASS
  (192/192).
- TSan full test group because synchronization code is touched — PASS
  (192/192; no TSan report).
- ASan+UBSan: required conservatively for ownership-sensitive intrusive queue
  refactor — PASS (192/192; no sanitizer report).
- Relevant formal suites: E10, E11, E12 Event/Sem/Mutex/Condition/Queue/RwLock
  and RwLock scheduler-liveness — PASS, including the suites' expected
  negative-model counterexamples and compile probes.
- Negative compile, claim/assert hygiene, architecture/mechanical/docs,
  whitespace, and pre-push gates — PASS.
- Benchmark: N/A; no performance claim. Review must confirm no added
  traversal/allocation/lock/callback and only one out-of-line helper call.

## REQUIRED GATES

1. Focused selected witness before and after production edit.
2. `xmake build sluice_core`, `xmake build sluice_async`, test group, full tests
   in Linux Clang Debug.
3. Linux Clang Release production/test gate.
4. ASan+UBSan full applicable test gate.
5. TSan full applicable test gate.
6. E10/E11 and all affected E12 formal suites, including required negatives
   and reachability checks through `scripts/formal/verify.py`.
7. Repository negative-compile, architecture/mechanical, whitespace, docs,
   assert hygiene, and `scripts/gates/pre-push.sh` gates.
8. Final diff/status proof; explicitly exclude this untracked report.
9. Adversarial code review and draft PR; do not merge or mark ready.

## WHAT MUST NOT CHANGE

- `WaitNode::resolve_`, WaitOutcome values, or absorbing-state semantics.
- Winner-owned unlink law or exact membership safety.
- `Scheduler::cancel_wait` caller precondition or publication-coupled bool.
- Public primitive cancel signatures or terminal-winner bool meaning.
- Any lock acquisition, order, or critical-section horizon.
- Timer arm/consume/retire/cache behavior established by AC-2b.
- Event SET/RESET semantics; Semaphore permits; Mutex ownership/handoff;
  Condition reacquire; Queue close/teardown/ring/lease/counters; RwLock writer
  fairness/reader batching/grant order.
- Fiber ownership, routing, `make_runnable`, wake epochs, or physical switching.
- Select group authority, Completion waiter delivery, I/O cancellation, task
  cancellation, shutdown, destruction, public API/ABI, capacity, or allocation.
