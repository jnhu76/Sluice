# E13 Select Formal-to-Production Mapping

**Task:** `E13-SELECT-PRODUCTION-PREPARATION-1`
**Authority:** the per-action mapping table required by task section S. Every
formal action (from `E13SelectContract.tla`, `E13SelectCentralClaim.tla`, and
`E13SelectEventTimer.tla`) is mapped to a concrete planned C++ type and
method, with its lock domain and its linearization/commit point.

The C++ names are the **planned** production names from
`docs/e13-select-production-architecture.md` §8. They are not yet defined; no
production code exists. The mapping is the contract the implementation must
satisfy.

---

## 1. Contract-layer actions (`E13SelectContract.tla`)

| Formal action              | Planned C++ function / method                                       | Lock domain                | Linearization / commit point                          |
|----------------------------|---------------------------------------------------------------------|----------------------------|-------------------------------------------------------|
| `ContractRegisterArm(i)`   | `Scheduler::select_register_arm_locked(SelectGroup&, SelectArmRegistration&)` | `global_mtx_` held         | `arm.state = Registered` (plain write under G); the registry link is the externally visible commit |
| `ContractFinishRegistration` | `SelectGroup::finish_registration_locked()`                       | `global_mtx_` held         | `group.phase_ = Selecting` (plain write under G)      |
| `ContractOfferReadiness(i)` | (Event) `Scheduler::event_scan_marks_candidate_locked` body; (Timer) `Scheduler::select_timer_pump_entry` body | `global_mtx_` held         | `arm.state = CandidateReady` (plain write under G)    |
| `ContractReserveReadiness(i)` | N/A in the first scope (Event/Timer have no reversible reservation; reservation_state stays `None`) | —                          | —                                                     |
| `ContractSuspendCaller`    | `Scheduler::select_suspend_caller_locked(SelectGroup&)`             | `global_mtx_` held (release before `context_switch`) | `me->make_waiting()` under G                          |
| `ContractLinearizeWinner(i)` | `SelectGroup::claim_winner_locked(arm_index)`                      | `global_mtx_` held         | `winner_.compare_exchange_strong(kNoWinner, arm_index, acq_rel, acquire)` — **the single linearization point** |
| `ContractCommitWinner(i)`  | `Scheduler::select_commit_winner_locked(SelectGroup&, arm_index)`   | `global_mtx_` held         | `arm.state = Retired (winner)` + (Timer) `SelectTimerRegistration::try_claim_expiry()` CAS |
| `ContractReleaseLoser(i)`  | `Scheduler::select_finalize_loser_locked(SelectGroup&, arm_index)` | `global_mtx_` held         | `arm.state = Retired (loser)` + (Timer) `SelectTimerRegistration::retire()` CAS |
| `ContractCloseAuthority(i)` | (Event) `SelectPort::unlink_locked(arm)`; (Timer) the consume/retire CAS in the row above | `global_mtx_` held         | Event: `arm` removed from the intrusive list; Timer: `state_ != active` |
| `ContractPublishInline`    | `Scheduler::select_publish_locked(group)` inline branch             | `global_mtx_` held         | `group.result_ = SelectResult{...}` + `group.phase_ = Completed` |
| `ContractPublishSuspended` | `Scheduler::select_publish_locked(group)` suspended branch          | `global_mtx_` held         | `f->make_runnable()` returning true (the exactly-once guard) followed by `group.phase_ = Completed` |
| `ContractResumeCaller`     | (implicit) the worker loop picks up the now-runnable caller         | `global_mtx_` held at route time | `route_runnable_locked(f, owner)`                     |
| `ContractConsumeResult`    | `select_impl` reads `group.result_` after resume                     | `global_mtx_` held (reacquire) | the read of `group.result_` under G                    |
| `ContractDestroyOperation` | `~SelectGroup()` / frame unwind                                      | no lock (precondition: phase ∈ {Consumed, Aborted}) | frame destruction                                    |
| `ContractBeginRollback`    | `Scheduler::select_begin_rollback_locked(SelectGroup&)`             | `global_mtx_` held         | `group.phase_ = Rollback` (plain write under G)       |
| `ContractRollbackRelease(i)` | `Scheduler::select_rollback_arm_locked(SelectGroup&, arm_index)`  | `global_mtx_` held         | Event: `SelectPort::unlink_locked`; Timer: `SelectTimerRegistration::retire()` CAS; `arm.state = Retired` |
| `ContractFinishRollback`   | `SelectGroup::finish_rollback_locked()`                             | `global_mtx_` held         | `group.phase_ = Aborted` (plain write under G)        |

### 1.1 ContractReservation reservation_state

The first scope leaves `reservation_state[i] == None` for every arm (Event and
Timer have no reversible pre-claim reservation). The reservation columns are
therefore no-ops: `ContractReserveReadiness` is unreachable, and
`reservation_close_count[i] == 0` throughout. The production design honors the
contract by simply never transitioning reservation out of `None`.

