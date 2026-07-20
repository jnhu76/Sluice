# E13 Select — Layered Formal Core (PR #17)

This directory contains the positive, safety-foundation TLA+ model for the
first E13 Select scope: Event arms plus independent Timer arms. It establishes
three semantic layers instead of making Event/Timer details the permanent
definition of Select.

```text
E13SelectContract.tla
        ^ TLC-checked explicit refinement mapping
E13SelectCentralClaim.tla
        ^ TLC-checked explicit refinement mapping
E13SelectEventTimer.tla
        ^ compatibility root
E13Select.tla
```

Central Claim is the currently selected Candidate A strategy. It is not the
only possible Select strategy. Candidate readiness is a strategy concept, not
a universal primitive or contract state.

## Files

| File | Purpose |
|------|---------|
| `E13SelectContract.tla` | Stable externally observable Select semantics |
| `E13SelectCentralClaim.tla` | Candidate A strategy and explicit Contract refinement |
| `E13SelectEventTimer.tla` | Event/Timer adapters and explicit Central Claim refinement |
| `E13Select.tla` | Thin root preserving existing scene config compatibility |
| `E13SelectContract*.cfg` | Contract safety and reachability |
| `E13SelectCentralClaim*.cfg` | Central safety/refinement and snapshot reachability |
| `E13SelectEventTimer.cfg` | Two-arm adapter safety/refinement |
| `E13Select.cfg` | Four-arm bounded mixed root: same-Event pair + two Timers |
| `E13Select.scene_*.cfg` | Concrete R1-R12 causal reachability probes |
| `tools/formal/verify-e13-select-core.sh` | Reproducible complete PR #17 formal gate |

## Layer 1: stable Select contract

`E13SelectContract` contains no Event, Timer, WaitQueue, timer heap,
CandidateReady, or global-coordination detail. Its abstract lifecycle
distinguishes:

1. offered readiness evidence;
2. optional reversible pre-claim reservation evidence;
3. winner linearization;
4. irreversible winner commit;
5. loser or rollback abort/release;
6. final per-arm authority closure;
7. inline or suspended publication;
8. result consumption and destruction eligibility.

The load-bearing laws checked by `ContractInv` are:

- exactly one winner after successful winner linearization;
- no irreversible effect before winner linearization;
- a reversible reservation closes exactly once as `Committed` or `Released`;
- only the winner may publish;
- all arm authority is closed before completion or aborted teardown;
- successful completion publishes one result;
- inline completion publishes zero runnable transitions;
- suspended completion publishes exactly one runnable transition;
- rollback publishes neither result nor runnable state.

Reversible reservations are permitted before selection only when loser release
fully restores primitive state. A strategy or adapter that cannot provide that
law must use offered evidence and must not claim a reservation.

### Rollback domain

`ContractBeginRollback` is enabled only on the **registration rollback** domain,
i.e. while the operation is still building registration and has not yet
suspended the caller or linearized a winner:

```text
ContractRollbackEnabledDomain ==
    contract_phase = "Building"
    /\ winner = NoArm
    /\ caller_state = "Running"
```

The same domain restriction propagates through both refinements
(`CentralRollbackEnabledDomain` requires `central_phase = "Registering"`;
`EventTimerRollbackEnabledDomain` additionally requires no arm to still be
mid-registration). Rollback is therefore reachable as a genuine
partial-registration failure path (R9) and unreachable after
`ContractSuspendCaller`, after `FinishRegistration`, after winner claim, or
after winner commit.

This deliberately does **not** model:

- cancellation after caller suspension;
- shutdown cancellation;
- user-requested Select cancellation;
- timeout of the whole Select operation.

Those are post-suspension cancellation paths that require a real wake/publication
protocol; PR #17 has no such protocol and must not simulate one by reusing
registration rollback. They remain deferred to PR #18.

The terminal caller state is pinned by two named invariants:
`TerminalCallerStateWellFormed` requires `caller_state = "Running"` in
`Rollback`/`Aborted` and a mode-consistent caller state in `Destroyed`;
`NoBadTerminalWaiting` forbids `caller_state = "Waiting"` in `Aborted`/`Destroyed`.
A dedicated `ContractRegistrationRollbackDisabledAfterSuspension` invariant plus
the `E13SelectContract.rollback_regression.cfg` gate exercise this boundary.

