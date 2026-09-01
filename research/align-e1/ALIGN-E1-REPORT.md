# ALIGN-E1 — small/medium-chunk application materiality (#268)

Execution authority: issue #268 (roadmap #259). Preregistration:
`ALIGN-E1-PREREGISTRATION.md` (FROZEN; AMENDMENT 1 — 128 MiB per run —
appended BEFORE the sweep; AMENDMENT 2 — E1-C1 strict causal-isolation
control — appended BEFORE the causal-control run). This report covers:

- **PART A** — the broad application sweep `aligne1-sweep-native-1`
  (840 runs, 0 gate errors), retained immutable;
- **PART B** — the adversarial-review findings that triggered AMENDMENT 2;
- **PART C** — E1-C1, the strict causal-isolation control
  `aligne1-causal-native-1` (252 runs, 0 gate errors);
- **PART D** — final synthesis and production decision.

```
VERDICT:        MICROBENCH-ONLY — NOT APPLICATION MATERIAL.
                The strict causal-control A/B (PART C) — same
                allocation primitive, same allocation size, same
                backing-alignment policy, same ownership/lifetime
                policy, same algorithm, same workload — found NO
                material application effect of the exposed-pointer
                phase treatment in any tested 4K–64K cell at depth
                {1,2}; the broad sweep (PART A) is consistent with
                that null.

BASE:           7092554fed9dd4f269e7b265f9317ae1e15c5c33 (master after
                the ALIGN-E0 evidence merge PR #266)
HEAD:           research/align-e1 (sweep ran at 468208df; causal control
                ran at dba7fd6f)
BRANCH:         research/align-e1
EXECUTION ISSUE:#268
DRAFT PR:       #269 (DRAFT, DO NOT MERGE)

PRODUCTION ALIGNMENT KNOB AUTHORIZED: NO
RUNTIME ADAPTATION AUTHORIZED: NO
REGISTERED BUFFER: NO
SIMD: NO
ALIGNMENT IS NOT PROMOTED INTO THE Sluice CONTROL SURFACE.
PRODUCTION CODE CHANGED: NO (bench + driver only)
```

---

## PART A — Broad application sweep (`aligne1-sweep-native-1`, 840 runs)

### Environment

Bare metal (NOT WSL/NOT container): Fedora 44, kernel
7.1.9-200.fc44.x86_64, Intel Xeon E5-2666 v3 @ 2.90 GHz (10C/20T, SMT),
cache line 64 B, page 4096, RAM 62 GiB, btrfs (compress=zstd:1) on SATA
SSD, glibc 2.43, clang 22.1.8 Release (warnings-as-errors PASS), perf
7.1.9 (paranoid=2), xmake 3.0.9. Same host as the ALIGN-E0 native
sessions and as PART C (per-session `environment.json` records).

### Sweep modules (as run)

| module | geometry | exposed alignment |
| --- | --- | --- |
| `engine` | production `run_pipelined_copy_with_backend` (ThreadPoolBackend, workers=1 — the production CLI default) | allocator (std::vector) |
| `replica-natural` | verbatim pipeline replica, plain `malloc(cap)` slots | allocator; recorded residual 16 (mod 32) at d1/d2, 0 at d4/d8 |
| `replica-aligned` | same replica, page-aligned over-alloc block, exposed = round_up(base, 64) | 64 B (residual 0 mod 64) |

ADVERSARIAL FINDING (PART B): `replica-natural` and `replica-aligned`
differed in allocation/backing policy as well as in exposed address —
the sweep's replica-vs-replica comparison is therefore suggestive, not
a strict single-variable causal A/B. PART C supplies that A/B.

Workload: 128 MiB (AMENDMENT 1) READ + WRITE copy, deterministic
repeated pseudo-random 4 KiB pattern (one splitmix64 master block,
seed `0xE1E1E1E121212121`, repeated across the file), per-run
`O_TRUNC` dst, driver-verified dst sha256 == src sha256 every run
(fail-closed). Chunks: 4K, 6K, 8K, 12K, 16K, 24K, 32K, 48K, 64K +
1 MiB reference. Depths: 1, 2, 4, 8. R = 7 interleaved seeded rounds;
per-cell median + MAD (n = 7).

Sessions:

