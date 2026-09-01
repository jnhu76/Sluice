# ALIGN-E1 — small/medium-chunk application materiality (#268)

Execution authority: issue #268 (roadmap #259; ALIGN-E0 #265 CLOSED as
completed, evidence record merged as PR #266 at 7092554f).

ALIGN-E1 answers whether ALIGN-E0's native READ per-op alignment micro-cost
surfaces as MATERIAL in a realistic 4K–64K application-level READ + WRITE
copy workload, and where the chunk-size sweet spots / crossovers / regime
boundaries are. It is the first user of a reusable application-level
performance sweep shape (workload × module/path × chunk × depth) — a
single research campaign, not a generic framework.

> ALIGN-E1 performs NO production implementation. Regardless of verdict:
> no production alignment knob, no aligned storage abstraction, no
> registered buffers, no SIMD (frozen in the preregistration §12).

## Entry points

| Artifact | Purpose |
| --- | --- |
| `ALIGN-E1-PREREGISTRATION.md` | Frozen experiment design (matrices, modules, workload, metrics, same-work, materiality rule, sweet-spot definitions, verdict rules, stop gates; AMENDMENTs additive only) |
| `ALIGN-E1-REPORT.md` | Final report + verdict + regime map (after measurement) |
| `bench/align_e1_bench.cpp` | Sweep bench (engine / replica-natural / replica-aligned; wired in `xmake/benchmarks.lua` as `align_e1_bench`, `-g bench`) |
| `scripts/align_e1.py` | Session driver (generate / validate / sweep / summarize) |
| `scripts/plot_align_e1.py` | Plot generator (SVG, derived artifacts) |
| `results/<session-id>/` | Immutable measurement sessions (environment.json, manifest.json, gates.json, notes.md, summary.csv/json, raw/runs.jsonl + perf.csv) |
| `plots/` | Derived plots: throughput / instructions-per-byte / alignment-ratio per depth |

## Matrices (frozen)

- Chunks: 4K, 6K, 8K, 12K, 16K, 24K, 32K, 48K, 64K (+ 1 MiB historical
  reference, excluded from materiality/regime analysis).
- Depths: 1, 2, 4, 8 (pipeline depth, workers = 1).
- Modules: `engine` (production `run_pipelined_copy_with_backend`), 
  `replica-natural` (same algorithm, malloc geometry), `replica-aligned`
  (same algorithm, 64 B exposed alignment).
- Workload: 512 MiB READ + WRITE file copy, deterministic pseudo-random
  src, per-run dst hash == src hash (fail-closed), R = 7 seeded
  interleaved rounds, median + MAD per cell.

## Verdict (measurement complete — see ALIGN-E1-REPORT.md)

```
VERDICT:        MICROBENCH-ONLY — NOT APPLICATION MATERIAL
                (ALIGN-E0's native READ per-op alignment micro-cost does
                NOT surface as a material effect in the 4K–64K READ+WRITE
                application copy; M(c)=0 at every chunk × depth, incl.
                the direct +16-residue-class test at d1/d2)
PRODUCTION ALIGNMENT KNOB AUTHORIZED: NO
RUNTIME ADAPTATION AUTHORIZED: NO
REGISTERED BUFFER: NO
SIMD: NO
```

Evidence: `aligne1-sweep-native-1` — 840 runs, 0 gate errors, dst hash
== src hash every run (fail-closed). Production stop gates (prereg §14):
no alignment knob, no aligned storage abstraction, no production-oriented
kernel archaeology on this thread; #267 may continue as independent
methodology research.