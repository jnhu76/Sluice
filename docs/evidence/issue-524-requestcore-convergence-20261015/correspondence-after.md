# #524 correspondence-after — the converged model

Model head: `28f0fe3397114dbcda491851fd1932b02349df49`
(`formal(request): remove retired ReleaseBind flavor` +
`formal(test): migrate retired binding-release mutant`).

| Artifact | SHA-256 |
|---|---|
| `formal/tla/RequestCore.tla` | `4ad6dbe5cd4af19e12a846f06818622e099a37899474206fdd6984e445725aa1` |
| `formal/tla/RequestCore.cfg` | `82c31d615fdeb73d947342b2a8990072c738069cfa2e1363455a53314f7f8edf` |
| `formal/tla/RequestCoreLive.cfg` | `062c8c3e6b873a5bfd518856952ceacd942dab2f08bf9e6145c34be4f771f09d` |
| `formal/tla/RequestCoreMutDiscardKeepsBind.cfg` | `26e149befe1b644a178663a7e0839e9378f746817ec9f0dd16f674f4a53f5ab7` |
| `scripts/verify_tla.sh` | `2e38fabae8a4224e98b34124675c325f5fd8353f4ce9661c9ced7bae3d90a756` |
| `formal/tla/tla2tools.jar` | `936a262061c914694dfd669a543be24573c45d5aa0ff20a8b96b23d01e050e88` (pinned v1.7.4) |

## Action vocabulary: model → current C++

| TLA+ action | Current C++ owner | Status after #524 |
|---|---|---|
| `Reserve` / `Rollback` | `reserve` / `rollback` | unchanged |
| `Accept` | `accept` | unchanged |
| `CloseAdmission` / `NoteHealth` | `close_admission` / `note_health_failure` | unchanged |
| `ClaimExec` | `claim_execution` | unchanged |
| `PhysicalOutcome` | `offer_terminal` | unchanged |
| `CancelRequest` | `cancel` | unchanged |
| `RetireExec` / `AcquireExec` | `release_execution` / `acquire_execution` | unchanged |
| `AcquireCtl` / `RetireCtl` | `acquire_control` / `release_control` | unchanged |
| `BeginPublish` / `CompletePublish` | `begin_publication` / `complete_publication` | unchanged |
| `Consume` | `consume_public_result` (`request.hpp:take_result`) | unchanged |
| `DiscardPublic` | `discard_public_result` (`request.hpp:discard`/destructor/move-assign-over) | **extended**: carries the `MutDiscardKeepsBind` switch |
| `Reclaim` | `reclaimable_` / `try_reclaim_` / `release_slot_` | unchanged |
| ~~`ReleaseBind`~~ | ~~pre-F2 caller-owned `Completion` release edge~~ | **removed** (no current owner) |
| `StaleTerminal` (mutant only) | no enabled C++ counterpart by construction | unchanged |

Every remaining action maps to a current `RequestCore` member or to the
synchronous reclaim realized inside the releasing calls. The model is no longer
a superset of the C++ protocol on the release sub-graph; the only abstractions
that remain are the two that predate this slice and are disclosed in the B1-1
note (`Reclaim` does not carry the observer-phase pin, which ObserverCore
models; `Accept` has no zero-op fast-path action). Neither is a release edge.

## What the model now states that it did not before

`InvBindImpliesPublished`:

```tla
InvBindImpliesPublished ==
  \A s \in Slots :
    (phase[s] = "accepted" /\ ~bind[s]) => (pub[s] = "done")
```

This is the #524 successor of the flavor's disclosure. Before the removal the
statement was false (the retired edge released an inflight binding); after it,
the statement is an invariant, and it is exactly what makes the dropped guard
in `MutReclaimIgnoresPub` redundant. It is inductive: `bind` is cleared only by
`Consume`/`DiscardPublic` (both require `pub = "done"`) and by `RetireSlot`
(which moves `phase` off `"accepted"`), and no action moves `pub` off `"done"`
while `phase` stays `"accepted"` (reviewer B's independent argument and
empirical check).

## Correspondence classification for the current claims

- Root **VERIFY-02** ("model actions explicitly map to implementation
  transitions"): now satisfied for the full action vocabulary, with no
  superset disclosure needed for a release flavor.
- F6 requirements-matrix **REQ-01** (`EXACT`) and **REQ-05**
  (`MODEL_SUPERSET`): the superset clause was "ReleaseBind models the deleted
  pre-F2 Completion release edge". With that edge gone, the clause is spent,
  which is why the frozen matrix row's finding reference (F6-F03) moves to
  RESOLVED-at-#524 rather than being rewritten in place. The remaining
  abstraction scope (observer pin, zero-op) is unchanged and pre-existing, so
  no exact-correspondence claim is made beyond it.

## Residuals handed to the owner (not fixed by this slice)

- `docs/roadmap/b1-1-request-core-evidence.md` rows `:137` (cfg count),
  `:566` (`MutReleaseResolvable` row) and `:678` (action list incl.
  `ReleaseBind`) are falsified by this convergence. The file is a dated B1-1
  slice record and is **outside this slice's allowed change set**, so it was
  deliberately not edited; its supersession is recorded here, in
  `review.md`, and in the conformance ledger's dated #524 section.
- The pre-existing C++-only paths listed in the B1-1 note (zero-op fast path,
  pre-claim physical outcome, publication target stand-in) are unchanged.
