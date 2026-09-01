# ALIGN-E0 — Phase 3 report (#265)

Execution authority: issue #265 (roadmap #259; Phase 2 #263 CLOSED as
completed; BUF-E0 evidence #264 merged at 312ede5). Preregistration:
`ALIGN-E0-PREREGISTRATION.md` (FROZEN, no AMENDMENTs required — the native
campaign ran the frozen arms/matrices/contracts unchanged).

```
VERDICT:        MIXED — NEED TARGETED FOLLOW-UP. The per-op READ +16
                anomaly REPRODUCES on native Linux in kind (same signature:
                only the +16 page-offset point is slow; offsets >= 32 B
                apart are fast) but at ~1/4 to ~1/10 of the WSL2 magnitude
                (native 1.14x-1.42x vs WSL2 2.2x-6.7x), with a different
                size profile (native peak 8K; WSL2 monotonic in size), and
                the application amplifier does NOT reproduce (native d1
                natural/best = 1.03x vs WSL2 1.41x; null at every depth).
                ENVIRONMENT-BLOCKED is SUPERSEDED.
CONFIDENCE:     HIGH on the native fail-closed measurements (0 gate errors
                across 5 native sessions / 1316 runs; same host, warm page
                cache, instructions/op arm-invariant in every cell);
                MIXED verdict stands on the microbench-vs-application
                split, which is DIRECTLY MEASURED on both environments.
ENVIRONMENT:    NATIVE Linux x86-64 (bare metal): Fedora 44, kernel
                7.1.9-200.fc44.x86_64, Intel Xeon E5-2666 v3 10C/20T,
                btrfs (zstd:1) on SATA SSD, page 4096, glibc 2.43,
                clang 22.1.8 Release.

BASE:           312ede532f66236b8e1723368d3d4ab6bbb7476f
HEAD:           7d7046fc (8600583 + one metadata-only text fix)
BRANCH:         research/align-e0
EXECUTION ISSUE:#265
DRAFT PR:       research/align-e0 (Draft, DO NOT MERGE)

PRODUCTION CODE CHANGED:
NO
```

---

## Sessions (11 immutable session records: 10 valid; 1 intentionally
retained invalid WSL2 session that the fail-closed same-work gate
correctly rejected)

| Session | Runs | Content |
| --- | --- | --- |
| `aligne0-validate-wsl2-1` | 128 | Harness validation (WSL2): 8 arms × {read,write} × {4K,1M} × {d1,d8} |
| `aligne0-ladder-wsl2-1` | 960 | Frozen ladder (WSL2): 8 arms × 5 sizes × 6 depths × 2 dirs |
| `aligne0-offset-wsl2-1` | 108 | PAGE-OFFSET-E0 (WSL2): offsets {0,16,32,…,2048} × {4K,64K,1M} × d1 |
| `aligne0-threaded-wsl2-1` | (80) | **INVALID (WSL2)** — harness bug (thread_main rep loop), gate failed closed, kept immutable |
| `aligne0-threaded-wsl2-2` | 80 | Fixed secondary topology diagnostic (WSL2): workers {2..32} × {a0,a7} × {64K,1M} |
| `aligne0-amp-wsl2-1` | 40 | Application amplifier (WSL2): depth {1..16} × {engine,natural,best,4096}, 512 MiB |
| `aligne0-validate-native-1` | 128 | Harness validation (NATIVE): same frozen subset as the WSL2 validation |
| `aligne0-ladder-native-1` | 960 | Frozen ladder (NATIVE): 8 arms × 5 sizes × 6 depths × 2 dirs |
| `aligne0-offset-native-1` | 108 | PAGE-OFFSET-E0 (NATIVE): offsets {0,16,32,…,2048} × {4K,64K,1M} × d1 |
| `aligne0-threaded-native-1` | 80 | Secondary topology diagnostic (NATIVE): workers {2..32} × {a0,a7} × {64K,1M} |
| `aligne0-amp-native-1` | 40 | Application amplifier (NATIVE): depth {1..16} × {engine,natural,best(64),4096}, 512 MiB |

All native sessions: same fail-closed same-work gates, 0 gate errors,
distinct immutable raw evidence, per-session `environment.json` (HEAD
7d7046fc, clean tree at run time, binary sha256, kernel, tools).
Sessions `*native-*` are the Phase-3 evidence; `*wsl2-*` remain the
harness-development / virtualization-comparison evidence.

