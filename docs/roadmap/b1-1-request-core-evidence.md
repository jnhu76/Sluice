# B1-1 evidence: standalone RequestCore protocol substrate

**Owning issue:** #394 (slice B1-1 only). **Design authorization:** #417
(`B1_DESIGN_READY_WITH_CORRECTIONS` / `AUTHORIZE_B1_1_WITH_CORRECTIONS`).
**Target authority:** the
[v1 Architecture and Contract Reference](../explicit-io-v1-final-decision.md)
(v1-r3) and the [v1 formal evidence map](v1-formal-evidence-map.md); the
[v1 conformance ledger](v1-conformance.md) carries the ledger-facing record.
**Branch:** `feat/394-b1-1-request-core-substrate`.

This note is the substrate's evidence record: the deterministic C++ evidence,
the TLA+ safety/liveness results, the mutation campaign, the C++/TLA+
correspondence in the evidence map's section 13 format, and the C++
happens-before argument that TLA+ deliberately does not provide (evidence map
section 11).

## 0. Corrective-1 (fresh-context adversarial review verdict `B1_1_FAIL`)

The first review pass of this branch found four load-bearing correctness
blockers against the frozen #417 contract. Corrective-1 (this state of the
branch) fixes exactly those four and nothing else:

1. **Publication-target last access.** `PublicationTarget::publish_ready`
   performed its sealed-detector bookkeeping *after* the ready release-store,
   contradicting the "ready store is the publisher's last target access"
   claim and creating a real consumer-destroys-target-then-publisher-writes
   use-after-free window the old detector could not see (the detector flag was
   part of the same object being illegally touched). The store is now the
   literal last statement: all detector bookkeeping is sealed before it, an
   order audit flags a seal that follows the ready store, a destroy audit
   flags any publisher access after consumer destruction, and the misordered
   variant is exercised as a failing control.
2. **Settled-work control acquisition.** `acquire_control` accepted new
   bookkeeping on `published ∧ ¬binding_live ∧ E = 0` work whenever a live C
   pin deferred the reclaim — the exact window the TLA+ `AcquireCtl` guard
   forbids. The guard is now explicit in the C++ API; the synchronous-reclaim
   argument covers only the `C = 0` window and never did cover this one.
3. **Health had no acceptance effect.** `note_health_failure` only set a flag
   that no admission path read, contradicting the frozen acceptance
   transaction (accept must re-check admission/health under the same
   authority). Health now closes the admission gate: `reserve` refuses and
   `accept` re-refuses at the commit point; it still selects no terminal and
   does not widen the public error vocabulary.
4. **E-chain unconstrained.** `release_execution` allowed the last E to
   retire before any terminal existed (a window in which no one can ever form
   an outcome again), and `acquire_execution` granted new borrow-touching
   execution authority after a terminal — including after a zero-effect
   cancel win whose entire meaning is that execution never escaped. The last
   E now retires only once a terminal holds the outcome, and no new E is
   granted after a chosen terminal.

The TLA+ model, the mutation campaign and this note were reworked with the
C++ corrections (sections 3, 7–10); the two falsified claims of the previous
note revision — "the ready release-store is the publisher's last access (as
written)" and "C++ needs no settled-control acquisition guard" — are
retracted and replaced.

**Finalization alignment (health observation).** The review left two
acceptable encodings for the health fact. Corrective-1 shipped the raw-flag
shape but left `admission_open()`'s raw administrative-switch meaning
unpinned by any test, and the consumer probe's health usage read as if a
health failure closed the gate. The finalized encoding folds the fact into
the gate it governs: `note_health_failure` also clears `admission_open_`,
`reserve`/`accept` check that single gate, `admission_open()` reports the
effective gate, and `health_failed()` remains the health fact. The TLA+
`Reserve`/`Accept` guards (`~closed ∧ ~health`) state the same predicate and
are unchanged.

## 1. Scope

Implemented: a standalone, bounded, address-stable, mutex-serialized,
host-neutral `RequestCore` expressing the frozen B1 request lifecycle
protocol:

- reservation is not acceptance; acceptance is one linearization point
  serialized with `close_admission()` under the core mutex;
- identity = context + slot + generation, active only while the slot is in the
  accepted phase at that generation;
- generation exhaustion permanently retires the slot (no wrap);
- the canonical terminal result is core-owned, bounded and expressed in the
  repository's semantic outcome vocabulary (`sluice::detail::IoOutcome`:
  success with confirmed bytes, failure with confirmed prefix and effect
  certainty, or `canceled_before_dispatch`);
- terminal admissibility floor: a cancel intent, a control acknowledgment or a
  health flag alone never terminalizes; a zero-effect cancel wins only before
  any execution claim; the first admissible candidate wins and the terminal is
  immutable thereafter;
- a health failure closes the same admission gate as `close_admission()`:
  it refuses new reservations, re-refuses acceptance at the commit point,
  never terminalizes existing accepted work, and `admission_open()` reports
  the effective gate while `health_failed()` reports the health fact;
- pin classes: **B** public binding, **E** borrow-touching execution
  responsibility (a frozen capability chain from acceptance: acceptance
  installs the first E, the last E retires only once a terminal holds the
  outcome, and no new E is granted after a terminal is chosen), **C**
  internal control bookkeeping (no new C on settled work), **P** publication
  in flight;
- publication begins only after the terminal is chosen and E has retired;
  every publisher-side target bookkeeping is sealed before the ready
  release-store, which is the publisher's last access to the caller target
  (audited); a public release racing the publication epilogue is safe;
