# R5 fio Native Baseline — formal runs (serial lane) [RUN]

Date: 2026-10-29, 09:29–09:55 +08:00. All runs serial, exclusive lane, load
average recorded per run in `run-log.md`. Every number below carries its
section identity — sections are distinct loads and are never compared or
aggregated with each other.

## Dataset identity (per-run, generated at lane start)

`dd if=/dev/urandom of=build/fio-r0-data/<f> bs=4M count=2048 status=none; sync`

```
31547d63caa2b48ebf924321a21706c78f82c1ff524fc7afeeebcae82f32cb04  r0-rand-8g.bin
15f564a4d2e97e8af6eaa09fa77b722f0b89ebbf72498e06cea947be6a2dd210  r0-randrw-8g.bin
11383c8e50497bfb20f7a48d61a7e980418d4e35a8eecd8d7ccb62de3f675232  r0-seq-1m.bin
```

Per the frozen README, the identity is size (8 GiB each) + generation method +
per-run SHA-256. After the randrw rounds the `r0-randrw-8g.bin` hash is (as
predicted by the README):
`77c4abda2c131dea440e9a313d42d477d439f9188a42cf0ca17c8b7929271bf5` — the
read-only datasets are unaffected.

Run form (one section per invocation, from `benchmarks/fio-r0/README.md`):

```bash
fio --section=<name> --directory=$PWD/build/fio-r0-data \
    --output=<ev>/round<N>-<name>.json --output-format=json <jobfile>
```

fio 3.40 (fio-3.40-3.fc44), kernel 7.2.11, btrfs
`rw,relatime,compress=zstd:1,ssd,discard=async,space_cache=v2`, governor
`schedutil` (unchanged).

## Baseline results (read side; lat = read completion latency)

| Section (identity) | round | IOPS | MiB/s | lat p50 | lat p99 |
|---|---|---|---|---|---|
| r0-sync-4k-direct (psync, qd1, 4k randread, direct=1) | 1 | 2306.7 | 9.0 | 264 µs | 436 µs |
| r0-sync-4k-direct | 2 | 3297.9 | 12.9 | 276 µs | 449 µs |
| r0-sync-4k-direct | 3 | 3345.7 | 13.1 | 268 µs | 444 µs |
| r0-sync-4k-buffered (psync, qd1, 4k randread, buffered, warm) | 1 | 3236.1 | 12.6 | 293 µs | 457 µs |
| r0-sync-4k-buffered (warm) | 2 | 3311.1 | 12.9 | 277 µs | 449 µs |
| r0-sync-4k-buffered-cold (drop_caches before run) | 3 | 3284.2 | 12.8 | 277 µs | 453 µs |
| r0-sync-4k-buffered-cold (drop_caches before run) | 4 | 3235.5 | 12.6 | 285 µs | 457 µs |
| r0-libaio-4k-qd32-direct (libaio, qd32) | 1 | 2647.3 | 10.3 | 7.63 ms | 304 ms |
| r0-libaio-4k-qd32-direct | 2 | 4001.7 | 15.6 | 7.70 ms | 8.98 ms |
| r0-libaio-4k-qd32-direct | 3 | 4058.2 | 15.9 | 7.63 ms | 8.85 ms |
| r0-io_uring-4k-qd32-direct (io_uring, qd32) | 1 | 2671.8 | 10.4 | 7.63 ms | 292 ms |
| r0-io_uring-4k-qd32-direct | 2 | 3994.3 | 15.6 | 7.70 ms | 8.98 ms |
| r0-io_uring-4k-qd32-direct | 3 | 3989.6 | 15.6 | 7.70 ms | 8.98 ms |
| r0-seqread-1m-direct (psync, qd1, 1M seq read, direct=1) | 1 | 329.5 | 329.5 | 2.51 ms | 3.10 ms |
| r0-seqread-1m-direct | 2 | 389.3 | 389.3 | 2.54 ms | 3.10 ms |
| r0-seqread-1m-direct | 3 | 382.6 | 382.6 | 2.54 ms | 3.88 ms |
| r0-randrw-4k-r75-direct (psync, qd1, 4k randrw 75/25, direct=1) | 1 | 1138.5 (r) | 4.4 | 322 µs | 987 µs |
| r0-randrw-4k-r75-direct | 2 | 1444.0 (r) | 5.6 | 277 µs | 954 µs |

