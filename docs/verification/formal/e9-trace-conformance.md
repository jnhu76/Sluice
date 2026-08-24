# E9 Trace-Conformance Pilot (#196, V2 of umbrella #163)

> Owner doc for the `e9-trace-conformance` suite
> (`scripts/formal/verify-e9-trace-conformance.sh`; manifest entry beside
> `e9-park-wake`). Established 2026-08-24 at master `d98d70d`.
>
> **Claim (exact wording, never stronger): `TRACE-CONFORMANT (TESTED
> EXECUTIONS)`** — the observed semantic traces of the named deterministic
> C++ corpus are behaviors of the matching as-built TLA+ model at the same
> revision. This is NOT implementation verification, NOT a proof over unseen
> executions, NOT a C++ memory-model proof, NOT scheduler-fairness
> evidence, and NOT platform validation.

## The containment being claimed

```text
ObservedBehaviors(deterministic C++ corpus at revision R)
    ⊆
Behaviors(E9ParkWake.tla at revision R)
```

established mechanically per trace: the validator
(`scripts/formal/e9_trace_validate.py`) compiles the trace's semantic
events to a sequence of named model actions and generates a TLC replay
wrapper (`EXTENDS` the **pristine** `E9ParkWake.tla`; generated in an
isolated temp workspace, never committed). TLC then answers the
existential question — does SOME behavior of the model fire exactly that
action sequence (interleaved freely with the non-wake-advancing actions)?
Invariant `TraceIncomplete` violated ⇒ ACCEPT (TLC's counterexample IS the
witness behavior); holds ⇒ REJECT. The repository model is the authority;
the validator is not a second protocol implementation.

## Revision binding

Every trace artifact carries `cpp_revision` and `model_revision` (both
required, 40-hex, and EQUAL — the validator fail-closes otherwise). The
checked-in canonical corpus (`spec/tla/e9_park_wake/traces/`) is bound to
`82acdd3` (the trace-capture revision; the C++ test's per-case shape
assertions are the freshness link between that corpus and the live
binary). The gate's `--fresh` mode re-runs the C++ corpus test bound to
the CURRENT `git rev-parse HEAD` and validates every emitted trace — the
strongest same-revision evidence; run it after any change to the C++
park/wake paths or to the model, and (when the emitted shapes move) update
the canonical corpus to the new HEAD.

## Real trace corpus

Captured by `tests/e9_trace_conformance_test.cpp` from REAL deterministic
executions (phase seams + the `wake_mtx_`-hold barrier + bounded
watchdogs; no sleep-ordering). The recorder is the existing
`SLUICE_ASYNC_INTERNAL_TESTING` controller (`tests/async_test_control*.h*
+ .cpp`) with a fixed-size event ring; every call site is macro-guarded,
so production builds compile none of it (seam-production-exclusion gate).

| fixture | config | semantic shape | verdict |
|---|---|---|---|
| `t1_unarmed_park_external_wake` | split (ThreadPool), Live | unarmed park, external wake-handle notify strictly inside the wait (`[ParkCommitted, ParkEntered, WakePublished(external), ParkReturned{epoch}]`) | ACCEPT |
| `t2_wake_races_park_commit` | split, Live | notify strictly inside the commit→wait window (post-baseline seam): immediate predicate-true return (`[ParkCommitted, WakePublished(external), ParkEntered, ParkReturned{epoch,immediate}]`) | ACCEPT |
| `t3_armed_park_observation_return` | reference (FakeAsync), Live | ENTRY-ARMED MW-S3 park (ready-flag wait, E5-A2) returns on the 2 ms observation timeout with no wake (`[ParkCommitted{armed}, ParkEntered, ParkReturned{timeout}]`) — the #185 `observationArmed` escape | ACCEPT |
| `t4a_…_retire_after` / `t4b_…_retire_before` | reference, Drain | unarmed MW-S2 non-participant park + the participant's no-progress terminate publication; the parked return and the retiring participant's epilogue wake race — BOTH orders are legal and both validate | ACCEPT |
| `t5_prebaseline_publication_refuses_park` | reference, Live | runnable publication strictly pre-baseline (seam B) → the G1 recheck REFUSES and signals (`[WakePublished(runnable_route), ParkRefused, WakePublished(refuse)]`) | ACCEPT |

The C++ test asserts each trace's deterministic shape in-test (the
freshness link for the fixtures); `t4a` is the alternative legal order the
same test accepts (which of the two a run emits is timing-dependent; both
are validated).

## Negative controls (mandatory, both REJECTED BY MODEL SEMANTICS)

| fixture | forbidden shape | why the model rejects it |
|---|---|---|
| `neg_a_causeless_return` | mutant of t1 (SplitWait=TRUE): the parked worker returns with NO wake and NO cause | `LeavePark` has no enabled disjunct: the armed escape is dead under SplitWait=TRUE and no scheduler-domain wake-due exists (no producer action may fire silently) |
| `neg_b_unconditional_escape_claim` | pre-#185-style old-model mutant (forbidden semantic mutant trace — **#185 was MODEL drift, never a C++ defect**): an UN-ARMED reference park claims an unconditional bounded-escape return | the faithful escape requires `observationArmed[w]` (#185); an unarmed park with no wake-due has no leave |

Plus malformed-input fail-closed fixtures (unknown event kind, revision
mismatch, missing revision) and the validator `--self-test` (eight
fail-closed legs + one ACCEPT and one REJECT TLC leg — the verdict itself
is non-vacuous).

## Event → model action mapping (authority table)

