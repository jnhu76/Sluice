# aligne0-threaded-native-1 — session notes

SECONDARY TOPOLOGY DIAGNOSTIC on native Linux (real in-flight depth =
worker count; each worker owns a buffer at the same exposed alignment,
barrier-synchronized steady state). Arms {a0, a7} x dirs {read,write} x
sizes {64K, 1M} x workers {2,4,8,16,32}. 80 runs, 0 gate errors, 40 rows.

## READ per-op cost under real overlap (thread_op_ns median, ns/op)

size=64K: a0 12.7-15.8 us vs a7 10.3-13.9 us -> a0 approx +12-24% slower
per-op at every worker count. The per-op READ alignment tax PERSISTS under
real overlap at 64K (WSL2 64K per-op gap was 4x-6.5x).

size=1M: a0 282-1063 us vs a7 266-982 us -> a0 approx +5-9% slower per-op
at w2..w32 (w16/w32 windows partially oversubscribed: 32 workers > 20
logical CPUs). Small but consistent.

Wall/op (amortized): noisier; READ 64K w8/w16/w32 ratios 1.11-1.15, 1M
1.06-1.11; w2/w4 windows at 64K are scheduling noise (0.79 / 1.67).

## WRITE

Flat at every worker count and size (wall ratios 0.97-1.04, thread_op
mixed) — no WRITE alignment effect under concurrency either.

## Interpretation

READ: overlap does NOT hide the per-op tax on native (same conclusion as
WSL2 — mechanism does not disappear with overlap — but at ~1/4 to ~1/5
the WSL2 magnitude). 1M per-op effect is detectable here (+5-9%) where the
sync ladder windows were too noisy to resolve it.