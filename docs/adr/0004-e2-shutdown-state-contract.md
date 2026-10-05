# ADR-0004: E2 Shutdown State Contract and Public Spelling (#401 / #452)

- **Status**: PROPOSED (E2-B slice, gate `E2_B_CONTRACT_DERIVED`)
- **Parent requirements**: SHUT-01, SHUT-02, SHUT-03, SHUT-04, PROG-04,
  THREAD-01, REQ-02/REQ-05/REQ-06, BACKEND-01/BACKEND-03, OBS-02, HANDLE-03,
  BOUND-02, VERIFY-04 (V20–V23, V26)
- **Baseline**: `afd421a6` on `fix/401-e2-shutdown-replacement`
- **Decision boundary**: this ADR freezes only choices the root delegates
  under ADR-01 (names, types, return/result spelling, policy enum spelling,
  internal mechanism representation). Every behavior rule below is derived
  from the cited root text; where the root text is quoted it governs and
  this ADR adds no semantics.

## 1. Public spelling (context lifecycle operations)

On `sluice::async::AsyncIoContext` (the v1 owning context class; the root's
`IoContext` name maps to it until the F-phase rename):

```cpp
enum class ShutdownPolicy : std::uint8_t {
    drain,             // SHUT-02 step 2 "drain"
    cancel_then_drain, // SHUT-02 step 2 "cancel_then_drain"
};

enum class ShutdownOutcome : std::uint8_t {
    completed,         // SHUT-03 completion: physical execution and
                       // notification references retired
    health_unresolved, // pending/unresolved health: safe retirement not
                       // established; borrows/resources retained; context is
                       // NOT execution-closed
    health_failed,     // wait/notification machinery failure (PROG domain)
};

void close_admission() noexcept;                    // any thread, idempotent
void request_stop(ShutdownPolicy policy) noexcept;  // any thread, idempotent
Result<ShutdownOutcome> shutdown(ShutdownPolicy policy); // progress owner only
bool admission_open() const noexcept;               // derived view
bool execution_closed() const noexcept;             // linearization record view
```

