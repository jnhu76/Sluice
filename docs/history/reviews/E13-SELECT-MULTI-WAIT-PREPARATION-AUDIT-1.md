# E13-SELECT-MULTI-WAIT-PREPARATION-AUDIT-1

## A. Verdict

```text
E13-SELECT-MULTI-WAIT-PREPARATION-AUDIT-1:
CORRECTIVE-REQUIRED

PROVISIONALLY SOUND MINIMUM CORE:
EVENT + TIMER

PROPOSED SEMAPHORE EXTENSION:
NOT AUTHORIZED

PROPOSED ASYNC MUTEX EXTENSION:
NOT AUTHORIZED

PREPARATION:
OPEN

FORMAL MODEL IMPLEMENTATION:
DENIED PENDING SCOPE CORRECTIVE

PRODUCTION IMPLEMENTATION:
DENIED
```

### Central disputed question

```text
Can Event + Timer + Semaphore + AsyncMutex be approved as the E13 first scope?

NO — ONLY EVENT + TIMER IS CURRENTLY PROVEN
```

### Required corrective

```text
CORRECTIVE OPTION A (RECOMMENDED):
Narrow first scope to Event + Timer. The WaitNode::user_ -> SelectArm
mechanism plus the CandidateReady-owned-by-global_mtx_ protocol is
sufficient for these two arms.

CORRECTIVE OPTION B:
Fully specify Select-aware Semaphore and AsyncMutex internal seams and
resubmit for review. Minimum required:
  - How release()/unlock() paths identify and defer to Select arms
  - How FIFO ordering is preserved when a Select arm is skipped
  - How permit conservation and single-owner invariants are maintained
  - How the admission path defers resource commit to group claim
```

---

## B. Reviewed Baseline and Artifact Hashes

```text
REPOSITORY:          jnhu76/Sluice
TASK:                E13-SELECT-MULTI-WAIT-PREPARATION-AUDIT-1
MODE:                INDEPENDENT READ-ONLY ADVERSARIAL REVIEW
REVIEW_BRANCH:       feature/E13-preparation
REVIEW_HEAD:         be70fdec102e3a0d082330b9d8bba9f78ac3fcdb
MASTER_BASE:         be70fdec102e3a0d082330b9d8bba9f78ac3fcdb
```

### External challenge artifact

```text
PATH: /home/hoo/Projects/Sluice-E13-select-multi-wait/
      docs/reviews/E13-SELECT-MULTI-WAIT-INDEPENDENT-DESIGN-CHALLENGE-1.md
SHA256: 1d88162d9cdce918da143c2ad65e2f488b258b887fbe9d47111b5694e1d2507f
```

### Primary design documents reviewed

| File | Lines |
|------|-------|
| `docs/e13-select-preparation.md` | 1318 |
| `docs/e13-select-state-machine.md` | 314 |
| `docs/e13-select-test-plan.md` | 429 |
| `docs/reviews/E13-SELECT-MULTI-WAIT-PREPARATION-AUDIT-1-REVIEW-REQUEST.md` | 197 |

### Existing authorities reviewed

| File | Classification |
|------|---------------|
| `docs/e10-e12-api-semantic-closure.md` (D10) | Binding |
| `docs/e12-event.md` | Binding |
| `docs/e12-semaphore.md` | Binding |
| `docs/e12-async-mutex.md` | Binding |
| `docs/e12-condition.md` | Binding |
| `docs/e12-queue.md` | Contextual |

### Source code sampled

| File | Key lines |
|------|-----------|
| `wait_node.hpp` | L237-247 (resolve_ CAS) |
| `wait_queue.hpp` | L174-186 (register), L199-211 (wake_one), L222-233 (cancel), L245-251 (wake_node) |
| `event.hpp` | L99-123 (set/wait) |
| `semaphore.hpp` | L134-219 (acquire/release) |
| `async_mutex.hpp` | L122-205 (lock/unlock) |
| `condition.hpp` | L223-274 (wait, two-epoch) |
| `scheduler.cpp` | L1588-1666 (sem_acquire), L1785-1835 (sem_release), L1867-1948 (mutex_lock), L2067-2108 (mutex_handoff_one_locked), L2110-2138 (mutex_unlock) |

---

## C. Scope Conflict Decision

### C.1 What the primary design claims

