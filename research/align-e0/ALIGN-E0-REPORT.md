# ALIGN-E0 — Phase 3 report (#265)

Execution authority: issue #265 (roadmap #259; Phase 2 #263 CLOSED as
completed; BUF-E0 evidence #264 merged at 312ede5). Preregistration:
`ALIGN-E0-PREREGISTRATION.md` (FROZEN, no AMENDMENTs required).

```
VERDICT:        ENVIRONMENT-BLOCKED (final Phase-3 verdict pending native
                Linux replication). WSL2-qualified READ effect located:
                only +16 slow in the preregistered offset sweep; minimum
                tested effective alignment 32 B.
CONFIDENCE:     HIGH on the WSL2-qualified measurements (fail-closed
                same-work, 0 gate errors across 5 valid sessions / 1316
                runs; 1 retained invalid session rejected by the gate);
                LOW-to-NONE on any production interpretation (native
                replication missing).
ENVIRONMENT:    WSL2 (QUALIFIED_BUT_VIRTUALIZED, ENVIRONMENT-LIMITED).
                Native Linux x86-64 not reachable from this workspace.

BASE:           312ede532f66236b8e1723368d3d4ab6bbb7476f
HEAD:           312ede5 + research-only artifacts (branch research/align-e0)
BRANCH:         research/align-e0
EXECUTION ISSUE:#265
DRAFT PR:       research/align-e0 (Draft, DO NOT MERGE)

PRODUCTION CODE CHANGED:
NO
```

---

## Sessions (6 immutable session records: 5 valid sessions with 0 gate
errors; 1 intentionally retained invalid session that the fail-closed
same-work gate correctly rejected)

| Session | Runs | Content |
| --- | --- | --- |
| `aligne0-validate-wsl2-1` | 128 | Harness validation: 8 arms × {read,write} × {4K,1M} × {d1,d8} |
| `aligne0-ladder-wsl2-1` | 960 | Frozen ladder: 8 arms × 5 sizes × 6 depths × 2 dirs |
| `aligne0-offset-wsl2-1` | 108 | PAGE-OFFSET-E0: offsets {0,16,32,…,2048} × {4K,64K,1M} × d1 |
| `aligne0-threaded-wsl2-1` | (80) | **INVALID** — harness bug (thread_main rep loop), gate failed closed, kept immutable |
| `aligne0-threaded-wsl2-2` | 80 | Fixed secondary topology diagnostic: workers {2..32} × {a0,a7} × {64K,1M} |
| `aligne0-amp-wsl2-1` | 40 | Application amplifier: depth {1..16} × {engine,natural,best,4096}, 512 MiB |

All numbers below are **QUALIFIED_BUT_VIRTUALIZED** (WSL2). They are the
harness-at-full-scale validation and directional evidence, NOT the Phase-3
verdict. Primary quantitative pair: in-process wall (steady_clock,
median/MAD over 14 reps) + `instructions:u` R7/R14 double-difference;
`cycles:u` UNRELIABLE on this host (virtualized non-monotonic counter,
BUF-E0 finding).

---

## ALIGNMENT THRESHOLD (WSL2-qualified, sync microbench READ)

Per-arm `wall_ns_per_op` (median over R14) and a0-relative ratio,
aggregated across the ladder session. `instr` = instructions/op.

```
arm   exposed            align   page_off   wall (4K d1)  ratio vs a0   instr/op
a0    base+16               16        16         2099 ns      1.00x       2400
a1    round_up(base,64)     64         0          965 ns      2.18x       2399
a2    round_up(base,128)   128         0         1201 ns      1.75x       2399
a3    round_up(base,256)   256         0          936 ns      2.24x       2399
a4    round_up(base,512)   512         0          970 ns      2.16x       2399
a5    round_up(base,1024) 1024         0          946 ns      2.22x       2399
a6    round_up(base,2048) 2048         0         1030 ns      2.04x       2399
a7    round_up(base,4096) 4096         0          963 ns      2.18x       2399
```

