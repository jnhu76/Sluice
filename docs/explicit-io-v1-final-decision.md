# Sluice v1 Explicit-I/O Architecture — Final Decision

**Status:** FINAL ARCHITECTURE DECISION for the next Sluice implementation phase  
**Decision baseline:** `c64f005e6e59e791f26a7ab4a594c33954f096dd`  
**Scope:** product architecture, ownership/lifetime, execution abstraction, host/runtime boundary, reliability direction  
**Not an implementation patch:** this document does not by itself authorize a particular class name, API spelling, state-machine compression, or deletion.

This document supersedes the earlier assumption that Sluice's North Star is permanently split into a `blocking` API and an `async` API. Those surfaces may remain as C++ invocation forms, but they are not the architectural authority.

Issue #387 remains the code-reality reconstruction and historical entry point. This document is the normative target against which subsequent code changes are judged.

---

## 1. Product goal

Sluice v1 is a modern C++20 **explicit file-I/O library**.

Its design direction is inspired by Zig 0.16's explicit I/O architecture, especially these principles:

- I/O execution capability is explicit rather than hidden in global policy.
- logical resource/operation semantics have one authority;
- the application chooses or binds the execution implementation;
- operation-level concurrency and task-level concurrency are separate abstractions;
- cancellation, completion, settlement, and resource lifetime form one coherent contract;
- backend mechanisms do not silently redefine File semantics.

Sluice does **not** attempt to copy Zig API syntax, Zig's `Io` type breadth, Zig's vtable layout, or Zig's stackful runtime semantics literally.

The design must remain idiomatic C++ where C++ has stronger native constraints, especially **RAII**.

---

## 2. Frozen architecture principles

The following principles are normative.

### N1 — File semantics have one authority

`File` is the canonical owned file resource for v1.

Open/access/close ownership, read/write meaning, positional semantics, shared cursor semantics, short I/O, EOF, sync/durability, metadata, and error classification must not acquire separate meanings merely because execution is direct, threaded, io_uring-based, fiber-hosted, coroutine-hosted, or externally driven.

A backend may expose capability limitations or reject unsupported execution forms. It may not silently redefine the logical operation.

### N2 — Logical operation, execution capability, and invocation/observation are separate axes

Every architecture review must distinguish:

```text
Logical operation
    What is being done to the resource and what does the result mean?

Execution capability
    Who may execute/progress the operation, using what execution resources?

Invocation / observation
    When does the caller regain control and how is the result observed?
```

These axes must not be collapsed into a single `blocking` versus `async` dichotomy.

The same execution mechanism can support multiple observation contracts. For example, one ThreadPool-backed implementation may support a completed-return call, an explicit outstanding request, a fiber suspension adapter, a coroutine adapter, or a host notification path.

### N3 — Explicit execution capability is required; its C++ representation is not frozen

The application must explicitly supply or explicitly bind the I/O execution capability used by an operation.

The architecture freezes the **responsibility**, not the type name or syntax.

The final C++ representation may be an execution context, capability object, executor-like handle, explicit binding, template parameter, or another mechanism justified by the accepted workloads.

This document does **not** freeze any of the following:

```cpp
Io&
IoProvider&
Executor&
ExecutionContext&
```

nor does it require every operation to take such an object as a function parameter.

Per-call supply and explicit object binding are both permitted architectural shapes, provided execution choice remains understandable and non-global.

### N4 — The old blocking/async dual surface is not the North Star

`blocking::*` and current async/File adapters may continue to exist as C++ invocation forms, migration surfaces, or convenience APIs.

They do not own two independent file-semantic worlds and are not permanently required as the product's top-level architecture.

A future common logical operation layer may have several invocation forms with different return/control-flow types. “One logical API” does not require identical C++ signatures or identical return types across all host environments.

### N5 — Completed-return and outstanding-request are distinct valid contracts

Sluice must distinguish at least these two semantic invocation families:

