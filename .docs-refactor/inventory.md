# Sluice Documentation Inventory

Generated: 2026-07-27
Base: ce15d6b460b922275f7b395aa4e5e5d333852d57
Branch: docs/documentation-architecture-refactor

## Summary

| Category | Count |
|----------|-------|
| Total documentation files | 399 |
| CURRENT (authoritative) | 285 (mostly TLA+ specs) |
| ACCEPTED (design decisions) | 2 |
| CLOSEOUT (phase evidence) | 62 |
| PROPOSED (unapproved designs) | 18 |
| HISTORICAL (obsolete/stale) | 31 |
| VERIFICATION (guides) | 1 |

## File Inventory

### docs/adr/ — Architecture Decision Records

| Path | Title | Status | Authority |
|------|-------|--------|-----------|
| ADR-024S-sync-runtime-contract.md | ADR-024S: Sync Runtime Contract | CURRENT | ACCEPTED_ADR |
| ADR-async-io-model.md | ADR: Async I/O model for sluice | ACCEPTED | ACCEPTED_ADR |
| ADR-execution-model.md | ADR: Dual Threaded/Evented Execution Model | ACCEPTED | ACCEPTED_ADR |

### docs/ — Root-level documentation

#### Public contract (CURRENT)

| Path | Title | Status | Authority |
|------|-------|--------|-----------|
| api-reference.md | API Reference | CURRENT | PUBLIC_CONTRACT |
| api-reference-zh.md | API 参考 | CURRENT | PUBLIC_CONTRACT |
| changelog.md | Changelog | CURRENT | HISTORICAL_RECORD |
| api-audit.md | Public API surface audit | CURRENT | NON_AUTHORITY |

#### Sync I/O architecture (CURRENT)

| Path | Title | Status | Authority |
|------|-------|--------|-----------|
| sync-io-architecture.md | Sync I/O Architecture | CURRENT | CURRENT_ARCHITECTURE |
| sync-io-model.md | Sync I/O Model | CURRENT | CURRENT_ARCHITECTURE |
| sync-durability-model.md | Sync Durability Model | CURRENT | CURRENT_ARCHITECTURE |
| sync-bench-methodology.md | Sync Bench Methodology | CURRENT | VERIFICATION_GUIDE |
| sync-bench-matrix.md | Sync Bench Matrix | CURRENT | VERIFICATION_GUIDE |
| io/sync-backend-taxonomy.md | Sync Backend Taxonomy | CURRENT | CURRENT_ARCHITECTURE |
| io/sync-error-semantics.md | Sync Error Semantics | CURRENT | CURRENT_ARCHITECTURE |

#### Async runtime architecture (CURRENT)

| Path | Title | Status | Authority |
|------|-------|--------|-----------|
| async-runtime-plan.md | Sluice Async Runtime Construction Roadmap | CURRENT | CURRENT_ARCHITECTURE |
| async-runtime-construction-method.md | Async-Runtime Construction Method | CURRENT | CURRENT_ARCHITECTURE |
| async-mutex-nothrow-authority.md | ASYNC Mutex Nothrow Authority | CURRENT | CURRENT_ARCHITECTURE |
| e10-e12-api-semantic-closure.md | E10–E12 API & Semantic Closure | CURRENT | CURRENT_ARCHITECTURE |
| e12-queue-state-machine.md | E12-E Queue State Machine | CURRENT | CURRENT_ARCHITECTURE |
| e12-queue-scheduler-integration.md | E12-E Queue Scheduler Integration | CURRENT | CURRENT_ARCHITECTURE |

#### Phase closeout/evidence documents (CLOSEOUT)

