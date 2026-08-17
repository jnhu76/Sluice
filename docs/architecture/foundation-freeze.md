# Async Foundation v0 Freeze

**Status:** FROZEN (2026-08-15, at the Phase G closeout)

After Phase G (`docs/architecture/phase-g-compliance-gate.md` — COMPLETE),
the async foundation — the synchronous core contract, the async request
lifecycle (RequestArena / RequestSlot / Completion), the production
backends (ThreadPool, real io_uring), the Scheduler wake/park protocol
(R1–R4 + the interrupt bridge), Runtime ownership, and the synchronization
primitives — is complete and frozen.

## Policy

A foundation change requires at least ONE of:

1. an application-level correctness reproducer;
2. a deadlock / lost wake / shutdown failure;
3. a backend semantic mismatch;
4. a measured application bottleneck;
5. a capability explicitly required by an application.

The following are NOT sufficient reasons:

- stylistic cleanup;
- speculative optimization;
- "Zig has this";
- theoretical future requirement;
- architecture elegance;
- more formal machinery without a concrete protocol change;
- benchmark-only curiosity.

## Scope

This freeze covers the architecture and protocol surface: park/wake
domains, request identity, terminal publication, cancellation layers,
resource bounds, shutdown/drain, and the public async API. Application
code, examples, benchmarks, and documentation corrections remain open —
they are not foundation changes.

## Frozen: the behavioral contract, not the implementation shape

What may not be weakened is the **invariant set**, not any particular
mechanism. Concretely: the happens-before / routing / consume obligations
between commit, arm, park, wake, and interrupt — for example, the
Phase G routing invariant that once a waiter may already be in a
backend-domain park, an external control wake must have a reliable route
into that domain (today implemented as `backend_wait_active_` gating the
interrupt bridge in `signal_wake_locked`).

The implementation shape is NOT itself frozen. A future change may replace
any concrete mechanism — the atomic flag, a data structure, a generation
counter, a wait token, or a whole backend abstraction — provided the
replacement re-proves the same evidence class under a normal `AGENTS.md`
§8 gate plus one freeze trigger from the policy above:

```text
causal Cases A–D            (commit→arm/park→wake race closure)
TP-G / UR-G matrices        (UR cases real-liburing, mode=real honesty)
mutation gates              (bridge / refuse / armed-floor consume)
formal-model invariants     (spec/tla/e9_park_wake)
```

Freeze the contract; do not treat accidental implementation as
untouchable, and do not weaken the contract while refactoring the
implementation.

## No Phase H

No Phase H is planned. The next work item is an application using the
existing public async/runtime surface. The development mode after the
freeze is application-driven: foundation work happens only through the
triggers above, each entering through a design/compliance gate under
`AGENTS.md` §8 exactly as a phased change would.

## Frozen behavioral semantics (explicit enumeration)

Post-freeze structural work (e.g. the R0/R1 hygiene pass,
`docs/post-freeze/structural-audit.md`) must demonstrate it changed **none** of
the following. Each row names its frozen authority; the authority documents are
the contract, this list is the index.

| Frozen surface | Binding authority |
|---|---|
| Public sync core (`Reader`/`Writer`/`Result<T>`/`IoError`, short-reads, exact/all loop contracts, positional-I/O offset isolation, `flush` ≠ durability, `sync_data`/`sync_all` distinction) | `AGENTS.md` §9, `docs/reference/sync-io-model.md` |
| Public async API surface (headers under `include/sluice/async/`, `docs/reference/api.md`) | §16.1 gate discipline; any change needs explicit approval |
| Operation state transitions (`free→…→completion-ready→free+generation`) and the five-stage submission transaction | `AGENTS.md` §10, `docs/adr/ADR-explicit-io-request-contract.md` |
| Wakeup semantics: park/wake obligation, predicate protocol, split-wait bridge, MIXED-WAKE backstop | AC-6, `docs/design/phase-g-backend-progress-wake.md` |
| Backend wait semantics: `backend_wait_active_` gating of the interrupt bridge; external control wake always has a route into a backend-domain park | Phase G closeout (PR #109), `spec/tla/e9_park_wake` |
| Deadline behavior: monotonic `deadline_t`, `advance_clock`, `*_until`/`*_deadline` waits, timer-select reconcile | E11 deadline design, `docs/architecture/phase-g-compliance-gate.md` |
| Interruption behavior: backend-owned interruption only; cancel intent never rewrites an ordinary result | `AGENTS.md` §11 |
| Cancellation behavior: the seven distinct layers; pending/enqueued/running/kernel-owned dispositions; exactly-once terminal winner | `AGENTS.md` §11, ADR-cancel-request-epoch |
| Concurrency invariants: lock-order tables, arena-leaf domain rules, wake-signal/predicate pairs, generation-safe reuse | `AGENTS.md` §13, AC-2/AC-14, per-phase gates |
| Completion publication authority (only designated reap publishes) | AC-5/AC-13, `docs/adr/ADR-explicit-io-completion-authority.md` |
| Resource bounds equations (arena capacity, worker counts, queue depths, wait-record pool as distinct resources) | AC-7, `AGENTS.md` §12 |
| Shutdown semantics: quiescent destruction, explicit close→drain→reap lifecycle | `AGENTS.md` §14 |

A refactor that keeps every row intact (pure code motion, full test matrix,
diff audit) does not trip the freeze policy; the policy triggers apply only
when a row's observable contract itself would change.

## Governing evidence

- Phase G design: `docs/design/phase-g-backend-progress-wake.md`
- Phase G compliance gate (final): `docs/architecture/phase-g-compliance-gate.md`
- Deterministic causal proof + TP/UR race matrices:
  `tests/phase_g_closeout_test.cpp`, `tests/phase_g_closeout_uring_test.cpp`
- Formal model (R1–R4 + bridge): `spec/tla/e9_park_wake/`
- Remediation roadmap (A–G all COMPLETE): `docs/architecture/remediation-roadmap.md`