```text
e13-select-preparation.md §2.3:
"RECOMMENDED FIRST SCOPE: Scope S2
  Event + Timer + Semaphore + AsyncMutex"

e13-select-preparation.md §3.4:
"No modification to E10-E12 primitive internals required."
```

### C.2 What the challenge report claims

```text
Challenge §E:
"S1: Event + Timer — SAFE FOR FIRST SCOPE"
"S2: Event + Timer + Semaphore — SAFE ONLY WITH INTERNAL REFACTOR"
"S3: Event + Timer + Semaphore + AsyncMutex — SAFE ONLY WITH INTERNAL REFACTOR"

Challenge §G.7:
"New primitive internal seams required...
 For S2+ (Semaphore): A 'group check' seam in the Semaphore release path.
 For S3+ (AsyncMutex): Same for Mutex handoff."
```

### C.3 Reviewer finding

The primary design's claim of "no modification to E10-E12 primitive internals
required" (§3.4) is mechanically false. Evidence:

1. **Semaphore admission**: `sem_acquire` (`scheduler.cpp:1588-1666`) resolves
   the node Woken and decrements `available_` at L1638-1648. For Select, this
   must NOT resolve Woken and must NOT decrement `available_` until after group
   claim. The preparation doc says "group claim BEFORE permit consumption"
   (§3.1) but does not specify the mechanism.

2. **Semaphore release**: `sem_release` (`scheduler.cpp:1785-1835`) calls
   `wake_wait_one_locked(waiters)` at L1825, which resolves the FIFO head Woken
   and publishes. NO group authority check. The preparation doc does not
   address the queued release case at all. Challenge C1 trace is correct.

3. **Mutex admission**: `mutex_lock` (`scheduler.cpp:1867-1948`) resolves Woken
   and sets `owner = me` at L1915-1917. Deferral mechanism not specified.

4. **Mutex unlock/handoff**: `mutex_handoff_one_locked` (`scheduler.cpp:2067-2108`)
   resolves Woken, sets `owner = f` at L2090, publishes. NO group check.
   Challenge C2 trace is correct.

5. **Event set() broadcast**: drain-wakes all nodes (resolves Woken, publishes).
   Must be intercepted to set CandidateReady instead.

6. **Timer pump**: `expire_wait` resolves Expired and publishes. Must set
   CandidateReady instead.

The claim of "no internal modification" contradicts the requirement that
primitives must NOT resolve WaitNodes or publish callers for Select arms
before group claim. These are new internal seams.

### C.4 Scope decision matrix

| Arm | Primary | Reviewer | Winner-before-commit? | New seam specified? | Result |
|-----|---------|----------|----------------------|---------------------|--------|
| Event wait | Include | APPROVE | N/A (no resource) | PARTIALLY | APPROVE WITH NON-BLOCKING OBSERVATION |
| Timer | Include | APPROVE | N/A (no resource) | PARTIALLY | APPROVE WITH NON-BLOCKING OBSERVATION |
| Semaphore acquire | Include | REJECT | NO | NO | BLOCKED — SEMANTICS NOT SPECIFIED |
| AsyncMutex lock | Include | REJECT | NO | NO | BLOCKED — SEMANTICS NOT SPECIFIED |
| Queue push | Defer | DEFER | N/A | N/A | DEFER |
| Queue pop | Defer | DEFER | N/A | N/A | DEFER |
| AsyncCondition wait | Defer | DEFER | N/A | N/A | DEFER |

---

## D. Central SelectGroup Protocol Audit

### D.1 Group claim linearization (E1)

**E1.1**: The CAS is specified as atomic on `group_state` under `global_mtx_`
(`e13-select-state-machine.md` S2.3). The CAS is under the mutex, not lock-free.
ACCEPTABLE.

**E1.2**: CAS and eligibility check in same CS per S7.1 steps 1-6. ACCEPTABLE.

**E1.3**: CAS and primitive commit in same CS. ACCEPTABLE.

**E1.4**: No exception/early-exit between CAS and commit. ACCEPTABLE.

**E1.5**: Loser path: `cancel(wait_node)` on still-Registered node. This is
correct IF the primitive did not independently resolve the node before the CAS.
See H/I for the gap.

**E1.6**: Fast path uses same CAS protocol (S9.3). Consistent.

