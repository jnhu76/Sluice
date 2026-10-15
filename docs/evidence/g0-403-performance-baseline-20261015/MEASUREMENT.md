# G0 (#403) measured baseline and bottleneck evidence

```text
[SLUICE-G0-403]
REPORT_VERSION = g0-2
BASE_SHA = dcc46266709e98d760b3be2957d18c09e1e55952
BENCHMARK_HEAD_SHA = c0bee1f8 (revision 2 bench sources; the six v2 binaries reproduce byte-identically)
V1_BENCHMARK_HEAD_SHA = 8c98b417
RAW_EVIDENCE_SHA = see COMMIT.txt (raw/ and raw-v1-oracle-degenerate/ were committed after run v2)
STAGE = B — measurement and bottleneck evidence (revision 2, correcting the oracle defect)
```

Preregistration (frozen before any measured repetition): `PREREGISTRATION.md`.
All numbers are medians over **7 measured repetitions** per case with 2 warmup
repetitions, on the host in `host-manifest.txt` (AMD Ryzen 7 5800H, 8 logical
CPUs = 4 cores x 2 SMT, WSL2 kernel 6.18.54.1-microsoft-standard-WSL2).

## 0. Revision note: the v1 content oracle was degenerate

The first published run (`raw-v1-oracle-degenerate/`, the evidence cited by
comment B) used a pattern table computed as `(i * 2654435761ull) >> 56`. For
every `i < 65536` that expression is **zero**, so every fixture was zero-filled
and every "byte verification" compared zeros with zeros. The *timed* phases are
unaffected — verification runs in a separate pass — but the v1 correctness claim
was vacuous, and two harness defects hid behind it:

1. `W01-write_at-4k-seq` wrote the same base-0 pattern to every block while the
   oracle expected offset-dependent content.
2. The out-of-order (`-anyready`) diagnostic reused `submitted % depth` as the
   buffer slot; once consumption is out of order, two in-flight requests could
   share one destination buffer — a LIFE-01 violation in the harness itself.

Both were found by the *fixed* oracle, not by inspection. The fix replaces the
table with a splitmix64 byte stream (all 256 byte values present), adds
`CAL-oracle-self-test` to every binary — it must pass before any case and it
proves that offset confusion, a single flipped byte and an all-zero buffer are
all detected — and fixes both defects. The self-test record is in each raw file.

**v1 → v2 movement.** The corrected run is authoritative; v1 is retained only
because comment B cites its hashes (they still match byte-for-byte). Where the
fixture content was zero, the deltas are large:

| Case | v1 ops/s | v2 ops/s | delta | note |
|---|---|---|---|---|
| `W01-read_at-1m-seq-cold` | 3,600 | 420 | **−88%** | v1 read zero pages from the sparse region; not a storage measurement |
| `W01-read_at-64k-seq-cold` | 68,624 | 13,132 | **−81%** | same |
| `W02-uring-64k-d16` | 111,666 | 62,951 | −44% | memcpy of real bytes vs zeros |
| `W02-threadpool-64k-d16-w4` | 72,503 | 54,150 | −25% | same |
| `W01-read_at-1m-seq` | 11,365 | 9,000 | −21% | same, at 1 MiB per operation |
| `W01-read_at-4k-seq` | 1,288,004 | 1,021,337 | −21% | same, at 4 KiB per operation |
| `W02-threadpool-4k-d16-w1` | 276,170 | 220,366 | −20% | same |
| `W01-write_all_at-4k` | 1,224,152 | 1,054,908 | −13.8% | partly deliberate: the timed source is now a per-slot pattern pool instead of one unfilled buffer |
| `W01-write_at-4k-seq` | 979,494 | 861,949 | −12.0% | same class; the v1 case also wrote identical content to every block |
| `W01-sync_data-4k-disk` | 1,367 | 1,392 | +1.8% | durability barrier unaffected |
| `W01-read_at-4k-rand` | 1,036,247 | 999,684 | −3.5% | 4 KiB real-byte reads |
| `W03-*`, `W02-*-saturate` | — | — | within ±1.5% | unaffected within spread |

The 4 KiB and 1 MiB read deltas (≈20%) are the same class of effect at page
granularity: a zero-filled page is cheaper to serve than a page with real bytes.
No conclusion in this report rests on a v1 number.

## 1. What executed, and the integrity check

| Run | Binary profile | Cases | Measured reps/case | Failures |
|---|---|---|---|---|
| `raw/w01.jsonl` | no-ring (core only) | 16 workload cases + 1 self-test record = 145 records | 7 | 0 |
| `raw/w02.jsonl` | ring | 18 cases (16 frozen + 2 preregistered `-scope64`) + self-test = 145 records | 7 | 0 |
| `raw/w03.jsonl` | ring | 6 cases + self-test = 55 records | 7 | 0 |
| `raw/w02-diagnostics.jsonl` | ring, `G0_DIAGNOSTICS=1` | 20 cases + self-test = 172 records | 7 | 0 |
| `raw/w02-noring-notrun.jsonl` | no-ring control | 16 + self-test = 17 records | 1 | 6 NOT_RUN (expected) |
| `raw/w03-noring-notrun.jsonl` | no-ring control | 6 + self-test = 7 records | 1 | 3 NOT_RUN (expected) |

