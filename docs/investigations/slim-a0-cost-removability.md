# SLIM-A0 — Cost / Removability Census

Issue: #301  
Branch: `research/slim-x0`  
Base master: `48f89d90cf228e2055d81d5c98ce0ab9f7068721`

## Status

```text
STATIC CENSUS: COMPLETE ENOUGH TO FREEZE FIRST A1 CANDIDATE
GENERATED-CODE MEASUREMENT: PENDING LOCAL/NATIVE RUN
RUNTIME MEASUREMENT: PENDING LOCAL/NATIVE RUN
PRODUCTION MASTER CHANGES: NONE
```

This document is intentionally written on the permanent SLIM line. It is not a proposal to rewrite `master`.

---

# 1. Frozen question

> For a deployment where the backend type is known, what runtime machinery remains only because master preserves backend/runtime generality, and how much of that machinery can be removed without weakening the frozen request/completion/lifetime safety boundary?

A0 does not assume that runtime polymorphism is materially expensive. It first identifies exactly where it exists and freezes the cheapest same-semantics intervention.

---

# 2. Reference path recovered from current master

For an application task submitting one positional I/O:

```text
user RuntimeTaskFn
  ↓
ApplicationRuntime::submit
  ↓
Group::async wrapper
  ↓
RuntimeTaskContext
  ↓
RuntimeTaskContext::submit_{read,write,...}
  ↓
AsyncIoContext::submit_{read,write,...}
  ↓
AsyncIoContext::access_mtx_
  ↓
virtual AsyncBackend::submit_*
  ↓
backend request/admission authority
  ↓
RequestArena / Completion binding / terminal machinery
```

Completion/wait side:

```text
RuntimeTaskContext::await_completion
  ↓
Scheduler::await_completion_*
  ↓
backend waiter / ReadySink / request identity machinery
  ↓
reap / publication / wake
  ↓
Fiber resume
```

The first line is not a claim that every arrow is expensive. It is the physical mediation chain to measure.

---

# 3. Layer census

| Layer | Current role | Classification | Per-I/O? | Initial SLIM disposition |
|---|---|---|---:|---|
| `RuntimeBuilder` | runtime configuration + backend injection | EXECUTION_POLICY / GENERICITY | no | STATICIZE LATER |
| `ApplicationRuntime` | lifecycle, dedicated driver, root Group, root cancellation | CORRECTNESS + EXECUTION MECHANISM | mixed | KEEP FOR A1 |
| `RuntimeTaskContext` | restricted task surface; three non-owning pointers; submit forwarding | SEMANTIC FACADE / GENERICITY ADAPTER | yes | MEASURE / COLLAPSE LATER |
| `RuntimeTaskFn = std::function<...>` | runtime task type erasure | GENERICITY | per task, not per I/O | NOT FIRST |
| `AsyncIoContext` | backend ownership, serialized backend access, stats, split-wait routing | CORRECTNESS + GENERICITY | yes | FIRST TARGET: STATIC BACKEND TYPE ONLY |
| `AsyncIoContext::access_mtx_` | one backend access domain | CORRECTNESS / SERIALIZATION | yes | KEEP IN A1 |
| `AsyncBackend` virtual interface | runtime backend substitution + optional capability surface | GENERICITY / BACKEND CAPABILITY | yes | STATICIZE IN A1 |
| capability queries (`wait_source`, `supports_*`, identity support) | runtime capability discovery | BACKEND CAPABILITY | some hot/some cold | CONSTEXPR/CONCEPT CANDIDATE |
| `RequestArena` / generation | stale identity / lifecycle | CORRECTNESS KERNEL | yes | KEEP |
| Completion binding/publication | exactly-one publication / borrow lifetime | CORRECTNESS KERNEL | yes | KEEP |
| Scheduler waiter/wake protocol | suspend/resume + lost-wake closure | CORRECTNESS + EXECUTION MECHANISM | yes | KEEP FOR A1 |
| stats | optional observation | OBSERVATION | yes when enabled | KEEP PAY-FOR-PLAY |

---

# 4. Concrete source facts motivating A1

## F1 — RuntimeTaskContext submit methods are direct forwarding

The four ordinary submit methods simply forward to `ctx_->submit_*`. The context object carries non-owning pointers to `AsyncIoContext`, `CancelToken`, and `Scheduler`.

Implication:

```text
RuntimeTaskContext is a real authority-restriction/API boundary,
but its submit path does not itself add request semantics.
```