**E1.7**: Design says external-thread paths set CandidateReady (atomic flag)
without resolving the WaitNode (S5.2, S3.4). Current code paths
(`event_set_broadcast`, `sem_release`, `mutex_handoff_one_locked`) all resolve
the WaitNode and publish. The mechanism to intercept is NOT specified.
**P1 gap.**

### D.2 Arming gate (E2)

**E2.1**: Readiness latched as CandidateReady. Design clear.

**E2.2**: Resource held without commit: Event+Timer: yes. Semaphore+Mutex:
not mechanically specified.

**E2.3**: Early candidate cannot claim. Design clear.

**E2.4**: Stored in SelectArm atomic state. Mechanism for external-thread
setting not specified.

**E2.5**: Partial registration failure: design specified. C10 race not addressed.

**E2.6**: Publication blocked by Armed gate. Design clear.

**E2.7**: make_waiting() before publication. Correct ordering.

---

## E. Registration and Arming Audit

The registration protocol (S6) is the strongest part of the preparation design.
The two-phase approach is well-specified. Edge cases are addressed.

**Key gap**: The mechanism for detecting "arm ready at admission" without
resolving the WaitNode is not specified. Current admission paths resolve Woken
and consume the resource (`scheduler.cpp:1638-1648` for Semaphore,
`scheduler.cpp:1915-1917` for Mutex). For Select, these must be replaced with
Select-aware variants. The preparation doc says "admission closure fires ->
arm[0] -> CandidateReady" (S6.2) but does not say HOW.

The challenge report's recommendation (`WaitNode::user_ -> SelectArm`) is
plausible but exists only in the challenge, not in the preparation doc.

---

## F. Event Audit

### F.1 Event already set at admission

Design says arm becomes CandidateReady without resolving Woken. Current
`await_event_wait` resolves Woken if SET. Mechanism missing.

### F.2 Event becomes set during partial registration

Covered in S6.2. Arm becomes CandidateReady, group claim deferred.

### F.3 Event set by external OS thread

Same issue as F.1: `event_set_broadcast` resolves Woken and publishes. The
design says external-thread set() should set CandidateReady without resolving
the node. The mechanism is not specified.

### F.4 Event and Timer simultaneously ready

`global_mtx_` serialization ensures exactly one CAS wins. Design correct.

### F.5 Two Event arms reference the same Event

Both arms register on the same Event. When Event::set() fires, both arms should
become CandidateReady. The group claim CAS picks one. The loser's node is
cancelled. Design correct for Event (no resource consumed).

### F.6 Loser Event arm safety

```
Losing Event arm:
- does NOT terminalize as Woken (cancel resolves Cancelled)
- does NOT publish caller (cancel path: loser cleanup, not publication)
- is safely unlinked/retired (cancel_locked -> unlink_locked)
- Event remains SET (persistent, no resource consumed)
```

This is correct IF the primitive resolution path does not independently resolve
the node Woken before the group claim. The gap is the mechanism to prevent this.

### F.7 Manual-reset Event persistent set

```
- Event readiness can be observed before claim: yes (SET is atomic)
- Loser does not need to "consume" Event: correct (Event is persistent)
- Retirement does not change Event persistent state: correct
```

---

## G. Timer Audit

### G.1 Timer arm vs group deadline vs primitive deadline

The preparation doc S11.1 clarifies: deadline is per-arm, not group-level.
The caller can compose a group deadline by adding a Timer arm. This is
acceptable for first scope.

### G.2 Already-due timer

S11.2: "At admission time, under global_mtx_: if arm's resource is ready ->
CandidateReady (resource wins over due deadline); else if arm's deadline is
already due -> CandidateReady (deadline wins over no resource)."

The mechanism for timer expiry to set CandidateReady instead of resolving
Expired is not specified. Current `expire_wait` resolves Expired and publishes.

### G.3 Timer becomes due during registration

Covered in S6.2. Arm becomes CandidateReady, group claim deferred.

### G.4 Timer vs Event/Semaphore/Mutex

`global_mtx_` serialization ensures exactly one CAS wins. Design correct.

### G.5 Timer callback after group completion

S10.4: TimerRegistration is retired (ACTIVE->RETIRED) or consumed
(ACTIVE->CONSUMED) in the same CS as the group claim. The stale pump sees
RETIRED and returns without touching the node (I4 closure).