1. **completed-return** — when the call returns, the operation's internal borrow has ended and the final result is available;
2. **outstanding-request** — a successful admission returns while an operation remains in flight, and the caller receives explicit responsibility for observing/settling it.

A provider argument by itself does not make an ordinary C++ call transparently suspendable.

Sluice does not promise that one ordinary non-coroutine function signature can simultaneously:

- return only after final completion,
- never block an arbitrary host thread,
- and work without a supported coroutine/continuation/stackful suspension environment.

### N6 — Operation-level concurrency and task-level concurrency are distinct

Outstanding file operations are a lower-level product concern.

General function/task concurrency (`Future`, `Group`, Scheduler/Fiber-style hosting, structured task scopes) is a separate layer.

The file I/O request core must not require Sluice's own general task runtime in order to exist or make progress under a supported host.

A task runtime may consume the request core. The request core must not gain product dependence on the current Scheduler/Fiber API merely because current applications use it.

### N7 — Host independence is required; current D1 is not grandfathered

Sluice v1 requires an I/O core that can be hosted without adopting the Sluice task runtime.

This requirement does **not** automatically preserve the current self-driven D1 API or its polling implementation.

Busy polling `ctx.poll()` proves only that progress can be manually driven. It does not prove efficient integration with GUI loops, service loops, reactors, or arbitrary external event loops.

A supported external-host mode must define:

- progress ownership;
- how the host is notified that useful progress can be made;
- whether the host must call poll/reap;
- whether waiting can block and on what primitive;
- shutdown/interruption semantics;
- bounded-resource behavior.

### N8 — Progress notification and public completion are distinct semantic events

The host-neutral seam must distinguish at least:

```text
progress notification
    backend/driver work is available and the progress owner should wake

public completion
    the request's terminal result has been published and may be observed
```

These need not become two public classes, but they may not be conflated in the correctness contract.

A backend may have kernel progress that still requires poll/reap work before a public terminal result exists.

### N9 — Request lifetime and observation lifetime are orthogonal state dimensions

The target model is not one mandatory linear chain containing a waiter.

At minimum, reason separately about:

```text
REQUEST LIFECYCLE
    rejected
    accepted
    execution/cancel race
    terminal outcome chosen
    backend references retired
    public terminal published
    storage/binding reclaimed

OBSERVATION LIFECYCLE
    no observer
    polling observer
    registration attempted
    registered
    registration failed
    canceled
    delivered
    result consumed
```

Some request transitions may legitimately skip physical execution stages, for example cancellation before dispatch. The architecture therefore freezes necessary **partial-order constraints and cross-invariants**, not one exact state enumeration.

Required cross-invariants include:

- observer registration failure does not revoke an already accepted request;
- wait cancellation does not end File/buffer borrow;
- terminal publication may race with observer registration;
- an accepted request may not be orphaned because observer setup failed;
- storage cannot be reclaimed while backend/control/observer references capable of publication or delivery remain live;
- stale request identity must not affect a reused request slot/generation.

### N10 — Bounded resources are part of the product contract

Request capacity, request storage, completed-but-not-yet-reclaimed storage, wait/observer capacity, backend queue/ring resources, worker limits, and any accepted task-host resources must have an owner and a defined exhaustion behavior.

Admission failure is different from operation terminal failure and different from observation/wait failure.

If an API contract promises that an accepted outstanding operation is returned to the caller, resource exhaustion may not silently change that contract by synchronously executing the operation unless that fallback is explicitly part of the invocation contract.

### N11 — Backend mechanism does not own File semantics

ThreadPool, io_uring, and future mechanisms must refine common logical File/operation contracts.

Permitted physical traces may differ. Cancellation strength, batching, registered resources, and platform facilities may differ. Conformance is therefore based on allowed observable outcomes, lifetime/settlement rules, and capability declarations — not byte-for-byte identical internal traces.

### N12 — Canonical File remains execution-neutral where the platform permits

The default architectural direction is:

