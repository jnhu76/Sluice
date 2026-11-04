# round-e5-20261104 — bench-side artifacts (uring-submit-policy)

Machine-readable evidence for the E5 2026-11-04 round; the narrative record is
[`docs/research/uring-submit-policy/round-e5-20261104/`](../../../../../docs/research/uring-submit-policy/round-e5-20261104/README.md).
Everything here is copied verbatim from the execution archive at
`/home/jnhu/sluice-595-e5/` (Layer A); nothing was regenerated or edited.
Coverage of the full archive: `MANIFEST.sha256` here is the verbatim copy of
that archive's 1,859-entry manifest (see `FULL_ARCHIVE.md`).

| Path | Contents |
|---|---|
| `patch.diff` | The complete research patch vs base `271b81bf` (6 files: transport/backend gate + 11-oracle test + bench D2/D3 selector + test targets) |
| `build-notes.md` | Build flags, toolchain, release binary SHA256s, case ids, arm-selector smoke check |
| `PREREGISTERED.md` | The frozen protocol: thresholds, pairing, A/A gates, machinery determination, the sanctioned repair, decision gates |
| `analysis/pair_runner.py` | Experiment driver (taskset-pinned invocations, wait4 rusage, JSONL aggregation) |
| `analysis/analyze.py` | Pair statistics, bootstrap CI (seed 5950), A/A and A/B gates, Holm control family |
| `analysis/*.json` | Stored analysis receipts: A/A pre-repair, A/A SMT alternative, A/A repaired (m5), A/B main, controls family |
| `analysis/summary-tables.md` | Consolidated pair statistics for every cell incl. placement and noise floors |
| `analysis/reanalysis-receipt.md` | Independent recomputation + measurement accounting audit (this archival round) |
| `raw-minimal/<phase>/results.jsonl` | Verbatim per-invocation records — the decision data (what `analyze.py` reads) |
| `raw-minimal/<phase>/header.json` | Phase configuration freeze (binary hash, case, cpu, env, measured count) |
| `raw-minimal/EXCLUDED.md` | What was left in the full archive and why |
| `correctness/*.log` | A-mode suite, B-mode suite, sanitizer lane, mutant receipts |
| `mechanism/strace-ledger.tsv` | enters/op per arm and shape + ledger reconciliation |
| `mechanism/pmu-w02-4k-d16.txt` | Raw `perf stat` output, both arms |

`raw-minimal` phase dirs: `ab-main-w02-4k-d16` (main A/B), `aa-calib-*` and
`aa-repair-*` (calibration + repair receipts), `aa-noise-*` (per-shape noise
floors), `ctl-d1/d2/d2-repl/d3/64k/w03-d4/w03-d16/w03-d64/threadpool/saturate`
(negative controls), `place-w02-cpu8`, `place-w03-smt`, `place-w03-cross`
(placement), `aa-probe-cpu8` (rejected repair alternative), `harness-smoke`
(machinery validation, excluded from analysis by protocol), `threadcheck-w02`
(/proc accounting consistency run).

Recompute commands: [docs .../reproduction.md](../../../../../docs/research/uring-submit-policy/round-e5-20261104/reproduction.md).