Integrity check recomputed from the raw JSONL: **all measured repetitions have
`ok=true`; 7 measured repetitions for every case; `AsyncStats::max_outstanding`
equals the declared `queue_depth` and never exceeds it in any W-02 record;
`would_block` is 0 outside the deliberate saturation cases and exactly 200 inside
them.** The only `ok=false` records are the 9 deliberate
`NOT_RUN_WITH_REASON` io_uring records of the no-ring control.

Both backends run in ONE process for W-02/W-03 and cases are rotated per
repetition (`order[i] = (i + round) mod n`), so ThreadPool and io_uring alternate
within every round.

Reproduce: `bench/build_profiles.sh release && bench/run_matrix.sh <outdir> 2 7 all v2`
and `... 2 7 diagnostics ""`. The build script asserts each binary's profile from
its dynamic dependencies (only the ring build depends on `liburing.so.2`;
`bench_w01_direct` is byte-identical in both profiles because W-01 has no async
dependency).

Raw evidence hashes and the retained v1 set are listed in `COMMIT.txt` and
section 0; `REPRODUCIBILITY.md` records that the v2 binaries reproduce
byte-identically from the revision-2 benchmark head (`c0bee1f8`) and from the
later comment-only source revision.

## 2. W-01 — direct execution (root W-01, INV-02)

One uninstrumented timed phase per repetition; every measured repetition also
passed a verification pass that re-read **every** exercised offset and compared
the bytes against the (non-degenerate) file pattern. The two write cases verify
by readback over distinct per-offset content; the two `-disk` durability cases
verify their payload, and the read cases verify returned bytes at every offset.

### W-01
| Case | ops/s | spread | CV | p50 ns | p95 ns | p99 ns | max ns | CPU util | all reps ok |
|---|---|---|---|---|---|---|---|---|---|
| `CAL-clock-overhead` | 46,626,600 | 6.2% | 2.1% | 22 | 22 | 22 | 22 | 40743971.4% | yes |
| `W01-file_info` | 2,074,765 | 1.7% | 0.7% | 491 | 551 | 612 | 56,498 | 100.1% | yes |
| `W01-open_close` | 367,290 | 2.5% | 1.0% | 2,675 | 2,695 | 2,715 | 35,528 | 100.0% | yes |
| `W01-read_at-1m-seq` | 9,000 | 5.1% | 1.7% | 107,365 | 149,425 | 190,965 | 264,846 | 100.0% | yes |
| `W01-read_at-1m-seq-cold` | 420 | 0.3% | 0.1% | 52,120 | 81,756 | 111,623 | 173,120 | 7.6% | yes |
| `W01-read_at-4k-rand` | 999,684 | 9.8% | 3.2% | 952 | 1,473 | 2,084 | 64,563 | 100.0% | yes |
| `W01-read_at-4k-seq` | 1,021,337 | 6.5% | 2.4% | 912 | 1,413 | 2,034 | 91,324 | 100.0% | yes |
| `W01-read_at-64k-seq` | 119,611 | 4.3% | 1.5% | 7,825 | 10,600 | 17,824 | 69,503 | 100.0% | yes |
| `W01-read_at-64k-seq-cold` | 13,132 | 1.4% | 0.6% | 3,877 | 5,140 | 9,969 | 35,688 | 9.5% | yes |
| `W01-read_exact_at-4k` | 1,520,845 | 4.1% | 1.4% | 631 | 781 | 1,332 | 62,660 | 100.1% | yes |
| `W01-sync_all-4k` | 1,084,822 | 10.9% | 4.3% | 942 | 952 | 962 | 1,203 | 101.9% | yes |
| `W01-sync_all-4k-disk` | 1,081 | 9.8% | 3.7% | 710,944 | 1,651,657 | 1,983,371 | 2,132,456 | 3.8% | yes |
| `W01-sync_data-4k` | 1,075,240 | 23.8% | 8.7% | 962 | 972 | 972 | 1,242 | 102.0% | yes |
| `W01-sync_data-4k-disk` | 1,392 | 3.3% | 1.2% | 696,851 | 816,269 | 925,840 | 1,525,786 | 4.2% | yes |
| `W01-write_all_at-4k` | 1,054,908 | 9.1% | 3.1% | 892 | 1,323 | 2,034 | 70,225 | 100.1% | yes |
| `W01-write_at-4k-seq` | 861,949 | 3.9% | 1.4% | 1,092 | 1,693 | 2,334 | 100,710 | 100.0% | yes |

Facts established by W-01 (v2):

