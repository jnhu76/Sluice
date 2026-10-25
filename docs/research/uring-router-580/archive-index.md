# #580 research archive index

Single map from every research stage to its surviving evidence, the tool
that produced it, its identity, and the verdict it supports. This file is
the entry point for auditing the closed issue; `results/manifest.json`
remains the per-file SHA-256 authority.

Baseline identities:

- Frozen production base: `46157fd1f4a1a4afec22d8834c86462b245b576b` (master)
- Research measurement HEAD: `2c00c36a8a44b2c2d0909c8e5178c736e99ac545`
  (chain: `ca1e7c67` R0–R8 → `350e7f7f` R9 → `2c00c36a` R9 delta review)
- Archive revision: this branch after the archival commits (production
  `src/`/`include/`/spec/CI restored to byte-identity with the base; see
  Gate A in the PR)
- Full research gate patch:
  `patches/research-gates-full.patch`
  SHA-256 `3bb61fa211e62721e13b0020cf7a9a67009195d2c1b5375330edcdd4e8fdd581`
  (= `git diff 46157fd1 2c00c36a -- src/async/`; admitted by
  base → apply → verify → reverse → base round-trip in a fresh worktree)
- Superseded partial patch: `patches/instrumentation-and-arm-gates.patch`
  SHA-256 `69cc2fcba2a1ab7169a58f9328c5dbeec6c29ad335c8341c6b293d1a8e95e291`
  (R0–R8-era; lacks the R9 clflush gate — the delta between the two patches
  is exactly the 8-line `SLUICE_580_CLFLUSH_ROUTER` block; retained for
  provenance, do not use for rebuilds)

## Stage map

| Stage | Evidence (committed) | Script | Identity | Verdict |
|---|---|---|---|---|
| R0 environment | `environment.md`, `results/manifest.json` | — | base `46157fd1` | PASS |
| R1 source DAG | `source-dag.md`, codegen identity in manifest | — | TU SHAs in manifest | PASS |
| R2 trigger matrix | `results/trigger-matrix.jsonl` (WSL), `results/e5/trigger-matrix-e5.jsonl`, `results/e5/trigger-matrix.jsonl` | `build_probe.sh`, `run_trigger_matrix.sh` | probe binaries (see below) | NO_TRIGGER |
| R3–R4 design / lifecycle | `design-alternatives.md`, `lifecycle-cost.md` | — | — (analytical) | NOT_ADMITTED |
| R5 correctness oracle | `results/e5/oracle_{baseline,we2,false_miss,slot_only,baseline_restore}.log` | `run_oracle_variants.sh` | oracle binaries (E5) | PASS: GREEN / RED(named) / GREEN |
| R6 paired A/B | `results/{ab,aa}-pairs.jsonl`, `results/e5/{ab,aa}-pairs-e5.jsonl`, `results/e5/conformance_e5_*.log`, disassembly both hosts | `build_arms.sh`, `run_ab.sh`, `reduce_ab.py` | arm binaries + disasm in results | NO_CHANGE (A/A-bounded) |
| R7–R8 reviews / decision | `reviews.md`, `decision.md`, `wsl-results.md` | `reduce_ab.py` | raw jsonl hashes in manifest | APPROVE / NO_TRIGGER_NO_CHANGE |
| R9 cache locality | `results/cache-{wsl,e5}.jsonl`, `cache-flush-sweep-{wsl,e5}.jsonl`, reduced JSONs, `results/pmu*/` (17 perf `.data` + stat/report text) | `build_cache.sh`, `run_cache_matrix.py`, `run_pmu_cache.sh`, `reduce_cache.py` | r9_binaries in manifest | HOT_PATH_ROBUST (scoped) |
| R9 delta review | `reviews.md` (delta record), revised `cache-results.md` §3–§5, `decision.md` boundaries | independent recomputation script (session) | `2c00c36a` | APPROVE |
| Archive | this file, `README.md`, `patches/`, `prepare-research-worktree.sh` receipts | `prepare-research-worktree.sh` | patch SHA above + generated `prepared-receipt.json` | per Gate A–F below |