- Policies are ordered `drain < cancel_then_drain`; the effective policy is
  the strongest ever requested and never downgrades (SHUT-02 step 2
  "existing explicit close may be upgraded to cancellation by stop, never
  reopened").
- `close_admission()` linearizes on the canonical `RequestCore` admission
  authority under the core mutex (REQ-02) and wakes the owner on the
  open→closed transition only.
- `request_stop(policy)` = `close_admission()` + policy upgrade + owner
  control signal (PROG-04).
- `shutdown(policy)` is the canonical driving shutdown: blocking, no finite
  timeout guarantee, idempotent on an already execution-closed context
  (returns the recorded outcome). It fails with `invalid_state` when called
  by a non-owner/inside a drive, on an inert context, or while an external
  notification interest is live (the host must detach first; PROG-03).

## 2. State table (derived from SHUT-01/SHUT-02)

| State | ENTRY | EXIT | ALLOWED | FORBIDDEN | OBSERVABLE |
|---|---|---|---|---|---|
| Open | construction | close/stop request | everything | — | `admission_open()==true` |
| AdmissionClosed | `close_admission()`/`request_stop()`/exhaustion/poison linearization | owner begins driving shutdown | cancel, polls, observer cancel, existing delivery | new acceptance (REQ-02), new observer attachment (SHUT-02 step 3) | `admission_open()==false`; finite accepted set frozen |
| Settling | owner enters `shutdown()` (or the destructor's settlement drive) | all internal obligations retired, or unresolved health | drive passes, cancel under policy, observer delivery + retirement, progress waits | new acceptance/attachment; nested drive | `shutdown()` has not returned; backend health composed per pass |
| ExecutionClosed | settle converged → backend execution resources stopped/joined/closed → ProgressSource notification retired → transition recorded | last public binding released and host detached | result consume/discard on retained handles; destruction after `Destroyable` | any new work, wait, attachment; any borrow-touching execution | `execution_closed()==true`; `shutdown()` outcome recorded; SHUT-01: retains RequestCore storage for ready unconsumed results |
| Destroyable | `execution_closed() ∧ external_precondition_clear` | context destruction runs | destruction | — | destructor performs remaining cleanup without violating HANDLE-03 |

`ExecutionClosed != Destroyable`: the former is an internal-retirement fact;
the latter additionally requires `external_precondition_clear` (SHUT-01
"public bindings released and host detached").

## 3. Predicates (derived views; no second mutable authority)

```text
external_precondition_clear ≡ core.public_bindings == 0
                           ∧ no live external notification interest
                           ∧ no live external observer registration
                             (armed/queued/delivering episodes are driven to
                              retirement by settlement; what remains is
                              external caller state)
                           ∧ no progress binding live (owner handle/drive)

internal_retirement_complete ≡ core: outstanding == 0
                             ∧ execution_refs == 0
                             ∧ control_refs == 0
                             ∧ publication_inflight == 0
                             ∧ no live observer delivery episode
                             ∧ backend: internal_work_retired()
                               (ThreadPool: dispatch empty ∧ no active worker
                                ∧ no owed publication ∧ no owed delivery event;
                                io_uring: dispatch empty ∧ no owed publication
                                ∧ live_cookies == 0 ∧ live_control_sqes == 0
                                ∧ transport ledger empty)

execution_closed (recorded transition) ≡ internal_retirement_complete held
    ∧ backend execution resources stopped/joined/closed
    ∧ ProgressSource notification retired
    (recorded once by the owner; not recomputed after the underlying
     resources are dismantled)

destroyable ≡ execution_closed ∧ external_precondition_clear
```

`outstanding()==0` is none of these (V20/M1): a published unconsumed slot
has `outstanding()==0` with a live public binding; an internal pin state has
`outstanding()==0` with `public_bindings==0` and live internal obligations.

## 4. Mechanism justification (new production state, per the #390 gate)

| Mechanism | Classification | Writer / linearization | Necessity | Evidence |
|---|---|---|---|---|
| `AsyncIoContext::stop_policy_` | semantic authority for the strongest requested policy | any thread under `access_mtx_`; monotone upgrade | the upgrade rule must survive across threads between `request_stop` and the owner's `shutdown` | M6 + policy-upgrade test |
| `AsyncIoContext::execution_closed_` + recorded outcome | linearization record of the SHUT-02 step-7 transition; after the transition the source facts are dismantled (ring exited, notification retired), so it is not recomputable | the owner thread inside `shutdown()` | SHUT-03 idempotence; refuse new external notification interest; ExecutionClosed≠Destroyable | V20, M1, M4, idempotence test |
| `CoreOccupancy` extension (`execution_refs`, `control_refs`, `publication_inflight`, observer episode count) | derived aggregate view | core mutex | the settlement predicate composes authoritative per-slot facts; the aggregate already exists for `accepted_live/outstanding/public_bindings` | M1, M7, V26 |
| `RequestCore::admission-closed observer rejection` | protocol gate on the existing admission authority | core mutex | SHUT-02 step 3 | observer shutdown test (E2-F) |
| backend seam `stop_execution()` / `retire_execution_resources()` / `internal_work_retired()` / `backend_health_failed()` | mechanism interface (BACKEND-01); per-backend physical facts, no shared representation | backend's own synchronization | the context composes the unified semantic view without forcing a shared backend state layout (root §17 unified predicate, distinct mechanisms) | backend retirement tests; M7, M10 |
| `ProgressSource` notification retirement (fd close at step 7, wake no-op afterwards) | physical obligation | `shutdown()` step 7, single-threaded with the owner | SHUT-02 step 7 "close ProgressSource"; prevents post-close eventfd writes | V23 |

No `destroyable_` flag is added: Destroyable is derived. No context-level
`admission_closed_` second truth: `RequestCore::admission_open_` remains the
single admission authority.

## 5. Settlement driver (the one canonical shutdown path)

```text
entry: drive guard (THREAD-01 owner-only, no nested drive, no external
       notification interest)
1 close admission on the canonical authority; freeze the finite accepted set
2 policy = strongest(requested); if cancel_then_drain: enumerate core
  outstanding keys and issue best-effort cancels (CANCEL-01 dispositions)
3 loop (bounded passes; park through the ProgressSource handshake between
  passes when nothing is actionable):
    pass = one progress pass (existing run_progress_pass_: reap/publish/
           deliver/dispatch/service controls)
    retire delivery episodes claimed by the pass (OBS-02 hook→retire)
    if internal_retirement_complete: break(converged)
    if wait-health failed: break(health_failed)
    if backend/core health failed ∧ pass completed==0 ∧ nothing immediate
       ∧ nothing retryable: break(health_unresolved)   // resources retained
4 stop_execution(); retire_execution_resources()   // SHUT-02 step 6
5 retire ProgressSource notification; record execution_closed + outcome
6 return outcome
```

The destructor runs the same driver after checking the external
preconditions (SHUT-04): fail-fast on detectable external violations, then
own internal retirement (may block), then member cleanup. If settlement
cannot establish safe retirement, the destructor follows the BACKEND-03 /
SHUT-04 fail-fast boundary — it never frees storage over unresolved
borrow-touching work and never fabricates retirement.

## 6. What this contract explicitly preserves

- #400 frozen uring invariants: settlement never settles a kernel-visible
  op by CQ absence, never treats a cancel CQE as original retirement, never
  assumes batch invisibility on negative submit (V22/M5/M9).
- The ThreadPool claim-failed/stale-dispatch worker-lane drain (M10): no
  pre-join dispatch-nonempty violation is reintroduced.
- Publication gating (REQ-03/04), reclaim predicate (REQ-05), observer
  protocol (OBS-01..04), no-lost-wake (PROG-02) are untouched; settlement
  drives the same public passes the driver already runs.
- `shutdown completed` is not "all public results consumed": retained
  results stay consumable after ExecutionClosed (V20/M4).