* **Direct execution is a single-syscall path.** `strace -c` counts **1.0002
  `pread64` per operation and no other syscall** for `read_at-4k-seq`
  (`logs/attribution.txt`). p50 is 912 ns for a 4 KiB tmpfs read with real byte
  content.
* **Durability is a different cost class and is measured separately.** On tmpfs
  the barrier is ~0.96 µs (meaningless as durability: tmpfs has no backing
  store); on ext4 the same barrier is **696,851 ns p50** — ~725x more — while
  process CPU utilisation falls to ~4%, i.e. the caller blocks in the kernel.
* **The storage path is reachable but virtualized.** With `fadvise(DONTNEED)`
  the 1 MiB read falls from 9,000 to 420 ops/s and the 64 KiB read from 119,611
  to 13,132 ops/s. The device is a WSL2 `Virtual Disk` (`ROTA=1`) in a VHDX; the
  physical medium is NOT_OBSERVABLE from inside the distribution.
  **Annotation:** the eviction is issued in `prepare()` once per repetition, so
  the *throughput* phase is the attempted-cold measurement while the *latency*
  column for those two rows comes from an instrumented pass that does not
  re-evict — read the `-cold` p50/p95 as warm-cache figures standing next to
  attempted-cold throughput.
* **Composition is nearly free for a fully-satisfiable request**: `read_exact_at`
  p50 631 ns vs raw `read_at` 912 ns (the difference is within the spread).
* **Timer instrumentation costs 22 ns per timestamp** (`CAL-clock-overhead`),
  which is why the throughput phases never call the clock.

## 3. W-02 — bounded RequestScope pipeline

### W-02
| Case | ops/s | spread | CV | p50 ns | p95 ns | p99 ns | max ns | CPU util | all reps ok |
|---|---|---|---|---|---|---|---|---|---|
| `W02-threadpool-4k-d1-w1` | 26,223 | 3.3% | 1.2% | 24,597 | 42,907 | 66,425 | 158,894 | 88.0% | yes |
| `W02-threadpool-4k-d16-w1` | 220,366 | 17.2% | 6.1% | 69,183 | 116,492 | 146,068 | 253,164 | 134.6% | yes |
| `W02-threadpool-4k-d16-w4` | 111,757 | 4.1% | 1.3% | 134,187 | 176,657 | 222,213 | 404,834 | 181.4% | yes |
| `W02-threadpool-4k-d16-w4-saturate` | 109,527 | 15.7% | 5.4% | 0 | 0 | 0 | 0 | 179.6% | yes |
| `W02-threadpool-4k-d16-w4-scope64` | 113,007 | 2.5% | 0.9% | 134,066 | 178,010 | 223,067 | 378,744 | 180.8% | yes |
| `W02-threadpool-4k-d16-w8` | 108,043 | 5.3% | 1.8% | 136,811 | 190,314 | 217,816 | 296,075 | 182.7% | yes |
| `W02-threadpool-4k-d4-w4` | 106,679 | 2.5% | 0.8% | 27,333 | 51,318 | 70,484 | 163,191 | 175.0% | yes |
| `W02-threadpool-4k-d4-w4-scope64` | 106,660 | 8.3% | 2.7% | 27,313 | 50,676 | 67,741 | 157,626 | 176.9% | yes |
| `W02-threadpool-4k-d64-w4` | 138,511 | 7.1% | 2.6% | 556,203 | 646,955 | 732,710 | 918,134 | 190.9% | yes |
| `W02-threadpool-64k-d16-w4` | 54,150 | 15.8% | 5.1% | 300,665 | 465,790 | 500,462 | 589,617 | 186.3% | yes |
| `W02-uring-4k-d1` | 26,644 | 2.8% | 1.2% | 24,106 | 40,938 | 64,042 | 129,117 | 91.7% | yes |
| `W02-uring-4k-d16` | 287,764 | 28.4% | 11.5% | 73,670 | 112,775 | 137,493 | 217,576 | 181.4% | yes |
| `W02-uring-4k-d16-saturate` | 274,404 | 12.5% | 4.2% | 0 | 0 | 0 | 0 | 177.0% | yes |
| `W02-uring-4k-d4` | 166,671 | 23.0% | 8.6% | 17,544 | 42,501 | 73,753 | 199,632 | 136.5% | yes |
| `W02-uring-4k-d64` | 291,082 | 9.0% | 3.0% | 328,608 | 453,236 | 534,742 | 669,850 | 175.6% | yes |
| `W02-uring-64k-d16` | 62,951 | 3.6% | 1.2% | 247,222 | 290,976 | 363,054 | 397,900 | 142.2% | yes |

### W-03
Facts established by W-02 (v2):

* **The request path costs 27x the operation it wraps at single outstanding**
  (p50 912 ns direct vs 24,597 ns ThreadPool d1), which is a 39x throughput
  difference (1,021,337 vs 26,223 ops/s) on the same file.
