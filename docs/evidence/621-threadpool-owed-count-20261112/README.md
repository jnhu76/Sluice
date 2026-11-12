# 621 ThreadPool owed-count productization (2026-11-12)

Round record for the #621 execution round that admitted the ThreadPool
owed-event zero-scan gate (umbrella #621; application-path authority #604;
the production mechanism mirror is the io_uring `owed_count_` from H2b/#564,
whose record is `docs/evidence/h2b-owed-count-productization-20261021/`).

- `REPORT.md` — full round report: repository reality, cost ledger,
  candidate selection and ranked alternatives, semantic mapping,
  mutant receipts, A/A + A/B results, verdicts.
- `production.diff` — the reviewed candidate diff (PR branch
  `product/621-threadpool-owed-count-gate`, base `f2c42535`).
- `battery-*.txt` — registered-test receipts per configuration at the PR
  head (release/debug × liburing y/n: 80/80 and 59/59, 0 failed; tsan and
  asan on the concurrency suites).
- `mutant-receipts.txt` — four named mutant kills with explanations.
- `formal-ab/` — 24-pair interleaved A/A and A/B raw tables (tsv),
  bootstrap analysis output, and the perf-stat mechanism summary.

Off-repo artifacts (external workspace per
`docs/performance/methodology.md`): `/home/jnhu/sluice-621-round/` holds
the timing binaries (sha256 in its `MANIFEST.sha256`; arm B binary
reproduced byte-identical from the PR head), runner/analysis scripts,
perf profile data, and the 256 MiB fixture digest. Reproduction commands
are in `REPORT.md` §4.

Headline (real consumer `sluice-copy`, 256 MiB, 4 KiB buffers, depth 4,
1 worker, btrfs warm, 24 interleaved pairs, bootstrap 95% CI seed 5950):

- CPU per oracle-verified copy task −5.58% [−6.45, −4.71]% vs an A/A
  calibration band of ±1.2%; wall −6.53% [−7.55, −5.53]%; user-space
  instructions −22.1%.
- Negative control (64 KiB buffers, depth 1): +0.10% [−1.56, +1.77]% — no
  statistically resolved regression in the measured negative-control cell.
  No non-inferiority margin was pre-registered for this round, so no
  non-inferiority claim is made; the +1.77% CI upper bound cannot exclude
  a CPU regression that small.
