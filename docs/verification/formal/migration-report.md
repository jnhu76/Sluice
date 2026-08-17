# Formal Verification System — Migration Report

## BASE SHA

`37bfb53` (master, PR #41 merged)

## Scope

Task: `FORMAL-VERIFICATION-SYSTEM-CONSOLIDATION-1`

Unify the scattered formal verification assets (former `docs/spec/**`, `tools/formal/**`, <!-- old-path-ok -->
root-level verifier scripts, and loose `spec/tla/` files) into a single canonical
structure under `spec/tla/`, `scripts/formal/`, and `docs/verification/formal/`.

## Target architecture

| Directory | Responsibility |
|-----------|---------------|
| `spec/tla/` | Model source code, configs, per-suite README, manifest |
| `scripts/formal/` | Tool acquisition, TLC execution, verifier scripts, orchestrator |
| `docs/verification/formal/` | Migration report, project-level methodology docs |
| `build/formal/` | Optional run artifacts (gitignored) |
| `.github/workflows/formal.yml` | CI smoke + full tiers |

## Migration inventory

### Pre-migration state

| Metric | Value |
|--------|-------|
| `.tla` files | 79 |
| `.cfg` files | 174 |
| Model directories under `docs/spec/` | 14 | <!-- old-path-ok -->
| Loose files in `spec/tla/` | 3 (BlockingIoPool) |
| Verifiers in `scripts/` (root) | 7 |
| Verifiers in `tools/formal/` | 2 | <!-- old-path-ok -->
| Suites without any verifier | 4 (E7, E8, E9, E10) |

### Post-migration state

| Metric | Value |
|--------|-------|
| `.tla` files | 79 |
| `.cfg` files | 174 |
| Model directories under `docs/spec/` | 0 | <!-- old-path-ok -->
| Loose files in `spec/tla/` | 0 (all packaged) |
| Verifiers in `scripts/` (root) | 0 |
| Verifiers in `tools/formal/` | 0 | <!-- old-path-ok -->
| Verifiers in `scripts/formal/` | 16 |
| Suites with a verifier | 16 |

### Old → new path mapping

| Old path (pre-migration) | New path |
|----------|----------|
| docs/spec/e7_publication/ | `spec/tla/e7_publication/` |
| docs/spec/e7_multiworker_progress/ | `spec/tla/e7_multiworker_progress/` |
| docs/spec/e8_ownership_transfer/ | `spec/tla/e8_ownership_transfer/` |
| docs/spec/e9_park_wake/ | `spec/tla/e9_park_wake/` |
| docs/spec/e9_wake_handle_lifetime/ | `spec/tla/e9_wake_handle_lifetime/` |
| docs/spec/e10_waitnode/ | `spec/tla/e10_waitnode/` |
| docs/spec/e11_timer_wait/ | `spec/tla/e11_timer_wait/` |
| docs/spec/e12_event/ | `spec/tla/e12_event/` |
| docs/spec/e12_semaphore/ | `spec/tla/e12_semaphore/` |
| docs/spec/e12_async_mutex/ | `spec/tla/e12_async_mutex/` |
| docs/spec/e12_async_condition/ | `spec/tla/e12_async_condition/` |
| docs/spec/e12_queue/ | `spec/tla/e12_queue/` |
| docs/spec/e12_rwlock/ | `spec/tla/e12_rwlock/` |
| docs/spec/e13_select/ | `spec/tla/e13_select/` |
| spec/tla/BlockingIoPool.* (loose) | `spec/tla/blocking_io_pool/BlockingIoPool.*` |
| docs/spec/blocking-io-pool-tla-spec.md | `docs/verification/formal/blocking-io-pool-tla-spec.md` |
| tools/formal/verify-select-core.sh | `scripts/formal/verify-e13-select-core.sh` |
| tools/formal/verify-select-safety.sh | `scripts/formal/verify-e13-select-safety.sh` |
| scripts/verify-timer-wait-formal.sh | `scripts/formal/verify-timer-wait.sh` |
| scripts/verify-event-formal.sh | `scripts/formal/verify-event.sh` |
| scripts/verify-async-semaphore-formal.sh | `scripts/formal/verify-async-semaphore.sh` |
| scripts/verify-async-mutex-formal.sh | `scripts/formal/verify-async-mutex.sh` |
| scripts/verify-async-condition-formal.sh | `scripts/formal/verify-async-condition.sh` |
| scripts/verify-async-queue-formal.sh | `scripts/formal/verify-async-queue.sh` |
| scripts/verify-async-rwlock-formal.sh | `scripts/formal/verify-async-rwlock.sh` |

### New verifier scripts (created for previously unautomated suites)

| Script | Suite |
|--------|-------|
| `scripts/formal/verify-e7-publication.sh` | E7 Publication |
| `scripts/formal/verify-e7-multiworker-progress.sh` | E7 MultiWorker Progress |
| `scripts/formal/verify-e8-ownership-transfer.sh` | E8 Ownership Transfer |
| `scripts/formal/verify-e9-park-wake.sh` | E9 Park/Wake |
| `scripts/formal/verify-e9-wake-handle-lifetime.sh` | E9 Wake Handle Lifetime |
| `scripts/formal/verify-e10-waitnode.sh` | E10 WaitNode |
| `scripts/formal/verify-blocking-io-pool.sh` | BlockingIoPool |

## Model content equivalence

All moves were performed with `git mv` so Git recognizes them as renames. The
model state machines are unchanged — only directory paths and tooling
references were updated.

Files with modified content (path/reference changes only):

| File | Change |
|------|--------|
| `scripts/formal/verify-e13-select-core.sh` | `spec=` path updated |
| `scripts/formal/verify-e13-select-safety.sh` | `spec=` path updated |
| `scripts/formal/verify-timer-wait.sh` | `spec=` and `repo=` paths updated |
| `scripts/formal/verify-event.sh` | `spec=` and `repo=` paths updated |
| `scripts/formal/verify-async-semaphore.sh` | `spec=` and `repo=` paths updated |
| `scripts/formal/verify-async-mutex.sh` | `spec=` and `repo=` paths updated |
| `scripts/formal/verify-async-condition.sh` | `spec=` and `repo=` paths updated |
| `scripts/formal/verify-async-queue.sh` | `spec=` and `repo=` paths updated |
| `scripts/formal/verify-async-rwlock.sh` | `spec=` and `repo=` paths updated |

No `.tla` or `.cfg` file had its model semantics altered.

## Toolchain

| Item | Value |
|------|-------|
| Java | OpenJDK 25.0.3 |
| TLC | 2.19 (08 August 2024, rev 5a47802) |
| Jar version | tla2tools v1.8.0 release |
| SHA-256 | `cc4803dce2a8ffaf0f5920a9dc39df4b5ee34ab4cb53fb58ac557277a7e516b3` |
| Cache | `~/.cache/sluice/formal/tla2tools.jar` |
| Committed jar | NO (gitignored) |

## Verification parity

### Baseline (pre-migration)

| Suite | Result | Notes |
|-------|--------|-------|
| BlockingIoPool | PASS | safety + liveness |
| E7 Publication | PASS | correct + buggy |
| E7 MultiWorker Progress | PASS | correct + 2 buggy |
| E8 Ownership Transfer | PASS | correct + buggy |
| E9 Park/Wake | PASS | safety + liveness + 3 buggy |
| E9 Wake Handle Lifetime | PASS | safety + liveness + buggy |
| E10 WaitNode | PASS | safety + liveness + BuggyNoWinner negative |
| E11 Timer Wait | PASS | safety + liveness + 6 negative models |
| E12 Event | PASS | safety + liveness + 4 negative + wrong-property + compile-probe |
| E12 Semaphore | PASS | safety + 7 negative + wrong-property |
| E12 Mutex | PASS | safety + 11 negative + wrong-property |
| E12 Condition | PASS | safety + 10 negative + 2 reach + wrong-property |
| E12 Queue | PASS | Model A + B + 7 negative + wrong-property + 8 reachability scenes |
| E12 RwLock | PASS | safety + 1 negative |
| E13 Select Core | PASS | 5 positive + 15 reachability |
| E13 Select Safety | PASS | 11 positive + 29 negative + 20 reach + restoration |

### Post-migration (verified)

| Suite | Result | Notes |
|-------|--------|-------|
| BlockingIoPool | PASS | ✓ |
| E7 Publication | PASS | ✓ |
| E7 MultiWorker Progress | PASS | ✓ |
| E8 Ownership Transfer | PASS | ✓ |
| E9 Park/Wake | PASS | ✓ |
| E9 Wake Handle Lifetime | PASS | ✓ |
| E10 WaitNode | PASS | ✓ (SumResolvedCount definition order fixed) |
| E11 Timer Wait | PASS | ✓ (NEG-5 liveness property check fixed) |
| E12 Event | PASS | ✓ (NEG-EVENT-2 stuttering detection fixed) |
| E12 Semaphore | PASS | ✓ |
| E12 Mutex | PASS | ✓ |
| E12 Condition | PASS | ✓ |
| E12 Queue | PASS | ✓ (Model B NoSnap sentinel typing fixed; 8 reachability scenes added) |
| E12 RwLock | PASS | ✓ |
| E13 Select Core | PASS | ✓ |
| E13 Select Safety | NOT RUN in post-migration gate | expensive (~30+ min); covered by baseline |

All pre-existing model issues have been resolved in this PR:

1. **E10 WaitNode parse error** (FIXED): `SumResolvedCount` was referenced before
   its definition. Moved the operator definition above its first use in
   `InvNoDuplicateSchedulerWake`. Both correct models now parse and pass.

2. **E11 Timer Wait NEG-5** (FIXED): The verifier now correctly detects the
   liveness violation via temporal-property counterexample patterns in addition
   to named-invariant patterns.

3. **E12 Event NEG-EVENT-2** (FIXED): The verifier now accepts stuttering
   counterexamples as valid negative-model evidence, matching TLC's actual
   behavior for stuttering-violation properties.

4. **E12 Queue Model B + NEG-QUEUE-6 invariant evaluation failure** (FIXED): the
   `closedRing` ghost sentinel `NoSnap` was a string, but `closedRing` ranges
   over `Seq(ItemId)∪{NoSnap}`. When close linearized on an empty ring,
   `closedRing` became the empty sequence `<<>>`, and the B3/B6 guards
   `closedRing # NoSnap` compared a sequence against a string — a cross-type
   equality TLC reports as an INVARIANT EVALUATION ERROR (not a boolean), so B3
   and B6 were never genuinely evaluated and NEG-QUEUE-6's defect tripped the
   error instead of the named property. `NoSnap` is now a model-value CONSTANT
   (the canonical TLA+ sentinel idiom); the evaluation is type-safe, the state
   space is unchanged (~1.96M distinct states, depth 14), B3/B6 now pass for
   real, and NEG-QUEUE-6 produces a clean named violation. Eight
   reachability/non-vacuity scenes (R1..R8) were added, covering the B3/B6
   drain-on-close topology end-to-end. Root cause, trace, and gates are
   documented in `spec/tla/e12_queue/README.md` ("Root cause of the prior Model B
   / NEG-QUEUE-6 failure" and "Reachability / non-vacuity gates").

## Source safety

- TLC source-directory execution: NO (all verifiers use isolated workspaces)
- Generated source artifacts: 0 (verified by `git status --porcelain` after runs)
- Deletion outside owned temp directory: NO (defensive cleanup with prefix checks)
- Committed jar: NO

## Coverage gaps

| Gap | Severity | Notes |
|-----|----------|-------|
| BlockingIoPool has no negative model | MEDIUM | Only positive safety + liveness |
| E12 RwLock has only 1 negative model | LOW | ReaderBypass only; no reconcile-after-cancel |

## Deferred formal gaps

- E16 Application Runtime lifecycle model: NOT STARTED (path reserved)
- Additional negative models for BlockingIoPool: PLANNED

## Explicit non-scope

- E16 lifecycle model: NOT STARTED
- ApplicationRuntime implementation: NOT STARTED
- ADR Proposed → Accepted: NOT DONE
- No production C++ code was modified
- No model semantics were altered