* **Adding ThreadPool workers still destroys throughput**: at depth 16,
  **w1 = 220,366, w4 = 111,757, w8 = 108,043 ops/s** (v1 showed the same ordering
  at 276,170 / 112,027 / 107,206). The inversion is robust: w1's minimum
  (195,206) exceeds w4's maximum (114,366). CPU per operation rises
  **6.18 → 16.17 → 16.91 µs** from w1 → w4 → w8 (2.6x), taken as the median over the 7
  repetitions of `cpu_s / wall_s ÷ ops_per_sec` in this primary run.
  ready first instead of the oldest changes nothing: **d16 109,730 (FIFO) vs
  109,212 (any-ready), −0.5%**; at d64 any-ready is 7.1% *slower*. The harness's
  ordered consumer is therefore not the cause of the inversion.
* **The slot-scan explanations are small and now measured one capacity at a
  time.** Holding depth, backend capacity and worker count fixed while raising
  only the scope tracking capacity: d4 99,972 → 98,012 (−2.0%), d16 109,730 →
  108,810 (−0.8%). Raising only the backend slot capacity 16 → 64 at fixed
  concurrency: 109,730 → 106,514 (−2.9%). Both are real but minor.
* **io_uring shows no inversion and saturates near depth 16**: d1 26,644,
  d4 166,671, d16 287,764, d64 291,082 ops/s — but d64 buys no throughput and costs
  tail latency: the medians of the per-repetition p50/p95 in the table above move
  73,670 → 328,608 ns (4.5x) and 112,775 → 453,236 ns (4.0x) from d16 to d64.
  full window is accepted and the rest rejected with `would_block` (200
  rejections per 200 rounds), `max_outstanding` = 16, no other error kind.

## 4. W-03 — external event loop

### W-03
| Case | ops/s | spread | CV | p50 ns | p95 ns | p99 ns | max ns | CPU util | all reps ok |
|---|---|---|---|---|---|---|---|---|---|
| `W03-threadpool-4k-d16-w4` | 95,938 | 4.9% | 2.0% | 83,219 | 229,238 | 274,906 | 458,426 | 191.8% | yes |
| `W03-threadpool-4k-d4-w4` | 53,065 | 6.9% | 2.3% | 47,731 | 79,532 | 98,450 | 197,978 | 173.2% | yes |
| `W03-threadpool-4k-d64-w4` | 107,785 | 10.0% | 4.0% | 293,541 | 911,917 | 1,060,963 | 1,439,143 | 195.6% | yes |
| `W03-uring-4k-d16` | 143,620 | 12.9% | 4.5% | 64,884 | 165,234 | 205,056 | 394,354 | 165.7% | yes |
| `W03-uring-4k-d4` | 76,589 | 8.2% | 2.7% | 33,094 | 62,907 | 94,110 | 180,809 | 125.9% | yes |
| `W03-uring-4k-d64` | 130,976 | 11.4% | 3.9% | 335,480 | 580,840 | 702,006 | 1,135,123 | 167.0% | yes |

Facts established by W-03 (v2):

* **Wake amplification is sublinear in depth.** Measured notifications per
  operation (v2): ThreadPool 0.410 (d4) → 0.133 (d16) → 0.047 (d64); io_uring
  0.598 → 0.198 → 0.031. Operations per observed wake rise from 2.4 to 21 for
  ThreadPool and 1.7 to 32 for io_uring.
* **Every observed notification costs exactly one progress pass**
  (`passes == notifications` in every measured W-03 record), and **`empty_wakes`
  is 0 in every measured record** — no spurious wakeups were observed at these
  depths.
* **The external loop is more expensive than the owned-driver scope at the same
  depth**: d16 threadpool 95,938 vs 111,757 (W-02); d16 io_uring 143,620 vs
  287,764. For io_uring that is a 2.0x gap — recorded as an open measured gap
  (B-G0-05), not as an attributed bottleneck.
* **No busy polling**: the driver parks in `epoll_wait` and the submitter parks
  on its own condition variable; CPU utilisation stays at 1.2-2.0 of 8 CPUs.

## 5. Profiling and causal attribution

