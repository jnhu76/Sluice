# End-to-end production A/B

Primary campaign: 20 configurations × 5 arms × 15 repetitions = 3000 harness
runs, 4500 accepted JSONL rows (timed, oracle, latency; warmup is not emitted).
Idle-window replication: 7 configurations × 5 arms × 15 repetitions = 525 runs,
1575 accepted rows. Both campaigns passed the independent validator with zero
oracle failures and zero timed allocations.

## 1. Design

The harness (`pipeline_ab.cpp`) is identical for every arm; only the linked
`libsluice_async.a`/`libsluice_core.a` differ. Each run exercises the public
`RequestScope::submit_read` → `AsyncIoContext::wait_one` → `UringAsyncBackend`
path over a 64 MiB tmpfs fixture, with a bounded in-flight window. Every run
performs:

**W03_AB = NOT_RUN.** The harness drives only the W-02 synchronous-wait shape
(`RequestScope::submit_read` → `scope.wait_for` → `scope.take`, always
`--backend=uring`); it never drives `progress_notification_fd`,
acknowledge/detach, or a host-owned external event loop. The configurations
below are 20 W-02 combinations, not a W-02 × W-03 matrix, and every confirmed
result in this document is W-02-scoped. W-03 wakeup and liveness under the
tested mechanisms remain an outstanding product-admission gate
(`h2-results.md` §6); a W-02 gain cannot certify W-03 behavior.

1. warmup (not emitted);
2. timed phase (oracle disabled, allocations counted);
3. oracle phase (every read verified against `fill_pattern`);
4. latency phase (per-operation timestamps, p50/p90/p99).

A failing oracle or a non-zero timed allocation invalidates the repetition; the
validator rejects the whole campaign if any repetition is missing or fails.
Arms are rotated by repetition (`(ai + rep - 1) mod 5`) and configurations are
rotated as well, so arm order is not confounded with time.

Matrix:

- block: 4096, 65536, 1048576 bytes;
- capacity: 16, 64, 256, 1024 (full 4 KiB row); 64 and 1024 for 64 KiB/1 MiB;
- window depth: 1, 4, 8;
- requests: 20,000 (4 KiB), 2,000 (64 KiB), 500 (1 MiB).

CPU affinity: `taskset -c 2`. Frequency and load were sampled before/after each
run. All runs used the same compiler (`g++ 16.2.1`), flags
(`-std=c++20 -O3 -DNDEBUG`), liburing 2.13, and kernel.

## 2. Frozen identity

| Item | Value |
|---|---|
| Source | `8fdcd99c5f9a42dadfe5aa0c095bf96c756aeb01` (master) |
| h1.patch | `c9d6e86ec2abc13394d969d82be287779b9f958be26bc783ad92889187cca999` |
| h2a.patch | `f31f4e0b8618089d0e1906160608b329656269ced7744988033e840fecc94d8a` |
| h2b.patch | `e36da40f00aae9003f4719e0973eab433d2bed042e805e1de16c8e66682b4c0e` |
| h2ab.patch | `65fef1ef67a9aaf816aba740303f76cd67289fb60fa2247bd1a98656c37ea91c` |
| Harness source | `pipeline_ab.cpp` `6aa9e30c...` |
| Primary runner | `run_ab.sh` `23066988...` (frozen before the campaign), retained at `results/ab-15/run_ab.frozen.sh` |
| Replication runner | `replicate.sh`, retained at `results/ab-clean/replicate.sh` |
| Committed runner | `run.sh` (provenance + self-validation added; measurement loop unchanged) |

Per-arm binary/library hashes: `results/ab-15/arm-identity.txt` and
`results/ab-clean/arm-identity.txt`. The replication used the same binaries and
libraries as the primary campaign; the recorded hashes match. The campaign
binaries and static libraries were retained on E5 and their SHA-256 values were
re-checked against the committed identity files. Local rebuilds are not expected
to match byte-for-byte (archive timestamps/build paths), so the E5 records—not a
local rebuild—are the identity chain.

## 3. Validation

`validate-results.sh` + `analyze.py` check, for every `(config, arm, rep)`:

- exactly one `timed` row with `ok=true` and no failure text;
- exactly one `timed` row with `ok=true` and an empty `failure`;
- exactly one `oracle` row with `ok=true`;
- exactly one `latency` row with `ok=true`;
- `allocations == 0` in the timed phase;
- `ops == requests`.

The timed-`ok` check was missing from the first version of the validator and was
added during review. All 4500/1575 existing timed rows have `ok=true`, so the
strengthened gate does not change any result; both campaigns were revalidated
with it.

Results:

| Campaign | Rows | Oracle failures | Timed failures | Timed allocations | Problems |
|---|---:|---:|---:|---:|---:|
| primary | 4500 | 0 | 0 | 0 | 0 |
| replication | 1575 | 0 | 0 | 0 | 0 |

