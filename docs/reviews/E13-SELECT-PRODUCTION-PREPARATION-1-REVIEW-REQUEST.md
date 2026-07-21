# Review Request: E13-SELECT-PRODUCTION-PREPARATION-1

```text
TASK:
E13-SELECT-PRODUCTION-PREPARATION-1

EXPECTED PR:
#19

BASE_BRANCH:
master

BASE_COMMIT:
6ff8cb3dfd411b78259a0fec0801f8b617fa7572

FORMAL CORE:
CLOSED — PR #17

FORMAL SAFETY:
CLOSED — PR #18

PRODUCTION PREPARATION:
THIS TASK — INDEPENDENT REVIEW REQUESTED

PRODUCTION IMPLEMENTATION:
DENIED PENDING PREPARATION REVIEW
```

## What this task is

This task maps the **closed** E13 Select formal semantics (PR #17 contract +
Central Claim + Event/Timer adapter layers; PR #18 layered safety, negative
models, non-vacuity) onto the existing production code (E10 `WaitNode` /
`WaitQueue`, E11 `TimerRegistration`, E12-A `Event`, E12-E `QueuePort`).

It produces a production architecture: type graph, public API, Event adapter,
Timer adapter, locking + publication plan, test matrix, and a formal-to-
production action mapping. **It does not implement any production behavior.**
Implementation is a separate task
(`E13-SELECT-EVENT-TIMER-PRODUCTION-IMPLEMENTATION-1`), denied until this
preparation passes an independent review.

## What this task changes

```text
docs/e13-select-production-architecture.md          NEW
docs/e13-select-public-api.md                       NEW
docs/e13-select-type-and-lifetime.md                NEW
docs/e13-select-event-adapter.md                    NEW
docs/e13-select-timer-adapter.md                    NEW
docs/e13-select-locking-and-publication.md          NEW
docs/e13-select-production-test-plan.md             NEW
docs/e13-select-formal-production-mapping.md        NEW
docs/reviews/E13-SELECT-PRODUCTION-PREPARATION-1-REVIEW-REQUEST.md  NEW (this file)
docs/spec/e13_select/README.md                      STATUS UPDATE ONLY
```

No modification to:

```text
include/**
src/**
tests/**
examples/**
benchmarks/**
CMakeLists.txt
CI workflows
public installed headers
docs/spec/e13_select/*.tla          (the closed formal model is untouched)
docs/formal/e13-select-formal-*.md  (the closed formal design is untouched)
```

## The selected decisions (one-line each)

```text
PUBLIC API:              Candidate C — fixed variadic select(Scheduler&, Cases&&...)
TYPE GRAPH:              SelectGroup caller-frame + embedded `SelectArmSlot` union array
EVENT ADAPTER:           Option E1 — separate sealed Select registry per Event
TIMER ADAPTER:           Option T1 — dedicated SelectTimerRegistration stable block
WAITNODE RELATION:       SEPARATE — no WaitNode reuse, no user_ channel
CENTRAL CLAIM AUTHORITY: C1+C2 hybrid — winner_ CAS under global_mtx_
LOCK ORDER:              G -> one registry -> no group mutex
LIFETIME:                all arm authority closed before caller resume
PUBLICATION:             single Scheduler::select_publish_locked(SelectGroup&)
```

Each decision is justified against rejected alternatives in its focused
document. The brief required this not be a "future-adjustable" verdict; each
decision records its irreversibilities.

## Review focus

The review should answer, for each selected decision: **does the production
design preserve every non-negotiable law in section D of the task brief, and
every formal invariant in the closed PR #18 safety suite?**

### Highest-risk areas (please scrutinize)

1. **WaitNode separation (`docs/e13-select-type-and-lifetime.md` §1).** The
   previous preparation (`docs/e13-select-preparation.md` §4–5) proposed
   reusing `WaitNode::user_` with a kind tag. This task **rejects** that route
   and introduces a separate `SelectArmSlot`. The review should
   confirm the separation does not re-invent the winner state machine, timer
   retirement, intrusive membership, or terminal authority (§1.4).

