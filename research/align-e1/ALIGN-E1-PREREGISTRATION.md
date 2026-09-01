# ALIGN-E1 — small/medium-chunk application materiality (#268)

Execution authority: issue #268 (roadmap #259; ALIGN-E0 #265 CLOSED as
completed; evidence record merged as PR #266 at 7092554f).

Status: **FROZEN** (preregistered BEFORE any formal measurement). Allowed
changes after this point: additive AMENDMENTs only (appended, never
rewritten). The scientific question, matrices, modules, workload, metrics,
same-work contract, materiality rule, sweet-spot definitions, regime-map
rule, and production stop gates are frozen below.

> **AMENDMENT 1 (2026-09-01, appended BEFORE the sweep; after
> `aligne1-validate-native-1`, which ran the original 512 MiB and caught a
> harness bug — the driver's perf invocation dropped the `-e` event list,
> so every validation run recorded zero perf counters and failed the
> perf-availability gate. The failed session is retained as immutable
> evidence; the harness fix is recorded in the same commit as this
> amendment).**
>
> Total data per run is reduced from 512 MiB to **128 MiB**
> (134 217 728 B) — UNIFORMLY for every cell and every module. Reason: at
> 512 MiB the 4K/6K/8K d1..d8 `engine` cells take ~14–15 s per run (the
> production ThreadPoolBackend at workers=1 has a per-op fixed cost of
> ~50–110 µs at 4K–16K chunks; the production CLI default workers=1 is
> confirmed in `apps/sluice-copy/cli_parse.hpp`), which makes the 840-run
> sweep a ~4+ hour campaign on this host. **Everything else is unchanged**:
> the same-bytes-across-compared-arms-in-one-cell contract, the 4K..64K +
> 1 MiB chunk matrix, depth {1,2,4,8}, the three modules, all metrics,
> same-work gates, materiality/regime/verdict rules, R=7 seeded rounds.
> All references to 536 870 912 B below are amended to 134 217 728 B.

## 1. Question

Does ALIGN-E0's native Linux READ per-op alignment micro-cost (the +16
exposed-pointer penalty, 1.14x–1.42x at 4K–64K in the sync microbench,
REPRODUCED in kind but small) surface as MATERIAL in a realistic
application-level READ + WRITE file copy at 4K–64K chunk sizes, and where
are the chunk-size sweet spots / crossovers / regime boundaries for the
current production copy engine?