- the reclaim predicate is `published ∧ binding released ∧ E = 0 ∧ C = 0 ∧
  P = 0`, and the final pin release itself drives reclaim synchronously
  (core-local L5).

Explicitly not done (non-goals of this slice): no production integration.
`AsyncIoContext`, `ThreadPool`, io_uring, `Scheduler`, `Fiber`, `WaiterToken`,
`RoutingLease` and `ProgressSource` are not wired to the substrate; no
production request path changed; nothing from #395–#401 is absorbed. The
production migration rows in the conformance ledger are unchanged.

Root requirement families exercised at substrate level: REQ (admission,
bounded reservation, identity), BOUND (slot bounds, exhaustion), the LIFE slot
lifecycle obligations owned by the core, the HANDLE identity-validity basis,
VERIFY-03 L1/L5 core-local halves, and VERIFY-04 cases V04 (admission/close
race), V05 (acceptance uniqueness), V09 (publication-before-release), the
V24 request-id facet (generation aliasing), and V26 (settled-work reclaim).

**Non-claims.** TLA+ proves nothing about C++ memory ordering (evidence map
section 11): `pub[s] = "done"` in the model does not establish that a consumer
thread's read happens-after the publisher's write. That argument is section 5
below. Finite TLC establishes only the explored instances (VERIFY-01). A
passing model is not a proof about the production protocol until the
production protocol exists and is connected (B1-2 onward).

## 2. Delivered surface

| Artifact | Role |
|---|---|
| `include/sluice/async/detail/request_core.hpp` | Substrate public surface (detail namespace; no host vocabulary) |
| `src/async/detail/request_core.cpp` | Substrate implementation; the only non-header TU, no dependency beyond `<mutex>`/`<vector>` and the semantic outcome vocabulary |
| `tests/request_core_test_driver.hpp` | `PublicationTarget` (caller-target contract: seal-before-ready ordering, the ready release-store, and post-ready / post-destroy / misordered publisher-access audits) and `FakePhysicalDriver` (deterministic physical-event injection plus a settlement ledger) |
| `tests/request_core_protocol_test.cpp` | 34 protocol tests (admission incl. health, identity, terminal floor, the E-chain capability rules, pins, reclaim) |
| `tests/request_core_publication_test.cpp` | 11 publication tests (ordering, races, epilogue safety, last-access audits, reclaim variants) |
| `tests/request_core_consumer_probe.cpp` | 2 host-neutrality probes (incomplete-type static asserts + a full lifecycle on the public surface) |
| `xmake/tests.lua` | Three additive test targets; each compiles the substrate TU directly and links only `sluice_core`; the protocol/publication targets define `SLUICE_ASYNC_INTERNAL_TESTING`, the probe does not |
| `formal/tla/RequestCore.tla` + 20 cfgs | Safety model, liveness cfg (`PROPERTY L1 L5`), 14 safety-mutant cfgs, 3 temporal-mutant cfgs, 1 subsumption-witness cfg |
| `scripts/verify_tla.sh` | Stage B1-1 gate: clean safety, clean liveness, 17 active mutant kills (14 invariant + 3 temporal), 1 clean subsumption witness |

## 3. Protocol facts and pin classes

The slot carries orthogonal lifecycle facts, not one enum: phase
(`free`/`reserved`/`accepted`/`retired`), generation, `binding_live`,
`terminal_chosen` + canonical outcome, `execution_claimed`, `cancel_intent`,
`publication_inflight`, `published`, and the E/C pin counts. Publication
eligibility and reclaimability are derived predicates, never stored states:

- `begin_publication` requires `terminal_chosen ∧ execution_refs = 0 ∧
  binding_live ∧ ¬(inflight ∨ published)` (`request_core.cpp:323`);
- `release_public_binding` requires `terminal_chosen ∧ execution_refs = 0 ∧
  (publication_inflight ∨ published)` (`request_core.cpp:214`);
- `reclaimable_` requires `accepted ∧ published ∧ ¬binding_live ∧
  execution_refs = 0 ∧ control_refs = 0 ∧ ¬publication_inflight`
  (`request_core.cpp:352`).

Acceptance installs the first E pin (the pending-dispatch obligation). The E
chain is a frozen capability discipline, not an unstructured count:

- `release_execution` refuses the *last* E while no terminal holds the
  outcome (`premature_rejected`, `request_core.cpp:278`), so an accepted,
  unchosen request always keeps `E > 0` and there is never a state in which
  no one can form the outcome;
- `acquire_execution` refuses once a terminal is chosen (`request_core.cpp:262`),
  so a zero-effect cancel win — whose content is that execution never
  escaped — can never be followed by new borrow-touching execution authority,
  and no execution ref can appear after the outcome is fixed;
- the zero-op descriptor is the one exception recorded in the contract: it is
  terminal at acceptance (`success(0)`) with no dispatch obligation, so E is
  installed as 0 and its `acquire_execution` is refused for the same
  terminal-chosen reason.

`acquire_control` refuses on settled work
(`published ∧ ¬binding_live ∧ E = 0`, `request_core.cpp:293`): once the
public terminal is published, the binding is released and E has retired, no
new control responsibility may be created. This is a protocol rule, not an
optimization — see section 5's reclaim closure.

Admission health: a health failure closes `admission_open_`
(`request_core.cpp:172-173`), the same gate `close_admission()` clears
(`request_core.cpp:162`); `admission_open()` reports that effective gate
(`request_core.cpp:165-168`) and `health_failed()` the underlying health
fact (`request_core.cpp:176-179`). `reserve` refuses when the gate is
closed (`request_core.cpp:91`), and `accept` re-checks it at the commit
point under the same lock (`request_core.cpp:129`), so a failure between a
reservation and its acceptance refuses the commit (`admission_closed`); the
reservation rolls back without residue. Health never terminalizes accepted
work and never widens the public error vocabulary.

