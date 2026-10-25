# #593 research harness and patch archive

Research-only artifacts for the #593 E5 key-path campaign. This directory is
**experiment provenance, not product tooling**: it is not built by `xmake`, not
wired into CI, and not used by any product or test target. The repository rule
that one-shot probes belong outside the tree is superseded here only because
brief §12 explicitly requires the research harness and the test-only patches to
be retained with the research record.

Record: `docs/research/key-path-593/`.
Frozen base: `bdfd0d8d4595fd82d1cd4b099af8ed37851e6d57`.

## 1. Patches

All patches apply to the frozen base SHA. None of them belongs in a product
tree; they exist to make every measured arm reproducible.

| File | Lane / purpose | Size (patch lines; files, +/−) |
|---|---|---:|
| `patches/baseline-bench-w02-uring-core64.patch` | adds one diagnostics-gated uring C>D case to `bench/bench_w02_pipeline.cpp` | 24; 1 file, +13/−0 |
| `patches/observe-instrumentation.diff` | `SLUICE_OBSERVE` counters + per-pass trace | 835; 7 files, +321/−3 |
| `patches/k1-elision.patch` | K1-a single-lock publication snapshot + K1-b no-observer delivery gate | 241; 4 files, +83/−40 |
| `patches/k1-elision-observe.patch` | K1 elision + observe counters (`pub_snapshot_lock`) | 835; 8 files, +207/−43 |
| `patches/k2-e18-arm.patch` | E18 ungated deferred kernel submission + mutant defines + research oracles | 438; 4 files, +386/−3 |
| `patches/k2-e18-test-repins.patch` | existing-test re-pins required by deferred semantics | 144; 5 files, +43/−14 |
| `patches/k2a-final.diff` | final K2-a A/B arm (ungated), full rebuildable patch | 582; 9 files, +429/−17 |
| `patches/k3-fusion-arm.patch` | K3-a three same-lock fusions + two mutation targets | 295; 11 files, +100/−9 |
| `patches/k4-defer-arm.patch` | early K4 defer arm + wake oracle + mutation target | 263; 3 files, +224/−4 |
| `patches/k4-defer-test-repins.patch` | one re-pin (`u2b`) | 25; 1 file, +5/−2 |
| `patches/k4-defer-observe.patch` | early K4 defer + observe counters | 917; 9 files, +354/−9 |
| `patches/k4b-final.diff` | final K4-b topology+depth-gated deferred flush, full rebuildable patch | 877; 8 files, +762/−6 |
| `patches/combo-k4b-k1ab.diff` | merged combo (K4-b + K1a/b), generated from the measured merge worktree | 1,103; 10 files, +845/−46 |
| `patches/combo-k4b-k1ab-merge-rejected-hunk.rej` | the one textual conflict hunk (adjacent private-member insertion), archived for review | 17; hunk only |

`k2a-final.diff`, `k4b-final.diff` and the observe patch are the same content as
`evidence/K2-a-patch.diff`, `evidence/K4-b-patch.diff` and
`evidence/observe-patch.diff`; the names are shortened for this archive.
`SHA256SUMS` records the exact bytes.

### Applying

```sh
git worktree add /tmp/kp593 bdfd0d8d4595fd82d1cd4b099af8ed37851e6d57
cd /tmp/kp593
git apply /path/to/bench/research/key-path-593/patches/k4b-final.diff
xmake f -y --mode=release --kind=static --liburing=y -o build-bench-ring -c
xmake -y -j16 bench_w02_pipeline bench_w03_external_loop
```

The K4-b patch was verified in its session by re-applying it to a fresh worktree
at the base and comparing all eight touched files byte-for-byte
(`docs/research/key-path-593/evidence-index.md` records the receipt). The combo
patch is a reconstruction generated from the surviving measured merge worktree;
see the storage note in the evidence index.

## 2. Harness

The scripts are exactly the ones that produced the archived results, with their
original absolute paths. They will not run unmodified on another machine; they
are kept for audit and for adaptation, not as a supported runner.

