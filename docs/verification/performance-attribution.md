# Performance Attribution — Ladder, Tooling, and the grep Round 1 Case Study

Status: **Active**. This document is the CONCRETE artifact: the attribution
ladder definition, the grep workload matrix, runner usage, and the round-1
evidence. The governing methodology (funnel, scaling signatures,
drill-down, economics, placement, promotion rules) lives in
[`performance-engineering.md`](performance-engineering.md); this page
applies it and cross-links back. Findings feed
[`docs/applications/performance-feedback-ledger.md`](../applications/performance-feedback-ledger.md).

Every performance claim in this repository must answer one question:

> **Which layer owns this cost?**

## Ownership labels

Final attribution uses a small label set (methodology §3):

| Label | Owns |
|-------|------|
| **APP** | algorithm, data structure, matcher/parser, line splitting, copy/materialization, buffering, batching, output formatting |
| **CORE** | public I/O abstraction overhead, submit path, RequestArena, Completion, scheduler, wait/wake, backend dispatch, syscall interaction, ThreadPoolBackend |
| **PLATFORM / ENVIRONMENT** | page cache state, filesystem, host virtualization (WSL), non-runtime cold state |
| **BENCHMARK_ARTIFACT** | Debug timing, mislabeled cache state, asymmetric warming/output, instrument bias |

