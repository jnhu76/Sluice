# CHUNK-E0 — chunk-size × depth sweet-spot map (#270)

Execution authority: issue #270 (roadmap #259). Phase H0 = Host-0
bare-metal x86-64 performance surface of the CURRENT production buffered
READ + WRITE copy engine over chunk_size × pipeline depth.

## Entry points

| Artifact | Purpose |
| --- | --- |
| `CHUNK-E0-H0-PREREGISTRATION.md` | Frozen experiment design (scope/hard exclusions, host, workload 1 GiB, chunk grid 16K..4M, depth {1,2,4,8}, metrics, same-work fail-closed, R=7 seeded interleaved, sweet-spot definitions, Pareto rule, stop/extension rule, verdict rules, production stop gates). Additive AMENDMENTs only. |
| `CHUNK-E0-H0-REPORT.md` | Final H0 report (host-local verdict, per-depth sweet-spot tables, Pareto, resource tradeoff, CPU cost, final status). |
| `bench/chunk_e0_bench.cpp` | Engine-only sweep bench (production `run_pipelined_copy_with_backend`, workers=1; wired in `xmake/benchmarks.lua` as `chunk_e0_bench`, `-g bench`). |
| `scripts/chunk_e0.py` | Session driver (generate / validate / sweep / summarize). |
| `scripts/plot_chunk_e0.py` | Plot generator (SVG, derived artifacts; per-depth throughput + instructions-per-byte + throughput-vs-in-flight/Pareto). |
| `results/<session-id>/` | Immutable measurement sessions (environment.json, manifest.json, gates.json, notes.md, summary.csv/json, analysis.json, raw/runs.jsonl + perf.csv). |
| `results/host-<host-id>/<session-id>/` | Imported remote-host sessions (placed by `scripts/import_host.py`; see below). |
| `scripts/run_host.py` (+ `run-host.sh`) | Portable one-command host runner for rented/remote hosts (see Rental Host Quickstart). Orchestration only — delegates every measured run to `scripts/chunk_e0.py`; never modifies the frozen design. |
| `scripts/import_host.py` | Verify + place a returned evidence archive (checksums, schema, ordering provenance, status). Never commits or edits reports. |
| `campaign.json` | Machine-readable mirror of the frozen matrix; the runner validates it against the driver constants at preflight and fails closed on divergence. The preregistration remains the sole scientific authority. |
| `plots/` | Derived plots: `throughput-vs-chunk-d{1,2,4,8}.svg`, `instructions-per-byte-vs-chunk-d{1,2,4,8}.svg`, `throughput-vs-inflight.svg`. |

## Frozen matrices

- Chunks: 16K, 32K, 64K, 96K, 128K, 192K, 256K, 384K, 512K, 768K, 1M,
  1.5M, 2M, 3M, 4M (16K/32K/64K = historical continuity anchors; focus
  region 64K → 4M).
- Depths: 1, 2, 4, 8 (pipeline depth, workers = 1).
- Workload: 1 GiB READ + WRITE file copy, deterministic repeated
  pseudo-random 4 KiB pattern, per-run dst hash == src hash (fail-closed),
  R = 7 seeded interleaved rounds, median + MAD per cell.

## Rental Host Quickstart

For a future rented H1/H2 host (x86_64 or aarch64, Fedora/RHEL or
Ubuntu/Debian). The runner never installs packages, never uses sudo, and
never changes sysctl/governor/kernel settings; it fails closed on missing
dependencies and marks every non-`full` profile as NOT FORMAL EVIDENCE.

```sh
# on the rented host: clone the repo, then
./research/chunk-e0/run-host.sh --preflight-only          # gate check, no measurement
./research/chunk-e0/run-host.sh --profile full            # the one command (formal)
#   variants: --profile verify | smoke ; --resume <session-id> after an interrupt

# back home: verify and place the returned archive
python3 research/chunk-e0/scripts/import_host.py \
    research/chunk-e0/archives/chunk-e0-<...>.tar.zst
```

The runner packages the session into a single `archives/*.tar.zst` (fallback
`.tar.gz`) with inner `SHA256SUMS` and a printed archive sha256 — take that
one file away and shut the host down. Claims are HOST-LOCAL ONLY; the runner
makes no cross-host causality judgments and never touches the preregistration.

## Verdict (see CHUNK-E0-H0-REPORT.md)

`HOST-LOCAL SWEET REGION LOCATED` — plateau entry at depth 2 (1.5 MiB),
depth 4 (1.5 MiB) and depth 8 (768 KiB); depth 1 does not plateau inside
16K..4M; KNEE NOT LOCATED at any depth. Depth 8 is entirely off the
Pareto frontier. This campaign establishes the Host-0 surface only; it
makes no cross-host or production-default claim.
