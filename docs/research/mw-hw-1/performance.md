# MW-HW-1 measurements (WSL2, research only)

Design: M2 harness (`pipeline_ab.cpp`), 4 arms (base = master `8e2aa5ab`;
e1/e2/e3 = single-mechanism worktrees), 6 W-02 configurations, 9 paired
repetitions, arm order rotated per repetition, `taskset -c 2`. Every run's
oracle phase passed (0 failures, 216/216 timed rows ok, 0 timed allocations).
Raw rows: `ab/rows.jsonl` in the raw output directory (see README).

## 1. Paired wall/CPU ratios (timed phase, ratio = arm/base per repetition)

| Config (block/D/C) | e1 med [min,max] | e2 med | e3 med | base wall µs/op |
|---|---|---|---|---:|
| 4 KiB / 1 / 1024 | **0.851 [0.811, 0.940]** | 0.994 | 0.986 | 11.11 |
| 4 KiB / 1 / 16 | 0.984 [0.908, 1.046] | 1.007 | 1.001 | 9.01 |
| 4 KiB / 8 / 1024 | **0.738 [0.713, 0.993]** | 1.043 | 1.010 | 5.03 |
| 4 KiB / 64 / 64 | 0.955 [0.909, 1.075] | 1.006 | 0.974 | 2.96 |
| 64 KiB / 8 / 1024 | **0.919 [0.902, 0.928]** | 1.009 | 0.995 | 19.35 |
| 1 MiB / 8 / 64 | 0.957 [0.918, 1.234] | 1.130 | 0.980 | 196.76 |

CPU-time ratios are within 0.005 of wall ratios in every cell (single-thread
steady state).

Reading:

- **E1's gain carries the exact C-scaling signature of the router scans**:
  ≈0 at C=16 (scan of 16 entries), −15% at C=1024/D=1, −26% at C=1024/D=8
  (fixed scan cost over a smaller kernel-amortized base), −8% at 64 KiB,
  noise-level at C=64 or 1 MiB (I/O-dominated / small router).
- **E2 and E3 are falsifiers**: both sit at 1.00 within noise at 4 KiB —
  confirming the analytical prediction that dispatch mediation and
  publication lock pairs are negligible per-op costs, not hidden hotspots.
  The E2 1MiB cell (1.13) and E3 tails are single-config noise at a
  memory-bandwidth-bound point (ranges overlap 1.0); no mechanism in either
  patch touches the I/O path.
- E1 absolute saving: 1.79 µs/op (4k/D1/C1024), 1.36 µs/op (4k/D8/C1024) —
  matching the C-sweep delta (+1.3 µs/op from C=16→1024) and the perf
  attribution (41% of user cycles; user time 0.22 s → 0.07–0.10 s per 80k
  ops, −61%, system unchanged).

## 2. Capacity sweep isolating the C-dependent component (base arm, D=1, 4 KiB)

| C | wall µs/op (median of 3) |
|---:|---:|
| 16 | 8.40 |
| 64 | 8.64 |
| 256 | 8.76 |
| 1024 | 9.72 |

Δ(C16→C1024) ≈ +1.3 µs/op ≈ 2·(1024−16)·24 B of router walk — the component
E1 deletes (measured 1.4–1.8 µs/op including loop/branch overhead).

## 3. Correctness gates (all arms)

Deterministic suites run per arm before timing (9 targets):
`uring_backend_smoke_test`, `uring_progress_race_test`,
`uring_external_loop_test` (W-03 external loop), `uring_owed_residue_test`,
`uring_owed_counter_parity_test`, `uring_registration_lifecycle_test`,
`uring_request_core_integration_test`, `backend_contract_conformance_uring_test`
(includes the sticky-intent scenario),
`request_core_ownership_uring_test`. All PASS on base, e1, e2, e3.

E1 additionally carries `uring_pending_control_parity_test` (4 scenarios:
zero-pending steady state; SQE-pressure cancel sets and keeps P=1 across
passes; P serviced when SQE frees — cancel issued, count returns to 0, both
requests settle through control + original CQEs; pending retired with route)
with parity `pending_control_count_ == |{i : router_[i].control_pending ∧
in_use}|` asserted at every checkpoint.

Negative controls (required CAUGHT):

| Mutant | Killing oracle | Result |
|---|---|---|
| `uring_pending_control_mut_missing_decrement` | parity test scenario 2 | CAUGHT (exit 1, `FAIL: sqe_pressure_cancel_sets_and_keeps_pending`) |
| `uring_pending_control_mut_missing_increment` | parity test scenario 2 | CAUGHT (exit 134; count 0 vs authoritative scan 1, then nonterminal-responsibility fail-fast on early return) |

## 4. Limits

- WSL2 only; absolute µs/op are host-specific; ratios are the claim.
- The missing-increment mutant's exit is an abort (fail-fast) rather than a
  clean FAIL print — loud, but the receipt records the masking.
- W-02 only. W-03 external-loop A/B was NOT run (same gap as M2); W-03
  correctness is covered by `uring_external_loop_test` passing on e1.
- Zero-op (K>0) has no A/B row (harness rejects block=0); E1 does not touch
  the owed path (verified by the owed suites) and the zero-op census shows
  the pipeline is already syscall-free.
- No tail-latency claims (WSL2 multi-ms outliers, per M1 precedent).