2. **Single winner authority (`docs/e13-select-locking-and-publication.md`
   §1).** The design uses a hybrid: `SelectGroup::winner_` CAS *under*
   `global_mtx_`. The review should confirm there is no competing winner
   authority (e.g. a per-arm CAS) and that the CAS is the single
   linearization point mapping to `ContractLinearizeWinner`.

3. **Two-phase Event broadcast (`docs/e13-select-event-adapter.md` §4–§5).**
   The Phase 1 scan must snapshot a deduplicated group set *before* Phase 2
   finalization mutates the registry. The review should confirm the snapshot
   discipline (caller-local array of unique `SelectGroup*`) prevents the
   forbidden "finalize mutates the list being scanned" pattern.

4. **Timer stale-entry safety (`docs/e13-select-timer-adapter.md` §6).** The
   I4 closure depends on `SelectTimerRegistration::state_` being observed
   *before* `arm_` is read, and `state_` transitioning out of `active` in the
   same `global_mtx_` CS that finalizes the arm. The review should confirm
   this order is enforced in `select_timer_pump_entry` and the finalize path.

5. **Publication exactly-once (`docs/e13-select-locking-and-publication.md`
   §5).** Exactly one call to `select_publish_locked`; the suspended branch's
   `make_runnable` return value is the runnable publication guard. The review
   should confirm no arm-finalize path reaches `route_runnable_locked`.

6. **Forgeable-authority exclusion (`docs/e13-select-production-test-plan.md`
   §4.5).** The production installed headers declare no Select test-hook type
   and grant no test friend. The PhaseTag seams live only in the
   `sluice_async_internal_testing` variant. The review should confirm the
   planned `detail/select_*.hpp` headers do not accidentally publish a
   forgeable friend type.

### Medium-risk areas

7. **Public API irreversibility (`docs/e13-select-public-api.md` §8).** The
   review should confirm the frozen surface (`SelectResult`, the variadic
   `select`, `kSelectMaxArms`, index semantics) is coherent and that the
   deferred alternatives (span, builder) can indeed be added later without
   breaking the variadic core.

8. **Implementation split gates
   (`docs/e13-select-production-test-plan.md` §7).** Each stage's exit gate is
   a set of test IDs. The review should confirm the gates are ordered
   correctly (no stage depends on behavior enabled by a later stage) and that
   each stage is independently reviewable.

## How to reproduce the formal baseline

```text
cd docs/spec/e13_select
# PR #17 + PR #18 verifier (closed, unchanged by this task):
bash ../../../tools/formal/verify-e13-select-core.sh
```

This task does not modify any `.tla` file. The formal model is the authority;
the production mapping in `docs/e13-select-formal-production-mapping.md` is
checked against it by inspection, not by re-running TLC.

## Self-assessment verdict

```text
E13-SELECT-PRODUCTION-PREPARATION-1:
PASS — AUTHOR SELF-ASSESSMENT

PUBLIC API:              SELECTED
TYPE GRAPH:              SELECTED
EVENT ADAPTER:           SELECTED
TIMER ADAPTER:           SELECTED
WAITNODE RELATION:       SELECTED
CENTRAL CLAIM AUTHORITY: SELECTED
LOCK ORDER:              SELECTED
LIFETIME:                SELECTED
PUBLICATION:             SELECTED
FORMAL-PRODUCTION MAPPING: COMPLETE
TEST MATRIX:             COMPLETE

PRODUCTION IMPLEMENTATION:
DENIED PENDING INDEPENDENT REVIEW
```

An independent review PASS authorizes
`E13-SELECT-EVENT-TIMER-PRODUCTION-IMPLEMENTATION-1`. A review finding that
contradicts a non-negotiable law (section D of the brief) or a closed formal
invariant (PR #18) must be resolved by changing this preparation, never by
relaxing the law.
