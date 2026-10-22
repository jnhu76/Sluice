# MW-HW-1 decision table

| Candidate | Verdict | Conditions / next gate |
|---|---|---|
| E1 pending-control derived count + zero-skip | **PROPOSE_UPSTREAM** (strongest; the only net-positive candidate) | owner child issue; native-Linux paired A/B (4k/64k × C{16,1024} × D{1,8} + 1 MiB control); W-03 external-loop A/B; reviewer-hardened `control_pending` transition enumeration; parity oracle + both mutants ship with the change |
| E2 direct dispatch | **NO_CHANGE_JUSTIFIED** | falsified on data (≈1.00); mediation is load-bearing for SQE-pressure FIFO and poison recovery |
| E3 publication snapshot | **NO_CHANGE_JUSTIFIED** (standalone) | ≈1.00; retain as inventory shape for a future poll() restructure |
| Submit batching/deferral | **NO_CHANGE_JUSTIFIED** | 1.000 enter/op floor at all sliding-window depths; sub-1/op requires contract/config change |
| Kernel eventfd per-COE signal suppression | **INVESTIGATE (native Linux only)** | blocked on WSL2: KERNEL_ATTRIBUTION_UNAVAILABLE; any suppression needs its own no-lost-wake proof for W-03 |
| ThreadPool owed count (sibling of H2b/E1 class) | **INVESTIGATE** | separate parity oracle (work_mtx_ domain); E1 evidence lowers the risk estimate |
| Router direct index (H1) / scope free list (H3) / core-mutex coalescing | **KEEP (refuted/unproductive)** | no new evidence; prior refutations stand |

## Answering the charter's closing question

If Sluice must cut one real executed path per logical I/O request next, it is
the **second full router scan per request — the zero-pending
`service_pending_controls_locked_` walk** (E1). It is the only remaining path
whose cost scales with configured capacity while its trigger condition
(P>0: SQE pressure plus a running cancel) is absent from ordinary traffic;
it is provably skip-not-delete (the scan is the sticky-cancel retry
mechanism); it carries the H2b precedent end to end (derived count, flags
authoritative, settlement untouched); and the measured saving (−15% at the
decisive config, −61% user time, zero syscall change) is the largest
attributable userspace win left on the board. Everything else measured at or
below noise, or is the kernel-side semantic floor (1 enter/op).

## Scope statement

No product source, public API, root spec, ADR, CI or conformance-ledger row
changed on master in this round. Evidence documents and the E1/E2/E3
worktree branches + one OPEN research PR carry the results. WSL2
exploration only: no native-Linux product performance gate is claimed.
