# CHUNK-E0 Phase H0 — chunk-size × depth sweet-spot map, Host-0 (#270)

Execution authority: issue #270 (roadmap #259). Preregistration:
`CHUNK-E0-H0-PREREGISTRATION.md` (FROZEN before formal measurement; no
amendments were required — the extension rule did not fire). This report
covers the complete H0 campaign on the bare-metal Fedora/x86-64 Host-0:

- harness validation session `chunk-e0-h0-validate-native-1`
  (8 subset runs + 6-run cycles:u stability probe, 0 gate errors);
- the official frozen sweep `chunk-e0-h0-sweep-native-1`
  (**420 runs = 7 rounds × 60 cells, 0 gate errors**), immutable.

```
VERDICT:        HOST-LOCAL SWEET REGION LOCATED (Host-0 only).
                Plateau entry located at depth 2 (1.5 MiB), depth 4
                (1.5 MiB) and depth 8 (768 KiB). Depth 1 never plateaus
                inside the tested range (see per-depth block d1).

BASE:           a2f7c4c904994137d50c571a1d36197866359f39 (master, post-#269)
HEAD:           branch research/chunk-e0-h0; the sweep ran on a working
                tree at a2f7c4c9 whose only deltas are this campaign's own
                research/ + bench/ + xmake additions (production library
                code unchanged). Measured artifact pinned by bench binary
                sha256 75f1db930e2dbc6101291db102b7e6eadeb954f21e9bd2d0651e9f4acf4ffcae
                and fixture sha256 8a3c4bf01ec3d32c0da34e9ed93a091bfaedfc48e39dad5bcd6c8b1bf548fd53.
BRANCH:         research/chunk-e0-h0
EXECUTION ISSUE:#270
DRAFT PR:       research(chunk-e0): #270 host-0 chunk-size × depth sweet-spot map (DRAFT, DO NOT MERGE)

TOTAL BYTES:    1 GiB (1073741824) per run, frozen by the preregistered
                smoke probe (prereg §5.1); identical total per cell.
REPETITIONS:    R = 7 seeded interleaved Fisher–Yates rounds
                (seed 0xE1E1E1E121212121 + round), median ± MAD per cell.

PRODUCTION DEFAULT CHANGE: NO
CHUNK_SIZE CONTROL-SURFACE PROMOTION: NOT YET
RUNTIME ADAPTATION: NO
FINAL SWEET-SPOT STATUS: HOST-LOCAL SWEET REGION LOCATED
                (depths 2/4/8; depth 1: NO STABLE SWEET REGION within the
                tested range)
```

---

## Environment (Host-0, as recorded in session environment.json)

Bare metal (systemd-detect-virt: none), Fedora 44, kernel
7.1.9-200.fc44.x86_64, glibc 2.43. Intel(R) Xeon(R) CPU E5-2666 v3
@ 2.90GHz (Haswell-EP, 10C/20T), governor schedutil / intel_cpufreq,
no_turbo=0 (turbo ENABLED — this is the frequency-scaling confounder, see
CPU COST). Filesystem btrfs (compress=zstd:1,ssd,discard=async) on
/dev/sda3 (SATA SSD), 1 NUMA node, 62 GiB RAM. clang 22.1.8 Release
(warnings-as-errors), xmake v3.0.9, perf 7.1.9 (paranoid=2, unprivileged
u === working for instructions:u / cycles:u).

## Measurement validity (fail-closed contract, prereg §8)

- 420/420 sweep runs passed every driver gate: bench exit 0,
  instructions:u > 0, and per-run `sha256(dst) == sha256(src)` over the
  full 1 GiB (same-work proof; 0 gate errors, recorded in gates.json).
- Run order: seeded per-round full-grid shuffle; the recorded run_id
  sequence is byte-identical to a re-derivation from the frozen
  constants (`run_plan`), which is the single ordering authority shared
  by execution and validation.
- Median |MAD| ≤ ~1.1% for every cell ≥ 64 KiB; 16K×d1 is the noisiest
  cell at ~9.8% (per-op overhead regime) — raw values preserved in
  raw/runs.jsonl.
- cycles:u is DEMOTED for the whole campaign by the preregistered probe
  (negative consecutive per-op double-differences: 16K −78.9, 4M −413.9
  counts/op) — turbo + schedutil make per-op cycle counts
  non-comparable; NO IPC or cycle-derived claims are made anywhere.

## Per-depth sweet-spot blocks (median MiB/s; prereg §10 definitions)

