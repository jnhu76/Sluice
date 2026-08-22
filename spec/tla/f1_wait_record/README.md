# F1 WaitRecord Registry — generation / lease / delivery (MODEL-007b)

Focused TLA+ safety model of the Phase-F1 identity-bearing Scheduler waiter
registry: the `WaitRecord` pool, its generation-reuse discipline, the
routing lease that pins a record, and the Race B (cancel_waiter vs reap
delivery) / Race C (stale token after record reuse) arbitrations.

Issue #174 (child of umbrella #171; origin: audit #162 MODEL-007(b) and
§7.3's second suggested focused model). C++ is the fact source — the model
conforms to the implementation, never the reverse.

## C++ binding

| Model construct | C++ (baseline c1e93f9) |
|---|---|
| `generation` bump inside `AcquireRegister` | `acquire_wait_record_locked` (`src/async/scheduler_park_wake.cpp:684`) — `++r->generation` inside the registry critical section, before the new occupant is visible; first use also bumps 0→1 |
| `recordState` Registered/Delivered/Free | `WaitRecordState` (`include/sluice/async/scheduler.hpp:1185`); the `cancelled` state is written-then-overwritten inside one registry CS in the cancel body — externally unobservable, modeled registered→free |
| `slotWaiter` | the arena SLOT's `waiter_token_` identity (`request_arena.hpp:649-651`) — decoupled from the record occupant because C++ cancel/extract act on the SlotHandle, not the WaitRecord: a cancel-reopened slot still names its waiter after the record was retired (the schedule that exposes the sequential double-grant) |
| `slotReg`, `deliveryPresent` | arena slot waiter registration (`include/sluice/async/detail/request_arena.hpp:629-690`): `WaiterRegistration` + `waiter_delivery_present_` |
| `ArenaCancelWin` / `ArenaReapExtract` | arena leaf `cancel_waiter` / `reap` extraction — the C2c exactly-once race for the waiter delivery |
| `SinkMarkDelivered` | `ReadyRoutingSink::on_ready` (`src/async/scheduler.cpp:1430`) — scheduler-identity → slot-bound → **generation** → state checks; runs on any poll thread with NO Scheduler lock |
| `DrainConsumePublish` | `drain_routed_completion_waits_locked` (`src/async/scheduler.cpp:1340`) — pop delivered list, retire, freeze `completed`, `make_runnable`, route (one `global_mtx_` scope) |
| `CancelRetirePublish` | `Scheduler::cancel_waiter` retire+publish body (`src/async/scheduler_park_wake.cpp:879`) — L3 `gen ∧ state==registered` check, freeze `canceled`, publish (one `global_mtx_` scope) |
| `SinkStaleGen0` | T6 `f1_stale_record_generation_no_wake` (`tests/scheduler_identity_wake_test.cpp:404`) — forged stale token injected directly at the sink |
| `liveCount` | `wait_record_live_count_` (P1-2 pool accounting, destructor-checked) |

## Boundary

1 record, 2 registration epochs, waiters {W0, W1}, one delivery actor,
one cancel actor. Safety only (no fairness/liveness claims).

### Generation mapping (mirrors the real C++ pool discipline)

| value | meaning |
|---|---|
| `0` | pool-construction generation — **never issued to any occupant** (the first acquire bumps before the occupant becomes visible); exactly the value a forged/stale token carries |
| `1` | W0's registration — the first real acquire bump `0→1` |
| `2` | W1's registration — the reuse bump `1→2` |

The delivered-marking sentinel is `3` (`NoDelivered`), chosen so it cannot
collide with any real generation value. `SinkStaleGen0` compares the forged
`0` against the current occupant's `2`.

Out of scope (explicit non-goals):

- Race A (reap between record-acquire and arena registration) — closed at
  the arena leaf; the fused `AcquireRegister` documents it.
- The non-arena legacy `Completion*`-map fallback (`waiting_size_` /
  `waiting_void_`) — disjoint registry, no identity path.
- Concurrent second slot during epoch 0 (the record pool is the reuse
  constraint; T6 reuses sequentially).
- Scheduler-identity and slot-bound validation dimensions (single
  Scheduler; they are parallel stale-drop branches of the same sink
  validation, collapsed to the generation check).
- Real backend semantics, fiber switching, stealing, timers, select.
- **C++ weak-memory proof**: every modeled transition runs under
  `wait_registry_mtx_` or the arena leaf mutex (SC domains);
  `make_runnable` is a single CAS unit. TLC proves the SC protocol
  abstraction only.

## Laws (positive cfg, all PASS)

