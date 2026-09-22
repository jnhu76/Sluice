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
- pin classes: **B** public binding, **E** borrow-touching execution
  responsibility (a continuous chain from acceptance), **C** internal control
  bookkeeping, **P** publication in flight;
- publication begins only after the terminal is chosen and E has retired;
  the ready release-store is the publisher's last access to the caller target;
  a public release racing the publication epilogue is safe;
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
| `tests/request_core_test_driver.hpp` | `PublicationTarget` (caller-target contract with the ready release-store and a post-ready publisher-touch detector) and `FakePhysicalDriver` (deterministic physical-event injection plus a settlement ledger) |
| `tests/request_core_protocol_test.cpp` | 29 protocol tests (admission, identity, terminal floor, pins, reclaim) |
| `tests/request_core_publication_test.cpp` | 10 publication tests (ordering, races, epilogue safety, reclaim variants) |
| `tests/request_core_consumer_probe.cpp` | 2 host-neutrality probes (incomplete-type static asserts + a full lifecycle on the public surface) |
| `xmake/tests.lua` | Three additive test targets; each compiles the substrate TU directly and links only `sluice_core`; the protocol/publication targets define `SLUICE_ASYNC_INTERNAL_TESTING`, the probe does not |
| `formal/tla/RequestCore.tla` + 16 cfgs | Safety model, liveness cfg (`PROPERTY L1 L5`), 14 mutant cfgs |
| `scripts/verify_tla.sh` | Stage B1-1 gate: clean safety, clean liveness, 12 invariant kills, 2 temporal kills |

## 3. Protocol facts and pin classes

The slot carries orthogonal lifecycle facts, not one enum: phase
(`free`/`reserved`/`accepted`/`retired`), generation, `binding_live`,
`terminal_chosen` + canonical outcome, `execution_claimed`, `cancel_intent`,
`publication_inflight`, `published`, and the E/C pin counts. Publication
eligibility and reclaimability are derived predicates, never stored states:

- `begin_publication` requires `terminal_chosen ∧ execution_refs = 0 ∧
  binding_live ∧ ¬(inflight ∨ published)` (`request_core.cpp:316`);
- `release_public_binding` requires `terminal_chosen ∧ execution_refs = 0 ∧
  (publication_inflight ∨ published)` (`request_core.cpp:213`);
- `reclaimable_` requires `accepted ∧ published ∧ ¬binding_live ∧
  execution_refs = 0 ∧ control_refs = 0 ∧ ¬publication_inflight`
  (`request_core.cpp:345`).

Acceptance installs the first E pin (the pending-dispatch obligation), so E is
a continuous chain from acceptance to full retirement with no gap in which
borrowed work is unowned. A zero-op descriptor is the one exception recorded
in the contract: it is terminal at acceptance (`success(0)`) with no dispatch
obligation, so E is installed as 0.

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
   target; after it, the publisher only locks the core. `complete_publication`
   resolves by full identity (`resolve_internal_`), which does not require
   `binding_live`, so a fully released binding cannot strand the epilogue.
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
   (`request_core.cpp:236`). Mutual exclusion orders this write before every
   later lock-protected read of the same slot.
2. **Core → publisher handoff.** `begin_publication` copies the canonical
   outcome into the caller's `PublicationPayload` under the same mutex
   (`request_core.cpp:320`). The mutex release/acquire pair between the two
   critical sections makes the copy happen-after the canonical write, on any
   thread pair.
3. **Publisher → caller target.** The publisher writes the target's payload
   (`store`) and then performs the ready release-store (`publish_ready`,
   `ready_.store(release)`), sequenced after the payload write on the
   publisher thread. The release store is the publication point.
4. **Consumer acquisition.** The consumer's `acquire_ready()`
   (`ready_.load(acquire)`) that observes `true` synchronizes-with the
   release store, so its subsequent read of the payload sees exactly the
   stored publication. The payload itself is plain (non-atomic) storage; its
   only synchronization is this release/acquire pair, which is the whole
   reason the pair exists.
