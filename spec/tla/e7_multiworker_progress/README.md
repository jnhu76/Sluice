# E7 Multi-Worker Progress / Blocking-Admission Protocol — TLA+ Model

Formal model of the E7 multi-worker progress and blocking-admission protocol
(sluice-CORE-E7 §9.2 of ADR-execution-model). Independently confirms the
protocol's abstract safety and demonstrates that the two production
admission decisions the ADR highlights (authoritative backend outstanding
for MW-S2; final readiness re-drain before commit) are each load-bearing.

This is a **companion** to `spec/tla/e7_publication/` (the runnable-
publication model, commit 63ed522). Where that model closes the
exactly-once publication contract, this one closes the **global MW-state
classification + two-phase blocking admission** contract that sits on top
of it.

## Files

- `E7MultiWorkerProgress.tla`               — correct protocol (MW-Inv hold)
- `E7MultiWorkerProgress.cfg`               — TLC config (exhaustive)
- `E7MultiWorkerProgressBuggyOutstanding.tla` — §9 defect: MW-S2 derived from wait regs
- `E7MultiWorkerProgressBuggyOutstanding.cfg`
- `E7MultiWorkerProgressBuggyAdmission.tla`   — §10 defect: commit skips final re-drain
- `E7MultiWorkerProgressBuggyAdmission.cfg`
- `README.md`                                 — this file + refinement map

## Running

```bash
java -cp /tmp/tla2tools.jar tlc2.TLC -config E7MultiWorkerProgress.cfg               E7MultiWorkerProgress
java -cp /tmp/tla2tools.jar tlc2.TLC -config E7MultiWorkerProgressBuggyOutstanding.cfg E7MultiWorkerProgressBuggyOutstanding
java -cp /tmp/tla2tools.jar tlc2.TLC -config E7MultiWorkerProgressBuggyAdmission.cfg   E7MultiWorkerProgressBuggyAdmission
```

## Model domain (finite, exhaustive TLC)

```text
Fibers  = {F0, F1}
Workers = {W0, W1}
```

## State dimensions

- **Fiber execution state** — `[Fibers -> {Waiting, Runnable, Running, Done}]`
- **worker-local runnable/running** — `[Workers -> SUBSET Fibers]` / `[Workers -> Fibers ∪ {NONE}]`
- **wait registrations** (DISTINCT from outstanding):
  - `completionWait[f]`  — f awaits a Completion
  - `readyFlagWait[f]`   — f awaits a ready flag
- **persistent readiness** (level-triggered, survives until drained):
  - `completionReady[f]`, `readyFlagReady[f]`
- **authoritative backend outstanding** — `backendOutstanding ∈ Bool`
- **coordinated admission** — `admission ∈ {None, Candidate_Wx, Committed_Wx}`
- **blocking participant** — `blockingWorker ∈ Workers ∪ {NONE}`

## Global classifier (authoritative)

Mirrors `Scheduler::classify_locked()` (ADR §9.2.6):

```text
MW_S1       = AnyRunnable ∨ AnyRunning
MW_S2       = ¬MW_S1 ∧ backendOutstanding      ← authoritative, NOT wait regs
MW_S3       = ¬MW_S1 ∧ ¬backendOutstanding ∧ AnyWaitRegistration
QUIESCENT   = ¬MW_S1 ∧ ¬backendOutstanding ∧ ¬AnyWaitRegistration
```

**Load-bearing:** MW_S2 uses `backendOutstanding`, the authoritative backend
counter — NOT `AnyWaitRegistration`. This is the §9 invariant.

## Results

| model | distinct states | depth | result |
|-------|----------------:|------:|--------|
| `E7MultiWorkerProgress` (correct) | 294 | 15 | **MW-Inv PASS** — no error |
| `E7MultiWorkerProgressBuggyOutstanding` | 26 | 5 | **counterexample** — `InvMWS2ImpliesOutstanding` violated |
| `E7MultiWorkerProgressBuggyAdmission` | 114 | 9 | **counterexample** — `InvBlockingNoUndrainedReady` violated |

### Correct-model invariants (all PASS)

- **MW-Inv1** (transition/structural): commit requires MW_S2 at the commit
  moment. Enforced by `FinalAdmissionRecheckAndCommit` precondition. The
  state-level companion `MWInv1CommitBlocksAtomicly` (commit ⇒ blocking) holds.
- **MW-Inv2**: at most one committed participant AND at most one blocking worker.
- **MW-Inv3**: blocking requires committed admission.
- **MW-Inv4** (structural): commit requires authoritative MW_S2 at final recheck.
- **MW-Inv5** (transition/structural): commit requires no ready-registered
  waiter undrained (`~ReadyRegUndrained` precondition).
