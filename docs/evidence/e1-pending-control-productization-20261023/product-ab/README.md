# E1 Product A/B — Final Production-Patch Verification (Gate R5)

## Formal verdict

**PASS** (`results/verdict.json`, reducer `../e1_product_verdict.py`,
selftest 16/16, pairing: per-replication ID). Campaign runner
`run_e1_product_ab.sh` (hash in `results/provenance.txt`), 15 interleaved
AB/BA rep pairs per config, pinned CPU 2, fail-closed per-row validation,
refuse-overwrite.

## Arms

| Arm | HEAD | `libsluice_async.a` sha256 |
|---|---|---|
| base | `859e4b56` (master, clean worktree `~/sluice-e1-prodbase`) | in `results/provenance.txt` (`lib_async_base_sha256`) |
| e1 | product HEAD (branch `perf/e1-pending-control-zero-scan`, clean worktree `~/sluice-e1-product`) | in `results/provenance.txt` (`lib_async_e1_sha256`) |

Both arms: clean `rm -rf build`, `xmake f -m release --liburing=y`,
`sluice_core` + `sluice_async`; harnesses compiled `-std=c++20 -O3 -DNDEBUG`
from the frozen in-tree research sources (`pipeline_ab_e1.cpp`,
`external_loop_ab_e1.cpp`, `bench_common.hpp`, hashes in provenance) linked
per arm. Core libs are byte-identical across arms (E1 touches only the
async layer); async libs differ (verified by the reducer's identity gate).

Provenance note (recorded here, the frozen file is not edited): the
runner's final SHA line collapsed `base_sha` and `e1_sha` onto one
`key=value` line, so the reducer's identity block shows `e1_sha: null`.
The load-bearing identity gates (four harness-binary hashes, fixture hash,
and the differing per-arm `lib_async` hashes) were parsed and enforced
correctly. The arm SHAs are `base_sha=859e4b5603002792d1c4efa116a68f400bc5d297`,
`e1_sha=<product HEAD at campaign start, see git log of this branch>`.

## W-02 (pipeline; per-rep paired medians, gate ≤0.95 headline + ≥12/15 wins, ≤1.03 others)

| Config | Median E1/Base CPU | Wins <1 | Gate |
|---|---|---|---|
| 4096 / C1024 / D1 (headline) | **0.6980** | 15/15 | PASS |
| 4096 / C1024 / D8 (headline) | **0.6196** | 15/15 | PASS |
| 4096 / C16 / D1 (negative control) | 0.9758 | 12/15 | PASS (≤1.03) |
| 1 MiB / C64 / D8 (I/O-dominated control) | 1.0004 | 6/15 | PASS (neutral, ≤1.03) |

## W-03 (real epoll+eventfd external loop; gate ≤1.03, verify rows required)

| Config | Median E1/Base CPU | Pairs | Verify |
|---|---|---|---|
| 4096 / C1024 / D1 | 0.7990 | 15 | 5+5 rows, all `no_lost_wake=true` |
| 4096 / C1024 / D8 | 0.6085 | 15 | (same verify corpus, shared) |
| 64 KiB / C1024 / D8 | 0.9202 | 15 | (same verify corpus, shared) |

## P>0 pressure row

Discharged by the seam-bound campaign (`../seam-bound/`): four P>0 phases,
per-rep medians 0.9382–0.9613 against the ≤1.05 line, P>0 observed in-lock
in 15/15 E1 reps and retained-intent-proven in 15/15 BASE reps.
`P_POSITIVE_NATIVE = NOT_MEASURED` (see `../reachability.md`,
`../native-p5-probe/`).

## Attribution (headline 4096/C1024/D1)

- `perf stat :u` over 3×20 000 ops: cycles 668 188 248 (base) →
  234 099 483 (e1), ratio **0.350**.
- Correction 2026-10-23 (independent review B, F-1): the previously quoted
  instruction figures 552 804 305 → 410 779 352 ("ratio 0.743") were a
  perf-CSV field misread — that field is the time-enabled column (ns),
  byte-identical on the instructions and cycles lines of each file and
  equal to the run durations (0.5528 s / 0.4108 s ≈ the 3-phase wall
  sums). The field-1 instruction counts (1 845 898 997 → 350 946 663)
  imply an IPC shift (2.76 → 1.50) inconsistent with the cycle and
  rusage ratios, so this run's instruction counters are not used as
  attribution. Cycles, rusage and the strace census carry the attribution.
- Timed-phase rusage: `utime` 4136.6 → 1598.2 ns/op (the user-space scan
  is the removed work); `stime` 4937.9 → 5101.2 ns/op (kernel side
  unchanged within noise).
- strace census: `io_uring_enter` = **60 003 on both arms**; `eventfd2`
  2–3 (setup); futex ≤2. The kernel boundary is byte-count identical; the
  entire gain is user-space redundant-work elimination.
- Wall latency (latency phase): p50 1470 → 566 ns, p95 9927 → 7176 ns.

The research round's headline observations (0.6930/0.6239) were historical,
not mandatory targets; this campaign's 0.6980/0.6196 reproduces them within
rep-to-rep variation.

## Historical replay (research data, read-only)

`e1_product_verdict.py`'s W-02 reduction replays the frozen research
campaign `docs/research/mw-hw-1/native-linux-e1/evidence/ab.jsonl`
(990 rows) with zero issues and reproduces the research round's published
per-rep medians exactly — headline 0.6930 / 0.6239, all 11 configs ≤0.9962.
The research `verdict.json` was not overwritten (the reducer also refuses
to write over any input path).
