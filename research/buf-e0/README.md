# BUF-E0 — Phase 2 buffer truth (#263)

Phase 2 execution campaign of roadmap #259 (Zero-Cost Control Plane +
Explicit Data-Movement Boundary). Execution authority: issue #263.
Phase 1 (CONTROL-PLANE TRUTH, #250) is CLOSED and frozen — not reopened.

> This campaign does not authorize a production buffer abstraction. Its
> purpose is to determine whether the current per-slot
> `std::vector<std::byte>` storage creates a material cost after allocation,
> initialization, first-touch, and steady-state reuse are separated.

## Entry points

| Artifact | Purpose |
| --- | --- |
| `BUF-E0-BUFFER-LIFECYCLE.md` | Static lifecycle census (CODE FACT only) |
| `buf_e0_census.json` | Machine-readable census |
| `BUF-E0-PREREGISTRATION.md` | Frozen experiment design (+ AMENDMENT 1) |
| `BUF-E0-REPORT.md` | Final report + verdict + Phase-3 gate |
| `bench/` | (bench sources live in `bench/` at repo root, wired in `xmake/benchmarks.lua`) |
| `scripts/bufe0.py` | Session driver (run/align/amp/arena/report) |
| `results/<session-id>/` | Immutable measurement sessions |

## Verdict (see BUF-E0-REPORT.md)

STEADY-STATE STORAGE EFFECT MEASURED — page alignment of the I/O buffer
(mechanism attributed via b1a; 1.8x end-to-end at production-default
depth). BUF-F01 eager-init = cost shift, not material (amplifier
null). BUF-F02 per-slot ownership = not proven. PHASE 3 AUTHORIZED: YES
— narrowly, as a minimal alignment experiment within current per-slot
ownership, NOT a buffer-pool framework.

## Artifacts by milestone (issue #263)

- BUF-E0A — lifecycle census + preregistration
- BUF-E0B — allocation / initialization / first-touch measurements
- BUF-E0C — prefaulted steady-state + application amplifier
- BUF-E0 VERDICT

## Scope guards

Research-only storage variants; production code changed: NO. Production
`PipelineSlot` (`apps/sluice-copy/copy_task.cpp`) is the measured subject, not
a modification target. No BufferStorage / BufferPool / BufferLease / public
API change / zero-copy / O_DIRECT / allocator framework in this campaign.
