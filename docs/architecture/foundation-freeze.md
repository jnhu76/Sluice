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

## Governing evidence

- Phase G design: `docs/design/phase-g-backend-progress-wake.md`
- Phase G compliance gate (final): `docs/architecture/phase-g-compliance-gate.md`
- Deterministic causal proof + TP/UR race matrices:
  `tests/phase_g_closeout_test.cpp`, `tests/phase_g_closeout_uring_test.cpp`
- Formal model (R1–R4 + bridge): `spec/tla/e9_park_wake/`
- Remediation roadmap (A–G all COMPLETE): `docs/architecture/remediation-roadmap.md`
