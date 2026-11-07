# PROT-PLACEMENT — A0-only representative placement (B blocked)

| field | value |
|---|---|
| phase status | **NOT_ESTABLISHED** (B placement comparison not run) |
| case | `W03-uring-4k-d16`, WARMUP=1 MEASURED=5 |
| runs | 8 A0 invocations per mask + 1 `/usr/bin/time -v` per mask |
| topology | cpu2 siblings `2,12`, core 2; cpu8 siblings `8,18`, core 11; same package 0 |
| raw | `/home/jnhu/sluice-595-e5/r3-completion-20261105T083818Z/raw/protection/SAFE-A0-PLACEMENT-*/` |

| mask | taskset | CPU/op (ns) | tput (ops/s) | p95 (ns) | p99 (ns) | threads | delta vs cpu2 (cpu/tput/p95/p99) |
|---|---|---|---|---|---|---|---|
| cpu2 | `2` | 3153.5 | 317335 | 47397.6 | 57059.7 | 3 | |
| cpu8 | `8` | 3150.3 | 317635 | 44606.2 | 51695.2 | 3 | | -0.1% / +0.1% / -5.9% / -9.4% |
| smt-2-12 | `2,12` | 7740.9 | 234240 | 75283.6 | 94170.7 | 3 | | +145.5% / -26.2% / +58.8% / +65.0% |
| xphys-2-8 | `2,8` | 8406.1 | 218218 | 81751.5 | 100292.9 | 3 | | +166.6% / -31.2% / +72.5% / +75.8% |

- `cpu8` is statistically indistinguishable from `cpu2` (CPU -0.1%, p99 -9.4%
  descriptive).
- `{2,12}` and `{2,8}` are much slower for A0: CPU/op +145.5% and +166.6%,
  throughput -26.2% and -31.2%, p99 +65.0% and +75.8%. This is an A0 baseline
  placement property (thread/CQ migration across two CPUs), not a B result.
- `/usr/bin/time -v` max RSS: {'cpu2': [5352], 'cpu8': [5308], 'smt': [5324], 'xphys': [5212]} KiB.
- cpu2/cpu12 before-window busy fractions were 0.000 in the cpu2/cpu8/xphys
  masks; cpu12 showed <=0.011 mean (max 0.091) in the cpu8 and smt masks.
  schedutil frequencies ranged 1.2-3.5 GHz; see the per-run `before`/`after`
  snapshots in `results.jsonl`.
- App-thread `processor_last`: cpu2 -> 2; cpu8 -> 8; smt -> 2/12; xphys -> 2/8.

## Boundary

- A0-only. The prereg placement blocks are safety-only and do not produce PASS;
  with B blocked they are `NOT_ESTABLISHED`.
- Sequential mask order (2, 8, 2,12, 2,8) and schedutil drift are recorded as
  limitations; no randomized placement order was used.
