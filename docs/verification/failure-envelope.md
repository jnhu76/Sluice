# Failure envelope matrix (#198, child of #163 V5)

**Artifact**: [`failure-envelope.json`](failure-envelope.json) — the
machine-checkable `phase × fault ⇒ required outcome` matrix for the major
lifecycle paths, with the core invariant

```text
accepted -> cannot disappear
```

as the spanning row class (`"spanning": true`).

**Gate**: `python3 scripts/gates/failure-envelope.py` (self-test:
`--self-test`). Wired into `scripts/gates/pre-push.sh` (Gate 5d) and therefore
into CI's repository mechanical gates. The gate is file-scoped, not
changed-lines-scoped: a row going stale fails the gate even if the row itself
was not touched.

This matrix **consumes** existing evidence — `docs/architecture/failure-model.md`
(T1–T7 taxonomy) and the C2b–C2e / D2–D4 / WAL-fuzz mutation-evidence layers —
it does not manufacture any (#198 non-goal). It is a separate evidence
technology, not TLA+ debt (see
[`formal/cpp-model-coverage.md`](formal/cpp-model-coverage.md) "Not TLA debt").

## Row shape

| Field | Meaning |
|-------|---------|
| `id` | `FE-NNN`, unique |
| `phase` | lifecycle phase (closed vocabulary, 16 phases) |
| `fault` | fault class (closed vocabulary) |
| `layer` | `core` / `arena` / `fake-async` / `threadpool` / `uring` / `context` / `scheduler` |
| `required_outcome` | the contract the fault response must produce (closed vocabulary) |
| `taxonomy` | T1–T7 class from `failure-model.md` |
| `spanning` | the row expresses the `accepted -> cannot disappear` invariant |
| `status` | `VERIFIED` / `PENDING` / `PLATFORM-BOUND` / `COVERAGE-BOUNDARY` |
| `evidence` | pointer list (see below) |
| `status_note` | **required** for every non-VERIFIED row — never a silently green row |

## Evidence pointers and tiers

Every evidence entry has `kind` (`mutation` / `test` / `death` / `weakmem` /
`fuzz` / `formal`), `ref` (repository file), `tier`, and optional resolution
constraints: `anchor` (token that must appear in `ref`, e.g. a mutant id),
`case` (test-case name that must appear under `tests/`), `target` (xmake
target name that must appear under `xmake/`). A pointer that stops resolving
fails the gate.

Evidence tiers follow the #163 §10 order — deterministic internal/fake
injection → syscall-boundary → kernel/backend fault (real liburing, scripted)
→ real platform — plus `bounded-model` (#197 weak-memory kernels) and
`formal-tla`. The highest available tier is recorded per row; lower tiers are
not hidden by higher ones (multiple pointers per row are the norm).

## Statuses (honest open states)

- **VERIFIED** — at least one resolvable evidence pointer.
- **PLATFORM-BOUND** — the next evidence tier requires an environment this
  host does not provide (e.g. real-kernel fault injection). The `status_note`
  names what re-opens the row.
- **COVERAGE-BOUNDARY** — no mechanism-level evidence exists for the cell at
  all; filling it would require a new seam (a #198 non-goal). Recorded so the
  boundary is visible instead of silently green.
- **PENDING** — runnable evidence exists but has not been run. A **spanning**
  row must never be PENDING (the gate enforces this).

## Current open rows

| Row | Status | Why open |
|-----|--------|----------|
| FE-017 | PLATFORM-BOUND | real-kernel EINTR arrival on a blocked worker syscall is not injectable here; the deterministic retry authority is FE-016 |
| FE-048 | PLATFORM-BOUND | real kernel/backend fault on an in-flight uring operation; the scripted kernel-edge tier is FE-022..FE-026 |
| FE-049 | COVERAGE-BOUNDARY | Scheduler-tier dispatch fault injection has no seam; adding one is a deliberate reviewed change, not a matrix edit |

## Documented negative results (kept, not hidden)

- C2e M6/M7 (FE-042 note): the ThreadPool destructor's `slot_in_use` /
  `backend_ready` checks are defense-in-depth redundancy — the ARENA
  destructor is the covering destruction authority (FE-043).
- D2 M8 first attempt (FE-026 note): a surviving mutant was correctly treated
  as *not evidence*; the detector was rebuilt on a transport-state invariant.

## Adding or changing a row

1. Edit `failure-envelope.json`; add new vocabulary tokens only in
   `scripts/gates/failure-envelope.py` (the closed vocabularies are the gate's,
   so extending them is a reviewed gate change, not a matrix edit).
2. Every VERIFIED row must cite evidence that resolves; every non-VERIFIED row
   must carry a `status_note`.
3. Run `python3 scripts/gates/failure-envelope.py --self-test && python3
   scripts/gates/failure-envelope.py` — the pre-push gate runs both.
4. A phase that loses its last VERIFIED row fails the coverage floor; either
   restore evidence or shrink the phase vocabulary deliberately.
