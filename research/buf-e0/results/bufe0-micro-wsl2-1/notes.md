# bufe0-micro-wsl2-1 — formal 4-arm microbench session notes

PRIMARY canonical session for BUF-E0 phases A/B/C/D (prereg as amended).
384 runs (12 cells x 4 arms x 4 phases x R7/R14), 0 skips, 0 same-work
gate errors, 0 pinned-regime violations (per-rep slot0_kind /
phase-C construct_kind all anon-mmap / own-mmap).

Tree state: clean at commit 39b7ca4 (bench binary sha256 in
environment.json). Data file: build/bufe0-data/src.bin (256 MiB,
splitmix64 master tiling, sha256 in environment.json).

## Metric availability notes

- cycles:u is UNRELIABLE on this host: 5/192 cells produced NEGATIVE
  R7/R14 double-differences (virtualized cycle counter non-monotonicity).
  instructions:u: 1/192 anomalous cell; otherwise reproducible. Primary
  quantitative pair for this campaign: in-process wall (steady_clock
  spans, median/MAD over 14 reps) + instructions:u double-difference.
- Fault metric is in-process getrusage ru_minflt per REGION (alloc / io /
  touch). perf software-event fault counting was dropped as unsound for
  this design: faults triggered inside kernel copy (pread) attribute to
  kernel context, which :u filtering miscounts (reason recorded here
  instead of an amendment: the prereg lists getrusage as primary and perf
  faults as optional/secondary).
- PMU cache/TLB events: not collected (not needed for the prereg causal
  questions; no TLB/cache stories are claimed).

## Headline physics (see summary.csv for full data)

- Phase B fault conservation: fault totals per cell are IDENTICAL across
  b0/b1/b3 (2/buffer at 4K, 17 at 64K, 257 at 1M; b2 has one fewer from
  the absent malloc chunk header) — only the LOCATION differs: b0 pays
  them in the alloc span (eager zero-init faults pages in), b1/b3 pay
  them in the first-I/O span. The prereg cost-shift question is directly
  instrumented.
- Phase B totals (total-to-first-useful-I/O, ns/buffer): 4K cells B0 vs
  B1 statistically indistinguishable (6.3-7.4us vs 6.5-7.7us across slot
  counts). 64K: 48-65us vs 50-60us — equal within dispersion. 1M: B1
  9-11% lower than B0; B2/B3 ~25-40% lower.
- Phase D: b0 first-touch ~5-30 ns/page (pages already resident from
  construct-time init) vs 1.4-2.2 us/page for b1/b2/b3 (real demand
  faults) — the host's demand-fault cost, paid at construct (b0) or at
  first use (others).
- Phase C steady state: B0 == B1 everywhere (representation without
  alignment does not matter), but B2/B3 (page-aligned) are 2.5-5x faster
  per op across ALL sizes and slot counts → Outcome-D pattern, mechanism
  attributed in session bufe0-align-wsl2-1 (AMENDMENT 1): page alignment
  of the I/O buffer, not mmap-vs-malloc.
- Phase A (context, not a headline): b0 construction cost is dominated by
  eager zero-init touching fresh pages (1M: 434-514 us/buffer vs 4-5 us
  b1); 4K construction differences are small in absolute terms.

## Harness mechanism facts recorded (pinned regime)

- glibc serves >=threshold requests from the brk top chunk BEFORE the
  sysmalloc mmap path is consulted; a fresh arena's top silently serves
  4K constructions from brk. Neutralized by the eater chain + reservoir
  (bench source comments); verified per-rep by the regime gates.
- Malloc'd fresh allocations pay 1 minor fault per buffer for the chunk
  metadata page write (b0 4K = 2 faults: metadata + memset spill into the
  second page of the 8192-byte chunk mapping; b2 own-mmap = 0).

## Environment classification

QUALIFIED_BUT_VIRTUALIZED (WSL2, kernel 6.18.33.2-microsoft, ext4 on
virtual block device, warm page cache). All absolute numbers
ENVIRONMENT-LIMITED; valid for allocation / initialization /
fault-shifting attribution and same-host causal comparison only.
