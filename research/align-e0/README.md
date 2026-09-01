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

- READ alignment effect located and causally isolated: only the
  **16-aligned-but-not-32-aligned** exposed pointer (`base + 16`, page
  offset 16) is slow (2.2x–6.7x across the size × depth matrix);
  **32-byte alignment at any page offset is already fast** — the READ
  threshold is in (16, 32] bytes, and 4096 is NOT necessary (64 B captures
  the full benefit). instructions/op identical across arms (copy-path
  latency, not instruction count).
- WRITE: **no material alignment effect** at any size × depth × worker
  count.
- The per-op READ cost does NOT disappear with depth or overlap (threaded
  diagnostic: true per-op latency a0 4x–6.5x slower at every worker
  count). The BUF-E0 d1-material/d8-null is an application-pipeline
  overlap/ceiling phenomenon: the amplifier shows the benefit at d1
  (natural/best = 1.41x) and null at d2+.
- Exact kernel/uaccess mechanism: UNRESOLVED. Native replication is
  required to determine whether the +16 signature is a real Linux property
  or a WSL2 virtualization artifact.
