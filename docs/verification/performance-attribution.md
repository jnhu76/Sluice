# Performance Attribution Framework

Status: **Active** — first campaign (sluice-grep) completed 2026-08; the
framework is reusable for every later workload (copy/hash/tail, networking,
external-memory structures).

Every performance claim in this repository must answer one question:

> **Which layer owns this cost?**

## The two ownership layers

Only two layers may be named in an attribution:

| Layer | Owns |
|-------|------|
| **APP** (application) | algorithm, data structure, matcher/parser, line splitting, copy/materialization, buffering, batching, output formatting, traversal, codec/crypto implementation |
| **CORE** (Sluice) | public I/O abstraction overhead, submit path, RequestArena, Completion, scheduler, wait/wake, queue contention, runtime task lifecycle, backend dispatch, syscall interaction, io_uring bookkeeping, ThreadPoolBackend |

Internal measurements may subdivide (the ladder below); reports collapse to
these two plus the environment (OS/page cache/tmpfs) and benchmark artifacts
(Debug builds, cold caches mislabeled as hot).

The forbidden move:

```text
GNU grep 12 ms; sluice-grep 800 ms → "Sluice runtime is 65× slower"
```

An application algorithm gap is not a runtime indictment. Runtime overhead is
only the delta that REMAINS after the application layer is normalized
(same matcher, same buffers, same workload, blocking-I/O baseline vs Sluice
pipeline).

## Attribution ladder

Measured cost centers, each isolating what the previous layer does not
include. `Cost(Ln) − Cost(Ln-1)` approximates what layer n adds. The grep
ladder lives in `bench/grep_attribution_bench.cpp`; the same shape applies to
future workloads.

| Stage | Contents | Owns |
|-------|----------|------|
| L0_sum | byte-sum loop | raw memory-read floor |
| L0_memchr_nl | `memchr('\n')` walk | newline-scan kernel |
| L1_line_std | split lines + `std::search` per line | V1 matcher core shape |
| L1_chunk_anchor | chunk-wide anchor-memchr candidate scan | search primitive |
| L1_chunk_memmem | glibc `memmem` | libc SIMD reference |
| L2_matcher | full matcher + counting sink | line assembly + match events |
| L2e_matcher_emit | L2 + formatted output to suppressed file | + output cost |
| L3_pread_matcher | positional `pread` loop + matcher | + blocking file I/O |
| L4_sluice | real engine (Runtime + ThreadPoolBackend) | + Sluice runtime |
| L5 | alternative production backend | (out of band when one exists) |
| L6 | full CLI + stdout | + process/CLI/output policy |

**Sluice core increment = L4 − L3** (same matcher, same buffer, same file).
**I/O increment = L3 − L2.** **Algorithm increment = L2 − L1.**

## Workload matrix

Deterministic, seeded (`bench/support/grep_workloads.hpp`): one generator
serves the ladder, the on-disk CLI/competitor runs (`grep_workload_gen`), and
the runner — identical bytes across every surface.

Dimensions: pattern classes (empty, 1-byte common/rare, 3/16/64-byte,
rare-first-byte, repetitive, newline-containing), match densities
(0, ~1/MiB, 1%, 10%, every line), topologies (short/normal/long/huge lines,
binary bytes), cache state (page-cache hot in-process; cold-cache runs are
runner-level and must not be attributed to the runtime).

## Required metrics

- wall time: min / median / max over ≥5 iterations after ≥1 warmup;
- throughput (GB/s) against bytes scanned;
- `perf stat` counters where available: cycles, instructions, branches,
  branch-misses, cache-misses, context-switches, cpu-migrations, page-faults;
- derived: instructions/byte, IPC;
- RSS where memory behavior is part of the claim.

Environment recorded with every result: git SHA + dirty flag, compiler +
version, build mode (performance data is **Release only**), kernel, CPU,
core count, glibc, workload filesystem.

## Benchmark hygiene

- Release (or explicitly labeled optimized) builds only for performance
  data; Debug is for correctness.
- Same bytes for every tool compared (one seeded generator).
- Competitor runs (GNU grep, ripgrep) state that semantics differ; the
  comparison separates *algorithm class* from *runtime overhead*, it is not
  a leaderboard.
- No benchmark cheating: no buffer inflation only for the benchmark, no
  dropped durability/cancellation semantics, no asymmetric warming, no
  safety-check removal on one side only.
- Baselines preserved as JSON (`before`/`after` SHAs inside); prefer
  rebuilding at both SHAs on the same machine in one session over comparing
  numbers from different days.
- CI validates that benches build and the runner works; flaky absolute-time
  thresholds stay out of CI.

## Optimization promotion rule

> **No Sluice core optimization without evidence that the cost remains after
> application-level normalization.**

Concretely:

1. MEASURE on the ladder (which layer?);
2. ATTRIBUTE (APP / CORE / OS / BENCHMARK_ARTIFACT label per finding);
3. fix APP first;
4. re-measure; only a CORE-labeled cost that survives drives a core change;
5. every optimization commit carries before/after numbers from the same
   session;