- **MW-Inv6**: MW-S3 is not logical quiescence.
- **MW-Inv7**: MW_S2 ⇒ backendOutstanding (authoritative).
- **MW-Inv8** (structural): no local inference of global MW-S2; the classifier
  is global.

### Buggy-outstanding counterexample (§9)

```text
F0 completionWait=TRUE, F1 completionWait=TRUE
backendOutstanding = FALSE          ← no backend op submitted (ready-flag/no-op case)
MW_S2 = TRUE (buggy: derived from wait regs)
InvMWS2ImpliesOutstanding VIOLATED: MW_S2 ∧ ¬backendOutstanding
```

A worker would elect candidate → commit → enter blocking wait_one with no
backend op to reap → blocks forever. The correct classifier (`MW_S2 ⇐
backendOutstanding`) prevents this: the state is MW-S3, not MW-S2.

### Buggy-admission counterexample (§10)

```text
ElectCandidate(W0) under MW_S2
[concurrent] MakeCompletionReady(F0)  ← ready reg appears
FinalAdmissionRecheckAndCommit(W0)    ← BUGGY: skips ~ReadyRegUndrained check
blockingWorker = W0, ReadyRegUndrained = TRUE
InvBlockingNoUndrainedReady VIOLATED
```

The committed worker enters blocking while a runnable-ready waiter exists.
The correct protocol's `~ReadyRegUndrained` precondition at commit forces the
drain first, flipping the state to MW_S1 and cancelling the candidate.

## Refinement map (TLA+ action → production path)

| TLA+ action | production function / path | lock domain |
|-------------|---------------------------|-------------|
| `MakeCompletionReady` / `MakeReadyFlagReady` | backend `wait_one` internal poll; external flag setter | backend-internal / external |
| `BackendOpOutstanding` / `BackendOpResolved` | `AsyncIoContext::submit_*` / completion reap | `access_mtx_` |
| `DrainReadyCompletion` / `DrainReadyFlag` | `wake_ready_completions_locked` / `wake_ready_flags_locked` | `global_mtx_` |
| `RunFiber` | `worker_loop` pop + `run_next_on` → `make_running` | `inbox_mtx` |
| `SuspendOnCompletion` / `SuspendOnReadyFlag` | `await_completion_*` / `await_ready_flag` (register + make_waiting + switch) | `global_mtx_` (register) |
| `FinishFiber` | `fiber_entry_bridge` → `make_done` | single-threaded on worker |
| `ElectCandidate` | `classify_locked` == MW_S2 + `admission_ = candidate` | `global_mtx_` |
| `FinalAdmissionRecheckAndCommit` | re-drain + `classify_locked` recheck + `admission_ = committed` + `wait_one` | `global_mtx_` (recheck); released before `wait_one` |
| `CancelCandidate` | reclassify shows non-MW_S2 → `admission_ = none` | `global_mtx_` |
| `BackendProgressOrWaitReturn` | `wait_one` returns; re-drain under `global_mtx_` | `global_mtx_` |
| `WorkerObserveOrReclassify` | loop-top `classify_locked` | `global_mtx_` |

### The two load-bearing production decisions (each refines a checked invariant)

1. **MW_S2 derived from `backendOutstanding`** (production `classify_locked` uses
   `ctx_.outstanding()`, NOT wait-map size). Refines MW-Inv7. The buggy-outstanding
   model shows the wait-reg derivation admits a spurious-blocking counterexample.

2. **Final readiness re-drain before commit** (production calls
   `wake_ready_completions_locked` + `wake_ready_flags_locked` at lines 274-275
   before the MW_S2 recheck at commit). Refines MW-Inv5. The buggy-admission
   model shows omitting it admits a blocking-while-runnable counterexample.

## Scope (explicit non-goals)

- Does NOT model E8 work-stealing or Fiber migration.
- Does NOT model AsyncBackend internals, context-switch asm, stack layout,
  io_uring, ThreadPool, Future values, Group, timers, cancellation, performance.
- Does NOT model runnable publication cardinality (closed by `e7_publication/`).
- Does NOT claim the C++ implementation is bug-free; the empirical validation
  (T2 5000/5000, full suite GREEN, TSan 0 races) is the production-stability
  gate. This model is the independent protocol-safety confirmation.

## Conclusion

```text
The abstract E7 multi-worker progress / blocking-admission protocol preserves
MW-Inv1-8 for the checked finite model.

The two buggy producer rules (wait-reg-derived MW-S2; commit-skips-re-drain)
each admit a counterexample matching the ADR §9.2.6 / §9.2.9 hazard description.

The production implementation refines the checked protocol: classify_locked
uses authoritative backendOutstanding, and the admission path re-drains ready
registrations before the final MW_S2 recheck at commit.
```
