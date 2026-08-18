# Performance Attribution Evidence

Canonical, runner-produced performance-evidence artifacts
(`scripts/bench/perf-attribution.py`). Never hand-created: every JSON here
was produced by a real benchmark run and records its own git SHA, dirty
state (+ provenance note when dirty), environment fingerprint, workload
parameters, warmups/iterations, raw per-iteration samples, and derived
statistics.

Validate: `python3 scripts/bench/perf-evidence-validate.py` (runs in the
pre-push gate and CI). Methodology:
[`docs/verification/performance-engineering.md`](../../verification/performance-engineering.md);
round-1 case study:
[`docs/verification/performance-attribution.md`](../../verification/performance-attribution.md).

## Round 1 — grep (2026-08)

| Artifact | What it is |
|----------|------------|
| `round1-grep-v1-ladder.json` | V1 (per-line `std::search`) matcher ladder, 256 MiB, measured at baseline commit `b5657ae` with the measurement-instrument overlay described in its note |
| `round1-grep-v2-ladder.json` | V2 (chunk-level anchor scan) matcher ladder, 256 MiB, same instrument, clean tree |
| `round1-grep-v2-cli.json` | L6 CLI matrix (sluice-grep vs GNU grep vs ripgrep), 1 GiB, byte-equality + exit-code + output-size records |
| `round1-grep-v2-perf.json` | `perf stat` counters for one sluice-grep CLI run (PMU counters are diagnostic evidence, not optimization authorization) |

Baseline and candidate were measured on the same machine in the same
session (fingerprint inside each artifact; the compare command warns on any
material mismatch). See the case study for interpretation and its
variance caveats.