6. semantic changes to concurrency protocols are out of scope for a
   performance branch (formal branch coordinates those).

## Tooling

```bash
xmake build grep_attribution_bench grep_workload_gen   # ladder + file gen
scripts/bench/perf-attribution.py ladder --bytes 268435456 --output out.json
scripts/bench/perf-attribution.py cli   --bytes 1073741824 --output out.json
scripts/bench/perf-attribution.py perf  -- <cmd>        # perf stat capture
scripts/bench/perf-attribution.py compare before.json after.json
```

The runner embeds the environment fingerprint and writes machine-readable
JSON plus a human table (`compare`). GNU grep is invoked as
`/usr/bin/grep -F` deliberately — a shell alias (e.g. to ugrep) must never
silently change the competitor.

## Round 1 evidence (sluice-grep, 2026-08)

Environment: WSL2, AMD Ryzen 7 5800H (8 cores), clang 21.1.8, Release
(`-O3`), glibc 2.43, tmpfs `/tmp`, 1 MiB read buffer, page-cache hot.
Medians over 5 iterations after 1 warmup, same-session where compared.
The machine shows up to ~2× run-to-run variance under host load; all
conclusions use clustered same-session readings, and every number below is
reproducible via `scripts/bench/perf-attribution.py` artifacts.

### Ladder (256 MiB): L4 GB/s, V1 → V2 matcher

| workload | V1 | V2 | Δ |
|---|---|---|---|
| sparse rare patterns (qz9, 1b_z, 16b, 64b, rare1st, rep) | 0.85–1.81 | 2.41–4.29 | **2.4–2.9×** |
| binary | 1.28 | 3.42 | 2.7× |
| dense/common anchor (`the` all densities, 1b_e) | 0.74–0.98 | 0.78–1.05 | ≈1.05× |
| short lines | 0.42 | 0.49 | 1.2× |
| long / huge lines | 4.71 / 4.95 | 5.03 / 4.84 | ≈0.98–1.07× |

V1's dominant gap was **APP**: per-line `std::search` + per-line state. V2
scans the chunk's complete-line region once for occurrences (anchor memchr +
memcmp), resolves lines lazily with an incremental cursor (jump for far
misses so huge-line work stays linear), and counts newlines with a
borrow-free SWAR pass fused into the cursor. Dense/common-anchor patterns
retain the emit cost of one `std::string` per matched line (API-bound) and
remain an APP follow-up (Boyer-Moore/kwset-class skip or SIMD candidate
filters) rather than proof of runtime overhead — GNU grep/rg still beat us
2–4× on every row, which is the documented algorithm-class gap.

### Ladder attribution (256 MiB, sparse qz9 row, GB/s)

| stage | V1 | V2 |
|---|---|---|
| L0_memchr_nl | 7.3 | 11.1 (SWAR path in matcher) |
| L1_line_std / L1_chunk_anchor | 2.06 | 20.3 |
| L2_matcher | 1.91 | 7.6 |
| L3_pread | 1.59 | 5.4 |
| L4_sluice | 1.48 | 4.3 |

**Sluice core increment (L4 − L3)** ≈ constant ~40–45 ms/GiB at 1 MiB
chunks (submit/await/reap + blocking-pool handoff ≈ 2 context switches per
chunk) — measured CORE cost, invisible until APP is fast. **I/O (L3 − L2)**
≈ tmpfs page-cache pread copy (~10 GB/s ceiling on this host). **Algorithm
(L2 − L1)** collapses with the V2 scan. Cold-first-run in a fresh process
(CLI shape) measures ~2× the steady-state engine on this host — read-path
page faults + host load (OS/environment class, reproduced on ext4 too;
Runtime symbols stay <2% even cold).

### CLI L6 (1 GiB, median wall seconds)

sluice-grep cold first-run vs GNU grep `-F` vs ripgrep `-F`; outputs are
byte-identical across all three tools on every text workload (runner records
per-tool output md5s), except binary inputs where GNU grep/rg short-circuit
("binary file") — a documented semantics difference, not a defect.

| workload | sluice-grep | GNU grep | rg |
|---|---|---|---|
| qz9 sparse | 0.65 s (1.7 GB/s) | 0.15 s (7.0) | 0.17 s (6.4) |
| 16b sparse | 0.60 s (1.8) | 0.15 s (7.4) | 0.15 s (7.1) |
| `e` (naturally dense) | 2.66 s (0.40) | 1.71 s (0.63) | 1.50 s (0.71) |
| `the` all-lines | 3.03 s (0.35) | 1.93 s (0.56) | 1.61 s (0.67) |
| long lines | 1.34 s (0.80) | 0.79 s (1.37) | 0.70 s (1.54) |
| binary | 1.10 s (0.98) | 0.002 s (short-circuit) | 0.003 s (short-circuit) |

The remaining gap vs GNU grep/rg on sparse rows is the **algorithm class**
(kwset skip loop / SIMD candidate filters that do not touch every byte)
plus this host's cold-first-run effect — not runtime overhead (the Runtime
symbols stay <2% even on cold runs).
