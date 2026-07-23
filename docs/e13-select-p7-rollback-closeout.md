# E13-P7 — Select Registration-Rollback & Destruction-Closure Closeout

**Stage:** E13-P7 (Select registration-failure rollback + Select destruction-contract
closure).
**Branch:** `e13-select-p7-registration-rollback`.
**Authority:** this document + the closed formal models
(`docs/spec/e13_select/E13SelectContract.tla`, `E13SelectEventTimer.tla`,
`E13SelectCentralClaim.tla`).
**Scope reminder:** P7 is exception rollback, NOT cancellation. It refines the
pre-`FinishRegistration` `Building -> Rollback -> Aborted` lifecycle only.

This document holds the §17 exception-topology audit, the §25 formal-to-production
refinement map, and the §26 formal/prose drift disposition. It is normative for the
P7 implementation; the production code must satisfy it.

---

## §17 Exception-topology audit of `select_admit`

Every operation in the admission core is classified below. Columns:

- **can throw naturally?** — does the operation itself throw (independent of the
  P7 synthetic seam)?
- **before first registration?** — does it run before any arm's registry mutation
  is committed (i.e. before `[0, registered_count)` becomes non-empty)?
- **rollback needed?** — must the catch transaction invoke
  `select_rollback_registration_locked` for an exception escaping here?

| Operation | can throw naturally? | before first registration? | rollback needed? |
| --- | ---: | ---: | ---: |
| caller validation (`ws`, `owner_scheduler`, `current`) | yes — `std::logic_error` | yes | no |
| case Scheduler validation (Event/Timer identity) | yes — `std::invalid_argument` | yes | no |
| arm array materialization (fixed caller-frame `std::array`) | no | yes | no |
| Timer tmp_pool `emplace_back` (per Timer arm, before G) | yes — `std::bad_alloc` | yes | no |
| deadline overflow check (`max_size()` length guard) | yes — `std::length_error` | yes (before reserve) | no |
| deadline heap `reserve` (only allocation under G) | yes — `std::bad_alloc` | yes (before `mark_admitted`) | no |
| `group.mark_admitted()` | no (`noexcept`) | — (after reserve) | — |
| Event link (`select_event_link_locked`) | no (intrusive relink, asserts) | no (a registration commit) | yes, for this arm prefix |
| Timer single-node splice (`select_timer_splice_one_locked`) | no (`std::list::splice` is `noexcept`, allocation-free; heap push within reserved capacity) | no (a registration commit) | yes, for this arm prefix |
| heap push within reserved capacity (`heap_push_entry_locked`) | no (capacity reserved) | no | yes, for this arm prefix |
| `active_deadline_count_` increment | no | no | yes, for this arm prefix |
| `FinishRegistration` phase write (`set_phase(Selecting)`) | no (`noexcept`) | no (post-registration) | n/a (terminal success) |
| readiness snapshot (`monotonic_now()` + scan) | no | no (post-FinishRegistration) | n/a |

**Conclusion (matches §17 expected):**

```text
natural user-visible exceptions occur BEFORE any registration
  (caller validation, case Scheduler validation, tmp_pool bad_alloc,
   length_error overflow, deadline-heap reserve bad_alloc)

the post-reserve registration operations are designed allocation-free:
  - select_event_link_locked   (intrusive doubly-linked relink)
  - select_timer_splice_one_locked
      std::list::splice(single-element) is noexcept and allocation-free
      (C++ [list.ops]; nodes are relinked, not copied)
  - heap_push_entry_locked     (within reserved capacity)
  - active_deadline_count_ ++  (plain integral increment)

P7 rollback is therefore exercised through a synthetic internal-testing
exception seam (§18). The production rollback protocol is nevertheless real and
future-safe: any exception escaping the registration prefix `[0, registered_count)`
is rolled back by the catch transaction, preserving the invariant that no
Scheduler-visible registration authority outlives the Building group.
```

**Reserve-failure discipline (§6.3):** `deadline_heap_.reserve` runs BEFORE
`group.mark_admitted()`. A reserve throw leaves no arm registered, no admission
marker, no splice; the tmp_pool unwinds locally with the frame; the exception
propagates unchanged. No rollback is needed. `mark_admitted()` is NOT moved
before reserve.

**Context7 note:** the C++ guarantees above (`std::list::splice(single)`
`noexcept`/allocation-free; `std::vector::reserve` throwing `std::bad_alloc` on
growth) were confirmed against the existing production code's own documented
contract (`select_timer_splice_one_locked` comment, select.cpp §5). A direct
Context7 fetch of cppreference was blocked at P7 time (Cloudflare interstitial);
the standard guarantees are stable and the production code already depends on
them verbatim.