---

## PART A — WSL2-qualified campaign (QUALIFIED_BUT_VIRTUALIZED, harness
development + virtualization comparison; superseded as the Phase-3 verdict
by Part B/C below)

A0-natural READ was 2.2x–6.7x slower than every tested alignment >= 32 B
across the whole size × depth matrix; offset sweep showed ONLY +16 slow
(offsets 0, 32, 64, …, 2048 all fast), WRITE no consistent material
effect, amplifier d1 natural/best = 1.41x and null at d2+. instructions/op
arm-invariant (2400 @ 4K, 484 607 @ 1M); `cycles:u` unreliable on WSL2
(virtualized non-monotonic counter). Full WSL2 tables are retained in the
sections below (they remain valid virtualization-classified evidence and
the comparison baseline).

### ALIGNMENT THRESHOLD (WSL2-qualified, sync microbench READ, d1)

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

### READ size × depth map (WSL2-qualified, a0-vs-best ratio, sync)

```
            d1     d2     d4     d8    d16    d32
  4K       2.24   2.36   2.47   2.20   3.40   2.21
  8K       2.95   3.00   3.18   3.04   3.05   3.01
 16K       3.42   3.45   3.98   3.74   3.60   3.70
 64K       4.90   5.08   4.68   6.09   4.92   5.16
  1M       3.86   3.95   4.05   3.99   4.24   6.68
```

### READ offset sweep (WSL2, d1, wall ns/op; 1M in µs)

```
offset   0     16     32     64    128    256    512   1024   2048
  4K    711   1718    722    850    738    740    736    708    745
 64K   4818  20583   5313   4173   4363   5176   4173   4851   4245
  1M   86.1  358.1   89.6  106.5   88.9   88.0   89.0   87.6  101.2
```

Only +16 slow (2.4x–4.3x); minflt=0 everywhere. **32 B minimum tested
effective alignment on WSL2; 4096 NOT necessary.**

### D1 → D8 crossover (WSL2-qualified)

- instructions/op flat across depths and arms — not an instruction-count
  effect.
- Threaded diagnostic: true per-op READ latency a0 4x–6.5x slower than a7
  at every worker count — the per-op cost is NOT hidden by overlap in
  pure-I/O; MECHANISM DISAPPEARS: NO.
- Amplifier: benefit survives at d1 (natural/best 1.41x), null at d2+
  (1.00–1.19x within dispersion) — SUPPORTED INTERPRETATION:
  application-level masking/overlap; exact application bottleneck
  UNRESOLVED (writeback / page-cache ceiling / control-plane =
  hypotheses).

### WRITE (WSL2-qualified)

NO CONSISTENT MATERIAL ALIGNMENT EFFECT ESTABLISHED (ratios ~0.96–2.18x
scattered, isolated cells noisy; effect==0 not claimed).

---

## PART B — NATIVE LINUX REPLICATION (frozen harness, unchanged arms)

Environment (session records + notes): bare metal, NOT WSL/NOT container
(no `hypervisor` CPU flag; DMI JUXIESHI desktop). Kernel
7.1.9-200.fc44.x86_64 (Fedora 44, native PREEMPT_DYNAMIC); CPU Intel Xeon
E5-2666 v3 @ 2.90 GHz base / 3.5 GHz turbo, 10 physical cores / 20
threads (SMT), schedutil governor, L3 25 MiB; RAM 62 GiB; page size 4096;
filesystem btrfs subvol /home on /dev/sda3 (SATA SSD GS-480) with
compress=zstd:1 — the timed path is warm page-cache uaccess copy, not
device/compression I/O; glibc 2.43; clang 22.1.8 Release (warnings-as-
errors PASS), xmake 3.0.9; perf 7.1.9 (paranoid=2); git 7d7046fc clean at
run time. One metadata-only text change vs 8600583: the validate purpose
string ("WSL2 development-only subset" → "frozen development subset") so
native immutable records do not carry a mislabel; no scientific element
touched.

### B.1 READ ladder (native, sync microbench; a0 vs median(a1..a7))

