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
ADVERSARIAL REVIEW REMEDIATION (2026-09-01):
  two-segment knee regression bug:  FIXED (analysis code only)
  formal H0 benchmark rerun:        NO
  raw evidence:                     UNCHANGED (420 runs, 0 gate errors)
  sustained-flatness diagnostic:    ADDED (post-hoc, not a prereg change)
  resource arithmetic:              FIXED (in-flight bytes, throughput gap)
  near-peak candidate re-evaluated: 2 MiB x d2 (was 1.5 MiB x d2)
  CPU-cost claim:                   TIGHTENED
  <algorithm> portability include:  FIXED (bit-identical binary on Host-0)
  runner FORMAL_ELIGIBLE summary:   ADDED
  frozen preregistration text:      UNTOUCHED; frozen thresholds UNTOUCHED
```

```
FROZEN-RULE VERDICT:  HOST-LOCAL SWEET REGION LOCATED (Host-0 only).
                      Plateau entry located at depth 2 (1.5 MiB), depth 4
                      (1.5 MiB) and depth 8 (768 KiB). Depth 1 never
                      plateaus inside the tested range (see per-depth
                      block d1). The frozen verdict is UNCHANGED by the
                      remediation — the plateau rule never used the buggy
                      fit.

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

## Terminology discipline (remediation)

Four different questions are kept separate everywhere below; they are NOT
interchangeable and MUST NOT be merged into one concept:

```text
FROZEN-RULE RESULT                what the preregistered deterministic
                                  rules output (plateau entry, knee,
                                  verdict) — the only protocol-level
                                  verdict of H0
POST-HOC ROBUSTNESS DIAGNOSTIC    adversarial checks added after the
                                  campaign (sustained-to-boundary
                                  flatness); they inform interpretation
                                  and NEVER rewrite the frozen verdict
KNEE                              deterministic piecewise-linear knee
                                  descriptor of the frozen two-segment
                                  SSE fit — not the plateau entry, not
                                  the 95% point, not an engineering
                                  recommendation
ENGINEERING CANDIDATE             an explicitly-labeled operating point
                                  derived from frozen metrics for
                                  discussion; NOT an optimum and NOT a
                                  production decision
```

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
- Raw evidence is untouched by the remediation: `raw/runs.jsonl` and
  `raw/perf.csv` byte-identical before/after; run count still 420; gate
  errors still 0. The analysis was regenerated FROM the immutable raw
  evidence — the experiment was not rerun.
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

## KNEE FIX (P0 — analysis regression, remediated)

The preregistered knee rule (frozen, unchanged): deterministic
two-segment least-squares fit on (log2 chunk, median MiB/s); breakpoint =
interior point with ≥2 points per side minimizing total SSE; labeled
KNEE only if SSE reduction ≥ 10% vs the single-line fit.