| invariant | meaning |
|---|---|
| `InvGenerationIsolation` | a delivered-marking belongs to the CURRENT occupant's generation (Race C) |
| `InvSingleAuthority` | **historical XOR per registration epoch**: at most one authority kind (cancel XOR delivery) is EVER granted at the arena leaf — even after the first grant is consumed (`authorityGrants[w]` history set, `Cardinality ≤ 1`); the earlier simultaneous-only form let the sequential schedule (cancel granted → cancel consumed → delivery granted) pass undetected |
| `InvSingleDelivery` | ≤1 runnable publication per waiter (E7-T2 CAS) |
| `InvSlotLeasePinsRecord` | the in-slot lease pins the record live |
| `InvAuthorityPinsRecord` | an extracted-but-unconsumed authority pins the record Registered |
| `InvLiveRecordAccounting` | `liveCount = (recordState ≠ Free)` (P1-2) |
| `InvOutcomeFrozenOnPublication` | P1-1, **one-directional as-built contract**: publication ⇒ frozen outcome. The freeze precedes `make_runnable` and is unconditional (`set_completion_wait_outcome` is `noexcept`); publication is E7-T2 CAS-gated. The converse (frozen ⇒ published) is NOT a C++ guarantee — a concurrent already-runnable wake could lose the CAS after the freeze — so the earlier iff-form was over-strong and was corrected in review |

## Negative controls (one-rule cfg flips; each names its invariant)

| gate | defect | named CEX | specificity exclusions |
|---|---|---|---|
| NegNoGenCheck | sink drops the generation comparison | `InvGenerationIsolation` | co-victims `InvSlotLeasePinsRecord`, `InvAuthorityPinsRecord` (entailed: as-built Delivered ⇒ slot closed ∧ no cancel authority) |
| NegReuseWhileDelivered | acquire pops a delivered-pinned record | `InvLiveRecordAccounting` | none — all 6 other laws PASS |
| NegCancelKeepsDelivery | cancel keeps `waiter_delivery_present_` set | `InvSingleAuthority` (historical form: the sequential schedule cancel-granted → cancel-consumed → delivery-granted breaks epoch exclusivity exactly like the simultaneous overlap) | co-victim `InvAuthorityPinsRecord` (entailed: the broken arbitration lets a delivery authority outlive the terminal retire) |

### Exactly-once layering (the load-bearing finding)

Double publication is NOT reachable from any single-rule break:

- **L1** arena leaf: cancel XOR extract owns the delivery (NEG-WR3's target).
- **L2** sink state check and **L3** cancel state check each independently
  suppress the second publication in every interleaving after an L1 break.
- **L4** E7-T2 `make_runnable` CAS: with L1–L3 intact, the CAS guard alone
  is never even offered a second attempt. Removing it is `spec/tla/
  e7_publication/`'s modeled defect; this suite does not duplicate that
  mutant, and a compound ≥3-rule chain mutant was rejected as ceremonial.

## Reachability (6 witnesses, each a NoReach* CEX)

`NoReachAuthorityWindow` (extracted winner in flight) · `NoReachCancelWon` /
`NoReachDeliveryWon` (both arena outcomes) · `NoReachReuse` (epoch-1
occupant registered at generation 2) · `NoReachStaleDropped` (forged
gen-0 event inertly dropped during epoch 1) ·
`NoReachSequentialDoubleGrant` (under the NEG-WR3 cfg: a delivery grant
landed after a cancel grant in the SAME epoch — the sequential
double-grant shape that motivates the historical form of
`InvSingleAuthority`; proves the strengthened law non-vacuous against the
exact sequential schedule, not only the simultaneous-overlap prefix).

## Results

TLC 2.19 (tla2tools 1.7.4), exhaustive, 1 worker:

- positive: 101 states generated, 41 distinct, depth 10, no error.
- all 3 negative gates: exact named CEX; all 3 specificity gates PASS;
  all 6 reachability gates CEX as expected.
- `bash scripts/formal/verify-f1-wait-record.sh` → 14/14, PASS.
- review-fix adversarial probes (temp workspace, not committed): under
  NEG-WR3 the simultaneous-overlap shape stays reachable AND the
  pure-sequential shape (cancel granted → cancel consumed → delivery
  granted with the lease already retired) reaches its own named CEX —
  both orders violate the historical `InvSingleAuthority`.

## C++ bridges (no new tests; existing deterministic coverage)

- T5 `f1_cancel_waiter_vs_reap_race` — real-thread Race B: exactly one
  legal outcome per iteration, P1-1 outcome consistency per winner,
  sink-routed ≤ 1, registry drains to 0.
- T6 `f1_stale_record_generation_no_wake` — capacity-1 reuse, forged stale
  token inertly dropped (`stale_dropped == 1`), the real completion still
  routes, registry drains to 0.

Cosmetic drift noted in issue #174 Comment A (still present in the C++ test
comment, not fixed here — comment-only C++ touch, out of this change's
scope): T6's comment calls the forged token "the token A used (record 0,
generation 0)" — the first acquire bumps 0→1, so A's real token generation
was 1; the forged gen-0 token was never issued. The MODEL now uses the real
values (see "Generation mapping" above), so the model↔test mapping is exact.

## Verdict

**AS-BUILT MODELED.** No C++ defect candidate. Allowed claims: the SC
abstraction of the registry protocol satisfies the seven laws above, and
each of the three studied single-rule defects is excluded by the as-built
protocol. Forbidden claims: C++ weak-memory correctness, implementation
freedom from bugs, liveness/stranded-waiter freedom (NEG-WR2's liveness
hole is noted, not claimed), and "TLC green ⇒ C++ correct".
