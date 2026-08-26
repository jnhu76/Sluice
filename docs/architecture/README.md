# Architecture — Classification Index

This directory mixes two kinds of material at one path level: **current
architecture authority** and **point-in-time compliance/audit evidence**. This
index separates them so a reader can find current truth without archaeology,
and so future archive moves (issue #167 Step 5) know which documents are
path-pinned.

Classification baseline: `master@28284da` (2026-08-24, issue #167 Step 3).
Re-classify when documents change role, not on a schedule.

## Classes

- **CURRENT** — describes the as-built architecture, an active invariant, an
  active contract, or an active process/policy authority.
- **EVIDENCE** — point-in-time compliance, gate, audit, or closeout record.
  Still navigated by scripts, CI, spec manifests, tests, or current documents;
  not current authority for new decisions, but not dead history either.
- **HISTORICAL** — superseded planning/design record kept for provenance only.

Move policy: **classify before move**. A document moves to
[`docs/history/`](../history/README.md) only when its exact-path consumers are
inventoried and updated atomically, a historical banner is added, and no
active #163 verification anchor depends on the current path. "Blocked" below
means a **mechanical pin** — a script, CI step, spec manifest, or operational
verification anchor that hard-codes the path — or a **current code/verification
authority** that explicitly depends on the document at that location (Step 5b
ontology, issue #167). A relocatable prose, source, or test **comment** that
merely contains the path is a consumer, not a pin: it does not by itself block
a move, and it MUST be updated atomically with the move.

## CURRENT — current architecture authority

| Document | Current purpose | Move? |
|----------|-----------------|------|
| [`architecture-constitution.md`](architecture-constitution.md) | AC-N engineering principles; pinned by `AGENTS.md` and `scripts/verify-architecture-docs.py` | No |
| [`overview.md`](overview.md) | Architecture entry point; linked from root READMEs | No |
| [`async-runtime.md`](async-runtime.md) | Scheduler/Fiber/execution architecture | No |
| [`async-synchronization.md`](async-synchronization.md) | WaitNode/WaitQueue + synchronization primitives | No |
| [`async-io-foundation.md`](async-io-foundation.md) | Completion/AsyncBackend/AsyncIoContext layer | No |
| [`async-request-lifecycle.md`](async-request-lifecycle.md) | Per-request lifecycle narrative (as-built) | No |
| [`sync-io-architecture.md`](sync-io-architecture.md) | Synchronous I/O model | No |
| [`sync-durability-model.md`](sync-durability-model.md) | Durability contracts (`sync_data`/`sync_all`) | No |
| [`sync-backend-taxonomy.md`](sync-backend-taxonomy.md) | Sync backend boundary decisions | No |
| [`failure-model.md`](failure-model.md) | T1–T7 failure taxonomy + assert policy; consumed by `AGENTS.md` §9.2, assert-hygiene gates, `scripts/gates/pre-push.sh`, failure-model tests | No |
| [`divergence-registry.md`](divergence-registry.md) | Live Zig-divergence registry; pinned by `AGENTS.md` and `scripts/verify-architecture-docs.py` | No |
| [`design-compliance-gate.md`](design-compliance-gate.md) | Generic Gate 0–4 compliance-gate template; referenced by `AGENTS.md` §8 | No |
| [`zig-io-conformance-map.md`](zig-io-conformance-map.md) | Zig std.Io conformance/divergence map; pinned by `scripts/verify-architecture-docs.py` | No |
| [`foundation-freeze.md`](foundation-freeze.md) | Active freeze policy: entry conditions for any foundation change | No |

## EVIDENCE — point-in-time compliance/audit records

| Document | Records | Move? |
|----------|---------|------|
| [`as-built-async-architecture.md`](as-built-async-architecture.md) | Commit-pinned as-built snapshot + subsystem update blocks; pinned by `scripts/verify-architecture-docs.py` | Blocked |
| [`current-architecture-findings.md`](current-architecture-findings.md) | Audit findings with resolution notes; pinned by `scripts/verify-architecture-docs.py` | Blocked |
| [`remediation-roadmap.md`](remediation-roadmap.md) | Phase A–G ordering record (all COMPLETE); pinned by `scripts/verify-architecture-docs.py` | Blocked |
| [`phase-b-compliance-gate.md`](phase-b-compliance-gate.md) | RequestKey/RequestSlot reference lifecycle gate | Deferred |
| [`phase-c1-conformance-gate.md`](phase-c1-conformance-gate.md) | Backend conformance framework; pinned by `scripts/tests/test_backend_conformance_manifest.py` | Blocked |
| [`phase-c2a-compliance-gate.md`](phase-c2a-compliance-gate.md) | Capacity/admission/accounting conformance | Deferred |
| [`phase-c2b-compliance-gate.md`](phase-c2b-compliance-gate.md) | Generation/stale-key/cancel-winner conformance | Deferred |
| [`phase-c2c-compliance-gate.md`](phase-c2c-compliance-gate.md) | Waiter/borrow/delivery-lease conformance | Deferred |
| [`phase-c2d-compliance-gate.md`](phase-c2d-compliance-gate.md) | Failure-injection/allocator-failure conformance | Deferred |
| [`phase-c2e-compliance-gate.md`](phase-c2e-compliance-gate.md) | Close/drain/reset/destruction conformance | Deferred |
| [`phase-d1-uring-frozen-design.md`](phase-d1-uring-frozen-design.md) | Uring private-ring migration frozen design; pinned by `spec/tla/manifest.json` | Blocked |
| [`phase-d1-uring-permanent-submit-failure-audit.md`](phase-d1-uring-permanent-submit-failure-audit.md) | `io_uring_submit()` permanent-failure audit; pinned by `spec/tla/manifest.json` | Blocked |
| [`phase-d2-uring-failure-noalloc-gate.md`](phase-d2-uring-failure-noalloc-gate.md) | Uring failure/no-allocation gate | Deferred |
| [`phase-e-compliance-gate.md`](phase-e-compliance-gate.md) | ThreadPoolBackend bounded migration gate | Deferred |
| [`phase-f1-compliance-gate.md`](phase-f1-compliance-gate.md) | Scheduler identity-bearing reap gate; relocatable test-comment consumers `tests/scheduler_identity_wake_test.cpp:17`, `tests/uring_f1_scheduler_routing_test.cpp:21` (updated atomically; no script/spec pin) | Deferred |
| [`phase-f3-compliance-gate.md`](phase-f3-compliance-gate.md) | Public RequestHandle gate | Deferred |
| [`phase-g-compliance-gate.md`](phase-g-compliance-gate.md) | backend-ready progress wake integration gate | Deferred |
| [`issue-115-runnable-publication-wake-gate.md`](issue-115-runnable-publication-wake-gate.md) | Runnable-publication wake obligation gate | Deferred |
| [`issue-116-reentry-liveness-gate.md`](issue-116-reentry-liveness-gate.md) | Invocation-boundary lost re-entry liveness gate; pinned by `scripts/gates/mechanical-facts.py` (`TEST_TOTAL_EXTRA_DOCS`) | Blocked |
| [`issue-161-idle-dance-contribution-generation-gate.md`](issue-161-idle-dance-contribution-generation-gate.md) | Idle-dance contribution generation gate; pinned by `spec/tla/manifest.json` and `spec/tla/e12_rwlock_scheduler_liveness/README.md` | Blocked |
| [`issue-229-deadline-test-seam-lock-gate.md`](issue-229-deadline-test-seam-lock-gate.md) | Timer test-seam observation race repair gate (locked snapshot under `global_mtx_`) | Deferred |

## HISTORICAL — superseded records (provenance only)

| Document | Records | Move? |
|----------|---------|------|
| [`phase-d-uring-migration-plan.md`](phase-d-uring-migration-plan.md) | Phase D plan; self-declares historical banner, superseded by the frozen design + completed gates | Deferred (5 docs consumers to update atomically) |

## Step 5 archive moves (executed 2026-08-25, issue #167)

The zero-consumer first move set from Step 3 now lives in `docs/history/`:

- `phase-d2-uring-failure-noalloc-implementation-plan.md` →
  `docs/history/implementation-plans/`
- `issue-137-submission-transaction-design.md` →
  `docs/history/implementation-plans/`
- `issue-137-submission-transaction-{compliance-gate,mutation-evidence}.md` →
  `docs/history/closeout/`
- `c7-runtime-await-helpers-compliance-gate.md` → `docs/history/closeout/`
- `phase-f-compliance-gate.md`, `phase-d3-uring-identity-waiter-gate.md`,
  `phase-d4-uring-wait-close-drain-gate.md` → `docs/history/closeout/`

Each moved file carries an archive banner; every live reference (one public
header, the pinned-evidence cross-references, and this index) was updated
atomically, and the old paths are recorded in `KNOWN_MOVED`
(`scripts/check-doc-links.py`) so any new reference to them fails the docs
gate. The remaining moves are the Deferred rows above; Blocked rows move only
together with the mechanical / current-authority pins that reference them.