The original `two_segment_knee()` implementation was WRONG: inside each
segment it used `mean(y)` where `mean(x)` was required (slope numerator/
denominator and intercept `m - slope*m` with m = mean(y)). The segment
fits were biased garbage, the "best" two-segment SSE could exceed the
single-line SSE, and every depth reported a NEGATIVE reduction ("fit not
improved") — an artifact of the bug, not a property of the surface.

The fix computes each segment's OLS fit with separate `mean(x)` and
`mean(y)` (left and right segments independently); the 10% threshold and
all other frozen constants are untouched. Verification is by synthetic
analysis-unit diagnostics (`chunk_e0.py knee-diagnostics`, deterministic,
NOT scientific measurement):

```text
synthetic_single_line  perfect line                     -> NO KNEE   PASS
synthetic_two_segment  injected breakpoint at 1 MiB     -> KNEE at the
                       (90x vs 3x slope change)            breakpoint PASS
synthetic_flat         constant curve                   -> NO KNEE   PASS
                       (3/3 PASS)
```

Re-run on the REAL H0 raw evidence, the corrected fit DOES reach the 10%
threshold at every depth — the "KNEE NOT LOCATED everywhere / smooth
surface" statement in the pre-remediation report is RETRACTED:

```text
depth 1:  KNEE      at 384 KiB   (SSE reduction 67.4%)
depth 2:  KNEE      at 1.5 MiB   (SSE reduction 36.9%)
depth 4:  KNEE      at 1.5 MiB   (SSE reduction 52.9%)
depth 8:  KNEE      at 128 KiB   (SSE reduction 71.6%)
```

These reductions are numerically solid (residuals are MiB/s-scale, far
above float epsilon; see the numerical note in the diagnostics docstring).
A KNEE is a slope-change descriptor of the fitted curve — by itself it is
neither a plateau nor a recommendation. The knee and the plateau entry
coincide at d2/d4 (both 1.5 MiB) and differ sharply at d8 (knee 128 KiB
vs plateau entry 768 KiB) and d1 (knee 384 KiB, plateau never) — exactly
why the terms are kept separate.

## Per-depth sweet-spot blocks (median MiB/s; prereg §10 definitions)

Naming discipline: every peak sitting at chunk = 4 MiB is a TESTED-RANGE
PEAK (the largest sampled value), never an "optimum".

### depth 1
```
TESTED-RANGE PEAK:  4 MiB  — 689.5 MiB/s  (AT the sampled upper boundary)
P95 POINT:          3 MiB  (689.5 × 0.95 = 655.0)
PLATEAU:            NOT LOCATED (no two consecutive <3% pairs anywhere;
                    last-pair gain 3M→4M = +2.05%, below the 3% material
                    threshold but the whole top of the curve creeps)
KNEE:               LOCATED at 384 KiB (SSE reduction 67.4%) — a slope
                    change, NOT a plateau
SUSTAINED-TO-BOUNDARY: N/A (no frozen plateau candidate)
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
KNEE:               LOCATED at 1.5 MiB (SSE reduction 36.9%)
SUSTAINED-TO-BOUNDARY: NO — a material rise (+4.86%) reappears at 3M→4M
                    after the entry: LOCAL-FLATNESS CANDIDATE, NOT A
                    SUSTAINED PLATEAU TO THE TESTED BOUNDARY
```

### depth 4
```
TESTED-RANGE PEAK:  2 MiB  — 845.1 MiB/s (INTERIOR point, not at the
                    boundary; 3M/4M sit ~1–2% below it)
P95 POINT:          1.5 MiB (827.2 MiB/s)
PLATEAU ENTRY:      1.5 MiB
KNEE:               LOCATED at 1.5 MiB (SSE reduction 52.9%)
SUSTAINED-TO-BOUNDARY: YES — no material rise after the entry up to the
                    sampled 4 MiB boundary
```

### depth 8
```
TESTED-RANGE PEAK:  4 MiB  — 814.9 MiB/s (AT the sampled upper boundary;
                    last-pair gain 3M→4M = +0.24%, flat)
P95 POINT:          2 MiB  (784.5 MiB/s)
PLATEAU ENTRY:      768 KiB (741.3 MiB/s = 91.0% of peak)
KNEE:               LOCATED at 128 KiB (SSE reduction 71.6%) — well below
                    the plateau entry; the two rules answer different
                    questions
SUSTAINED-TO-BOUNDARY: NO — material rises at 1M→1.5M (+5.58%) and
                    2M→3M (+3.63%) after the entry: LOCAL-FLATNESS
                    CANDIDATE, NOT A SUSTAINED PLATEAU TO THE TESTED
                    BOUNDARY
```

Note the plateau-entry metric is a consecutive-pair flatness rule, not a
global proximity rule; for d8 it fires while the curve is still at 91% of
peak because the *incremental* gains have flattened. Both metrics are
reported so the distinction stays visible.

## PLATEAU ROBUSTNESS — SUSTAINED_TO_BOUNDARY_FLATNESS (post-hoc diagnostic)

POST-HOC ROBUSTNESS DIAGNOSTIC — added by the adversarial review
remediation. It does NOT rewrite the preregistration, does NOT replace
the frozen verdict, and does NOT alter raw evidence. Definition: starting
at the frozen plateau-entry candidate and up to the sampled upper
boundary (4 MiB), no further ≥3% material throughput rise may occur.
Computed mechanically from the measured medians (no expected result was
hard-coded); `plateau_entry` column is the FROZEN-RULE RESULT:

```text
depth 1:
  frozen plateau:        NOT LOCATED
  sustained-to-boundary: N/A (no frozen plateau candidate)

depth 2:
  frozen plateau:        1.5 MiB
  sustained-to-boundary: NO — material rise 3M→4M (+4.86%)
                         LOCAL-FLATNESS CANDIDATE, NOT A SUSTAINED
                         PLATEAU TO THE TESTED BOUNDARY

depth 4:
  frozen plateau:        1.5 MiB
  sustained-to-boundary: YES — flat (all pairs <3%) from entry to 4 MiB

depth 8:
  frozen plateau:        768 KiB
  sustained-to-boundary: NO — material rises 1M→1.5M (+5.58%) and
                         2M→3M (+3.63%)
                         LOCAL-FLATNESS CANDIDATE, NOT A SUSTAINED
                         PLATEAU TO THE TESTED BOUNDARY
```

Interpretation: only depth 4's frozen plateau is stable all the way to
the tested boundary. The d2 (1.5 MiB) and d8 (768 KiB) plateau entries
are LOCAL flatness findings — two consecutive <3% pairs at that point of
the curve — and MUST NOT be read as "everything from here on is plateau".
The frozen verdict vocabulary remains "plateau entry", which is what the
preregistered rule actually located; the sustained diagnostic bounds how
much stronger a claim would be unjustified.

## GLOBAL REGIME (H0, Host-0)

The corrected knee fit changes the surface description: the H0 throughput
curve is NOT best described as a smooth monotone rise — every depth has a
slope change under the frozen deterministic two-segment SSE rule (KNEE,
36.9–71.6% SSE reduction vs a single line). The shape is: steep rise through the sub-1M region, marked
slope change (d1/d2/d4: 384K–1.5M; d8: already 128K), then a flattened
tail that at d2 is still capable of a material +4.86% step into 4 MiB.
Depth shifts the curve up from d1 to d2 (~+28% at 4 MiB), then backs off
slightly: d2 > d4 > d8 at the large-chunk end, with d4's best point
(2 MiB, 845.1) within the run-to-run MAD band of d2's 2 MiB level
(857.7 ± MAD ~1%). The
historical anchor region ≤64K is a distinct CPU-overhead-dominated regime
(see CPU COST) and is 1.3–5× slower than the plateau region at every
depth.

Rule-trace transparency (prereg §12, priority order): the extension rule
(PLATEAU NOT REACHED) requires the last pair to be material (≥3%) at ≥2
depths with a boundary peak — only d2 qualifies (+4.86%), so it does NOT
fire; the sweet-region rule requires plateau entry at ≥2 depths — located
at d2/d4/d8, so HOST-LOCAL SWEET REGION LOCATED. Depth 1's failure to
plateau is reported but does not override the frozen rule. The knee
locations are descriptive additions; they feed no verdict rule.

## RESOURCE TRADEOFF (throughput × instructions/byte × in-flight bytes)

Pareto frontier (non-dominated over max throughput, min instructions/byte,
min in-flight bytes): **31 of 60 cells** (unchanged by the remediation —
the Pareto rule never used the knee fit).

Engineering candidates are stated per the prereg §11 vocabulary, with the
GLOBAL OPTIMUM status made explicit:

```text
ABSOLUTE TESTED PEAK:
  4 MiB × d2 — 885.9 MiB/s, 8 MiB in-flight
  (sampled upper boundary; d2 still rising +4.9% there)

NEAR-PEAK LOW-RESOURCE CANDIDATE:
  2 MiB × d2 — 857.7 MiB/s = 96.8% of the tested peak (gap ≈ 3.2%)
  in-flight bytes: 4 MiB — HALF of the peak cell's 8 MiB
  RSS: 8804 KiB vs 12276 KiB (≈ −28%)
  cost: 0.0098 instructions/byte vs 0.0066 (≈ +49% CPU per byte);
  utime+stime 2319 ms vs 2275 ms (≈ +2%)

secondary, smaller still:
  1.5 MiB × d2 — 833.1 MiB/s = 94.0% of the tested peak (gap ≈ 6.0%)
  in-flight bytes: 3 MiB; RSS 8800 KiB (≈ −28% vs peak);
  0.0122 instructions/byte (≈ +85% CPU per byte)
  1 MiB × d2 extends the trade: 756.3 MiB/s (85.4%) at 2 MiB in-flight.

GLOBAL OPTIMUM: NOT PROVEN — the peak sits at the sampled 4 MiB boundary
  of a grid that ends at 4 MiB; nothing beyond it was measured, and H0
  makes no claim about any configuration it did not test.
```

Arithmetic note (remediation): the pre-remediation report mis-stated the
1.5 MiB × d2 cell as "HALF the in-flight bytes (4 MiB)" and its gap to
peak as "~2%". Correct values: in-flight = 1.5 MiB × 2 = **3 MiB** (3/8
of the peak cell), and 833.1/885.9 = **94.0%**, i.e. a **6.0%** gap. The
re-evaluation promotes 2 MiB × d2 to the NEAR-PEAK LOW-RESOURCE CANDIDATE
because it dominates the 1.5 MiB point on both frozen metrics that matter
at equal-ish footprint (throughput +2.9%, instructions/byte −20% for one
additional MiB in flight, RSS equal within 4 KiB); it does NOT "dominate"
in the frozen Pareto sense (3 MiB < 4 MiB in-flight keeps 1.5 MiB × d2 on
the frontier).

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
per-operation fixed cost that dominates the CPU budget.

RETRACTED claim: "beyond the plateau entry the curve is essentially flat
(per-byte copy cost only)". The measured data contradicts it —
instructions/byte at d2 continues to fall materially deep into the
large-chunk region: 0.0122 at 1.5 MiB → 0.0098 at 2 MiB → 0.0074 at
3 MiB → 0.0066 at 4 MiB (another −46% past the plateau entry). Corrected
statement:

> Instructions/byte continues to fall materially into the large-chunk
> region, consistent with continued amortization of per-operation/control
> cost. H0 does not locate the residual per-byte floor, and this campaign
> does NOT claim that only copy cost remains.

cycles:u is DEMOTED campaign-wide (frequency scaling; see validity
section) — no IPC, cycles/op or efficiency-ratio claims are made.
utime+stime medians follow the same shape as instructions/byte.

## PORTABILITY FIX (post-measurement, source-only)

`bench/chunk_e0_bench.cpp` gained an explicit `#include <algorithm>` (for
`std::min`) — POST-MEASUREMENT PORTABILITY-ONLY SOURCE FIX. On the
measuring host (libstdc++/clang) the include was already satisfied
transitively:

```text
formal H0 measured binary sha256:
  75f1db930e2dbc6101291db102b7e6eadeb954f21e9bd2d0651e9f4acf4ffcae
post-fix source HEAD, full recompile on Host-0:
  BIT-IDENTICAL binary (same sha256 75f1db93…)
```

The formal evidence chain is therefore untouched by the source fix —
binary hash still pins the measured artifact; NO H0 rerun was performed
or needed. The include matters for other standard-library configurations
(e.g. future aarch64 hosts with different transitive-include behavior),
which is exactly where the portable runner will go next.

## RUNNER — FORMAL_ELIGIBLE (added)

`run-host.sh --preflight-only` now ends with an explicit machine-readable
verdict:

```text
FORMAL_ELIGIBLE: YES
```
or
```text
FORMAL_ELIGIBLE: NO

REASONS:
- instructions:u blocked (perf_event_paranoid/permissions)
- required compiler unavailable
- ...
```

Rationale: being able to run a wall-clock benchmark is NOT formal-campaign
eligibility. The frozen full profile records `instructions:u` per run
fail-closed, so a rented host with blocked perf counters would burn the
whole sweep into gate errors. The preflight now states eligibility BEFORE
any machine time is spent, and the full profile fails preflight fast
unless instructions:u is RELIABLE. The runner never installs packages and
never modifies system state to become eligible.

## FINAL SWEET-SPOT STATUS

```text
FROZEN-RULE VERDICT: HOST-LOCAL SWEET REGION LOCATED
  - depth 2: plateau entry 1.5 MiB; tested-range peak 885.9 MiB/s at 4 MiB
    (boundary — still rising +4.9% there; nothing beyond 4 MiB was
    sampled, so no claim is made past it); NOT sustained to boundary
  - depth 4: plateau entry 1.5 MiB, sustained to the tested boundary;
    interior peak 845.1 MiB/s at 2 MiB
  - depth 8: plateau entry 768 KiB; tested-range peak 814.9 MiB/s at 4 MiB;
    NOT sustained to boundary
  - depth 1: NO STABLE SWEET REGION within 16K..4M (monotone creep)
  - KNEE (frozen rule, corrected fit): LOCATED at every depth — d1 384 KiB,
    d2 1.5 MiB, d4 1.5 MiB, d8 128 KiB

ROBUSTNESS INTERPRETATION (post-hoc, not the verdict):
  SUSTAINED PLATEAU:  depth 1: N/A (no plateau) · depth 2: NO ·
                      depth 4: YES · depth 8: NO

ENGINEERING CANDIDATE (from frozen metrics; not an optimum):
  ABSOLUTE TESTED PEAK:          4 MiB × d2 — 885.9 MiB/s @ 8 MiB in-flight
  NEAR-PEAK LOW-RESOURCE:        2 MiB × d2 — 857.7 MiB/s (96.8% of peak)
                                 @ 4 MiB in-flight, ≈ −28% RSS
  GLOBAL OPTIMUM:                NOT PROVEN

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
                                     (--preflight-only ends with
                                     FORMAL_ELIGIBLE: YES/NO + REASONS)
  scripts/chunk_e0.py              session driver (generate/validate/sweep/
                                   summarize/knee-diagnostics; env-var path
                                   overrides; --resume)
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
NEXT:   adversarial review of the remediation (knee fix + diagnostics,
        sustained-flatness, arithmetic, CPU claim, portability fix,
        runner eligibility). No production change, no issue close, no
        merge from this round. Cross-platform (H1/H2) work is OUT OF
        SCOPE here.
```
