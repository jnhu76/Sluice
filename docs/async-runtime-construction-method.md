# Async-Runtime Construction Method (Project Method Lock)

Status: **NORMATIVE** for E9-CORRECTIVE and all later async-runtime phases (E10+).

This is NOT a generic software-engineering essay. It is the project-specific
construction method distilled from the E9 corrective review, made binding so
future async-runtime work (E10 WaitNode/cancellation, E11 timers, E14
runtime closure) cannot repeat the E9 lifetime-conflation defect.

The decisive E9 defect this method exists to prevent:

```text
E7/E8 accepted the MW-S3 drain contract:

    unresolved wait remains
    no executable work
    no backend progress source
    current run invocation may return as STALLED

E9 implementation:

    MW-S3 + external_wake_possible
        -> park
        -> timed wake
        -> reclassify same MW-S3
        -> park forever

Result: normal-build deterministic hang; E7/E8 baseline broken.
```

The formal model (TLA+) passed because it modeled `persistent state`,
`worker park phase`, `wake epoch`, and `backend participant`, but **omitted**
`run invocation mode` and `run return state`. A formal model that omits one
load-bearing dimension must never be accepted merely because TLC is green
(see M2, M5).

---

## M1 — Production authority first

Before modifying protocol code, inventory the actual production source of
truth. Do not infer it from the plan, an old ADR, a TLA+ variable name, or a
final report.

Examples already learned in this codebase:

```text
Runnable ownership           -> Scheduler::fiber_owner_
Running execution            -> TLS g_worker / WorkerState::current
Waiting resume route         -> WaitReg.owner
Backend outstanding          -> ctx_.outstanding()
Persistent readiness         -> Completion ready flag / atomic<bool> ready
Wake epoch                   -> Scheduler::wake_epoch_ (under wake_mtx_)
Run invocation lifetime      -> RunMode (Drain | Live)  [INTRODUCED by E9-CORRECTIVE]
```

Forbidden authorities (never trust these as the source of truth):

```text
plan text
old ADR prose
TLA+ variable name
final report narrative
```

## M2 — Four-dimensional state topology

Every async protocol phase MUST explicitly audit four dimensions before a
production change is accepted:

```text
1. Resource / readiness state
2. Execution / ownership state
3. Coordination / admission state
4. Invocation / lifetime state
```

For E9 these are:

```text
resource:        runnable, backend outstanding/ready, external wait/ready
execution:       Worker/Fiber execution + E8 runnable ownership
coordination:    MW classifier, backend admission, park phase, wake epoch
invocation:      RunMode (Drain | Live); run return classification
                 (Active | ReturnedStalled | ReturnedQuiescent | Shutdown)
```

A formal model that omits one load-bearing dimension must not be accepted
merely because TLC is green. E9's original model omitted dimension 4 and
green-lit the hang.

## M3 — One protocol vocabulary

Reuse the accepted vocabulary; do not mint synonyms unless a genuinely new
protocol state is proven necessary.

Accepted terms:

```text
PUBLISH  MOVE  CONSUME
MW-S1  MW-S2  MW-S3  QUIESCENT
ownerRecord  execWorker  waitOwner
CANDIDATE  COMMITTED
persistent state  wake signal  wake epoch
RunMode (Drain | Live)        [added by E9-CORRECTIVE]
```

Forbidden synonyms (do not introduce):

```text
sleep-ready  soft-idle  semi-quiescent  wake-owned-task
```

## M4 — Explicit policy axis, no hidden semantic switch

A behavior difference caused by policy MUST be represented by an explicit
policy state. For E9:

```text
RunMode = Drain | Live
```

Forbidden designs (silent semantic switches):

```text
if wait-map nonempty:        silently change run lifetime
if wake handle exists:       silently change run lifetime
if backend type is ThreadPool: silently choose another semantic contract
```

RunMode is an explicit invocation policy; it MUST NOT be mutated by the
existence, copy count, retention, or destruction of a wake handle.

## M5 — Positive and negative formal model

For every load-bearing protocol change:

```text
correct model     -> accepted properties PASS
negative model    -> one isolated rejected rule
                  -> causally corresponding counterexample
```

The negative model MUST differ from the correct model by the smallest
semantic defect (one rule), not a compound of bugs. E9-CORRECTIVE adds
`E9ParkWakeBuggyDrainParks` whose only defect is the idle-action rule that
admits the Drain MW-S3 park; its counterexample is the deterministic hang.

## M6 — Refinement map before production

Before production implementation, map every formal concept to production:

```text
formal state/action
    -> production representation/function
    -> authority role
    -> lock/memory/execution domain
```

A formal variable must not ambiguously refine multiple production
authorities without an explicit abstraction argument. E9-CORRECTIVE extends
the refinement map with `runMode`, `runState`, `SelectIdleAction`, and the
physical wake sets per blocking action.

## M7 — Deterministic causal tests before stress

A race proof uses:

```text
phase seam  barrier  latch  condition  explicit test hook
```

NOT:

```text
sleep_for  "run 1000 times and hope"  "maximize the chance"
```

Stress is empirical evidence gathered AFTER the causal boundary is proven.
E9-CORRECTIVE replaces the sleep-based T3/T4 synchronization with
deterministic test seams at the candidate/commit causal boundaries.

## M8 — Reproducible verification

Every claimed repetition gate MUST be runnable from committed repository
artifacts. Distinguish honestly:

```text
historically executed
repository reproducible
independently rerun
```

The committed `scripts/verify-e{8,9}-stability.sh` runners and `.cfg` files
are the reproducible artifacts.

## M9 — Independent review reads production first

Independent refinement review order:

```text
production authority inventory
blocking/wake topology
state topology
formal model
refinement map
ADR
final report
```

Never begin by trusting the implementer's architecture story.

---

## Roadmap reference

This method is binding for:

```text
E9-CORRECTIVE  run lifetime contract separation (this task)
E10            WaitNode + cancellation-safe wait queue core
E11            Deadline / timer wait integration
E12            Async synchronization primitives (E12-A..G)
E13            Select / multi-wait winner protocol
E14            Threaded vs Evented semantic parity and runtime closure
```

An async-runtime phase that violates M1–M9 (e.g. omits a load-bearing
dimension, ships a hidden semantic switch, or relies on sleep-based causal
proof) is NOT accepted as CLOSED.
