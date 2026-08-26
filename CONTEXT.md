# Sluice Context

> **Orientation only — not architecture authority.**
>
> This file is a maintained map of the repository's current context for humans and coding agents.
> It does **not** redefine architecture, public API, accepted ADRs, verification policy, or an active
> approved task. If this file conflicts with a higher-authority source, the higher-authority source wins.
>
> Keep this file short. Prefer issue numbers, file paths, and named authorities over copied prose.

---

## 1. What Sluice is

Sluice is an experimental C++20 I/O control-flow library exploring a:

> **bounded, explicit, inspectable I/O execution layer**

The current architectural north star is:

> **Explicit control, implicit correctness.**

Applications should explicitly control decisions that materially shape I/O, while Sluice owns the
correctness machinery needed to make those decisions safe: request identity/lifecycle, publication,
cancellation races, wait/wake correctness, bounded resource accounting, and backend execution seams.

---

## 2. Stable checkpoint

The pre-six-domain-refactor reference baseline is:

```text
v0.0.1
a38df5e9a7bee3603a439857f036de2b5a136bf2
```

Use it as the comparison point for later behavior, API/product surface, tests/formal evidence,
performance, and backend/resource behavior.

`v0.0.1` does **not** mean:

- the public API is mature/stable;
- the six-domain architecture work is complete;
- resource/observability modeling is complete;
- Wait/Transfer/Capability refactors are complete;
- execution-shape autotuning is approved.

Current development may be ahead of this tag. Do not pin a moving `master` SHA in this file.

---

## 3. Roadmap authorities

Four issues define the current direction:

| Issue | Role | Question |
|---|---|---|
| #221 | Evidence / product value | What does explicit I/O buy, what does it cost, and where does it fail? |
| #225 | Architecture north star | What architecture is Sluice deliberately building toward? |
| #226 | Adversarial architecture audit | Where does the current implementation duplicate, miss, or violate semantic authority? |
| #227 | Execution roadmap | What do we do next, in what order, and where do we stop? |

Do not collapse these roles.

- A benchmark result in #221 does not by itself authorize a Core change.
- A target shape in #225 does not prove current code needs immediate refactoring.
- A finding in #226 is evidence, not automatic implementation permission.
- #227 controls sequencing.

---

## 4. Six architectural domains

All significant architecture work should map to one primary domain.

### R1 — Request

Owns request lifecycle and identity:

- acceptance;
- `RequestKey` / generation;
- slot lifecycle;
- Completion binding/publication;
- borrow lifetime;
- terminal winner;
- reap/reset boundary;
- backend-independent request correctness.

Current strong authorities include:

- `RequestArena`;
- `detail::submit_transaction`.

Backend-specific admission, health, queueing, and execution ownership remain backend-local unless a
later audit proves a deeper shared authority.

### R2 — Wait

Owns waiting correctness:

- registration;
- admission/recheck;
- deadlines;
- cancellation;
- winner selection;
- timer retirement;
- wake publication;
- suspend/resume bookkeeping;
- fairness / lost-wakeup avoidance.

Do not infer that `Scheduler` or a large Scheduler function should be split merely because it is large.

### R3 — Transfer

Owns composed-I/O progress:

- partial read/write;
- remaining bytes;
- offsets;
- retry/progress;
- EOF;
- zero-progress behavior;
- cancellation observation;
- result aggregation.

Sync, polling, and await/fiber drivers may intentionally expose different policies.

### R4 — Resource

Owns explicit bounded-resource vocabulary and saturation behavior.

Keep distinct:

- application pipeline depth;
- Completion count;
- buffer / in-flight byte budget;
- request capacity;
- backend dispatch capacity;
- ThreadPool worker count;
- io_uring SQ/CQ depth;
- kernel in-flight execution.

Do not replace these with one vague `concurrency = N` concept.

### R5 — Capability

Owns optional execution opportunities and strategy/capability boundaries.

Examples may include buffered availability, vectored I/O, sendfile/splice/range-copy, registered
buffers/fixed files, multishot operations, durability operations, readiness/timer support, and
cancellation strength.

Target principle:

> **common semantic minimum + explicit capability differences**

Do not invent a generic capability framework before multiple real use cases justify it.

### R6 — Observability

Owns attribution of behavior, cost, and saturation across:

```text
APP
→ PUBLIC / COMPOSED API
→ RUNTIME / WAIT
→ BACKEND
→ OS / FILESYSTEM / DEVICE
```

#221 E1 / PR #224 is the first abstraction-tax evidence foundation.

Observability must follow existing synchronization/lifecycle authority rather than invent a second
authority merely for diagnostics.

---

## 5. Current execution phase

The repository has completed:

```text
low-risk mechanical cleanup
→ product-surface freeze
→ v0.0.1 baseline
```

The next major planned campaign is:

```text
#221 + #225 + #226 joint audit
        ↓
evidence + target architecture + current gap
        ↓
derive real refactor order
        ↓
Request
→ Resource + Observability
→ Wait
→ Transfer
→ Capability
```

