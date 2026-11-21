# 621 progress-wait-path perf campaign — banked process record

Owner instruction (2026-11-21): bank this campaign's process and
conclusions on the evidence branch; raw captures stay local.

- `p1/` — baseline build + smoke logs and four session snapshots
  (environment census) from 2026-11-18.
- `p2/profile/`, `p4/profile/` — profiling cells (`P-A`..`P-F`: w02/w03
  workloads, uring/threadpool/sluice-copy shapes). Only the derived
  `report-*` outputs are committed: per-run self tables
  (`report-inline-self-run-*`, `report-flat-self-run-*`), callgraph
  reports (`report-callgraph-*`), and dso/symbol cycle tables
  (`report-cycles-*`).
- `tools/`, `*/analysis/` — analysis script sources were not retained;
  only `__pycache__` byproducts exist and are excluded. Provenance gap
  recorded here rather than papered over.

Intentionally not committed (≈6.9 GB, several files above any hostable
size): every `*.data` perf capture (`record-*`, `run-*`, `overhead-*`,
`control-*`, `smoke-cycles`) and every `script-*` symbolized event dump.
They remain in the original workspace only.

Conclusions and adjudication live outside this directory: the
`[Unreleased]` #621 entries in `CHANGELOG.md`, the round-2 record in
`docs/evidence/621-round2-R1/REPORT.md`, and the committed campaign
records under `docs/evidence/621-threadpool-owed-count-20261112/`.
