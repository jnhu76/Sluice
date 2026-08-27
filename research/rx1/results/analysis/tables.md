# RX-1 analysis tables (machine source: analysis.json)

- attribution-matrix runs: 240; valid: 222; invalid: 18; observation-tax runs: 48 (separate matrix, excluded from the attribution denominator)
- accuracy C = 0.9955, E = 0.9820, Δ = -1.35 pp [95% CI -3.60, +0.45] (preregistered run-level paired bootstrap)
- ROBUSTNESS ANALYSIS — NOT PRIMARY PREREGISTERED SCORE: cell-level paired block bootstrap (unit = workload-shape × intervention, 28 cells): Δ = -1.35 pp [95% CI -4.55, +0.91]
- macro-F1 C = 0.9945, E = 0.9788
- UNKNOWN rate C = 0.000, E = 0.000; wrong-cause C = 0.005, E = 0.018

### C confusion (rows = ground truth)

true\pred | CONTROL | APP_PIPELINE_LIMITED | REQUEST_CAPACITY_LIMITED | THREADPOOL_WORKER_LIMITED | CPU_CONTENDED | IO_SERVICE_CONTENDED | UNKNOWN
--- | --- | --- | --- | --- | --- | --- | ---
CONTROL | 29 | 0 | 0 | 1 | 0 | 0 | 0
APP_PIPELINE_LIMITED | 0 | 48 | 0 | 0 | 0 | 0 | 0
REQUEST_CAPACITY_LIMITED | 0 | 0 | 48 | 0 | 0 | 0 | 0
THREADPOOL_WORKER_LIMITED | 0 | 0 | 0 | 48 | 0 | 0 | 0
CPU_CONTENDED | 0 | 0 | 0 | 0 | 48 | 0 | 0
IO_SERVICE_CONTENDED | 0 | 0 | 0 | 0 | 0 | 0 | 0
UNKNOWN | 0 | 0 | 0 | 0 | 0 | 0 | 0

### E confusion (rows = ground truth)

true\pred | CONTROL | APP_PIPELINE_LIMITED | REQUEST_CAPACITY_LIMITED | THREADPOOL_WORKER_LIMITED | CPU_CONTENDED | IO_SERVICE_CONTENDED | UNKNOWN
--- | --- | --- | --- | --- | --- | --- | ---
CONTROL | 30 | 0 | 0 | 0 | 0 | 0 | 0
APP_PIPELINE_LIMITED | 0 | 48 | 0 | 0 | 0 | 0 | 0
REQUEST_CAPACITY_LIMITED | 0 | 0 | 48 | 0 | 0 | 0 | 0
THREADPOOL_WORKER_LIMITED | 4 | 0 | 0 | 44 | 0 | 0 | 0
CPU_CONTENDED | 0 | 0 | 0 | 0 | 48 | 0 | 0
IO_SERVICE_CONTENDED | 0 | 0 | 0 | 0 | 0 | 0 | 0
UNKNOWN | 0 | 0 | 0 | 0 | 0 | 0 | 0

### Paired transitions C→E

| transition | count |
|---|---|
| wrong_to_right | 1 |
| unknown_to_right | 0 |
| right_to_wrong | 4 |
| right_to_unknown | 0 |
| right_to_right | 217 |
| wrong_to_wrong | 0 |
| unknown_to_unknown | 0 |
| wrong_to_unknown | 0 |
| unknown_to_wrong | 0 |

### Per-class recall C vs E

| class | recall C | recall E |
|---|---|---|
| CONTROL | 0.967 | 1.000 |
| APP_PIPELINE_LIMITED | 1.000 | 1.000 |
| REQUEST_CAPACITY_LIMITED | 1.000 | 1.000 |
| THREADPOOL_WORKER_LIMITED | 1.000 | 0.917 |
| CPU_CONTENDED | 1.000 | 1.000 |
| IO_SERVICE_CONTENDED | 0.000 | 0.000 |
| UNKNOWN | 0.000 | 0.000 |

### Observability tax (paired per shape vs same-shape OBS-OFF)

| shape | mode | n | throughput MB/s (median) | thr tax % | p99 µs (median) | p99 tax % |
|---|---|---|---|---|---|---|
| read/65536 | off | 8 | 12191.8 | baseline | 259 | baseline |
| read/65536 | low | 8 | 11931.6 | +2.1 | 263 | +1.4 |
| read/65536 | high | 8 | 11933.2 | +2.1 | 254 | -1.9 |
| read/1048576 | off | 8 | 5728.1 | baseline | 4335 | baseline |
| read/1048576 | low | 8 | 5646.1 | +1.4 | 5288 | +22.0 |
| read/1048576 | high | 8 | 5501.8 | +4.0 | 4729 | +9.1 |

- aggregate (median of per-shape normalized effects): OBS-LOW throughput tax +1.8%, OBS-HIGH +3.0%
- No reproducible or monotonic observation-tax signal was established at the current sample size.

**Verdict gate: NOT SUPPORTED / STOP EXPANSION**