## Layer 2: Central Claim strategy

`E13SelectCentralClaim` adds only Candidate A state:

- `candidate_ready`;
- `claim_candidates`, latched from the complete ready set at linearization;
- `LowestReady` selection;
- inline/suspended claim mode;
- per-arm Winner/Loser classification.

Its contract projection variables are mapped through a top-level:

```tla
ContractRefinement == INSTANCE E13SelectContract WITH ...
RefinesContract == ContractRefinement!ContractSpec
```

`E13SelectCentralClaim.cfg` checks `RefinesContract` as a TLC temporal
`PROPERTY`. Every Central transition therefore projects to a Contract action or
Contract stutter. An alternative future strategy may provide a different
mapping without changing `E13SelectContract`.

## Layer 3: Event/Timer adapters

`E13SelectEventTimer` instantiates Central Claim with the authorized first-scope
adapters:

- `event_state` is indexed by real `Events`, not by arms;
- `arm_event` maps each Event arm to its Event identity;
- post-suspension Event set uses scan, queue-authority release, then group claim;
- every same-Event arm is scanned before the held Event identity is released;
- admission observation cannot be bypassed by a post-arming direct resolver;
- independent Timer arms own `Active/Consumed/Retired` registrations;
- `TimerPumpEntry` checks `Active` before incrementing `timer_node_deref`;
- `TimerPumpSkip` observes a stale terminal registration without dereference;
- Timer winner ordering is claim, consume registration, expire/detach node;
- Timer loser ordering is cancel/detach node, then retire registration;
- Event/Timer finalization closes per-arm waiting/timer accounting;
- typed context clears only after the WaitNode is terminal and detached;
- completion requires every adapter arm to reach `Retired` and every abstract
  authority to close.

The explicit refinement is checked as:

```tla
CentralRefinement == INSTANCE E13SelectCentralClaim WITH ...
RefinesCentralClaim == CentralRefinement!CentralSpec
```

## Preparation-to-model mapping

| Preparation concept | Formal location |
|---------------------|-----------------|
| stable operation lifecycle | Contract `contract_phase` |
| offered/reserved evidence | Contract `readiness_evidence` / `reservation_state` |
| winner linearization | Contract `ContractLinearizeWinner` |
| irreversible winner commit | Contract `ContractCommitWinner` |
| loser/rollback release | Contract release actions |
| final authority closure | Contract `ContractCloseAuthority` |
| CandidateReady | Central `candidate_ready` only |
| lowest-index tie-break | Central `claim_candidates` + `LowestReady` |
| group Winner/Loser classification | Central `arm_class` |
| Event queue identity | Adapter `Events`, `arm_event`, `held_event` |
| two-phase Event broadcast | Adapter `StartEventBroadcast` through `ClaimEventWinner` |
| Timer pre-dereference authority | Adapter `TimerPumpEntry` |
| stale timer skip | Adapter `TimerPumpSkip` / `timer_skip_observed` |
| WaitNode finalization | Adapter `wait_outcome`, `wait_linked`, `context_state` |
| waiting/deadline accounting | per-arm `waiting_account_open`, `timer_account_open` |
| operation/group phases | Contract `contract_phase`, Central `central_phase` |
| required `arm_index` | Adapter identity function `arm_index[i] = i` |
| result publication | Contract count plus concrete `select_result_published` mirror |

## R1-R12 causal witnesses

The scene configs use inverse invariants. A named `NotReach_Rn` violation is
the expected verdict and carries the reachable causal trace.