**BOUNDARY** is a diagnostic-only domain ("the App using the Core poorly,
or the Core forcing a poor usage pattern?"); every BOUNDARY finding must
eventually resolve to one of the four labels above.

The forbidden move:

```text
GNU grep 12 ms; sluice-grep 800 ms → "Sluice runtime is 65× slower"
```

An application algorithm gap is not a runtime indictment. Runtime overhead
is only the delta that REMAINS after the application layer is normalized
(same matcher, same buffers, same workload, blocking-I/O baseline vs Sluice
pipeline).

## Attribution ladder

Measured cost centers, each isolating what the previous layer does not
include. `Cost(Ln) − Cost(Ln−1)` approximates what layer n adds. The grep
ladder lives in `bench/grep_attribution_bench.cpp`; the same shape applies
to future workloads.

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

**Sluice core increment = L4 − L3** (same matcher, same buffer, same
file). **I/O increment = L3 − L2.** **Algorithm increment = L2 − L1.**

> **What L4 − L3 does and does not prove.** L4 includes the whole real
> engine shape — RuntimeBuilder/build, ApplicationRuntime lifecycle,
> start, task admission, Scheduler, submit, RequestArena,
> ThreadPoolBackend dispatch, blocking worker, `pread`, Completion,
> backend-ready, ready publication, wait/wake, reap, Fiber resume, stop,
> drain, join, destruction. L4 − L3 is therefore the measured
> **aggregate CORE increment** for the current
> workload/backend/buffer configuration. It proves that a Core-owned cost
> remains after APP normalization. It does NOT decompose that increment
> among runtime lifecycle, task admission, request submission, backend
> handoff, syscall interaction, completion publication, wait/wake, reap,
> or Fiber resume — any internal cause is a **hypothesis / candidate
> contributor requiring decomposition** (roadmap §10.8 Core Cost
> Decomposition) until measured.

## Workload matrix

Deterministic, seeded (`bench/support/grep_workloads.hpp`): one generator
serves the ladder, the on-disk CLI/competitor runs (`grep_workload_gen`),
and the runner — identical bytes across every surface.

Dimensions: pattern classes (empty, 1-byte common/rare, 3/16/64-byte,
rare-first-byte, repetitive, newline-containing), match densities
(0, ~1/MiB, 1%, 10%, every line), topologies (short/normal/long/huge
lines, binary bytes), cache state (page-cache hot in-process).

**Cache-state terminology rules** (enforced in wording, methodology §13):

- `fresh-process / runtime-cold` — a newly spawned process with an
  uninitialized runtime; says NOTHING about the page cache;
- `page-cache-hot` — workload bytes resident (in-process repeated reads);
- if page-cache state is not guaranteed, record it separately:
  `fresh-process/runtime-cold; page-cache state separately recorded`;
- **process freshness MUST NOT be used as evidence of page-cache
  coldness** (or hotness).

## Required metrics

- wall time: min / median / max over ≥5 iterations after ≥1 warmup
  (median is the true mathematical median, including the even-iteration
  case; timing-loop artifacts — ladder and CLI — record the raw
  per-iteration samples; `perf` captures one run's counters plus the
  verbatim perf output);
- throughput (GB/s) against bytes scanned;
- `perf stat` counters where available: cycles, instructions, branches,
  branch-misses, cache-misses, context-switches, cpu-migrations,
  page-faults;
- derived normalized ratios (ns/byte, cycles/request, ...) per
  methodology §7; ladder artifacts carry per-workload Core increment /
  overhead ratio / share in `derived`.

Environment recorded with every result (fingerprint): git SHA + dirty
flag and the exact `dirty_paths` (+ provenance note when dirty), build
mode (performance data is **Release only**), compiler, kernel, platform,
CPU, logical CPUs, glibc, python, WSL classification, workload
input/output filesystem mounts (via `/proc/self/mountinfo`), GNU grep /
ripgrep versions.

## Benchmark hygiene

- Release (or explicitly labeled optimized) builds only for performance
  data; Debug is for correctness.
- Same bytes for every tool compared (one seeded generator).
- Competitor output semantics are unified, not assumed: GNU grep/ripgrep
  run with `-a` (text-mode) under `LC_ALL=C`, so every tool implements the
  same line-output contract and byte-equality is machine-checked per row;
  the remaining comparison gap separates *algorithm class* from *runtime
  overhead*, it is not a leaderboard.
- Exit-code semantics are honored, not fought: 0 = match, 1 = no match
  (legitimate data for every grep-family tool — never an infrastructure
  failure), 2 = tool error (row flagged `tool_error`, invalid as
  evidence).
- Output handling is symmetric: every competitor's stdout is captured to
  its own file on the same filesystem, truncated fresh each iteration,
  hashed (md5) and size-recorded (`output_bytes` makes dense-output
  materialization cost visible); nobody writes to `/dev/null` while
  others pay for I/O.
- No benchmark cheating: no buffer inflation only for the benchmark, no
  dropped durability/cancellation semantics, no asymmetric warming, no
  safety-check removal on one side only.
- Baselines preserved as runner JSON under
  `docs/results/performance-attribution/` (never hand-created;
  structurally validated by the pre-push/CI gate); prefer rebuilding both
  SHAs on the same machine in one session over comparing numbers from
  different days. `compare` warns on material fingerprint differences
  (CPU / filesystem / build / compiler / WSL / workload config / dirty
  state) instead of silently calling the data comparable.
- CI validates that benches build and that evidence artifacts are
  structurally valid; flaky absolute-time thresholds stay out of CI.

## Promotion rule

> **No Sluice core optimization without evidence that the cost remains
> after application-level normalization.** (methodology §4: NORMALIZE APP
> — establish a competent baseline — not "optimize the app to
> theoretical perfection".)

```text
MEASURE (ladder)
→ ATTRIBUTE (APP / BOUNDARY / CORE / ENVIRONMENT / ARTIFACT per finding)
→ TEST BOUNDARY COUNTERFACTUAL
→ ISOLATE CORE (aggregate increment; decompose before naming internals)
```

Only a CORE-labeled cost that survives normalization drives a core
change; a Core candidate additionally needs Common-Tax or material
Cliff-Weakness evidence, an economics decision, and a placement decision
(methodology §8–§10). Semantic changes to concurrency protocols are out
of scope for a performance branch (the formal branch coordinates those).

## Tooling

```bash
xmake build grep_attribution_bench grep_workload_gen sluice-grep
scripts/bench/perf-attribution.py ladder --bytes 268435456 --output out.json
scripts/bench/perf-attribution.py cli   --bytes 1073741824 --output out.json
scripts/bench/perf-attribution.py perf  -- /path/to/sluice-grep pat file
scripts/bench/perf-attribution.py compare before.json after.json
scripts/bench/perf-attribution.py env        # fingerprint only
scripts/bench/perf-attribution.py self-test  # hermetic runner logic tests
scripts/bench/perf-evidence-validate.py      # artifact structure gate
```

GNU grep is invoked as `/usr/bin/grep -F -a` deliberately — a shell alias
(e.g. to ugrep) must never silently change the competitor, and `-a`
(--text) forces text-mode line-output semantics on binary workloads
(ripgrep gets the same `-a`); without it both competitors short-circuit to
"Binary file matches" and stop being comparable. All timed commands run
under `LC_ALL=C`. `perf` subcommand accepts `--requests N` to emit
per-request normalized ratios and `--exit-semantics` to declare how its
child's exit status must be classified.

## Round 1 evidence (sluice-grep, 2026-08)

Canonical artifacts: `docs/results/performance-attribution/`
(`round1-grep-v1-ladder.json` at baseline `b5657ae` — V1 per-line
matcher, measured with the same instrument as the candidate via the
measurement-instrument-only overlay described in its provenance note;
`round1-grep-v2-ladder.json`, `round1-grep-v2-cli.json`,
`round1-grep-v2-perf.json` at the candidate, clean tree). All four were
re-measured in one session with the schema-2 runner after review round 3
hardened the evidence contract: every artifact binds the measured
executable (path + sha256 + size + mtime), CLI competitors run with `-a`
text-mode parity (byte-identical outputs on every workload including
binary), and perf artifacts classify the child's exit status
(`exit_semantics`). Earlier numbers, including same-session numbers from
the development log and the pre-hardening artifacts, are superseded by
these artifacts.

Environment (from the artifacts' fingerprints): WSL2, kernel
6.18.33.2-microsoft-standard-WSL2, AMD Ryzen 7 5800H (8 logical CPUs),
Ubuntu clang 21.1.8, Release (`-O3`), glibc 2.43, workload files on tmpfs
(`/tmp`), 1 MiB read buffer, page-cache hot (in-process ladder reads).
Medians over 5 iterations after 1 warmup. The host shows up to ~2×
run-to-run variance under load; every number below is a same-session
median and the raw samples live in the artifacts.

### Ladder (256 MiB): L4 GB/s, V1 → V2 matcher

| workload class | V1 | V2 | speedup |
|---|---|---|---|
| sparse rare patterns (qz9, 1b_z, 16b, 64b, rare1st, rep) | 0.89–1.92 | 2.43–4.54 | **2.4–3.1×** |
| binary | 1.27 | 3.53 | 2.8× |
| dense/common anchor (`the` all densities, 1b_e) | 0.74–0.94 | 0.79–1.08 | ≈1.1× |
| short lines | 0.42 | 0.51 | 1.2× |
| long / huge lines | 5.53 / 5.91 | 5.58 / 5.30 | ≈0.90–1.01× (parity) |

V1's dominant gap was **APP**: per-line `std::search` + per-line state. V2
scans the chunk's complete-line region once for occurrences (anchor memchr +
memcmp), resolves lines lazily with an incremental cursor (jump for far
misses so huge-line work stays linear), and counts newlines with a
borrow-free SWAR pass fused into the cursor. Dense/common-anchor patterns
retain the emit cost of one `std::string` per matched line (API-bound) and
remain an APP follow-up (Boyer-Moore/kwset-class skip or SIMD candidate
filters, PF-004) rather than proof of runtime overhead — on the
output-dominated rows ripgrep and GNU grep stay ahead by measured 1.3–1.6×
(see CLI table); that is the documented algorithm-class gap, not runtime
evidence.

### Ladder attribution (256 MiB, sparse qz9 row, GB/s medians)

| stage | V1 | V2 |
|---|---|---|
| L0_memchr_nl | 7.2 | 7.4 |
| L1_line_std / L1_chunk_anchor | 2.0 | 24.8 |
| L2_matcher | 1.9 | 8.2 |
| L3_pread | 1.7 | 5.7 |
| L4_sluice | 1.5 | 4.2 |

**Algorithm (L2 − L1)** collapsed with the V2 scan (V1: L2 ≈ L1; V2: L2
runs at ~1/3 of the raw anchor scan, 8.2 vs 24.8 GB/s — the residual is
line-assembly and event cost, APP). **I/O (L3 − L2)** ≈ tmpfs page-cache `pread` copy
(~10 GB/s ceiling on this host, APP/PLATFORM). **Sluice core increment
(L4 − L3)** — the aggregate measure, wording per the ladder section
above — is **48–67 ms/GiB on sparse/binary rows** at 1 MiB chunks, and
larger (≈ 107–150 ms/GiB on dense-anchor rows including `1b_e`, ≈ 146
ms/GiB on short lines) where more per-line events flow through the engine. The increment
is therefore NOT a workload-independent constant; characterizing what it
scales with (requests at fixed bytes vs match-event count) is precisely
the pending Core Cost Decomposition / scaling-signature work (roadmap
§10.8, ledger PF-002). Until that runs, per-request handoff/wake/reap
decompositions of this number are hypotheses, not findings.

**Recorded anomaly (not hidden):** `normal__p_qz9__d_0` shows an
anomalously slow L3 in both the V1 and V2 same-session artifacts
(2.56 GB/s in V2 vs 5.74 GB/s for its nearest sibling `qz9__d_s`; 1.18
vs 1.65 GB/s in V1), which makes its
L4 − L3 negative (−186 ms/GiB). The row is retained in the artifacts and
excluded from increment claims pending investigation.

**Fresh-process effect:** a fresh process's FIRST engine call measures
~2× the in-process steady state on this host (fresh-process/runtime-cold;
page-cache state separately recorded — also observed on an ext4 run
during the round-1 investigation, a development-log observation, not a
canonical artifact; the committed artifacts record tmpfs only). Sluice
symbols account for <2% of sampled userspace symbols in that profile, but
this alone does not exclude Core overhead mediated through libc, kernel
scheduling, synchronization, or backend handoff; the classification as
OS/environment rests on reproducing independent of the runtime shape, not
on the symbol share.

### CLI L6 (1 GiB, median wall seconds)

Every iteration is a fresh process for EVERY tool (symmetric); workload
files are page-cache-hot by construction of the matrix (generated once,
then read repeatedly). State: fresh-process/runtime-cold per iteration,
page-cache-hot. Competitors run with `-a` (text-mode) and `LC_ALL=C`, so
all three tools implement the same output contract; outputs are
byte-identical across all three tools on **every** workload including
binary (per-tool md5 recorded, `outputs_equal` true — the validator
rejects a false value as non-comparable evidence). Exit codes 0/1
(match/no-match) occurred as data; no tool-error rows were recorded.

| workload | sluice-grep | GNU grep | rg | notes |
|---|---|---|---|---|
| qz9 sparse | 0.52 s (2.1 GB/s) | 0.11 s | 0.14 s | outputs 1.2 KB; search-dominated |
| 16b sparse | 0.55 s (1.9) | 0.11 s | 0.14 s | outputs 1.1 KB |
| `e` (naturally dense) | 2.43 s (0.44) | 1.80 s | 1.92 s | **outputs 1.07 GB ≈ input; output-materialization-dominated** |
| `the` sparse | 2.59 s (0.41) | 1.92 s | 1.73 s | outputs 901 MB |
| `the` all-lines | 2.84 s (0.38) | 2.11 s | 1.97 s | outputs 1.07 GB |
| long lines | 1.33 s (0.81) | 0.84 s | 1.00 s | outputs 1.07 GB |
| binary | 0.58 s (1.86) | 0.28 s | 0.14 s | identical 4.9 KB outputs under `-a` parity |

Dense-output reading (methodology: search work vs output/materialization
work vs filesystem work): on rows whose output approaches the input size,
every tool pays a ~1 GiB write+read materialization cost and the field
converges — on the dense/output rows GNU grep stays ~1.35× and ripgrep
~1.3–1.5× ahead, and on long lines GNU grep ~1.6×, versus ~4.7–5.2× (GNU)
and ~3.7–4.0× (rg) gaps on search-dominated sparse rows; the runner
records `output_bytes` per row so this never has to be inferred. On sparse
rows the remaining gap to GNU grep/rg is the algorithm class.

The remaining gap vs GNU grep/rg on sparse rows is the **algorithm
class** (kwset skip loop / SIMD candidate filters that do not touch every
byte) plus this host's fresh-process effect — not a decomposed runtime
measurement.

### perf stat (candidate, 1 GiB sparse qz9, 1024 × 1 MiB requests)

`round1-grep-v2-perf.json` (divisor `params.requests` = 1024; per-request
ratios in `derived`; child exit 0 under `exit_semantics` grep-family):
~425k cycles/request, ~1.7M instructions/request,
~5.8k cache-misses/request, 0.41 page-faults/request. On this host `perf` runs with user-space-only
counters (perf_event_paranoid; events reported with a `:u` modifier —
preserved verbatim in the artifact's `raw` field), so kernel-side time
(syscalls, scheduling) is NOT captured; context-switch/migration counts
read 0 for that reason, not because none occur. Diagnostic evidence only.

### Round-1 workload signature (grep)

```text
request size   : ~1 MiB reads (engine buffer)
request count  : bytes / 1 MiB (1024 per GiB)
mix            : read-only, mostly sequential
in-flight depth: ~1 (single task, serial submit/await)
Fiber count    : ~1
worker count   : ThreadPoolBackend blocking workers (default)
compute/I/O    : compute-significant (matcher dominates on sparse rows)
sensitivity    : throughput; latency-insensitive
footprint      : bounded (buffer + carry ≤ max_line_bytes)
```

This signature scopes every conclusion above: a different in-flight
depth or worker count is a different experiment (methodology §5).