---

## 2. Central Claim strategy actions (`E13SelectCentralClaim.tla`)

The Central Claim strategy adds the CandidateReady concept and the
lowest-index admission tie-break. Its actions refine the contract actions
above (TLC-checked).

| Central action              | Planned C++ function / method                                       | Refines                  | Notes                                                  |
|-----------------------------|---------------------------------------------------------------------|--------------------------|--------------------------------------------------------|
| `CentralObserveCandidate(i)` | `Scheduler::event_scan_marks_candidate_locked` / `select_timer_pump_entry` | `ContractOfferReadiness` | Sets `CandidateReady`; does not claim                  |
| `CentralClaimWinner(i)`     | `SelectGroup::claim_winner_locked(arm_index)`                       | `ContractLinearizeWinner` | The CAS; lowest-index admission tie-break implemented by walking arms in index order |
| `CentralAdmissionTiebreak`  | inline scan inside `select_impl` admission branch                   | (strategy internal)      | the first `CandidateReady` arm in index order wins     |

The other Central actions map 1:1 to the contract actions in §1.

---

## 3. Event/Timer adapter actions (`E13SelectEventTimer.tla`)

These are the concrete adapter-layer actions. Line numbers reference
`docs/spec/e13_select/E13SelectEventTimer.tla` at base `6ff8cb3`.

| Adapter action (line)            | Planned C++ function / method                                       | Lock domain                | Linearization / commit point                          |
|----------------------------------|---------------------------------------------------------------------|----------------------------|-------------------------------------------------------|
| `BeginRegistration(i)` (746)     | `select_impl` frame setup                                           | no lock                    | `SelectGroup` constructed in the caller frame         |
| `RegisterEventArm(i, e)` (760)   | `Scheduler::select_register_arm_locked` → `SelectPort::link_locked` | `global_mtx_` held         | intrusive link established under G                    |
| `RegisterTimerArm(i)` (788)      | `Scheduler::select_register_arm_locked` → heap push of `SelectTimerRegistration` | `global_mtx_` held         | heap membership established under G                   |
| `AdmissionObserveReady(i)` (832) | inline scan inside `select_impl` (post-registration)                | `global_mtx_` held         | `arm.state = CandidateReady`                          |
| `AdmissionObserveNotReady(i)` (849) | inline scan (no transition)                                      | `global_mtx_` held         | (no state change)                                     |
| `ClaimAdmissionWinner(i)` (865)  | `SelectGroup::claim_winner_locked` (inline branch)                  | `global_mtx_` held         | `winner_` CAS                                         |
| `SetEventBeforeRegistration(e)` (883) | Event `set_` observed at admission                              | `global_mtx_` held         | `set_.load(acquire) == true`                          |
| `StartEventBroadcast(e)` (899)   | `Scheduler::event_set_broadcast` (Select-aware) entry               | `global_mtx_` acquired     | `set_.store(true, release)`                           |
| `ScanEventArm(i)` (921)          | `event_scan_marks_candidate_locked`                                 | `global_mtx_` held         | `arm.state = CandidateReady`                          |
| `ClaimEventWinner(i)` (955)      | `SelectGroup::claim_winner_locked` (Phase 2)                        | `global_mtx_` held         | `winner_` CAS                                         |
| `TimerPumpEntry(i)` (984)        | `select_timer_pump_entry`                                           | `global_mtx_` held         | `state_.load(acquire) == active` gate                 |
| `ClaimTimerWinner(i)` (1005)     | `SelectGroup::claim_winner_locked` (timer branch)                   | `global_mtx_` held         | `winner_` CAS                                         |
| `FinalizeEventWinner(i)` (1018)  | `select_commit_winner_locked` (Event branch)                        | `global_mtx_` held         | `arm.state = Retired (winner)`; `SelectPort::unlink_locked` |
| `ConsumeTimerWinner(i)` (1050)   | `SelectTimerRegistration::try_claim_expiry()`                       | `global_mtx_` held         | `state_` CAS `active → consumed`                      |
| `FinalizeTimerWinner(i)` (1075)  | `select_commit_winner_locked` (Timer branch)                        | `global_mtx_` held         | `arm.state = Retired (winner)`                        |
| `FinalizeEventLoser(i)` (1112)   | `select_finalize_loser_locked` (Event branch)                       | `global_mtx_` held         | `arm.state = Retired (loser)`; `SelectPort::unlink_locked` |
| `CancelTimerLoser(i)` (1144)     | `select_finalize_loser_locked` (Timer branch)                       | `global_mtx_` held         | `arm.state = Retired (loser)`                         |
| `RetireTimerLoser(i)` (1176)     | `SelectTimerRegistration::retire()`                                 | `global_mtx_` held         | `state_` CAS `active → retired`                       |
| `CloseAdapterAuthority(i)` (1207)| (Event) unlink in the finalize rows above; (Timer) the CAS in the rows above | `global_mtx_` held         | Event: not in `select_port_`; Timer: `state_ != active` |
| `RollbackEventArm(i)` (1299)     | `select_rollback_arm_locked` (Event branch)                         | `global_mtx_` held         | `SelectPort::unlink_locked`; `arm.state = Retired`    |
| `RollbackCancelTimer(i)` (1330)  | `select_rollback_arm_locked` (Timer branch)                         | `global_mtx_` held         | `arm.state = Retired`                                 |
| `RollbackRetireTimer(i)` (1361)  | `SelectTimerRegistration::retire()` (rollback path)                 | `global_mtx_` held         | `state_` CAS `active → retired`                       |
| `TimerPumpSkip(i)` (1396)        | `select_timer_pump_entry` early-return                              | `global_mtx_` held         | `state_.load(acquire) != active` — **no dereference of `arm_`** |