| Reach | Causal evidence required by the predicate |
|-------|-------------------------------------------|
| R1 | inline claim + Event winner finalization + result publication |
| R2 | inline claim + Timer consume/expire finalization |
| R3 | suspended claim + Event winner + runnable publication |
| R4 | suspended Timer pump dereference + Timer winner |
| R5 | Event winner history + Timer loser cancel/retire history |
| R6 | Timer winner consume history + Event loser history |
| R7 | `Cardinality(claim_candidates) >= 2` and winner is its minimum |
| R8 | one recorded broadcast Event identity shared by at least two claim candidates |
| R9 | registration rollback: strict subset registered (`0 < registration_count < MaxArms`), every registered arm Retired + Released + authority-closed with `finalization_step = "Rollback"`, every unregistered arm Detached + `None`, `winner = NoArm`, caller Running, zero publication |
| R10 | Event winner + a distinct Timer loser with actual `TimerPumpSkip`, `timer_state = "Retired"`, `timer_skip_observed`, and `timer_node_deref = 0` |
| R11 | inline result reaches `Consumed` with runnable count zero |
| R12 | suspended result reaches `Consumed` after caller resume |

R7 can no longer be witnessed by an arbitrary registered loser. R8 can no
longer be witnessed by independently setting two per-arm pseudo-events. R10 can
no longer be witnessed by a rollback terminal state in which the timer pump
never ran. R9 can no longer be witnessed by an all-registered terminal: it
requires a genuine strict-subset partial registration, and it can no longer be
reached after `FinishRegistration` or `SuspendCaller` because rollback is
confined to the pre-suspension registration domain.

## Reproduction

```bash
tools/formal/verify-e13-select-core.sh
```

Environment overrides:

```bash
TLA2TOOLS_JAR=/path/to/tla2tools.jar TLC_WORKERS=1 \
  tools/formal/verify-e13-select-core.sh
```

The runner copies the spec tree into a fresh `mktemp` work directory, runs every
TLC invocation with a private `-metadir` inside that temporary root, and cleans
up only the temporary root. It never deletes or writes generated-looking files
into `docs/spec/e13_select/`. A source-preservation sentinel regression
(`E13SelectUserTTrace.keep`) confirms no source-tree trace/metadir artifact is
created. The runner rejects parse/config failures and deadlocks, requires all
positive/refinement checks to pass, and requires every reachability run to
violate its expected named inverse invariant.

An exhaustive two-arm `-coverage 1` author run also exercised every
non-stutter adapter action. The explicit stutter branch produced no distinct
transition, as expected.

TLC runtime used for the author run:
`TLC2 Version 2.19 of 08 August 2024 (rev: 5a47802)`.

The final single-worker author gate produced (corrective-1 run, OpenJDK 25,
`TLC2 Version 2.19 of 08 August 2024 (rev: 5a47802)`):

| Gate | Generated | Distinct | Queue at stop | Depth | Result |
|------|----------:|---------:|--------------:|------:|--------|
| Contract semantics | 809 | 346 | 0 | 16 | PASS |
| Contract registration-rollback regression | 809 | 346 | 0 | 16 | PASS |
| Central + Contract refinement | 253 | 111 | 0 | 16 | PASS |
| Event/Timer + Central refinement | 24,761 | 8,432 | 0 | 30 | PASS |
| constrained 4-arm mixed root | 71,868 | 17,108 | 0 | 40 | PASS |
| Contract inline reach | 530 | 240 | 43 | 11 | expected witness |
| Contract reservation/suspended reach | 691 | 299 | 23 | 13 | expected witness |
| Central tie-break reach | 55 | 31 | 9 | 7 | expected witness |
| 3-arm mixed reach | 87,437 | 27,074 | 4,130 | 16 | expected R5 witness |
| R1–R12 | 865–453,829 | 380–134,998 | witness stop | 7–20 | all expected named witnesses |

The complete per-scenario metrics are recorded in the PR #17 review request.

## Abstraction boundaries and deferred work

The model abstracts concrete list pointers, timer-heap storage, worker routing,
atomic memory orders, and actual mutex objects while retaining their semantic
authority and ordering. There is one SelectGroup, which is sufficient to model
deduplication of multiple same-Event arms within that group; cross-group Event
broadcast scaling remains outside this bounded PR #17 graph.

PR #17 does not claim complete E13 concrete safety closure. PR #18 remains
responsible for the full safety invariant suite and negative models, including
the stale-pump mutation. Production implementation remains denied.