All raw profiling output and the exact commands are committed under
`logs/attribution.txt` (plus the per-tool files and the symbol-preserving
profiling build's hashes at the top of that file). `perf` and `strace` perturb
the run, so only counts, ratios and per-operation counters are quoted; the
profiling build uses the same `-O3 -DNDEBUG` code shape but is unstripped, and
its absolute throughput is never compared with the baseline.

### 5.1 Per-operation syscall cost (`strace -c`)

Each measured repetition runs three passes over the operation pattern
(uninstrumented throughput, byte-verifying oracle, instrumented latency), so one
recorded operation equals three phase-operations; figures are per
**phase-operation**.

| Path | syscalls per operation |
|---|---|
| W-01 direct `read_at` | **1.00 `pread64`**, nothing else |
| W-02 io_uring d16 | **1.0001 `io_uring_enter`**, nothing else above noise |
| W-02 ThreadPool d16 (w1) | 1.00 `pread64` + 1.00 eventfd `write` + 1.99 eventfd `read` + 0.98 `poll` + **4.96 `futex`** ≈ **9.9** |
| W-02 ThreadPool d16 (w4) | 1.00 `pread64` + 1.00 eventfd `write` + 0.07 eventfd `read` + <0.02 `poll` + **3.61 `futex`** ≈ 5.7 |

`strace -y` identifies the notification fd as `anon_inode:[eventfd]` and
attributes the writes: **three writer threads each writing exactly 20,000 times
for 20,000 operations** (one per phase/context) and a **single reader** (the
progress owner) doing 119,589 eventfd reads per 60,000 phase-ops. Each completion
therefore generates exactly one notification signal, at every depth.

### 5.2 Hardware counters (`perf stat`, user space only)

Figures below are taken from the committed captures
(`attribution/perf-stat-W02-threadpool-4k-d16-w*.txt`, one repetition of 20,000
recorded operations = 60,000 phase-operations each) and normalised **per recorded
operation**. Instructions and branches are flat across worker counts while cycles
and cache misses grow — the extra cost is stalls, not new code.

| Case | cycles/recorded op | instructions/recorded op | cache-misses/recorded op | branches/recorded op | IPC |
|---|---|---|---|---|---|
| ThreadPool d16 w1 | 11,255 | 15,011 | 187 | 3,221 | 1.33 |
| ThreadPool d16 w4 | 13,517 | 14,818 | 296 | 3,164 | 1.10 |
| ThreadPool d16 w8 | 21,780 | 14,761 | 603 | 3,136 | 0.68 |

Divided by the three passes, that is 3,752 / 4,506 / 7,260 cycles and 62 / 99 /
201 cache misses per phase-operation, with instructions at 5,004 / 4,939 / 4,920.

`context-switches:u` and `cpu-migrations:u` read 0 for this process at
`perf_event_paranoid=2` — recorded as UNAVAILABLE_WITH_REASON, not as evidence of
zero contention.

`perf record` DSO split for ThreadPool d16 w4, from the committed capture
(`attribution/perf-w4.data`): **[unknown] 47.5%**, libc 23.7%,
bench+library 23.3%, vdso 2.8%, libstdc++ 1.6%. The `[unknown]` bucket is not
literally all kernel — it is samples whose DSO has no resolvable symbols, which
at `perf_event_paranoid=2` means the kernel samples (`[k]` frames were observed
there) plus any unresolved mapping; it is quoted as an upper bound on the kernel
share, not as a measured kernel fraction.

Top in-binary symbols by self percentage (resolved with `addr2line`, since perf
does not symbolise the main DSO here, self percentages summed across addresses):
`RequestScope::take` 1.74% across three, `ThreadPoolBackend::submit_read` 1.47%
across three, `RequestCore::occupancy()` 1.24% across three,
NOPE

### 5.2b Per-operation mutex traffic (LD_PRELOAD probe, test-side)

A test-side shim interposes `pthread_mutex_lock`; an added `pthread_mutex_trylock`
counts one *contended* acquisition. Source: `logs/mutex_probe.c`; output:
`logs/mutex-probe.txt` (the table below); the re-run in `logs/attribution.txt` reports
the same shape with slightly different counts. The shim perturbs every lock, and
its effect on throughput is configuration-dependent. Captured while the v1 medians
were still the baseline, it ranges against *those* from −18.4% (d16 w1) to +6.5%
(w4), with +3.3% (w8), +5.1% (io_uring d16) and −5.1% (d1 w1); recomputed against
the authoritative v2 medians the same runs are +2.2% / +6.7% / +2.5% / +10.8% /
−3.0%. Each probe configuration has a single repetition, so neither range is a
distribution. Only counts and rates from this probe are used anywhere in this
report; no throughput figure from it is compared with the baseline binary.

| Case | lock acquisitions | per phase-op | contended | contended rate |
|---|---|---|---|---|
| ThreadPool d16 **w1** (fastest ThreadPool) | 1,942,464 | 32.4 | 95,752 | **4.93%** |
| ThreadPool d16 **w4** | 1,271,956 | 21.2 | 22,136 | 1.74% |
| ThreadPool d16 **w8** | 1,248,509 | 20.8 | 17,404 | 1.39% |
| **io_uring** d16 (fastest path) | 1,118,432 | 18.6 | **0** | **0.0000%** |
| ThreadPool **d1 w1** (slowest case) | 2,698,944 | 45.0 | **161** | **0.006%** |

Two consequences:

* **Lock contention is not the cause of the inversion**: the contended rate is
  highest in the fastest ThreadPool configuration and is zero on the fastest path
  of all. Contention is anti-correlated with slowness.
* The request path takes **18-45 mutex acquisitions per 4 KiB read** even when a
  single thread drives everything (io_uring: 18.6, zero contended). The probe
  counts only interposed `pthread_mutex_lock` calls, so this is a **floor**, not
  a total.

### 5.3 CPU-placement discriminator

Pinning to fewer or distinct physical cores changes the w4 result materially but
never approaches w1. From the committed capture (`logs/attribution.txt`, placement
section; one repetition per configuration, so these are single-run values, not
medians): w4 unpinned **111,787 ops/s**, w4 on two physical cores (`taskset -c 0-3`)
**178,913** (+60%), w4 on four distinct cores (`0,2,4,6`) **162,547** (+45%), against
w1 at **226,830** unpinned, **231,260** on one physical core (`0,1`) and **220,892**
on two distinct cores (`0,2`) in the same section. Pinning therefore recovers a large
part of the w4 penalty, but w4 still stays below w1 and w1 is insensitive to
placement — so placement is a contributing factor, not the cause.
BOTTLENECK_ID = B-G0-01
OBSERVED_SYMPTOM = the ThreadPool request path costs ~27x the wrapped operation
  at single outstanding (p50 24,597 ns vs 912 ns) and ~10 syscalls per operation
  in steady state at every depth.
MEASURED_COST = 1 eventfd signal write + ~2 eventfd drain reads + ~1 poll +
  ~5 futex per completed operation (d16/w1); 1 io_uring_enter/op for uring and
  1 pread/op for direct. The `[unknown]` DSO bucket — an upper bound on the kernel
  share, not a measured kernel fraction — is 47.5% for w4 (section 5.2).
  strace -c / strace -y in logs/attribution.txt.
SOURCE_LOCATION = src/async/threadpool_backend.cpp (completion signal, work_mtx_/
  work_cv_ handshake), src/async/detail/progress_source.cpp and
  include/sluice/async/detail/progress_source.hpp (arm/drain), 
  src/async/async_io_context.cpp (owned-driver wait).
HYPOTHESIS = the completion->owner handshake is a fixed per-operation cost: the
  completion path signals the eventfd once per completion regardless of depth,
  and every parked wait performs an arm/acknowledge/drain sequence.
ALTERNATIVE_EXPLANATIONS = (a) the pread dominates -> refuted (direct path: same
  pread, 1 syscall, 912 ns); (b) profiler artefact -> refuted (counts are
  independent of perf); (c) slot scans -> B-G0-03/B-G0-04, both <=3%.
DISCRIMINATING_EXPERIMENT = syscall counts per operation for direct vs
  ThreadPool vs io_uring at the same block size, depth 16, same phase structure.
RESULT = 1.0 vs ~9.9 vs 1.0 syscalls per operation.
CONFIDENCE = high for the cost; medium for the split between the signal write and
  the wait handshake (the wait count depends on how often the owner parks).
AFFECTED_REQUIREMENTS = PROG-01, PROG-02, PROG-03, REQ-06, W-02, W-03.
PROOF_PRESERVATION_OBLIGATIONS = suppressing or batching a signal must preserve
  PROG-02 persistent readiness and must not remove a work obligation; needs
  re-validation against the progress/observer model.
```

```text
BOTTLENECK_ID = B-G0-02
OBSERVED_SYMPTOM = ThreadPool throughput inverts with worker count at depth 16:
  w1 220,366 > w4 111,757 > w8 108,043 ops/s, while CPU utilisation rises
  134.6% -> 181.4% -> 182.7% and p50 latency rises 69,183 -> 134,187 -> 136,811 ns.
MEASURED_COST = 1.97x throughput loss and 2.6x CPU per operation (6.18 -> 16.91 µs as
  the median of the per-repetition `cpu_s/wall_s ÷ ops_per_sec`)
WORKLOAD_AND_PROFILE = W02-threadpool-4k-d16-{w1,w4,w8}; perf stat per-op
  counters; perf record DSO/symbol attribution; strace; the lock probe.
SOURCE_LOCATION = src/async/detail/request_core.cpp (single mutex_ guarding
  accept/occupancy/publication/reclaim), src/async/threadpool_backend.cpp
  (work_mtx_/work_cv_ on submit, dequeue, completion; occupancy() at lines 587,
  774, 835), src/async/async_io_context.cpp (context access mutex across the
  backend submit).
HYPOTHESIS = every operation's state is touched by two threads through many
  mutual-exclusion steps, so each operation carries inter-thread coherence
  traffic that grows with the number of participating cores; more workers mean
  more distinct cores touching the request slot, queue node and counters, so the
  cost is cache-line transfer and stalls rather than blocking.
ALTERNATIVE_EXPLANATIONS =
  (a) CPU exhaustion -> refuted (1.3-1.8 of 8 CPUs used).
  (b) **lock contention -> refuted by direct measurement**: contended-acquisition
      rate 4.93% in the fastest ThreadPool configuration, 0.006% in the slowest,
      0% for io_uring (section 5.2b). Anti-correlated with slowness.
  (c) **ordered-consumer head-of-line blocking -> refuted**: consuming whichever
      request is ready first instead of the oldest gives d16 109,212 vs 109,730
      ops/s (-0.5%) and d64 135,964 vs 146,340 (-7.1%). The harness's FIFO
      consumer is not the cause.
  (d) CPU placement / SMT sharing -> contributing, not causal: pinning w4 to two
      physical cores recovers 60% (111,787 -> 178,913) but stays below w1
      (226,830-231,260) in the same capture, and w1 is placement-insensitive.
  (e) O(capacity) slot scans -> B-G0-03/04, both <=3% with one capacity varied at
      a time.
  (f) The submitter is the hard ceiling -> not supported: the same submitter
      sustains 220,366 submissions+consumes per second at w1 and 111,757 at w4.
DISCRIMINATING_EXPERIMENT = worker dose-response at fixed depth and work per
  operation; per-op instruction/cycle/cache counters; per-call futex blocking
  time; an LD_PRELOAD lock probe separating acquisitions from contended
  acquisitions; a completion-order variant; a CPU-placement sweep.
RESULT = instructions per recorded operation essentially constant (15,011 -> 14,818 ->
  14,761) while cycles rise 11,255 -> 13,517 -> 21,780 and cache misses rise
  187 -> 296 -> 603 (IPC 1.33 -> 1.10 -> 0.68), all from the committed captures;
  throughput; ordering and contention both refuted by experiment.
CONFIDENCE = medium for the mechanism (coherence/stall traffic is inferred from
  cache-miss counts, IPC and the placement result; cross-core invalidation counts
  and lock hold time are NOT_OBSERVABLE here); high for the refutations and for
  the inversion itself.
AFFECTED_REQUIREMENTS = REQ-01..REQ-06, PROG-01, THREAD-01, W-02.
PROOF_PRESERVATION_OBLIGATIONS = RequestCore's mutex is load-bearing for
  acceptance/reclaim/publication atomicity; any granularity change must preserve
  every REQ-02/REQ-04/REQ-05 invariant and re-validate the core correspondence.
```

```text
BOTTLENECK_ID = B-G0-03  (REJECTED as a bottleneck)
OBSERVED_SYMPTOM = RequestScope::resolve_ linearly scans the slot table on every
  take/wait_for, so per-operation cost was expected to grow with scope capacity.
MEASURED_COST = 2.0% (d4: 99,972 -> 98,012) and 0.8% (d16: 109,730 -> 108,810),
  with depth, backend capacity and worker count held fixed and only the scope
  tracking capacity raised 4->64 / 16->64 (raw/w02-diagnostics.jsonl, 7 reps each).
DISCRIMINATING_EXPERIMENT = one-capacity-at-a-time diagnostic.
RESULT = real but small; not worth an isolated optimization on this evidence.
CONFIDENCE = high (7 repetitions, single-capacity isolation, same-run controls).
```

```text
BOTTLENECK_ID = B-G0-04  (REJECTED as a bottleneck)
OBSERVED_SYMPTOM = RequestCore::occupancy() scans every slot under the core
  mutex and is called from poll_progress, outstanding and the worker completion
  path.
MEASURED_COST = 2.9% (d16 w4: 109,730 -> 106,514) with concurrency held at 16 and
  only the backend slot capacity raised 16->64 (raw/w02-diagnostics.jsonl, 7 reps).
DISCRIMINATING_EXPERIMENT = core capacity sweep at fixed concurrency.
RESULT = the scan length is not what makes the pipeline slow; the mutual
  exclusion and coherence traffic around it are (B-G0-02).
CONFIDENCE = high (7 repetitions, single-capacity isolation).
```

```text
BOTTLENECK_ID = B-G0-05  (open, MORE_EVIDENCE_REQUIRED)
OBSERVED_SYMPTOM = the external-loop integration is 1.16x (ThreadPool) to 2.0x
  (io_uring) slower than the owned-driver scope at the same depth and block size.
MEASURED_COST = d16 io_uring 143,620 (W-03) vs 287,764 (W-02); d16 ThreadPool
  95,938 vs 111,757.
HYPOTHESIS = the cost is dominated by the app-level completion handoff (driver
  wake -> mutex-protected counter -> condition variable -> submitter) plus one
  progress pass per notification, rather than by the notification itself.
ALTERNATIVE_EXPLANATIONS = (a) extra thread and condvar handoff per wake;
  (b) `poll_progress` called once per notification where the owned-driver path
  amortises its wait; (c) the io_uring driver parking in epoll_wait rather than
  sitting in io_uring_wait_cqe, so every wake costs an extra syscall.
DISCRIMINATING_EXPERIMENT = not yet run; needs a pass-level breakdown with the
  app handoff removed, and a measurement of the 1 ms transport-retry path.
RESULT = not established; recorded as an open measured gap.
CONFIDENCE = low (gap measured, cause not attributed).
AFFECTED_REQUIREMENTS = PROG-01, PROG-03, PROG-04, W-03.
```

```text
BOTTLENECK_ID = B-G0-06
OBSERVED_SYMPTOM = the request path performs 18-45 mutex acquisitions per 4 KiB
  read on every backend, including the single-threaded io_uring path.
MEASURED_COST = 18.6 acquisitions/phase-op (io_uring d16, zero contended), 21.2
  (ThreadPool d16 w4), 32.4 (w1), 45.0 (d1 w1). Counts are a floor: the probe
  only sees interposed pthread_mutex_lock calls.
WORKLOAD_AND_PROFILE = W02-*; LD_PRELOAD lock probe in logs/attribution.txt.
HYPOTHESIS = the per-operation work is expressed as many small, separately locked
  steps; each is cheap alone but costs a coherence event and prevents the
  compiler from keeping the operation's state in registers.
ALTERNATIVE_EXPLANATIONS = (a) contention -> refuted (B-G0-02 (b)); (b) each
  acquisition protects a distinct invariant -> not yet established either way;
  (c) an artefact of tiny operations -> the 64 KiB cases show the same pattern at
  lower relative cost.
DISCRIMINATING_EXPERIMENT = acquisitions per operation per backend and depth
  (this probe) plus per-op cache-miss counts across thread counts.
RESULT = counts as above; no configuration approaches a hand-serialised path.
CONFIDENCE = high for the counts, medium for the cost interpretation.
AFFECTED_REQUIREMENTS = REQ-01..REQ-06, PROG-01, W-02, W-03.
PROOF_PRESERVATION_OBLIGATIONS = merging or removing a lock step must preserve the
  invariant that step protected; the invariant accounting must be written down
  before any code change.
```

```text
BOTTLENECK_ID = B-G0-07  (REJECTED)
OBSERVED_SYMPTOM = hypothesis that the harness's ordered consumer (always waiting
  for the oldest ticket) head-of-line blocks when workers complete out of order,
  and that this creates the apparent worker-count inversion.
