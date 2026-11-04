# results.md — measured cells, statistics, and their receipts

All statistics below were **recomputed from the committed raw pair data**
(`raw-minimal/*/results.jsonl`) with the committed analysis scripts and match
the stored analysis JSONs exactly — see
[`analysis/reanalysis-receipt.md`](../../../../../bench/research/uring-submit-policy/round-e5-20261104/analysis/reanalysis-receipt.md)
(`REANALYSIS_MATCH = YES`).

Conventions: 24 balanced interleaved pairs per cell unless stated; bootstrap
95 % CI over pairs (10,000 resamples, seed 5950); "B worse" orientation for
cpu/wall/p95/p99/rusage — throughput is displayed as **B throughput
change** (positive = B faster) with its oriented value in brackets where the
CI is quoted. Each pair member = 5 measured reps of 20,000 valid reads
(the sanctioned A/A repair; [protocol.md](protocol.md) §5).

## 1. Main shape — W-02 4 KiB d16

| metric | B vs A | 95 % CI |
|---|---|---|
| whole-executor CPU/op | **−26.46 %** | [−26.89, −26.02] |
| wall/op | −26.49 % | [−26.93, −26.05] |
| throughput (B faster) | **+36.07 %** | [+35.26, +36.88] |
| p50 | −32.13 % | [−32.34, −31.91] |
| p95 | −30.77 % | [−31.95, −29.39] |
| p99 | −29.23 % | [−30.85, −27.34] |
| whole-invocation rusage (secondary view) | −26.76 % | [−27.02, −26.51] |

Absolute: CPU/op 2,925.8 → 2,151.6 ns; throughput 341,739 → 464,952 ops/s;
p99 74.0 → 52.4 µs. Receipts: `analysis/ab-main-w02-4k-d16.json`,
`raw-minimal/ab-main-w02-4k-d16/`.

## 2. Negative controls (the nine registered shapes; D2 replication separate)

| shape | CPU | tput (B) | p95 | p99 | verdict |
|---|---|---|---|---|---|
| D1 | +0.09 % | +0.08 % | +0.07 % | +0.48 % | PASS (neutral) |
| **D2** | +4.17 % [+3.53,+4.83] | **−3.99 %** [+3.40,+4.60] | **+5.69 %** [+4.90,+6.50] | **+7.45 %** [+5.77,+9.38] | **FAIL** |
| D2 replication (confirmation only) | +3.05 % | −2.95 % | +4.98 % | +5.81 % | confirms direction |
| D3 | −6.81 % | +7.34 % | −11.57 % | −9.13 % | PASS (improves) |
| 64 KiB d16 | −0.72 % | −0.82 % | +0.02 % | −0.00 % | PASS |
| W-03 d4 | +0.07 % | −0.02 % | +0.61 % (hw 3.88 pp) | +0.72 % | mean in-band; p95 **not-establishable** |
| W-03 d16 | −0.05 % | −0.07 % | +0.11 % | +0.03 % | PASS |
| W-03 d64 | +0.11 % | +0.06 % | +0.33 % | −0.31 % (hw 7.21 pp) | mean in-band; p99 **not-establishable** |
| threadpool d16 | +0.25 % | +0.20 % | +0.18 % | +0.22 % | PASS |
| saturation | −35.70 % | +56.14 % | n/a (lat_n=0) | n/a | PASS |

Receipts: `analysis/controls-family.json` (34 Holm entries, 9 shapes),
`raw-minimal/ctl-*/`, `raw-minimal/ctl-d2-repl/`.

### 2.1 D2 — precise wording (corrected record)

- D2 **CPU** +4.17 % [+3.53, +4.83] (replication +3.05 %) is **within** the
  frozen 5 % CPU bound in both runs. The performance-protection failure is
  triggered by the **p95 (+5.69 % > 5 %)**, **p99 (+7.45 % > 5 %)** and
  **throughput (−3.99 % > 3 % drop)** conditions, each Holm-rejected.
- It is real, not noise: tight CI, replication, A/A noise floor 0.5–1.6 pp on
  this shape, and the syscall ledger shows enters unchanged (3.0002 → 3.0002) —
  one held request per pass with zero amortization, only added delay.