Do not delete it in A1: it also intentionally prevents raw Scheduler/backend authority from escaping. It becomes a later facade-collapse candidate only after generated-code evidence.

## F2 — backend type is erased before the hot submit path

`RuntimeBuilder` accepts `std::unique_ptr<AsyncBackend>` and `ApplicationRuntime` constructs an `AsyncIoContext` from that erased pointer.

`AsyncIoContext::submit_*` then executes:

```cpp
lock(access_mtx_)
backend_->submit_*(...)
stats tally
optional max-outstanding tally
```

The backend call is therefore virtual in the master representation.

## F3 — AsyncBackend combines required operations with optional capabilities

The base class carries virtual operations for:

```text
submit read/write/sync
poll / wait_one
cancel
outstanding
wait_source
wait_one_is_nonblocking
request identity support/resolution
waiter registration/cancellation
```

Several optional methods default to `nullptr`, `false`, or `not_supported`.

For a deployment with a statically known backend, much of this capability discovery can in principle become type/concept/constexpr information.

## F4 — `access_mtx_` is not genericity fluff

Current `AsyncIoContext` explicitly defines `access_mtx_` as the serialized backend-access domain. It also protects plain stats accounting in split-wait paths.

Therefore A1 MUST NOT remove it merely because a static backend exists.

A future no-lock profile requires a separate proof that its execution topology has exactly one backend mutator/reaper domain, plus a same-semantics race/safety argument.

## F5 — master already fixed one fake pay-for-play cost