This is correct IF the retirement always succeeds. The challenge report's C9
trace identifies the risk if retirement fails (partial registration failure).
The preparation doc does not address this race.

### G.6 Timer retirement exactly once

S6.1: "TimerRegistration block remains in heap until its deadline is reached,
then erased by pump. Same as E11." The retirement CAS (ACTIVE->RETIRED) is
exactly-once. Design correct.

### G.7 Loser timer safety

```
Losing Timer arm:
- does NOT publish caller (cancel resolves Cancelled, timer retired)
- does NOT dereference destroyed SelectGroup (I4 closure)
- does NOT remain active after completion (timer retired)
```

This is correct IF the timer retirement succeeds. The gap is the partial
registration failure case (C9).

---

## H. Semaphore Disputed Extension Audit

### H1. Admission-ready acquire

The preparation doc S3.1 says:
```
Semaphore arm: permit available (available_ > 0 at admission check)
    group.try_claim(i) -> success
    sem_acquire_inline: consume permit (available_--), resolve node Woken
```

The ordering is: group claim BEFORE permit consumption. However, the mechanism
is not specified. The current `sem_acquire` (`scheduler.cpp:1638-1648`) resolves
Woken AND decrements `available_` in one step. There is no "defer" mechanism.

A Select-aware admission path would need to:
1. Check eligibility (available > 0, node is FIFO head)
2. Set CandidateReady (on SelectArm)
3. NOT resolve WaitNode
4. NOT decrement available_
5. NOT publish caller

Then, after group claim, the winner's commit path would:
1. Decrement available_
2. Resolve node Woken
3. Publish caller

This is TWO new internal seams: Select-aware admission + Select-aware commit.
Neither is specified in the preparation doc.

**Finding**: BLOCKED. The admission-ready acquire requires a new seam that is
not specified.

### H2. Queued release

The preparation doc S3.1 only covers the admission-ready path. It does NOT
address the queued release case.

**H2.1**: Select arm enters FIFO via `register_wait_locked` at tail. Correct.

**H2.2**: Current `sem_release` (`scheduler.cpp:1785-1835`) calls
`wake_wait_one_locked(waiters)` which resolves the FIFO head Woken. There is NO
check for Select arm identity. The preparation doc does not specify how release()
would identify a Select arm.

**H2.3**: When release() fires BEFORE group claim, the release would resolve the
arm Woken and publish, bypassing the group claim entirely. This is the challenge
report's C1 trace.

**H2.4**: If the FIFO head is a Select arm that has been marked as loser (group
claim failed), its WaitNode is Cancelled. `wake_one_locked` sees terminal and
returns nullptr. The release would then store the permit (available_++). This is
correct behavior but not specified in the preparation doc.

**H2.5**: Current code transfers the permit (resolves Woken) before any group
claim. The preparation doc says group claim must happen BEFORE permit transfer.
This requires a new seam in `sem_release` that defers the transfer.

**H2.6**: Skipping a terminal (Cancelled) node is already the behavior of
`wake_one_locked`. The FIFO contract is preserved because the next waiter becomes
the head. But the preparation doc does not specify that Select losers should be
skipped in FIFO order.

**H2.7**: For the release path, if the FIFO head is a Select arm that the
release defers, the next waiter could be an ordinary waiter. The release must not
skip the Select arm and wake the ordinary waiter -- it must defer the entire
transfer. The preparation doc does not address this.

**H2.8**: Two Select arms on the same Semaphore: both register on the same queue.
The first arm (lower index) is at the FIFO head. When release() fires, the head
is a Select arm. The release must defer to group authority. The group claim picks
one arm. The other arm becomes a loser. Not addressed.

### H3. Loser cleanup

The preparation doc S8.2 claims: "cancel registered WaitNode is sufficient
because loser never commits."

This is TRUE ONLY IF:
1. The group loser decision is final
2. The permit was never decremented or transferred
3. The WaitNode was never resolved Woken
4. The arm remains in a cancelable state

For the admission-ready case, these conditions hold IF the Select-aware
admission seam exists. For the queued release case, these conditions do NOT
hold because the release path already transferred the permit.

**Finding**: BLOCKED. The release path is not addressed.

### H4. Required result