- **DIRECTLY OBSERVED**: in the preregistered offset sweep, +16 (16-byte
  aligned, page offset 16) was the only slow exposed pointer (2.2x–6.7x
  across the size × depth matrix); all offsets 0, 32, 64, …, 2048 were
  fast at every size (minflt=0). a0 is the only slow ladder arm; a1..a7
  are statistically equivalent.
- **MINIMUM TESTED EFFECTIVE ALIGNMENT**: 32 B in the tested WSL2
  configurations — every tested alignment ≥ 32 B captures the benefit.
- **SUPPORTED HYPOTHESIS**: the observed WSL2 split is consistent with a
  32-byte-alignment explanation (32-aligned fast, 16-mod-32 slow as
  tested at +16); the residue-class rule is NOT proven.
- **4096 necessary: NO (on WSL2 READ)** — 64 B already captures the full
  benefit; 32 B is the smallest tested alignment that does so.
- instructions/op shows no material arm separation (2399–2400) — the
  wall-time effect is NOT explained by an instruction-count delta; the
  exact latency/kernel/uaccess mechanism is UNRESOLVED.

## READ (WSL2-qualified size × depth map)

a0-vs-best ratio by size × depth (sync microbench; all arms a1..a7
equivalent):

```
            d1     d2     d4     d8    d16    d32
  4K       2.24   2.36   2.47   2.20   3.40   2.21
  8K       2.95   3.00   3.18   3.04   3.05   3.01
 16K       3.42   3.45   3.98   3.74   3.60   3.70
 64K       4.90   5.08   4.68   6.09   4.92   5.16
  1M       3.86   3.95   4.05   3.99   4.24   6.68
```

The effect is present at **every size and every depth** in the synchronous
microbench (no overlap in sync mode — per-op cost persists). The ratio grows
with size (2.2x @ 4K → ~3.4x @ 16K → ~5x @ 64K → ~4x @ 1M).

## WRITE (WSL2-qualified size × depth map)

a0-vs-best ratio by size × depth (sync microbench):

```
            d1     d2     d4     d8    d16    d32
  4K       1.09   1.05   0.99   1.11   1.03   1.10
  8K       1.41   1.13   1.25   0.96   1.18   1.05
 16K       1.09   1.09   1.10   1.09   1.38   1.11
 64K       0.97   1.25   1.19   1.22   1.16   2.18
  1M       1.15   1.91   1.21   1.15   1.38   2.02
```

**WRITE: NO CONSISTENT MATERIAL ALIGNMENT EFFECT ESTABLISHED** in the
WSL2-qualified measurements. Ratios hover ~1.0–1.4x within dispersion
(write 1M cells are extremely noisy — MADs up to ~100–200 µs on ~50 µs
medians; WSL2 virtualized writeback is a candidate explanation, not a
proven root cause). Some isolated cells reach 1.9–2.2x, but they are noisy
and lack neighboring-cell / regime consistency. No alignment effect == 0
claim is made; equally, no material effect has been established. WRITE gets
its own independent verdict: **NO CONSISTENT MATERIAL ALIGNMENT EFFECT
ESTABLISHED (QUALIFIED_BUT_VIRTUALIZED)**.

## PAGE-RELATIVE OFFSET

RUN (trigger met: a0 vs a1 changes both alignment and page offset).
Exposed = page-aligned base + offset, d1. READ `wall_ns_per_op`:

```
offset   0     16     32     64    128    256    512   1024   2048
  4K    711   1718    722    850    738    740    736    708    745
 64K   4818  20583   5313   4173   4363   5176   4173   4851   4245
  1M   86.1  358.1   89.6  106.5   88.9   88.0   89.0   87.6  101.2  (µs)
```

**Only offset +16 is slow (2.4x–4.3x)**; offsets 0, 32, 64, …, 2048 are all
fast, at every size. `minflt_io = 0` for every cell (no page-fault
artifact).

- **DIRECTLY OBSERVED**: +16 was the only slow offset in the preregistered
  offset sweep; offsets 0, 32, 64, …, 2048 were all fast at every size.