MEASURED_COST = none: out-of-order consumption does not help (d16 -0.5%, d64 -7.1%).
DISCRIMINATING_EXPERIMENT = the `-anyready` diagnostic, 7 measured repetitions in
  raw/w02-diagnostics.jsonl.
RESULT = refuted. The inversion is a property of the measured paths, not of the
  consumer's ordering.
CONFIDENCE = high.
```

## 6. What is NOT_OBSERVABLE on this host, and how it could be measured

| Metric | Why not observable here | Non-invasive method that would measure it |
|---|---|---|
| Allocation count per operation | No public allocation counter exists | `LD_PRELOAD` interposition on `malloc`/`free` (test-side only), or a derived counter in an instrumentation build; RSS is recorded today as a coarse substitute |
| Mutex hold time | `perf lock` needs kernel support unavailable at `perf_event_paranoid=2`; `context-switches:u`/`cpu-migrations:u` read 0 | `LD_PRELOAD` shim timing `pthread_mutex_lock`/`unlock` per thread (the acquisition *count* probe already exists in `logs/`), or a test-side instrumentation build |
| Cross-core cache-line transfers | No counter for invalidations is exposed | `perf stat` of platform-specific coherence events if permitted, or an instrumented layout experiment |
| Kernel-side symbol attribution, and the kernel share itself | Kernel symbols are hidden at this `perf_event_paranoid`; the `[unknown]` DSO bucket mixes kernel samples with unresolved mappings, so the kernel share is **not separable** here (the bucket is only an upper bound: 47.5% for w4) | `perf` with kernel symbols / `ftrace` where permitted would separate them |
| Physical storage device behaviour | WSL2 `Virtual Disk` in a VHDX | Re-run the same frozen matrix on a native Linux host with a known device |
| Page-cache residency per page | Not exposed by the API | `mincore()` in a test-side probe, or `smaps`/`fincore` sampling between phases |
| Per-thread CPU time | `pidstat` (sysstat) not installed | Read `/proc/<pid>/task/*/stat` from a harness-side sampler |
| Segmented queueing / submission / completion latency | `AsyncStats` counts calls but attaches no timestamps and no stage clock is public | Test-side timestamps around submit and consume plus derived instrumentation inside the pipeline; the end-to-end submit-to-consume distribution is what is recorded today |

## 7. Stability

Defined convention: the **37 non-calibration cases of the primary v2 runs**, each
with 7 measured repetitions, CV computed with the **sample** standard deviation
over repetitions of `ops_per_sec`.

* CV ≤ 3%: **22 / 37**; CV ≤ 6%: **33 / 37**; CV ≤ 15%: **37 / 37**.
* Worst five: `W02-uring-4k-d16` 11.5%, `W01-sync_data-4k` (tmpfs) 8.7%,
  `W02-uring-4k-d4` 8.6%, `W02-threadpool-4k-d16-w1` 6.1%,
  `W02-threadpool-4k-d16-w4-saturate` 5.4%.

Conclusions are drawn only from differences far larger than that noise (the
1.97x inversion, the 2-3% scan effects, the 3-21x syscall and lock-count
differences), never from a case inside its own spread. The per-case spread column
is in the tables above.