This campaign does NOT re-measure the alignment threshold. Alignment is a
single application-materiality treatment using ALIGN-E0's earned
tested-effective geometry (64 B; 32 B was the minimum tested effective
separation, 64 B the amplifier's best arm).

## 2. Modules / path interface (B5)

Three modules share ONE application-copy algorithm (the ALIGN-E0 amplifier
replica of the production Version B pipeline, kept in lockstep with
`apps/sluice-copy/copy_task.cpp`); the ONLY delta between modules is the
chunk-buffer geometry:

| module | buffer geometry | exposed alignment |
| --- | --- | --- |
| `engine` | production `run_pipelined_copy_with_backend` (ThreadPoolBackend), production `std::vector<std::byte>` slot storage — production code, read-only | allocator geometry (glibc) |
| `replica-natural` | same pipeline algorithm; slot storage = plain `malloc(cap)` | allocator geometry (glibc) |
| `replica-aligned` | same pipeline algorithm; slot storage = over-allocated page-aligned block, exposed pointer `round_up(base, 64)` | 64 B (page offset 0) |

Same semantics / same bytes / same op count / same depth / same worker
topology / same hash validation across all three modules in every cell.
Per-slot exposed address residual (mod 64) is recorded in the raw output
for the replica modules; the engine's internal slot addresses are not
observable (production code owns them) — `engine` vs `replica-natural`
fidelity is measured by time ratio, not by address.

Explicitly OUT for this campaign (prohibited): registered buffers, fixed
buffers, copy_file_range, splice, SIMD-transform, O_DIRECT, and any
alignment arm beyond the single 64 B treatment. No 128/256/512/1024/2048/
4096 arms — this is not a threshold campaign.

## 3. Workload (B7)

- READ + WRITE file copy of a fixed **512 MiB** (536 870 912 B) source into
  a per-run `O_TRUNC` destination.
- Source: deterministic pseudo-random bytes (the ALIGN-E0/TAX-0 generator:
  4 KiB splitmix64 master block, seed `0xE1E1E1E121212121`), generated once
  per session by `--generate`; incompressible.
- Buffered (page-cache) I/O, warm page cache, native Linux primary host
  (same bare metal as ALIGN-E0 native: see environment record).
- Fail-closed integrity: driver computes `sha256` of src once (cached) and
  of dst after every run; `dst_hash != src_hash` fails the run closed.
- Same total bytes in every run and across compared arms in every cell.

## 4. Matrices (B3, B4)

- **Chunk sweep** (primary x-axis): 4096, 6144, 8192, 12288, 16384, 24576,
  32768, 49152, 65536 bytes (4K, 6K, 8K, 12K, 16K, 24K, 32K, 48K, 64K).
- **Historical reference point** (excluded from materiality/regime
  analysis, included in sweet-spot analysis as the throughput ceiling
  reference): 1 048 576 bytes (1 MiB).
- **Depth sweep**: 1, 2, 4, 8 (pipeline depth — pre-submitted read window,
  exactly the ALIGN-E0 amplifier depth semantics). Workers = 1 (production
  CLI default) for every module and every cell → same worker topology.
- **Modules**: engine, replica-natural, replica-aligned(64).
- Cell count per round: 10 chunks × 4 depths × 3 modules = 120 cells.

## 5. Repetitions / ordering (B9)

- **R = 7 rounds.** In every round the 120 cells run in a fresh seeded
  Fisher–Yates permutation (seed = `0xE1E1E1E121212121 + round`), so module
  order is interleaved and no module block can drift against another in
  time.
- Per-cell statistics over n = 7 samples: **median** and **MAD** (median
  absolute deviation). MAD overlap is NEVER treated as significance; the
  materiality rule below requires both a ratio threshold AND robust scale
  separation.
- One run = one invocation of the bench under `perf stat -x, -e
  instructions:u,cycles:u,task-clock`. Wall time is measured in-process
  (`steady_clock`) around the FULL engine span (Runtime
  build/start/submit/wait/drain/join + copy + construct for replicas), so
  perf attachment cost affects all modules uniformly and never enters the
  timed span.

## 6. Metrics (B8)

Per run (raw): `total_ns` (wall, full engine span), `engine_ns`, `construct_ns`
(replicas), `bytes_copied`, `read_ops`, `write_ops`, `short_writes`,
`instructions:u` (perf), `cycles:u` (perf), `task-clock` (perf), CPU time
`getrusage` utime+stime delta, per-slot exposed address residuals, gate
bits, `dst_sha256`.

Per cell (summary): `total_ns` median+MAD, MiB/s (from total_ns), wall per
chunk = total_ns/chunks, `instructions` total (sum over the run's perf
value), instructions/byte, instructions/chunk, CPU-time median, gate
results.

PMU rule (frozen): `instructions:u` is the quantitative instruction pair
(stable on this host per ALIGN-E0 B.6). `cycles:u` is recorded but
**DEMOTED by default** — ALIGN-E0 showed negative per-op double
-differences on this host (turbo/frequency scaling between process runs).
The validation session runs a 3-rep consecutive stability probe; only if
the probe shows a positive per-op double-difference at both {4K, 64K} × d1
for the two replica modules is cycles upgraded to secondary evidence. No
IPC claims are made with demoted counters.

## 7. Same-work contract (fail-closed)

Bench-side gates (any violation → exit 3, semantic failure, run fails
closed):

```
bytes_copied == 536870912
write_ops   == ceil(536870912 / chunk)
read_ops    in [ceil(536870912 / chunk), ceil + depth]
short_writes == 0
```

Driver-side gates (any violation → run fails closed): bench exit 0; perf
exit 0; `dst_sha256 == src_sha256`; cell not already recorded.

## 8. Materiality rule (per cell, preregistered)

For chunk `c` and depth `d`, let `W_n` = 7 sampled total_ns of
replica-natural, `W_a` = 7 sampled total_ns of replica-aligned.

```
MATERIAL(c, d)  <=>  median(W_n)/median(W_a) >= 1.05
                    AND
                    median(W_a) + 1.5*MAD(W_a) < median(W_n) - 1.5*MAD(W_n)
```

(robust separation required on both sides; ratio alone is never enough).

## 9. Sweet-spot definitions (per module × depth; B11)

Over the 10 tested chunks (4K..64K + 1 MiB reference) with median MiB/s:

- `PEAK_THROUGHPUT_POINT`: the chunk with the highest median MiB/s (ties →
  smallest chunk).
- `P95_POINT`: the SMALLEST chunk whose median MiB/s >= 95% of the
  module/depth peak. The 1 MiB reference may be the peak; then P95 answers
  "how small can the chunk be while keeping >= 95% of the 1 MiB ceiling".
- `KNEE_POINT`: deterministic two-segment least-squares fit of
  (log2(chunk), median MiB/s). The breakpoint is the interior data point
  (>= 2 points per segment) minimizing total SSE; reported as `KNEE_CHUNK`
  with the SSE reduction vs the single-line fit. Labeled `KNEE` only if SSE
  reduction >= 10%; else `NO KNEE (flat)`.
- `ALIGNMENT_MATERIALITY_CROSSOVER`: defined on the 4K–64K primary range
  only (see §10).

## 10. Regime map (B12)

For chunk c: `M(c)` = number of depths d in {1,2,4,8} with
`MATERIAL(c,d)` (0..4).

- `STABLE_MATERIAL_REGIME`: maximal contiguous 4K–64K chunk interval with
  M(c) >= 2.
- `ALIGNMENT_MATERIALITY_CROSSOVER`: the SMALLEST chunk c* in 4K–64K such
  that M(c') == 0 for every tested chunk c' >= c*; answered as
  "materiality disappears from c* onward".
- If no contiguous interval exists or no such c* exists, the report says
  **NO STABLE CROSSOVER LOCATED** — noisy single-cell wins are reported
  raw, never smoothed into a continuous interval.

## 11. Engine fidelity (B15)

Fidelity holds at cell (c,d) iff `0.98 <= median(W_engine)/median(W_natural)
<= 1.02`. Overall: `ENGINE_REPRODUCES_REPLICA_NATURAL` iff fidelity holds
in >= 75% of the 40 cells. This detects drift between the production
engine and the research replica; it also answers "the production engine's
own geometry behaves like the natural replica".

## 12. Verdict rules (B13, B14)

Verdict vocabulary (exactly one, in priority order):

1. If MATERIAL at >= 6 of the 9 primary chunks AND at >= 3 depths (broad
   materiality) → **APP-MATERIAL — SMALL/MEDIUM REGIME**.
2. Else if a STABLE_MATERIAL_REGIME exists and the crossover is located →
   **REGIME-SPECIFIC — CROSSOVER LOCATED** (report both).
3. Else if MATERIAL(c,d) is false at EVERY primary chunk × depth →
   **MICROBENCH-ONLY — NOT APPLICATION MATERIAL**.
4. Else → **MIXED — NEED ONE TARGETED DIAGNOSTIC**.

Production stop gates (frozen):

```
If NOT APPLICATION-MATERIAL (verdict 2/3/4 without a stable material
regime): DO NOT create a production alignment knob, DO NOT create an
aligned storage abstraction, DO NOT continue kernel archaeology for
production purposes on this thread (#267 may continue as independent
methodology research, decoupled from production Sluice).

Authorization ceilings for ALL verdicts:
  PRODUCTION ALIGNMENT KNOB AUTHORIZED: NO
  RUNTIME ADAPTATION AUTHORIZED: NO
  REGISTERED BUFFER: NO
  SIMD: NO
This issue performs NO production implementation.
```

## 13. Environment / evidence rules

- Native Linux bare metal (same host as ALIGN-E0 native sessions; recorded
  per session in `environment.json`: kernel, CPU/model/flags/SMT cache
  line, page size, filesystem and mount options, RAM, glibc, clang,
  xmake, perf, git HEAD + dirty state, bench binary sha256).
- Sessions are immutable: `results/<session-id>/{environment.json,
  manifest.json, gates.json, notes.md, summary.csv, summary.json, raw/}`.
  Raw evidence = ONE append-only `runs.jsonl` (one JSON object per run,
  values preserved) + ONE `perf.csv` (perf `-x,` lines) — no per-run tiny
  file trees.
- Claim vocabulary from ALIGN-E0 applies: DIRECTLY MEASURED / CAUSALLY
  ISOLATED / INFERRED / UNRESOLVED; "no effect" is never claimed — only
  "no consistent material effect established".
- Generated artifacts (plots, derived tables) must have a generator and
  default to SVG only.

## 14. Structure commitment (B16.5)

One new top-level campaign directory `research/align-e1/` containing
README.md, preregistration, report, `scripts/` (driver + plot generator),
`results/` (immutable sessions, one dir per session), `plots/` (flat,
metric-depth named SVGs). The bench source lives in the repository's
unified `bench/` location (`bench/align_e1_bench.cpp`) wired in
`xmake/benchmarks.lua`, rebuilding ALIGN-E0's amplifer convention
(`add_deps(sluice_core, sluice_async)`, `apps/sluice-copy` include path).
No duplicated util trees; no per-chunk/per-depth/per-module directory
trees; dimensions live in CSV/JSON columns.