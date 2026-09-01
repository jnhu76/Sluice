# ALIGN-E1 — small/medium-chunk application materiality (#268)

Execution authority: issue #268 (roadmap #259). Preregistration:
`ALIGN-E1-PREREGISTRATION.md` (FROZEN; AMENDMENT 1 — 128 MiB per run —
appended BEFORE the sweep). This report describes the single frozen
sweep session `aligne1-sweep-native-1` (840 runs, 0 gate errors) plus
the validation chain.

```
VERDICT:        MICROBENCH-ONLY — NOT APPLICATION MATERIAL.
                ALIGN-E0's native READ per-op alignment micro-cost does
                NOT surface as a material effect in the 4K–64K
                application-level READ + WRITE copy workload on this
                host — including the direct +16-residue-class test at
                depth 1/2 (median natural/aligned wall ratio 1.039 @d1,
                0.998 @d2; materiality rule fails at every cell).

BASE:           7092554fed9dd4f269e7b265f9317ae1e15c5c33 (master after
                the ALIGN-E0 evidence merge PR #266)
HEAD:           research/align-e1 (see git log; sweep ran at 468208df)
BRANCH:         research/align-e1
EXECUTION ISSUE:#268
DRAFT PR:       created after this report (DRAFT, DO NOT MERGE)

PRODUCTION ALIGNMENT KNOB AUTHORIZED: NO
RUNTIME ADAPTATION AUTHORIZED: NO
REGISTERED BUFFER: NO
SIMD: NO
PRODUCTION CODE CHANGED: NO (bench + driver only)
```

## Environment

Bare metal (NOT WSL/NOT container): Fedora 44, kernel
7.1.9-200.fc44.x86_64, Intel Xeon E5-2666 v3 @ 2.90 GHz (10C/20T, SMT),
cache line 64 B, page 4096, RAM 62 GiB, btrfs (compress=zstd:1) on SATA
SSD, glibc 2.43, clang 22.1.8 Release (warnings-as-errors PASS), perf
7.1.9 (paranoid=2), xmake 3.0.9. Same host as the ALIGN-E0 native
sessions.

## Modules (identical algorithm, only buffer geometry differs)

| module | geometry | exposed alignment |
| --- | --- | --- |
| `engine` | production `run_pipelined_copy_with_backend` (ThreadPoolBackend, workers=1 — the production CLI default) | allocator (std::vector) |
| `replica-natural` | verbatim pipeline replica, plain `malloc(cap)` slots | allocator; **recorded residual 16 (mod 32) at d1/d2, 0 at d4/d8** |
| `replica-aligned` | same replica, page-aligned over-alloc block, exposed = round_up(base, 64) | 64 B (residual 0 mod 64) |

Workload: 128 MiB (AMENDMENT 1) READ + WRITE copy, deterministic
pseudo-random src, per-run `O_TRUNC` dst, driver-verified dst sha256 ==
src sha256 every run (fail-closed). Chunks: 4K, 6K, 8K, 12K, 16K, 24K,
32K, 48K, 64K + 1 MiB reference. Depths: 1, 2, 4, 8. R = 7 interleaved
seeded rounds; per-cell median + MAD (n = 7).

## Sessions

| Session | Runs | Status |
| --- | --- | --- |
| `aligne1-validate-native-1` | 48 | INVALID (harness bug 1: perf `-e` dropped; 512 MiB era) — retained immutable |
| `aligne1-validate-native-2` | 48 | INVALID (harness bug 2: perf `-o /dev/null`) — retained immutable |
| `aligne1-validate-native-3` | 48 | INVALID (harness bug 3: `--file-bytes` not passed) — retained immutable |
| `aligne1-validate-native-4` | 48 | INVALID (harness bug 4: perf `-x,` field order) — retained immutable |
| `aligne1-validate-native-5` | 48 | INVALID (harness bug 5: unit-field scan) — retained immutable |
| `aligne1-validate-native-6` | 48 | GREEN (0 gate errors); cycles:u DEMOTED per prereg §6 probe |
| `aligne1-sweep-native-1` | 840 | GREEN (0 gate errors) — **the frozen sweep evidence** |

## Throughput sweet spots (median MiB/s; prereg §9)