---

## 4. The two linearization authorities

Across all three layers there are exactly **two** atomic linearization
authorities in the production design. Every formal commit/claim maps to one of
them:

```text
1. SelectGroup::winner_        std::atomic<uint32_t>
       CAS kNoWinner -> arm_index, acq_rel/acquire
       THE single winner linearization point
       (ContractLinearizeWinner, CentralClaimWinner, ClaimAdmissionWinner,
        ClaimEventWinner, ClaimTimerWinner)

2. SelectTimerRegistration::state_   std::atomic<State>
       CAS active -> consumed  (try_claim_expiry)
       CAS active -> retired   (retire)
       THE single timer callback-authority close point
       (ConsumeTimerWinner, RetireTimerLoser, RollbackRetireTimer,
        CloseAdapterAuthority for Timer arms)
```

Everything else is plain field writes guarded by `global_mtx_`. The brief's
injunction "no two competing winner authorities" is satisfied: there is one
winner CAS. The brief's "no retired/consumed registration dereferences a dead
arm" is satisfied: the timer-state CAS is observed before any `arm_` read.

---

## 5. Publication mapping

```text
ContractPublishInline
    -> Scheduler::select_publish_locked(group) inline branch
       commit point: group.phase_ = Completed (under G)
       runnable_publication_count: 0
       result_publication_count: 1

ContractPublishSuspended
    -> Scheduler::select_publish_locked(group) suspended branch
       commit point: f->make_runnable() returning true (the exactly-once
       guard), followed by route_runnable_locked under G
       runnable_publication_count: 1
       result_publication_count: 1
```

`route_runnable_locked` is the existing canonical runnable-enqueue seam
(`scheduler.cpp:910`). Select calls it exactly once, only on the suspended
branch, only inside `select_publish_locked`. No arm-finalize code reaches it.

---

## 6. Mapping cross-check

For each formal invariant in the closed PR #18 safety suite, the production
counterpart is the source order enforced by the mapped functions above:

| Formal invariant                              | Production enforcement (via the mapped source order)        |
|-----------------------------------------------|-------------------------------------------------------------|
| `C_InvAtMostOneLinearizedWinner`              | `winner_` CAS single point (§4.1)                           |
| `C_InvCommitRequiresWinnerLinearization`      | commit functions require `winner_ != kNoWinner`             |
| `C_InvNoIrreversibleEffectBeforeLinearization`| no `select_commit_winner_locked` runs before `claim_winner_locked` |
| `C_InvWinnerIdentityStableAfterLinearization` | `winner_` is never rewritten after a successful CAS         |
| `C_InvLoserNeverPublishesResult`              | `select_finalize_loser_locked` never writes `group.result_` |
| `C_InvLoserNeverPublishesRunnable`            | `select_finalize_loser_locked` never calls `make_runnable`  |
| `C_InvAllLosersAbortedBeforeCompletion`       | `select_publish_locked` asserts every loser is `Retired`    |
| `C_InvAtMostOneRunnablePublication`           | one call site, guarded by `make_runnable` return value      |
| `C_InvInlineCompletionPublishesNoRunnable`    | inline branch does not call `route_runnable_locked`         |
| `C_InvCompletionRequiresAllAuthorityClosed`   | `select_publish_locked` asserts every arm authority closed  |
| `C_InvRegistrationRollbackOnlyBeforeSuspension` | `select_begin_rollback_locked` requires `caller_state_ == Running` |
| `C_InvRollbackNeverPublishes`                 | rollback path never calls `select_publish_locked`           |

Every formal action has a concrete C++ counterpart; every formal invariant is
enforced by the source order of those counterparts. No cell reads "Scheduler
handles it" — every row names a specific planned function.
