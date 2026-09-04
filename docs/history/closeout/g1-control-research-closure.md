# G1-Control Research Closure — Semantic Authority Verdict

> Historical research closeout. This document records how the G1-Control thesis was tested, what the experiments established, how external prior work changes the interpretation, and the resulting STOP/reopen rule. It is evidence/history, not current architecture authority.

**Date:** 2026-09-04  
**Decision issue:** [#283](https://github.com/jnhu76/Sluice/issues/283)  
**Governing roadmap before rewrite:** [#227](https://github.com/jnhu76/Sluice/issues/227)  
**Evidence campaign:** [#221](https://github.com/jnhu76/Sluice/issues/221)  
**Subordinate control/data-movement roadmap:** [#259](https://github.com/jnhu76/Sluice/issues/259)

## 0. Executive verdict

The strong form of **G1-Control** is **falsified / retired as a mandatory Sluice thesis gate**.

The rejected general claim is:

> **Explicit I/O semantics generally create useful runtime control that leads to specialization and material performance/value.**

The narrower claim that survives is:

> **A specific semantic contract can create optimization authority when it changes the legal transformation space — making a transformation legal that would not be legal under the baseline contract.**

The project therefore replaces the old collapsed concept of `Control` with separate proof obligations:

```text
Semantic Information
        ≠
Semantic Authority
        ≠
Backend Mechanism
        ≠
Unique Incremental Value / Non-redundancy
        ≠
Material Performance
```

Operationally:

```text
STRONG G1-CONTROL:
    RETIRED / FALSIFIED

SEMANTIC AUTHORITY:
    RETAINED AS A NARROW, CANDIDATE-SPECIFIC PROOF OBLIGATION

CURRENT C0:
    CLOSED — NO G1-CONTROL PROMOTION

CURRENT COPY-X0:
    CLOSED — LEGAL TRANSFORMATION BOUNDARY SUPPORTED,
             BUT NO SLUICE-SPECIFIC PREMIUM ESTABLISHED;
             PERFORMANCE ARM BLOCKED

CURRENT BATCH-X0:
    CLOSED — CURRENT BATCH DOES NOT GRANT GROUP ADMISSION;
             S9 IS A SEMANTIC DIVERGENCE WITNESS

NEW GENERIC CONTROL-MECHANISM HUNTING:
    STOP
```

This closure does **not** prove G1-Safety. Correctness, ownership, lifetime, cancellation and composability remain separate hypotheses that must be tested on their own.

---

## 1. The original G1-Control intuition

The original roadmap made `Control` one of the central links in the thesis chain. In practice, the implicit causal model was approximately:

```text
explicit I/O lifecycle / resource semantics
                 ↓
runtime sees more information
                 ↓
runtime gains more control
                 ↓
backend specialization
                 ↓
material performance / product value
```

That intuition is attractive because many successful systems expose explicit semantic structure. But it hides several distinct questions:

1. Did the runtime learn anything new?
2. Is it **allowed** to change an otherwise constrained observable behavior?
3. Does a backend mechanism exist?
4. Is the capability unique to this abstraction, or could a thin baseline/lower layer already do the same thing?
5. Does the capability matter on the target workload?

The C0 → COPY-X0 → BATCH-X0 sequence progressively separated these questions.

---

## 2. Project definition: Semantic Optimization Authority

For future Sluice research, use **Semantic Optimization Authority** as the narrow construct.

Let:

- `C` be an API contract;
- `T` be a proposed transformation;
- `AllowedTraces(C, p)` be the observable traces permitted by `C` for a program `p`.

A project-level legality definition is:

```text
Legal(T, C) :=
    for every program p satisfying C,
    Traces(T(p)) is a subset of AllowedTraces(C, p)
```

A new semantic grant `G` creates new authority for `T` only if:

```text
not Legal(T, C_baseline)
        and
Legal(T, C_baseline + G)
```

This is a **Sluice research definition**, not a claim that the formula itself is a novel theorem. It is a compact operationalization of the standard requirement that an optimization must preserve the observable semantics allowed by its contract.

The decisive question becomes:

> **What exact semantic commitment makes an otherwise-illegal transformation legal?**

If there is no answer, there is no new Semantic Authority claim to benchmark.

### 2.1 Important qualification

Not every useful optimization needs new semantic authority.

If a transformation is already observationally equivalent under the baseline contract — for example, reducing internal submission overhead without changing operation identity, acceptance, failure, completion or ordering guarantees — the implementation is already free to perform it.

Such an optimization can be valuable, but it is evidence about implementation quality or backend mechanisms, **not evidence that high-level semantics created new authority**.

---

## 3. Experiment 1 — C0 fixed-file/resource identity

**Evidence:** [#279](https://github.com/jnhu76/Sluice/issues/279), [PR #280](https://github.com/jnhu76/Sluice/pull/280)

### 3.1 Question

Does explicit long-lived resource identity/lifetime give Sluice a useful, Sluice-specific fixed-file specialization opportunity?

### 3.2 What the campaign established

The campaign established real resource-identity/binding facts and audited a real `io_uring` fixed-file mechanism. It also produced a useful wrong-target identity witness: ordinary fd reuse can target a replacement object, while the fixed-file binding preserves the selected target under the tested binding conditions.

However, the formal campaign verdict was:

```text
C0-PERF:
    FIXED-FILE PERFORMANCE BENEFIT NOT ESTABLISHED

CLASSIFICATION:
    PRODUCT CAPABILITY ONLY — G1-CONTROL NOT PROVEN

STOP-GATE:
    STOP PROMOTION → C1 DO NOT PROMOTE
```

The registered/fixed-file mechanism itself is real: `io_uring_register(2)` documents that registering files or buffers lets the kernel hold long-term references/mappings and reduce per-I/O overhead [R6].

### 3.3 What C0 means

C0 must **not** be summarized as “resource identity is useless.”

The correct decomposition is:

```text
Information:
    real — identity / reuse / lifetime are meaningful

Authority:
    no new Sluice-specific transformation authority established

Mechanism:
    real — fixed/registered resources are first-class io_uring mechanisms

Materiality:
    fixed-file benefit not established in the frozen campaign
```

The research lesson is:

> **A strong backend capability does not prove that a higher-level semantic boundary created that capability or created new optimization authority.**

This is the first break in the original G1-Control chain.

---

## 4. Experiment 2 — COPY-X0 explicit composed Copy

**Evidence:** [PR #281](https://github.com/jnhu76/Sluice/pull/281)

### 4.1 Question

Can an explicit composed `Copy` boundary legally select a bounded data-movement transformation that primitive `read`/`write` operations must not silently receive?

### 4.2 What the campaign established

COPY-X0 is deliberately **not** a simple negative result.

Its frozen/mechanical verdicts included:

```text
COPY-X0-SEMANTIC-EQUIVALENCE:
    EQUIVALENT FOR DECLARED COPY CONTRACT

COPY-X0-TRANSFORMATION-BOUNDARY:
    LEGAL TRANSFORMATION BOUNDARY SUPPORTED

COPY-X0-MINIMALITY:
    LOCAL COPY BRANCH SUFFICIENT;
    GENERIC CAPABILITY FRAMEWORK NOT EARNED

C_boundary_control_property:
    true

C_sluice_specific_premium:
    false

COPY-X0-G1-CONTROL:
    NOT ESTABLISHED

PROMOTION:
    STOP — NO C1
```

The capability/performance arm was **BLOCKED** by the frozen A/A measurement-validity gate, so the campaign did not claim either a performance win or a performance loss.

### 4.3 Why this matters

COPY-X0 supports a narrower architectural idea:

> **A composed operation can be a legal transformation boundary.**

The primitive operations need not silently acquire every transformation. A composed `Copy` contract can own a specific transformation while preserving the declared observable semantics.

But this is **not sufficient** to establish G1-Control as originally phrased.

The same experiment showed that an 81-line thin research boundary could express the transformation pattern, and it explicitly did not establish a Sluice-specific premium over a competent thin standalone wrapper.

This separates three things that the old Control construct had conflated:

```text
legal transformation boundary
        ≠
Sluice-specific unique authority/value
        ≠
material performance
```

COPY-X0 therefore contributes a positive result to **Semantic Authority / legal transformation placement**, while still producing a negative/no-promotion result for **generic G1-Control**.

---

## 5. Experiment 3 — BATCH-X0 explicit Batch

**Evidence:** [PR #282](https://github.com/jnhu76/Sluice/pull/282)

### 5.1 Question

Does the fact that multiple operations belong to one explicit `sluice::async::Batch` grant the runtime a legal right to fuse/amortize group admission while preserving the current per-operation contract?

### 5.2 The decisive S9 witness

BATCH-X0 is the strongest semantic falsifier because **new information is genuinely present**.

The runtime knows that operations belong to one Batch.

However, the current Batch contract does not promise:

```text
all-or-nothing admission
atomic group acceptance
non-interleaving
reserved group capacity
group-level completion visibility
```

The deterministic S9 witness used request capacity 3 and showed:

```text
per-op admission:
    accepted {A1, A2, B}
    rejected {A3, A4}

fused group admission:
    accepted {A1, A2, A3}
    rejected {A4, B}
```

The proposed fused admission changes **which external request crosses acceptance**.

Therefore it changes observable behavior and is not a semantics-preserving optimization of the current Batch contract.

The campaign result was:

```text
BATCH-X0-SEMANTIC-GRANT:
    CURRENT BATCH DOES NOT GRANT GROUP ADMISSION

BATCH-X0-G1-CONTROL:
    NOT ESTABLISHED

PROMOTION:
    STOP — NO C1
```

After Corrective-1, the performance side was separately BLOCKED because the formal performance session had used a substrate inconsistent with the frozen ext4 requirement and the executing host had no permitted replacement substrate. That does not weaken the semantic result: the missing semantic grant is substrate-independent and precedes any materiality question.

### 5.3 What BATCH-X0 proves

```text
Group Information: YES
Mechanism conceivable: YES
Group-admission Authority: NO
```

Hence the key result:

> **Group information does not imply group authority.**

This directly falsifies the old shortcut:

```text
runtime knows more
    therefore
runtime may change more
```

---

## 6. The three experiments form a progression, not three identical failures

The campaign should not be summarized as “three benchmarks failed.”

The evidence progression is more informative:

| Campaign | Information | Authority | Mechanism | Unique/material value | Research lesson |
|---|---|---|---|---|---|
| **C0 fixed-file** | meaningful resource identity/lifetime | no new Sluice-specific transformation authority established | strong existing io_uring mechanism | perf benefit not established | mechanism is not semantic authority |
| **COPY-X0** | explicit composed Copy boundary | legal transformation boundary supported | `copy_file_range` path available | Sluice-specific premium not established; perf BLOCKED | legal boundary is not unique/material value |
| **BATCH-X0** | genuinely new group membership information | **no group-admission authority** | fused admission is implementable in principle | materiality not reached; perf BLOCKED | information is not authority |

The campaigns successively break the original causal chain at different places.

---

## 7. External prior work — strong positive controls

The adversarial literature review is useful because real systems **do** contain strong semantic-control examples. Those examples reveal what is missing from a generic “explicit semantics → control” claim.

### 7.1 LLVM: caller/program obligations create optimizer authority

LLVM `noalias` is not merely “useful metadata.” The Language Reference states a semantic guarantee and specifies undefined behavior when forbidden accesses violate that guarantee. `speculatable` likewise specifies semantic conditions under which a function can be treated as safe for speculation [R1].

The important pattern is:

```text
semantic commitment
    ↓
invalid executions are excluded from the contract
    ↓
optimizer can rely on the commitment
```

This is a clean example of **contract-backed authority**.

### 7.2 System R / SQL: physical execution is intentionally not prescribed

System R's classic optimizer paper states that SQL requests are non-procedural and do not prescribe access paths; System R chooses access paths for simple and complex queries [R2].

The source of optimization freedom is not merely “the database knows more.” The interface intentionally leaves many physical execution choices outside the user's procedural specification.

The relevant lesson for Sluice is:

> **Declarativity creates authority by defining an equivalence class of legal physical executions, not by adding a richer wrapper around an imperative sequence.**

### 7.3 Halide: algorithm and schedule are distinct contracts

Halide's own overview shows the algorithm with “no storage or order” and a separate schedule that defines order/locality and implies storage; its PLDI work formalized this design direction [R3].

This is much stronger than simply grouping imperative operations:

```text
algorithm result contract
        +
separate scheduling freedom
```

The implementation is explicitly given degrees of freedom that ordinary per-operation I/O APIs often keep observable.

### 7.4 Idempotent APIs: semantic commitment authorizes retry

AWS's Builders' Library describes idempotent APIs as contracts that let a caller retry requests without additional side effects and uses client request identifiers to preserve semantic equivalence [R4].

Without that contract, automatically duplicating a mutating request may create duplicate side effects.

With it:

```text
idempotent logical request
        ↓
retry/replay can be legal
        ↓
dedup/retry machinery can exploit the grant
```

This is a direct runtime analogue of Semantic Authority.

### 7.5 io_uring linked and multishot operations

`io_uring` itself distinguishes ordinary mechanism from explicit semantic contracts.

Linked requests establish request dependencies/sequential relationships [R7]. Multishot requests explicitly change completion shape: one SQE may generate multiple CQEs [R8].

These features do not rely on inferring strong semantics from generic grouping. The application opts into a specific contract with a specific observable consequence.

---

## 8. External prior work — negative/control cases

### 8.1 POSIX `posix_fadvise`: information without semantic authority

POSIX states that `posix_fadvise()` advises the implementation about expected access behavior and that the implementation may use the information to optimize handling, but it **shall have no effect on the semantics of other operations** on the data [R5].

This is the archetypal separation:

```text
Information: YES
Permission to violate existing semantics: NO
```

A hint can still improve heuristics. But it does not make an otherwise-illegal observable transformation legal.

### 8.2 io_uring registered resources: strong mechanism without a high-level thesis

`io_uring_register(2)` explicitly supports registered files/buffers and long-term references/mappings to reduce per-I/O overhead [R6].

That mechanism is real and valuable in the right regime. But the existence of the low-level registration API does not imply:

```text
some higher-level lifetime abstraction
    therefore
new high-level semantic authority exists
```

This is precisely the distinction exposed by C0.

---

## 9. Revised Control taxonomy

For future discussions, use the following taxonomy to avoid calling every optimization “Control.”

| Level | Dimension | Typical transformation | Authority question |
|---|---|---|---|
| **C0 Transport** | submission/doorbell/CQ overhead | batch internal submissions | Usually no new semantic grant if per-op behavior is unchanged |
| **C1 Resource** | identity/reuse/pinning | registered files/buffers, caches | Does the contract grant lifetime/ownership rights beyond existing resource APIs? |
| **C2 Scheduling** | order/placement/priority | reorder, delay, affinity | Which order/timing dimensions are explicitly flexible? |
| **C3 Admission** | capacity/acceptance | group/atomic admission | Does the contract own all-or-nothing/non-interleaving acceptance? |
| **C4 Fusion** | intermediate execution states | merge/drop/fuse operations | Are intermediate states non-observable or operations provably transformable? |
| **C5 Failure/Retry** | duplicate execution/failure recovery | retry, hedge, dedup | Is idempotence/transactional behavior explicitly promised? |
| **C6 Completion/Publication** | completion visibility | coalesce/delay/multishot | What completion shape/timing is contractually allowed? |
| **C7 Data Movement** | ownership/alias/lifetime | zero-copy, DMA, transfer ownership | What ownership/mutability/lifetime promise makes the path safe/legal? |

The purpose of this table is not to authorize new features. It forces every candidate to identify the exact observable dimension and grant.

---

## 10. Final adjudication

| Thesis | Verdict | Basis |
|---|---|---|
| **A. Explicit I/O semantics generally create valuable runtime control opportunities.** | **FALSE / RETIRED** | C0/COPY/BATCH + positive/negative external controls do not support the general implication |
| **B. Explicit semantic contracts can create valuable control when they constrain/relax observable behavior.** | **SUPPORTED** | LLVM, SQL/System R, Halide, idempotent APIs, linked/multishot operations |
| **C. More runtime-visible information is itself enough to justify specialization.** | **FALSE** | BATCH-X0 S9 is a direct counterexample; POSIX hints are an external control |
| **D. A composed semantic boundary can be a legal transformation boundary.** | **SUPPORTED, narrowly** | COPY-X0 |
| **E. Backend mechanism availability proves semantic-control value.** | **FALSE** | C0 and registered-resource evidence separate mechanism from high-level grant |
| **F. Specialization needs separate grant + mechanism + non-redundancy + materiality proof.** | **SUPPORTED as the replacement research discipline** | combined campaign + literature boundary |
| **G. Control failure proves Safety/correctness value.** | **FALSE / NOT TESTED** | logical non sequitur; Safety remains independently falsifiable |

The wording difference between “DROP” and “SPLIT + DEMOTE” is resolved as follows:

- **DROP/RETIRE the strong named G1-Control implication** as a mandatory core thesis.
- **RETAIN Semantic Authority** as a narrower construct that may be proven for an individual contract/transformation pair.

---

## 11. STOP rule

For the currently existing resource/lifecycle/Copy/Batch constructs:

```text
STOP NEW GENERIC CONTROL-CANDIDATE HUNTING
```

Do not proceed by searching for:

```text
another batching trick
another registered-resource trick
another zero-copy primitive
another scheduler mechanism
```

until a new semantic grant is written first.

Otherwise the activity ceases to be falsifiable thesis testing and becomes mechanism search until a benchmark happens to turn positive.

---

## 12. Reopen protocol

A future Control candidate may reopen only when all of the following are preregistered.

### A. Contract

There is one explicit new contract clause `G`.

### B. Observable dimension

The proposal states exactly which user-observable dimension is constrained, relaxed, made non-observable, or otherwise changed by `G`.

Examples include:

```text
ordering
acceptance identity
retry/duplicate execution
intermediate-state visibility
completion shape/timing
ownership/lifetime
```

### C. Otherwise-illegal transformation

There is a concrete transformation `T` that is **not legal** under the baseline contract.

This is the load-bearing gate.

### D. Legality under the new contract

`T` is legal for every program satisfying the new contract, not merely for the benchmark fixture.

### E. Non-redundancy

The lower layer cannot obtain the same authority solely from ordinary op parameters, existing low-level contracts, or inference.

### F. Reachable mechanism

A real backend/system mechanism can exploit the authority.

### G. Materiality preregistration

The performance/value threshold is fixed before measurement.

### H. Falsifier

There is an explicit result that will STOP the candidate.

The first question in every reopening remains:

> **What exact semantic commitment makes an otherwise-illegal transformation legal?**

If that question cannot be answered, stop before implementing a benchmark or production abstraction.

---

## 13. Consequence for Sluice direction — intentionally not executed here

This closeout records evidence for a later #227 rewrite; it does not perform that rewrite.

The old shortcut:

```text
explicit lifecycle/resource boundary
        ↓
runtime control opportunity
        ↓
specialization
```

must not survive unchanged.

The evidence supports a split:

```text
Sluice semantic/lifecycle model
        │
        ├── correctness / ownership / lifetime / composition
        │       separate hypothesis and evidence track
        │
        └── optional specialization track
                │
                ├── explicit Semantic Authority
                ├── reachable mechanism
                ├── non-redundancy
                └── materiality
```

The Control closure makes correctness/ownership/lifetime a cleaner next research direction, but does **not** establish that direction as positive. A future G1-Safety campaign must still demonstrate which concrete hazard classes become unrepresentable, statically rejected, fail-fast, deterministically reproducible, or otherwise materially improved — while counting Sluice-induced hazards as well.

---

## 14. Research contribution of the negative result

The result is not simply “no optimization found.”

The campaign produced three reusable research outcomes:

1. **A corrected construct.** `Control` is decomposed into Information, Authority, Mechanism, Non-redundancy and Materiality.
2. **A semantic falsifier.** BATCH-X0 demonstrates that genuinely new information can exist without the authority required for a proposed transformation.
3. **A STOP/reopen discipline.** New performance mechanisms are no longer generated from semantic-looking abstraction boundaries by default; they require an explicit legal-transformation argument first.

That is a valid research closure even though the original strong thesis did not survive.

---

## References

### R1 — LLVM semantic/optimization attributes

LLVM Project. **LLVM Language Reference Manual**. `noalias`, pointer/attribute semantics, `speculatable`.  
https://llvm.org/docs/LangRef.html

### R2 — System R / declarative query optimization

P. Griffiths Selinger, M. M. Astrahan, D. D. Chamberlin, R. A. Lorie, T. G. Price. **Access Path Selection in a Relational Database Management System**. ACM SIGMOD, 1979.  
IBM Research: https://research.ibm.com/publications/access-path-selection-in-a-relational-database-management-system  
DOI: https://doi.org/10.1145/582095.582099

### R3 — Halide algorithm/schedule separation

Jonathan Ragan-Kelley et al. **Halide: A Language and Compiler for Optimizing Parallelism, Locality, and Recomputation in Image Processing Pipelines**. PLDI 2013.  
Halide overview/publications: https://halide-lang.org/

### R4 — Idempotent API contracts

Malcolm Featonby. **Making retries safe with idempotent APIs**. Amazon Builders' Library.  
https://aws.amazon.com/builders-library/making-retries-safe-with-idempotent-APIs/

### R5 — POSIX advice as information without semantic change

POSIX / Linux man-pages. **posix_fadvise(3p)**.  
https://man7.org/linux/man-pages/man3/posix_fadvise.3p.html

### R6 — io_uring registered-resource mechanism

Linux man-pages. **io_uring_register(2)**.  
https://man7.org/linux/man-pages/man2/io_uring_register.2.html

### R7 — io_uring linked-request contract

liburing/Linux man-pages. **io_uring_linked_requests(7)**.  
https://man7.org/linux/man-pages/man7/io_uring_linked_requests.7.html

### R8 — io_uring multishot completion contract

liburing/Linux man-pages. **io_uring_multishot(7)**.  
https://man7.org/linux/man-pages/man7/io_uring_multishot.7.html

---

## Repository evidence index

- [#227 — thesis-driven execution roadmap](https://github.com/jnhu76/Sluice/issues/227)
- [#221 — Explicit-I/O Value Evaluation](https://github.com/jnhu76/Sluice/issues/221)
- [#259 — Zero-Cost Control Plane + Explicit Data-Movement Boundary](https://github.com/jnhu76/Sluice/issues/259)
- [#279 — G1-Control fixed-file resource identity and specialization falsification](https://github.com/jnhu76/Sluice/issues/279)
- [PR #280 — C0 fixed-file/resource-identity campaign](https://github.com/jnhu76/Sluice/pull/280)
- [PR #281 — COPY-X0](https://github.com/jnhu76/Sluice/pull/281)
- [PR #282 — BATCH-X0](https://github.com/jnhu76/Sluice/pull/282)
- [#283 — G1-Control verdict decision record](https://github.com/jnhu76/Sluice/issues/283)