## Evidence grades

- **Historical measurement** — the committed JSONL/logs/PMU text above,
  produced by the runs of record; every file SHA-256'd in `manifest.json`
  (68 entries). Never overwritten by replays: all runner defaults were
  moved to `build-research/replay/` and replay receipts are separate files.
- **Recomputed analysis** — reducers re-run over the raw JSONL (bootstrap
  medians/CI, probe tabulation). Gate E of the archive acceptance re-ran
  them and matched the published figures.
- **Rebuilt executable** — binaries produced by
  `prepare-research-worktree.sh` from base + archived patch. They hash
  differently from the historical binaries by construction (build path,
  timestamp-independent but toolchain-dependent); the generated
  `prepared-receipt.json` records them. Historical and rebuilt identities
  are never conflated.
- **Fresh replay** — oracle RED/GREEN suite and probe/cache smokes run in
  the isolated worktree (`--mode replay`), receipts under the worktree's
  `build-research/`.
- **Analytical estimate** — Candidate A/B lifecycle entries (lower bounds,
  never built or measured); the R9 cold-refill +8% (E5) / +29% (WSL)
  figures (capacity-gradient differential, not a directly measured pure
  miss cost); the 3–5 cycles/probe warm attribution band.
- **Untested hypothesis** — the dispatch-batching explanation of the
  D=512 net-faster inversion; any index-vs-scan comparison in the cold
  regime (no index arm exists).

## Accessibility of non-committed evidence

- **Historical WSL binaries** (`cache_ab`, `cache_ab_flush`, `cache_probe`,
  `lookup_probe`, `router_ab_*`, conformance/integration binaries): retained
  untracked in the research worktree's `build-research/` on the WSL host;
  SHA-256-verified equal to the manifest's `r9_binaries` WSL entries. Not in
  git (large); rebuildable via the archived patch.
- **Historical E5 binaries**: on the E5 server research tree
  (`~/Source/Sluice-580-wt/build-research/`), manifest records their SHAs.
- **R6-era raw perf.data** (5 files, `/tmp` on both hosts): tmpfs, no
  longer accessible — disclosed in `manifest.json`
  `uncommitted_raw_artifacts`. The surviving R6 PMU evidence is the
  extracted text (`results/pmu*/events-*.txt`, `stat*/report*` where
  present, `shares-e5.txt`); conclusions cite those, not the lost raw
  samples.
- **R9 perf.data**: committed (17 `.data` files under `results/pmu*/`) —
  re analyzable.

## Reproduction entry point

`bench/research/uring-router-580/prepare-research-worktree.sh`
(`--mode verify` builds all research arms from base + patch in a throwaway
worktree; `--mode replay` additionally runs the enforced oracle suite and
deterministic smokes). Build scripts fail closed (exit 2) on unpatched
trees. Historical receipts (manifest) and rebuild receipts
(`prepared-receipt.json`) are kept distinct by construction.

Accepted-run receipts (WSL, gcc 15.2.0, 2026-10-25) are committed under
`results/archive/`: `replay-receipt-wsl.json` (12 binaries),
`replay/oracle_*.log` (enforced suite: baseline/we2/slot_only GREEN 23/23,
FALSE_MISS RED by 3 named s2 assertions, restored-GREEN), `replay/
trigger-smoke.json` (lookups/install 1.0, 0 misses), `replay/cache-smoke-*`
and `gate-e-reproduction.md` (all published key figures reproduced exactly).
Notably the rebuilt `cache_ab` is **byte-identical** to the historical
binary that produced the committed WSL results (same toolchain + flags +
patched sources ⇒ same SHA-256), the strongest form of rebuild evidence.
The E5 fresh rebuild is `NOT_RUN`: the E5 tree is not a git checkout and
the host cannot reach the repository remote; E5-side identity remains
covered by the R9 codegen proof recorded in the manifest.
