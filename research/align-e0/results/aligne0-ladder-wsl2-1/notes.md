# aligne0-ladder-wsl2-1 — full frozen alignment ladder (QUALIFIED_BUT_VIRTUALIZED)

Purpose: frozen matrix on WSL2 (HARNESS VALIDATION AT FULL SCALE + qualified
evidence; native Linux is the mandatory environment for the Phase-3 verdict).

- 960 runs = 8 arms x 5 sizes {4K,8K,16K,64K,1M} x 6 depths {1,2,4,8,16,32}
  x 2 dirs x R7/R14. 0 gate errors.
- READ: a0 2.2x-6.7x slower than a1..a7 at every size x depth; a1..a7
  (64..4096) equivalent. instructions/op identical across arms and ~flat
  across depths. Effect present at ALL depths (sync mode has no overlap;
  per-op uaccess cost persists).
- WRITE: no material alignment effect (ratios ~1.0-1.4x within huge
  dispersion; write 1M MADs up to ~100-200 us on ~50 us medians -> WSL2
  virtualized writeback noise; limitation).
- d1/d8 at sync-microbench level: per-op cost does NOT disappear with batch
  depth; the BUF-E0 d8-null is an application-pipeline overlap/ceiling
  phenomenon (see threaded + amp sessions), NOT a vanishing uaccess cost.