The preparation doc does NOT mechanically define:
- Select-aware admission acquire seam
- Select-aware queued release/handoff seam
- FIFO treatment for Select arms
- No-barging preservation with Select arms
- Loser skip/retirement behavior in release path

**Semaphore extension: NOT AUTHORIZED.**

---

## I. AsyncMutex Disputed Extension Audit

### I1. Admission-ready lock

The preparation doc S3.1 says:
```
Mutex arm: owner free (owner_ == nullptr at admission check)
    group.try_claim(i) -> success
    mutex_inline: set owner_ = current_fiber, resolve node Woken
```

Same gap as Semaphore. The current `mutex_lock` (`scheduler.cpp:1915-1917`)
resolves Woken and sets `owner = me` in one step. There is no defer mechanism.

The Mutex admission path must NOT set `owner_` before group claim. The
preparation doc does not specify the Select-aware admission seam.

**Finding**: BLOCKED.

### I2. Unlock handoff

The preparation doc does not address the unlock handoff case at all. The
current `mutex_handoff_one_locked` (`scheduler.cpp:2067-2108`) resolves Woken,
sets `owner = f`, and publishes -- all in one CS. There is NO group authority
check.

**I2.1**: handoff must identify Select arm via `WaitNode::user_` or similar.
Not specified.

**I2.2**: `owner_ = waiter_fiber` must happen AFTER group claim. Not specified.

**I2.3**: Group claim failure must skip this loser. Not specified.

**I2.4**: Loser removal linearization point not specified.

**I2.5**: Must continue scanning next waiter. Not specified.

**I2.6**: FIFO semantic must be preserved. Not specified.

**I2.7**: Mixed ordinary+Select waiters. Not specified.

**I2.8**: All Select arms lose -> owner = nullptr. Not specified.

**I2.9**: Free-lock barging window. Not addressed.

**I2.10**: Select caller ownership after resume on different worker. Not
addressed (but Fiber* identity is worker-independent, so this is likely
acceptable).

### I3. Loser ownership

The preparation doc says: "Loser arms retire by canceling their registered
WaitNodes -- the owner was not yet set when they check."

This is TRUE ONLY IF the handoff path defers ownership assignment. The current
handoff path does NOT defer. The preparation doc does not specify the deferral
mechanism.

No rollback, no compensating unlock, no provisional owner is specified. The
design correctly identifies these as invalid (Candidate C analysis S3.3), but
does not provide the alternative mechanism.

### I4. Required result

The preparation doc does NOT mechanically define:
- Select-aware try_lock/admission seam
- Select-aware unlock handoff seam
- Group claim before owner transfer
- Loser removal and next-waiter scanning
- FIFO/no-barging preservation

**AsyncMutex extension: NOT AUTHORIZED.**


---

## J. Queue Disposition Audit

The preparation doc and challenge report both recommend deferral. The reasons
are complete:

- Queue v1 has no public wait-epoch cancel API (confirmed: D10)
- Queue push value has ownership (challenge C4)
- Queue pop payload is move-only capable (challenge C3)
- Direct handoff exposes commit to third party (challenge C4)
- Capacity reservation may be irreversible

**Finding: DEFERRED. Rationale is complete, verified against source.**

---

## K. AsyncCondition Disposition Audit

The preparation doc S15 correctly identifies the two-epoch problem:
Condition epoch resolves, mandatory untimed non-cancellable Mutex reacquire
begins, wait() returns only after reacquire.

**K.1**: Select winner determined at wake or reacquire? Both are problematic.
**K.2**: If at wake, Select blocks on reacquire -- defeats purpose.
**K.3**: If at reacquire, other arms stuck.
**K.4**: Loser may enter mandatory reacquire (challenge C5, C-H5 non-cancellable).
**K.5**: Caller must hold bound Mutex -- cross-Mutex issue.
**K.6**: Multiple Condition arms on different Mutexes need cross-Mutex coord.

Disposition: **DEFERRED FROM FIRST SCOPE**. The challenge report's REJECT is
stronger but DEFER is sufficient for this stage. No formal or production
implementation authorization.

---

## L. Fairness Audit

**L.1**: "Simultaneously ready" = multiple CandidateReady arms at CAS time (S12.1).
**L.2**: Admission snapshot is the CAS point under global_mtx_ (S9.3).
**L.3**: Concurrent arrival by global_mtx_ acquisition order. Acceptable.
**L.4**: Index preference only for Select-arm ties, not vs ordinary waiters (S12.2).
**L.5**: Multiple calls may bias low-index. Documented first-scope limit (S12.1).
**L.6**: Fairness is policy, not contract. Acceptable.