```text
File
    canonical owned resource
    RAII lifetime
    logical access/identity semantics

Execution-specific binding/registration
    separate explicit relationship/resource when needed
```

Provider/backend affinity must not be smuggled into ordinary `File` semantics merely because one optimized mechanism has such affinity.

Examples of legitimate affinity layers include io_uring registered-file slots, Windows completion-port association, registered buffers, or future platform-specific acceleration.

A future binding may take a shape such as a registration/lease/bound-resource object. The name is not frozen.

Platforms that impose hard affinity may restrict which execution capabilities are compatible with a resource or registration. That restriction must be explicit and must not silently change the resource's logical file semantics.

The stronger claim `any File × any execution capability is always valid` is **not** frozen.

### N13 — C++ RAII is a HARD REQUIREMENT

Zig-inspired explicit I/O must not weaken C++ resource ownership.

Owning resource types remain automatically and deterministically releasing.

For canonical `File`:

```text
explicit close()
    exists so callers can deliberately observe/report close failure

destructor
    noexcept
    deterministically releases owned native resource
    is the cleanup backstop even if explicit close was not called
```

Concrete naming and error type may evolve, but these ownership semantics may not regress.

Additional hard requirements:

1. An owning `File` destructor must not require an externally supplied execution capability merely to avoid leaking the native resource.
2. Provider lifetime must not become a hidden prerequisite for ordinary `File` destruction.
3. An accepted asynchronous operation must not silently outlive the lifetime guarantees of every resource it borrows.
4. If an outstanding request escapes the initiating scope, the API must preserve lifetime safety through explicit borrowing obligations, structured settlement, ownership transfer/pinning, or another named mechanism.
5. wait timeout/cancel does not imply operation termination and does not end File/buffer borrow.
6. explicit close, durability (`sync_*`), operation settlement, and destructor fallback are distinct contracts.
7. provider-affine registration objects, if introduced, must themselves obey explicit RAII/settlement rules appropriate to their affinity; this must not contaminate the ordinary `File` destructor contract.

No future “Zig parity” argument may override these C++ RAII requirements.

### N14 — A general public task runtime carries synchronization obligations

Sluice v1 does not automatically promise a general-purpose task runtime.

If the retained host is only a narrow file-I/O control-flow adapter, its synchronization/product scope may remain narrow.

If Sluice accepts a public runtime in which users can schedule arbitrary cooperating tasks, then task scope, cancellation propagation, settlement, and runtime-aware synchronization become real product obligations. In that case, synchronization integration cannot be declared permanently out of scope merely because the explicit-I/O core does not need a public primitive library.

Existing Event/Semaphore/Mutex/Condition/Queue/select implementations gain no automatic KEEP authority from this rule. Their public/product disposition remains separate.

### N15 — Formal verification protects the selected architecture; it does not select it

Lean, TLA+, executable models, property tests, deterministic seams, sanitizers, stress, and fault injection are evidence mechanisms.

Formal coverage is not architecture ownership.

The next reliability focus is the actual load-bearing protocol rather than additional proof volume for unrelated public primitives.

The central target is the protocol family around:

```text
acceptance / rejection
borrow activation
operation/cancel winner
backend/control-reference retirement
public terminal publication
observer registration/cancel/delivery
storage reclamation
shutdown/admission closure
```

Safety and liveness must be separated. “Exactly once” must be decomposed into at-most-once safety and conditional eventual-progress obligations with explicit environment/fairness assumptions.

---

## 3. Target dependency direction

The target architecture is a dependency direction, not a frozen class diagram.

```text
                         Application
                              |
                              v
                 File + logical I/O semantics
                              |
                              v
                explicit execution capability
                              |
                    invocation/observation
                   /                      \
        completed-return             outstanding request
                   \                      /
                    \                    /
                     request / I/O core
                              |
                       backend mechanism
                         /          \
                  ThreadPool       io_uring

        OPTIONAL / SEPARATE TASK-HOST LAYER
        -----------------------------------
          task scopes / Future / Group / runtime
                       Scheduler / Fiber
                              |
                              v
                    consumes the I/O core
```

