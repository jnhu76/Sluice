# ALIGN-E0 — Phase 3 I/O buffer alignment threshold × size × depth × direction (#265)

Phase 3 execution campaign of roadmap #259 (Zero-Cost Control Plane +
Explicit Data-Movement Boundary). Execution authority: issue #265.
Phase 2 (BUFFER TRUTH / BUF-E0, #263) is CLOSED as completed; its evidence
is merged at 312ede5 (#264).

> ALIGN-E0 is a research-only experiment. It does not authorize a production
> buffer abstraction or a production alignment change. Its purpose is to
> locate the minimum effective alignment and the workload regimes where
> alignment materially changes I/O cost.

## Mission

Advance the Phase-2 conclusion "alignment matters" into an actionable
ALIGNMENT POLICY MAP:

```
how much alignment?
for which operation size?
at which queue depth?
for READ or WRITE?
on native Linux does it reproduce?   <- answered: MIXED, see below
```

Governing rule: OBSERVED EFFECT → ALIGNMENT THRESHOLD →
SIZE/DEPTH/DIRECTION CROSSOVER → NATIVE REPLICATION → ONLY THEN a
production alignment decision. "4096 worked → make all buffers page
aligned" and "alignment helps d1 → alignment universally faster" are both
prohibited.

## Entry points

| Artifact | Purpose |
| --- | --- |
| `ALIGN-E0-PREREGISTRATION.md` | Frozen experiment design (arms, matrices, same-work, metrics, gate; AMENDMENTs append-only — none required) |
| `ALIGN-E0-REPORT.md` | Final report + verdict + evidence taxonomy (WSL2 + native) |
| `bench/` | Bench sources (`align_e0_bench` microbench, `align_e0_amp_bench` amplifier; wired in `xmake/benchmarks.lua`) |
| `scripts/aligne0.py` | Session driver (validate/run/amp/report) |
| `results/<session-id>/` | Immutable measurement sessions (6 WSL2 + 5 native) |

## Baselines

| Field | WSL2 campaign | Native campaign (Phase-3 evidence) |
| --- | --- | --- |
| BASE SHA | `312ede532f66236b8e1723368d3d4ab6bbb7476f` (master after #264) | same |
| Branch | `research/align-e0` | `research/align-e0` (native sessions @ 1936a466; merged as PR #266) |
| Host | WSL2 (`Hu`, kernel `6.18.33.2-microsoft-standard-WSL2`) — VIRTUALIZED | bare metal (NOT WSL/container): Fedora 44, kernel `7.1.9-200.fc44.x86_64` |
| CPU | AMD Ryzen 7 5800H (8 logical CPUs, SMT) | Intel Xeon E5-2666 v3 @ 2.90 GHz (10C/20T, SMT, turbo 3.5 GHz) |
| Page size | 4096 | 4096 |
| Filesystem | ext4 on WSL2 virtual block device | btrfs (compress=zstd:1) on SATA SSD (warm page-cache timed path) |
| Compiler | clang / g++ | clang 22.1.8 Release (warnings-as-errors PASS) |
| libc | glibc 2.43 | glibc 2.43 |
| Build | xmake release, Linux x86_64 | xmake release (clang), Linux x86_64 |

Per-session `environment.json` captures the exact state (HEAD/branch/dirty,
binary sha256, kernel, tools) for every immutable session; `notes.md` adds
the environment detail (mount options, device, SMT, governor, PMU
observations).

## Verdict (Phase 3 FINAL — ENVIRONMENT-BLOCKED superseded)

```
MIXED — NEED TARGETED FOLLOW-UP
PRODUCTION ALIGNMENT CHANGE AUTHORIZED: NO
```

The native Linux replication of the frozen harness completed (5 immutable
native sessions, 0 gate errors). Findings:

- **READ +16 per-op anomaly REPRODUCED in kind, not in magnitude**: on
  native the +16 exposed pointer (16-aligned, page offset 16) is the only
  slow point (offset sweep: offsets 0/32/64/…/2048 fast), but the penalty
  is 1.14x–1.42x at 4K–64K (peak 8K) vs WSL2's 2.2x–6.7x; at 1M the sync
  windows are noise, and threaded per-op shows +5–9%. Same signature,
  ~1/4–1/10 the magnitude, different size profile.
- **WRITE: null on both hosts** — no consistent material alignment effect
  (ladder, offset sweep, threaded all flat).
- **Application amplifier: NOT reproduced.** Native d1 natural/best =
  1.03x (within MAD), null at every depth; WSL2's d1 1.41x is a
  WSL2-environment effect candidate, not a native property. Per
  preregistration §9 the amplifier is the application-level boundary — no
  application-level alignment benefit is established on native.
- instructions/op arm-invariant on both hosts (2382 @4K native / 2400
  @4K WSL2; 485 102 / 484 607 @1M) — wall effects are NOT
  instruction-count deltas. `cycles:u` unreliable on both hosts (WSL2:
  virtualized counter; native: frequency scaling between process runs).
- Minimum TESTED effective alignment on native: 32 B separation / 64 B
  ladder arm (4096 NOT necessary). NOT PROVEN: the full address ≡ 16
  (mod 32) residue class (only +16 was tested in the slow class). With
  no application-level payoff and a sub-1.5x microbench effect, no
  production change is authorized.
- Residual scope is re-tracked: #267 (mechanism/environment), #268
  (application materiality).

WSL2-qualified references (not the verdict): the full WSL2 tables live in
`ALIGN-E0-REPORT.md` Part A; the side-by-side table is Part C.

## Scope guards

- Production code changed: NO. All arms are research-only bench code.
- No BufferStorage / BufferPool / BufferLease / registered-fixed buffers /
  provided buffers / O_DIRECT / splice / copy_file_range / reflink /
  zero-copy / SIMD / custom memcpy / NUMA / huge pages / scheduler change.
- One research PR (#266), merged as the Phase-3 evidence record; adversarial
  review of the evidence follows. Even a YES verdict puts no production
  code on this PR.