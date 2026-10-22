# MW-HW-1 measurements (WSL2, research only)

Design: M2 harness (`pipeline_ab.cpp`), 4 arms (base = master `8e2aa5ab`;
e1/e2/e3 = single-mechanism worktrees), 6 W-02 configurations, 9 paired
repetitions, arm order rotated per repetition, `taskset -c 2`. Every run's
oracle phase passed (0 failures, 216/216 timed rows ok, 0 timed allocations).
Raw rows: `ab/rows.jsonl` in the raw output directory (see README).

## 1. Paired wall/CPU ratios (timed phase, ratio = arm/base per repetition)

| Config (block/D/C) | e1 med [min,max] reps<1.0 | e2 med | e3 med | base wall µs/op |
|---|---|---|---|---:|
| 4 KiB / 1 / 1024 | **0.858 [0.763, 0.940] 9/9** | 1.005 | 0.992 | 11.11 |
| 4 KiB / 1 / 16 | 0.999 [0.825, 1.046] 6/9 | 1.007 | 0.995 | 9.01 |
| 4 KiB / 8 / 1024 | **0.731 [0.708, 0.993] 9/9** | 1.018 | 0.997 | 5.03 |
| 4 KiB / 64 / 64 | 0.966 [0.785, 1.082] 7/9 | 1.012 | 0.976 | 2.96 |
| 64 KiB / 8 / 1024 | **0.917 [0.847, 1.017] 8/9** | 1.018 | 0.995 | 19.35 |
| 1 MiB / 8 / 64 | 0.975 [0.842, 1.286] 5/9 | 0.996 | 1.019 | 196.76 |

Ratios are per-repetition pairs (rep-index pairing reconstructed from run
order; the campaign's arm rotation makes each rep an independent paired
block — per review B, an earlier rank-paired reduction is retained only as
a cross-check and agrees on all medians). `reps<1.0` is the sign-test count;
exact two-sided Wilcoxon on log-ratios gives p = 0.0039 (4k/D1/C1024),
0.0039 (4k/D8/C1024), 0.0078 (64k/D8/C1024), and p ≥ 0.43 at C=16, C=64
and 1 MiB (no effect). Bootstrap 95% CI for the decisive 4k/D1/C1024 median:
[0.763, 0.910]. The 64k/D8 rep range includes one rep above 1.0 (1.017), so
no "whole-interval-below-1.0" claim is made anywhere. CPU-time ratios track
wall ratios within 0.007 in every cell (single-thread steady state).

Reading:

- **E1's gain carries the exact C-scaling signature of the router scans**:
  null at C=16 (scan of 16 entries), −14% at C=1024/D=1 (9/9), −27% at
  C=1024/D=8 (9/9; fixed scan cost over a smaller kernel-amortized base),
  −8% at 64 KiB/D8 (8/9), borderline at C=64 (7/9) and null at 1 MiB (5/9,
  I/O-dominated).
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

| C | wall µs/op (mean of 3; medians 8.365/8.536/8.761/9.593) |
|---:|---:|
| 16 | 8.40 |
| 64 | 8.64 |
| 256 | 8.76 |
| 1024 | 9.72 |

Δ(C16→C1024) ≈ +1.3 µs/op (means; medians 1.2) — the C-dependent component
E1 deletes, ~2·(1024−16)·24 B of router walk at ~0.5 ns/entry-scan. This is
an independent order-of-magnitude estimate at D=1 only (see §1 for the
per-config scans/op reconciliation and the D=8 gap).

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
| `uring_pending_control_mut_missing_increment` | parity test scenario 2 | CAUGHT (exit 134; the parity mismatch fires first, and the early return's fixture teardown hits the nonterminal-responsibility fail-fast before the FAIL line prints — per review A finding 9, the recorded killer is the fail-fast, with the count-vs-scan divergence as the underlying cause) |

## 4. Limits

- WSL2 only; absolute µs/op are host-specific; ratios are the claim.
- The missing-increment mutant's exit is an abort (fail-fast) rather than a
  clean FAIL print — loud, but the receipt records the masking.
- perf-record profiles are n=1 per arm; the E1 profile is majority
  kernel-leakage samples (53.5%) under cycles:u on WSL2, so only
  user-only-renormalized shares and the vanish/no-vanish contrast are used.
- E2/E3 medians sit at 1.00 within a ≥5–10% per-rep noise floor; a real
  effect of that size (including E2's 1MiB median 1.16 from 500-op runs)
  cannot be excluded — the decision is "no detectable win", not "no effect".
- W-02 only. W-03 external-loop A/B was NOT run (same gap as M2); W-03
  correctness is covered by `uring_external_loop_test` passing on e1.
- Zero-op (K>0) has no A/B row (harness rejects block=0); E1 does not touch
  the owed path (verified by the owed suites) and the zero-op census shows
  the pipeline is already syscall-free.
- No tail-latency claims (WSL2 multi-ms outliers, per M1 precedent).