The old architecture direction in which File-facing async I/O is conceptually owned by the Sluice Scheduler is not the target.

---

## 4. Explicitly rejected architecture assumptions

The following are no longer allowed as unstated premises:

```text
blocking API + async API == permanent product architecture

"async" == initiation must immediately return

provider argument == arbitrary C++ call stack can transparently suspend

current D1 == proven external-event-loop integration

current D2 == automatically required general task runtime

waiter == mandatory stage of every request

formalized == KEEP

zero consumer == DELETE

implemented == product requirement

one outcome authority == one enum / one object

File must always be bound to exactly one provider

any File must work with every provider

explicit I/O requires abandoning RAII
```

---

## 5. Open decisions — intentionally NOT frozen

The following require accepted workloads, C++ ergonomics, platform evidence, or an implementation experiment before they become architecture authority:

1. concrete type names and representation of the execution capability;
2. dynamic dispatch vs templates vs another mechanism;
3. per-call capability supply vs explicit binding vs a mixed model;
4. exact public spelling of completed-return APIs;
5. exact public spelling/type of outstanding request APIs;
6. whether C++20 coroutines become a supported observation adapter;
7. whether the current stackful Fiber host remains a supported v1 configuration;
8. D2 scope: narrow file-I/O adapter vs public general task runtime;
9. concrete host-neutral progress/completion notification mechanism;
10. concrete provider/resource registration type and platform affinity policy;
11. async metadata surface and whether it is `size`, `file_info`, or another coherent operation;
12. same-file identity API shape;
13. async open/resize/close product priority;
14. multi-worker Scheduler support level for v1;
15. third-party backend/plugin ABI;
16. exact public cancellation disposition vocabulary;
17. deadline policy and which invocation forms may return before physical operation termination.

An implementation PR may not silently decide one of these questions without documenting why the accepted workload or platform constraint now requires it.

---

## 6. v1 reference workloads required before expanding MUST capability

Concrete API capabilities must be justified against accepted reusable workloads rather than by symmetry or by reproducing every syscall used by current examples.

At minimum, future capability decisions should evaluate:

### W1 — Ordinary file utility

```text
open
metadata
read/write
sync when durability is requested
close / RAII cleanup
```

Must remain simple and idiomatic C++.

### W2 — Bounded multi-outstanding file pipeline

```text
N bounded operations in flight
explicit admission failure
request identity when responsibility escapes scope
short I/O / partial effects
best-effort cancellation
safe settlement
bounded storage
shutdown
```

### W3 — External host owns the main/event loop

The application adopts Sluice's I/O core without adopting Sluice's general task runtime.

Acceptance requires a non-busy, documented progress/notification integration story for the supported host class; current polling alone does not satisfy this workload.

### W4 — Optional Sluice task host

If retained as a v1 product capability:

```text
sequential-looking task code
multiple outstanding I/O
cancellation propagation
structured shutdown/settlement
well-defined synchronization scope
```

Capabilities that serve none of the accepted workloads require a separate product justification before becoming v1 MUST.

---

## 7. Reliability evidence policy

Use the least powerful evidence mechanism that adequately addresses the risk.

- table/property tests: validation precedence, short I/O, EOF, partial progress;
- executable/bounded interleaving model: request admission, generation, terminal winner, publication, observer interaction, reclaim;
- TLA+ safety: ownership/settlement/order protocols and lost-notification classes;
- TLA+ liveness only with explicit environment and fairness assumptions;
- C++ memory-order review + deterministic handoff + TSan/litmus where relevant for publication visibility;
- shared backend conformance suites for logical contract refinement;
- deterministic fault injection + real-kernel stress for io_uring cancellation/control CQE/poison/partial-submit behavior;
- lifecycle/fault tests for shutdown/teardown;
- Lean only where a stable parameterized invariant/refinement theorem will remain useful across implementation rewrites.

