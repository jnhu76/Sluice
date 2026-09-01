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
on native Linux does it reproduce?
```

Governing rule: OBSERVED EFFECT → ALIGNMENT THRESHOLD →
SIZE/DEPTH/DIRECTION CROSSOVER → NATIVE REPLICATION → ONLY THEN a
production alignment decision. "4096 worked → make all buffers page
aligned" and "alignment helps d1 → alignment universally faster" are both
prohibited.

## Entry points

| Artifact | Purpose |
| --- | --- |
| `ALIGN-E0-PREREGISTRATION.md` | Frozen experiment design (arms, matrices, same-work, metrics, gate; AMENDMENTs append-only) |
| `ALIGN-E0-REPORT.md` | Final report + verdict + evidence taxonomy |
| `bench/` | Bench sources (`align_e0_bench` microbench, `align_e0_amp_bench` amplifier; wired in `xmake/benchmarks.lua`) |
| `scripts/aligne0.py` | Session driver (validate/run/amp/report) |
| `results/<session-id>/` | Immutable measurement sessions |

## Baseline (start of Phase 3)

| Field | Value |
| --- | --- |
| BASE SHA | `312ede532f66236b8e1723368d3d4ab6bbb7476f` (master after #264) |
| Branch | `research/align-e0` |
| git status at start | clean |
| Host | WSL2 (`Hu`, kernel `6.18.33.2-microsoft-standard-WSL2`) — VIRTUALIZED, not native Linux |
| CPU | AMD Ryzen 7 5800H (8 logical CPUs, SMT) |
| Page size | 4096 |
| Filesystem | ext4 on WSL2 virtual block device (`/dev/sdd`) |
| Compiler | clang 21.1.8 / g++ 15.2.0 |
| libc | glibc 2.43 |
| Build | xmake release, Linux x86_64 |

Per-session `environment.json` captures the exact state (HEAD/branch/dirty,
binary sha256, kernel, tools) for every immutable session.

## Environment classification

WSL2 = `QUALIFIED_BUT_VIRTUALIZED`, `ENVIRONMENT-LIMITED`: same-host causal
comparison and harness validation only. **Native Linux (real kernel,
non-WSL, x86-64, warm page cache) is the mandatory primary environment for
any final Phase-3 verdict.** No native host is reachable from this workspace;
therefore the Phase-3 verdict is `ENVIRONMENT-BLOCKED` with
`PRODUCTION ALIGNMENT CHANGE AUTHORIZED: NO` until a native Linux run of the
frozen matrix exists. The harness is native-ready (plain pread/pwrite, no
WSL-specific behavior) and the frozen matrix is exactly what a native session
should run.

## Scope guards

- Production code changed: NO. All arms are research-only bench code.
- No BufferStorage / BufferPool / BufferLease / registered-fixed buffers /
  provided buffers / O_DIRECT / splice / copy_file_range / reflink /
  zero-copy / SIMD / custom memcpy / NUMA / huge pages / scheduler change.
- One Draft research PR; DO NOT MERGE; stop for adversarial review after the
  verdict. Even a YES verdict puts no production code on this PR.

## Verdict so far (WSL2 qualified evidence only, see ALIGN-E0-REPORT.md)

Phase-3 verdict: **ENVIRONMENT-BLOCKED** — native Linux (the mandatory
environment gate) is not reachable from this workspace; `PRODUCTION
ALIGNMENT CHANGE AUTHORIZED: NO`. The harness is native-ready and the frozen
matrix is exactly what a native session must run.

WSL2-qualified findings (QUALIFIED_BUT_VIRTUALIZED, not the Phase-3 verdict):

- READ alignment effect located and causally isolated: only the **+16**
  exposed pointer (`base + 16`, page offset 16) was slow in the
  preregistered offset sweep (2.2x–6.7x across the size × depth matrix);
  offsets 0, 32, 64, …, 2048 were all fast (minflt=0). **32 B is the
  minimum tested alignment that captures the benefit** — 4096 is NOT
  necessary (64 B already captures the full benefit in the ladder). The
  observed split is consistent with a 32-byte-alignment explanation, but
  the exact threshold and residue-class rule are UNRESOLVED (16-mod-32
  offsets other than +16, e.g. +48/+80/+112, were not tested).
  instructions/op shows no material arm separation — the wall effect is
  NOT explained by an instruction-count delta.
- WRITE: **no consistent material alignment effect established** at any
  size × depth × worker count. Some isolated cells are large (1.9–2.2x)
  but noisy and lacking neighboring-cell / regime consistency; effect == 0
  is not claimed.
- The per-op READ cost does NOT disappear with depth or overlap (threaded
  diagnostic: true per-op latency a0 4x–6.5x slower at every worker
  count). MECHANISM DISAPPEARS: NO. At the application level, BUF-E0's
  d1-material/d8-null is consistent with application-level masking /
  overlap (SUPPORTED INTERPRETATION — amplifier shows the benefit at d1,
  natural/best = 1.41x, and no material separation at d2+); the exact
  application bottleneck is UNRESOLVED (writeback / page-cache ceiling /
  control-plane factors are hypotheses, not proven root cause).
- Exact kernel/uaccess mechanism: UNRESOLVED. Native replication is
  required to determine whether the +16 signature is a real Linux property
  or a WSL2 virtualization artifact.