Correctness suites (all arms, E5):

| Arm | Tests | Result |
|---|---:|---|
| base | 74 | 74 passed |
| h1 | 75 | 75 passed |
| h2a | 74 | 74 passed |
| h2b | 75 | 75 passed |
| h2ab | 75 | 75 passed |

The H2 parity mutant (`uring_owed_counter_parity_mut_no_decrement`) fails
`counter_tracks_zero_op_owed_events`; the H1 identity envelope test passes all
seven controls. No `CORRECTNESS_REJECTED` condition occurred.

The suites were run before the campaigns as the admission gate. The retained
`results/tests/*.log` files are a re-run on the same worktrees (their mtimes are
later than the campaign outputs because the earlier console output was not
saved). The logs have no timestamps, so the ordering is procedural, not
timestamp-evidenced; the A/B oracle independently verifies read correctness, but
it does not exercise the H2b owed-counter paths.

## 4. Statistical treatment

For each configuration and arm, the harness produces one timed row per
repetition. The analysis forms the paired ratio `arm / base` per repetition for:

- wall seconds (`wall_s`);
- process CPU ns/op (`cpu_ns_per_op`);
- oracle-phase p99 latency (`p99_ns`).

The reported point estimate is the median of the 15 ratios; the 95% interval is
a percentile bootstrap over the 15 paired ratios (10,000 resamples, seed
20261021), not a CV threshold. With 15 pairs the interval is intentionally
coarse; it is used to reject "no difference", not to claim a precise effect
size. All claims below are paired within a campaign; no absolute ns/op is
compared between E5 and WSL2.

## 5. Headline results

CPU-time ratio, clean replication (lower is better; full tables in
`h1-results.md` and `h2-results.md`):

| Workload | H1 | H2a | H2b | H2ab |
|---|---:|---:|---:|---:|
| 4 KiB, C=64, d=1 | 0.986 | 0.920 | **0.884** | 0.902 |
| 4 KiB, C=64, d=8 | 0.981 | 0.963 | **0.914** | 0.946 |
| 4 KiB, C=1024, d=1 | 0.963 | 0.592 | **0.478** | 0.477 |
| 4 KiB, C=1024, d=4 | 0.977 | 0.741 | **0.678** | 0.683 |
| 4 KiB, C=1024, d=8 | 0.963 | 0.798 | **0.740** | 0.744 |
| 64 KiB, C=1024, d=1 | 0.988 | 0.747 | **0.648** | 0.647 |
| 64 KiB, C=64, d=8 | 1.005 | 1.001 | 0.988 | 0.995 |

Primary campaign agrees in sign and magnitude; the largest H2b effect there is
0.487 at 4 KiB/C=1024/d=1.

Decisions:

| Mechanism | Decision | Reason |
|---|---|---|
| H1 direct index | **MICROBENCH_ONLY** | lookup 0.04-0.82× in isolation, but ≤3.7% end-to-end; requires product-domain narrowing |
| H2a single-lock snapshot | **MECHANISM_IMPROVEMENT_CONFIRMED** (not selected) | removes per-slot lock pairs; H2b is better on the decisive 4 KiB/64 KiB configs, but H2a is better at 1 MiB/C=64/d=1 |
| H2b derived count | **MECHANISM_IMPROVEMENT_CONFIRMED** | 0.478-0.988 on the decisive 4 KiB/64 KiB configs; neutral at 1 MiB; system time essentially unchanged |
| H2a+H2b | **not selected** | usually 1.01-1.04× H2b and sign-flips at 4 KiB/C=1024/d=1; no repeatable gain |

H2b's remaining 0.478 floor at 4 KiB/C=1024/d=1 is consistent with the perf
split: user time falls from 2.468 s to 0.735 s while system time stays ≈1.05 s.
Further gains would have to come from kernel submission/completion or from the
real dispatch/control work, not from the owed sweep.

## 6. Environment during measurement

Primary campaign: `loadavg(1)` 1.15-3.20 (median 2.48); the owner's
`parquet_bench` was active for part of it (not stopped). Replication:
`loadavg(1)` 0.17-1.54 (median 1.35, including our own pinned runner); no sample
at or above 2.0. Frequency telemetry: mostly ≈3.19 GHz, with idle samples near
1.20 GHz and one implausible 6.41 GHz artifact. CPU 2's SMT sibling is CPU 12;
it was not cpuset-isolated. These are recorded threats, mitigated—not
eliminated—by paired/rotated ordering and the independent replication.

## 7. Artifact retention

Committed (small, reviewable):

- `results/ab-15/{validation.txt,ab-summary.json,arm-identity.txt,provenance.txt,run_ab.frozen.sh}`;
- `results/ab-clean/{validation.txt,ab-summary.json,arm-identity.txt,provenance.txt,replicate.sh}`;
- `results/tests/*.log` (correctness suites, H1 slot-only mutant kill, H2 mutant kill);
- Layer A summaries `results/layer-a/` (decisions, provenance, mechanisms.jsonl,
  perf_stat.txt);