Naming discipline: every peak sitting at chunk = 4 MiB is a TESTED-RANGE
PEAK (the largest sampled value), never an "optimum". KNEE = deterministic
two-segment LS fit on (log2 chunk, MiB/s) requiring ≥10% SSE reduction —
**the fit NEVER reached that threshold at any depth** (all reductions are
NEGATIVE, i.e. the two-segment fit is worse than a single line: the H0
surface is smooth, so KNEE NOT LOCATED everywhere).

### depth 1
```
TESTED-RANGE PEAK:  4 MiB  — 689.5 MiB/s  (AT the sampled upper boundary)
P95 POINT:          3 MiB  (689.5 × 0.95 = 655.0)
PLATEAU:            NOT LOCATED (no two consecutive <3% pairs anywhere;
                    last-pair gain 3M→4M = +2.05%, below the 3% material
                    threshold but the whole top of the curve creeps)
KNEE:               NOT LOCATED (SSE reduction −20.07)
```
Depth 1 is the slowest curve at every chunk ≥ 64K and never stabilizes
inside 16K..4M. It contributes NO sweet region to the verdict.

### depth 2
```
TESTED-RANGE PEAK:  4 MiB  — 885.9 MiB/s (AT the sampled upper boundary;
                    the ONLY depth still materially rising there:
                    3M→4M = +4.86% ≥ 3%)
P95 POINT:          2 MiB  (857.7 MiB/s = 96.8% of peak)
PLATEAU ENTRY:      1.5 MiB (833.1 MiB/s = 94.0% of peak; the 1.5M→2M and
                    2M→3M pairs are both <3%)
KNEE:               NOT LOCATED (SSE reduction −13.48)
```

### depth 4
```
TESTED-RANGE PEAK:  2 MiB  — 845.1 MiB/s (INTERIOR point, not at the
                    boundary; 3M/4M sit ~1–2% below it)
P95 POINT:          1.5 MiB (827.2 MiB/s)
PLATEAU ENTRY:      1.5 MiB
KNEE:               NOT LOCATED (SSE reduction −8.31)
```

### depth 8
```
TESTED-RANGE PEAK:  4 MiB  — 814.9 MiB/s (AT the sampled upper boundary;
                    last-pair gain 3M→4M = +0.24%, flat)
P95 POINT:          2 MiB  (784.5 MiB/s)
PLATEAU ENTRY:      768 KiB (741.3 MiB/s = 91.0% of peak)
KNEE:               NOT LOCATED (SSE reduction −5.92)
```

Note the plateau-entry metric is a consecutive-pair flatness rule, not a
global proximity rule; for d8 it fires while the curve is still at 91% of
peak because the *incremental* gains have flattened. Both metrics are
reported so the distinction stays visible.

## GLOBAL REGIME (H0, Host-0)

Throughput rises smoothly with chunk size at every depth — the two-segment
knee fit does not beat a single line anywhere, so there is no statistically
supported knee; the surface is a monotone rise that flattens, not a curve
with a break point. Depth shifts the curve up from d1 to d2 (~+28% at
4 MiB), then backs off slightly: d2 > d4 > d8 at the large-chunk end, with
d4's best point (2 MiB, 845.1) statistically at d2's 2 MiB plateau level
(857.7 ± MAD ~1%). The historical anchor region ≤64K is a distinct
CPU-overhead-dominated regime (see CPU COST) and is 1.3–5× slower than the
plateau region at every depth.

Rule-trace transparency (prereg §12, priority order): the extension rule
(PLATEAU NOT REACHED) requires the last pair to be material (≥3%) at ≥2
depths with a boundary peak — only d2 qualifies (+4.86%), so it does NOT
fire; the sweet-region rule requires plateau entry at ≥2 depths — located
at d2/d4/d8, so HOST-LOCAL SWEET REGION LOCATED. Depth 1's failure to
plateau is reported but does not override the frozen rule.

## RESOURCE TRADEOFF (throughput × instructions/byte × in-flight bytes)

Pareto frontier (non-dominated over max throughput, min instructions/byte,
min in-flight bytes): **31 of 60 cells**.

- ABSOLUTE PEAK: 4 MiB × d2 — 885.9 MiB/s at 8 MiB in-flight
  (tested-range peak, at the sampled boundary).
- NEAR-PEAK LOW-RESOURCE: 1.5 MiB × d2 — 833.1 MiB/s (94.0% of peak) at
  HALF the in-flight bytes (4 MiB) and ~28% less RSS (8.8 MiB vs
  12.3 MiB); its CPU cost is ~1.8× more instructions/byte than the peak
  cell (0.0122 vs 0.0066 — per-operation overhead amortized over fewer
  bytes), i.e. the smaller footprint trades memory and CPU efficiency
  for ~2% throughput. 1 MiB × d2 extends the trade: 756.3 MiB/s (85.4%)
  at 2 MiB in-flight.
