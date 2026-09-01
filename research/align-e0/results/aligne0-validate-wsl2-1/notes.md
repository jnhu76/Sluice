# aligne0-validate-wsl2-1 — HARNESS VALIDATION (WSL2 development-only subset)

Purpose: prove the ALIGN-E0 harness end-to-end (same-work gates, JSON,
R7/R14 perf wiring, READ/WRITE/threaded/offset paths) before the frozen
matrix. NOT a verdict session.

- 128 runs = 8 arms (a0..a7) x dirs {read,write} x sizes {4K,1M} x depths
  {1,8} x R7/R14. 0 gate errors.
- READ validation signal (QUALIFIED_BUT_VIRTUALIZED): a0 (16-aligned, page
  offset 16) ~2.2x (4K) to ~3.8x (1M) slower than every a1..a7; 64..4096
  all equivalent; instructions/op IDENTICAL across arms (~2399 read / ~492
  write) -> copy-path latency, not instruction count.
- WRITE: no material alignment pattern in sync microbench.
- metric availability: cycles:u unreliable on WSL2 (BUF-E0 finding);
  quantitative pair = in-process wall + instructions:u.
