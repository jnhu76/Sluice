# RX-1 analysis tables (machine source: analysis.json)

- valid attribution runs: 222 / 288 (invalid: 18)
- accuracy C = 0.9955, E = 0.9820, Δ = -1.35 pp [95% CI -3.60, +0.45]
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

### Observability tax

| mode | n | throughput MB/s (median) | thr tax % | p50 ns | p99 ns | cpu cores | ctxt/kop |
|---|---|---|---|---|---|---|---|
| obs_off | 16 | 8331.5 | +0.00 | 1268889 | 2095006 | 3.90 | 281.5 |
| obs_low | 16 | 7728.3 | +7.24 | 1309047 | 2065014 | 3.88 | 326.9 |
| obs_high | 16 | 8571.6 | -2.88 | 1312176 | 2065866 | 3.74 | 311.5 |

**Verdict gate: NOT SUPPORTED / STOP EXPANSION**