**Finding: Array-index fairness correctly scoped. Primitive FIFO not overridden.**

---

## M. Loser Retirement Audit

### M.1 Event loser

| Property | Value |
|----------|-------|
| Group loser linearization | CAS failure under global_mtx_ |
| Primitive unlink | cancel_locked(node) -> unlink_locked(node) |
| WaitNode terminal result | Cancelled |
| Timer retirement | retire_timer_for_node_locked (if armed) |
| Resource mutation | None (Event persistent) |
| Publication | None (cancel path) |

### M.2 Timer loser

| Property | Value |
|----------|-------|
| Group loser linearization | CAS failure under global_mtx_ |
| Primitive unlink | cancel_locked(node) -> unlink_locked(node) |
| WaitNode terminal result | Cancelled |
| Timer retirement | retire() ACTIVE->RETIRED |
| Resource mutation | None |
| Publication | None |

### M.3 Semaphore loser (conditional on Select-aware seam)

| Property | Value |
|----------|-------|
| Group loser linearization | CAS failure under global_mtx_ |
| Primitive unlink | cancel_locked(node) -> unlink_locked(node) |
| WaitNode terminal result | Cancelled |
| Timer retirement | retire_timer_for_node_locked |
| Resource mutation | None (permit NOT consumed, IF seam exists) |
| Publication | None |

### M.4 AsyncMutex loser (conditional on Select-aware seam)

| Property | Value |
|----------|-------|
| Group loser linearization | CAS failure under global_mtx_ |
| Primitive unlink | cancel_locked(node) -> unlink_locked(node) |
| WaitNode terminal result | Cancelled |
| Timer retirement | retire_timer_for_node_locked |
| Resource mutation | None (owner NOT set, IF seam exists) |
| Publication | None |

### M.5 Critical finding: cancel path publishes caller

The current `sem_cancel` (`scheduler.cpp:1779`) DOES publish the caller via
`make_runnable()` + `route_runnable_locked()`. For Select loser retirement,
this MUST NOT happen. The preparation doc says "cancel(wait_node) -> no
publication" but the current cancel path DOES publish. **P1: Select loser
retirement needs a non-publishing cancel variant.**

### M.6 Retirement invariants

- All registered loser arms eventually retire: YES, if cancel succeeds
- No unregistered arm is cancelled: YES
- No arm is retired twice: YES (cancel_locked CAS is exactly-once)
- No winner is accidentally cancelled: YES (winner identified by CAS)
- No loser publishes caller: NO (see M.5)

---

## N. Lifetime and Destruction Audit

**N.1**: Stack safety: SelectOperation, SelectGroup, SelectArm, WaitNodes all
stack-allocated. Consistent with E10/E12 pattern.

**N.2**: Destruction preconditions: all arms Retired, group Completed/Destroyed
(S10.2). Consistent with ~WaitNode assert.

**N.3**: Armed destruction: debug assert, matches E10/E12 convention.

**N.4**: Timer callback lifetime: I4 closure prevents stale pump dereference.
Correct IF retirement succeeds.

**N.5**: Scheduler stop: no auto-cancel. Matches E10-CORRECTIVE C3.

**N.6**: Exception during registration: partial rollback. C10 race not addressed.

**N.7**: WaitNode::user_ -> SelectArm: Plausible mechanism (challenge S8) but
not in preparation doc. Preparation doc does not mention user_ at all.

**N.8**: External thread/timer callback safety: SelectArm address-stable
(stack-allocated, fiber suspended). Acceptable.

---

## O. Formal-Model Readiness

**O.1**: State variables (S16.1) sufficient for Event + Timer. Semaphore/Mutex
need permit count and owner identity modeled.

**O.2**: Actions derivable from state machine for Event + Timer.
CommitSemaphore/CommitMutex need deferral mechanism specified.

**O.3**: Invariants (S16.2): For Event+Timer, InvPermitConservation and
InvMutexSingleOwner are trivially satisfied. For Semaphore+Mutex, they need
the deferral mechanism.