- **SUPPORTED HYPOTHESIS**: the observed WSL2 split is consistent with a
  32-byte-alignment explanation — 16-aligned-but-not-32-aligned (+16) is
  slow and every tested alignment ≥ 32 B is fast at any page offset (all
  non-zero page offsets up to 2048 are fast when 32-aligned), which also
  argues against a page-relative-phase property. The residue-class rule
  (all of 16 mod 32) is NOT proven — only +16 was tested.
- **MINIMUM TESTED EFFECTIVE ALIGNMENT**: 32 B in the tested WSL2
  configurations.
- **UNRESOLVED**: the exact threshold and residue-class rule; other
  16-mod-32 offsets such as +48/+80/+112 were not tested, nor the finer
  16-mod-64 classes (24/48/80/112…).
- WRITE: flat across all offsets (within the dispersion noted above).

## D1 → D8 CROSSOVER (WSL2-qualified)

- **instructions behavior**: instructions/op shows no material arm
  separation and is ~flat across depths (2400 @ 4K; 484 607 @ 1M) — the
  wall-time effect is NOT explained by an instruction-count delta, and it
  does not disappear with depth. The exact latency/kernel/uaccess
  mechanism is UNRESOLVED.
- **wall behavior (sync microbench)**: per-op benefit persists at ALL
  depths — the mechanism does not disappear at the syscall level.
- **wall behavior (threaded diagnostic, real overlap)**: true per-op
  latency for READ a0 is 4x–6.5x slower than a7 at every worker count
  (64K: 22–28 µs vs 5.4–8.7 µs; 1M: 340–566 µs vs 113–306 µs); amortized
  wall/op improves with workers for both and the gap WIDENS with overlap
  (64K w32: a0 4.0 µs vs a7 0.55 µs). The cost is NOT hidden by overlap in
  a pure-I/O harness.
- **wall behavior (application amplifier)**: the benefit survives at d1
  (natural/best = 1.41x) and is null at d2+ (1.00–1.19x, within dispersion).

verdict:

```
MECHANISM DISAPPEARS:   NO — supported by pure-I/O and threaded
                        measurements (per-op READ cost persists at every
                        depth; true per-op latency a0 4x–6.5x slower at
                        every worker count).
APPLICATION-LEVEL MASKING / OVERLAP:
                        SUPPORTED INTERPRETATION — the amplifier shows the
                        benefit at d1 (natural/best = 1.41x) and no
                        material separation at d2+, while the pure-I/O
                        measurements show the tax persists; consistent
                        with the READ copy tax ceasing to be the
                        throughput-limiter at application depth.
EXACT APPLICATION BOTTLENECK:
                        UNRESOLVED — writeback, page-cache ceiling and
                        control-plane factors are candidate explanations
                        (hypotheses), not proven root cause.
```

This adjudicates the BUF-E0 d1-material/d8-null observation: it is
consistent with a pipeline/overlap interpretation, not a vanishing uaccess
cost.

## APPLICATION AMPLIFIER (WSL2-qualified, 512 MiB copy, 1 MiB chunks,
ThreadPoolBackend workers=1)

Full-engine span (ms), median over 14 reps; runner-verified dst hash ==
src hash; same-work gates green:

```
depth   engine    natural   best(64)   4096
  1     458.1     462.6     328.1     366.2
  2     273.8     267.4     266.3     236.8
  4     285.5     267.4     225.0     240.0
  8     387.8     298.9     282.5     249.8
 16     272.8     355.9     315.9     295.5
```

- d1: **natural/best = 1.41x, natural/4096 = 1.26x** — the READ alignment
  benefit survives at d1 at application level.
- d2+: ratios 1.00–1.19x, within the large WSL2 writeback dispersion
  (MAD 15–83 ms on 225–460 ms medians) — no material effect beyond d1.
- `engine` (production, natural vector storage) ≈ `natural` replica at d1
  (458 vs 463 ms) — replica fidelity holds.

## EVIDENCE TAXONOMY