Write side of randrw: 380.0 / 482.4 IOPS (1.5 / 1.9 MiB/s).

Raw JSON: `fio/round*.json` in this directory. Per-run start time + load:
`fio/run-log.md`.

## A/A noise assessment [RUN]

Adjacent same-condition pairs (round N vs N+1, identical invocation):

| Section | pair | Δ IOPS | pair spread |
|---|---|---|---|
| r0-sync-4k-direct | r1→r2 | +42.9% | (outlier round, below) |
| r0-sync-4k-direct | r2→r3 (settled) | +1.4% | 1.4% |
| r0-sync-4k-buffered warm | r1→r2 | +2.3% | 2.3% |
| r0-sync-4k-buffered cold | r3→r4 | −1.5% | 1.5% |
| r0-libaio-4k-qd32 | r2→r3 (settled) | +1.4% | 1.4% |
| r0-io_uring-4k-qd32 | r2→r3 (settled) | −0.1% | 0.1% |
| r0-seqread-1m | r2→r3 (settled) | −1.7% | 1.7% |
| r0-randrw-4k-r75 | r1→r2 | +26.8% | 26.8% (see below) |

**Outlier disclosure (round 1 across sections).** Round 1 ran immediately
after the 24 GiB dataset generation (`sync` completed, but btrfs
`discard=async` / extent-cleanup background work was still settling): every
section's round 1 is the slowest sample, with QD32 p99 latency spikes of
~290–300 ms (rounds 2–3: ~9 ms). The settled rounds agree within ≤2.3%. The
round-1 samples are kept in the raw JSON (never deleted) and are excluded from
the settled A/A statement above; the honest conclusion is that this stack has
a warm-up/hysteresis of one round after bulk allocation, and any future
comparison must either discard the first post-allocation round or run a
settling pass first.

**randrw pair spread (26.8%)**: this section mutates its dataset on every run
(README-predicted), so rounds 1 and 2 are the same load shape but not the same
byte state; with only 2 rounds this spread is recorded as high-noise /
under-sampled, not as a baseline claim.

## What the baseline establishes (per-section, no cross-section comparison)

1. **r0-sync-4k-direct ≈ r0-sync-4k-buffered ≈ cold-buffered** (≈3.2–3.3k
   IOPS, ~12.6–13.1 MiB/s, p50 ≈ 264–293 µs): on this btrfs stack the 4k
   random read path is not page-cache-bound — with an 8 GiB dataset in 62 GB
   RAM, warm buffered (whole touched range cached after round 1) is within
   noise of direct, and drop_caches changes nothing beyond noise. The per-read
   cost is dominated by filesystem per-I/O work (btrfs extent lookup +
   checksum verification apply to both buffered and DIO reads on this
   filesystem) plus ~250 µs device latency. `O_DIRECT` on this stack buys no
   4k-random throughput; nor does the page cache.
2. **Queue depth barely helps** (qd32 libaio/io_uring ≈ 4.0k IOPS, +20% over
   sync qd1): the device/FS serializes almost all parallelism; libaio and
   io_uring are statistically indistinguishable at qd32 (settled pair <1.5%).
   No engine-choice advantage is visible on this host at this shape.
3. **The only shape where the storage shows bandwidth**: r0-seqread-1m-direct,
   ~330–389 MiB/s (settled pair 1.7%). Large sequential reads are the regime
   where this device's throughput exists.
4. Any Sluice Direct / composition opportunity on this host must therefore be
   argued in syscall-count / CPU-per-I/O terms or in the large-read regime —
   not in "O_DIRECT reaches hidden device performance", which the 4k sections
   refute on this stack.

## Statistical limits

- n=3 (sections with a settled round 3) / n=2 (buffered warm, randrw). These
  are baseline-identity numbers with disclosed pair spreads, not
  hypothesis-test evidence; no MDE claim is made for fio. The A/A purpose
  here is to characterize run-to-run noise, which the settled pairs bound at
  ≤2.3% (read sections, settled state) — sufficient for a native storage
  floor baseline whose section values differ from each other by ≥20×.
- `perf_event_paranoid=2` limits perf to user-space; not changed, not needed
  for fio.