```
arm   exposed            align   page_off   wall (4K d1)  ratio vs a0   instr/op
a0    base+16               16        16         1432 ns      1.00x       2382
a1    round_up(base,64)     64         0         1303 ns      1.10x       2382
a2    round_up(base,128)   128         0         1247 ns      1.15x       2382
a3    round_up(base,256)   256         0         1252 ns      1.14x       2382
a4    round_up(base,512)   512         0         1306 ns      1.10x       2382
a5    round_up(base,1024) 1024         0          997 ns      1.44x       2382
a6    round_up(base,2048) 2048         0         1249 ns      1.15x       2382
a7    round_up(base,4096) 4096         0         1247 ns      1.15x       2382
```

(4K d1 single-arm dips like a5 = 997 ns and 8K a2 = 1292 ns are quiet-window
noise on this host — individual arms occasionally catch a clean window; the
robust reference is the aligned-group median, used everywhere below.)

a0 vs median(a1..a7) by size × depth:

```
            d1     d2     d4     d8    d16    d32
  4K       1.15   1.14   1.14   1.14   1.15   1.14
  8K       1.35   1.35   1.42   1.34   1.33   1.34
 16K       1.23   1.24   1.21   0.98*  1.22   1.24
 64K       1.25   1.26   1.26   1.01*  1.27   1.22
  1M       0.95   0.98   1.11   0.99*  1.05   1.12
```

`*` d8 cells at 16K/64K/1M are noise windows (MAD up to ~5 µs; neighbors
d4/d16 return to the 1.2x pattern) — no systematic depth crossover on
native. The 1M rows are inside window noise: the sync 1M d1 measurement
cannot resolve a 1M READ effect, but the threaded diagnostic can (B.4).