- **DIRECTLY MEASURED**: READ a0 vs a1..a7 wall gap across the full
  ladder; offset sweep (only +16 slow); instructions/op equality across
  arms; WRITE null at all sizes/depths; threaded true-per-op and
  amortized gaps; amplifier d1/d2+ spans. All with fail-closed same-work,
  0 gate errors.
- **CAUSALLY ISOLATED**: exposed pointer alignment/offset is the ONLY
  variable (same over-allocated backing, same page set, same bytes, same
  prefault, minflt=0) — the +16 wall effect on WSL2 is causally the
  exposed address phase (offset/alignment), not allocator, mapping,
  faults, or content; which offset/residue classes are slow is the
  UNRESOLVED part above.
- **PROFILE/SOURCE SUPPORT**: none. PMU unreliable on WSL2; kernel
  uaccess source attribution not performed (native step).
- **INFERRED**: d1/d8 application null interpreted as application-level
  masking/overlap (SUPPORTED INTERPRETATION backed by threaded +
  instructions evidence); the exact application bottleneck is UNRESOLVED —
  writeback / page-cache ceiling / control-plane factors are candidate
  hypotheses, not established root cause.
- **UNRESOLVED**: exact kernel/uaccess branch for the +16 penalty; whether
  +16 reproduces on NATIVE Linux (it may be a WSL2 virtualization
  artifact); the exact threshold and residue-class rule (16-mod-32 offsets
  other than +16, e.g. +48/+80/+112, and the 16-mod-64 offset classes were
  not tested); WRITE 1M dispersion mechanism.

## PHASE 3 VERDICT

```
ALIGNMENT EFFECT:
  WSL2-qualified READ effect located and causally isolated: +16 was the
  only slow offset in the preregistered offset sweep (2.2x-6.7x slower);
  32 B is the minimum tested alignment that captures the benefit at any
  page offset, and the observed split is consistent with (but does not
  prove) a 32-byte-alignment rule; WRITE shows no consistent material
  effect. FINAL PHASE-3 VERDICT: ENVIRONMENT-BLOCKED — native Linux
  replication is mandatory before any interpretation beyond WSL2 and
  before any production decision.

PRODUCTION ALIGNMENT CHANGE AUTHORIZED:
  NO

MINIMAL RESOURCE BOUNDARY JUSTIFIED:
  NO  (a posix_memalign + RAII wrapper expresses any earned policy; no
  BufferStorage framework need is shown by this phase)

BUFFERPOOL JUSTIFIED:
  NO

REGISTERED BUFFER AUTHORIZED:
  NO
```

## LIMITATIONS

- **Native Linux unavailable from this workspace** — the mandatory
  environment gate is unmet; the final verdict is ENVIRONMENT-BLOCKED. The
  harness is native-ready and the frozen matrix is exactly what a native
  session must run (`scripts/aligne0.py run --kind ladder/offset/threaded`
  + `amp --best-align <n>`; no code changes needed).
- WSL2 virtualization: `cycles:u` unreliable; write 1M dispersion very high
  (virtualized writeback) — WRITE conclusions rest on the absence of a
  consistent pattern, not on tight CIs.
- The +16 signature may be a WSL2-specific artifact; native replication is
  required to know whether it is a real Linux kernel/uaccess property.
- The exact threshold and residue-class rule are UNRESOLVED: 16-mod-32
  offsets other than +16 (e.g. +48/+80/+112) and the 16-mod-64 classes
  (24/48/80/112) were not tested; only the preregistered offset set was
  run.
- Offset diagnostic at d1 only; workers=1 primary (secondary topology
  diagnostic only a0/a7 at 64K/1M).
- No kernel source inspection or perf attribution performed (PMU
  unreliable on WSL2); mechanism classified UNRESOLVED.
- No production code was changed or authorized; no registered buffers, no
  SIMD, no O_DIRECT, no buffer framework.

## FINAL STATUS

```
Draft PR:     HUMAN REVIEW READY (WSL2-qualified campaign complete;
              native replication is the named residual before any
              production interpretation)
MERGED:       NO
```