Peak is the 1 MiB reference point at every module × depth; the 95%-point
(= the smallest chunk within 95% of that peak) is therefore 1 MiB
everywhere — no 4K–64K chunk reaches 95% of the 1 MiB-chunk throughput on
this engine path. Two-segment knee fit: NO KNEE at any module × depth
(SSE reduction < 10%; the curve is a monotone ramp through 4K–64K with a
final jump at 1 MiB, not a two-regime shape).

```
depth 1 (median MiB/s):            engine  natural  aligned
  4K                                56.7     55.7     57.4
  8K                               111.9    110.6    114.9
 16K                               184.4    176.0    184.0
 32K                               192.4    217.0    254.3
 64K                               267.3    257.3    269.4
  1M (reference)                   624.5    652.9    647.2

depth 2:
  4K                               139.1    158.6    144.9
  8K                               272.8    247.5    216.1
 16K                               361.9    366.6    352.6
 32K                               487.5    502.3    498.0
 64K                               557.3    592.6    591.3
  1M (reference)                   873.2    888.9    887.7

depth 4:
  4K                               157.5    133.2    139.0
  8K                               273.2    251.4    246.9
 16K                               409.3    448.5    333.0
 32K                               493.0    491.8    600.6
 64K                               562.5    623.5    588.0
  1M (reference)                   856.9    908.9    908.6

depth 8:
  4K                               148.3    151.7    153.2
  8K                               250.0    265.1    247.6
 16K                               439.0    399.8    326.8
 32K                               550.5    482.8    515.8
 64K                               584.8    591.6    591.2
  1M (reference)                   831.2    896.3    867.9
```

Regime observation (derived from the tables, not a fitted claim): the
engine's per-op fixed cost (~35–100 µs/op at workers=1, measured as
wall/chunk 69 µs @4K d1 up to 234 µs @64K d1) dominates below ~64K; the
chunk-size lever moves throughput ~4–10x across 4K→64K and another ~2.4x
from 64K → 1 MiB. This is a chunk-size regime fact for the CURRENT copy
engine, independent of alignment.

## Alignment application materiality (prereg §8, §10)

Per (chunk, depth): MATERIAL iff median(natural wall)/median(aligned
wall) ≥ 1.05 AND 1.5·MAD robust separation on both sides.

```
ratios natural/aligned (wall), by depth:
chunk     d1     d2     d4     d8
 4K     1.031  0.913  1.043  1.010
 6K     0.922  1.032  0.982  1.063
 8K     1.039  0.873  0.982  0.934
12K     0.912  1.142  0.917  1.012
16K     1.046  0.962  0.742  0.817
24K     0.887  0.950  0.919  1.082
32K     1.172  0.991  1.221  1.068
48K     1.186  1.021  0.907  1.167
64K     1.047  0.998  0.943  0.999
1M      0.991  0.999  1.000  0.968
median  1.0388 0.9979 0.9819 1.0117
```

**M(c) = 0 at every chunk** (no cell passes the ratio + robust-separation
rule; the d4-16K 0.742 and d4-32K 1.221 single-cell swings are isolated
noise windows with no neighboring-cell consistency — identified, not
interpreted).

The depth-1/depth-2 rows are the strongest direct evidence: the
replica-natural slot addresses were DETERMINISTICALLY in the ALIGN-E0
tested-slow class (exposed residual 16 mod 32) in every round at d1/d2,
while replica-aligned sat at residual 0 — and the median wall ratio is
still 1.039 (d1) / 0.998 (d2): the sub-µs READ uaccess tax (measured at
1.14–1.42x per READ op in the sync microbench) is absorbed by the
engine's ~35–100 µs per-op cost and the read+write pipeline. The d1 32K/
48K cells reach 1.17–1.19 but fail the robust-separation leg.

Regime map (prereg §10):

```
STABLE MATERIAL REGIME:        NONE (M(c)=0 for all 4K–64K chunks)
ALIGNMENT MATERIALITY
CROSSOVER:                     NOT LOCATED — no materiality anywhere in
                               the tested range ("disappears from the
                               first chunk" = never present)
```

## CPU cost

- instructions/byte: identical across the three modules at every
  (chunk, depth) to <0.1% (e.g. d1: 3.8093 @4K, 0.2579 @64K,
  0.0359–0.0360 @1M for all modules) — the algorithm bytes are the same;
  geometry changes no instruction count.