Focused, already-proven correctness defects may be repaired before the joint audit when they can be
kept narrow and do not silently begin a six-domain architecture campaign.

Do not start broad architecture work merely because a local fix reveals a larger design opportunity.

---

## 6. Current architectural posture

### Preserve unless evidence disproves

These are currently treated as meaningful authorities, not obvious cleanup targets:

- `RequestArena` request-slot lifecycle;
- `detail::submit_transaction` pre-accept transaction/rollback ladder;
- backend-local admission/resource/transport differences;
- Scheduler routing authority;
- explicit separation of resource dimensions;
- independent race/death/formal test witnesses.

### Audit / deepen later

Known architecture questions include:

- repeated backend request glue around the existing Request authority;
- duplicated wait lifecycle mechanics across primitives;
- composed-I/O progress duplication with intentional driver-policy differences;
- incomplete resource/backpressure vocabulary;
- incomplete runtime attribution/observability;
- capability/strategy product decisions around Copy and optional execution paths.

These belong to #225/#226-driven campaigns, not opportunistic cleanup.

---

## 7. Refactoring rule: semantic compression, not DRY

Duplication classification:

```text
S0 — textual duplication
S1 — structural duplication
S2 — semantic duplication
S3 — authority duplication
```

Cross-module centralization is strongly justified only when **S2 + S3** hold.

A useful deep module:

1. owns a real invariant/lifecycle;
2. removes duplicate semantic authority;
3. exposes less conceptual surface than the complexity it absorbs;
4. preserves intentional backend/resource/policy differences;
5. remains independently testable and observable.

Deleted LOC is not the primary measure of progress.

---

## 8. Things agents must not infer

Do **not** infer:

- `0 repo callers => dead code`;
- repeated code => shared abstraction required;
- large function => module boundary;
- public-but-unused => removable;
- empty function => removable;
- same loop shape => same observable contract;
- same test scenario names => duplicate tests;
- backend similarity => one generic backend base;
- one fast benchmark cell => new universal default;
- one capability probe => broken abstraction;
- a TSan/sanitizer workaround => correctness fix.

Before deleting an apparently unused symbol, classify it:

```text
D0 — private implementation dead
D1 — installed/internal surface
D2 — public/stable-ish API
D3 — test/instrumentation authority
D4 — identity/layout/ABI marker
```

Only D0 is automatically a mechanical-cleanup candidate.

---

## 9. Hard refactoring guardrails

Do not casually:

1. delete public-but-repo-unused surface;
2. flatten provenance-bearing backend identity;
3. introduce a mega `BackendAdapter` for DRY;
4. normalize intentional sync/poll/await semantics;
5. split Scheduler mega-functions without moving a real invariant;
6. over-DRY race choreography, formal traces, or independent test oracles;
7. collapse distinct resources into one concurrency knob;
8. add hidden coalescing/splitting of already-explicit I/O;
9. add speculative capability APIs;
10. optimize Core from aggregate application benchmarks without attribution.

---

## 10. Where the authority lives

When working in a subsystem, navigate from this file to the real authority.

Use the repository authority order from `AGENTS.md`; in practical terms, start with:

1. current approved task / issue / review scope;
2. accepted subsystem design / ADR;
3. `docs/architecture/architecture-constitution.md`;
4. `AGENTS.md`;
5. public headers under `include/sluice/` and `docs/reference/api.md`;
6. production implementation under `src/`;
7. contract/regression/death/negative/formal tests;
8. `xmake.lua`;
9. `.github/workflows/*.yml`;
10. `README.md`.

`CONTEXT.md` is **not** inserted into that authority chain. It only points to it.

---

## 11. Key repository maps

Start here:

- `AGENTS.md` — repository-wide operating contract;
- `docs/architecture/architecture-constitution.md` — architecture constitution;
- `docs/architecture/README.md` — architecture document index;
- `docs/reference/api.md` — public API reference;
- `xmake.lua` — build/test target truth;
- `.github/workflows/` — CI/merge-gate truth;
- `CHANGELOG.md` — release/change history.

Roadmap / evidence:

- #221 — Explicit-I/O Value Evaluation;
- #225 — six-domain architecture roadmap;
- #226 — six-domain adversarial architecture audit;
- #227 — short-term execution roadmap.

Baseline:

- tag `v0.0.1`.

---

## 12. Maintenance rule for this file

Update this file only when one of these changes materially:

- stable checkpoint/tag;
- roadmap issue roles;
- six-domain names or ownership boundaries;
- current execution phase;
- named semantic authorities;
- repository authority/navigation entry points.

Do **not** update it for every PR, benchmark run, bug, or implementation detail.

Do not copy large ADR/issue sections here. Prefer:

```text
short statement
+ file/issue pointer
```

over duplicated normative prose.

If a statement here becomes stale, fix or delete it; do not preserve historical sediment in this file.
