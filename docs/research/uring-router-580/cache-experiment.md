# R9-B/C — Cache-sensitivity experiment design and protocol

Frozen baseline: master `46157fd1`, branch `research/uring-router-580-wsl`
at `ca1e7c67` + the R9 additions (this revision). The R9 code changes are:
one compile-time gate (`SLUICE_580_CLFLUSH_ROUTER`, mechanism sensitivity
only) in `uring_transport.cpp` and new research harnesses under
`bench/research/uring-router-580/`. With no research defines, the patched
TUs compile byte-identical to pristine master on WSL gcc 15.2 (transport
`17f7899c…`, completion `fc4b0d19…`; the E5 proof is re-run in this round —
see `cache-results.md`). Production tree, spec, public API, and existing
benches/tests are untouched; PR #592 remains open and unmerged.

## Hosts and cache topology

| Host | CPU | L1d/core | L2/core | L3 | PMU events available |
|---|---|---|---|---|---|
| WSL2 (primary mechanism) | Ryzen 7 5800H (Zen 3), 8 CPUs | 32 KiB | 512 KiB | 16 MiB | cycles:u, instructions:u, L1-dcache-loads/misses:u, branch-misses:u, cache-misses:u — **LLC-loads/-misses: `<not supported>` → CACHE_ATTRIBUTION_UNAVAILABLE (partial)** |
| E5 (native recheck) | Xeon E5-2666 v3 (Haswell), 20 CPUs | 32 KiB | 256 KiB | 25 MiB | full set incl. LLC-loads/-misses (multiplexing when >4 events per group; ≤2 groups per pass) |

`perf_event_paranoid=2` on both (unchanged, read-only). No sysctl, no
drop_caches, no global parameter was modified anywhere in this round.

## Working-set placement

At 4 KiB interference per op: 64 KiB sits in L2; 512 KiB spans L2 capacity
(WSL) / exceeds it (E5 256 KiB L2); 4 MiB is an L3-resident stream; 16 MiB
exceeds nothing on E5's 25 MiB L3 but is at WSL's 16 MiB L3 capacity. The
router array itself is C×32 B (2 KiB @C64, 16 KiB @C512).

## Harness (`cache_router.cpp`, research-only)

Two execution shapes, both driven through the public API:

- **pipeline (W-02 shape)** — one owner thread through `RequestScope`,
  identical loop to the R6 `router_ab` runner (submit to depth, wait/take
  oldest, byte-pattern oracle per read), single core.
- **external (W-03 replica)** — submitter thread + driver thread that claims
  `claim_progress_owner()`, parks in `epoll_wait` on
  `progress_notification_fd()` and drives `poll_progress()` — the same
  public-API structure as `bench_w03_external_loop.cpp:172-258`, which
  cannot carry an interference seam without modifying a production bench.
  Pin modes: `single` (both threads on CPU 2 — the only legal form for the
  scope shape, which is single-owner by construction) and `split`
  (submitter CPU 2, driver CPU 3 — the real producer/poller separation).
  Driver-side interference is a separate cell (`--interference-side driver`).

Unsupported forms, marked: split-core for the pipeline shape (the scope
wait path drives progress on the caller thread; two-thread driving of one
scope is not a supported public-API form); LLC attribution on WSL.

**Interference workload**: deterministic sequential sweep over an
independent working-set buffer (one dependent 8-byte load per 64-byte line,
splitmix64-filled, cursor persists across the run; sink printed in the
output and identical across conditions). Interleaved between completions
(pipeline: after every take; external: per settled op on the submitter, or
per completed batch on the driver). Fixed **4 KiB/op** across all working-set
sizes, so locality varies while the interference work itself is constant.
Its seconds are metered separately (`interf_s`); `net_wall_s = wall − interf`
is reported for the pipeline shape, where one thread serializes both, so
cache-pressure overhead is never attributed to the sweep itself. User time
cannot be netted this way (disclosed; the sweep is pure user time inside the
timed region, identical across conditions of a pair).

**Flush arm** (`SLUICE_580_CLFLUSH_ROUTER`): every cookie lookup begins by
`clflush`-ing the whole router array — the fully-cold-per-lookup upper
bound. A mechanism sensitivity experiment only; no real application evicts
its own hot index synchronously before every access, and the result is never
used as a representative application number.

## Matrix

- **Probe matrix (instrumented, counts only, never timing)**: pipeline
  C=D ∈ {16, 64, 128, 256, 512} × ws ∈ {0, 64 KiB, 512 KiB, 4 MiB, 16 MiB},
  2 reps; external D=64 {warm, 16 MiB} × {single, split} + driver-side 16
  MiB split, 2 reps. Reads: lookups/install (must stay 1.00), per-site
  probes avg/p50/p99, misses (must stay 0), avg live depth.
- **Timing pairs (uninstrumented `cache_ab`)**: (warm, ws) AB/BA, 15 reps:
  D16×16M, D64×{64K, 4M, 16M}, D128/256/512×16M. A/A controls:
  byte-identical binaries under two paths at D64 warm, D64 16M, D512 warm
  (no-interference, small-D, high-D, same-interference floors).
- **Flush pairs**: (warm `cache_ab`, `cache_ab_flush`) at D64, D512, 15
  reps AB/BA.
- **External pairs**: D64 split {warm vs 16M submitter-side}, split
  {warm vs 16M driver-side}, single {warm vs 16M}, split A/A warm; 15 reps.

Reducer (`reduce_cache.py`): paired ratios interfered/warm (slot1/slot0 for
A/A), median + percentile bootstrap 95% interval (B=10000, seed 7), plus
absolute per-op medians per condition. The interval describes these pairs on
this host and session, not a population.

## Attribution (R9-C)

Per cell (D ∈ {64, 512} × {warm, 16 MiB} on both hosts):

- `perf stat -x,` two event groups (cycles/instructions/L1 loads+misses;
  LLC loads+misses/branch-misses/cache-misses) on the uninstrumented binary;
  unsupported events recorded verbatim as unavailable.
- `perf record -e cycles:u -F 1999` (`-m 64`, needed on E5 since M2) →
  self-share of `find_live_router_cookie_`; on WSL perf prints raw offsets,
  resolved through the binary's `nm` table (script embedded in
  `run_pmu_cache.sh`).
- Shares are read **together with absolute cycles/op** (stat totals ÷ ops)
  so a share change is never confused with a denominator change, and
  cache-pressure overhead is never attributed to the router without a
  symbol-level reason. This is the answer to the audit's finding 4.

Ordering of measurements on the shared hosts: probe matrix → pairs → flush →
external → PMU, one stage at a time; no other load runs during a stage
(E5 loadavg 0.18–0.34 at session start, rechecked in results).