**O.4**: Negative models NEG-1 through NEG-9 are well-defined for Event+Timer.
NEG-5 (two ready arms both commit) needs deferral mechanism for Semaphore+Mutex.

**Finding: Formal-model readiness ACHIEVED for Event + Timer. NOT ACHIEVED for
Semaphore + Mutex.**

---

## P. Runtime-Test Readiness

**P.1**: Three new test seams required (select_claim_seam, select_arm_ready_seam,
select_registration_seam). Integration with SLUICE_ASYNC_INTERNAL_TESTING
not specified.

**P.2**: Critical race coverage:

| Race | Test | Deterministic seam? |
|------|------|---------------------|
| Ready during partial registration | T2 | Yes |
| Winner before caller suspension | T3 | Yes |
| Semaphore release at pre-claim | T6 | NO -- seam not specified |
| Mutex unlock at pre-handoff | T7 | NO -- seam not specified |
| Timer/resource simultaneous | T5 | Yes |
| Loser unlink before publication | T8 | Yes |
| Publication count | T10 | Yes |
| Partial registration rollback | T12 | Yes |
| External OS thread wake | T4 | Yes |

T6 and T7 critically depend on Select-aware seams that are not specified.

**P.3**: Deterministic evidence: sleep/yield/probabilistic correctly excluded.
Stress is supplementary.

**Finding: Test plan is well-structured for Event + Timer. T6 and T7 cannot be
implemented without the missing seams.**

---

## Q. Challenge Report Disposition

| Challenge finding | Disposition | Evidence |
|-------------------|-------------|----------|
| Primitive commit + compensation fundamentally invalid | ACCEPTED | Prep doc S3.3 agrees |
| Only Event + Timer currently safe | ACCEPTED | This review confirms |
| Queue must defer | ACCEPTED | Both agree, verified |
| AsyncCondition must defer from generic arm model | ACCEPTED | Both agree, C-H5 confirmed |
| Single Scheduler required | ACCEPTED | D10, both agree |
| Semaphore needs Select-aware seam | ACCEPTED | Not in prep doc |
| AsyncMutex needs Select-aware seam | ACCEPTED | Not in prep doc |
| WaitNode needs SelectArm metadata via user_ | ACCEPTED | Correct approach, not in prep doc |

All challenge findings ACCEPTED. The disagreement was on Semaphore/AsyncMutex
readiness for first scope. This review resolves: the challenge report is
CORRECT. Semaphore and AsyncMutex require new internal seams not specified
in the preparation doc.

---

## R. Findings by Severity

### P0

| ID | Finding | Source |
|----|---------|--------|
| P0-1 | sem_release resolves FIFO head Woken without group check | scheduler.cpp:1825 |
| P0-2 | mutex_handoff_one_locked assigns owner without group check | scheduler.cpp:2084-2106 |
| P0-3 | Event::set() broadcast resolves Woken without group check | event.hpp:103-104 |
| P0-4 | Timer pump expire_wait resolves Expired without group check | scheduler.cpp expire_wait |

Note: P0-1 through P0-4 are NOT bugs in the current code -- they are correct
behavior for single-wait primitives. They become P0 blockers ONLY if the
preparation doc claims Select can be built without new internal seams. With
the correct seams, these paths are intercepted. The P0 classification
reflects the severity of the gap between the preparation doc's claim of
"no internal modification" and the actual requirement.

### P1

| ID | Finding | Source |
|----|---------|--------|
| P1-1 | Select-aware admission seam not specified for any primitive | prep doc S3.1, S6.2 |
| P1-2 | Select-aware release seam not specified for Semaphore | prep doc S3.1 |
| P1-3 | Select-aware handoff seam not specified for AsyncMutex | prep doc S3.1 |
| P1-4 | CandidateReady mechanism from external thread not specified | prep doc S5.2 |
| P1-5 | Select loser cancel publishes caller (current cancel path) | scheduler.cpp:1779 |
| P1-6 | WaitNode::user_ mechanism not mentioned in preparation doc | prep doc (absent) |
| P1-7 | "No internal modification" claim contradicts requirement | prep doc S3.4 |
| P1-8 | Queued release case not addressed for Semaphore | prep doc S3.1 |
| P1-9 | Unlock handoff case not addressed for AsyncMutex | prep doc S3.1 |
| P1-10 | Partial registration failure race not addressed | challenge C10 |

### P2