5. **Last-publisher-access rule.** After the ready store the publisher
   touches only the core. `PublicationTarget` detects violations of this rule
   (a post-ready `store`/`publish_ready` sets `publisher_touched_after_ready_`),
   and `release_racing_publication_epilogue_is_safe` asserts the detector
   stays clean across the race.

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
C++ has a zero-gap `reclaimable ⇒ reclaimed` postcondition per release call,
and no other thread can interleave an `acquire_control`/`acquire_execution`
on a reclaimable slot — the window the TLA+ model must forbid explicitly
(section 9) is structurally absent in C++.

**Memory-order review summary.** Core state is exclusively mutex-guarded
plain data; the one lock-free pair is the target's `ready_`
release/acquire, whose payload-visibility obligation is stated above; the
`sealed_` misuse detector is deliberately `relaxed` because it carries no
synchronization duty. Cross-thread instrumented concurrency tests and TSan
are **not** claimed here: the C++ tests are deterministic single-thread
interleavings by construction, so this section is a review argument plus the
deterministic ordering tests, with the multi-threaded publication
instrumentation owned by the later public-surface/threading slice.

## 6. Deterministic C++ evidence

Commands (debug configuration, from the repository root):

```sh
xmake build request_core_protocol_test request_core_publication_test \
       request_core_consumer_probe   # one target per invocation
xmake run request_core_protocol_test          # 29/29 pass
xmake run request_core_publication_test       # 10/10 pass
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
- **Pin classes and reclaim:** `execution_refs_block_publication`,
  `execution_responsibility_chain_has_no_gap` (E chain via
  `acquire_execution` handoffs),
  `released_identity_stays_invalid_while_control_pin_lives`,
  `publication_states_are_distinguishable`,
  `terminal_without_publication_does_not_reclaim`,
  `publication_without_binding_release_does_not_reclaim`,
  `binding_release_with_pins_live_does_not_reclaim`,
  `final_blocking_release_drives_each_reclaim_variant` (B-last, C-last and
  P-last all reclaim inside the final release call).
- **Publication ordering (V09):** `happy_path_publication_is_ordered`
  (terminal → E retirement → begin → payload → ready → epilogue → release,
  each step asserted), `terminal_known_while_execution_retirement_delayed`,
  `delayed_control_retirement_after_publication`,
  `duplicate_terminal_before_publication_changes_nothing`,
  `public_release_before_publication_is_refused`,
  `release_racing_publication_epilogue_is_safe`,
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
injected events map to the same actions. The model contains no ThreadPool
worker structure, io_uring SQE/CQE structure, Scheduler, Fiber, Observer or
ProgressSource vocabulary.

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
(cleared on release), and `reclaimLog` (pin facts captured at every reclaim).
The ghosts are monitors only; they have no C++ counterpart and none is
claimed (section 11 states the correspondence per real variable).

## 8. Safety results

`scripts/verify_tla.sh` stage B1-1 (`run_clean rcore-safety`), TLC 1.7.4
(SHA-256-pinned by the script), OpenJDK 25.0.4.1, 20 hardware threads:

- `Model checking completed. No error has been found.`
- 2,244,362 states generated, 399,890 distinct, 0 left on queue.

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
(`~(pub[s] = "done" /\ ~bind[s] /\ exec[s])`). Without this guard, an
environment could forever re-acquire control bookkeeping on settled work,
making `Reclaimable` non-monotone, and `WF_vars(ReclaimAny)` could not force
convergence; L5 would then hold only under an extra environment assumption
about control retirement. The guard encodes the protocol rule that new
bookkeeping on settled work is outside the protocol. Its C++ image is the
zero-gap synchronous reclaim of section 5: because every release call reclaims
inside its own critical section, no interleaving gap exists in which an
`acquire_control` could land on a reclaimable slot, so the C++ API needs no
explicit check. The mutant `MutLazyReclaim` (reclaim gated on an unrelated
submit wake) violates L5 and is killed by the temporal gate.

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
Run metadata: same TLC/jar/host as section 8; 399,890 distinct states (the
reachability graph is the safety run's; liveness checking evaluates the
behavior graph over it).

## 10. Mutation matrix

Every mutant is a `Mut*` constant defaulting to `FALSE`, enabled in exactly
one cfg, and killed by its named invariant or property — never a bare exit
code (`run_violate` greps the exact `Invariant … is violated` line,
`run_temporal_violate` greps `Temporal properties were violated`).

| Mutant | Mechanism | Expected violation | Observed |
|---|---|---|---|
| `MutIgnoreClose` | accept ignores admission close | `InvNoAcceptAfterClose` | killed on `InvNoAcceptAfterClose` |
| `MutRollbackResidue` | rollback enabled from accepted, leaves facts | `InvUnoccupiedClean` | killed on `InvUnoccupiedClean` |
| `MutTerminalOverwrite` | a second candidate overwrites the winner | `InvSingleWinner` | killed on `InvSingleWinner` |
| `MutCancelAsPhysical` | cancel wins even after the execution claim | `InvCancelWinRequiresUnclaimed` | killed on `InvCancelWinRequiresUnclaimed` |
| `MutPublishWhileE` | publication may begin while E > 0 | `InvNoPubWhileExec` | killed on `InvNoPubWhileExec` |
| `MutReleaseResolvable` | binding release forgets to invalidate binding | `InvReleasedNotLive` | killed on `InvReleasedNotLive` |
| `MutGenWrap` | release does not advance the generation | `InvAcceptedRepresented` | killed on `InvAcceptedRepresented`; `InvSingleAcceptance` kills it when checked alone (the wrapped slot re-accepts the same identity — both kills are legitimate, the gate pins the first) |
| `MutReclaimIgnoresPub` | reclaim drops the published requirement | `InvReclaimConditions` | killed on `InvReclaimConditions` (a logged reclaim with `pub # "done"`) |
| `MutReclaimIgnoresCtl` | reclaim drops the C-pin requirement | `InvReclaimConditions` | killed on `InvReclaimConditions` (a logged reclaim with `ctl > 0`) |
| `MutReadyBeforePayload` | publication may begin with no terminal chosen | `InvPublishedBacked` | killed on `InvPublishedBacked` |
| `MutStaleEvent` | a stale-generation outcome hits a reused slot | `InvTerminalMatchesGeneration` | killed on `InvTerminalMatchesGeneration` |
| `MutDoubleDecrement` | retirement decrements the same ref twice | `TypeOK` | killed on `TypeOK` (a ref count leaves its domain) |
| `MutStrandPostAccept` | a recorded intent blocks the physical outcome | temporal | `Temporal properties were violated` (L1: accepted, never published) |
| `MutLazyReclaim` | reclaim requires an unrelated submit wake | temporal | `Temporal properties were violated` (L5: reclaimable, never reclaimed) |

