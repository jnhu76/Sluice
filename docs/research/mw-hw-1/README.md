# MW-HW-1 — WSL2 path elimination & kernel boundary audit

Round: research only, WSL2. Authority: issue #563 (MW-HW-0 umbrella); this
round is the MW-HW-1 charter (path elimination & kernel boundary audit).
BASE: master `8e2aa5ab94670a318288c77da796825cfa9f3cfe` (post-H2b, PR #565).

## Reading order

1. `environment.md` — host, tools, permissions, frozen identities.
2. `path-dag.md` — source-precise execution DAG, three backends, current
   master (supersedes the M0 DAG's uring section for this SHA).
3. `syscall-census.md` — kernel boundary: 1 enter/op floor, cold park path,
   ThreadPool 2.3 futex/op contrast, attribution limits.
4. `work-ledger.md` — userspace redundancy census (W1–W10) with
   cardinalities; H-K1/H-K2/H-K3 adjudications.
5. `performance.md` — paired A/B (base vs e1/e2/e3), capacity sweep,
   correctness gates, mutants.
6. `hypotheses.md` — candidate ranking + counterfactuals + NO_CHANGE list.
7. `decision.md` — verdicts and next gates.
8. `review-A-concurrency.md` (verdict ACCEPT), `review-B-performance.md`
   (verdict SOUND_WITH_REQUALIFICATIONS; every P2 applied in-round) —
   independent reviews.

## Headline results

- **E1 (skip empty pending-control sweeps via derived count)**: wall ratio
  0.858 (9/9 paired reps < 1.0, Wilcoxon p=0.0039, bootstrap 95% CI
  [0.763,0.910]) at 4 KiB/D1/C1024, 0.731 (9/9) at D8, 0.917 (8/9) at
  64 KiB/D8; null at C=16 and 1 MiB (mechanism signature); paired user-CPU
  ratio 0.438 (7/7), system 0.983 (no detectable change); syscall census
  identical; 55% of user-only cycles attributed to the scan (leakage-
  renormalized). All deterministic suites pass; parity
  oracle + 2 CAUGHT mutants. **PROPOSE_UPSTREAM** (gates: native-Linux A/B,
  W-03 A/B).
- **E2/E3 falsifiers**: ≈1.00 — dispatch mediation and publication lock
  pairs are NOT hidden hotspots. NO_CHANGE_JUSTIFIED.
- **Kernel boundary floor**: exactly 1 `io_uring_enter` per op at every
  sliding-window depth; park path cold on warm tmpfs; zero-op path is
  syscall-free. ThreadPool pays ~2.3 futex/op by contrast.

## Reproduction

```bash
# baseline + arms (worktrees /tmp/mw-hw-1-wt/{e1,e2,e3}, branches listed in environment.md)
xmake f -m release --liburing=y && xmake build -y sluice_core sluice_async
g++ -std=c++20 -O3 -DNDEBUG -Iinclude -Ibench \
  bench/native-linux-m2/pipeline_ab.cpp \
  build/linux/x86_64/release/libsluice_{async,core}.a -luring -lpthread \
  -o pipeline_ab_<arm>
./pipeline_ab_base --fixture=<f> --create-fixture
ab/run_ab_mwhw1.sh 9 2            # paired rotated campaign (retained raw)
# census
strace -f -c -o census.txt taskset -c 2 ./pipeline_ab_base --arm=base \
  --backend=uring --block=4096 --depth=1 --capacity=1024 --requests=20000 \
  --warmup=1 --fixture=<f>
# correctness + mutants (e1 worktree)
xmake build -y uring_pending_control_parity_test uring_pending_control_mut_missing_decrement \
  uring_pending_control_mut_missing_increment && ...
```

## Reviews A (concurrency) and B (performance/attribution) completed; all P2
findings fixed in these documents (rep-paired statistics, leakage
renormalization, paired user/system rerun with receipts, per-config
scan/op counts, zero-op census retraction, E2/E3 resolution-bounded
phrasing).

Raw evidence (untracked, outside Git)

Directory: `/tmp/mw-hw-1-out/` (131 MiB; `SHA256SUMS.txt` lists all 32 files,
including `ab/rows.jsonl` (216 timed rows + oracle/latency),
`ab/analysis.txt`, `strace-*.txt` censuses, `perf-base-g.data` /
`perf-e1-g.data`, `c-sweep-d1.txt`, `test-gates.txt`, `e1-mutant-receipts.txt`,
`baseline-identity.txt`, `ab/arm-identity.txt`). The directory is disposable
after the PR is reviewed; the committed documents above carry every number
used in a claim. Regenerate with the commands in this file.

## Honest limitations

WSL2 only (no native-Linux gate); W-02 shape only (W-03 A/B not run);
zero-op has no A/B row (harness rejects block=0; owed path untouched by E1
and covered by its suites); no tail-latency claims; kernel attribution
unavailable (paranoid=2); strace counts only.
