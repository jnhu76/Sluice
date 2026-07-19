# E13-SELECT-MULTI-WAIT-PREPARATION-CORRECTIVE-1 -- Review Request

**Task**: `E13-SELECT-MULTI-WAIT-PREPARATION-CORRECTIVE-1`
**Pre-Reaudit Hardening**: `E13-SELECT-MULTI-WAIT-PREPARATION-CORRECTIVE-1-PRE-REAUDIT-HARDENING-1`
**Date**: 2026-07-19
**Status**: INDEPENDENT RE-REVIEW REQUESTED (POST-HARDENING)

---

## 1. Review Scope

This corrective narrows the E13 Select first scope to Event + Timer only,
removes Semaphore and AsyncMutex from active scope, and specifies the
Select-aware internal seams required for Event and Timer resolution.

### In scope

| File | Description |
|------|-------------|
| `docs/e13-select-preparation.md` | Corrected preparation design |
| `docs/e13-select-state-machine.md` | Corrected state machine spec |
| `docs/e13-select-test-plan.md` | Corrected test plan (15 tests) |
| `docs/reviews/E13-SELECT-MULTI-WAIT-PREPARATION-CORRECTIVE-1-REVIEW-REQUEST.md` | This document |

### Out of scope

- `include/**`, `src/**`, `docs/spec/**`, `tests/**`, `xmake.lua`

---

## 2. Baseline

```
REPOSITORY:     jnhu76/Sluice
BASE_BRANCH:    master
BASE_COMMIT:    be70fde
REVIEW_DATE:    2026-07-19
```

---

## 3. Corrective Applied

### 3.1 Scope narrowed

```
FROM: Event + Timer + Semaphore + AsyncMutex (Scope S2)
TO:   Event + Timer (Scope S1)

Semaphore: DEFERRED
AsyncMutex: DEFERRED
Queue: DEFERRED
AsyncCondition: DEFERRED
```

### 3.2 New specifications added

| Section | Content |
|---------|---------|
| S4 | WaitNode::user_ -> SelectArmMetadata discriminator |
| S8 | Select-aware Event resolution seam |
| S9 | Select-aware Timer resolution seam |
| S10 | Non-publishing loser retirement seam |
| S11 | Inline vs suspended winner paths |
| S7 | R1: One continuous global coordination interval |

### 3.3 Hardening applied (pre-reaudit)

| Item | Description | Location |
|------|-------------|----------|
| B | WaitNodeUserKind typed discriminator replaces unsafe static_cast | prep S4, sm S5.1 |
| C | Registration order: metadata+kind before queue | prep S7.1 |
| D | Timer arm private timer_waiters_ queue; group claim before consumption | prep S9, S5.2 |
| E | R1 corrected: no external ready during registration; ObservedAdmissionReady | prep S7.2 |
| F | Event broadcast: two-phase algorithm (scan then group claims) | prep S8.3 |
| G | Fast-path: Arming->WinnerClaimed, not Constructing->WinnerClaimed | prep S6.1, sm S2.4 |
| H | Inline vs suspended: CompleteInline vs PublishSuspendedCaller | prep S11 |
| I | Duplicate arm policy: permitted/forbidden explicit; removed misuse entry | prep S13 |
| J | Formal actions + invariants corrected; authority probe test T16 | prep S16.3-4, tp T16 |

### 3.4 Review findings disposition (updated)

```
P0-1 Semaphore release:            ACCEPTED
P0-2 Mutex handoff:                ACCEPTED
P0-3 Event set unconditional:      CLOSED (two-phase broadcast, S8.3)
P0-4 Timer pump unconditional:     CLOSED (Select-aware timer, S9.3)
P1-1 Select-aware admission:       CLOSED FOR EVENT + TIMER
P1-2 Semaphore release:            DEFERRED
P1-3 Mutex handoff:                DEFERRED
P1-4 CandidateReady external:      CLOSED (S8.4, S9.2, R1 S7.2)
P1-5 cancel publishes:             CLOSED (non-publishing S10, clear_user_context)
P1-6 WaitNode user_ absent:        CLOSED (WaitNodeUserKind S4)
P1-7 no-internal-modification:     CLOSED
P1-8 queued Semaphore:             DEFERRED
P1-9 Mutex unlock:                 DEFERRED
P1-10 partial registration:        CLOSED (R1 + external serialization S7.2)
```

---

## 4. Review Checklist

- [ ] Scope correctly narrowed to Event + Timer
- [ ] WaitNodeUserKind discriminator correctly distinguishes None/Queue/Select
- [ ] Type-safe resolver reads kind before casting pointer
- [ ] Authority probes: Queue-tagged node never enters Select path, Select-tagged never enters Queue
- [ ] Registration order: metadata+kind installed before queue registration
- [ ] Timer arm WaitNode registered in private timer_waiters_, not in any primitive
- [ ] Group claim BEFORE TimerRegistration consumption (ACTIVE->CONSUMED)
- [ ] R1 correctly prevents external set() during registration
- [ ] Two distinct readiness causes: AdmissionObservation vs PostArmingResolution
- [ ] Event broadcast: two-phase algorithm with deduplication
- [ ] No two queue mutexes held simultaneously in Phase 2
- [ ] Fast-path: Arming->WinnerClaimed (not Constructing->WinnerClaimed)
- [ ] Inline completion: make_runnable=0, route_runnable=0
- [ ] Suspended completion: make_runnable=1, route_runnable=1
- [ ] Non-publishing retirement clears user_kind to None
- [ ] Duplicate-arm policy: permitted patterns listed, forbidden patterns listed
- [ ] Formal actions updated: InstallTypedUserContext, EventScanMarksCandidates, etc.
- [ ] New invariants: InvUserKindMatchesDynamicContext, InvNoWrongContextCast, etc.
- [ ] Test plan: 16 deterministic tests (T1-T16); T2 corrected for R1, T2b added, T8/T8b split, T16 authority probe
- [ ] No production/test/spec code was modified