The C++ side is pinned by the deterministic tests of section 6 rather than
compile-time mutants: each mutant's mechanism corresponds to a named failing
assertion (for example `MutCancelAsPhysical` ↔ `pre_execution_cancel_wins_only_without_claim`
plus `cancel_intent_alone_never_terminalizes`; `MutGenWrap` ↔
`stale_event_cannot_touch_reused_generation`; `MutLazyReclaim` ↔
`final_blocking_release_drives_each_reclaim_variant`).

## 11. Source correspondence (evidence map section 13 format)

State authority for every mapped variable is `RequestCore` itself
(root ARCH-02: the core owns request lifecycle state). File abbreviations:
`H` = `include/sluice/async/detail/request_core.hpp`,
`C` = `src/async/detail/request_core.cpp`.

```text
MODEL VARIABLE phase:
C++ AUTHORITY:            RequestCore::Slot::phase (H:140, H:201)
WRITE SITES:              C:103 (reserve), C:132 (accept), C:358/C:373
                          (release_slot_: free / retired)
READ/OBSERVATION SITES:   C:38, C:51, C:62, C:68 (resolvers),
                          C:346 (reclaimable_), C:417-425 (snapshot)
LIFETIME:                 constructed free; vector storage sized once (C:15)
SYNCHRONIZATION:          core mutex on every access

MODEL VARIABLE gen:
WRITE SITES:              C:372-377 (advance or retire), C:446 (test seam)
READ/OBSERVATION SITES:   C:40, C:53 (resolvers), C:146 (identity mint)
SYNCHRONIZATION:          core mutex; advance is inside release_slot_'s
                          single critical section

MODEL VARIABLE bind:
WRITE SITES:              C:134 (accept), C:218 (release_public_binding),
                          C:360 (release_slot_)
READ/OBSERVATION SITES:   C:75, C:84 (resolve_public_), C:213-216, C:316,
                          C:346, C:428
SYNCHRONIZATION:          core mutex

MODEL VARIABLE term (terminal_chosen + outcome):
WRITE SITES:              C:142-143 (zero-op acceptance), C:202-203
                          (zero-effect cancel win), C:236-237 (offer_terminal)
READ/OBSERVATION SITES:   C:195, C:230, C:233, C:248, C:316, C:325
                          (publication payload copy)
SYNCHRONIZATION:          core mutex; immutable after choice

MODEL VARIABLE exec:
WRITE SITES:              C:139/C:144 (acceptance installs 1, zero-op 0),
                          C:264 (acquire_execution), C:277
                          (release_execution), C:367 (release_slot_)
READ/OBSERVATION SITES:   C:213, C:261, C:278, C:316, C:347
SYNCHRONIZATION:          core mutex

MODEL VARIABLE ctl:
WRITE SITES:              C:289, C:302, C:368
READ/OBSERVATION SITES:   C:347
SYNCHRONIZATION:          core mutex

MODEL VARIABLE pub (publication_inflight / published):
WRITE SITES:              C:319 (begin), C:339-340 (complete), C:363-364
READ/OBSERVATION SITES:   C:186 (lookup), C:213-214, C:313, C:316,
                          C:336, C:345-347, C:425-431
SYNCHRONIZATION:          core mutex for the flags; the caller-target half
                          of publication is section 5's release/acquire pair

MODEL VARIABLE claimed / intent:
WRITE SITES:              C:135/C:251 (claim), C:199 (intent set),
                          C:238 (intent cleared on win), C:364-365 (reset)
READ/OBSERVATION SITES:   C:195-202 (cancel arbitration), C:233
                          (zero-effect admissibility), C:248 (late claim)
SYNCHRONIZATION:          core mutex

MODEL VARIABLE closed:
WRITE SITES:              C:162
READ/OBSERVATION SITES:   C:91 (reserve), C:129 (accept)
SYNCHRONIZATION:          core mutex; single acceptance/close linearization

MODEL ACTIONS Reserve/Rollback/Accept/CloseAdmission/ClaimExec/
              PhysicalOutcome/CancelRequest/RetireExec/AcquireCtl/
              RetireCtl/BeginPublish/CompletePublish/ReleaseBind/Reclaim:
C++ AUTHORITY:            the same-named RequestCore member
                          (H:150-172; C:89-343), Reclaim realized as
                          try_reclaim_/release_slot_ (C:350-379) invoked
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
- **Extra-E refinement is C++-only:** `acquire_execution` (E-count handoff
  chains beyond the acceptance-installed pin) and the pre-claim physical
  outcome path (`post_accept_dispatch_failure_publishes_through_terminal`)
  have no model action. The model's driver always claims before an outcome
  and models E as install-at-acceptance/retire. The properties these paths
  must preserve (single winner, E-retirement gating publication, effect
  vocabulary) are pinned by the deterministic tests; the state-machine
  exploration of those refinements is not claimed.
- **Zero-op fast path is C++-only:** the model has no accept-with-terminal
  action; its `BeginPublish` reaches eligibility only through outcome +
  retirement. The zero-op path is deterministic and test-pinned.
- **Health flag is C++-only** (`note_health_failure`): admission health is a
  fact next to the model's `closed` bit; its non-terminal nature is pinned by
  `health_failure_alone_never_terminalizes`.
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