The old unconditional `backend_->outstanding()` evaluation with stats disabled was removed from production (#261 / TAX F01). This is an important negative control:

```text
SLIM must not claim generic 'abstraction tax' that master already optimized away.
```

---

# 5. Ranked candidates after static census

## Candidate 1 — A1: Static backend boundary

**Freeze as first intervention.**

Question:

> If backend type is compile-time known, can we remove runtime backend polymorphism/capability discovery from the matched path while preserving `AsyncIoContext` serialization, RequestArena correctness, Completion semantics, wait/wake behavior, and resource bounds?

Research arms:

```text
M0 MASTER-ERASED
   RuntimeTaskContext
     -> AsyncIoContext
     -> AsyncBackend virtual
     -> concrete backend

M1 SLIM-STATIC-BACKEND
   RuntimeTaskContext
     -> StaticAsyncIoContext<Backend>
     -> Backend direct call

M2 is NOT authorized in A1.
```

M1 must retain the same logical operations and the same `access_mtx_` domain. The only intended causal variable is runtime backend erasure/capability dispatch.

Expected compile-time opportunities:

```text
virtual submit/poll/cancel/outstanding -> direct calls
supports_* / wait_source availability -> concepts / constexpr traits where legitimate
impossible backend/profile combinations -> static_assert / constrained construction
backend ownership -> concrete member or unique_ptr<Backend>, chosen by layout/lifetime evidence
```

Do not create a generic template framework beyond what M1 needs.

### Falsifier

A1 is a negative result if optimized master already devirtualizes the relevant calls or if measured runtime improvement is negligible relative to code-size/compile-time/diagnostic cost.

Verdict vocabulary:

```text
STATIC_BACKEND_EARNED
STATIC_BACKEND_ZERO_RUNTIME_VALUE
STATIC_BACKEND_COST_EXCEEDS_VALUE
INCONCLUSIVE
```

---

## Candidate 2 — RuntimeTaskContext facade collapse / static task context

Potential later experiment:

```text
RuntimeTaskContext
  three runtime pointers
  + forwarding submits
```

Possible static profile could bind I/O + scheduler authority more directly while preserving restricted task authority.

Not first because the facade may optimize away completely already, and its authority-restriction role is useful even if runtime cost is zero.

Status: `MEASURE_AFTER_A1`.

---

## Candidate 3 — ApplicationRuntime / root Group / task wrapper topology

`ApplicationRuntime::submit` currently:

```text
locks lifecycle state
reserves admitted_count
Group::async
captures std::function task in wrapper lambda
sets Fiber execution tag
constructs RuntimeTaskContext
runs task
locks lifecycle again
increments terminal_count
notifies runtime CV
notifies Scheduler wake handle
```

This is a substantial lifecycle/composition path, but much of it buys shutdown/admission/worker-call correctness and is per-task rather than per-I/O.

Potential SLIM fixed-topology runtime may eventually collapse dedicated lifecycle components or use a smaller run-to-completion profile.

Status: `HIGH STRUCTURAL INTEREST / NOT FIRST PERF INTERVENTION`.

---

# 6. A1 generated-code experiment — required

Build Release with Clang for both arms using identical optimization/LTO settings.

Record:

```text
compiler/version
flags
LTO on/off
binary hash
text size / total binary size
compile wall time
peak compiler RSS if easy
```

Inspect the matched submit/read path with available LLVM/binutils tools:

```bash
llvm-objdump -d -C <binary>
llvm-nm -C <binary>
llvm-size <binary>
```

If supported, add Clang optimization remarks:

```text
-Rpass=inline
-Rpass-missed=inline
-Rpass-analysis=inline
```

Answer mechanically:

```text
M0 backend call direct or indirect?
M1 backend call direct or indirect?
which capability branches disappeared?
which wrappers inline away already in M0?
code-size delta?
compile-time delta?
```

Do not infer devirtualization from source syntax.

---

# 7. A1 runtime experiment — required

Use the existing same-work performance discipline. Prefer a current native Linux / real io_uring-capable host; WSL2 may be a pilot only.

Compact frozen matrix:

```text
operation: READ, WRITE
size:      4 KiB, 64 KiB
depth:     1, 8, 32
stats:     OFF primary; ON control
```

Keep all of the following matched:

```text
same backend implementation
same RequestArena capacity
same Completion lifecycle
same buffer/file setup
same worker/topology
same poll/wait strategy
same useful ops/bytes
same compiler flags
```

Primary metrics:

```text
instructions/op
cycles/op
```

Secondary:

```text
wall ns/op
branches/op + branch-miss if reliable
code size
compile time
```

Do not claim storage throughput improvement if the signal is only visible in control-dominated microcells.

---

# 8. Safety/conformance gate for A1

M1 must preserve at least the applicable existing behavior for:

```text
B01 request identity / generation
B02 submit rollback / admission
B03 terminal winner
B04 Completion publication
B05 borrowed storage lifetime
B06 cancel vs ordinary completion
B09 resource accounting / retirement
B10 shutdown/quiescence interactions applicable to the profile
```

Because A1 does not redesign Scheduler/wait semantics, B07/B08 must remain untouched rather than re-proved by a new implementation.

Required rule:

```text
same existing backend/request tests must be runnable against M0 and M1
```

Where static construction rejects an impossible backend/profile combination, add a negative-compile test rather than a runtime branch.

---

# 9. Explicitly forbidden in A1

```text
NO removing access_mtx_
NO rewriting RequestArena
NO changing Completion lifecycle
NO changing cancellation semantics
NO new scheduler
NO removing Group/ApplicationRuntime lifecycle
NO fixed buffers / registered files
NO SQPOLL or backend-feature confounders
NO public master API change
NO merge-back to master
```

The point of A1 is causal cleanliness:

> runtime backend genericity versus static backend composition.

---

# 10. A1 closure template

```text
SLIM_A1_RESULT

BASE_MASTER: 48f89d90cf228e2055d81d5c98ce0ab9f7068721
BRANCH: research/slim-x0
M0_HEAD:
M1_HEAD:

CAUSAL VARIABLE:
  runtime-erased backend -> static backend type

SEMANTICS:
  matched:
  deviations: NONE / ...

GENERATED CODE:
  M0 backend call:
  M1 backend call:
  branches removed:
  text size delta:
  compile time delta:

RUNTIME MATRIX:
  instr/op:
  cycles/op:
  wall/op:

SAFETY / CONFORMANCE:
  B01:
  B02:
  B03:
  B04:
  B05:
  B06:
  B09:
  B10:

VERDICT:
  STATIC_BACKEND_EARNED
  / STATIC_BACKEND_ZERO_RUNTIME_VALUE
  / STATIC_BACKEND_COST_EXCEEDS_VALUE
  / INCONCLUSIVE

NEXT:
  If positive, decide whether to widen staticization to RuntimeTaskContext/runtime composition.
  If negative, retain the evidence and move to the next independently motivated layer.
```

---

# 11. A0 verdict

```text
A0 VERDICT: FIRST A1 CANDIDATE FROZEN

Candidate:
  static backend boundary inside a same-semantics AsyncIoContext-shaped control.

Why first:
  - directly targets runtime genericity;
  - per-I/O;
  - narrow causal variable;
  - Request correctness can remain untouched;
  - master/reference behavior remains an oracle;
  - can fail cleanly if the compiler already removes the cost.
```

The next code change on `research/slim-x0` should implement M1 only. Do not start a broad SLIM rewrite from this audit.