| File | Purpose |
|---|---|
| `harness/pair-run-k1.sh` | K1 merged-elision AB/BA runner (W-02/W-03 d16, A/A) |
| `harness/pair-run-k2.sh` | K2 E18 discrimination AB/BA runner |
| `harness/pair-run-k3.sh` | K3 fusion AB/BA runner (d1 primary, d4) |
| `harness/pair-run-k4.sh` | K4 defer discrimination AB/BA runner |
| `harness/pair-run-k2a.sh` | final K2-a 13-case A/B runner |
| `harness/pair-run-k4b.sh` | final K4-b 13-case A/B runner |
| `harness/pair-run-combo.sh` | combo vs baseline runner |
| `harness/pair-run-combo-increment.sh` | combo vs K4-b increment runner |
| `harness/analyze_pairs.py` | K2-a/K4-b/combo reducer (10,000-iteration bootstrap, seed 593; emits `p95_ns` in `pair-results.json`) |
| `harness/analyze_pairs-k1.py` | K1 discrimination reducer (min–max range, no bootstrap) |
| `harness/analyze_pairs-k3.py` | K3 discrimination reducer (range; the file's docstring wrongly says "K2 E18") |
| `harness/census-k2a.sh`, `census-k4b.sh`, `census-combo.sh` | per-arm counter census (`submit_flushes_` site + `strace`) |
| `harness/summarize_census.py` | census reducer (per-op ratios, PMU medians) |
| `harness/census-all-k2a.sh`, `run-all-pairs-k2a.sh`, `run-all-pairs-k4b.sh` | case batteries |
| `harness/baseline-run_stage.sh`, `baseline-attribution.sh`, `analyze_baseline.py` | frozen-baseline stages and `/usr/bin/time`, `perf stat`, `strace` attribution |
| `harness/attribution-census-run.sh`, `attribution-overhead-run.sh`, `analyze_census.py` | observe-binary census and instrumentation-overhead arms |

**There is no archived reducer for the K4 discrimination arm.** Its
`pair-results.txt` format was produced ad hoc; a reader cannot regenerate it from
this directory. The K1/K3 reducers above were copied from the external evidence
workspace.

Common protocol: 15 pairs, even pairs baseline-first / odd pairs
variant-first, `SLUICE_BENCH_WARMUP=2`, `SLUICE_BENCH_MEASURED=7`,
`SLUICE_BENCH_STATS=1`, single-case filter, `taskset -c 2`, fresh process, and a
per-run `bench/check_run.py` gate plus the in-process oracle self-test and
64 MiB byte verification. **`timeout 300` and the `invalid/` quarantine are
implemented only in `pair-run-k2a.sh`, `pair-run-k4b.sh` and the two
`pair-run-combo*.sh` runners.** The `pair-run-k1/k2/k3/k4.sh` discrimination
runners have neither; they echo `CHECK_FAILED`. All their `.check` files are OK
(K1 120/120, K2 discrimination 180/180, K3 120/120, K4 240/240), so no invalid
data is included, but the earlier "every pair runner" claim was too broad.

The reducers are deterministic functions of the JSONL: success is "produces the
recorded `case-summary.tsv` / `pair-results.txt` from the archived JSONL";
failure is a different number or a different valid-pair count. No script here
has an independent self-test — the independent check is the external G0 checker
and the byte oracle in the bench binary, which are part of the product tree and
were not modified.

## 3. What is deliberately absent

- No build system integration, no CMake/xmake target, no test registration.
- No CI job.
- No raw JSONL, perf `.data`, or binary; those live in the external evidence
  workspace (`/home/jnhu/sluice-593-research/evidence`) and are indexed, with
  hashes, in `docs/research/key-path-593/evidence-index.md`.
- No product source change. `patches/` is the only place where research code
  exists, and nothing here applies it automatically.

## 4. Known label defects in the archived artifacts

- `pair-run-k4b.sh` begins with a "K2-a native paired experiment" banner and
  logs "K2-a paired run"; its `git_var` line also reads
  `worktrees/K2-a` even though `var_bin` points at `worktrees/K4-b`. Both
  worktrees are at the same base SHA, so the recorded value is correct; the
  paths and binary hashes are K4-b's. Copy-paste labels only.
- `analyze_pairs-k3.py` has a docstring that says "K2 E18"; it is the K3
  reducer.
- The K4-b and combo `census-summary.txt` headers say `# K2-a census medians`;
  the values match each arm's own README.
- Both are recorded in `docs/research/key-path-593/measurement-review.md` §9 and
  do not affect any number.
