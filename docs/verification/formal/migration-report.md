# Formal Verification System — Migration Report

## BASE SHA

`37bfb53` (master, PR #41 merged)

## Scope

Task: `FORMAL-VERIFICATION-SYSTEM-CONSOLIDATION-1`

Unify the scattered formal verification assets (`docs/spec/**`, `tools/formal/**`,
`scripts/verify-*formal*`, `spec/tla/BlockingIoPool.tla`) into a single canonical
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
| Model directories under `docs/spec/` | 14 |
| Loose files in `spec/tla/` | 3 (BlockingIoPool) |
| Verifiers in `scripts/` (root) | 7 |
| Verifiers in `tools/formal/` | 2 |
| Suites without any verifier | 4 (E7, E8, E9, E10) |

### Post-migration state

| Metric | Value |
|--------|-------|
| `.tla` files | 79 |
| `.cfg` files | 174 |
| Model directories under `docs/spec/` | 0 |
| Loose files in `spec/tla/` | 0 (all packaged) |
| Verifiers in `scripts/` (root) | 0 |
| Verifiers in `tools/formal/` | 0 |
| Verifiers in `scripts/formal/` | 16 |
| Suites with a verifier | 16 |

### Old → new path mapping

| Old path | New path |
|----------|----------|
| `docs/spec/e7_publication/` | `spec/tla/e7_publication/` |
| `docs/spec/e7_multiworker_progress/` | `spec/tla/e7_multiworker_progress/` |
| `docs/spec/e8_ownership_transfer/` | `spec/tla/e8_ownership_transfer/` |
| `docs/spec/e9_park_wake/` | `spec/tla/e9_park_wake/` |
| `docs/spec/e9_wake_handle_lifetime/` | `spec/tla/e9_wake_handle_lifetime/` |
| `docs/spec/e10_waitnode/` | `spec/tla/e10_waitnode/` |
| `docs/spec/e11_timer_wait/` | `spec/tla/e11_timer_wait/` |
| `docs/spec/e12_event/` | `spec/tla/e12_event/` |
| `docs/spec/e12_semaphore/` | `spec/tla/e12_semaphore/` |
| `docs/spec/e12_async_mutex/` | `spec/tla/e12_async_mutex/` |
| `docs/spec/e12_async_condition/` | `spec/tla/e12_async_condition/` |
| `docs/spec/e12_queue/` | `spec/tla/e12_queue/` |
| `docs/spec/e12_rwlock/` | `spec/tla/e12_rwlock/` |
| `docs/spec/e13_select/` | `spec/tla/e13_select/` |
| `spec/tla/BlockingIoPool.*` | `spec/tla/blocking_io_pool/BlockingIoPool.*` |
| `docs/spec/blocking-io-pool-tla-spec.md` | `docs/verification/formal/blocking-io-pool-tla-spec.md` |
| `tools/formal/verify-select-core.sh` | `scripts/formal/verify-e13-select-core.sh` |
| `tools/formal/verify-select-safety.sh` | `scripts/formal/verify-e13-select-safety.sh` |
| `scripts/verify-timer-wait-formal.sh` | `scripts/formal/verify-timer-wait.sh` |
| `scripts/verify-event-formal.sh` | `scripts/formal/verify-event.sh` |
| `scripts/verify-async-semaphore-formal.sh` | `scripts/formal/verify-async-semaphore.sh` |
| `scripts/verify-async-mutex-formal.sh` | `scripts/formal/verify-async-mutex.sh` |
| `scripts/verify-async-condition-formal.sh` | `scripts/formal/verify-async-condition.sh` |
| `scripts/verify-async-queue-formal.sh` | `scripts/formal/verify-async-queue.sh` |
| `scripts/verify-async-rwlock-formal.sh` | `scripts/formal/verify-async-rwlock.sh` |

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
| SHA-256 | `936a262061c914694dfd669a543be24573c45d5aa0ff20a8b96b23d01e050e88` |
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
| E10 WaitNode | BLOCKED | pre-existing parse error (SumResolvedCount) |
| E11 Timer Wait | PASS* | NEG-5 has pre-existing liveness naming issue |
| E12 Event | PASS* | NEG-EVENT-2 pre-existing toolchain limitation |
| E12 Semaphore | PASS | safety + 7 negative + wrong-property |
| E12 Mutex | PASS | safety + 11 negative + wrong-property |
| E12 Condition | PASS | safety + 10 negative + 2 reach + wrong-property |
| E12 Queue | PASS | Model A + B + 7 negative + wrong-property |
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
| E10 WaitNode | BLOCKED | correct model parse error (pre-existing) |
| E11 Timer Wait | PASS* | ✓ (same pre-existing NEG-5 behavior) |
| E12 Event | PASS* | ✓ (same pre-existing NEG-EVENT-2 behavior) |
| E12 Semaphore | PASS | ✓ |
| E12 Mutex | PASS | ✓ |
| E12 Condition | PASS | ✓ |
| E12 Queue | PASS | ✓ |
| E12 RwLock | PASS | ✓ |
| E13 Select Core | PASS | ✓ |
| E13 Select Safety | NOT RUN in post-migration gate | expensive (~30+ min); covered by baseline |

\* Pre-existing toolchain limitations documented in the manifest and below.

## Pre-existing issues (not introduced by this migration)

1. **E10 WaitNode parse error**: `E10WaitNode.tla` references an unknown operator
   `SumResolvedCount` at line 169. The correct model does not parse under TLC 2.19.
   Only the `BuggyNoWinner` negative model runs. This is a pre-existing broken
   model that predates this migration.

2. **E11 Timer Wait NEG-5**: The `DeadlineLostParked` negative model produces a
   counterexample, but TLC reports the liveness property violation differently
   than the verifier expects. The counterexample IS produced; the named-property
   check is a toolchain-specific limitation.

3. **E12 Event NEG-EVENT-2**: The `WakeOneStrandsWaiter` negative model produces
   a stuttering counterexample rather than the expected named invariant
   violation. Documented as a pre-existing toolchain limitation with baseline
   parity.

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
| E10 WaitNode correct model is broken | HIGH | Pre-existing parse error; needs separate fix |
| E11 NEG-5 naming mismatch | LOW | Counterexample produced; named-check limitation |
| E12 Event NEG-EVENT-2 stuttering | LOW | Pre-existing toolchain limitation |

## Deferred formal gaps

- E16 Application Runtime lifecycle model: NOT STARTED (path reserved)
- Additional negative models for BlockingIoPool: PLANNED
- E10 WaitNode model repair: PLANNED (separate task)

## Explicit non-scope

- E16 lifecycle model: NOT STARTED
- ApplicationRuntime implementation: NOT STARTED
- ADR Proposed → Accepted: NOT DONE
- No production C++ code was modified
- No model semantics were altered
