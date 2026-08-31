# TAX-0B canonical session notes — tax0b-zladder-wsl2-formal4

Canonical semantic-floor ladder session for #250 TAX-0B (PR #260 evidence).
This session supersedes formal1–formal3 as the citable ladder numbers
(those remain as raw history: formal2/formal3 measured a z1/z1b variant
that paid 2 io_uring enters per op — a harness competence defect fixed
before formal4; see "protocol corrections").

- BASELINE: branch `research/tax0-a2-control-plane` @ `867ac94`
  (production files byte-identical to master `9670224` except the
  TAX-0D research seams, which are `SLUICE_ASYNC_INTERNAL_TESTING`-guarded
  and absent from this session's production-linked bench binary).
- BENCH: `research/tax0/bench/tax0_z_ladder_bench.cpp` (target
  `tax0_z_ladder_bench`, links PRODUCTION sluice_async), Release + clang
  + liburing 2.14, stripped release build (sha256 in environment.json).
- REPRODUCIBILITY: the stripped binary re-measured after formal4 gave
  3121 vs 3112 instr/op at read 4K d32 z2 (+0.3%).
- ARMS: z1 (raw liburing floor) / z1b (frozen F05 checklist as explicit
  machinery) / z1bw (z1b + one lost-wake-safe continuation consumer:
  reaper thread + per-slot terminal predicate wait) / z2 (AsyncIoContext
  manual driver, no Scheduler) / z3w1, z3w4 (ApplicationRuntime).
- PROTOCOL: `--warmup 0 --reps R` under `perf stat -x, -e
  instructions:u,cycles:u,branch-misses:u,cache-misses:u`; per-op work
  normalized by DOUBLE DIFFERENCE `(total(R14) − total(R7))/7/ops`, which
  exactly removes setup/teardown (ring/runtime construction, teardown,
  first-touch) from the whole-process counters. Write-arm final
  verification is deferred to the runner (`--runner-verify`) so the
  verification pread never enters the measured window. Read-arm word_sum
  is computed inline in every arm (uniform workload component).
- SAME-WORK: fail-closed per rep (ops/bytes/word_sum exact); cross-arm
  word_sum equality checked by the runner (60/60 OK); write arm
  full-file byte verification runner-side after every write combo.

## Headline ladder (instructions/op, double-difference)

| cell | Z1 | Z1b | Z1bw | Z2 | Z3w1 | Z3w4 |
| --- | --- | --- | --- | --- | --- | --- |
| 4K d1 read | 1154 | 1201 | 1968 | 3827 | 5796 | 6647 |
| 4K d32 read | 1043 | 1080 | 1290 | 3112 | 3899 | 3924 |
| 4K d64 read | 1043 | 1080 | 1290 | 3212 | 4105 | 4138 |
| 64K d8 read | 14499 | 14537 | 14783 | 16552 | 17380 | 17498 |
| 1M d8 read | 229553 | 229590 | 230155 | 231605 | 232468 | 233416 |

Fixed per-op deltas (read cells): capability cost Z1b−Z1 ≈ +37/op
constant; L1 abstraction tax Z2−Z1b ≈ +2015/op constant across 4K/64K/1M
(2032 / 2015 / 2015); runtime continuation Z3w1−Z2 ≈ +787..+863/op.

## Protocol corrections vs formal1–formal3 (provenance, not edits)

- formal2/formal3 z1/z1b staged SQEs but ALSO called a bare
  `io_uring_submit` in the fill phase plus `io_uring_submit_and_wait` in
  the reap phase (2 enters/op) while the production driver uses one
  merged enter. formal4 merged the flush (single enter per op), making
  z1/z1b at least as enter-efficient as production. Sessions are
  append-only; no historical numbers were edited.
- formal1–3 also showed the runner bugs fixed before formal4 (perf `:u`
  key parsing, CSV extrasaction, bounded combo retries).

## Observations / limitations

1. ENVIRONMENT-LIMITED (QUALIFIED_BUT_VIRTUALIZED): WSL2 kernel
   6.18.33.2, virtualized PMU (real, verified: distinct instruction /
   cycle / branch counters on a calibrated loop), ext4 on a virtual
   block device. Control-plane user-instruction attribution is valid;
   no native NVMe/throughput conclusions.
2. z3w4 write cells: intermittent instability — two distinct symptoms:
   (a) spurious `canceled` (IoError code 1) terminal with
   `os_errno=125` (EAGAIN) surfacing through `Completion::result()` on
   the ApplicationRuntime write path (no cancel was ever requested by
   the harness); (b) `terminate called without an active exception`
   abort during runtime teardown. In-session (perf-stat-wrapped)
   frequency was high enough that all retry attempts failed (formal2:
   4/4 combos, formal3: 3/4), while standalone probes passed 11/12 and
   5/5, and one discriminated probe showed 4/5 pass / 1 abort under the
   perf wrapper vs 5/5 plain. Minimal recovery applied from formal4: z3w4
   write cells measured WITHOUT the perf wrapper (wall/user/sys +
   same-work only; `instructions_u_per_op` = NOT RUN — environment
   limitation). This is recorded as a suspected production-side
   instability (candidates: spurious cancel-disposition publication or
   EAGAIN mapped to `canceled` in the uring backend; runtime worker
   teardown race) — follow-up in #250, not root-caused in this campaign.
3. Write≫read asymmetry at shallow depth REPRODUCED on this host
   (4K d1 write wall 39–70µs vs read 1.6µs; asymmetry vanishes at d≥8).
   Carries the v1.1 census UNKNOWN forward as
   ENVIRONMENT-SPECIFIC-until-native.
4. Historical #221 cliff (4K, depth≥32, workers=4) re-judgment on the
   uring Z-ladder: NOT REPRODUCED — z3w4 vs z3w1 at 4K d32 is +0.6%
   instructions (3924 vs 3899) and +40% wall (1820 vs 1302 ns), at 4K d64
   +0.8% / +54%; smooth in depth, no d≥32 discontinuity. Scope note: the
   historical evidence was ThreadPool-backend; the Z-ladder re-judges the
   uring path (this campaign's target) in this environment only.
