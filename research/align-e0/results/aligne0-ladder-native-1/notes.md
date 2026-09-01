# aligne0-ladder-native-1 — session notes

Frozen 8-arm alignment ladder on native Linux, 960 runs, 0 gate errors,
480 summary rows. Same-work gates green throughout; instructions/op
arm-invariant in every cell (e.g. 2382 @4K, 4277 @8K, 30779 @64K,
485102 @1M) — the wall differences below are NOT instruction-count
driven (matches WSL2 finding).

`git.dirty` in environment.json = true: at session creation the tree held
the untracked notes.md of the closed aligne0-validate-native-1 session;
source and binaries were at head 7d7046fc.

## READ (the primary direction)

a0 (16-aligned, page offset 16) is consistently slower than the aligned
arms a1..a7 (page offset 0), but the magnitude is size-dependent and far
below WSL2:

| size | a0 wall/op | median(a1..a7) | ratio | WSL2 ratio (d1) |
| --- | --- | --- | --- | --- |
| 4K   | ~1420-1433 ns | ~1245-1252 ns | 1.14-1.15 | 2.2x |
| 8K   | ~2183-2303 ns | ~1624-1647 ns | 1.33-1.42 | (not in WSL2 d1 table; 4K/1M only) |
| 16K  | ~3589-3640 ns | ~2932-2976 ns | 1.21-1.24 | — |
| 64K  | ~11566-12006 ns | ~9400-9534 ns | 1.22-1.27 | — |
| 1M   | ~160-191 us | ~153-181 us | 0.95-1.12 (noise) | 3.8x |

- Effect is flat across depth 1..32 at 4K/8K (sync batching depth): per-op
  penalty is a per-syscall latency effect, not a queue-depth interaction.
- d8 windows at 16K/64K/1M are machine-noise windows (MAD up to 5 us,
  a3/a5 outliers), not a systematic depth crossover: neighbors d4/d16
  return to the 1.2x pattern. Machine (KDE desktop, fresh boot) adds
  window-level noise, worst at 1M.
- 8K is the strongest cell (ratio 1.33-1.42) — NOT the WSL2 shape
  (WSL2 effect grew with size to 3.8x at 1M).
- Within the "fast" group a1/a6 wobble (e.g. 8K d1: a1 1691 ns, a6
  1699 ns vs a2 1292 ns / a3,a5,a7 ~1620 ns) — no clean threshold
  between 64 and 4096 detectable beyond "a1..a7 all far faster than a0".
- Minimum tested effective alignment: 64 B (a1 is already in the fast
  group on native, same as WSL2's amplifier choice).

## WRITE

Flat: a0/median-aligned ratio 0.83-1.04 across the whole matrix with no
consistent pattern (extremes are noise windows, e.g. 4K d2 0.81 and
64K d32 0.83 where an aligned arm's window was slow). Matches WSL2
("no consistent material WRITE effect").

## PMU

- instructions:u — stable, arm-invariant (see above); double-difference
  attribution clean.
- cycles:u — still unreliable as a per-op double-difference on this host
  (negative values recur at small windows; frequency/turbo differs between
  the R7 and R14 process runs). Demoted; quantitative pair =
  instructions:u + in-process wall (same conclusion as WSL2, different
  cause: not a virtualized counter).

## Headline

NATIVE READ: a0-vs-aligned penalty REPRODUCED directionally (+14%..+42%,
size-dependent, peak at 8K) but 5-15x smaller than WSL2's 2.2x-6.7x and
with a different size profile (native peak at 8K; WSL2 monotonic in
size). WRITE: flat on both. Full adjudication via offset/threaded/amp
sessions + final report.