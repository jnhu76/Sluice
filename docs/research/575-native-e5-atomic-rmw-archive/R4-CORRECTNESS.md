# R4 — Correctness and Counterfactual Oracles

## Gate outcome

`PASS` for all arms. Both candidates preserve every audited semantic surface,
the internal test builds keep exact counting behavior, public `AsyncStats`
parity holds in both wiring modes, and each backend has a named mutant killed
by its intended counter assertion (receipts in `R4-MUTATION-RECEIPTS.md`).

## Deterministic suite (arm green)

`xmake test` (Release, real ring, E5 native): **all green** on BASE,
TP-OP-ONLY, TP-FULL, UR-OP-ONLY, UR-FULL — identical outcome on every arm
(78/78 at the time of the arm runs; 79/79 after the oracle registration was
propagated to the arms, re-verified by Review A). The suite includes the
seam-built suites (which re-enable the guarded counters) and the
production-linked suites (which link the patched production library).

### Oracle coverage mapping (required checklist → existing named tests)

| Required coverage | Evidence tests (all green on every arm) |
|---|---|
| **TP:** normal physical syscall execution | `threadpool_request_core_integration_test` (`syscall_count_for_test() == 1` at the "physical syscall has completed" assertion) |
| TP: multiple worker execution | same suite (`== 4`, "every syscall ran on a worker"); `threadpool_progress_race_test` |
| TP: cancellation before execution | `request_scope_test` (`syscall_count_for_test() == 0`, "cancel-won operation never ran") |
| TP: cancellation after execution claim | `request_scope_test` (`== 2` episode counting); `threadpool_progress_race_test` |
| TP: failed dispatch | `request_scope_test` / `public_request_test` (`== 0` "no worker ever ran the operation") |
| TP: zero-length request | integration suite (`== 0` "no data syscall is dispatched"; `+0` deltas) |
| TP: publication pending | `threadpool_request_core_integration_test` (publication visibility/epilogue gates) |
| TP: exactly-once terminal / RequestCore reclamation | `request_core_protocol_test`, `request_core_publication_test`, `request_core_ownership_test` (idle-and-whole reclamation checks) |
| TP: shutdown drain, shutdown/cancel races | `shutdown_lifecycle_test`, `threadpool_progress_race_test`, `external_loop_stop_order_test` |
| TP: borrow lifetime | `request_core_init_closure_test`, `request_core_protocol_test` |
| TP: W-03 external loop / PROG-02 | `threadpool_external_loop_test`, `threadpool_progress_pass_bound_test`, `progress_notification_suppression_test` |
| **UR:** successful transport submission | `uring_flush_count_oracle_test` (new; `== 1`/`== 2` exact positive counts) |
| UR: failed submission / retryable result | `uring_request_core_integration_test` (sequenced-submit hard-error and EINTR/EAGAIN paths) |
| UR: partial submission | same suite (`SLUICE_BACKEND_CONTRACT_MUTANT_*` environment, ledger/kernel reconciliation) |
| UR: zero-length request | `uring_flush_count_oracle_test` (`== 0`) and integration suite (`flushes unchanged`) |
| UR: running cancellation / prepared+submitted controls | `uring_request_core_integration_test`, `uring_pending_control_parity_test` |
| UR: poison and recovery | `uring_request_core_integration_test`, `shutdown_lifecycle_test` |
| UR: pending transport ledger | `uring_owed_residue_test`, `uring_owed_counter_parity_test`, ledger reconciliation tests |
| UR: CQE completion and publication | `uring_request_core_integration_test` (inject_cqe paths), `request_core_publication_test` |
| UR: shutdown/drain, no lost wakeup | `shutdown_lifecycle_test`, `uring_progress_race_test`, `uring_registration_lifecycle_test` |
| UR: W-03 real external progress / PROG-02 | `uring_external_loop_test` (epoll + eventfd + external progress owner) |
| Production-linked real ring | `semantic_backend_conformance_test`, `uring_backend_smoke_test`, `uring_public_consumer_probe` |

### Counter semantics preserved in the internal test builds

- TP: exact positive totals asserted in-tree (`== 1`, `== 2`, `== 4`,
  `+1`/`+0` deltas across 16 sites) — all green on every arm, so the seam
  builds execute the guarded increment identically to BASE.
- UR: the new oracle pins the positive semantics the pre-existing suite
  lacked (see mutation receipts for why that mattered): exactly one flush per
  accepted non-zero data submission (`== 1`), monotonic across sequential
  submissions (`== 2`), completion/polling add none, zero-length never
  flushes (`== 0`), dispatch-failure never flushes (existing integration
  assertion). Green on BASE and every arm. At closeout this oracle was
  productized on master via test-only PR #585
  (`1df8b4b82f9078dab824a4559684c40503d3e2cd`); the 78/78→79/79 counts
  above are the research-time records and are not rewritten.
- Release binary has no target RMW (R3 assembly oracle: 0 matching locked
  instructions per arm object and absent at linked-binary target sites) while
  the seam-built test binaries still exercise the counting — the two facts
  together are the fail-closed authenticity pair the issue demanded.

## Public stats parity (both wiring modes, all arms)

Product benchmark `bench_w02_pipeline` run per arm × `SLUICE_BENCH_STATS`
∈ {0,1} × {threadpool-4k-d16-w4, uring-4k-d16}, 20 000 requests each
(raw JSONL: `raw/parity/`):

- `ok=true`, `ops=20000`, empty failure on **all 20 runs**.
- Stats OFF: `stats_submit_calls=0`, `stats_completed_ops=0` (null pointer
  respected; no counter values exposed) — identical across arms.
- Stats ON: `stats_submit_calls=20000`, `stats_completed_ops=20000` —
  byte-identical values across all five arms and both backends.
- No public `AsyncStats` behavior was deleted or redefined: the opt-in
  accounting path is untouched by both patches (R1 audit + these runs).

## ODR / build isolation confirmation

Every seam target compiles its own backend TUs with
`SLUICE_ASYNC_INTERNAL_TESTING`; production binaries link the macro-free
library objects; no binary mixes the two layouts (R3). The 78/78 result on
each arm re-confirms that no link-time or runtime layout violation was
introduced by guarding the fields.