**NATIVE READ: the a0 (+16) penalty REPRODUCES directionally at 4K–64K
(1.14x–1.42x, MAD-separated, flat across depth) — same signature, ~1/4 to
~1/10 of the WSL2 magnitude. At 1M the sync effect is unresolvable (noise);
threaded per-op shows +5–9% (B.4).** instructions/op arm-invariant (2382 @
4K … 485 102 @ 1M) — not an instruction-count effect, same as WSL2.
Minimum tested effective alignment on native: 64 B in the ladder (a1
already fast; consistent with the WSL2 amplifier's 64 B choice).

### B.2 READ offset sweep (native, d1, wall ns/op; 1M in µs)

```
offset   0     16     32     64    128    256    512   1024   2048
  4K   1289   1415   1305   1334   1298   1242   1254   1249   1234
 64K   9686  11674   9774   9534   9981   9500  10087   9610   9169
  1M   186.0  195.6  186.0  180.3  179.4  168.9  180.8  176.4  179.0
```

**Only +16 slow at 4K (~+9–15%) and 64K (~+16–27%); offsets 0/32/64/…/2048
all fast; minflt=0 everywhere.** The structural signature of the WSL2
offset sweep (only +16 slow, all tested offsets >= 32 B apart fast)
REPRODUCES on native; the magnitude does not (WSL2 +16 was 2.4x–4.3x).
1M: +16 = 195.6 µs vs 168.9–186.0 µs others, inside window noise (MAD
7–87 µs) — weak but same-direction. As on WSL2, only +16 was tested in the
slow residue: "32 B minimum tested effective" (all tested offsets >= 32 B
apart fast) is DIRECTLY MEASURED; the full mod-32 residue class is not
exhaustively proven.

### B.3 WRITE (native)

Flat across the whole ladder (a0/median-aligned 0.83–1.04, no pattern —
extremes are noise windows) and flat across the offset sweep and the
threaded diagnostic (0.97–1.04). **WRITE: NO CONSISTENT MATERIAL ALIGNMENT
EFFECT ESTABLISHED — same conclusion on native and WSL2.**

### B.4 Threaded diagnostic (native, real in-flight depth = workers)

Per-op wall inside timed spans (`thread_op_ns` median), READ:

```
size=64K:  a0 12.7-15.8 us vs a7 10.3-13.9 us  -> a0 +12-24% at every
           worker count (w2..w32)
size=1M:   a0 282-1063 us vs a7 266-982 us     -> a0 +5-9% at w2..w32
           (w16/w32 partially oversubscribed: 32 workers > 20 CPUs)
```

Amortized wall/op: READ 64K w8/w16/w32 ratios 1.11–1.15; 1M 1.06–1.11
(w2/w4 windows at 64K are scheduling noise). WRITE flat. **The per-op READ
alignment tax PERSISTS under real overlap on native — mechanism does not
disappear with depth — at ~1/4 to ~1/5 of the WSL2 per-op magnitude (WSL2
64K: 4x–6.5x).**

### B.5 Application amplifier (native, 512 MiB copy, 1 MiB chunks, best=64)

Full-engine span (ms), median over 14 reps; dst hash == src hash;
same-work gates green:

```
depth   engine    natural   best(64)   4096
  1     869.0     881.2     858.4     860.3
  2     667.5     638.5     637.0     636.0
  4     639.2     642.1     637.4     668.6
  8     686.8     698.3     665.4     685.3
 16     715.0     714.5     700.8     720.7
```

- d1: natural/best = 1.027x, natural/4096 = 1.024x — **NO material
  application-level alignment benefit at any depth on native** (WSL2 d1
  was 1.41x). d2–d16: 0.96–1.05x, all within MAD.
- `engine` (production natural storage) ≈ `natural` replica (869 vs 881 ms
  d1) — replica fidelity holds.
- Per prereg §9 the amplifier is the application-level boundary: the
  microbench per-op READ tax does not surface as an end-to-end benefit in
  the realistic copy pipeline on native (the pipeline absorbs it).

### B.6 PMU (native)

- `instructions:u` stable and arm-invariant in every cell (2382 @ 4K,
  4277 @ 8K, 30 779 @ 64K, 485 102 @ 1M; write-loop cells ~478–485) —
  attribution clean; wall gaps are NOT instruction-count deltas.
- `cycles:u` unreliable as a per-op double-difference on this host too
  (negative values recur; turbo/frequency state differs between the R7 and
  R14 process runs) — demoted like WSL2, but the native cause is frequency
  scaling, not a virtualized counter. Quantitative pair = instructions:u +
  in-process wall. Secondary PMU events not required: no causal story needs
  them, and the retest would cost a full campaign for an already-unreliable
  counter family on this host.

---

## PART C — WSL2 → NATIVE side-by-side (Phase-3 conclusion table)

```
                   WSL2                          NATIVE
---------------------------------------------------------------
READ +16           slow (2.2x-6.7x,              slow but SMALL:
                   monotonic in size)            1.14x-1.42x @4K-64K
                                                 (peak 8K); ~1.0x @1M
                                                 sync (noise), +5-9%
                                                 threaded per-op
32 B tested        fast                          fast (same signature:
effective           (only +16 slow)              only +16 slow)
4096               fast                          fast (equal to 64 B)
WRITE              no consistent material        no consistent material
                   effect                        effect
d1 amplifier       material (natural/best        NULL (natural/best
                   1.41x)                        1.03x, within MAD)
d2+ amplifier      null (within dispersion)      null (0.96-1.05x)
instructions/op    equal across arms             equal across arms
cycles:u           unreliable (virtualized       unreliable (frequency
                   counter)                      scaling between runs)
---------------------------------------------------------------
CLASSIFICATION:    MIXED — per-op READ +16 anomaly REPRODUCED in kind
                   (same signature, reduced magnitude, different size
                   profile); application-level benefit NOT REPRODUCED
                   (amplifier null at every depth); WRITE null on both.
```

---

## EVIDENCE TAXONOMY (final)

- **DIRECTLY MEASURED**: native READ a0-vs-aligned wall gap 1.14x–1.42x at
  4K–64K across the full ladder (0 gate errors, 960 runs); native offset
  sweep (only +16 slow; offsets 0/32/64/…/2048 fast); native threaded
  per-op persistence (+12–24% @64K, +5–9% @1M); native amplifier null
  (natural/best ≤ 1.03x); instructions/op arm-invariance on both hosts;
  WRITE null on both hosts; WSL2 numbers as recorded in Part A.
- **CAUSALLY ISOLATED**: exposed pointer alignment/offset is the ONLY
  variable (same over-allocated backing, same page set, same bytes, same
  prefault, minflt=0, same-work fail-closed) — the +16 wall effect is
  causally the exposed address phase on BOTH hosts; which offset/residue
  classes are slow remains the UNRESOLVED part.
- **PROFILE/SOURCE SUPPORT**: none. PMU cycles unreliable on both hosts;
  kernel uaccess source attribution not performed (out of scope; the
  verdict stands on the A/B data).
- **INFERRED**: the native amplifier null under a real per-op microbench
  tax is interpreted per prereg §9 as application-level absorption
  (SUPPORTED INTERPRETATION — the 1 MiB-chunk pipelined copy overlaps the
  per-op tax; the exact pipeline bottleneck is not attributed).
- **UNRESOLVED**: exact kernel/uaccess branch behind the +16 penalty on
  either host; the residue-class rule (only +16 tested in the slow class;
  +48/+80/+112 and 16-mod-64 classes untested — frozen matrix); why the
  native penalty peaks at 8K while WSL2 grew with size; whether a
  different application shape (smaller chunks, readier-pipeline, direct
  READ-heavy workload) surfaces the 4K–64K per-op tax end-to-end.

## PHASE 3 VERDICT (FINAL)

```
VERDICT (primary term):
  MIXED — NEED TARGETED FOLLOW-UP

  READ +16 per-op anomaly: REPRODUCED on native in kind (same signature:
  +16 slow, offsets >= 32 B apart fast) but at 1.14x-1.42x vs WSL2's
  2.2x-6.7x, peaking at 8K (WSL2 grew with size). instructions/op equal
  across arms on both hosts; cycles:u unreliable on both (different
  causes). WRITE: null on both — READ-only effect.
  Application amplifier: NOT reproduced — native natural/best =
  1.027x at d1 and 0.96-1.05x at d2-d16 (all within MAD); WSL2's d1
  1.41x is not confirmed and is best read as a WSL2-environment effect
  candidate. Per prereg §9 the amplifier is the application-level
  boundary: no application-level alignment benefit is established on
  native.
  The WSL2 +16 penalty is therefore NOT a pure virtualization artifact
  (a genuine, small per-op READ uaccess property candidate exists on
  native), but the WSL2 MAGNITUDE, its size-profile, and its
  application-level benefit are environment-specific and do not
  transfer.

PRODUCTION ALIGNMENT CHANGE AUTHORIZED:
  NO   (production gate: 1 native-reproduced — partially (per-op READ
       only); 6 application-amplifier-survives — NO; verdict MIXED)

MINIMAL RESOURCE BOUNDARY JUSTIFIED:
  NO  (a posix_memalign + RAII wrapper expresses any earned policy; no
  BufferStorage framework need shown)

BUFFERPOOL JUSTIFIED:
  NO

REGISTERED BUFFER AUTHORIZED:
  NO

SIMD / CUSTOM MEMCPY:
  NO
```

Targeted follow-up candidates (not authorized work; for a later issue if
the roadmap keeps the thread): (a) refine the native READ threshold 24/32/
48 and the +48/+80 residue (requires a preregistered AMENDMENT — frozen
matrix must change consciously); (b) an application amplifier with the
4K–64K regime that shows the per-op tax (1 MiB-chunk copy absorbs it);
(c) kernel uaccess source attribution (copy_user_enhanced_fast_string on
Haswell-EP) if a causal mechanism claim is ever needed.

## LIMITATIONS

- Machine noise: window-level MAD at 1M is large on this desktop host
  (up to ~170 µs); d8 cells at 16K/64K/1M and several single-arm dips are
  noise windows, identified as such rather than interpreted. The verdict
  rests on MAD-separated 4K–64K cells and the threaded/amplifier sessions,
  not on 1M sync windows alone.
- btrfs compress=zstd:1 on SATA SSD: data is deterministic pseudo-random
  (incompressible); timed path is warm page-cache uaccess copy; background
  writeback compression is outside timed spans but contributes to machine
  noise (recorded per session).
- Only +16 tested in the slow residue class; frozen offset set only.
- Offset diagnostic at d1 only; workers=1 primary; threaded diagnostic
  a0/a7 at 64K/1M only.
- cycles:u unusable on both hosts; no secondary PMU cache/TLB campaign was
  run (retest cost vs no causal story). Kernel source inspection not
  performed; MICRO-MECHANISM UNRESOLVED is the accepted outcome.
- One metadata-only harness text change vs the frozen head (validate
  purpose string) — recorded in commit 7d7046fc; no scientific element
  changed; no AMENDMENT required.

## FINAL STATUS

```
Draft PR:     HUMAN REVIEW READY — Phase 3 scientifically closed
              (ENVIRONMENT-BLOCKED superseded by a completed native
              replication); verdict MIXED — NEED TARGETED FOLLOW-UP;
              PRODUCTION ALIGNMENT CHANGE AUTHORIZED: NO
MERGED:       NO
```