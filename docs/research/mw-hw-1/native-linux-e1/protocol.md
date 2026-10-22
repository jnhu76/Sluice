# E1 Native-Linux Validation — Protocol (Gates R2/R7)

## Question

Does the E1 patch (derived `pending_control_count_` summary + zero-count gate
in `service_pending_controls_locked_()`) (a) remove the duplicate O(C) router
scan on native Linux, (b) convert it into end-to-end CPU/wall/instruction
savings, (c) without regressing sticky-cancellation (P>0) paths or the W-03
real external loop, and (d) what remains of the CPU cost afterwards?

## Preregistered thresholds (frozen before the formal campaigns)

- Headline gain: `4096-1024-1` and `4096-1024-8` median paired
  `cpu_ns_per_op` ratio ≤ 0.95 AND ≥ 12/15 pairs strictly < 1.0.
- Non-regression: every other non-P>0 config median ratio ≤ 1.03
  (negative controls included — they must show no benefit and no harm).
- P>0 (had it been measurable): median ratio ≤ 1.05 (engineering line fixed
  in advance; not relaxed post hoc).
- W-03: ≥ 15 valid interleaved pairs per config, median ratio ≤ 1.03, real
  kernel event loop (`loop_kind=epoll+eventfd`), every verify row
  `no_lost_wake=true`.
- Correctness overrides performance: any oracle/invariant/parity failure is
  `CORRECTNESS_FAIL` regardless of timings.
- W-02 PASS + semantic PASS but W-03 or P>0 evidence missing ⇒ overall at
  most `CONDITIONAL`; never a silent `NATIVE_GO`.

## Workloads

W-02 matrix (block/requests/capacity/depth; tmpfs fixture; pinned CPU 2;
`taskset`):

```
4096 20000 16 1     # C=16 negative control
4096 20000 64 1     # capacity transition
4096 20000 256 1    # dose response
4096 20000 1024 1   # headline D1
4096 20000 16 8     # C near D
4096 20000 1024 8   # headline D8
4096 20000 64 64    # high occupancy
4096 20000 1024 64  # higher concurrency
65536 2000 1024 1   # large-block control
65536 2000 1024 8   # medium IO
1048576 500 64 8    # IO-dominated negative control
```

W-03 configs: `4096/20000/1024/1`, `4096/20000/1024/8`, `65536/2000/1024/8`
over a real epoll+eventfd external loop (driver thread parks in
`epoll_wait(-1)`, acknowledges, drives `poll_progress`; submitter runs the
depth window). Verify phase (both arms): idle park/wake, zero-length op,
64-op burst coalescing (eventfd counter K>0 evidence), real kernel cancel of
a pending pipe read, detach/shutdown terminal responsibility.

P>0 workload (`file-cancel-congestion`, C32/Q8/W24): instant-completing
tmpfs reads accumulate in the CQ faster than anyone reaps → submit EBUSY →
SQ pressure → cancel of the oldest kernel-visible op exercises the real
cancel path; a seam-built probe samples `pending_control_count_` vs the
authoritative router scan to test whether sticky states actually occur.

## Procedure

1. R0 froze environment + identity (see `identity.md`); harness binaries are
   per-arm and hash-pinned; the runner re-verifies every gate at start
   (SHAs, clean trees, path overlap, stale binaries, fixture hash,
   same-library refusal).
2. Campaigns interleave BASE/E1 per (rep, config) with AB/BA rotation by rep
   parity; warmup and measured phases are separate harness phases; every row
   carries the byte-oracle phase (`oracle`) that must pass for the campaign
   to be valid.
3. Runner validation per row: JSON parse, `ok=true`, known phase/arm, finite
   positive timings, zero completion errors/invalid-state rejections, ops ==
   requests. Any violation aborts the campaign with `status=FAILED`; a
   finished campaign can never be overwritten (`ab.jsonl` exists ⇒ refuse).
4. Reducer (`e1_verdict.py`, preregistered) rebuilds every
   (rep, cfg, arm, phase) cell, rejects missing rows, duplicates, unknown
   configs, non-numeric measurements, failing oracles, depth gaps
   (`max_outstanding != depth`), and identity drift; counterfactual selftest
   (14 cases) must pass.
5. Statistics: true per-rep pairing (E1 row of rep i vs BASE row of rep i,
   same config, same AB/BA rotation slot); medians reported with full ratio
   lists; one-side binomial note (12/15 ⇒ p≈0.0176) explicitly NOT treated
   as independence proof; 11 simultaneous comparisons flagged as a
   multiple-comparison limitation in `verdict.json`.

## Deviations / notes

- The W-03 campaign produces two artifact files (perf rows, verify rows);
  they are concatenated into `w03-combined.jsonl` (sha256 recorded) as the
  reducer input. The frozen reducer was not modified.
- The planned `p95` latency is emitted by the harness (latency phase);
  cancel-latency p50/p95 in the P>0 rows come from the instrumented phase
  only, never from timed rows.
- tracefs is unreadable for a normal user and the protocol forbids sudo, so
  tracepoint-level kernel attribution is NOT_RUN; kernel-side attribution
  rests on rusage user/system split, `perf stat` kernel-excluding `:u`
  counters, and the strace census.
- (Review B P2-1) The frozen reducer computes W-03 medians by rank-pairing
  (`zip(sorted(e1), sorted(base))` in `e1_verdict.py`; the P5 branch is
  rank-paired the same way but never reaches a median because
  `p_events_total == 0` short-circuits to NOT_MEASURED). The preregistered
  statistic is per-rep pairing. Both are now reported side by side in
  `w03-ab.md` (rep-paired 0.8508/0.6146/0.9118 vs rank-paired
  0.8587/0.6171/0.9206); both satisfy the ≤1.03 gate and no status flips.
  `verdict.json` records the rank-paired values and stays byte-reproducible
  from the frozen reducer. The reducer was not edited after the campaigns.
- (Review B P2-3) `results-w03/arm-identity.txt` and
  `results-p5/arm-identity.txt` are stale verbatim copies of the W-02 file;
  they are not receipts for those campaigns. The W-03/P5 runners verify
  worktree SHA pins and clean trees at start and record runner/harness-source
  SHAs, but — unlike `run_e1_e5.sh` — perform no per-run library-hash drift
  re-check and do not hash the harness binaries. Post-hoc hashes of the
  actual W-03/P5 binaries are recorded in `identity.md`, clearly labeled.
- (Review B P3) The reducer enforces the ≤1.03 non-regression bound on every
  non-headline config but does not mechanically enforce a "negative controls
  show no benefit" direction; the controls were observed neutral (0.996 both)
  and the full ratio lists are published, so the gate is audit-ready.
- (Review A P2) Probe statistics first cited for the P>0 attempts were
  round-log observations without archived artifacts; they have been reworded
  as such, and citable supplementary probe evidence was archived during
  review remediation (`results-p5/p5-probe-supplement.jsonl` + provenance:
  12 runs / 1,731 monitor samples / 0 positives / 0 parity violations, frozen
  binary, byte-identically recreated fixture). This supplement is post-review
  evidence, recorded 2026-10-22T11:13Z, and changes no gate.
- (Review B P3) The protocol was frozen in the working tree before the formal
  campaigns, but this docs directory was committed to git only at round end
  (after reviews and remediation), so VCS timestamps alone do not prove
  preregistration; the frozen script hashes, campaign provenance files, and
  the byte-reproducible reducer output are the preregistration audit trail.