Generation exhaustion (`UINT64_MAX`) permanently retires the slot on release;
capacity then becomes `exhausted` (permanent) as opposed to `capacity`
(transient, free slots exist later).

## 4. Lock discipline

1. One core mutex serializes every public entry point, including const
   observers (`lookup`, `admission_open`, `observe_slot`, `snapshot`).
2. Every critical section is a bounded state transition: no callback, no
   I/O, no waiting on anything but the mutex itself. `accept` copies the
   caller's descriptor and borrow facts by value under the lock and never
   dereferences the borrow pointer.
3. Slot storage is address-stable (`std::vector<Slot>` sized at construction,
   never resized); `free_slots_` is pre-reserved to capacity, so no critical
   section allocates.
4. `begin_publication` copies the canonical payload into caller storage under
   the lock; the caller-target writes (payload and ready) happen outside the
   lock; `complete_publication` re-acquires the lock for the epilogue. The
   target is never touched by the core while the lock is held.
5. The ready release-store is the publisher's last access to the caller
   target, and this is now true by construction: every publisher-side
   bookkeeping write (the target's seal) happens strictly before the ready
   store, so the store is the literal last statement that touches the target.
   `PublicationTarget` audits the rule from three sides — a post-ready
   publisher access, a seal that follows the ready store (the misordered
   control), and any access after the consumer destroyed the target.
   `complete_publication` resolves by full identity (`resolve_internal_`),
   which does not require `binding_live`, so a fully released binding cannot
   strand the epilogue.
6. A pin count never reaches the mutex-guarded decrement with value zero
   (`underflow_rejected`); a release of a stale identity is refused by the
   generation check before any state is touched.
7. `try_reclaim_` runs synchronously inside the critical section of whatever
   call dropped the last pin; there is no background reclaimer and no
   observable state in which the slot is reclaimable but not yet reclaimed.
8. Generation advance and the full slot reset are one atomic step inside
   `release_slot_`, so a stale event can never observe a reused slot as its
   own identity.
9. The constructor publishes no references and the destructor fail-fasts if
   any slot is still reserved or accepted: settled-before-destruction is a
   checked protocol precondition, not a best-effort cleanup. (The file-local
   fail-fast deliberately avoids `fail_fast.cpp`, which depends on the fiber
   context and would break host neutrality.)
10. Nothing in the substrate blocks or defers accepted work: hidden blocking
    fallback is structurally absent because there is no execution at all —
    only the protocol substrate.

## 5. C++ publication happens-before argument (CPP_MEMORY_MODEL class)

The publication chain, link by link (names per evidence map section 11):

1. **Producer → core canonical storage.** The executor reports the outcome via
   `offer_terminal`, which writes `slot->outcome` under the core mutex
   (`request_core.cpp:238`). Mutual exclusion orders this write before every
   later lock-protected read of the same slot.
2. **Core → publisher handoff.** `begin_publication` copies the canonical
   outcome into the caller's `PublicationPayload` under the same mutex
   (`request_core.cpp:326-332`). The mutex release/acquire pair between the
   two critical sections makes the copy happen-after the canonical write, on
   any thread pair.
3. **Publisher → caller target.** The publisher writes the target's payload
   (`store`), performs its local seal bookkeeping, and *then* performs the
   ready release-store (`publish_ready`, `ready_.store(release)`),
   sequenced after both on the publisher thread. The release store is the
   publication point and — by construction and audit — the publisher's last
   access to the target object.
4. **Consumer acquisition.** The consumer's `acquire_ready()`
   (`ready_.load(acquire)`) that observes `true` synchronizes-with the
   release store, so its subsequent read of the payload sees exactly the
   stored publication. The payload itself is plain (non-atomic) storage; its
   only synchronization is this release/acquire pair, which is the whole
   reason the pair exists.
5. **Last-publisher-access rule.** After the ready store the publisher
   touches only the core. `PublicationTarget` detects violations of this
   rule from three sides (`publisher_touched_after_ready_` for any post-ready
   access, `seal_followed_ready_` for bookkeeping ordered after the store,
   `used_after_destroy_` for any access after consumer destruction), and
   `ready_store_is_the_last_publisher_access_audited` plus
   `release_racing_publication_epilogue_is_safe` assert all three detectors
   stay clean on the ordered path — including across the consumer-destroys-
   target race, where the previous revision's detector was itself part of the
   illegally-touched object and could not have caught the break.

**The critical race: public release racing the publication epilogue.** The
consumer may observe ready, read the payload, call
`release_public_binding`, and destroy the target while the publisher is still
between its ready store and `complete_publication`. Both interleavings are
safe:

- *Release first:* the release guard passes because `publication_inflight` is
  set (P pins the slot); `try_reclaim_` is a no-op while P lives. The
  publisher's epilogue then resolves by full identity — `resolve_internal_`
  requires phase and generation, not `binding_live` — sets `published`, drops
  P, and reclaims inside the same critical section. The publisher never
  touches the destroyed target, so there is no use-after-free.
- *Epilogue first:* `published` is set and the slot stays pinned only by the
  live binding; the consumer's release is then the final pin drop and the
  reclaim happens inside the consumer's own critical section.

After the race, whichever ordering occurred, the slot is reclaimed exactly
once, by the last pin holder, under the mutex.

**Reclaim closure.** Every transition that can make `reclaimable_` first
become true (`complete_publication` setting `published`/clearing P,
`release_public_binding` clearing B, `release_execution`/`release_control`
zeroing E/C) calls `try_reclaim_` inside the same critical section. Therefore
C++ has a zero-gap `reclaimable ⇒ reclaimed` postcondition per release call
for the `C = 0` case: no interleaving window exists in which an
`acquire_control` could land on a reclaimable slot. That argument alone does
not cover the `C > 0` case — a live control pin defers reclaim and re-opens
a window in which settled work is `published ∧ ¬binding_live ∧ E = 0` while
`reclaimable_` is still false — so the API itself forbids acquisition in
that state (`request_core.cpp:293`), matching the model's `AcquireCtl` guard
explicitly (section 9). The zero-gap argument covers the window it can close;
the guard closes the one it cannot.

**Memory-order review summary.** Core state is exclusively mutex-guarded
plain data; the one lock-free pair is the target's `ready_`
release/acquire, whose payload-visibility obligation is stated above; the
publisher-side seal and the misuse detectors are deliberately `relaxed`
because they carry no synchronization duty. Cross-thread instrumented
concurrency tests and TSan are **not** claimed here: the C++ tests are
deterministic single-thread interleavings by construction, so this section is
a review argument plus the deterministic ordering tests, with the
multi-threaded publication instrumentation owned by the later
public-surface/threading slice.

## 6. Deterministic C++ evidence

Commands (debug configuration, from the repository root):

```sh
xmake build request_core_protocol_test      # one target per invocation
xmake build request_core_publication_test
xmake build request_core_consumer_probe
xmake run request_core_protocol_test          # 34/34 pass
xmake run request_core_publication_test       # 11/11 pass
xmake run request_core_consumer_probe         # 2/2 pass
python3 scripts/check_cpp_comment_authority.py .   # clean
```

All three binaries were also built and run under ASan+UBSan
(`xmake f -m asanubsan --toolchain=clang`; the project-wide mode cannot use
gcc here because `libasan` is absent on this host) with all tests passing.
The full suite in this configuration is 37/37 targets.

Load-bearing cases (names as registered by the binaries):

- **Admission/identity:** `accept_close_race_has_one_serialized_winner`
  (close-before-accept refuses with no residue; accept-before-close wins and
  stays represented and cancelable), `reserve_creates_no_public_identity`,
  `accept_activates_identity_exactly_once`, `wrong_context_is_rejected`,
  `wrong_generation_is_rejected`, `rollback_restores_capacity_and_leaves_no_residue`.
- **Generation discipline (V24 request-id facet):**
  `reuse_advances_generation_without_aliasing`,
  `stale_event_cannot_touch_reused_generation`,
  `generation_exhaustion_retires_slot`,
  `slot_reuse_clears_canonical_result`,
  `capacity_exhaustion_is_transient_until_permanent`.
- **Terminal floor (CANCEL):** `first_admissible_terminal_wins`,
  `duplicate_and_stale_events_do_not_double_release`,
  `cancel_intent_alone_never_terminalizes`,
  `pre_execution_cancel_wins_only_without_claim`,
  `health_failure_alone_never_terminalizes`.
- **Health-gated admission (Corrective-1):**
  `health_failure_closes_new_acceptance_from_reserve` (failed health refuses
  new reservations and closes the admission gate — `admission_open()` reads
  false under health alone),
  `health_between_reserve_and_accept_refuses_commit` (accept re-checks
  health at the commit point; the refused reservation rolls back without
  residue).
- **Pin classes and reclaim:** `execution_refs_block_publication`,
  `execution_responsibility_chain_has_no_gap` (E chain via
  `acquire_execution` handoffs),
  `released_identity_stays_invalid_while_control_pin_lives`,
  `settled_work_admits_no_new_control_pin` (Corrective-1: acquisition
  refused while a live C pin defers reclaim on settled work, the existing
  pin still retires into the synchronous reclaim, a reclaimed identity also
  refuses),
  `publication_states_are_distinguishable`,
  `terminal_without_publication_does_not_reclaim`,
  `publication_without_binding_release_does_not_reclaim`,
  `binding_release_with_pins_live_does_not_reclaim`,
  `final_blocking_release_drives_each_reclaim_variant` (B-last, C-last and
  P-last all reclaim inside the final release call).
- **E-chain capability rules (Corrective-1):**
  `last_execution_release_requires_a_terminal` (the last E cannot retire
  before a terminal exists; the chain stays repairable),
  `terminal_choice_freezes_execution_capability` (no new E after a
  zero-effect cancel win, after a chosen physical terminal, or on a zero-op
  terminal).
- **Publication ordering (V09):** `happy_path_publication_is_ordered`
  (terminal → E retirement → begin → payload → ready → epilogue → release,
  each step asserted), `terminal_known_while_execution_retirement_delayed`,
  `delayed_control_retirement_after_publication`,
  `duplicate_terminal_before_publication_changes_nothing`,
  `public_release_before_publication_is_refused`,
  `release_racing_publication_epilogue_is_safe` (with the target destroyed
  mid-epilogue and the post-destroy audit asserted),
  `ready_store_is_the_last_publisher_access_audited` (Corrective-1: ordered
  seal-before-ready is clean, the misordered seal-after-ready control is
  flagged, post-ready and post-destroy accesses are flagged),
  `stale_events_after_slot_reuse_miss_everywhere`.
- **Effect vocabulary (ERR-02 substrate representation):**
  `post_accept_failure_converges_through_terminal` (claimed execution fails:
  canonical failure keeps the confirmed prefix and `unknown` remainder),
  `post_accept_dispatch_failure_publishes_through_terminal` (pre-claim
  initiation failure: same representation, no zero-byte claim),
  `zero_op_accepts_and_settles_without_execution` /
  `zero_op_publication_needs_no_execution`.
- **Host neutrality:** the consumer probe's incomplete-type static asserts
  (`AsyncIoContext`, `AsyncBackend`, `Scheduler`, `Fiber`, `Completion<int>`,
  `RequestArena`, `SynchronousReadySink`, `WaiterToken`, `RoutingLease`) plus
  a full lifecycle and a close-admission usage on the public surface only.

## 7. TLA+ model and bounds

`formal/tla/RequestCore.tla` is the repository's first liveness-carrying
model: `FairSpec == Spec /\ Fair` with weak fairness on the named service
actions, and `RequestCoreLive.cfg` checks `SPECIFICATION FairSpec` with
`PROPERTY L1 L5`. One model action per protocol transition; the test driver's
injected events map to the same actions. Since Corrective-1 the model also
carries `NoteHealth` (a health failure refuses `Reserve` and `Accept`),
`AcquireExec` (the handoff acquisition, guarded on no terminal chosen), the
`RetireExec` last-ref guard (the final E retires only once a terminal holds
the outcome), and the explicit `AcquireCtl` settled-work guard whose C++
image is now the API guard rather than the reclaim race window. The model
contains no ThreadPool worker structure, io_uring SQE/CQE structure,
Scheduler, Fiber, Observer or ProgressSource vocabulary.

Bounds (`RequestCore.cfg` and all mutant cfgs): `Slots = {"s0","s1"}`,
`GenMax = 2`, `MaxExec = 2`, `MaxCtl = 1`; identity domain
`Ids = [slot: Slots, gen: 0..GenMax]` (6 identities); `choices[s] ≤ 4`;
`reclaimLog ≤ 8`; `acceptCount ≤ 2`. Two slots are the minimum that exhibits
slot reuse and cross-slot interleaving; `GenMax = 2` forces reuse-then-retire
within the bound; `MaxCtl = 1` plus the `AcquireCtl` guard exercises the
delayed-control-pin obligation of evidence map section 9 without a
state-space blowup.

Ghost monitoring by full request identity, so slot reuse cannot erase an
obligation inside the model: monotone `stage[id]`
(0 never accepted / 1 accepted / 2 published / 3 binding released /
4 reclaimed), `acceptCount[id]`, `chosenIds`, per-occupancy `choices[s]`
(cleared on release), `reclaimLog` (pin facts captured at every reclaim), and
the Corrective-1 `execRevived[s]` ghost that latches any post-terminal
execution acquisition so `InvNoExecRevival` can pin its absence globally.
The ghosts are monitors only; they have no C++ counterpart and none is
claimed (section 11 states the correspondence per real variable).

## 8. Safety results

`scripts/verify_tla.sh` stage B1-1 (`run_clean rcore-safety`), TLC 1.7.4
(SHA-256-pinned by the script), OpenJDK 25.0.4.1, 20 hardware threads:

- `Model checking completed. No error has been found.`
- 6,165,549 states generated, 996,436 distinct, 0 left on queue.

Checked invariants and what each pins:

| Invariant | Pins |
|---|---|
| `TypeOK` | Value domains of every variable including ghost bounds |
| `InvUnoccupiedClean` | A non-accepted slot carries no pins, terminal, intent or choices |
| `InvSingleAcceptance` | `acceptCount[i] ≤ 1` per full identity |
| `InvAcceptedRepresented` | An accepted identity stays represented until released (and beyond, in a retired slot at its final generation) |
| `InvSingleWinner` | This occupancy's choices have strictly increasing generations — one winner per occupancy |
| `InvTerminalMatchesGeneration` | Choices never exceed the slot generation; a live occupancy's last choice is at the current generation (kills stale-generation events) |
| `InvCancelWinRequiresUnclaimed` | Every zero-effect cancel win happened before any execution claim |
| `InvNoPubWhileExec` | Publication requires E fully retired |
| `InvPublishedBacked` | A published identity had a terminal chosen (no payload-less publication) |
| `InvReleasedNotLive` | A released identity is no longer publicly resolvable |
| `InvReclaimedNotLive` | A reclaimed identity is no longer internally resolvable |
| `InvReclaimConditions` | Every logged reclaim saw E=0, C=0, B released, published |
| `InvNoAcceptAfterClose` | No acceptance commits after `close_admission` |
| `InvNoAcceptAfterHealth` | No acceptance commits after a health failure (Corrective-1) |
| `InvExecHeldUntilTerminal` | An accepted, unchosen request always keeps E > 0 — no window in which the outcome can no longer be formed (Corrective-1) |
| `InvNoExecRevival` | No execution reference is ever acquired after a chosen terminal (Corrective-1) |

## 9. Conditional liveness (L1, L5)

Checked formulas, quoted exactly as written in the module:

```tla
L1 == \A i \in Ids : [](stage[i] >= 1 => <>(stage[i] >= 2))
L5 == \A i \in Ids : [](ReclaimableId(i) => <>(stage[i] = 4))
```

with `ReclaimableId(i) == stage[i] >= 3 /\ phase[i.slot] = "accepted" /\
gen[i.slot] = i.gen /\ Reclaimable(i.slot)` and `Reclaimable(s) ==
phase[s] = "accepted" /\ pub[s] = "done" /\ ~bind[s] /\ exec[s] = 0 /\
ctl[s] = 0`. Both are checked over the full identity domain `Ids`
(quantification over `CONSTANT Ids`, per TLC cfg convention), so slot reuse
cannot satisfy the property by rebinding an identity.

**Fairness conjuncts** (all `WF_vars` on named actions; `FairSpec == Spec /\
Fair`):

- `WF_vars(ClaimExecAny)` — the dispatch handoff service is fair;
- `WF_vars(PhysicalOutcomeAny)` — the physical/executor service eventually
  reports an outcome for claimed work (backend-completes-or-retires premise,
  inside Sluice's future backend contract);
- `WF_vars(RetireExecAny)` — borrow-touch retirement is fair;
- `WF_vars(BeginPublishAny)` / `WF_vars(CompletePublishAny)` — the
  publication service is fair (both halves, so the outside-lock window is a
  modeled pair of steps, not an assumption);
- `WF_vars(ReclaimAny)` — the reclaim service is fair.

**Decomposition compliance.** L1 is proved through the protocol's own
enabling steps — claim, physical outcome, E retirement, publication begin and
completion each carry a fairness conjunct. There is no
`EventualCompletion`-shaped assumption: the closest conjunct,
`WF_vars(PhysicalOutcomeAny)`, is a transition of the modeled machine, not a
premise about an unmodeled kernel. What remains outside Sluice authority is
listed separately below.

**L5 honesty.** `AcquireCtl` is disabled on a fully-released slot
(`~(pub[s] = "done" /\ ~bind[s] /\ exec[s] = 0)`). Without this guard, an
environment could forever re-acquire control bookkeeping on settled work,
making `Reclaimable` non-monotone, and `WF_vars(ReclaimAny)` could not force
convergence; L5 would then hold only under an extra environment assumption
about control retirement. The guard encodes the protocol rule that new
bookkeeping on settled work is outside the protocol.

Its C++ image changed in Corrective-1. The previous revision claimed the
zero-gap synchronous reclaim made an explicit C++ check unnecessary; that
argument holds only while `C = 0` (the state is reclaimable and reclaimed in
one critical section). When a live C pin defers the reclaim, the slot sits
in `published ∧ ¬binding_live ∧ E = 0` with `reclaimable_` still false, and
a new `acquire_control` in that window is exactly the non-monotone
re-acquisition the guard forbids — the first review demonstrated the
sequence (`C=1`, publish, release binding, re-acquire, `C=2 → 3 → …`).
`acquire_control` therefore carries the guard explicitly
(`request_core.cpp:293`), model and C++ now state the same rule, and the
temporal mutant `MutCtlAfterSettled` (guard removed) violates L5 — the
formal reproduction of the review's blocker. `MutLazyReclaim` (reclaim gated
on an unrelated submit wake) still violates L5 as before.

**Environment assumptions outside Sluice authority** (named, not proved):
kernel/filesystem eventual completion and OS scheduling of the runnable
owner and publisher threads. These enter only at integration (B1-2 onward,
#400): in this substrate model the physical world is an action of the
machine, so the model's premise set is exactly the fairness conjuncts above.
The liveness claim is conditional on those premises, per evidence map
section 10.

**Liveness result.** `run_live_clean rcore-live` (`RequestCoreLive.cfg`):
`Model checking completed. No error has been found.` plus `Finished checking
temporal properties` — L1 and L5 hold under `FairSpec` in the explored bound.
Run metadata: same TLC/jar/host as section 8; 6,165,549 states generated /
996,436 distinct / 0 on queue (the reachability graph is the safety run's;
liveness checking evaluates the behavior graph over it, expanding to
11,957,232 behavior-graph nodes at the final check).

## 10. Mutation matrix

Every `Mut*` constant defaults to `FALSE` and is enabled in exactly one cfg.
The campaign splits into two explicit categories, labeled in the gate
script's stage headers:

- **Active negative mutants (17).** Still-expressible bad transitions; each
  must be killed by its named invariant or property — never a bare exit code
  (`run_violate` greps the exact `Invariant … is violated` line,
  `run_temporal_violate` greps `Temporal properties were violated`). 14 are
  invariant-killed, 3 temporal-killed.
- **Subsumption witnesses (1).** Historical fault shapes whose enabling
  state the corrected protocol has made unreachable; the witness cfg must
  complete cleanly, and a violation would mean the stronger invariant had
  been weakened. This campaign has exactly one: `MutReadyBeforePayload`.

| Mutant | Mechanism | Expected violation | Observed |
|---|---|---|---|
| `MutIgnoreClose` | accept ignores admission close | `InvNoAcceptAfterClose` | killed on `InvNoAcceptAfterClose` |
| `MutIgnoreHealth` | accept ignores the health failure | `InvNoAcceptAfterHealth` | killed on `InvNoAcceptAfterHealth` |
| `MutRollbackResidue` | rollback enabled from accepted, leaves facts | `InvUnoccupiedClean` | killed on `InvUnoccupiedClean` |
| `MutTerminalOverwrite` | a second candidate overwrites the winner | `InvSingleWinner` | killed on `InvSingleWinner` |
| `MutCancelAsPhysical` | cancel wins even after the execution claim | `InvCancelWinRequiresUnclaimed` | killed on `InvCancelWinRequiresUnclaimed` |
| `MutPublishWhileE` | publication may begin while E > 0 | `InvNoPubWhileExec` | killed on `InvNoPubWhileExec` |
| `MutReleaseResolvable` | binding release forgets to invalidate binding | `InvReleasedNotLive` | killed on `InvReleasedNotLive` |
| `MutGenWrap` | release does not advance the generation | `InvAcceptedRepresented` | killed on `InvAcceptedRepresented`; `InvSingleAcceptance` kills it when checked alone (the wrapped slot re-accepts the same identity — both kills are legitimate, the gate pins the first) |
| `MutReclaimIgnoresPub` | reclaim drops the published requirement | `InvReclaimConditions` | killed on `InvReclaimConditions` (a logged reclaim with `pub # "done"`) |
| `MutReclaimIgnoresCtl` | reclaim drops the C-pin requirement | `InvReclaimConditions` | killed on `InvReclaimConditions` (a logged reclaim with `ctl > 0`) |
| `MutRetireLastExec` | the last E retires with no terminal chosen | `InvExecHeldUntilTerminal` | killed on `InvExecHeldUntilTerminal` |
| `MutExecAfterTerminal` | a new E is acquired after a chosen terminal | `InvNoExecRevival` | killed on `InvNoExecRevival` (the gate also accepts `InvNoPubWhileExec` — the same mutant state can surface first as publication with a live ref) |
| `MutStaleEvent` | a stale-generation outcome hits a reused slot | `InvTerminalMatchesGeneration` | killed on `InvTerminalMatchesGeneration` |
| `MutDoubleDecrement` | retirement decrements the same ref twice | `TypeOK` | killed on `TypeOK` (a ref count leaves its domain) |
| `MutStrandPostAccept` | a recorded intent blocks the physical outcome | temporal | `Temporal properties were violated` (L1: accepted, never published) |
| `MutLazyReclaim` | reclaim requires an unrelated submit wake | temporal | `Temporal properties were violated` (L5: reclaimable, never reclaimed) |
| `MutCtlAfterSettled` | control may be re-acquired on settled work | temporal | `Temporal properties were violated` (L5: `Reclaimable` non-monotone, `WF_vars(ReclaimAny)` starved through intermittent enabling) |
| `MutReadyBeforePayload` | publication may begin with no terminal chosen | (subsumed — must hold clean) | **Classification: `SUBSUMED_BY_STRONGER_INVARIANT`.** Historical fault shape: publication before the canonical terminal/payload, enabled by `terminal = none ∧ exec = 0`. Corrective: `InvExecHeldUntilTerminal` makes that enabling state unreachable, so the fault can no longer be expressed at `BeginPublish`. Gate expectation: the witness model remains clean. Observed: `Model checking completed. No error has been found.` (previously killed on `InvPublishedBacked`) |

The C++ side is pinned by the deterministic tests of section 6 rather than
compile-time mutants: each mutant's mechanism corresponds to a named failing
assertion (for example `MutCancelAsPhysical` ↔ `pre_execution_cancel_wins_only_without_claim`
plus `cancel_intent_alone_never_terminalizes`; `MutGenWrap` ↔
`stale_event_cannot_touch_reused_generation`; `MutLazyReclaim` ↔
`final_blocking_release_drives_each_reclaim_variant`; `MutRetireLastExec` ↔
`last_execution_release_requires_a_terminal`; `MutExecAfterTerminal` ↔
`terminal_choice_freezes_execution_capability`; `MutCtlAfterSettled` ↔
`settled_work_admits_no_new_control_pin`; `MutIgnoreHealth` ↔
`health_failure_closes_new_acceptance_from_reserve` plus
`health_between_reserve_and_accept_refuses_commit`).

## 11. Source correspondence (evidence map section 13 format)

State authority for every mapped variable is `RequestCore` itself
(root ARCH-02: the core owns request lifecycle state). File abbreviations:
`H` = `include/sluice/async/detail/request_core.hpp`,
`C` = `src/async/detail/request_core.cpp`.

```text
MODEL VARIABLE phase:
C++ AUTHORITY:            RequestCore::Slot::phase (H:140, H:201)
WRITE SITES:              C:103 (reserve), C:132 (accept), C:365/C:380
                          (release_slot_: free / retired)
READ/OBSERVATION SITES:   C:38, C:51, C:62, C:68 (resolvers),
                          C:353 (reclaimable_), C:424-432 (snapshot)
LIFETIME:                 constructed free; vector storage sized once (C:15)
SYNCHRONIZATION:          core mutex on every access

MODEL VARIABLE gen:
WRITE SITES:              C:379-384 (advance or retire), C:453 (test seam)
READ/OBSERVATION SITES:   C:40, C:53 (resolvers), C:146 (identity mint)
SYNCHRONIZATION:          core mutex; advance is inside release_slot_'s
                          single critical section

MODEL VARIABLE bind:
WRITE SITES:              C:134 (accept), C:219 (release_public_binding),
                          C:367 (release_slot_)
READ/OBSERVATION SITES:   C:75, C:84 (resolve_public_), C:214-217, C:323,
                          C:353, C:435
SYNCHRONIZATION:          core mutex

MODEL VARIABLE term (terminal_chosen + outcome):
WRITE SITES:              C:141-144 (zero-op acceptance), C:203-204
                          (zero-effect cancel win), C:237-238 (offer_terminal)
READ/OBSERVATION SITES:   C:196, C:231, C:234, C:249, C:262, C:278, C:323,
                          C:332 (publication payload copy)
SYNCHRONIZATION:          core mutex; immutable after choice

MODEL VARIABLE exec:
WRITE SITES:              C:139/C:144 (acceptance installs 1, zero-op 0),
                          C:265 (acquire_execution, refused after a chosen
                          terminal at C:262), C:281 (release_execution,
                          last-ref guard at C:278), C:374 (release_slot_)
READ/OBSERVATION SITES:   C:214, C:279, C:293, C:323, C:354
SYNCHRONIZATION:          core mutex

MODEL VARIABLE ctl:
WRITE SITES:              C:293/C:296 (settled-work guard, acquire),
                          C:309 (retire), C:375 (release_slot_)
READ/OBSERVATION SITES:   C:354
SYNCHRONIZATION:          core mutex

MODEL VARIABLE pub (publication_inflight / published):
WRITE SITES:              C:326 (begin), C:346-347 (complete), C:370-371
READ/OBSERVATION SITES:   C:187 (lookup), C:214-215, C:320, C:323, C:343,
                          C:352-354, C:432-438
SYNCHRONIZATION:          core mutex for the flags; the caller-target half
                          of publication is section 5's release/acquire pair

MODEL VARIABLE claimed / intent:
WRITE SITES:              C:135/C:252 (claim), C:200 (intent set),
                          C:239 (intent cleared on win), C:371-372 (reset)
READ/OBSERVATION SITES:   C:196-203 (cancel arbitration), C:234
                          (zero-effect admissibility), C:249 (late claim)
SYNCHRONIZATION:          core mutex

MODEL VARIABLE closed:
WRITE SITES:              C:162 (close_admission clears the gate)
READ/OBSERVATION SITES:   C:91 (reserve), C:129 (accept), both via the
                          folded gate below
SYNCHRONIZATION:          core mutex; single acceptance/close linearization

MODEL VARIABLE health:
WRITE SITES:              C:172; the same call also clears the admission
                          gate (C:173)
READ/OBSERVATION SITES:   C:91 (reserve), C:129 (accept), both via the
                          folded gate below; C:178 (health_failed()),
                          C:422 (snapshot)
SYNCHRONIZATION:          core mutex; single acceptance/health linearization

ADMISSION GATE mapping: C++ keeps the two facts but one gate —
note_health_failure also clears admission_open_ (C:172-173), so
admission_open() (C:165-168) reports closed ∨ health and reserve/accept
check only that gate (C:91, C:129). The model's Reserve/Accept guards
(~closed ∧ ~health) are the same predicate.

MODEL ACTIONS Reserve/Rollback/Accept/CloseAdmission/NoteHealth/ClaimExec/
              PhysicalOutcome/CancelRequest/RetireExec/AcquireExec/
              AcquireCtl/RetireCtl/BeginPublish/CompletePublish/ReleaseBind/
              Reclaim:
C++ AUTHORITY:            the same-named RequestCore member
                          (H:150-172; C:89-350), Reclaim realized as
                          try_reclaim_/release_slot_ (C:357-386) invoked
                          synchronously from the releasing calls
STALE EVENT correspondence: resolve rejections by phase/generation/context
                          (C:34-87) map to the *_stale verdicts;
                          the StaleTerminal mutant has no enabled C++
                          counterpart by construction
```

Ghost variables (`stage`, `acceptCount`, `chosenIds`, `choices`,
`reclaimLog`) are model monitors with `NO CURRENT CORRESPONDENCE` in C++ by
design: they exist so the model cannot lose an obligation across slot reuse.
Their C++ image is the observable settled state before reclaim
(`observe_slot`/`snapshot` under `SLUICE_ASYNC_INTERNAL_TESTING`) plus the
protocol-level guarantees (generation advance, single acceptance) that hold
after the observations are gone.

## 12. Known limitations and deferred obligations

- **Context identity allocation** (`ContextIdentity` values, cross-core
  namespaces) is deferred to B1-2a; the substrate takes the identity as a
  constructor value and rejects foreign contexts.
- **Extra-E refinement is modeled since Corrective-1, partially:** the
  handoff acquisition is a model action (`AcquireExec`) under the same
  capability rules as C++ (no new E after a chosen terminal; the last E
  retires only once a terminal holds the outcome, `InvExecHeldUntilTerminal`
  / `InvNoExecRevival` + `MutRetireLastExec` / `MutExecAfterTerminal`). The
  pre-claim physical outcome path (`post_accept_dispatch_failure_publishes_
  through_terminal`) remains C++-only: the model's `PhysicalOutcome` still
  requires `claimed[s]`, and that path's properties (single winner,
  E-retirement gating publication, effect vocabulary) stay pinned by the
  deterministic tests.
- **Zero-op fast path is C++-only:** the model has no accept-with-terminal
  action; its `BeginPublish` reaches eligibility only through outcome +
  retirement. The zero-op path is deterministic and test-pinned.
- **Health is modeled since Corrective-1** (`NoteHealth` + the Reserve/Accept
  guards + `InvNoAcceptAfterHealth` + `MutIgnoreHealth`); the model keeps
  `closed` and `health` as separate facts while C++ folds both into one
  admission gate — `admission_open()` reports the effective gate
  (`closed ∨ health`) and `health_failed()` the health fact (section 11's
  gate mapping). Health never terminalizes accepted work
  (`health_failure_alone_never_terminalizes`).
- **PublicationTarget is a test stand-in** for the public completion surface.
  The production publication target (public `Request`/result surface) is
  B1-2+ scope; the ordering discipline the stand-in pins (payload then ready
  release-store, last publisher access) is the contract the production
  surface must inherit.
- **Cross-thread instrumentation** (TSan, deterministic multi-thread
  publication tests) is deferred to the public-surface/threading slice;
  section 5 is a review argument plus deterministic tests.
- **Bounds:** the model explores 2 slots × generations 0..2 × limited pin
  counts. Finite TLC establishes only the explored instances (VERIFY-01);
  parameterized proofs are not claimed.
- **Production migration is not started:** production request paths,
  `Completion` authority and `RequestHandle` are unchanged in this slice; the
  conformance ledger's production rows and their owners are untouched.
