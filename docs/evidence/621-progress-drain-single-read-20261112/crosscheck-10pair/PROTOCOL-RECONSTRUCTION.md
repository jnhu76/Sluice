# 10-pair perf-stat cross-check: what the archived files prove

This note reconstructs the protocol of the historical 10-pair session from
its raw files. It does not retroactively establish a protocol that was not
recorded.

## Archived files

- `raw-r2/ab/stat/stat.txt` (mtime 2026-11-12T17:22:44+08:00): pairs 1-5,
  every pair ordered `A run N` then `B run N` (fixed A->B, no AB/BA
  alternation).
- `raw-r2/ab/stat/stat2.txt` (mtime 17:28:25): pairs 6-10, same fixed order.
  The five-minute gap between the two files is not explained by any archived
  note; the two files are separate batches.
- `raw-r2/ab/stat/A1-A5.stdout`, `B1-B5.stdout` (mtime 17:22:32-17:22:44):
  oracle lines only, 10 files for pairs 1-5. Pairs 6-10 have no stdout files.
- `pairs.tsv` here: parsed task-clock, cycles, instructions, elapsed, user,
  sys for all 20 runs, in file order.

## Facts visible in the files

- Command line per run: `taskset -c 2,3 <binary> --buffer-size 4096
  --pipeline-depth 4 --workers 1 --sync none --no-atomic <src> <dst>`; A and B
  binaries are the same two builds used elsewhere; destination names are
  per-run (`dst-ab-A1..A10`, `dst-ab-B1..B10`).
- Oracle: every archived stdout line reports
  `copied 268435456 bytes (read_ops=65540 write_ops=65536 short_writes=0
  sync=none pipeline_depth=4 atomic=off)`. Pairs 6-10 have no archived
  stdout, so their oracle is not evidenced by a file.
- Each run is followed by a `perf stat` block with task-clock, cycles,
  instructions, context-switches, migrations, page-faults, branch/cache
  counters, elapsed, user, sys.

## Facts NOT visible in the files

- No record of `os.sync()` or `/proc/meminfo` Writeback/Dirty polling before
  runs; no pre-run quiescence evidence.
- No warmup runs, no discarded samples, no governor record, no frequency
  samples, no load-average or competing-process guard.
- No statement of destination reuse/allocation policy beyond the names.
- No randomization or alternation: treatment is fully confounded with
  position (A always first).
- No record of why the session split at pair 5.

## Metric definition actually used

The historical aggregate quoted in the report (`-2.42%` instructions,
`-2.10%` cycles, `-1.71%` task-clock, `-1.416%` rusage) is the **ratio of arm
means**, not the mean of per-pair ratios. From `pairs.tsv`:

| metric | ratio of means | mean of per-pair ratios |
|---|---|---|
| task-clock | -1.7140% | -1.6736% |
| cycles | -2.1007% | -2.0951% |
| instructions | -2.4166% | -2.4154% |
| rusage user+sys | -1.4160% | -1.3801% |

The controlled session (`session-r2r1`, `session-r2r2`) uses mean of per-pair
ratios with bootstrap CI. The two historical definitions are close here but
are not the same estimator; this is one documented reason the historical
cross-check cannot be pooled with the controlled session.

## Admissible use

This session can be cited only as a historical cross-check that found a
negative direction under a fixed A->B order with no archived discipline. It
cannot serve as a negative control, a non-inferiority result, or a calibration
of environment noise, and it does not contradict the controlled session.
