# ADR-0002 Visual Companion — Explicit File Architecture Views

- **Status**: Visual companion to [`0002-explicit-file-api-architecture.md`](0002-explicit-file-api-architecture.md)
- **Authority**: No new decision. If this file ever conflicts with ADR-0002 text, ADR-0002 wins.
- **Purpose**: Keep one complete architecture map and add two reduced views that make the same normative structure easier to read on GitHub.

The three diagrams are intentionally redundant:

1. **All-in-one** — complete relationship map;
2. **Semantic spine** — the main File → Operation → API → Execution chain;
3. **Boundary discipline** — what may inform implementation but must not silently become semantic authority.

---

## View A — All-in-one architecture

```mermaid
flowchart LR
    APP["Application"]

    subgraph CONTRACT["Explicit File Contract"]
        direction TB
        FILE["File Resource<br/>identity · ownership · lifetime"]
        OPEN["Resource Lifecycle<br/>open · close"]
        STATE["Observable File State<br/>size · minimal metadata · resize"]
        OP["Canonical File Operations<br/>read · write · positional<br/>sync_data · sync_all"]
        COMPOSE["Composed Operations<br/>exact · all · copy"]

        FILE --> OPEN
        FILE --> STATE
        FILE --> OP
        OP --> COMPOSE
    end

    subgraph API["API Levels"]
        direction TB
        COMMON["Common Logical API<br/>operation → Result"]
        LOW["Explicit Operation API<br/>Operation + Completion<br/>multiple outstanding · cancellation"]
    end

    subgraph EXEC["Replaceable Execution"]
        direction TB
        BLOCK["Blocking<br/>direct syscall"]
        POOL["ThreadPool<br/>blocking syscall offload"]
        URING["io_uring<br/>native async execution"]
    end

    subgraph CAPS["Capabilities / Constraints / Hints"]
        direction TB
        DIRECT["Direct I/O<br/>capability + validity constraints"]
        SPACE["Space Reservation<br/>resource guarantee"]
        ADVICE["Access Advice<br/>hint only"]
        COPYCAP["Transfer Mechanisms<br/>copy_file_range · splice · sendfile"]
    end

    subgraph POLICY["Backend-local Policy"]
        direction TB
        QD["queue depth"]
        WORKERS["worker count"]
        POLLING["polling / SQPOLL"]
        REGISTERED["registered files / buffers"]
    end

    APP --> FILE

    OP --> COMMON
    OP --> LOW

    COMMON --> BLOCK
    COMMON --> POOL
    COMMON --> URING

    LOW --> POOL
    LOW --> URING

    FILE -. "capability" .-> DIRECT
    FILE -. "resource guarantee" .-> SPACE
    OP -. "hint" .-> ADVICE
    COMPOSE -. "legal lowering" .-> COPYCAP

    POOL -. "local policy" .-> QD
    POOL -. "local policy" .-> WORKERS
    URING -. "local policy" .-> POLLING
    URING -. "backend capability" .-> REGISTERED
```

Read this diagram as five responsibility bands:

> **File contract → API level → execution**, with capability/constraint/hint information beside the contract and backend-local policy beside execution.

The side bands may influence lowering and validation, but they do not redefine the canonical File semantics.

---

## View B — Semantic spine

This is the diagram to use when explaining what Sluice I/O *is*.

```mermaid
flowchart LR
    APP["Application"]
    FILE["File Resource<br/>identity · ownership · lifetime"]
    OPS["Canonical Semantics<br/>open/close · size/resize<br/>read/write · positional · durability"]

    COMMON["Common Logical API<br/>logical wait → Result"]
    EXPLICIT["Explicit Operation API<br/>outstanding request · Completion"]

    BLOCK["Blocking<br/>cheap direct path"]
    POOL["ThreadPool<br/>blocking offload"]
    URING["io_uring<br/>native async"]

    APP --> FILE --> OPS

    OPS --> COMMON
    OPS --> EXPLICIT

    COMMON --> BLOCK
    COMMON --> POOL
    COMMON --> URING

    EXPLICIT --> POOL
    EXPLICIT --> URING
```

Normative reading:

```text
File is the resource root.
Operations own semantics.
API level owns initiation/lifetime visibility.
Execution owns mechanism.
```

Therefore:

```text
sync semantics != a separate I/O world
async semantics != raw-fd semantics
build target != semantic boundary
```

Blocking remains first-class and may take the shortest valid syscall path. The explicit-operation layer exists only when the caller truly needs outstanding-request authority, cancellation, request identity, completion ownership, or bounded admission.

---

## View C — Boundary discipline

This is the diagram to use when reviewing whether a new field, optimization, backend feature, or abstraction belongs in the public API.

```mermaid
flowchart LR
    subgraph CORE["What may legitimately define the contract"]
        SEM["SEMANTIC CONTRACT<br/>observable File / I/O behavior"]
        CORR["CORRECTNESS AUTHORITY<br/>lifetime · publication · cancellation"]
        BOUND["RESOURCE BOUND<br/>real bounded resource"]
        TRANS["COMPOSED TRANSFORMATION<br/>explicitly granted lowering freedom"]
    end

    subgraph NONCORE["What does not automatically become semantics"]
        CAP["BACKEND CAPABILITY<br/>registered resources · opcode support"]
        POLICY["EXECUTION POLICY<br/>workers · queue depth · polling"]
        HINT["HINT / OBSERVATION<br/>access advice · measurements"]
        INFO["INFORMATION ONLY<br/>grouping · identity · metadata without grant"]
    end

    CORR --> SEM
    BOUND --> SEM
    TRANS --> SEM

    CAP -. "does not define" .-> SEM
    POLICY -. "does not define" .-> SEM
    HINT -. "does not grant authority" .-> SEM
    INFO -. "does not grant authority" .-> SEM
```

The governing inequalities remain:

```text
resource identity != fixed-resource optimization authority
operation grouping != fused / atomic admission authority
backend capability != semantic authority
hint / information != authority
benchmark result != public semantic contract
```

Examples:

- `size` / `resize`: semantic File state;
- request capacity: named resource bound when saturation is caller-visible;
- Direct I/O alignment: capability + validity constraint, not a generic performance hint;
- `fadvise`: hint only;
- registered files / buffers and SQPOLL: backend capability / execution policy;
- `copy_file_range` / `splice` / `sendfile`: possible lowering mechanisms beneath an explicit Copy transformation boundary, not automatically public primitive operations.

---

## Review rule

When a proposed Sluice concept cannot be placed cleanly on one of these three views, do not create a generic framework to make it fit.

Instead ask:

1. What observable behavior does it define?
2. What correctness authority does it own?
3. What real bounded resource does it name?
4. What transformation does it explicitly authorize?
5. Or is it merely capability, policy, hint, or information?

If none of the first four answers is supported, the concept has not earned Core/public semantic status.
