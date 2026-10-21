# M2 native-Linux validation: entry point

This directory contains the M2 investigation of two mechanisms on the native E5
host. It is research evidence, not a product change.

**Outcome**

| Mechanism | Decision |
|---|---|
| H1 io_uring router direct index | **MICROBENCH_ONLY** — lookup 0.04-0.82× in isolation, ≤3.7% end-to-end; requires product-domain narrowing |
| H2a single-lock owed snapshot | **MECHANISM_IMPROVEMENT_CONFIRMED**, not selected — H2b is better on the decisive 4 KiB/64 KiB configs, H2a is better at 1 MiB/C=64/d=1 |
| H2b derived owed count | **MECHANISM_IMPROVEMENT_CONFIRMED** — 0.478-0.988 on the decisive 4 KiB/64 KiB configs; neutral at 1 MiB. W-02 only: `W03_AB = NOT_RUN` |
| H2a+H2b | not selected — usually 1.01-1.04× H2b and sign-flips at one config |

All Layer C results are W-02-scoped (`submit_read` → `wait_for` → `take`);
the W-03 external-event-loop path was not driven and remains a
product-admission gate (`h2-results.md` §6).

The H2b gain comes from removing a per-pass `O(C)` sweep with `C` mutex
lock/unlock pairs when no owed event exists. The remaining time is real
submission/control work plus kernel time; further optimization would need a
different target.

## Documents

| File | Contents |
|---|---|
| `environment.md` | E5 admission, bare-metal evidence, permissions, isolation, background load |
| `protocol.md` | M0/M1 dependencies, corrected H2 complexity, frozen inputs, correctness gate, decision rule |
| `h1-identity-matrix.md` | Complete identity dataflow across SQE, ledger, cancel64, CQE, recovery, reuse, exhaustion |
| `h1-results.md` | H1 Layer A, A/B, attribution, decision |
| `h2-results.md` | H2 baseline semantics, Layer A, A/B, attribution, BACKEND-04, minimal proposal |
| `cpu-attribution.md` | perf method, counters, symbols, source mapping, limits |
| `end-to-end-ab.md` | Campaign design, validation, statistics, artifacts, reproduction |
| `review.md` | Independent Review A (concurrency/protocol) and Review B (benchmark/attribution) |
| `results/` | Frozen validation tables, summaries, arm identity, test logs, Layer A decisions, perf summaries |
| `patches/` | Non-product candidate patches, hashes and apply instructions |

## Evidence layers

1. **Layer A** — mechanism microbenchmark on E5, 15 reps, oracle and negative
   controls (`results/layer-a/`).
2. **Layer B** — production-path perf: unmultiplexed `perf stat` and
   user-space `perf record` (`results/perf/`, `cpu-attribution.md`).
3. **Layer C** — real production A/B through `RequestScope`, 20 configs × 5 arms
   × 15 reps plus a 7-config idle replication (`results/ab-*`).

Both campaigns: zero oracle failures, zero timed allocations, zero validation
problems. All five arms pass their test suites; the H2 no-decrement mutant is
killed and all seven H1 identity controls pass.

## Reproduction

```bash
bash bench/native-linux-m2/run.sh <worktree-root> <output-dir> [reps] [cpu]
```

`<worktree-root>` must contain `base/ h1/ h2a/ h2b/ h2ab/`, each at the frozen
baseline with its patch applied and built in release mode. See
`end-to-end-ab.md` for the exact setup. `run.sh` compiles the harness against
each arm's static library, runs the matrix, and invokes
`validate-results.sh`; a failing oracle or timed allocation fails the campaign.

## Status

- Source baseline: `8fdcd99c5f9a42dadfe5aa0c095bf96c756aeb01`.
- M0/M1 research input: #560, closed out at `2e998fba` (H1 identity rule and
  H2 complexity corrections) and merged; this branch was opened stacked on it.
- Product `src/`/`include/`: unchanged. No CI changes.
- No product-fix PR is created or merged by M2. H2b productization — with the
  W-03 admission and the submit-reset fail-fast obligation in
  `h2-results.md` §6 — is a separate owner decision.
