# 621 ProgressSource drain single-read evidence (2026-11-12)

> **Archival evidence PR — DO NOT MERGE.** This pull request exists so the
> measurement package is reviewable and citable; it must never be merged into
> `master`. The production change and its tests are in PR #635.

Production change: `ProgressSource::drain_notification_nolock_()` no longer
issues a second `read(2)` after a successful non-semaphore eventfd read. The
successful read already consumed the whole counter; the same `mtx_` critical
section clears `notification_pending_` before returning. The `EINTR` retry and
`EAGAIN` branches are unchanged, and every userspace writer already holds
`mtx_`.

- Product PR: **#635** — https://github.com/jnhu76/Sluice/pull/635 (not merged; owner merge gate retained).
- Base: `3f22d2d08a53077b6377934b1895af78126de0a1` (PR #634 merge).
- Candidate tree: `a3a14a31` on `research/621-r2-R1-drain-eagain-read`;
  product branch `product/621-progress-drain-single-read` from `origin/master`.
- Measured candidate diff: `candidate.diff` (sha256
  `ee7c4c3e90ed959ac26d48378f94bdc4ed02ef742830581d61509b0c09ed0199`,
  4 files +760/−3); production-only change is `production.diff` (header,
  +47/−3, tree `a3a14a31`).
- Product branch diff: `product.diff` (sha256 `96922d0e…`, 4 files +758/−3;
  production +45/−3). It removes two test-only comment lines from the measured
  tree; the rebuilt release `sluice-copy` is still `db972b19…`, so the delta has
  no codegen effect.
- Verdict: mechanism GO, end-to-end GO with conservative magnitude
  **5.0180% instructions**, product READY_FOR_PRODUCT_PR (owner merge gate
  retained). Full reasoning: `REPORT.md`.

## What was removed

The old drain loop executed:

```
read -> success -> continue -> read -> EAGAIN -> clear pending -> return
```

On the ThreadPool path the second read is guaranteed `EAGAIN` for userspace
writers because they all hold `mtx_` and eventfd was created without
`EFD_SEMAPHORE`. The second read's only effect was clearing
`notification_pending_`; the candidate clears it on the successful read.

## Primary measurement (session-r2r2)

One controlled session, pre-registered before sampling
(`formal-ab/PROTOCOL.md`, sha256
`b696a39d53ee38d7ec90971007739b230c64721afdfdc0d44cab832b360929c7`,
mtime before session start; `formal-ab/PRE-REGISTRATION.txt` records the
binary, source, script and diff hashes):

- A/A 36 pairs, main A/B 36 pairs, negative control (64 KiB/d1) 12 pairs;
  AB/BA alternation; 168 formal runs + 6 discarded warmups.
- Every run: `sudo perf stat -e task-clock,cycles,instructions -- taskset -c
  2,3 sluice-copy ...`; `RUSAGE_CHILDREN` user+sys; `os.sync()` and
  Writeback/Dirty quiescence before every pair; performance governor set and
  restored.
- A/A CPU: +0.7713% `[-0.4342,+1.9748]`, width 2.4090 (gate ≤3.0).
- Main, mean of per-pair ratios with 10,000-resample percentile bootstrap
  (seed 5950):
  - instructions −5.0180% `[-5.7603,-4.2987]`, 36/36 negative;
  - cycles −5.1111% `[-5.8111,-4.3976]`, 36/36 negative;
  - task-clock −5.4653% `[-6.2955,-4.6237]`, 35/36 negative;
  - rusage CPU −5.3199% `[-6.1475,-4.4848]`, 35/36 negative.
- p99 (rusage CPU) B/A: −7.2690% (budget +3.0%).
- Negative control rusage CPU: −1.4611% `[-2.7425,-0.2340]`; it is not a
  zero-effect cell, because one notification is still consumed per request.
  Here "request" means one read or write I/O operation (131,076 = 65,540 reads
  + 65,536 writes in the main cell; 8,193 = 4,097 + 4,096 in the control), not
  the 65,536 or 4,096 data chunks.
- Raw rows and metadata: `formal-ab/session-r2r2-*.tsv` and `.meta.json`;
  analysis: `formal-ab/session-r2r2.analysis.{txt,json}` and `summary.txt`.

The R1 session (`session-r2r1`) is historical: its 12-pair A/A width was
3.8789 and it did not pass its own gate. Its numbers are retained in
`REPORT.md` but are not the current claim.

## Correctness receipts

- Semantic oracle: 9 cases, exit 0 on pure BASE (test registration only,
  production header diff empty) and on the candidate.
- Mutant `progress_source_mut_drain_success_keeps_pending`: exit 1, exactly
  three semantic re-arm failures; restored candidate target exit 0.
- BASE drain body + observation-only seam: mechanism target exit 1 with seven
  assertion failures, i.e. two reads per notification / one EAGAIN. The seven
  failures are: a non-empty drain issues exactly one read; the successful
  drain does not probe the emptied counter; every notification round trip stays
  at one read; the notification after an empty drain is consumed in one read;
  the retry issued one real read; the retry consumed the notification; the
  notification after an interrupted drain is consumed in one read.
- Matrix: release/debug × liburing y/n = 81/81, 81/81, 60/60, 60/60.
- 15 targeted suites: exit 0 (see `correctness/suites/`).
- ASAN and TSAN: three targets each, all exit 0, with `__asan_`/`__tsan_`
  instrumentation verified in each log.
- Release `sluice-copy` binary `db972b19…`; no test seam symbols.

## Reproduction

The one-shot session driver lived in the experiment directory, not in this
repository (AGENTS.md: commit evidence, not collectors). The exact commands
are in `REPORT.md` §F.2 and in `formal-ab/PRE-REGISTRATION.txt`. The raw TSVs
and analysis outputs here are sufficient to recompute every summary with:

```
(B - A) / A per pair, mean of per-pair ratios,
10,000 percentile bootstrap resamples, numpy default_rng(5950), 95% interval.
```

## B.2 and historical cross-check

`b2-recompute/` recomputes the B.2 caller shares from
`raw-r2/record/agg-attrib.tsv` (the source header's columns are transposed)
and explains why B.2 remains a location map, not a quantitative ledger.
`crosscheck-10pair/` reconstructs what the historical 10-pair perf-stat files
do and do not prove: fixed A→B order, no sync/Writeback record, pairs 6–10
without stdout, and an aggregate that is a ratio of means rather than a mean
of per-pair ratios.

## Scope

ThreadPool backend, non-semaphore eventfd, all production userspace writers
holding `mtx_` (the test-only `saturate_notification_for_test` is the sole
non-locking writer and is not used by production), one host, btrfs, warm page
cache, one worker, 4 KiB/depth 4 primary
cell and 64 KiB/depth 1 control. Not established: wall, energy, io_uring
end-to-end, multi-worker, cold cache, real kernel EINTR, or a future writer
that does not hold `mtx_`. The io_uring kernel re-signal window has no
deterministic injection; the physical probe remains the safety net, and the
window can only leave readiness asserted (a redundant write, never a
suppressed wake).

## Manifest

`MANIFEST.sha256` covers every file in this directory except itself. Verify
with:

```
cd docs/evidence/621-progress-drain-single-read-20261112
sha256sum -c MANIFEST.sha256
```