- perf summaries `results/perf/` (stat samples, stat-summary.csv, flat reports,
  build IDs; raw `perf.data` not committed).

Not committed (large raw artifacts, kept on E5 under `~/sluice-m2/out/`; the
campaign binaries and static libraries are retained there too and their hashes
were re-checked):

| Artifact | Rows/Size | SHA-256 |
|---|---|---|
| `ab-15/ab.jsonl` | 4500 rows / 2.1 MiB | `b244bb7870a622c5860f4161eb1b0a63f4713d525c40d4e07b1589cd514f24ea` |
| `ab-15/frequency.jsonl` | 1500 rows / 196 KiB | `02f08980d3b7ce5779b053781cdece237aa6503e145f93e5095eba049ab5f86b` |
| `ab-clean/ab.jsonl` | 1575 rows / 752 KiB | `39718e7db8ed8914fe0d19dffc63ca2a7e2d66363d7c1942d791ec40f388a1f5` |
| `ab-clean/frequency.jsonl` | 525 rows / 69 KiB | `6ab0ee957c67c9b0a1e1b6d0ff7c0c78881b2be1186e9bb3b613e6f3f31606fb` |
| `perf/rec_{base,h1,h2a,h2b}.data` | 29.6-59.9 MB each | kept on E5; flat reports committed |

## 8. Reproduction

```bash
# 1. Create clean worktrees at the frozen baseline and apply one research patch.
git worktree add ~/sluice-m2/wt/base 8fdcd99c
for arm in h1 h2a h2b h2ab; do
  git worktree add ~/sluice-m2/wt/$arm 8fdcd99c
  git -C ~/sluice-m2/wt/$arm apply \
    docs/research/native-linux-m2/patches/$arm.patch
done

# 2. Build every arm in release mode (same flags).
for arm in base h1 h2a h2b h2ab; do
  (cd ~/sluice-m2/wt/$arm && xmake f -m release --liburing=y -y && xmake build -g bench)
done

# 3. Run the paired campaign (builds the harness per arm, validates at the end).
bash bench/native-linux-m2/run.sh ~/sluice-m2/wt ~/sluice-m2/out/ab 15 2

# 4. Re-validate an existing JSONL independently.
bash bench/native-linux-m2/validate-results.sh \
  ~/sluice-m2/out/ab/ab.jsonl 15
```

The patch files are research artifacts. `run.sh` does not apply them
automatically and never writes product `src/` or `include/`.

## 9. WSL2 to E5 reproduction (sign and ordering only)

The committed M1 campaign in `docs/research/state-complexity/m1-results.md` ran
Layer A on WSL2 (Ryzen 7 5800H, kernel 6.18.54.1). M2 re-ran the same mechanism
shapes on E5 (Xeon E5-2666 v3, kernel 7.2.9). Absolute ns/op are not comparable
between the two hosts; only the direction, ordering and rough magnitude are.

| H1 ratio (scan/direct) | WSL2 | E5 |
|---|---:|---:|
| capacity 8, lookup | 0.79x | 0.82x |
| capacity 64, lookup | 0.41x | 0.47x |
| capacity 1024, lookup | 0.05x | 0.06x |
| capacity 64, churn | 0.37x | 0.33x |
| capacity 1024, miss | 0.03x | 0.04x |

The direct arm is flat in capacity on both hosts and the scan grows with
capacity on both; the crossover (none inside the supported range) reproduces,
and the ratios agree within a few hundredths.

| H2 idle ns/pass | WSL2 | E5 |
|---|---:|---:|
| capacity 64, single-lock (H2a) | 129.2 | 205.9 |
| capacity 64, per-slot (baseline) | 458.0 | 601.9 |
| capacity 64, counter+snapshot (H2ab) | 87.9 | 166.6 |
| capacity 1024, single-lock | 1503.8 | 2290.9 |
| capacity 1024, per-slot | 6891.6 | 8749.3 |
| capacity 1024, counter+snapshot | 1016.8 | 1735.0 |

Both hosts order the shapes identically at idle
(counter+snapshot < single-lock < per-slot) and both show the per-slot sweep
scaling with capacity. The per-slot minus counter+snapshot gap is 370 ns/pass
at C=64 on WSL2 and 435 ns/pass on E5; at C=1024 it is 5875 versus 7014. The
mechanism's relative benefit is therefore reproduced, with E5 paying somewhat
more absolute per-pass cost. The production A/B on E5 is the deciding layer on
both hosts' evidence; the M1 Layer B estimate (H1 <=2%, CI 1-3%) is consistent
in direction with M2's end-to-end H1 result (<=3.7%), but the two use different
harnesses and depths and are not pooled.
