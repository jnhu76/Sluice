# TAX-0 ROUTER R1 production landing — residual measurement (A0)

Issue authority: #255 (fix-selection gate; downstream of #227/#250).
Causal evidence: EXP-U0 / PR #254. Candidate selection: PR #256 (practical
tie → simplest candidate R1 reverse scan). This report covers the PRODUCTION
landing measurement only — the research phases are recorded in
`TAX0-ROUTER-FIX-SELECTION.md` and are not re-opened here.

## What landed

The `#else` (non-internal) scan in `UringAsyncBackend::find_live_router_cookie_`
traverses high → low (R1). No other production semantic changed: live cookies
stay unique within backend lifetime (no-wrap `allocate_cookie_`), so the
traversal order cannot change found/not-found or the matched index.

The internal-testing seam was synced so it keeps modeling production:
`production_baseline` and the U0 seam default now track the shipped REVERSE
scan; the pre-fix forward traversal is `forward_ablation` (causal-comparator
direction). Historical EXP-U0 artifacts and the frozen runner/validator
mode labels (`production_forward` / `reverse_ablation`) are unchanged
frozen evidence.

## Production-path measurement provenance

`tax0_capacity_bench` links the PRODUCTION `sluice_async` only (no
`SLUICE_ASYNC_INTERNAL_TESTING`; `xmake/benchmarks.lua`) — the measured
binary cannot contain the research seam, so the measured scan is the shipped
production implementation. This is the production-path proof required by the
A0 task: the residual numbers below are NOT `RouterFixModeForTest` runs.

- BEFORE: master `489f0dc5`, clean tree, built in a dedicated worktree;
  binary sha256 `0f31b13deeb04c4e…`.
- AFTER: branch `perf/tax0-router-r1-production`; the binary was built from
  the working tree that became commit `d4a329c4` (artifacts record
  `489f0dc5` + dirty, as is runner-verbatim); binary sha256
  `a3cd1d1708632926…`. Binary identity is the binding provenance.
- Same session, interleaved arms (btrfs BEFORE→AFTER, tmpfs AFTER→BEFORE,
  threadpool control BEFORE→AFTER) on the Host-0 machine; clang 22.1.8
  Release, `--with-liburing=true`; canonical EXP-0 frozen matrix: READ 4 KiB,
  D=8, Q=8, C ∈ {8,32,128,512}, 256 MiB same-work, 11 blocked-randomized
  rounds (seed 0x54415830), 2 warmup rounds, `taskset 0,2,4,6`,
  `perf stat` user-mode counters per process.

## Result — capacity slope of instructions/op vs C (OLS, runner-derived)

| arm | before (instr/op/C) | after (instr/op/C) | recovered |
| --- | --- | --- | --- |
| uring / btrfs | +6.0007 (r²=1.000000) | +0.0007 (r²=0.999816) | 99.99% |
| uring / tmpfs | +6.0002 (r²=0.999999) | −0.0066 (r²=0.557, flat) | ≈100% |
| threadpool / btrfs (control) | −0.1246 (r²=0.524, no trend) | −0.0668 (r²=0.256, no trend) | n/a |

Cycles/op slopes: uring btrfs +1.98 → −0.01; uring tmpfs +2.35 → −0.41;
threadpool control shows no capacity trend in either build.

Baseline comparability: the BEFORE btrfs slope +6.0007 reproduces the frozen
EXP-0 artifact `tax0b-exp0-uring-btrfs.json` (+6.0007) to four decimals on
this host, and AFTER matches the EXP-U0 reverse ablation
(`tax0u0-uring-btrfs.json` +0.0007) the same way — the landed production scan
behaves identically to the causally attributed ablation arm.

## Answers (A0-G2)

- **Recovered tax**: essentially the whole measured T0-U-ROUTER capacity
  slope — +6.00 instr/op/C of the uring READ path, 99.99% (btrfs) / ≈100%
  (tmpfs) of the before slope.
- **Residual tax**: the after slope is indistinguishable from zero at this
  measurement resolution (|b| ≤ 0.007 instr/op/C; tmpfs r² collapses to 0.56,
  i.e. no linear capacity trend). No material residual capacity-dependent
  instruction cost remains on this path. The ThreadPoolBackend control
  (no router) shows no capacity slope before or after, consistent with the
  tax being uring-router-specific.

## Gates run for this measurement

Release build with real liburing; `perf-evidence-validate.py` 64/64 artifacts
(6 new: `tax0routerfix-{before,after}-{uring-btrfs,uring-tmpfs,
threadpool-btrfs}.json`). Correctness gates, sanitizer/TSan runs and the
mechanical gates are recorded in the landing PR.

## Scope notes

- The runner/validator `tax0u0` mode labels are frozen EXP-U0 evidence-chain
  vocabulary and were not renamed; re-running EXP-U0 post-landing executes
  the direction arms correctly (bench CLI `forward|reverse` unchanged), but
  the label "production_forward" now denotes the pre-fix comparator arm.
- No PUBLIC API changed; no semantic authority changed; no document outside
  this report and the PR needed a direction update (checked: no CURRENT doc
  states the scan direction).