| Session | Runs | Status |
| --- | --- | --- |
| `aligne1-validate-native-1` | 48 | INVALID (harness bug 1: perf `-e` dropped; 512 MiB era) — retained immutable |
| `aligne1-validate-native-2` | 48 | INVALID (harness bug 2: perf `-o /dev/null`) — retained immutable |
| `aligne1-validate-native-3` | 48 | INVALID (harness bug 3: `--file-bytes` not passed) — retained immutable |
| `aligne1-validate-native-4` | 48 | INVALID (harness bug 4: perf `-x,` field order) — retained immutable |
| `aligne1-validate-native-5` | 48 | INVALID (harness bug 5: unit-field scan) — retained immutable |
| `aligne1-validate-native-6` | 48 | GREEN (0 gate errors); cycles:u DEMOTED per prereg §6 probe |
| `aligne1-sweep-native-1` | 840 | GREEN (0 gate errors) — the frozen sweep evidence |
| `aligne1-causal-native-1` | 252 | GREEN (0 gate errors) — E1-C1 (PART C) |

### Chunk-size sweet spots (median MiB/s; prereg §9) — with claim-hygiene labels

```
TESTED-RANGE PEAK:      1 MiB (upper boundary point of the sampled range)
P95 POINT:              1 MiB (at every module x depth)
PLATEAU:                NOT REACHED (64K -> 1 MiB still rises ~1.4-2.5x)
SWEET SPOT:             NOT LOCATED
KNEE:                   NOT LOCATED IN THE SAMPLED RANGE
```

