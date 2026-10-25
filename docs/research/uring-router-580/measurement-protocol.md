# Measurement protocol (as executed)

Consolidated from the runners in `bench/research/uring-router-580/` so the
campaign is reproducible without reading four scripts.

## Counting (R2 trigger discovery)

- Instrumented build: async sources compiled with
  `SLUICE_HAS_LIBURING SLUICE_ASYNC_INTERNAL_TESTING
  SLUICE_URING_LOOKUP_INSTRUMENT` (`lookup_probe` binary). Counters mutate only
  under `dispatch_mtx_`; the harness reads them after the context is drained.
- The instrumented binary is **never** used for a timing claim.
- Scenarios (`--scenario`): pipeline, churn (64↔1 depth alternation), cancel
  (public cancel on a fraction of tickets), metadata (file_info), miss-inject
  (SEAM: replay retired cookies; output tagged `seam-inject`).
- Every scenario verifies the byte-pattern oracle per read; failed oracles are
  recorded, not retried silently.
- Matrix (2 reps each, both kept): C16/D16, C64/D16, C1024/{D1,D16,D64},
  payloads 4 KiB/64 KiB/1 MiB, churn, cancel r=0.2 at C64/C1024, metadata,
  miss-inject at C16/C1024.

## End-to-end timing (R6)

- Uninstrumented arms, arm fixed at compile time (`SLUICE_580_ARM`, mechanism
  gate `SLUICE_580_WE2_REVERSE_CANCEL_SCAN`), built by `build_arms.sh`; binary
  SHA-256s recorded per host in `environment.md` / `results/manifest.json`.
- Per timed run the runner reports wall/user/sys (`getrusage` + monotonic
  clock) plus completed ops and verified bytes; any oracle failure marks the
  row `ok:false` and it is excluded from ratios but kept in the file.
- 15 repetitions per shape, AB/BA order alternating by repetition index.
- A/A control: byte-identical baseline copies under two paths
  (`router_ab_aa_alpha` / `_beta`), same 15-rep AB/BA schedule — bounds
  path-name and slot-order bias.
- Pinning `taskset -c 2`; fixtures per-run unique paths under `/tmp` (tmpfs on
  both hosts); fixed warmup per shape (600 cancel / 2000 pipeline).
- Reducer `reduce_ab.py`: pairs by run tag, normalizes arm order, computes
  paired ratios, median + percentile-bootstrap 95% interval (B=10000, seed 7)
  over the 15 pairs. Small-sample scope disclosed; raw JSONL never modified.

## Attribution

- WSL2: `perf record -e cycles:u -F 1999` (+ `perf stat` totals); perf printed
  raw offsets; symbols resolved offline through `nm` (script embedded in the
  session record; the resolution inputs are the archived binaries). 16.2% of
  samples landed at hypervisor/kernel addresses under `:u` and are excluded
  and disclosed.
- E5: `perf record -e cycles:u -F 1999 -m 64` (`-m 64` is a host requirement,
  known since M2); symbols resolved natively; shares reported from the
  no-children report.
- Bottom-up cross-check: probes/op (instrumented) × 1-2 cycles/probe vs the
  measured totals. R9 audit note: the attributed cost computed directly from
  share × cycles/op ÷ probes is ~2-5 cycles/probe warm (1.2-5× the bottom-up
  figure, sampling noise ~±40%); 1-2 cycles/probe is an L1-resident lower
  bound, and R9 (`cache-results.md`) replaces it with measured warm and
  cold-bound values.

## Correctness (R5)

- `identity_oracle.cpp`: bounded waits everywhere; assertion text flushed
  immediately; take_result guarded by readiness so a mutant's RED is always a
  named assertion, never a downstream contract abort.
- Variants built from one source with gates: `we2`, `SLUICE_580_MUTANT_FALSE_MISS`
  (M7), `SLUICE_580_MUTANT_SLOT_ONLY` (M3/M6). Logs archived per variant.
- Production suites additionally run on both arms on both hosts:
  `backend_contract_conformance_uring` (20), `uring_request_core_integration`
  (21), `uring_backend_smoke` (8).

## Codegen provenance

- Arm disassembly archived per host toolchain
  (`results/disasm_find_live_router_index_*.txt`, `results/e5/…`).
- Patch-to-production identity: `uring_transport.cpp` and
  `uring_completion.cpp` compiled with **no** research defines produce
  byte-identical objects to pristine master (SHA-256s in `manifest.json`,
  proven on E5 gcc 16.2.1; the third patched file is the header whose gated
  block vanishes at preprocessing).

## Host-switch record

WSL2 was the primary host through R2-R5 per the original protocol; the owner
authorized the E5 server mid-R6 (recorded in `environment.md` addendum). All
timing conclusions in `decision.md` rest on the E5 campaign; WSL rows are
retained as mechanism/counts replication, not as timing evidence.