---

## §25 Formal-to-production refinement map (P7)

Each formal rollback action is mapped to its **as-built P7 production authority**.
Lock domain is `global_mtx_` (G) for every row (single lock domain, §27).

| Formal action | P7 production authority | Precondition | State mutation | Authority-close point | Exception behavior | Test |
| --- | --- | --- | --- | --- | --- | --- |
| `ContractBeginRollback` | `Scheduler::select_begin_rollback_locked(group)` | `phase==Building && winner==kNoWinner && completion_mode==None && result_ has no winner && caller_/caller_owner_ set` (reject Selecting/Armed/Completed/Consumed/Aborted/Rollback) | `group.phase = Rollback` | none (no arm authority closed by BeginRollback) | `noexcept`; fail-fast (debug assert + `select_invariant_fail_fast`) on invalid domain — never throws | P7-N1..N4, P7-T* |
| `ContractRollbackRelease(i)` + `ContractCloseAuthority(i)` (Event) | `Scheduler::select_rollback_arm_locked(group, arm)` Event branch | `arm.kind==Event && arm.state==Registered && arm.group==&group && arm.event.event_!=null && arm.home_==&arm.event.event_->select_port_` | `arm.state = Retired` (classification FIRST), then `select_event_unlink_locked(event, arm)` (canonical repair; clears home_/next_/prev_) | the `select_event_unlink_locked` call (single canonical Event unlink path) | `noexcept` | P7-T2, P7-T4, P7-N5 |
| `ContractRollbackRelease(i)` Timer — `RollbackCancelTimer(i)` | `Scheduler::select_rollback_arm_locked(group, arm)` Timer branch, step 1 | `arm.kind==Timer && arm.state==Registered && arm.group==&group && arm.timer.stable_reg_!=null && reg.scheduler()==this && pool_owns_select_block_locked && reg.arm()==&arm && reg.is_active()` | `arm.state = Retired` (classification FIRST — matches `RollbackCancelTimer`: `adapter_phase Registered -> TimerCancelled`, `timer_state` STILL Active) | none yet (registration still ACTIVE) | `noexcept` | P7-T3, P7-T4 |
| `ContractCloseAuthority(i)` Timer — `RollbackRetireTimer(i)` | `Scheduler::select_rollback_arm_locked(group, arm)` Timer branch, step 2: `select_timer_retire_locked(reg)` | as above; arm now Retired | `reg.retire()` CAS `active -> retired` exactly once; `--active_deadline_count_` exactly once; `recompute_earliest_deadline_locked()`. Returns `true`; false => `select_invariant_fail_fast()` (P7-N7) | the `select_timer_retire_locked` CAS (single canonical Timer retirement authority — no direct `active_deadline_count_` decrement, no direct Timer state store) | `noexcept` | P7-T3, P7-T4, P7-T8, P7-N6, P7-N7 |
| `ContractFinishRollback` | `Scheduler::select_finish_rollback_locked(group, arms, arm_count, registered_count)` | rollback prefix processed; every registered Event arm Retired+unlinked; every registered Timer arm Retired+registration non-active; every never-registered suffix arm Detached; `winner==kNoWinner && completion_mode==None && result_ has no winner` | `group.phase = Aborted` (only after proving the above) | validated — no open authority remains | `noexcept` | P7-T*, P7-N8 |
| `ContractDestroyOperation` after Aborted | `~SelectGroup()` / frame unwind | `admitted_ && phase==Aborted` | frame destruction | n/a | accepted by destructor (`Consumed || Aborted`) | P7-T6, destruction closure |
| `RollbackEventArm(i)` | `select_rollback_arm_locked` Event branch | (see Event row) | (see Event row) | unlink | `noexcept` | P7-T2 |
| `RollbackCancelTimer(i)` | `select_rollback_arm_locked` Timer branch step 1 | (see Timer row) | `arm.state = Retired` | — | `noexcept` | P7-T3 |
| `RollbackRetireTimer(i)` | `select_rollback_arm_locked` Timer branch step 2 (`select_timer_retire_locked`) | (see Timer row) | CAS active->retired + accounting | the CAS | `noexcept` | P7-T3 |
| `TimerPumpSkip(i)` after rollback | `select_timer_pump_entry_locked` early-return (UNCHANGED from P3) | popped entry `state != active` (Retired after rollback) | skip; do NOT read `arm_`; caller physically erases the popped block | n/a | `noexcept`; arm-load delta == 0 | P7-T8 |