| Semantic event | C++ origin | Model action |
|---|---|---|
| `ParkCommitted{w,epoch,armed}` | baseline record, `scheduler_park_wake.cpp` park commit | `FinalParkRecheckAndCommit(W)` |
| `ParkEntered{w}` | cv-wait boundary (under `wake_mtx_`) | `EnterPhysicalPark(W)` |
| `ParkReturned{w,immediate=false}` | blocking wait return (cause bits snapshot) | `LeavePark(W)` |
| `ParkReturned{w,immediate=true}` | predicate true at wait entry | fused into the preceding `EnterPhysicalPark` (its predicate-true branch — one model action) |
| `ParkRefused{w}` (+ adjacent `WakePublished{refuse}`) | the G1 refuse branch and its signal | `AbandonParkCandidate(W)` (one fused action; the refusal signal is the action's `BridgeEffect`) |
| `WakePublished{external}` | `notify_external_wake` → `signal_wake_locked` | `ExternalReadyPublish` |
| `WakePublished{runnable_route}` | `spawn`/`spawn_on`/`route_runnable_locked` | `PublishRunnable` |
| `WakePublished{terminate}` | `global_terminate_` publication signals | `ShutdownSignal` |
| `WakePublished{retire,w}` | the worker-loop retire epilogue | `RetireWorkerQuiescent(W)` |
| `WakePublished{idle_dance}` (+ following park pair) | the not-last dance signal (fires pre-park in C++) | fused into that worker's `EnterPhysicalPark` (the model's R4 signal fires at ENTER) |
| `WakePublished{none}` | — | REJECT (unattributed publication) |

Documented mapping notes:
- **Terminal collapse (t4)**: the model's E9-CORRECTIVE terminal semantics
  freezes parked workers at `runState # "Active"` (both `LeavePark` and
  `RetireWorkerQuiescent` require `runState = "Active"`, by design — the
  invocation ended). The C++ terminate+epilogue wake PAIR therefore maps
  to ONE `ShutdownSignal` advance (#189/#191 fused-exit authority), the
  post-terminal epilogue wake is the fused-away second half, and a
  post-terminal `ParkReturned` whose causes include `terminate` is the
  physical teardown the model collapses. NEVER dropped: `causes=[]` or
  timeout-only returns (those reject even post-terminal).
- **Pre-history (compiled, #202 review)**: C++ pre-run fiber admission /
  pre-run backend submits publish no wake; the model reaches those states
  only via epoch-advancing producers. Each declared pre-history is
  therefore COMPILED into those real model actions as the FIRST required
  steps of the replay wrapper, fired from the true `Init` under their
  actual guards — the pre-history state's reachability is mechanically
  established by the same TLC search (no hand-written state formula, no
  Init disjunct):

  ```text
  external_wait_registered == Init -> PublishRunnable -> RunFiber -> SuspendFiber
  backend_outstanding      == Init -> PublishRunnable -> RunFiber
                                  -> SubmitBackend -> FinishFiber
  ```

  The pre-history is a PINNED prefix: no silent step may fire until it is
  consumed, matching the contiguous C++ pre-run. Without the pin, a silent
  `SubmitBackend` could interleave between `RunFiber` and `SuspendFiber`
  and drift the pre-history state (turning the MWS3 external wait into
  MWS2, which changes the park domain and could fabricate an acceptance) —
  the validator self-test REJECT leg guards exactly this.
- **Silent actions**: only non-wake-advancing actions may fire between
  trace steps (fiber lifecycle, backend submit/ready, the candidate
  decision, backend-bound commits, backend-branch park enter/leave).
  Every C++ wake epoch advance passes `signal_wake_locked` and is
  recorded, so no wake-advancing action may fire silently.
- **Publisher abstraction (t5)**: the mid-run external-thread
  `spawn_on` maps to `PublishRunnable` (the model's only
  runnable-publication action; its `SomeActiveWorker` guard is satisfied
  by the live worker that will execute the ticket). Publisher identity is
  abstracted; the wake/park/refusal sequence is what is validated.

## Assumptions / bounds / discovered model-scope boundaries

1. **E5-A2 ready-flag observation return under SplitWait=TRUE**: the C++
   `:1188` park is 2 ms-capped while ready-flag waits are registered (any
   backend); the model's armed escape is scoped `~SplitWait`. A
   split-config ready-flag observation-timeout return has NO model action
   — a real C++ behavior outside the current model (the model
   under-covers). The corpus excludes it; recommended follow-up: a
   focused model decision (extend `observationArmed` semantics or record
   the divergence). NOT repaired here (the #185-repaired model is the
   authority; a semantic change needs its own gates/witnesses).
2. **C++ park classes beyond model admission**: mw_s1-delegation parks,
   quiescent parks, and Drain MW-S3 dance parks are real C++ parks whose
   model counterpart is the narrower `ParkAdmitted` rule (the model's
   Active-phase abstraction / R1 refuse). The corpus covers the
   model-admitted classes; the validator fail-closes on the others.
3. **Backend-domain parks** (`ctx_.wait_one`) are outside the pilot's
   observable vocabulary (E9's backend park is the bridge/progress
   world — `BackendReadyPublish`/bridge liveness is carried by the
   e9-park-wake invariants, not by this trace vocabulary).
4. The model's 1-bit `wakeEpoch` toggle vs production's monotonic
   counter: epoch parity is not trace-observable; the model's leave
   authority is persistent state, so R4/fused advances that the C++ trace
   does not observe as separate signals are invisible to the mapping
   (projection semantics).

## Reproduce

```sh
bash scripts/formal/verify-e9-trace-conformance.sh          # fixtures + self-test
bash scripts/formal/verify-e9-trace-conformance.sh --fresh  # + live C++ corpus at HEAD
python3 scripts/formal/e9_trace_validate.py --trace <file.json> --expect accept
SLUICE_TEST_FILTER=e9_trace_t1_unarmed_park_external_wake xmake run e9_trace_conformance_test
```
