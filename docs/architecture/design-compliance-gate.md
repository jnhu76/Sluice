# Architecture Design Compliance Gate

**Purpose:** Every change affecting async I/O architecture MUST pass this gate
before production implementation begins. The gate ensures ownership, state,
wake, resource, and failure semantics are explicitly designed — not discovered
during code review.

**Trigger:** This gate applies to any change that:

- Adds or modifies an `AsyncBackend` implementation
- Modifies `Completion<T>` state machine or publication
- Modifies `submit_*` / `poll()` / `wait_one()` / `cancel()`
- Modifies Scheduler wake/progress/park domains
- Modifies Runtime ownership, lifecycle, or admission
- Adds a thread pool, executor, or queue
- Modifies pipeline concurrency or resource defaults
- Adds a synchronization primitive
- Modifies I/O cancellation semantics
- Modifies public async API surface
- Introduces coroutine/P2300/Asio/libuv/uv
- Introduces a new OS backend

**Non-trigger:** Pure documentation, formatting, test-only changes that do not
alter production semantics, and synchronous-core-only changes do not require
this gate.

---

## Gate 0 — Architecture Classification

Before writing any code, the design MUST declare:

```text
Affected capability:    [AsyncIoContext | Scheduler | Runtime | Backend | Group | Batch | primitive]
Affected layer:         [L0 backend | L1 context | L2 runtime | E7-E13 scheduler | E16 runtime]
Classification:         [Faithful | Intentional Divergence | Corrective | New Direction]
Governing ADR:          [ADR name or "new ADR required"]
Conformance map change: [yes/no — if yes, which rows change]
Constitution rules:     [AC-N list that this change touches]
```

**Blocking:** If any field is "unknown" or "TBD," coding MUST NOT begin.
Resolve the classification first.

---

## Gate 1 — Ownership and State Machine

The design MUST provide a state machine for every new or modified lifecycle
object. Example format:

```text
States:
  idle → admitted → pending → executing/kernel-owned → backend-ready → reaped → reusable

Transitions:
  idle → admitted
    Authority:      [who triggers]
    Lock domain:    [which mutex/atomic]
    Allocation:     [none | bounded | unbounded — justify]
    Failure:        [what happens if this transition fails]
    Wake:           [who must be notified]
    Shutdown:       [what happens during shutdown]

  admitted → pending
    Authority:      ...
    ...
```

**Required for each transition:**
1. Authority (exactly one owner)
2. Lock/atomic domain
3. Allocation possibility (none / bounded / unbounded)
4. Failure behavior (rollback / error completion / terminate)
5. Wake obligation (who sleeps, who signals)
6. Shutdown behavior (drain / abandon / fail-fast)

**Blocking:** If any transition lacks an authority or failure behavior, the
design is incomplete.

---

## Gate 2 — Resource and Failure Model

The design MUST answer:

```text
Construction-time resources:
  - [resource]: capacity=[N], allocation=[stack|heap|preallocated], failure=[error|terminate]

Submit-time resources:
  - [resource]: capacity=[N], allocation=[...], failure=[...]
  - Is submit allocation-free after acceptance? [yes/no]

Completion-time resources:
  - [resource]: capacity=[N], allocation=[...], failure=[...]
  - Can a completed result be lost due to allocation failure? [yes/no — if yes, justify]

Capacity and backpressure:
  - Maximum outstanding operations: [N | unbounded — justify]
  - Queue-full behavior: [error code | block | retry]
  - OOM at each stage: [synchronous error | operation error | terminate]

Reclamation:
  - Do containers shrink when load decreases? [yes/no]
  - Is growth bounded by outstanding or by historical total? [outstanding | historical]
```

**Blocking:** If submit success can be followed by permanent operation loss due
to allocation failure, the design violates AC-4 and MUST be revised.

---

## Gate 3 — Progress and Wake Model

The design MUST answer:

```text
Blocking/suspension:
  - Who may block? [caller thread | worker thread | scheduler worker]
  - Who may suspend? [Fiber | none]
  - What makes them continue? [signal source]

Backend → Scheduler progress:
  - How does backend-ready reach the Scheduler? [poll | wait_one | direct wake | timeout]
  - Is this mechanism signal-based or observation-based?
  - What is the worst-case latency?

External wake coexistence:
  - Can external wake and backend progress coexist without lost wake?
  - What closes the commit-to-sleep race? [lock+epoch | condition | timeout]

Polling dependency:
  - Does progress depend on a periodic timeout? [yes/no]
  - If yes: is it protocol authority, observation fallback, or deadline?
  - What happens if the timeout is removed?

Single-worker liveness:
  - With one scheduler worker, can a runnable task make progress while
    another task is suspended on backend I/O? [yes/no — explain]
```

**Blocking:** If the answer to "what makes them continue" is "periodic poll"
without explicit justification and bounded interval, the design violates AC-6.

---

## Gate 4 — Evidence Plan

Before implementation, the design MUST list the tests that will prove
correctness. After implementation, actual results MUST be filled in.

```text
Deterministic causal tests:
  - [test name]: proves [property]
  - [test name]: proves [property]

Backend conformance:
  - Runs against: [ThreadPool | Uring | Fake | Sync | all]
  - [test name]: proves [property]

Resource-bound tests:
  - [test name]: proves capacity=[N], full behavior=[...]

OOM / failure-injection:
  - [test name]: injects failure at [point], verifies [recovery]

Shutdown race tests:
  - [test name]: proves [convergence property]

Sanitizers:
  - [ ] ASan+UBSan clean
  - [ ] TSan clean (if concurrency)

Benchmark (if performance claim):
  - [benchmark name]: workload=[...], result=[...]
```

**Blocking:** Pre-filling "PASS" before execution is forbidden. Each test MUST
fail on the pre-fix code (for bug fixes) or be demonstrably reachable (for new
features).

---

## Gate Completion Checklist

Before merging, the PR author confirms:

- [ ] Gate 0 classification is complete and accurate
- [ ] Gate 1 state machine covers all new/modified lifecycles
- [ ] Gate 2 resource model has no unbounded growth without ADR approval
- [ ] Gate 3 wake model has no undocumented polling dependency
- [ ] Gate 4 evidence is filled with actual results
- [ ] Conformance map updated (if classification changed)
- [ ] Divergence registry updated (if new intentional divergence)
- [ ] Constitution rules satisfied or exception approved
- [ ] AGENTS.md change-class gates run (Debug, Release, sanitizers as applicable)

---

## Relationship to AGENTS.md

This gate supplements (does not replace) the AGENTS.md §6 change-class gates.
The AGENTS.md gates verify build/sanitizer/test health. This gate verifies
architectural design completeness. Both are required for architecture-affecting
changes.
