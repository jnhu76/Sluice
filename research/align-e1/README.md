# ALIGN-E1 — small/medium-chunk application materiality (#268)

Execution authority: issue #268 (roadmap #259; ALIGN-E0 #265 CLOSED as
completed, evidence record merged as PR #266 at 7092554f).

ALIGN-E1 answers whether ALIGN-E0's native READ per-op alignment
micro-cost (directly tested at the +16 exposed/page-offset point)
surfaces as MATERIAL in a realistic 4K–64K application-level READ +
WRITE copy workload, and where the chunk-size sweet spots / crossovers
/ regime boundaries are. It is the first user of a reusable
application-level performance sweep shape (workload × module/path ×
chunk × depth) — a single research campaign, not a generic framework.

> ALIGN-E1 performs NO production implementation. Regardless of verdict:
> no production alignment knob, no aligned storage abstraction, no
> registered buffers, no SIMD (frozen in the preregistration §12).

## Entry points

| Artifact | Purpose |
| --- | --- |
| `ALIGN-E1-PREREGISTRATION.md` | Frozen experiment design (matrices, modules, workload, metrics, same-work, materiality rule, sweet-spot definitions, verdict rules, stop gates; additive AMENDMENTs only — AMENDMENT 1: 128 MiB per run; AMENDMENT 2: E1-C1 strict causal-isolation control) |
| `ALIGN-E1-REPORT.md` | Final report — PART A broad sweep / PART B adversarial-review findings / PART C E1-C1 causal control / PART D synthesis |
| `bench/align_e1_bench.cpp` | Sweep bench (engine / replica-natural / replica-aligned + causal-phase16 / causal-aligned64; wired in `xmake/benchmarks.lua` as `align_e1_bench`, `-g bench`) |
| `scripts/align_e1.py` | Session driver (generate / validate / sweep / causal / summarize / summarize-causal) |
| `scripts/plot_align_e1.py` | Plot generator (SVG, derived artifacts; `--causal` for the causal-ratio plots) |
| `results/<session-id>/` | Immutable measurement sessions (environment.json, manifest.json, gates.json, notes.md, summary.csv/json, analysis.json, raw/runs.jsonl + perf.csv) |
| `plots/` | Derived plots: throughput / instructions-per-byte / alignment-ratio per depth; causal-ratio d1/d2 |

## Matrices (frozen)

- Chunks: 4K, 6K, 8K, 12K, 16K, 24K, 32K, 48K, 64K (+ 1 MiB historical
  reference, excluded from materiality/regime analysis).
- Depths: 1, 2, 4, 8 (pipeline depth, workers = 1).
- Modules: `engine` (production `run_pipelined_copy_with_backend`),
  `replica-natural` (same algorithm, malloc geometry), `replica-aligned`
  (same algorithm, 64 B exposed alignment). E1-C1 adds `causal-phase16`
  and `causal-aligned64` — same `posix_memalign(4096, chunk+64)`
  allocation primitive, allocation size and backing-alignment policy in
  both arms, ONLY the exposed-pointer phase treatment differing
  (+16 vs 0).
- Workload: 128 MiB READ + WRITE file copy (AMENDMENT 1), deterministic
  repeated pseudo-random 4 KiB pattern, per-run dst hash == src hash
  (fail-closed), R = 7 seeded interleaved rounds, median + MAD per cell.

## Verdict (measurement complete — see ALIGN-E1-REPORT.md)

```
VERDICT:        MICROBENCH-ONLY — NOT APPLICATION MATERIAL

E1-C1 STRICT CAUSAL CONTROL (AMENDMENT 2):
  PASS — with the same allocation primitive, allocation size,
  backing-alignment policy, ownership/lifetime policy, algorithm,
  workload (bytes, op counts, chunk, depth, worker topology) in both
  arms, changing ONLY the exposed-pointer phase treatment from exact
  page-offset +16 to page/cache-line-aligned produced NO material
  application-copy benefit in ANY tested 4K–64K cell at depth {1,2}
  (252 runs, 0 gate errors, frozen prereg §8 materiality rule:
  0 / 18 cells material).

The earlier sweep's natural slots sat (recorded) at residual 16 mod 32 —
CONSISTENT WITH the ALIGN-E0 candidate geometry; ALIGN-E0 itself proved
only the specifically tested +16 point, not a whole residue class.

PRODUCTION ALIGNMENT KNOB AUTHORIZED: NO
RUNTIME ADAPTATION AUTHORIZED: NO
REGISTERED BUFFER: NO
SIMD: NO
ALIGNMENT IS NOT PROMOTED INTO THE Sluice CONTROL SURFACE.
```

Evidence: `aligne1-sweep-native-1` (broad application sweep — 840 runs,
0 gate errors, dst hash == src hash every run) + `aligne1-causal-native-1`
(strict causal confirmation — 252 runs, 0 gate errors, per-run address
gates FAIL CLOSED). Production stop gates (prereg §14): no alignment
knob, no aligned storage abstraction, no production-oriented kernel
archaeology on this thread; #267 may continue as independent methodology
research.

Separate retained finding (claim-bound scoped: current host, current
buffered copy engine, workers=1, tested range): chunk_size is a
materially stronger performance lever than alignment. GLOBAL SWEET
SPOT NOT LOCATED; TESTED-RANGE PEAK is the 1 MiB boundary point (64K →
1 MiB still rising; plateau not reached). A sweet-spot search beyond
1 MiB requires a separate issue — not in #268.
