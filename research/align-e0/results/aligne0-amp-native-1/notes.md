# aligne0-amp-native-1 — session notes

Application amplifier on native Linux (frozen prereg §9): realistic copy
pipeline, 512 MiB src -> dst, 1 MiB chunks, arms {engine, natural, best
(64), 4096}, depth {1,2,4,8,16}, R7/R14 pairs; best-align=64 (same choice
as the WSL2 campaign; 64 is the minimum ladder-tested effective
alignment on native because a1 is already in the fast group).
40 runs, 0 gate errors, 20 rows; runner-verified dst hash == src hash
(same-work green). `engine` = real production copy engine
(apps/sluice-copy/copy_task.cpp) fidelity reference.

## Full-engine span (engine_ns median, ms; MAD in ms)

```
depth   engine    natural   best(64)   4096
  1     869.0     881.2     858.4     860.3
  2     667.5     638.5     637.0     636.0
  4     639.2     642.1     637.4     668.6
  8     686.8     698.3     665.4     685.3
 16     715.0     714.5     700.8     720.7
```

- d1: natural/best = 1.027x, natural/4096 = 1.024x — NO material
  alignment benefit at application level (WSL2 d1 was natural/best =
  1.41x). d2-d16: 0.96-1.05x, within MAD.
- engine (production natural storage) tracks the natural replica (869 vs
  881 ms at d1) — replica fidelity holds.
- The per-op READ microbench tax (+14..42% at 4K-64K) does NOT surface as
  an application-level benefit in the 1 MiB-chunk copy pipeline on native:
  the copy pipeline overlaps/absorbs it (application-level boundary per
  prereg §9 = amplifier, verdict follows the amplifier).

## PMU

instructions:u stable; per-engine-call counts arm-invariant (not shown in
summary.csv columns but present in raw perf text). cycles:u unreliable as
usual on this host.

## Interpretation

On native, the alignment benefit does NOT survive the application
amplifier at any depth. This is the key difference from the WSL2-qualified
campaign (where d1 showed natural/best = 1.41x). Combined with the
microbench +14-42% READ tax, the Phase-3 classification is MIXED: per-op
READ effect reproduced directionally; application-level benefit NOT
reproduced.