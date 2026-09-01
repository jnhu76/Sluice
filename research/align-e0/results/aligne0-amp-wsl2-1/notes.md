# aligne0-amp-wsl2-1 — application amplifier (QUALIFIED_BUT_VIRTUALIZED)

Purpose: realistic 512 MiB copy, 1 MiB chunks, ThreadPoolBackend workers=1
(production engine semantics), depth {1,2,4,8,16}, arms {engine (production
fidelity reference), natural (base+16), best (64-aligned), 4096}. Same-work
fail-closed per rep; runner verified dst hash == src hash.

- 40 runs, 0 gate errors. High dispersion: WSL2 virtualized writeback makes
  copy spans noisy (MAD 15-83 ms on 225-460 ms medians) -> limitation.
- d1: natural 462 ms vs best(64) 328 ms (1.41x) vs 4096 366 ms (1.26x);
  engine 458 ms ~= natural (replica fidelity OK). The READ alignment
  benefit survives at d1 at application level.
- d2+: natural/best ~1.00-1.19x, natural/4096 ~1.11-1.20x, within
  dispersion -> no material effect beyond d1.
- Consistent with BUF-E0's "d1 material / d8 null" pattern at application
  level, while the microbench + threaded diagnostics show the per-op cost
  itself never disappears (overlap/ceiling, not vanishing cost).