- The frontier splits into d1 (15 points — the low-in-flight,
  low-throughput tail from 16K to 4M) and d2 (15 points — 384K and up);
  d4 contributes exactly 1 point (its 2 MiB peak) and **d8 is entirely
  OFF the frontier** — every d8 cell is dominated by a d2 cell with
  higher throughput at LESS in-flight memory (e.g. 4M×d2: 885.9 MiB/s at
  8 MiB vs 4M×d8: 814.9 MiB/s at 32 MiB). Under the frozen three-axis
  rule there is no point choosing depth 8 on Host-0.
- RSS tracks in-flight bytes (8.8 MiB at 16K×d1 up to 36.9 MiB at
  4M×d8); minor faults are negligible everywhere.

## CPU COST

instructions/byte falls monotonically and steeply with chunk size:
0.950 at 16K → 0.0066 at 4M×d2 (≈145× span). The sub-64K region pays a
per-operation fixed cost that dominates the CPU budget; beyond the plateau
entry the curve is essentially flat (per-byte copy cost only). cycles:u is
DEMOTED campaign-wide (frequency scaling; see validity section) — no IPC,
cycles/op or efficiency-ratio claims are made. utime+stime medians follow
the same shape as instructions/byte.

## FINAL SWEET-SPOT STATUS

```
FINAL SWEET-SPOT STATUS: HOST-LOCAL SWEET REGION LOCATED
  - depth 2: plateau from 1.5 MiB; tested-range peak 885.9 MiB/s at 4 MiB
    (boundary — still rising +4.9% there; nothing beyond 4 MiB was
    sampled, so no claim is made past it)
  - depth 4: plateau from 1.5 MiB; interior peak 845.1 MiB/s at 2 MiB
  - depth 8: plateau from 768 KiB; tested-range peak 814.9 MiB/s at 4 MiB
  - depth 1: NO STABLE SWEET REGION within 16K..4M (monotone creep)
  - KNEE: NOT LOCATED at any depth (smooth surface)

PRODUCTION DEFAULT CHANGE: NO — nothing in H0 authorizes changing the
  production copy default; this is a microbench map, not an application
  materiality result.
CHUNK_SIZE CONTROL-SURFACE PROMOTION: NOT YET — chunk_size is already a
  caller-visible knob of the copy engine; H0 adds no API and promotes
  nothing into policy.
RUNTIME ADAPTATION: NO — no autotuner, no runtime chunk selection, no
  alignment/registered-buffer/splice/copy_file_range/O_DIRECT claims.
```

Claim boundary: every number above is a HOST-LOCAL RESULT on Host-0
(Haswell-EP, Fedora 44, btrfs/SATA-SSD, turbo on). No x86-wide, Haswell-
wide, Linux-wide or cross-host claim is made; H1/H2 cross-platform
comparison requires the portable runner (see README Rental Host
Quickstart) and stays descriptive until causally attributed.

## STRUCTURE

```
research/chunk-e0/
  CHUNK-E0-H0-PREREGISTRATION.md   frozen design (authority)
  CHUNK-E0-H0-REPORT.md            this report
  README.md                        navigation + rental-host quickstart
  campaign.json                    machine mirror of the frozen matrix
  run-host.sh / scripts/run_host.py  portable one-command host runner
  scripts/chunk_e0.py              session driver (generate/validate/sweep/
                                   summarize; env-var path overrides; --resume)
  scripts/plot_chunk_e0.py         SVG plot generator (prereg §13)
  scripts/import_host.py           archive verify + import
  results/chunk-e0-h0-validate-native-1/   harness validation (14 runs)
  results/chunk-e0-h0-sweep-native-1/      OFFICIAL session (420 runs)
  plots/throughput-vs-chunk-d{1,2,4,8}.svg
  plots/instructions-per-byte-vs-chunk-d{1,2,4,8}.svg
  plots/throughput-vs-inflight.svg
bench/chunk_e0_bench.cpp           engine bench (production copy path)
xmake/benchmarks.lua               chunk_e0_bench target wiring
```

Sessions are immutable (append-only raw evidence; no per-run files). The
runner/orchestrator never re-implements measurement: `run_host.py`
delegates every run to `chunk_e0.py`, validates the frozen matrix against
`campaign.json` at preflight (fails closed), and marks non-`full` profiles
NOT FORMAL EVIDENCE.

## STATUS / NEXT

```
STATUS: #270 OPEN, PR DRAFT (DO NOT MERGE)
NEXT:   adversarial review of preregistration, driver, sessions, analysis
        and this report. No production change, no issue close, no merge
        from this round. Cross-platform (H1/H2) work is OUT OF SCOPE here.
```