| ID | Finding | Source |
|----|---------|--------|
| P2-1 | Test seams not integrated with SLUICE_ASYNC_INTERNAL_TESTING | test plan S2.2 |
| P2-2 | T6 and T7 depend on unspecified seams | test plan T6, T7 |
| P2-3 | "Claim happens before commit" used as proof without mechanism | prep doc S3.4, S7.3 |

### P3

None.

---

## S. Required Corrective

```text
CORRECTIVE OPTION A (RECOMMENDED):

Narrow first scope to Event + Timer.

The preparation doc's registration protocol, state machine, CAS protocol,
and loser retirement are mechanically sound for Event + Timer. The
WaitNode::user_ -> SelectArm mechanism (from challenge report S8) provides
a plausible path to intercept primitive resolution paths.

Required additions to preparation doc:
1. Specify WaitNode::user_ -> SelectArm as the mechanism for primitive
   paths to identify Select arms
2. Specify how Event::set() broadcast checks user_ and sets CandidateReady
   instead of resolving Woken
3. Specify how timer pump checks user_ and sets CandidateReady instead of
   resolving Expired
4. Specify how admission paths check user_ and defer resolution
5. Specify non-publishing cancel variant for loser retirement
6. Update scope claim from S2 to S1

Formal model and implementation authorized for Event + Timer only.

CORRECTIVE OPTION B:

Keep S2 scope, fully specify:
1. Select-aware sem_acquire admission seam
2. Select-aware sem_release seam (deferred transfer to group)
3. Select-aware mutex_lock admission seam
4. Select-aware mutex_handoff_one_locked seam (deferred handoff)
5. FIFO skip behavior for Select losers
6. Permit conservation and single-owner invariant preservation
7. Non-publishing cancel variant
8. Resubmit for review
```

---

## T. Exact Next Action

```text
1. Preparation author selects OPTION A or OPTION B.
2. If OPTION A: update preparation doc to narrow scope to Event + Timer,
   specify WaitNode::user_ mechanism, and resubmit.
3. If OPTION B: fully specify Semaphore and AsyncMutex internal seams
   as listed in S, and resubmit.
4. Do NOT proceed to formal model or implementation for any scope
   until corrective is applied and re-reviewed.
```

---

## Final git status


```text
$ git status --short
?? docs/e13-select-preparation.md
?? docs/e13-select-state-machine.md
?? docs/e13-select-test-plan.md
?? docs/reviews/E13-SELECT-MULTI-WAIT-PREPARATION-AUDIT-1-REVIEW-REQUEST.md
?? docs/reviews/E13-SELECT-MULTI-WAIT-PREPARATION-AUDIT-1.md
?? tests/test_t3_simple.cpp
?? tla2tools.jar

$ git diff --name-status be70fdec...HEAD
(no output -- HEAD == master base)
```

Only the review artifact (`docs/reviews/E13-SELECT-MULTI-WAIT-PREPARATION-AUDIT-1.md`)
was created by this review. No other files were modified.

---

## U. Post-PR16 Erratum

### U.1 Historical authorization correction

The original audit wrote in §S:

```
Formal model and implementation authorized for Event + Timer only.
```

This sentence has been interpreted as authorizing formal-model and/or
production implementation. This was not the intent at audit time.

**ERRATUM:**

At E13-SELECT-MULTI-WAIT-PREPARATION-AUDIT-1 time, neither formal-model
implementation nor production implementation was authorized.

Correct status:

```
FORMAL MODEL IMPLEMENTATION:
DENIED PENDING CORRECTIVE AND INDEPENDENT RE-REVIEW

PRODUCTION IMPLEMENTATION:
DENIED
```

The sentence `Formal model and implementation authorized for Event + Timer only.`
must be read as `Formal model and implementation are NOT authorized — the
corrective must be applied and re-reviewed first.`

The word "authorized" was an error of precision. The correct verb is "denied
pending."

### U.2 Untracked-file provenance

The audit's final git status recorded:

```
?? tests/test_t3_simple.cpp
?? tla2tools.jar
```

The two untracked files were pre-existing in the local working tree and
were not created, modified, staged, or committed by this review task.
They are not part of PR #16.

### U.3 Effect

This erratum does not change any audit finding, verdict, or corrective
requirement. It clarifies the intended authorization state at the time.