Do not extend formal work merely because an abstraction exists.

---

## 8. Migration constraints for all code changes

Every architecture-changing PR after this decision must state:

1. which frozen decision(s) it implements or protects;
2. which current mechanism it changes;
3. which semantic/lifetime obligations must remain invariant;
4. which reference workload it serves;
5. which reliability evidence protects the change;
6. whether it resolves an OPEN decision; if so, the new evidence that earns that resolution;
7. how RAII/resource lifetime remains correct on success, failure, cancellation, timeout, and exception paths.

Forbidden shortcuts:

```text
unused => delete
formalized => keep
fewer states => simpler/correct
one class => one authority
same backend => same invocation semantics
wait returned => operation settled
request terminal => all backend/observer references retired
close returned => durability guaranteed
```

---

## 9. Relationship to #387 and #388

### Issue #387

#387 remains authoritative for the code-derived architecture reality at baseline `c64f005e…`, subject to its recorded corrigenda.

When #387's earlier North Star wording conflicts with this document — especially any wording that treats `blocking` and `async` as the permanent top-level product split — **this document wins**.

The intended reasoning order is now:

```text
FINAL DECISION (this document)
        +
CODE REALITY (#387)
        |
        v
current-vs-target gap
        |
        v
workload-backed architecture migration
```

### Issue #388

#388 is retained as a historical formal-coverage/risk/optimization audit. It is not the roadmap authority.

Its useful role is to show where proof effort exists, where the production spine lacks evidence, and which historical optimization hypotheses require re-evaluation under this decision.

No API or mechanism is retained because #388 or an existing model covers it.

---

## 10. External design references

These references informed the architecture direction. They are precedent and design evidence, not normative dependencies.

- Zig 0.16.0 release notes, `Io` as an interface, Threaded/Evented, operation/task concurrency: https://ziglang.org/download/0.16.0/release-notes.html
- Zig 0.16.0 release source: https://ziglang.org/download/0.16.0/zig-0.16.0.tar.xz
- WG21 P2300R10, `std::execution`: https://www9.open-std.org/JTC1/SC22/WG21/docs/papers/2024/p2300r10.html
- WG21 P0443R14, Unified Executors: https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2020/p0443r14.html
- WG21 P1322R2, Networking TS custom I/O executors: https://open-std.org/jtc1/sc22/wg21/docs/papers/2020/p1322r2.html
- WG21 P4003R3, A Minimal Coroutine Execution Model: https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2026/p4003r3.pdf
- Boost.Asio asynchronous-operation model and associated executors: https://www.boost.org/latest/doc/html/boost_asio/reference/asynchronous_operations.html
- Windows I/O completion ports / handle association: https://learn.microsoft.com/en-us/windows/win32/fileio/createiocompletionport
- Linux `io_uring_register(2)` registered resources: https://man7.org/linux/man-pages/man2/io_uring_register.2.html
- C++ Core Guidelines, RAII/resource rules: https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines
- WG21 N3679, async future destructors and lifetime safety: https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2013/n3679.html
- WG21 P3149R6, `async_scope` lifetime/structured settlement: https://www9.open-std.org/JTC1/SC22/WG21/docs/papers/2024/p3149r6.html

---

## 11. Final architecture statement

> **Sluice v1 is an explicit file-I/O C++ library with one File/logical-operation semantic authority, an explicit application-selected or explicitly bound execution capability, distinct invocation/observation contracts, a host-independent bounded request core, replaceable backend mechanisms, and C++ RAII as a non-negotiable ownership rule. Operation concurrency is not task concurrency; request lifetime is not observer lifetime; progress notification is not public completion. Optional runtime facilities may consume the I/O core but do not define it. Formal verification protects these selected contracts rather than selecting the product architecture.**

This statement is the basis for subsequent architecture gap analysis and code repair.