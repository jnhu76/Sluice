# TV-1 — Historical Drift Sensitivity via Semantic Trace Validation

> Owner doc for TV-1 (issue #305, authority chain #298 → #196/#163).
> Established 2026-09-07 at master `9c79bf1c` (PR #304 merge), branch
> research/tv1-trace-drift. Research-only: no production semantics, no
> static-FDG routing, no enforcement wiring.
>
> Corrective round (adversarial review of PR #306, same day): C1 evidence
> provenance restated at the true source revision (below); C2 prehistory
> sensitivity diagnostic added (non-scoring); C3 Phase-C comparison
> restored to the frozen miss/safety-failure facts; C4 the
> `tv1_wake_scan_routed` capture moved under the internal-testing guard
> (a production TU now contains no `tv1` identifier — preprocessor-level
> verified). No experimental verdict changed.
>
> **Verdict: TRACE_CHANNEL_PARTIAL.** The existing E9 channel — real
> deterministic C++ → semantic events → trace artifact → TLC replay against
> the pristine same-revision model — decisively discriminates the repaired
> vs. broken worlds of the C-001 lost-wake specimen at BOTH the trace level
> (repaired trace ACCEPT, broken trace REJECT — the model refuses the
> broken world's continuation outright) and the execution level (bounded
> divergence). It has equally explicit, named blind spots: the C-012
> cancel-path poison wake is invisible at trace level (backend-domain wait)
> and the C-007 frozen-winner outcome has no event in the vocabulary; the
> C-011 boundary answer is NO — a trace ACCEPT does not propagate to any
> other formal suite. No new event kind, no new mapping rule, no framework.

## Question

Can the EXISTING E9 trace-conformance channel distinguish the real
historical failure classes that FDG-0 Phase C exposed (#298), using only
the frozen event vocabulary, mapping rules, validator, corpus harness, and
pristine `E9ParkWake.tla`? No new trace framework; static FDG routing not
tuned; forbidden: generic function logging, line-number tracing, whole-
program event buses, LLM-derived labels, production always-on tracing,
pre-push/CI enforcement.

## Stage 0 — reuse audit (frozen matrix)

| capability | reality at BASE `9c79bf1c` | reuse |
| --- | --- | --- |
| semantic event vocabulary | 5 kinds, 7 wake causes, 4 return-cause bits (`tests/async_test_control_internal.hpp`) | unchanged |
| wake choke-point attribution | every wake recorded in `signal_wake_locked`; one-shot `set_trace_wake_cause` producer attribution | unchanged |
| recorder | sticky 64-event ring, overflow flag, enable/disable window (`E9TraceRecorder`) | unchanged |
| validator | `e9_trace_validate.py`: schema (40-hex same-revision binding), `compile_actions` fusion rules, generated `E9TraceReplay` wrapper, TLC existential replay, `--self-test` fail-closed legs | unchanged |
| corpus gate | `verify-e9-trace-conformance.sh` (7 accepts / 6 rejects + self-test; `--fresh` HEAD-bound C++ rerun) | unchanged |
| deterministic phase control | `test_phase` / `test_phase_worker` arming, state-based release (issue #115/#161 seam discipline: pause-only, no locks held) | unchanged |
| TLC toolchain | tla2tools 1.7.4 locked via `resolve-jar.sh` (checksum-verified) | unchanged |
| C-012 backend surface | real `SLUICE_HAS_LIBURING` semantics; scripted `io_uring_submit` hooks; SQ-full determinism; `UringWaitSource` split-phase wait | unchanged |

Gaps found (none inventable away): no seam can freeze the trace window
inside the C-001 protocol point; no C-012 scenario case exists; no build
world runs a broken-leg variant. All three are supplied by this task as
declared observation points / new cases / declared build worlds.

## TV1-A — C-001 lost wake (fix `422036cd`, gold F08 HIGH)

Design: two worlds, ONE deterministic external stimulus. The fiber
suspends on a not-yet-set ready flag and holds at its world's
suspend-protocol seam while a 2-worker live run parks the executor worker.
The coordinator sets the flag and publishes a `SchedulerWakeHandle::notify()`;
the executor's re-loop runs `wake_ready_flags_locked` DURING the hold.

- **Repaired world** (no defines): hold = existing
  `scheduler_suspend_before_physical_switch` (after the atomic
  register+recheck+commit critical section). The wake finds the
  registration with the fiber Waiting: make_runnable CAS succeeds, the
  ticket routes (`WakePublished{runnable_route}`) while the fiber is held.
- **Broken world** (`-DSLUICE_TV1_C001_MUTANT`):
  `await_ready_flag` reverted to the exact `422036cd^` shape (register
  under lock → recheck outside → suspension state change last) plus the
  preregistered `tv1_c001_registered_presuspend` pause seam inside the
  historical window (registered but still Running — the state the repaired
  critical section makes impossible). The same wake erases the
  registration with a FAILING CAS; the fiber then suspends unregistered.

Declared new observation point (repaired world, pause-only, internal-
testing guarded): `tv1_wake_scan_routed` — fired in the worker-loop
readiness-drain pass ONLY when the scan routed a ready-flag waiter. Without
it the window cannot be closed deterministically: after the route lands
with the owner held, the routing peer's park attempts refuse in a tight
loop (`ParkRefused` + `park_refuse` wake per attempt) and overflow the
sticky ring in under a millisecond — faster than any poll-based close
(observed: 64 events flooded before a 1 ms poll observed the boundary).
The seam holds no lock, emits no event, and compiles out of production.

Results (same stimulus, same choreography):

- Repaired: **TRACE_ACCEPT** —
  `python3 scripts/formal/e9_trace_validate.py --trace tv1a_c001_repaired.json --expect accept`
  → ACCEPT, actions `FinalParkRecheckAndCommit(W1), EnterPhysicalPark(W1),
  ExternalReadyPublish, LeavePark(W1), PublishRunnable`. The route
  publication is in-window: the wake reached the registered waiter — the
  boundary C-001 closed. The fiber completed; the testcase passed.
- Broken: **TRACE_REJECT** and **EXECUTION_DIVERGED**. The bounded
  fiber-done watchdog fired (`wait_flag` 10 s, healthy harness, runner
  joined): the fiber suspended unregistered, the run quiesced, no
  `WakePublished(runnable_route)` ever fired. The captured window — a real,
  executed, schema-valid trace — was then offered to the validator:

```json
{ "schema": 1, "suite": "e9_park_wake", "test": "tv1a_c001_broken",
  "cpp_revision": "89b86ab884b6324d352d54cc8fcb9c74e91df2ef",
  "model_revision": "89b86ab884b6324d352d54cc8fcb9c74e91df2ef",
  "split_wait": true, "run_mode": "live",
  "prehistory": "external_wait_registered",
  "events": [
    {"seq": 1, "event": "ParkCommitted", "worker": 1, "epoch": 0, "armed": true},
    {"seq": 2, "event": "ParkEntered", "worker": 1},
    {"seq": 3, "event": "WakePublished", "epoch": 1, "cause": "external"},
    {"seq": 4, "event": "ParkReturned", "worker": 1, "immediate": false, "causes": ["epoch"]},
    {"seq": 5, "event": "ParkCommitted", "worker": 1, "epoch": 1, "armed": false},
    {"seq": 6, "event": "ParkEntered", "worker": 1} ] }
```

  → REJECT (no behavior of the pristine model consumes the 6-step trace;
  58-state state graph, depth 14). Mechanism: the model's
  `LatentExternalWork == externalReady ∧ externalWaitRegistered`, and
  `BeginParkCandidate` / `FinalParkRecheckAndCommit` require
  `~LatentExternalWork`; the ONLY action that clears the pair is
  `DrainExternalReady` (observe + route). The broken world's continuation —
  flag set, waiter erased WITHOUT routing, worker re-parks at epoch 1 — is
  outside the model's language. Honest caveat: the REJECT is not a
  localized detection of the erasure itself (the model cannot express
  "erased registration"); it is the model refusing a shape that violates
  its park-admission law. The channel discriminates; it does not
  localize.

Evidence provenance (corrective C1 — the original run mislabeled both
traces with the BASE SHA `9c79bf1c`, but the executable necessarily
contained post-BASE specimen code: the `SLUICE_TV1_C001_MUTANT` branch,
`tv1_c001_registered_presuspend`, and `tv1_wake_scan_routed` exist only
from `104d3759` on; `write_trace_json` stamps the
`SLUICE_E9_TRACE_REVISION` string verbatim and the validator only checks
cpp_revision == model_revision as 40-hex — neither verifies
binary/source identity, so provenance honesty is a procedure obligation).
Both legs were re-run and re-validated at the committed corrective source
state:

| field | value |
| --- | --- |
| source revision (`cpp_revision` = `model_revision`) | `89b86ab884b6324d352d54cc8fcb9c74e91df2ef` (contains the full TV-1 specimen incl. the C4 guard fix; `E9ParkWake.tla` unchanged from BASE) |
| repaired build variant | default debug config (no extra defines) |
| broken build variant | same source + `-DSLUICE_TV1_C001_MUTANT` (compile-time variant, not a source revision) |

Permanent clarification for the #196 channel: **the revision binding
binds the SOURCE revision, not the compiler/build variant** — a mutant
define is a build variant of one source revision, recorded as such.
Rerun outcomes: repaired trace ACCEPT with the same action sequence;
broken trace byte-identical to the original six events (deterministic),
REJECT unchanged.

Prehistory sensitivity (non-scoring diagnostic, corrective C2): the same
six broken events with `prehistory` changed to `none`, replayed once, no
tuning either way → **REJECT as well — EVENTS_ALONE_SENSITIVE**: the
REJECT verdict is not an artifact of the declared prehistory. The
mechanism split is the informative part:

- scored declaration (`external_wait_registered`): the prelude compiles
  `PublishRunnable → RunFiber → SuspendFiber`, the waiter is registered,
  `ParkAdmitted` holds at event 1, and the refusal lands at event 5 — the
  re-park commit under `LatentExternalWork`, i.e. exactly the lost-route
  shape C-001 fixed;
- `none`: the prelude establishes only the worker population — no fiber,
  no registered wait, no backend work → `GlobalClass = "QUIESCENT"` →
  `ParkAdmitted` is FALSE → event 1's park commit is unrealizable and the
  trace dies at its first event (TLC: 14 states generated, 6 distinct,
  depth 4).

Reading: the event sequence and the prehistory/refinement mapping are
BOTH part of the correspondence authority. The events alone carry the
discriminative verdict (REJECT under either declaration); the
scenario-bound declaration carries the localization (without it the
model refuses the trace for the generic reason that a quiescent world
admits no park at all). "The trace alone found the bug" would overclaim;
"the executed trace + its declared refinement mapping are jointly
rejected by the pristine model" is what happened. Recorded as a
diagnostic; no mapping rule added or changed; nothing re-scored.

Discipline notes: zero new event kinds; the mutant is the named historical
defect only, well-formed C++, model untouched; the broken world was run
once (no tuning after first result); no trace was fabricated — the broken
artifact is the recorder's ring content from the actual run.

## TV1-B — C-012 cancel-path poison wake (fix `869be913` D4-RM17, gold F08 MEDIUM)

### B0 — default build world (with-liburing=false)

`uring_submit_failure_test` does not exist as a target
(`xmake show -t` → "not a valid target name"); the backend compiles the
stub: `cancel()` is a no-op and `wait_one()` returns 0
(`src/async/uring_backend.cpp:86-89`) — no running-op cancel, no transport
flush, no poison, no wake transport, no split-phase waiter. The scenario is
not expressible. Classification: **NOT_APPLICABLE_BUILD_WORLD**. Per the
task contract this is a coverage answer only: B0 conformant would be a
FAIL; nothing is claimed for C-012 from B0.

### B1 — with-liburing=true (real io_uring semantics)

Build identity: `xmake f -m debug --with-liburing=true`; target
`uring_submit_failure_test`; compiled TUs `src/async/uring_backend.cpp`,
`src/async/fail_fast.cpp`, `tests/uring_submit_failure_test.cpp`; defines
`SLUICE_HAS_LIBURING` + `SLUICE_ASYNC_INTERNAL_TESTING`. Scenario
(`uring_tv1_b1_cancel_path_poison_wake_waiter`, scripted submits only —
no invented protocol): a RUNNING kernel-side operation (blocked pipe read,
real `kRealSubmit`) is cancelled while the 4-entry physical SQ (depth-3
config) is full of dispatched-but-unsubmitted write SQEs; the cancel's
best-effort AsyncCancel append finds `get_sqe == null`; the retry flush is
scripted `-EIO` — a permanent failure that newly poisons the backend
(script `calls == 2` exactly). A waiter parks in the split-phase ready
wait BEFORE the stimulus (park announced through the deterministic
`set_wait_phase_flag_for_test` latch); between cancel and classification
no other driver touches the backend (the defect's geometry: "no reap runs
on this path").

- Repaired: the deferred `signal_ready_progress()` at the
  `issue_running_cancel` tail fires; the parked waiter returns with
  `BackendWakeReason::progress`; the driver's repoll publishes the retired
  Class-A terminals (4 × backend_error/EIO); teardown drains the
  surviving kernel-side read. Case PASSES (whole target ALL TESTS PASSED).
- Broken (`-DSLUICE_TV1_C012_MUTANT` = the pre-`869be913` early return):
  the wake is dropped. Signature: "waiter parked on published terminals
  (wake obligation missing)" — the waiter is still parked 5 s after the
  terminals published; the control plane (`interrupt_all`) recovers it;
  the case FAILS fail-closed with full cleanup. This case is also the
  regression coverage the fix shipped without: the mutant world cannot
  stay green.

Trace-channel result for BOTH B1 legs: **TRACE_COVERAGE_GAP** — the E9
vocabulary does not exist in this target (backend-domain waits are outside
the scheduler park/wake protocol; T4 precedent: "the participant parks in
the untraced backend domain"). The B1 evidence is runtime-level; the trace
channel claims nothing about C-012.

## TV1-C — C-011 (fix `7afb9378`, gold F08 HIGH, facet wake-publication, formal target e12-rwlock-scheduler-liveness)

Executed: the full E9 corpus gate
(`bash scripts/formal/verify-e9-trace-conformance.sh` → PASS, every
fixture ACCEPT incl. the TV1-A repaired trace) AND the E12 gate
(`bash scripts/formal/verify-e12-sched-liveness.sh` → PASS, 8 positive /
6 negative / 1 witness, verdicts identical to the established ones).

Answer to the exact question — **does an E9 TRACE_ACCEPT mechanically
prove e12-rwlock-scheduler-liveness has been revalidated? NO.**

1. `E9ParkWake.tla` contains NO contribution-generation/dance-epoch state
   (the C-011 repair's core mechanism — monotonic
   `dance_epoch_`, park-commit currency check — is modeled ONLY in
   `E12SchedLiveness.tla` as the contribution-identity law). The E9 model
   cannot express the repaired invariant or its M4 hazard.
2. The E9 corpus (t1–t5, TV1-A) contains no two-worker dancer/eraser
   convergence shape; the C-011 hazard is unreachable in every ACCEPTed
   trace.
3. The suites run under separate verifiers with zero cross-references; no
   cross-suite propagation machinery exists, and per the task contract none
   may be built.

An ACCEPT is "this named executed trace admitted by this named model" —
nothing more.

## TV1-D — C-007 (fix `96e3d66e`, gold F03 MEDIUM, frozen winner outcome)

Audit-only. The frozen-winner outcome lives in the per-Fiber
`CompletionWaitOutcome` slot (`fiber.hpp`), written by the drain /
cancel-waiter winner before `make_runnable` and read after the context
switch. The `TraceEvent` record (kind/worker/cause/immediate/return_causes/
armed/epoch) has no outcome field, none of the 5 event kinds observes
which winner published, and no corpus fixture drives a completion-wait
drain race. → **TRACE_COVERAGE_GAP**. No generic F03 tracing was built.

## Phase-C comparison

Frozen Phase-C facts (`fdg0-phase-c-historical-gold.md`): C-001, C-007,
and C-012 were MISSES; C-011 was a facet-level SAFETY FAILURE.

| Phase-C specimen | gold | Phase-C outcome (frozen) | TV-1 trace channel | TV-1 runtime |
| --- | --- | --- | --- | --- |
| C-001 lost wake `422036cd` (F08) | HIGH | **F08 MISSED** — structural-reach limit (no depth-2 path to the wake anchors; file-only baseline A caught it via file binding, the facet route did not) | repaired **ACCEPT** / broken **REJECT** | repaired converges; broken **EXECUTION_DIVERGED** (bounded, healthy harness) |
| C-012 poison wake `869be913` (F08) | MEDIUM | **F08 MISSED** — CONFIG_WORLD_CONFOUNDED (frozen world `with-liburing=false`; the diff's real io_uring path is not compiled there; post-hoc diagnosis) | B0 **NOT_APPLICABLE_BUILD_WORLD**; B1 **TRACE_COVERAGE_GAP** | B1 repaired wake present; broken wake dropped → fail-closed |
| C-011 dance-epoch `7afb9378` (F08) | HIGH | top-level claim surfaced BUT **PRECISE facet narrowing omitted `e12-rwlock-scheduler-liveness` — SAFETY FAILURE** | ACCEPT does **not** propagate (answer NO) | n/a (audit) |
| C-007 frozen winner `96e3d66e` (F03) | MEDIUM | **F03 MISSED** — authority-span limit (winner-outcome symbols in the scheduler wait domain; `scheduler.cpp` not in F03's file bindings) | **TRACE_COVERAGE_GAP** | n/a (audit) |

Apples-to-apples information gain over the frozen Phase-C results: where
static routing MISSED C-001, the runtime/trace channel discriminates it
(both levels); where static routing MISSED C-007, the trace channel is
ALSO blind — the vocabulary gap is confirmed at both layers, not healed;
where static routing missed C-012 because the frozen build world was
wrong, TV-1 names the build world per leg and confines the claim to the
runtime level; and where C-011's facet narrowing was unsafe, the trace
channel explicitly cannot substitute for downstream formal-obligation
propagation. The one Phase-C confusion TV-1 does resolve conservatively
is C-012's world confound: the build world is declared per leg, and
neither world claims trace coverage it does not have.

## Mutation disclosure

- Broken behaviors falsely ACCEPTED by the channel: **0** (the broken
  C-001 trace is REJECTED; the broken C-012 world is not offered to the
  trace channel at all — its result is the declared coverage gap plus the
  runtime fail-closed).
- Repaired behaviors falsely REJECTED: **0** (all repaired traces/scenarios
  accepted, including the entire standing corpus).

## Regressions (corrective round; C4 at `89b86ab8`, results regenerated per HEAD)

- `bash scripts/formal/verify-e9-trace-conformance.sh` → **PASS** (self-test
  + 7 accepts / 6 rejects, verdicts unchanged).
- `python3 scripts/formal/e9_trace_validate.py --self-test` → **PASS**.
- `bash scripts/formal/verify-e12-sched-liveness.sh` → **PASS** (15 gates,
  verdicts unchanged).
- C-001 legs re-run at the true source revision `89b86ab8…`: repaired
  **ACCEPT** / broken **REJECT** (six events byte-identical to the original
  run — deterministic; see provenance above); C2 diagnostic **REJECT**
  (`prehistory: none`), non-scoring.
- C-012 B1 legs re-run (`xmake f -m debug --with-liburing=true`): repaired
  **ALL TESTS PASSED** (17 cases incl.
  `uring_tv1_b1_cancel_path_poison_wake_waiter`); mutant
  (`-DSLUICE_TV1_C012_MUTANT`) **fails fail-closed** with the D4-RM17
  signature (waiter parked on published terminals, wake obligation
  missing).
- `python3 scripts/formal/formal_impact.py check --structure-only` → **OK**
  (#300/FTLR-0 registry structure).
- `python3 scripts/formal/fdg0_phase_a_eval.py run` → all_pass **true** at
  `89b86ab8` (committed `05d06761`); `fdg0_phase_b_eval.py run` →
  integrity all_pass **true** at `05d06761` (committed `7145b6ec`; the
  same known `unexpected_rows` as the established runs); Phase B frozen
  PRECISE 7-key mean reduction **33.33%** (authoritative, met);
  `fdg0_phase_c.py validate-gold` → **OK** (24 cases, 23 required, 12
  non-AMBIGUOUS positives, 12 expected claim labels).
- `python3 scripts/gates/mechanical-facts.py` → **OK** (LOC table synced
  for C4: `scheduler.cpp` 2266, attribution paragraph added).

## Scope fences

PRODUCTION SEMANTICS CHANGED: **NO** — the two mutant branches compile
only under `SLUICE_TV1_C001_MUTANT` / `SLUICE_TV1_C012_MUTANT`, defines
set by no repo config, CI, or gate; the `tv1_wake_scan_routed` freeze seam
is `SLUICE_ASYNC_INTERNAL_TESTING`-guarded, pause-only, lock-free, and
compiles out of production. STATIC FDG TUNED: **NO**. AST/CODEQL: **NO**.
ENFORCEMENT (pre-push/CI): **NO**. NEW EVENT KINDS: **0**. NEW MAPPING
RULES: **0**. NEW OBSERVATION POINTS: **2** (both pause-only, declared
above). Framework added: none.

## Final conclusion

The channel is EARNED where its vocabulary reaches and honest where it
does not: one of the two F08 park/wake specimens (C-001) is discriminated
at the trace level by the pristine model — the stronger outcome, since
the REJECT falls out of the model's park-admission law rather than from
any TV-1-specific tuning — while C-012/C-007 are named blind spots
and C-011 marks the hard boundary between behavioral trace correspondence
and formal-impact propagation. The C2 diagnostic sharpens what "at the
trace level" means: the REJECT verdict is carried by the six events
themselves (robust to the prehistory declaration), but the C-001-specific
localization — the refusal of the lost-route re-park, not a generic
realizability failure — is carried jointly with the declared
scenario-bound prehistory/refinement mapping. Both are part of the
correspondence authority; neither alone is the whole channel.
TRACE_CHANNEL_PARTIAL. No expansion; corrective round closed; returned
for adversarial review.