**Orchestrator** (`select_rollback_registration_locked`, called from the catch
under G): `select_begin_rollback_locked` -> for `i` in `registered_count-1 .. 0`:
`select_rollback_arm_locked` (reverse registration order, §10) -> for `i` in
`[registered_count, arm_count)`: normalize `Prepared -> Detached` (never-registered
suffix, §11) -> `select_finish_rollback_locked`.

**Reverse-order rationale (§10):** registration is index-order acquisition;
rollback is reverse-order release. Formal safety does not depend on arm-release
order, but production uses one deterministic order and tests verify it through
TEST-only observations.

---

## §26 Formal/prose drift disposition

**Normative authority:** the closed formal adapter
`E13SelectEventTimer.tla` (`RollbackCancelTimer` / `RollbackRetireTimer`,
base of P7) defines the canonical Timer rollback order:

```text
RollbackCancelTimer(i):  adapter_phase[i] "Registered" -> "TimerCancelled"
                          timer_state[i] stays "Active"
                          (arm classification/cancel FIRST)

RollbackRetireTimer(i):  adapter_phase[i] "TimerCancelled" -> "Finalized"
                          timer_state[i] "Active" -> "Retired"
                          (Timer callback-authority retirement SECOND)
```

This matches the existing Timer-**loser** ordering discipline
(`select_finalize_timer_loser_locked`: `arm.state = Retired` THEN
`select_timer_retire_locked`) and the SN-9 loser-ordering seam. The normative
order is therefore:

```text
arm terminal classification (Retired) FIRST
Timer ACTIVE -> RETIRED retirement SECOND
```

### Drift 1 — `docs/e13-select-type-and-lifetime.md` §5.2 (CORRECTED)

The preparation prose described the Timer rollback loop in the **opposite**
order:

```text
a.stable_reg.state_.store(retired)  // retire the stable block   <- WRONG ORDER
a.state = retired                                                  <- WRONG ORDER
```

and the Event branch as unlink-then-state. **Disposition: corrected in this
P7-A commit.** The §5.2 rollback sequence now reads classify-then-close for
both kinds, matching the formal adapter. The canonical formal transition is
unchanged (it was already correct); only the stale prose was wrong.

### Drift 2 — `docs/e13-select-formal-production-mapping.md` §1 row 35

`ContractRollbackRelease(i)` was described loosely as
"Event: unlink; Timer: retire CAS; arm.state = Retired" (order unspecified /
Timer lumped). **Disposition: the §1 row is left as the coarse contract-layer
summary, but §3 (adapter rows `RollbackCancelTimer`/`RollbackRetireTimer`) is
the authoritative split and already states the correct order.** The §25 map
above is the P7-corrected production refinement. No formal-model change.

### Drift 3 — planned vs as-built function names

The mapping doc used the **planned** P7 names (`select_register_arm_locked`,
`SelectGroup::finish_rollback_locked`, etc.). **Disposition:** P7 implements
the as-built production authorities defined in §25
(`select_begin_rollback_locked`, `select_rollback_arm_locked`,
`select_finish_rollback_locked`, `select_rollback_registration_locked`) as
private `Scheduler` members. The mapping is updated in §25 above; the historical
planned-name rows are left in place as closed history (the doc header already
states the names are planned, not as-built).

---

## §13 / §22 Destruction-contract closure

**Current accepted group rule (unchanged):**

```text
if group was admitted (mark_admitted):
    destruction requires phase == Consumed or Aborted
```

P7 must prove:

```text
successful operation:       ends Consumed
registration failure:        ends Aborted
pre-admission validation/
allocation failure:          group not admitted (no destructor contract)
```

**Destruction-safety audit (§22):** the destructor must NOT dereference
`TimerArmPayload::stable_reg_` after completion/rollback. A retired Timer stable
block may already have been physically reclaimed by the pump (lazy reclamation,
§12). The existing `~SelectGroup()` only inspects caller-frame-only state
(`phase_`, `admitted_`) — it does not touch arms or Timer registrations — so it is
already safe for the Aborted path. P7 adds NO new destructor check that
dereferences a Timer registration. The smallest genuinely-missing safe assertion
is already present (phase ∈ {Consumed, Aborted}); no further destructor work is
required.

The Aborted group's arms unwind with the caller frame. Event arms are already
fully unlinked (home_/next_/prev_ == nullptr) by rollback; Timer arms are Retired
with their stable blocks Scheduler-owned-but-RETIRED (the pump will lazily
reclaim them; the caller-frame `arm.timer.stable_reg_` becomes a dangling pointer
that is never dereferenced again — the pump's state-before-arm rule, §12). This is
the same lazy-reclamation boundary already proven by P3/P6.
