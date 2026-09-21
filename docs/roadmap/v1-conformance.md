# Sluice v1 Conformance Ledger

**Target authority:** [v1 Architecture and Contract Reference](../explicit-io-v1-final-decision.md), revision v1-r3.
**Starting implementation baseline:** `c64f005e6e59e791f26a7ab4a594c33954f096dd`.
**Purpose:** track implementation and evidence, not add or relax contracts.

This ledger starts a new assessment. Previous architecture CLOSED/CONFORMANT
labels do not transfer. PR #389 changes documentation only; it does not implement
RequestCore, new Request APIs, progress integration, or shutdown behavior.

## Status vocabulary

| Status | Meaning |
|---|---|
| NOT_ASSESSED | No complete assessment against these target requirements recorded |
| GAP | A specific missing/nonconforming behavior has been identified with source evidence |
| IMPLEMENTED_UNVERIFIED | A candidate implementation exists; required evidence is incomplete |
| VERIFIED | Named requirements passed the recorded evidence for specified commits/configurations |
| DEFERRED_OUT_OF_SCOPE | Not a required v1 capability; no supported-product claim |

VERIFIED never means universal correctness. A required v1 item may not use
DEFERRED_OUT_OF_SCOPE. Split a row if only part has evidence. Keep safety,
conditional liveness, memory visibility and kernel evidence separately visible.

## Implementation assessment

| Slice | Root requirements | Initial status | Required evidence before closure |
|---|---|---|---|
| Public operation oracle | SEM, ERR | NOT_ASSESSED | Semantic tables; short-I/O/reference-byte properties; V01–V03, V15–V17, V27 |
| Direct invocation | INV, ARCH, W-01 | NOT_ASSESSED | No request/runtime dependency; File lifetime and consumer tests |
| Admission and slot lifecycle | REQ, BOUND | NOT_ASSESSED | V04–V05, V08–V09, V24, V26; executable model and failure injection |
| Public Request and result lifetime | HANDLE, LIFE | NOT_ASSESSED | Move/consume/discard, retained results, release-build violation behavior |
| Observer attachment and retirement | OBS | NOT_ASSESSED | V06–V08; attach/publication and cancel/delivery interleavings |
| Progress and external integration | PROG, W-03 | NOT_ASSESSED | V10–V12, V23; both backends; no-busy-poll/no-lost-wake evidence |
| Public threading and memory handoff | THREAD, REQ-04 | NOT_ASSESSED | API concurrency matrix, publication review, deterministic handoff and race instrumentation |
| Cancel and effect reporting | CANCEL, ERR-02 | NOT_ASSESSED | V13–V15, V19; unsupported/retryable/coalesced control behavior |
| ThreadPool profile | BACKEND, PROD-02 | NOT_ASSESSED | Full required operation matrix; bounded workers; shared conformance; shutdown |
| io_uring profile | BACKEND, PROD-02 | NOT_ASSESSED | Shared conformance; partial-submit/control-CQE faults; real-kernel configuration |
| Bounded pipeline scope | HOST-02, W-02 | NOT_ASSESSED | V18–V19; early return/exception cleanup and scope capacity |
| Shutdown and destruction | SHUT | NOT_ASSESSED | V20–V23; poison/retirement; retained-result and host-detachment checks |
| Core/public build boundary | ARCH, MIG | NOT_ASSESSED | V25; installed headers and standalone core-only consumers |
| Optional stackful host | HOST-01, HOST-03, W-04 | NOT_ASSESSED | If shipped as supported: task failure/stop/unwind, single-owner driver, bounded resources |
| Legacy and compatibility retirement | MIG-02 | NOT_ASSESSED | Consumer audit; replacements usable; old Completion/public surfaces explicitly retired |
| Windows/macOS, multi-worker host, coroutine adapter | PROD-02 | DEFERRED_OUT_OF_SCOPE | Root amendment before supported-v1 claims |
| Registered/vectored/direct-I/O/zero-copy extensions | PROD-03, LIFE-03 | DEFERRED_OUT_OF_SCOPE | Workload and root amendment; no inheritance from an existing header |

NOT_ASSESSED does not imply the baseline has no useful implementation or evidence.
It prevents partial or older evidence from silently becoming certification of
the new target. The implementing PR records the precise source/evidence.

## Evidence record template

For each reviewed slice, record:

| Field | Required content |
|---|---|
| Requirement scope | Exact IDs and accepted workload; explicit exclusions |
| Implementation | Commit SHA, code paths and public API/configuration |
| Change | Current behavior, target behavior and compatibility impact |
| Semantic/regression evidence | Test names/commands, result and output artifact or durable CI link |
| Protocol/model evidence | Model-to-C++ mapping, explored bounds, safety/liveness properties and assumptions |
| Publication/thread evidence | Synchronization argument and relevant instrumentation/configuration |
| Backend/kernel evidence | Backend, compiler, kernel/liburing, filesystem, injected faults and limitations |
| Status | New status and remaining gaps; never close a required slice with skipped tests |

## Phase gates

Use MIG-01 for the normative sequence and VERIFY-04 for named counterexamples.
Implement evidence with the changed protocol. Phase B already includes shutdown
and observer-reference obligations even though full integration closes later.

1. A: semantic oracle and direct path.
2. B: core, handles, publication/reclaim and teardown skeleton.
3. C: host-neutral observation and persistent progress integration.
4. D: bounded scope and optional host cleanup.
5. E: required operations, cancellation, both backend profiles and shutdown closure.
6. F: compatibility/public-surface retirement after consumer migration.
7. G: measured optimization preserving verified obligations.

Release evidence must enumerate actual supported configurations and pass W-01,
W-02, W-03, and W-04 for any supported optional host. This file is intentionally
not a claim that those gates have already passed.