- instructions/byte falls with chunk size (loop/await amortization) —
  the CPU-efficiency crossover of the CURRENT engine favors large
  chunks, independent of alignment.
- cycles:u: DEMOTED (prereg §6 probe; negative consecutive per-op
  double-differences, frequency scaling) — no IPC claims.

## Engine fidelity (prereg §11)

engine/replica-natural median wall ratio: median 1.018, range 0.801–
1.166, 22 cells slower / 18 faster (no systematic direction). The
preregistered 2% fidelity band is NOT met (17.5% of 40 cells held) — on
this host the cell-to-cell dispersion (±10–20%, allocator-phase and
machine noise) exceeds the 2% band; the engine and the verbatim replica
agree structurally (same algorithm, same instructions/byte) but not at
the 2% quantitative level. Conclusion: structural fidelity holds;
quantitative fidelity band not resolvable on this host.

## Evidence taxonomy

- **DIRECTLY MEASURED**: 840-run sweep, 0 gate errors, dst hash == src
  hash every run; per-cell median+MAD wall; slot residuals per run;
  instructions/byte per cell.
- **CAUSALLY ISOLATED**: the ONLY variable between replica modules is the
  exposed buffer geometry (verbatim algorithm replica, same page set
  semantics, same bytes/ops/depth/workers); at d1/d2 the natural module
  is provably in the ALIGN-E0 +16 slow residue class while aligned is at
  residual 0 — and no material separation appears.
- **INFERRED**: absorption of the sub-µs READ tax under the engine's
  per-op cost (~35–100 µs) and the read+write pipeline (the numerical
  claim "tax << per-op cost" is measured; the attribution of the
  absorption mechanism is interpretation).
- **UNRESOLVED**: exact kernel/uaccess mechanism of the +16 micro-cost
  (#267, methodology research, decoupled from production Sluice);
  whether a different engine shape (lower per-op overhead, direct-IO)
  could surface the tax — out of this campaign's matrix.

## Limitations

- 128 MiB per run (AMENDMENT 1) — the 4K–64K engine cells are per-op-cost
  limited; total-bytes parity across arms in every cell holds.
- workers=1 (production CLI default): depth is the pre-submitted read
  window, not worker parallelism; real multi-worker overlap was ALIGN-E0's
  threaded diagnostic domain.
- Machine noise: isolated single-cell ratio swings (d4-16K, d4-32K)
  could not pass the robust materiality rule; single-cell ratios beyond
  ±20% are not interpretable on this host.
- cycles:u demoted; no secondary PMU campaign.

## FINAL VERDICT

```
MICROBENCH-ONLY — NOT APPLICATION MATERIAL

ALIGN-E0's native READ per-op alignment micro-cost does NOT surface as
a material application-level effect in the 4K–64K READ + WRITE copy
workload: M(c)=0 at every chunk x depth, including the direct
+16-residue-class test at d1/d2 (median ratio 1.039/0.998). The 1 MiB
amplifier null from ALIGN-E0 extends across the whole 4K–64K regime.

PER PREREG §14 (stop gates):
  DO NOT create a production alignment knob
  DO NOT create an aligned storage abstraction
  DO NOT continue kernel archaeology for production purposes on this
  thread (#267 may continue as independent methodology research,
  decoupled from production Sluice)
```

## REPOSITORY STRUCTURE (prereg §14 / B16.5 gate)

```
new top-level campaign dirs:  research/align-e1/ (ONE)
source files added:           bench/align_e1_bench.cpp,
                              research/align-e1/scripts/align_e1.py,
                              research/align-e1/scripts/plot_align_e1.py,
                              xmake/benchmarks.lua (one target block)
immutable evidence sessions:  7 (6 validate incl. 5 retained INVALID
                              harness-bug attempts + 1 sweep)
raw evidence file count:      2 per session (runs.jsonl + perf.csv) —
                              NO per-run tiny files
derived plot count:           12 SVG (throughput / instr-per-byte /
                              alignment-ratio x depths 1,2,4,8)
total PR changed files:       see git diff --stat in the PR

DIRECTORY STRUCTURE: CLEAN
```

## FINAL STATUS

Draft PR (research/align-e1): DO NOT MERGE — stop for adversarial review
of the sweep evidence. Issue #268 remains OPEN until the review.