| Path | Title | Status | Authority |
|------|-------|--------|-----------|
| e10-waitnode-wait-queue.md | E10 — WaitNode and WaitQueue | CLOSEOUT | CLOSEOUT_EVIDENCE |
| e11-deadline-timer-wait.md | E11 — Deadline/Timer Wait | CLOSEOUT | CLOSEOUT_EVIDENCE |
| e11-arch-recon-audit.md | E11-ARCH-RECON Audit | CLOSEOUT | CLOSEOUT_EVIDENCE |
| e12-event.md | E12-A Async Event | CLOSEOUT | CLOSEOUT_EVIDENCE |
| e12-semaphore.md | E12-B Async Semaphore | CLOSEOUT | CLOSEOUT_EVIDENCE |
| e12-async-mutex.md | E12-C Async Mutex | CLOSEOUT | CLOSEOUT_EVIDENCE |
| e12-condition.md | E12-D Async Condition | CLOSEOUT | CLOSEOUT_EVIDENCE |
| e12-queue.md | E12-E Queue Corrective-2 | CLOSEOUT | CLOSEOUT_EVIDENCE |
| e12-queue-implementation-authorization.md | E12-E Queue Authorization | CLOSEOUT | CLOSEOUT_EVIDENCE |
| e12-queue-production-implementation.md | E12-E Queue Production Progress | CLOSEOUT | CLOSEOUT_EVIDENCE |
| e12-queue-corrective-3.md | E12-E Queue Corrective-3 | CLOSEOUT | CLOSEOUT_EVIDENCE |
| e12-cross-primitive-terminal-audit.md | E12-G Cross-Primitive Audit | CLOSEOUT | CLOSEOUT_EVIDENCE |
| async-mutex-nothrow-implementation.md | Mutex Nothrow Implementation | CLOSEOUT | CLOSEOUT_EVIDENCE |
| async-backend-parity.md | Async Backend Parity | CLOSEOUT | CLOSEOUT_EVIDENCE |
| async-runtime-hang-and-gcc-corrective.md | Runtime Hang Corrective | CLOSEOUT | CLOSEOUT_EVIDENCE |
| e0a-waiting-policy-audit.md | E0-A Waiting Policy Audit | CLOSEOUT | CLOSEOUT_EVIDENCE |
| e8-0-ownership-topology-audit.md | E8 Ownership Topology Audit | CLOSEOUT | CLOSEOUT_EVIDENCE |
| e8-formal-corrective/audit.md | E8 Formal Corrective Audit | CLOSEOUT | CLOSEOUT_EVIDENCE |
| e8-formal-corrective/refinement-trace.md | E8 Refinement Trace | CLOSEOUT | CLOSEOUT_EVIDENCE |
| e9-0-wake-source-topology-audit.md | E9 Wake Source Topology Audit | CLOSEOUT | CLOSEOUT_EVIDENCE |
| sync-io-model-gap-audit.md | Sync I/O Model Gap Audit | CLOSEOUT | CLOSEOUT_EVIDENCE |
| sync-optimization-notes.md | Sync Optimization Notes | CLOSEOUT | CLOSEOUT_EVIDENCE |
| sync-before-async-readiness-gate.md | Sync-before-Async Readiness Gate | CLOSEOUT | CLOSEOUT_EVIDENCE |
| bench/sync-runtime-bench-notes.md | W1-W4 Bench Notes | CLOSEOUT | CLOSEOUT_EVIDENCE |
| e13-select-p7-rollback-closeout.md | E13 P7 Rollback Closeout | CLOSEOUT | CLOSEOUT_EVIDENCE |
| e13-select-preparation.md | E13 Select Preparation | CLOSEOUT | CLOSEOUT_EVIDENCE |

#### Proposed/Unapproved designs (PROPOSED)

| Path | Title | Status | Authority |
|------|-------|--------|-----------|
| e12-rwlock.md | E12-F AsyncRwLock Design | PROPOSED | PROPOSED_DESIGN |
| e13-select-production-architecture.md | E13 Select Production Architecture | PROPOSED | PROPOSED_DESIGN |
| e13-select-public-api.md | E13 Select Public API | PROPOSED | PROPOSED_DESIGN |
| e13-select-state-machine.md | E13 Select State Machine | PROPOSED | PROPOSED_DESIGN |
| e13-select-locking-and-publication.md | E13 Select Locking & Publication | PROPOSED | PROPOSED_DESIGN |
| e13-select-type-and-lifetime.md | E13 Select Type & Lifetime | PROPOSED | PROPOSED_DESIGN |
| e13-select-event-adapter.md | E13 Select Event Adapter | PROPOSED | PROPOSED_DESIGN |
| e13-select-timer-adapter.md | E13 Select Timer Adapter | PROPOSED | PROPOSED_DESIGN |
| e13-select-test-plan.md | E13 Select Test Plan | PROPOSED | PROPOSED_DESIGN |
| e13-select-production-test-plan.md | E13 Select Production Test Plan | PROPOSED | PROPOSED_DESIGN |
| e13-select-formal-production-mapping.md | E13 Select Formal Mapping | PROPOSED | PROPOSED_DESIGN |
| e14-threaded-evented-parity-preparation.md | E14 Evented Parity Preparation | PROPOSED | PROPOSED_DESIGN |
| formal/e13-select-formal-core-design.md | E13 Formal Core Design | PROPOSED | PROPOSED_DESIGN |
| formal/e13-select-formal-core-plan.md | E13 Formal Core Plan | PROPOSED | PROPOSED_DESIGN |
| formal/e13-select-formal-safety-design.md | E13 Formal Safety Design | PROPOSED | PROPOSED_DESIGN |
| formal/e13-select-formal-safety-plan.md | E13 Formal Safety Plan | PROPOSED | PROPOSED_DESIGN |

