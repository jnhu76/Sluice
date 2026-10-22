# E1 Pending-Control Zero-Scan Productization — Evidence Index (2026-10-23)

Issue #568 (umbrella #563). Branch `perf/e1-pending-control-zero-scan`,
base `859e4b56`. Production change: derived `pending_control_count_`
summary + zero-count gate in `service_pending_controls_locked_()`;
router flags remain the semantic authority (BACKEND-04, TERM-01).

| Gate | Artifact |
|---|---|
| R0 baseline / scope | Issue #568 body (frozen scope, requirement IDs, gates) |
| R1 production diff | `e1-production.diff` (5 files: backend header, transport, seams, tests.lua, parity test) |
| R2 state-transition closure | `transition-closure.md` (write sites, invariants, proofs, oracle map) |
| R3 deterministic tests + mutation gate | `mutant-receipts.txt` (5 named kills, exit 1, binary hashes), `battery.txt` (release 18/18, debug 11/11, asanubsan 9/9, tsan 5/5, liburing=n build) |
| R4 P>0 reachability + cost | `reachability.md`; `native-p5-probe/` (real-kernel probe, 0 sticky observations); `seam-bound/` (Path B: verdict `P_POSITIVE_SEAM_BOUND = PASS`, medians 0.938–0.961 vs 1.05, P>0 proven 15/15 both arms) |
| R5 product A/B | `product-ab/` (runner, provenance, raw jsonl, verdict **PASS**; W-02 headline 0.698/0.620, W-03 0.799/0.609/0.920, kernel boundary identical) |
| R6 acceptance reducer | `e1_product_verdict.py` + `e1_product_verdict_selftest.py` (16/16 counterfactuals incl. the rank-vs-rep pairing discriminator; research-data replay reproduces the frozen research per-rep medians) |
| R7 ledger | `docs/roadmap/v2-conformance.md`: G6 row + dated section (mechanism justification, authority-contraction audit) |
| R8 independent reviews | `review-A-concurrency.md`, `review-B-performance.md` |

Standing scope limits (never silently closed):

- `P_POSITIVE_NATIVE = NOT_MEASURED` on kernel 7.2.9 / liburing 2.13 —
  sticky states were not reachable under protocol-legal real workloads
  attempted; the P>0 cost evidence class is the controlled seam bound only.
- The W-03 `no_lost_wake` oracle is bounded-liveness on the exercised
  interleavings; the deterministic suites beneath it are the load-bearing
  correctness evidence.
- Overall productization status: `PRODUCT_READY_WITH_NATIVE_P_POSITIVE_LIMIT`
  pending owner merge approval (PR stays open).