- A Holm robustness note (post-hoc, disclosed): adding the unregistered
  D2-replication cell to the family would additionally push D2 CPU past its
  Holm threshold (p = 0.0078 vs adjusted α = 0.00556). The registered family
  does not include it; either membership yields the same overall verdict.

### 2.2 W-03 d4 p95 / W-03 d64 p99 — power, not evidenced harm

Both cells have means ≈ 0 (+0.61 % / −0.31 %) but A/A noise floors at or above
the 5 pp bound (3.88 pp / 7.21 pp), so non-inferiority cannot be established at
24 pairs. They are recorded as **statistical-power failures, not measured
regressions**; any future claim about these two cells needs more pairs or a
tighter quantile estimator.

## 3. A/A calibration and noise floors

| phase | cpu hw | tput hw | p99 hw |
|---|---|---|---|
| pre-repair (m1) | 2.44 pp | 2.50 pp | **3.71 pp FAIL** |
| repaired (m5) | 0.44 pp | 0.43 pp | 1.08 pp |
| D2 floor | 0.53 pp | 0.53 pp | 1.61 pp |
| W-03 d4 floor | 1.05 pp | 1.05 pp | 1.88 pp (p95 3.88 pp) |
| W-03 d64 floor | 1.10 pp | 1.10 pp | 7.21 pp |

Receipts: `analysis/aa-calib-w02-4k-d16.json` (pre-repair),
`analysis/aa-repair-w02-4k-d16-m5.json` (repaired),
`analysis/aa-calib2-w02-4k-d16-smt.json` + `raw-minimal/aa-probe-cpu8/`
(rejected alternatives), `raw-minimal/aa-noise-*/` (floors).

## 4. Placement (8 pairs each, safety-only)

| placement | CPU | tput (B) | p99 |
|---|---|---|---|
| W-02 on cpu8 | −25.16 % | +33.73 % | −25.99 % |
| W-03 (2,12) SMT | +0.86 % | +0.76 % | +0.65 % |
| W-03 (2,8) cross | +1.58 % | +1.33 % | +0.65 % |

The main effect replicates off cpu2. W-03 small positives carry no enter change
(3.0002 → 3.0002) and sit within the ±1.6 % placement band — placement noise,
not mechanism. Receipt: `raw-minimal/place-*/`.

## 5. Mechanism ledger (`mechanism/strace-ledger.tsv`)

| shape | enters/op A | enters/op B | reduction | submitted = completed |
|---|---|---|---|---|
| W-02 4K d16 | 3.0002 | **0.5630** | **81.2 %** | 20000/20000, 0 errors |
| W-02 4K d2 | 3.0002 | 3.0002 | 0 % | 20000/20000 |
| W-02 4K d3 | 3.0002 | 2.0004 | 33.3 % | 20000/20000 |
| W-02 64K d16 | 3.0020 | 3.0020 | 0 % | 2000/2000 |
| W-02 saturate | 3.0013 | 0.5759 | 80.8 % | 3200/3200 |
| W-03 d16 | 3.0002 | 3.0002 | 0 % | 20000/20000 |

Zero completion errors, zero SQ-full retries in every arm. **Scope**: "enters"
counts *all* `io_uring_enter` syscalls per valid read (`strace -f -c`),
submit and wait uses combined; a pure-submit-only split was not instrumented.
The Mechanism gate survives any wait attribution under the archived-data bound
derived in the [reanalysis receipt §3](../../../../../bench/research/uring-submit-policy/round-e5-20261104/analysis/reanalysis-receipt.md);
the exact split remains an observation gap (RQ3-adjacent).

## 6. PMU (`mechanism/pmu-w02-4k-d16.txt`, 3 interleaved reps/arm, 100 % enabled)

| counter | A | B | Δ |
|---|---|---|---|
| cycles | 776.8 M | 607.0 M | −21.8 % |
| instructions | 776.3 M | 641.4 M | −17.4 % |
| task-clock | 245.7 ms | 180.9 ms | −26.4 % |
| context-switches | 27,335 | 10,935 | −60.0 % |
| cpu-migrations / page-faults | 1 / 843 | 1 / 847 | neutral |

## 7. Resource gate

Peak RSS identical both arms (19,764 KiB, +0.00 %; `raw-minimal/ab-main-*`
`ru_maxrss_kb`). Code audit of the patch: no new queue/thread/timer/per-request
allocation ([mechanism.md](mechanism.md) §1).
