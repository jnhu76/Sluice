# Pre-registered controlled-session protocol (session-r2r2)

Frozen before the first sample of session-r2r2. This file is not edited after
the session starts; its sha256 and mtime are evidence of pre-registration.

## Object under test

- Baseline A: `master` @ `3f22d2d08a53077b6377934b1895af78126de0a1`
  (`sluice-copy` sha256 `0eca1011b2ee047679a4f8dd1bfc5b21aba60f245f850cb2746f94260792b458`).
- Candidate B: `research/621-r2-R1-drain-eagain-read` @ `a3a14a31`
  (`sluice-copy` sha256 `db972b1946fcf2b282135ebdf600263b03492215b08846e867d856c6213fa0be`).
- Production difference: `ProgressSource::drain_notification_nolock_()` returns
  after one successful non-semaphore eventfd read and clears
  `notification_pending_`, instead of issuing a second read that is guaranteed
  to return `EAGAIN`.

## Segments (one session process, this order)

| Segment | Pairs | X | Y | Buffer / depth / workers | Frozen ops per run |
|---|---|---|---|---|---|
| `aa-4k-d4` | 36 | A | A | 4096 / 4 / 1 | read 65540, write 65536 |
| `main-4k-d4` | 36 | A | B | 4096 / 4 / 1 | read 65540, write 65536 |
| `neg-64k-d1` | 12 | A | B | 65536 / 1 / 1 | read 4097, write 4096 |

Pair ordinals restart at 1 in each segment. Even ordinal runs A first (AB);
odd ordinal runs B first (BA). Two discarded warmups per segment, one per arm.
One independent process per run. No sample is filtered.

## Per-run command and discipline

```
sudo perf stat -e task-clock,cycles,instructions -- \
  taskset -c 2,3 <bin> --buffer-size <4096|65536> \
  --pipeline-depth <4|1> --workers 1 --sync none --no-atomic <src> <dst>
```

- Source: `/home/jnhu/sluice-621-r2/data/src-256m.bin`, 268435456 B,
  sha256 `a1f8dd971b212ae487cad4edf8f2b742bd6b1a8feef92a4c44730bbe167547f3`.
- Measurements: `perf stat` task-clock/cycles/instructions plus
  `RUSAGE_CHILDREN` user+sys delta around the `sudo` child.
- Before every pair: `os.sync()`; poll `/proc/meminfo` until `Writeback==0 kB`
  and `Dirty<=4096 kB` (max 120 s); sleep 2.0 s.
- Before every run: no competing `xmake`, `cc1plus`, or `sluice-copy` process.
- All CPUs set to `performance` for the session; measured cores 2,3 checked at
  max frequency at setup; governor restored to `schedutil` in a finally block.
  In-run sysfs frequency samples are archived; this host is not assumed to hold
  `cur==max` during runs.
- Each run's oracle: exit 0; `bytes_copied=268435456`; `short_writes=0`;
  destination sha256 equals source; `read_ops/write_ops` equal the frozen
  values; all perf counters present and not `<not counted>`.

## Statistics

- Pair-level relative difference: `(Y - X) / X * 100`; in A/A both arms are
  binary A.
- Bootstrap: 10,000 percentile resamples over pair-level differences,
  `numpy.random.default_rng(5950)`, reseeded per metric; 95% interval.
- Percentiles: empirical, `numpy` linear interpolation.
- Valid pair: both arms present, both oracle-ok, both ops equal frozen values.

## Pre-registered gates

Frozen constants:

```
minMeaningfulEffectPct = 1.0
aaMaxAbsMeanPct        = 1.5
aaMaxCiWidthPct        = 3.0
p99BudgetPct           = 3.0
aaPairsRequired        = 36
mainPairsRequired      = 36
```

`confirmed = true` only if every item holds:

1. `aa-4k-d4`: at least 36 valid pairs; `|mean CPU diff| <= 1.5%`; CPU
   bootstrap CI width `<= 3.0`.
2. `main-4k-d4`: at least 36 valid pairs; for **each** of instructions,
   cycles, task-clock, and rusage CPU, the bootstrap 95% CI upper bound is
   `< 0`; rusage CPU mean `<= -1.0%`.
3. p99 non-regression: rusage CPU p99 of arm B `<= 1.03 x` p99 of arm A in the
   main segment. p99 is the empirical 99th percentile over all valid runs of
   each arm. This is a one-sided CPU criterion; wall is not used because
   writeback stalls dominate its tail.
4. Zero oracle failures across all segments; governor restored; session not
   aborted.

Environment-stall rule: a run with `wall_s > 5.0` and
`cpu <= 1.5 x` its arm's median CPU is labelled `environment_stall`. The label
excludes the run **only from wall-tail reporting**, never from CPU statistics.
No CPU sample is excluded.

## Decision mapping (fixed now, before data)

- All four gates pass -> `MECHANISM` may be GO and `END_TO_END` may be GO, with
  the adopted magnitude equal to the **minimum absolute value** of the four
  main-segment point estimates (instructions, cycles, task-clock, rusage CPU).
  No larger or rounded-up figure may be quoted.
- A/A gate fails -> `confirmed=false`; `END_TO_END` remains NOT_ESTABLISHED.
- Any main metric CI upper bound `>= 0` -> `END_TO_END` remains
  NOT_ESTABLISHED.
- p99 budget fails -> `confirmed=false`; no product admission.
- No post-hoc selection of segment, buffer size, depth, or metric. The negative
  control is reported, not used as a gate.

## Scope

ThreadPool backend only; non-semaphore `EFD_NONBLOCK|EFD_CLOEXEC` eventfd; all
userspace writers hold `mtx_`; warm page cache; one host; btrfs; one worker;
4 KiB/depth 4 primary cell and 64 KiB/depth 1 negative control. io_uring is not
executed by this workload. This protocol does not establish wall, energy,
multi-worker, cold-cache, or cross-backend effects.