#### Historical records (HISTORICAL)

| Path | Title | Status | Authority |
|------|-------|--------|-----------|
| archive/* | Various archive docs | HISTORICAL | HISTORICAL_RECORD |
| design-buffered-fast-path.md | Buffered Fast Path | HISTORICAL | HISTORICAL_RECORD |
| design-copy-strategy.md | Copy Strategy | HISTORICAL | HISTORICAL_RECORD |
| design-flush-sync-durability.md | Flush/Sync/Durability | HISTORICAL | HISTORICAL_RECORD |
| design-io-context.md | IoContext Design | HISTORICAL | HISTORICAL_RECORD |
| design-readv-writev.md | readv/writev Design | HISTORICAL | HISTORICAL_RECORD |
| design-wal-durability.md | WAL Durability | HISTORICAL | HISTORICAL_RECORD |
| bench-methodology.md | Core Microbench Methodology | HISTORICAL | HISTORICAL_RECORD |
| bench-decision-matrix.md | Optimization Decision Matrix | HISTORICAL | HISTORICAL_RECORD |
| bench-optimization-runbook.md | Optimization Runbook | HISTORICAL | HISTORICAL_RECORD |
| async-deferred-until-sync-baseline.md | Async Deferred Until Sync Baseline | HISTORICAL | HISTORICAL_RECORD |
| async-design-alternatives.md | Async Design Alternatives | HISTORICAL | HISTORICAL_RECORD |
| async-next-jobs.md | Async Next Job Cards | HISTORICAL | HISTORICAL_RECORD |
| async-problem-statement.md | Async Problem Statement | HISTORICAL | HISTORICAL_RECORD |
| async-readiness-gate.md | Async Readiness Gate | HISTORICAL | HISTORICAL_RECORD |
| async-source-inventory.md | Async Source Inventory | HISTORICAL | HISTORICAL_RECORD |
| io-uring-readiness-gate.md | io_uring Readiness Gate | HISTORICAL | HISTORICAL_RECORD |
| io-uring-spike.md | Experimental io_uring Spike | HISTORICAL | HISTORICAL_RECORD |
| zig-std-io-parity-audit.md | Zig std.Io Parity Audit | HISTORICAL | HISTORICAL_RECORD |
| zig-std-io-source-inventory.md | Zig std.Io Source Inventory | HISTORICAL | HISTORICAL_RECORD |
| zig-stdio-async-port-map.md | Zig std.Io Async Port Map | HISTORICAL | HISTORICAL_RECORD |
| zig-stdio-migration-jobs.md | Zig std.Io Migration Jobs | HISTORICAL | HISTORICAL_RECORD |
| e12-sync-primitives-plan.md | E12 Sync Primitives Plan | HISTORICAL | HISTORICAL_RECORD |
| sync-io-next-jobs.md | Sync I/O Next Jobs | HISTORICAL | HISTORICAL_RECORD |

### docs/archive/ — Previously archived

All 6 files are HISTORICAL. They should remain in archive/ or move to history/.

### docs/reviews/ — Review evidence

All ~30+ review files are CLOSEOUT evidence. They should be moved to history/closeout/ or remain in reviews/ with clear historical marking.

### docs/results/ — Benchmark results

3 files: CLOSEOUT evidence.

### docs/spec/ — Formal model specifications

All TLA+ models and READMEs are CURRENT formal model evidence. No changes needed.

### docs/known-issues/ — Security follow-ups

1 file: security-review-followups.md — CURRENT, non-authority reference.

### docs/slices/ — Static analysis reports

1 file: CLOSEOUT evidence.

## Recommended Migration Plan

1. Create `docs/README.md` — front door
2. Create `docs/adr/README.md` — ADR index
3. Create `docs/verification/` — reusable verification guides
4. Create `docs/design/README.md` — proposed designs index
5. Create `docs/roadmap/README.md` — active roadmap
6. Move historical planning docs to `docs/history/implementation-plans/`
7. Move closeout/phase docs to `docs/history/closeout/`
8. Move superseded docs to `docs/history/superseded/`
9. Update links throughout
10. Update changelog
11. Final report