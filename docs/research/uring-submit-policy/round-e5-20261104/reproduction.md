# reproduction.md — how to rebuild, re-run, and recompute

Three independent paths, in decreasing order of effort. Paths B and C need no
E5 host and no network.

## A. Full re-run on the E5 host (re-measure)

```sh
# 1. Recreate the research worktree at the frozen base + patch
git worktree add /home/<you>/sluice-595-wt -b research-595-bounded-progress \
    271b81bfc9d64eecc0266fe7066b9040525dd710
cd /home/<you>/sluice-595-wt
git apply bench/research/uring-submit-policy/round-e5-20261104/patch.diff

# 2. Build (gcc 16.2.1, liburing-devel 2.13-2, xmake 3.1.2; see build-notes.md)
xmake f -c -y --liburing=y -m release
xmake build bench_w02_pipeline bench_w03_external_loop uring_595_bounded_progress_test
sha256sum build/linux/x86_64/release/{bench_w02_pipeline,bench_w03_external_loop,uring_595_bounded_progress_test}
# must equal build-notes.md hashes; any mismatch = rebuild invalid, re-record + redo A/A

# 3. Correctness gates first
SLUICE_595_ARM=B xmake run uring_595_bounded_progress_test     # 11 oracles, rc 0
SLUICE_595_ARM=B xmake run uring_595_mut_defer_no_cap          # rc 1 = mutant killed
# (4 mutant targets; then the A/B-mode suites)

# 4. A/A calibration, then paired A/B (arm order alternates automatically)
H=bench/research/uring-submit-policy/round-e5-20261104
python3 -I $H/analysis/pair_runner.py --phase aa --case W02-uring-4k-d16 \
    --pairs 24 --measured 5 --binary build/linux/x86_64/release/bench_w02_pipeline \
    --cpu 2 --arms AA
python3 -I $H/analysis/analyze.py summary --phase-dir .../aa --arms AA --gate aa

python3 -I $H/analysis/pair_runner.py --phase main --case W02-uring-4k-d16 \
    --pairs 24 --measured 5 --binary build/linux/x86_64/release/bench_w02_pipeline \
    --cpu 2 --arms AB
python3 -I $H/analysis/analyze.py summary --phase-dir .../main --arms AB --gate ab

# D2/D3 need the depth-case selector:
python3 -I $H/analysis/pair_runner.py ... --env SLUICE_BENCH_595_DEPTHS=1 \
    --case W02-uring-4k-d2
```

Mechanism ledger (separate from timing):
`strace -f -c -e io_uring_enter <binary>` per arm with
`SLUICE_BENCH_CASE_FILTER==<case>`; PMU: `perf stat` interleaved arms.

Pair-to-pair numbers will differ (field noise); gate outcomes and effect
magnitudes are the reproduction target, not bit-identical values.

## B. Offline statistical recompute (no host needed)

The committed decision data is sufficient:

```sh
H=bench/research/uring-submit-policy/round-e5-20261104
python3 -I $H/analysis/analyze.py summary \
    --phase-dir $H/raw-minimal/ab-main-w02-4k-d16 --arms AB --gate ab
# expect: cpu -26.46% [-26.89,-26.02], tput +36.07% (B faster), p99 -29.23%; NET WIN + PERF PROTECTION PASS

python3 -I $H/analysis/analyze.py summary \
    --phase-dir $H/raw-minimal/aa-repair-w02-4k-d16-m5 --arms AA --gate aa
# expect: half-widths cpu 0.44 pp / tput 0.43 pp / p99 1.08 pp -> AA PASS

python3 -I $H/analysis/analyze.py summary \
    --phase-dir $H/raw-minimal/aa-calib-w02-4k-d16 --arms AA --gate aa
# expect: p99 3.71 pp -> AA FAIL (the pre-repair receipt)

R=$H/raw-minimal
python3 -I $H/analysis/analyze.py control-family \
    --dirs $R/ctl-d1 $R/ctl-d2 $R/ctl-d3 $R/ctl-64k $R/ctl-w03-d4 \
           $R/ctl-w03-d16 $R/ctl-w03-d64 $R/ctl-threadpool $R/ctl-saturate \
    --out /tmp/family.json
# expect: FAIL only on ctl-d2 (p95/p99/tput; CPU inside bound) and
#         not-establishable on w03-d4 p95 / w03-d64 p99; overall FAIL
```

Bootstrap resampling is seeded (5950) and pair data is committed verbatim, so
these commands reproduce the stored JSONs **bit-for-bit** — verified in
`analysis/reanalysis-receipt.md`. Also recomputable the same way: D2
replication (`ctl-d2-repl`), noise floors (`aa-noise-*`), placement
(`place-*`), and the consolidated table in `analysis/summary-tables.md`.

## C. Integrity verification

```sh
# Layer B (this archive): every committed file
sha256sum -c docs/research/uring-submit-policy/round-e5-20261104/SHA256SUMS   # from repo root

# Layer A (full E5 evidence): 1,859 entries, needs the full tree or the backup
tar -xzf sluice-595-e5-full-evidence-20261104.tar.gz -C /tmp/verify
cd /tmp/verify && sha256sum -c evidence/MANIFEST.sha256 --quiet   # expect silence
sha256sum sluice-595-e5-full-evidence-20261104.tar.gz
# expect c384c910116a3b2022c9871b229ad0aacfaebf6cb124228fe7d5f8c856dbdeb4
```

## What maps to what

`raw-minimal/<phase>/results.jsonl` is the verbatim per-invocation record
(the `metrics` block is what `analyze.py` reads; `reps` carries the five
per-rep records; `ru_*` is the secondary rusage view). `<phase>/header.json`
freezes the phase configuration (binary hash, case, cpu set, env overrides,
measured-per-invocation). `analysis/*.json` are the stored analysis receipts.
No committed file was regenerated or edited; exclusions are listed in
`raw-minimal/EXCLUDED.md`.
