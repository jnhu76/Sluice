# RX-1 — Controlled Attribution Falsification Gate

Research experiment (#234 RX-1): does Sluice's AC-1a explicit resource
information materially improve bottleneck attribution over ordinary external
telemetry, for controlled ThreadPool workloads?

- `RX1_METHOD.md` — human-readable method (frozen)
- `rx1_protocol_v1.json` — machine-readable frozen protocol
- `bench/rx1_workload_bench.cpp` — workload driver (xmake target
  `rx1_workload_bench`, links production `sluice_async` only)
- `scripts/rx1.py` — orchestrator (`env` / `run` / `freeze` / `classify` /
  `analyze` / `self-test`)
- `scripts/rx1_classify.py` — frozen classifiers C/E, feature whitelist,
  validity gates, scoring
- `synth/synth_cases.json` — synthetic classifier test dataset with known
  expected outputs (proves the pipeline before any real run)
- `results/` — pilot (calibration only) and formal artifacts, analysis

RX-1 touches no production source, no public API, no observability surface.
