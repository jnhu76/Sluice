# bufe0-amp-wsl2-1 — application amplifier session notes

16 runs (2 cells {1M x d1, 1M x d8} x 4 arms x R7/R14), all post-exit
src/dst sha256-verified, all in-bench same-work gates green
(bytes_copied == 512 MiB, write_ops == chunks, read_ops in bounds,
short_writes == 0). Tree clean at db072c9; binary sha256 in
environment.json. No concurrent host activity during this session.

Arms: engine-b0 = the REAL production engine (run_pipelined_copy_
with_backend — the same copy_task.cpp the CLI uses); replica-b0/b1/b3 =
verbatim research replica of the same copy task with slot storage as the
only variable (vector / uninitialized owned / page-aligned owned).
workers=1, sync=none (production CLI defaults).

## Replica fidelity

engine-b0 vs replica-b0: d1 486.4 vs 467.0 ms, d8 234.8 vs 226.9 ms —
engine slightly slower in both cells (it runs first per cell, coldest
cache); the 5-rep pre-session check showed overlapping dispersion
(283 vs 276 ms medians). The replica is a faithful instrument.

## Results (median per 512 MiB copy, R14 run)

| depth | arm | construct | engine | total |
| --- | --- | --- | --- | --- |
| 1 | engine-b0 | (inside) | 486.4 ms | 486.4 ms |
| 1 | replica-b0 (vector) | 0.1 ms | 466.9 ms | 467.0 ms |
| 1 | replica-b1 (uninit) | 0.0 ms | 472.0 ms | 472.0 ms |
| 1 | replica-b3 (aligned) | 0.0 ms | 260.3 ms | 260.4 ms |
| 8 | engine-b0 | (inside) | 234.8 ms | 234.8 ms |
| 8 | replica-b0 | 4.3 ms | 222.6 ms | 226.9 ms |
| 8 | replica-b1 | 0.1 ms | 257.3 ms | 257.3 ms |
| 8 | replica-b3 | 0.1 ms | 231.4 ms | 231.5 ms |

## Reading

- B1 (uninitialized construction) gives NO application-level benefit at
  either depth. Its only saving — slot construction (4.3 ms at d8 for
  vector eager-init, i.e. the whole B0-vs-B1 lifecycle delta) — is
  1.9% of one 512 MiB copy at d8 and 0.02% at d1, and the census says
  slots are constructed ONCE per copy and reused (Q1/Q2), so the saving
  does not grow with file size.
- B3 (page-aligned) is 1.8x faster end-to-end at depth 1 — the
  production CLI default. Arithmetic cross-check: the formal session's
  per-op alignment penalty (1M reads ~ +272 us/op unaligned, writes
  similar for the source side) x 512 reads + 512 writes ~= 207 ms,
  which matches the observed replica-b0 minus replica-b3 gap (207 ms).
- At depth 8 the alignment advantage vanishes on this host (227 vs
  231 ms, within dispersion). The d8 pipeline bounds the copy by
  something other than per-op copy cost here; recorded as an open
  observation, mechanism unattributed (out of BUF-E0 scope).

## Verdict relevance (prereg §22)

The microbench F01 story (cost shift, not material) SURVIVES the
amplifier. The alignment story (Outcome D) AMPLIFIES at the production
default depth. Per prereg §22, the amplifier governs the final verdict.
