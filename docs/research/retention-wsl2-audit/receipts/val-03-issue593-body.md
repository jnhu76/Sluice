# PocketJS-inspired Execution-Path Slicing — Sluice research pilot

> **Purpose:** borrow the **method** from [pocket-nexus/benchmark](https://github.com/pocket-nexus/benchmark), not its JS runtime, UI stack, simulator, or QEMU environment. Build **one finite, correctness-admitted pilot** that can split a real Sluice I/O execution into verifiable observations and controlled replay experiments, then determine whether the approach discovers or explains performance gaps better than profiling alone.
>
> **Status:** PROPOSED / OWNER-GATED. This issue authorizes research design and isolated experimental evidence only. It **does not** authorize a production refactor, new public API, persistent instrumentation layer, or mass deployment of a new replay framework.

## 1. Research question

Can we take **one frozen logical workload**, observe Sluice's full request lifecycle, replay carefully bounded subpaths under deterministic inputs, and use the resulting cross-view evidence to locate otherwise hard-to-attribute work **without mistaking synthetic or replay-only savings for end-to-end speedup**?

Sluice's #576 and #580 illustrate why this matters: a confirmed scan/lookup cost or an experimental ~30× reduction in probes did not, by itself, establish meaningful wall-time improvement. We need an independent way to ask *where* the cost survives, disappears, overlaps or migrates.

**Primary deliverable:** a **validated experimental method** and **one case study**, not a promised speedup or a universal framework.

## 2. Ownership and relationship to existing issues

| Owner | Responsibility |
|---|---|
| [#591 — Hardware-Aware Workload Benchmark](https://github.com/jnhu76/Sluice/issues/591) | Workload generation, legal backend/model comparisons, reference controls, measurement schema, statistical method, machine identity; avoid duplicating it |
| **This issue — Path Slicing / Replay Method** | How to partition one complex Sluice path into full / observe / neutral-input replay / controlled completion replay and validate cross-view correspondence |
| [#563 — Minimal Necessary Work × Hardware-Aware Design](https://github.com/jnhu76/Sluice/issues/563) | Optimization decision principles, state/authority reduction and hardware-aware mechanism admission |
| Existing [#577](https://github.com/jnhu76/Sluice/issues/577), [#579](https://github.com/jnhu76/Sluice/issues/579), [#580](https://github.com/jnhu76/Sluice/issues/580), [#581](https://github.com/jnhu76/Sluice/issues/581), [#582](https://github.com/jnhu76/Sluice/issues/582) | Existing independent performance research/gates; this issue may provide evidence **without** changing those gates or silently reopening negative findings |
| [#578](https://github.com/jnhu76/Sluice/issues/578) / dududu | Downstream application workloads, **not prerequisites** for this pilot |

The [adopted v2 contract](https://github.com/jnhu76/Sluice/blob/master/docs/explicit-io-contract.md), [AGENTS.md](https://github.com/jnhu76/Sluice/blob/master/AGENTS.md) and the [standing performance methodology](https://github.com/jnhu76/Sluice/blob/master/docs/performance/methodology.md) remain authoritative. Do not create another normative protocol or statistics document.

**Strict division:** #591 answers "How do we fairly measure these workloads?"; **this issue** answers "Can we isolate/replay one part of a complex *actual* path while preserving evidence correspondence?"; subsequent Gap Discovery answers "What new performance opportunities does the evidence support?" No product work is implied.

## 3. PocketJS ideas to transfer — and the limits

Grounding: [PLAN.md](https://github.com/pocket-nexus/benchmark/blob/main/docs/PLAN.md), [PROTOCOL.md](https://github.com/pocket-nexus/benchmark/blob/main/docs/PROTOCOL.md), [SHELL.md](https://github.com/pocket-nexus/benchmark/blob/main/docs/SHELL.md), [FINDINGS.md](https://github.com/pocket-nexus/benchmark/blob/main/docs/FINDINGS.md).

| PocketJS mechanism | Sluice translation | Important limitation |
|---|---|---|
| One canonical workload and neutral MutationTape | Frozen file-I/O **logical-operation trace** with typed inputs and independently validated data results | Trace identity is **not** equivalent to same interleaving or arrival timing |
| Full / guest-tape / native / raster views | **Full** Sluice execution; **Observe/Capture**; optional **Request Replay**; **test-only Completion/Progress Replay** when an exact seam exists | Never subtract dissimilar run times and call the difference removable overhead |
| Measure binary vs Observe binary | Separate low-overhead timing binary from detailed record/counter instrumented binary; share scenario ID and independently verify outputs | Instrumented CPU/wall are diagnostic, never an adoption A/B |
| Result hashes, RET and fail-closed tape validation | Exact offset/size/bytes, returned result/error, request identity/generation, terminal outcome, data digest and event-order constraints | Some kernel outcomes and cancellation races are nondeterministic; preserve *legal partial order*, not one invented total order |
| Neutral corpus with scale points, soak | Small/medium/high request count and cardinality, lifecycle-churn, short and long range as suitable | Supported real workload and diagnostic stress must be labeled separately |
| Per-stage observations and Findings ledger | Source-grounded execution DAG and typed boundary events; one reproducible negative/positive finding ledger | Stage percentiles do **not** add; CPU cycles are not request wall time |
| QEMU icount / fixed reference machines | Use native E5 separate perf/PMU/trace where supported; optional deterministic event tests | QEMU instructions ≠ native kernel timing; **no** SO3/QEMU port in this issue |

## 4. Choose exactly one first path

**Preferred pilot:** io_uring W-02 positional read, tracing this source-backed DAG on a frozen master SHA:

`AsyncIoContext::submit_read → UringAsyncBackend::submit_request → RequestCore reserve/accept → dispatch_after_accept / SQE → kernel submit/CQE → router lookup → terminal / publication → progress → observer/take → retirement`.

Relevant source anchors: `src/async/async_io_context.cpp`, `src/async/uring_backend.cpp`, `src/async/uring_transport.cpp`, RequestCore implementation and existing W-02 benchmark. Resolve exact files/lines at execution SHA; do not assume issue-era line numbers or hidden test hooks are production contracts.

Choose one **normal-read** scenario and one **adverse** lifecycle variant (e.g. cancel or zero-length). Use #580 as a **motivating example**, not permission to rerun or overturn its independent verdict. If replay requires new production authority or a prohibitive test seam, stop with a **feasibility decision** instead of implementing infrastructure.

Only extend to ThreadPool or W-03 after this pilot passes the gates *and* an owner explicitly approves the next study.

## 5. Modes and evidence correspondence

### F — FULL (performance authority)

- Run the real installed/repository Sluice path, with real Linux I/O completion/worker progression and v2 obligations.
- Frozen workload, fixture bytes/hash, compile/link SHA, machine/OS/cache state, legal C/D/W and actual outstanding depth.
- Retain **uninstrumented** wall/throughput/user+system CPU/latency with correctness checks outside the timed hot loop; use #591 or existing frozen W-02/W-03 evidence machinery.
- **Only this path**, after same-work semantic parity and matched A/A/paired A/B, can support production end-to-end speedup claims.

### O — OBSERVE / CAPTURE (causal mapping, NOT performance authority)

- In an isolated research build, record request-admitted/dispatch/SQE-submit/CQE-observed/terminal-selected/publication/observer-take/retirement boundaries *as they actually occur*.
- Include thread ID, monotonic timestamps where trustworthy, event kind, request identity + generation/cookie (without inventing a narrower identity), sequence or causal link, and bytes/results. Separate **logical** requests, **physical** CQE/control events and **observer** events.
- Capture syscalls, scheduler wake/claim and per-stage counters only where available. `notify_one` is **not** proof that a worker actually ran; a CQE timestamp is not necessarily the device completion instant.
- Preserve generated trace hashes, source/build identity and analyzer version; record instrumentation overhead and missing events as **UNKNOWN**, not zero.

### R — REQUEST REPLAY (input / workload reproduction)

- Feed the **same logical positional operations** to supported execution models; verify offsets, byte totals, correct data digest and outcomes.
- Explicitly record whether arrival order/dependencies, parallelism, short I/O, cancellation and errors are reproduced; if not, mark **WORKLOAD_ONLY_REPLAY**.
- Replay of a known read list **cannot** claim identical real application scheduling, kernel service time or concurrency.

### C — CONTROLLED COMPLETION/PROGRESS REPLAY (test-only, optional)

- Only if feasible, introduce the **narrowest test-only** seam to deliver a legally ordered frozen terminal/CQE/control event sequence into the matching internal state transitions. No simulated kernel path should masquerade as production io_uring.
- Keep RequestCore authoritative; preserve borrowed buffer lifetime, accept/claim/offer/terminal/publication/take exactly once, generation/ABA, cancel/reuse, W-03 no-lost-wake and shutdown obligations as applicable.
- No synthetic completion may be treated as proof that a real submitted kernel operation has completed; define which production obligations are **intentionally excluded**, and therefore what this replay **cannot measure**.
- If a faithful seam would duplicate the backend state machine, bypass authority, or demand a product refactor, conclude **REPLAY_NOT_ADMITTED**. A validated source-DAG + observe-only pilot is still valuable.

## 6. Workstream and gates (finite, negative results welcome)

### R0 — Source mapping / seam feasibility (mandatory, read-only)

- [ ] Freeze `SLUICE_SHA` and adopted contract requirements; map one complete normal async read and one selected adverse overlay.
- [ ] Enumerate stages, owns/writes/reads, queue/capacity and physical completion/control representations, public and test-only boundaries; identify exact test seams and replay hazards.
- [ ] Audit #591 tooling/G0 to avoid a second schema/runner/parser; reuse existing correctness and trace artifacts where possible.
- [ ] Define what cannot be replayed (kernel timing, actual cross-thread scheduling, waits, partial completions, poison recovery, etc.) before writing C++.
- **Gate R0:** explicit `FULL/OBSERVE/REQUEST_REPLAY/COMPLETION_REPLAY` feasibility matrix; fail early if no justified seam.

### R1 — Neutral logical corpus + cross-view oracle

- [ ] Build the *smallest* versioned trace/corpus needed for two cases, derived from accepted public operations, not implementation representation.
- [ ] Freeze fixture and input digests, logical request identity, offsets, read/write lengths, terminal values/errors, dependencies, and valid reorder constraints.
- [ ] Build a fail-closed reader/verifier: mismatch on missing/duplicate/spurious terminal, wrong-generation request, altered bytes/offset, wrong result or illegal event order must be RED (named negative controls).
- [ ] Prove a trace can be independently regenerated / replayed and is self-consistent under the supported scope. Avoid timestamps as correctness authority.
- **Gate R1:** a known-invalid trace is rejected, genuine full-path oracle passes, and any skipped semantic dimension is explicitly labeled.

### R2 — Observe/measure isolation

- [ ] Establish the smallest stage-boundary event representation; never instrument every operation unless necessary.
- [ ] Separate measure vs observe executable/build profile (or prove existing profiling lane offers equivalent isolation). Report sampler/instrumentation cost.
- [ ] Verify *same workload identity* across Full/Observe/Replay runs, while preserving the distinction in physical work performed.
- [ ] Show per-stage frequencies and source-level attribution with explicit concurrency/overlap and off-CPU caveats; p99(stage A)+p99(stage B) must **not** be treated as p99(total).
- **Gate R2:** one independently reviewed stage/authority map supported by real event evidence, plus uninstrumented native reference result.

### R3 — Optional controlled completion replay and counterfactual

- [ ] Only execute if R0–R2 earned the seam and owner authorizes it.
- [ ] For frozen events, demonstrate correspondence of terminal/publication/observer outcomes where equivalence is **required**, with exact scope and legal nondeterminism.
- [ ] Include at least one deliberately falsifying mutation (stale cookie, duplicated CQE/terminal, missing publication, wrong RET, swapped illegal control order) yielding RED, restored GREEN.
- [ ] Compare: mechanism-only probe vs controlled replay vs Full. State where differences arise and why they are not naively subtractable.
- [ ] Test a counterfactual designed to **distinguish two competing root causes**, not merely amplify router cost.
- **Gate R3:** causal evidence OR a transparent `REPLAY_NOT_ADMITTED` / `INCONCLUSIVE` result. No automatic production optimization.

### R4 — Review and research closeout

- [ ] Two independent reviews: **(A)** v2 semantic/cancellation/lifetime/temporal correspondence and **(B)** measurement/overhead/causality/statistical claims.
- [ ] A compact **Path Slicing Findings** record: exact SHA, workload, oracle, mode scopes, replay limitations, observed stage cost, false hypotheses, residual gap and follow-up decision.
- [ ] Answer whether the methodology exposed anything *not already obtainable from perf/flamegraph alone*, and whether its standing maintenance cost is justified.
- [ ] If useful, upstream only a **small, proven reusable slice** into #591 under its own review; otherwise archive evidence and keep one-off code in an isolated research branch/workspace.
- **Closeout vocabulary:** `METHOD_EARNED` | `PARTIAL_OBSERVATION_ONLY` | `REPLAY_NOT_ADMITTED` | `NO_EXTRA_DIAGNOSTIC_VALUE` | `BLOCKED_CORRECTNESS`.

## 7. Required acceptance evidence

- [ ] One **actual** read path (not an invented simulation) traced end-to-end at a frozen SHA.
- [ ] At least one controlled adverse path considered with explicit completion/borrow/cancel obligations.
- [ ] Distinct modes; never claim stage replay is same as production or compare incomparable runtimes as speedup.
- [ ] Logical trace and outcome digests, complete identity/provenance, exact failure behavior.
- [ ] Tests reject corrupted, stale/duplicated and identity-mismatched events; deterministic oracles not `sleep`-based races.
- [ ] Native Fedora E5 measurement carries exact **reprobed** CPU model/topology, OS, compiler, storage/cache type, build IDs and perf availability; WSL2 use disclosed as exploratory.
- [ ] Experimental stage counts and parent/child transitions mapped to real code; kernel wait time, CPU time and end-to-end wall remain separately identified.
- [ ] Negative/no-value verdict allowed without implementing the C replay mode.
- [ ] No production/runtime/public API changes, no G0 semantic drift, no standalone generic simulator, no new permanent CI jobs or mandatory QEMU dependency.
- [ ] A short reviewable report that an owner can use to determine **whether to repeat this technique elsewhere**, not merely a gallery of benchmark numbers.

## 8. Method-design takeaways from PocketJS (explicitly not copied)

**Transfer:** stable canonical cases, neutral input corpus, replay boundaries, measure/observe binary split, independent outcome hashes, negative controls, replay/reference identity, stage-local reporting and a findings ledger.

**Do not transfer blindly:** simulated hardware icount as native I/O evidence, lockstep virtual-time execution of real Linux async I/O, deterministic total scheduling, JIT/DOM-specific modes, and broad cross-platform build complexity.

**Execution plan:** **R0 feasibility first → R1/R2 narrow pilot → owner gate for R3 → R4 independent closeout.** Do not start another downstream study merely because this issue exists.

