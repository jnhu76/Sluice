# aligne1-sweep-native-1 — notes

VALID (frozen sweep, GREEN): 840 runs (7 rounds × 120 cells), 0 gate
errors, same-work fail-closed (bytes/ops gates in-bench; driver-side dst
sha256 == src sha256 on every run). File size 128 MiB (AMENDMENT 1).
Interleaving: per-round seeded Fisher–Yates over all 120 cells (seed =
PREREG_SEED + round); no module block ordering.

Environment at run time: HEAD 468208df (clean tree), bench binary sha256
c78342a508b7…, bare metal Fedora 44 kernel 7.1.9-200.fc44.x86_64, Xeon
E5-2666 v3 10C/20T, btrfs zstd:1 SATA SSD (warm page-cache timed path;
background compressed writeback of the incompressible 128 MiB dst per run
adds machine noise — see the noise windows below).

Replica-natural slot geometry (residual recorded per run in raw
runs.jsonl): the malloc'd slot addresses are DETERMINISTIC per (chunk,
depth): depth-1 cells landed at exposed residual 16 (mod 32) — the
ALIGN-E0 tested-slow class — in every round; depth-2 at (16,16); depth-4
and depth-8 at (0,0,…) — the fast class. So the d1/d2 alignment
comparison is a DIRECT test of the +16 slow class vs 64 B-aligned at the
application level. Replica-aligned slots: exposed residual 0 (mod 64) by
construction.

cycles:u: DEMOTED (per prereg §6; negative consecutive per-op
double-differences — see validate session probe). instructions:u is the
quantitative instruction pair; per-cell medians identical across the
three modules at the same (chunk, depth) to <0.1%.

Noise windows (identified, not interpreted): d4 16K (natural/aligned
ratio 0.742) and d4 32K (1.221) are isolated single-cell swings without
neighboring-cell consistency; machine noise on this host (same class as
ALIGN-E0's 1M windows). The materiality rule (ratio ≥ 1.05 AND 1.5·MAD
robust separation) rejects them — M(c) = 0 at every chunk.

Derived-artifact note (immutable raw untouched): the driver's
`mibps_median` summary column was initially labeled MiB/s but computed
bytes/s; the unit was corrected in the driver and summary.csv/analysis.
json were re-derived from the SAME raw/runs.jsonl (driver fix committed
after the sweep). No raw value changed.

Verdict (prereg §12): MICROBENCH-ONLY — NOT APPLICATION MATERIAL.