The 1 MiB point is the PEAK only because it is the largest sampled
chunk; nothing above 1 MiB was measured, so this is an upper-bound
peak of the tested range and MUST NOT be read as a global sweet spot.
Two-segment knee fit: NO KNEE at any module x depth (SSE reduction
< 10%; the curve is a monotone ramp through 4K–64K with a final jump
at 1 MiB, not a two-regime shape).

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
application per-chunk span is tens to hundreds of microseconds
(measured wall/chunk: 69 µs @4K d1 up to 234 µs @64K d1), substantially
larger than the ALIGN-E0 sub-microsecond READ delta, and the chunk-size
lever moves throughput ~4–10x across 4K→64K and another ~2.4x from 64K
to 1 MiB. This is a chunk-size regime fact for the CURRENT copy engine,
independent of alignment. MASKING / AMORTIZATION of the READ delta by
the larger application-path cost: SUPPORTED INTERPRETATION; the exact
masking mechanism: UNRESOLVED (#267).

### Alignment application materiality in the sweep (prereg §8, §10)

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

The depth-1/depth-2 rows were the sweep's closest probe of the
ALIGN-E0 candidate geometry: in every round at d1/d2 the recorded
natural-slot geometry (exposed residual 16 mod 32) was CONSISTENT WITH
the ALIGN-E0 candidate geometry, while replica-aligned sat at residual
0 — and the median wall ratio was still 1.039 (d1) / 0.998 (d2). Scope
limits on that statement, per adversarial review:

- ALIGN-E0 directly tested (and found slow) the SPECIFICALLY TESTED
  +16 exposed/page-offset point; it did NOT establish a whole
  "address % 32 == 16" slow residue class, and this sweep did not
  re-establish one either. The natural slots' 16-mod-32 residual is a
  recorded observation consistent with the candidate geometry, nothing
  stronger.
- The natural/aligned pair also differed in allocation primitive and
  backing (`malloc(cap)` vs `posix_memalign(4096, cap+64)`), so the
  sweep's replica comparison is not causally isolated on address phase
  alone. PART C runs the strict version.

Regime map (prereg §10):

```
STABLE MATERIAL REGIME:        NONE (M(c)=0 for all 4K–64K chunks)
ALIGNMENT MATERIALITY
CROSSOVER:                     NOT LOCATED — no materiality anywhere in
                               the tested range ("disappears from the
                               first chunk" = never present)
```

### CPU cost

- instructions/byte at d1: bit-for-bit identical across the three
  modules at every chunk (e.g. d1: 3.8093 @4K, 0.2579 @64K,
  0.0359–0.0360 @1M for all modules). At d2/d4/d8 small chunks the
  instruction counts disperse (up to ~24% within-arm spread at 4K d2)
  — wait-path noise of the backend, present in ALL modules; no
  systematic direction. Geometry changes no algorithm instruction.
- instructions/byte falls with chunk size (loop/await amortization) —
  the CPU-efficiency crossover of the CURRENT engine favors large
  chunks, independent of alignment.
- cycles:u: DEMOTED (prereg §6 probe; negative consecutive per-op
  double-differences, frequency scaling) — no IPC claims.

### Engine fidelity (prereg §11) — preregistered gate outcome

```
PREREGISTERED QUANTITATIVE ENGINE FIDELITY:   FAILED
  (7 / 40 cells within the ±2% band = 17.5%; the gate required >= 75%)

SOURCE / STRUCTURAL CORRESPONDENCE:           SUPPORTED
  (the replica is the verbatim production algorithm)

INSTRUCTIONS/BYTE STRUCTURAL AGREEMENT:       SUPPORTED
  (d1 identical to <0.01% at every chunk)

QUANTITATIVE WALL-TIME EQUIVALENCE:           NOT ESTABLISHED
  (engine/replica-natural median wall ratio: median 1.018,
   range 0.801–1.166, 22 cells slower / 18 faster — no systematic
   direction, but cell-to-cell dispersion ±10–20% exceeds the band)

PRODUCTION EXTRAPOLATION FROM THIS HOST:      LIMITED
```

The frozen quantitative fidelity gate FAILED and is not re-labeled:
"same algorithmic structure" and "same instructions/byte" hold, but
wall-time equivalence at the 2% level was never established on this
host, and no production-quantitative claim is drawn from this sweep.

### PART A limitations

- 128 MiB per run (AMENDMENT 1) — the 4K–64K cells are per-chunk-span
  limited; total-bytes parity across arms in every cell holds.
- workers=1 (production CLI default): depth is the pre-submitted read
  window, not worker parallelism; real multi-worker overlap was ALIGN-E0's
  threaded diagnostic domain.
- Machine noise: isolated single-cell ratio swings (d4-16K, d4-32K)
  could not pass the robust materiality rule; single-cell ratios beyond
  ±20% are not interpretable on this host.
- cycles:u demoted; no secondary PMU campaign.
- The workload bytes are a deterministic repeated pseudo-random 4 KiB
  pattern; absolute throughput on this host may additionally be
  influenced by the current btrfs/zstd/page-cache environment. Relative
  same-work comparisons (same bytes, same ops, same session) remain the
  primary evidence.

---

## PART B — Adversarial review findings (2026-09-01)

1. **Allocation confound (the material finding).** The original
   replica-natural / replica-aligned treatments changed BOTH the exposed
   address geometry AND the allocation/backing policy (`malloc(cap)` vs
   `posix_memalign(4096, cap+64)`). The sweep's null result is therefore
   strongly suggestive but not a strict single-variable causal A/B for
   alignment/address phase. Remediation: AMENDMENT 2 (E1-C1, PART C) —
   both arms on the same `posix_memalign(4096, chunk+64)` allocation
   primitive, allocation size and backing-alignment policy, only the
   exposed-pointer phase treatment (+16 vs 0) differing, gate-verified
   per run.
2. **Residue-class over-claim.** Draft wording had said the natural
   module was "provably in the ALIGN-E0 +16 slow residue class". What
   ALIGN-E0 proved is only that the specifically tested +16
   exposed/page-offset point was slow; the sweep's recorded 16-mod-32
   residual is "consistent with the ALIGN-E0 candidate geometry", not a
   proven class membership. All current documents use the narrowed
   wording.
3. **Engine fidelity gate.** The preregistered quantitative gate
   (>= 75% of cells within ±2%) FAILED at 17.5%. The report now states
   FAILED explicitly and separates structural correspondence (supported)
   from quantitative wall-time equivalence (not established); production
   extrapolation is marked LIMITED.
4. **"Fixed cost" label.** "Engine fixed cost ~35–100 µs/op" was an
   inferred attribution; the directly measured quantity is the
   per-chunk span (tens to hundreds of microseconds). The report now
   states the measured span, marks masking/amortization as a SUPPORTED
   INTERPRETATION, and keeps the exact mechanism UNRESOLVED (#267).
5. **Sweet-spot over-claim.** The 1 MiB point is the tested-range peak
   (64K → 1 MiB still rising; nothing above 1 MiB sampled). PLATEAU NOT
   REACHED; SWEET SPOT NOT LOCATED; KNEE NOT LOCATED IN THE SAMPLED
   RANGE. The upper-bound peak is no longer described as a sweet spot.
6. **Generator description.** The workload bytes are a deterministic
   repeated pseudo-random 4 KiB pattern (one master block repeated), not
   an "incompressible" stream. Current documents use that description
   and carry the btrfs/zstd/page-cache limitation note.

---

## PART C — E1-C1 strict causal-isolation control
## (`aligne1-causal-native-1`, 252 runs; AMENDMENT 2)

### Design

Two arms, identical in every respect except the ONE variable — the
exposed pointer address phase:

```
allocation (BOTH arms):  posix_memalign(4096, chunk + 64)
                         same allocation primitive / same allocation
                         size / same backing-alignment policy / same
                         ownership/lifetime policy
arm C0 causal-phase16:   base page-aligned; exposed = base + 16
                         -> page_offset 16, mod64 16 (ALIGN-E0's
                         actually tested +16 point)
arm C1 causal-aligned64: base page-aligned; exposed = base
                         -> page_offset 0, mod64 0
```

Same algorithm (verbatim replica), same bytes, same op counts, same
chunk, same depth, same worker topology (workers=1), same 128 MiB
READ + WRITE workload, same source file, same hash validation. Matrix:
9 chunks (4K–64K) x depths {1, 2} x 2 arms x R=7 seeded interleaved
rounds (frozen prereg seed machinery) = **252 runs**. No engine, no
d4/d8, no 1 MiB, no additional arms — a causal control, not a new
sweep.

Gates (FAIL CLOSED, every run): bench exit; perf instructions present;
dst sha256 == src sha256; read/write op counts; short writes == 0;
base page_offset == 0; phase16 exposed page_offset == 16 (mod64 == 16);
aligned64 exposed page_offset == 0 (mod64 == 0).

```
RESULT:  252 / 252 runs ok, 0 gate errors.
         All 126 phase16 runs exposed page_offset == 16 (mod64 == 16).
         All 126 aligned64 runs exposed page_offset == 0 (mod64 == 0).
         All 252 runs: base page-aligned.
```

### Results

Median wall per run (s; ±MAD) and wall ratio phase16/aligned64
(median of 7 runs per cell):

```
chunk   median phase16  ±MAD   median aligned64 ±MAD   ratio d1  ratio d2
 4K       2.3591        0.1078    2.3684        0.1868    0.9961    1.1243
 6K       1.7999        0.1779    1.8307        0.0284    0.9832    1.0322
 8K       1.2624        0.0487    1.2593        0.0372    1.0025    1.1872
12K       0.9223        0.0714    0.8651        0.0999    1.0662    1.1115
16K       0.7143        0.0211    0.7389        0.0427    0.9667    0.8174
24K       0.5843        0.0368    0.5764        0.0567    1.0136    0.9124
32K       0.6032        0.1087    0.5633        0.0887    1.0708    1.0303
48K       0.5420        0.0594    0.5718        0.0341    0.9480    1.0388
64K       0.4917        0.0183    0.4877        0.0162    1.0081    1.0205

median ratio: d1 1.0025, d2 1.0322, all cells 1.0171
range:        0.8174 .. 1.1872
```

MATERIALITY (frozen prereg §8 rule, unchanged):
ratio >= 1.05 AND 1.5·MAD robust separation on both sides.

```
MATERIAL(c,d):  FALSE at ALL 18 cells (9 chunks x 2 depths).
```

Five cells exceed the 1.05 ratio alone (d1 12K 1.0662, d1 32K 1.0708,
d2 4K 1.1243, d2 8K 1.1872, d2 12K 1.1115) — every one fails the
robust-separation leg, and the pattern has no neighbor consistency
(d2: 1.1115 @12K is followed by 0.8174 @16K). They are reported raw as
noise windows, not smoothed (prereg §10 discipline).

- instructions/byte at d1: near-deterministic and equal across arms
  (≤ ~0.01% relative); the d2 small-chunk instruction dispersion (up to
  ~7% median gap at 8K d2) reproduces the FROZEN SWEEP's pre-existing
  within-arm spread (14–24% at 4K/8K d2, overlapping distributions, no
  systematic direction) — backend wait-path noise, not an arm effect.
- Verdict path: no stable material pattern (no neighboring chunk pair
  at either depth, no chunk material at both depths) → **Case A** of
  AMENDMENT 2 §A2.3.

```
STRICT CAUSAL CONTROL:  PASS
APPLICATION MATERIALITY: NOT ESTABLISHED IN ANY TESTED 4K–64K CELL
```

### Evidence taxonomy (final)

- **DIRECTLY MEASURED**: 840-run sweep + 252-run causal control, 0 gate
  errors total, dst hash == src hash every run; per-cell median+MAD
  wall; per-slot address metadata (base/exposed mod 4096, exposed mod
  64) in every causal run; instructions/byte per cell; the materiality
  results.
- **CAUSALLY ISOLATED** (E1-C1 only): same allocation primitive, same
  allocation size, same backing-alignment policy, same
  ownership/lifetime policy, same algorithm, same workload (bytes, op
  counts, chunk, depth and worker topology identical between arms);
  ONLY the exposed-pointer phase treatment differs (+16 vs 0),
  gate-enforced per run. Under that isolation, no material application
  effect was found.
- **INFERRED**: the ALIGN-E0 sub-µs READ micro-cost is
  masked/amortized by the larger application-path cost (per-chunk span
  tens to hundreds of microseconds, read+write pipeline) — the numeric
  gap is measured; the masking attribution is an interpretation.
- **UNRESOLVED**: the exact kernel/uaccess/cache-line mechanism of the
  +16 micro-cost (#267, independent methodology track, decoupled from
  production Sluice); whether a different engine shape (lower per-op
  overhead, direct I/O) could surface the tax — outside this campaign.

---

## PART D — Final synthesis

1. **Was the original allocation confound removed?** YES. Both E1-C1
   arms used `posix_memalign(4096, chunk + 64)`; the recorded per-run
   address metadata and the 252/252 gate record show the arms differed
   only in exposed pointer phase (16 vs 0 page offset, verified mod 4096
   and mod 64 every run).
2. **Did exact +16 vs aligned64 produce application materiality?** NO.
   0 of 18 cells passed the frozen materiality rule; median wall ratio
   1.0025 (d1) / 1.0322 (d2); every >1.05 ratio cell failed the
   robust-separation leg with no neighbor consistency.
3. **Is there any stable 4K–64K material regime?** NO. No neighboring
   chunk pair is material at either depth, and no chunk is material at
   both depths — AMENDMENT 2's Case A applies.
4. **Does this justify an alignment knob?** NO.
5. **Does this justify runtime adaptation?** NO.

Authorized closing statement (AMENDMENT 2 §A2.3, Case A):

> Under an A/B with identical allocation primitive, allocation size,
> backing-alignment policy, ownership, algorithm and workload, changing
> the exposed destination pointer from exact page-offset +16 to
> page/cache-line-aligned did not establish a material
> application-copy benefit anywhere in the tested 4K–64K × depth
> {1,2} regime.

### FINAL ALIGN-E1 VERDICT

```
MICROBENCH-ONLY — NOT APPLICATION MATERIAL

ALIGNMENT IS NOT PROMOTED INTO THE Sluice CONTROL SURFACE.

PRODUCTION ALIGNMENT KNOB:   NO
RUNTIME ADAPTATION:          NO
REGISTERED BUFFER:           NO
SIMD:                        NO

PER PREREG §14 (stop gates):
  DO NOT create a production alignment knob
  DO NOT create an aligned storage abstraction
  DO NOT continue kernel archaeology for production purposes on this
  thread (#267 may continue as independent methodology research,
  decoupled from production Sluice)
```

### Chunk-size finding (separate from the alignment null)

```
chunk_size is a materially stronger performance lever than alignment
in the current application path.

CLAIM BOUNDARY:  current host, current buffered copy engine,
                 workers=1, tested range only.
GLOBAL SWEET SPOT:  NOT LOCATED
TESTED-RANGE PEAK:  1 MiB boundary point (64K -> 1 MiB still rising;
                    nothing above 1 MiB sampled)
PLATEAU:            NOT REACHED

A chunk-size sweet-spot search beyond 1 MiB is FUTURE WORK in a
SEPARATE issue; it is NOT in scope for #268 and this campaign does
not enlarge its matrix.
```

### REPOSITORY STRUCTURE (prereg §14 / B16.5 gate)

```
top-level campaign dirs:      research/align-e1/ (ONE — E1-C1 lives
                              inside it; no separate campaign dir)
source files added:           bench/align_e1_bench.cpp,
                              research/align-e1/scripts/align_e1.py,
                              research/align-e1/scripts/plot_align_e1.py,
                              xmake/benchmarks.lua (one target block)
immutable evidence sessions:  8 (6 validate incl. 5 retained INVALID
                              harness-bug attempts + 1 sweep + 1 causal)
raw evidence file count:      2 per session (runs.jsonl + perf.csv) —
                              NO per-run tiny files
derived plot count:           14 SVG (throughput / instr-per-byte /
                              alignment-ratio x depths 1,2,4,8 + causal-
                              ratio x depths 1,2)

DIRECTORY STRUCTURE: CLEAN
```

### FINAL STATUS

Draft PR #269 (research/align-e1): DO NOT MERGE — stop for adversarial
review of the causal-control evidence. Issue #268 remains OPEN.
