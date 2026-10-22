# DP-0 hardware attribution (R1 dynamic validation)

Full receipt: `results/receipts/perf-calibration-20261023.txt`. This document
states what the PMU counters on this host can and cannot support, and which
claims in this study rest on them.

## Counter semantics (E1-incident-aware)

`perf stat -x,` emits `count,,event,time_enabled_ns,runtime_pct,…` per line.
The count is **field 1**; the time-enabled nanoseconds (field 4) are shared by
every counter on the line. E1's review
(`docs/evidence/e1-pending-control-productization-20261023/review-B-performance.md`,
finding F-1) recorded an attribution error from reading field 4 as the
instruction count. DP-0's parser reads field 1 keyed by field 3, and every
instruction count in this study was sanity-checked against IPC bounds
(0 < cycles/instructions < 6 gate; observed IPC 0.35–3.9).

## Validation performed on this host

1. **Linearity** (semantics): `calib2` with a 4-instruction loop gives
   I(n) = 4.0·n + 117 572 exactly across 3 M and 6 M iterations —
   `instructions:u` counts what it claims.
2. **Repeatability** (noise): an identical binary executing identical work
   (w01ctl × base, 30 runs) stays within a 3.1e-6 relative band
   (28 130 921…28 131 009; calibration receipt). Instruction deltas of ≥1 instruction/iteration
   are therefore resolvable when the executed code is identical.
3. **Paired interleaving** neutralizes frequency/thermal drift; the AB/BA
   pair structure and the self-pair control quantify residual scheduler noise
   (`results/analysis.md`).

## Events judged INVALID or unavailable on this host — and what that forbids

| event class | status | consequence |
|---|---|---|
| `instructions:u`, `cycles:u` | calibrated usable | used, with per-run gates |
| `L1-dcache-*`, `dTLB-*` | not supported by this kernel/host | never collected; cache/TLB effects are **analytical only** (assembly + layout arithmetic, e.g. dp3-slot-layout.md) |
| `cache-references`, `cache-misses` | obtained but inconsistent across repetitions (WSL2 PMU multiplexing) | judged **INVALID**; never collected; any document citing them for this host should be treated as erroneous |
| `task-clock`, `context-switches`, `cpu-migrations` | collected as sanity context only | no verdict rests on them |

This is the honest marking the audit requires: **no cache-line, TLB, or
memory-hierarchy number in this study is a hardware measurement.** Cache-line
touch counts in dp3-slot-layout.md are derived from byte offsets and executed
instruction sequences, with the host limitation stated beside each.

## What the counters do support for this study's claims

- Exact per-operation user-instruction deltas on deterministic loops (zop,
  w01ctl) — resolvable to ±1 instruction, validated against assembly
  predictions to within a few instructions (results/analysis.md).
- Direction-and-balance evidence on poll-spinning loops above the
  self-pair variance floor.
- Cycles/wall as regression context (not verdict evidence on this host).

## Host boundary

All dynamic statements hold for this WSL2 kernel (6.18.54.1-microsoft) and
GCC 15.2.0 build. The task's alternative host (E5/Fedora 44/GCC 16.2.1) was
explicitly not used; re-running `results/receipts/` receipts there is the
required reproduction path before generalizing any number.
