# `spec/tla/` — TLA+ Formal Models

This directory is the canonical home for all executable TLA+ models in the
Sluice repository. Each subdirectory is a self-contained *suite* with the
models, configs, and a local README explaining the protocol being verified.

## Layout

```text
spec/tla/
├── README.md                # this file
├── manifest.json            # machine-readable inventory (authoritative)
├── blocking_io_pool/
│   ├── BlockingIoPool.tla
│   ├── BlockingIoPool.cfg
│   ├── BlockingIoPool_liveness.cfg
│   └── README.md
├── e7_publication/
├── e13_select/
└── ... (one directory per suite)
```

## What belongs here

- `*.tla` — TLA+ module sources (correct and negative/broken variants)
- `*.cfg` — TLC model configurations
- `README.md` — per-suite description, invariant list, expected results
- `EVIDENCE*.md`, `REFINEMENT*.md`, `INVARIANTS*.md`, `NEGATIVE_MODELS*.md`,
  `NON_VACUITY*.md` — evidence and refinement documents bound to the model

## What does NOT belong here

- Project-level verification methodology → `docs/verification/formal/`
- Tooling, bootstrap, and verifier scripts → `scripts/formal/`
- CI workflow → `.github/workflows/formal.yml`
- Historical planning/review documents → `docs/history/`

## Suite package structure

Each suite directory MUST contain:

1. At least one `.tla` module and one `.cfg` config.
2. A `README.md` describing the protocol, the modeled domain, the invariants,
   and the expected TLC verdicts.

It MAY contain:

- Negative/broken-model variants (`*Neg*.tla`, `*Buggy*.tla`).
- Multiple configs for safety, liveness, reachability, refinement.
- Evidence and refinement documents.

## The manifest

`spec/tla/manifest.json` is the machine-readable authority for the suite
inventory. Every `.tla` file MUST belong to exactly one suite declared in the
manifest. Every suite MUST have a verifier script. `python3 scripts/formal/verify.py check`
enforces this.

## Gate types

| Type | Expectation |
|------|-------------|
| **Positive safety** | TLC completes with no invariant violation |
| **Negative / broken model** | TLC violates the *expected named* invariant |
| **Reachability witness** | TLC violates a `NotReach_*` invariant |

A non-zero TLC exit alone is NOT a pass for a negative gate.

## Adding a new suite

1. Create `spec/tla/<suite-id>/` with models and a README.
2. Add a verifier at `scripts/formal/verify-<suite-id>.sh` (source-safe, isolated workspace).
3. Add an entry to `spec/tla/manifest.json`.
4. Update `docs/verification/formal-models.md`.
5. Run `python3 scripts/formal/verify.py check`.

## Governing principle

TLA+ serves the C++ design, never the reverse (AGENTS.md §7). The pipeline
every suite in this tree follows:

```text
C++ contract / production implementation
    -> protocol extraction (the load-bearing race)
    -> TLA+ abstraction (smallest model that captures it)
    -> counterexample / negative mutant (named invariant)
    -> C++ regression bridge (executable evidence)
```

A suite's `implementation_bindings` in `manifest.json` name the C++ files
that own what the model actually proves; external obligations (progress
fairness, caller discipline the leaf does not enforce) must be labeled as
assumptions, not leaf guarantees. The suite inventory (all 19 suites,
including `e16_application_runtime/` and `request_arena/`) is authoritative
in `manifest.json`; per-suite status and debt live in
`docs/verification/formal/cpp-model-coverage.md`.

## Important reminder

> TLA+ verification is of a finite abstract model, not the C++ implementation itself.
