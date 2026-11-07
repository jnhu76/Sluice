# PROT-SAT — R3 B structurally unavailable; A0-only saturation baseline

| field | value |
|---|---|
| phase status | **NOT_ESTABLISHED** (`NO_W02_R3_ARM`; W-03 has no saturation case) |
| case | `W02-uring-4k-d16-saturate` (W-02, depth 16, saturating) |
| comparison | `B_R2 - A_R2 / B - A0 (NOT RUN: NO_W02_R3_ARM; not in frozen enabledBlocks)` |
| A0 runs | 3 plain sampler runs + 1 `/usr/bin/time -v` + 1 `SLUICE_BENCH_STATS=1` run + 1 strace run |
| raw | `/home/jnhu/sluice-595-e5/r3-completion-20261105T083818Z/raw/protection/SAFE-A0-W02-SAT-d16/`, `/home/jnhu/sluice-595-e5/r3-completion-20261105T083818Z/raw/protection/SAFE-A0-W02-SAT-d16-time/`, `/home/jnhu/sluice-595-e5/r3-completion-20261105T083818Z/raw/protection/SAFE-A0-W02-SAT-d16-stats/`, `/home/jnhu/sluice-595-e5/r3-completion-20261105T083818Z/raw/protection/DIAG-SAT-A0/` |

## A0 saturation behavior (one measured pass)

| field | value |
|---|---|
| ops settled | 3200 |
| submits attempted | 3400 |
| would-block / rejected at saturation | 200 |
| accepted at saturation | 3200 |
| waits | 3200 |
| stats submit_calls / submitted_ops / completed_ops | 3200 / 3200 / 3200 |
| stats max_outstanding | 16 (== depth 16) |
| stats queue_full_retries | 0 |
| stats wait_calls / poll_calls | 364 / 551 |
| CPU/op | 3338.312 ns |
| throughput | 301509.147 ops/s |
| `/usr/bin/time -v` max RSS | [5212] KiB |
| max user threads | 2 |
| io_uring_enter (strace) | total=9604, pure_submit=9600, to_submit_hist={'1': 9604} (3 passes x 3,200) |

The saturation shape fills the bound to 16, records one would-block per round,
then settles the accepted 16; 200 rounds => 3,200 settled ops and 200
would-block rejections. No queue-full retries occur.

## Boundary

- R3 B cannot execute on W-02 (`NO_W02_R3_ARM`); no B-vs-A0 saturation protection
  comparison exists.
- The A0-only numbers are descriptive and do not establish a protection gate.
