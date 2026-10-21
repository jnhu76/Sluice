# H1 identity dataflow matrix (io_uring router)

Baseline: `8fdcd99c5f9a42dadfe5aa0c095bf96c756aeb01`.
Candidate: `docs/research/native-linux-m2/patches/h1.patch` applied to that baseline.

The router identity is the value the kernel echoes back in a CQE `user_data`.
Every production consumer below must agree on its format; H1 changes the format
from a flat 63-bit cookie to `[control tag | router slot 15 | cookie 48]` and
preserves the full-identity re-check at every consumer.

## Matrix

| # | Location | Stored identity (baseline) | Producer | Consumer | Verification rule (baseline) | H1 candidate | Semantics preserved |
|---|---|---|---|---|---|---|---|
| 1 | Operation SQE `user_data` | `op_cookie` (1..2^63-2) | `dispatch_one_locked` (`src/async/uring_transport.cpp:189`) | kernel, echoed in the operation CQE | kernel echo is the routing key | `(router_slot << 48) \| cookie48`, bit 63 clear | yes; kernel echo remains the routing key |
| 2 | `RouterEntry.cookie` | same `op_cookie` (`:192`) | `dispatch_one_locked` | `find_live_router_cookie_` (`:551-557`), cancel lookup, recovery | reverse scan over live entries, exact 64-bit equality | same field holds the packed identity; lookup decodes the slot and re-checks the full value | yes; exact equality plus `in_use` |
| 3 | `TransportLedger::Entry.cookie` (operation) | same `op_cookie` (`:199`) | `dispatch_one_locked` | `poison_and_recover_locked` (`:388`) | `find_live_router_cookie_` + handle slot/generation match; mismatch terminates | ledger stores the packed identity; decode + full-equality + handle match | yes |
| 4 | `TransportLedger::Entry.cookie` (cancel_control) | `target_cookie = route.cookie` (`:627, :662-663`) | `issue_running_cancel_locked_` | `mark_consumed_control_locked_` (`:283-287`) via `account_transport_result_locked` / `reconcile_ledger_with_kernel_locked_` | router lookup by cookie, then `control_state == prepared` and handle match; mismatch terminates | ledger stores the packed identity; decode + full equality + `prepared` + handle match | yes |
| 5 | Cancel64 target (`sqe->addr`) | `route.cookie` (`:655`) | `issue_running_cancel_locked_` | kernel `IORING_OP_ASYNC_CANCEL` matching | must equal the target SQE's `user_data` byte-for-byte | packed identity, identical to row 1 | yes; exact SQE match |
| 6 | Control SQE `user_data` | `CONTROL_TAG \| target_cookie` (`:656`) | `issue_running_cancel_locked_` | kernel, echoed in the control CQE | bit 63 selects the control path | `CONTROL_TAG \| packed_identity`; bit 63 still free because the slot field is 15 bits | yes |
| 7 | Control CQE `user_data` | `CONTROL_TAG \| target_cookie` | kernel | `handle_one_cqe` (`src/async/uring_completion.cpp:101-105`) | `is_control_cookie`, strip tag, router lookup, require `control_state == submitted` and `live_control_sqes_ != 0` | strip tag, decode slot, full-equality lookup; same state checks | yes |
| 8 | Operation CQE `user_data` | `op_cookie` | kernel | `handle_one_cqe` (`:148-151`) | `find_live_router_cookie_`; a miss drops the completion | decode slot, verify full identity; a mismatch is a miss and drops the completion | yes; stale completion still cannot attach |
| 9 | Consumed control (`mark_consumed_control_locked_`) | ledger cancel entry's `cookie` | `account_transport_result_locked` / `reconcile_ledger_with_kernel_locked_` | router control state | lookup by cookie + handle match + `prepared` | lookup decodes the slot from the same packed identity | yes |
| 10 | Poison / Class-A recovery | each surviving ledger entry's `cookie` | `poison_and_recover_locked` (`:377-443`) | router terminal/control retirement | lookup by cookie + handle slot/generation match; operation without a delivered terminal is failed and its borrow retired; control refs released | same, with decode-by-slot lookup | yes; a borrow is still retired only through the authoritative recovery path |
| 11 | Stale completion after retirement | old identity | kernel / injected CQE | `handle_one_cqe` | no live entry ⇒ dropped | decode slot; slot free or holding a different cookie ⇒ dropped | yes; never-reuse counter plus full equality |
| 12 | Router slot reuse | `cookie_free_list_` LIFO (`:161-162`), entry zeroed on retire (`:573`) | `retire_router_entry_` / `dispatch_one_locked` | next operation | new operation gets a new cookie; old identity can never match | unchanged; the packed identity differs because the 48-bit cookie differs | yes (48-bit horizon, see below) |
| 13 | Cookie exhaustion | `next_cookie_` must stay `< CONTROL_TAG` (`:530-538`), terminate | `allocate_cookie_` | process | fail-fast before entering the control-tag range or wrapping | fail-fast at `2^48` | explicit failure preserved; horizon narrowed |
| 14 | Shutdown / teardown | live router, ledger, control counters | `~UringAsyncBackend`, `internal_work_retired`, `retire_execution_resources` | process invariant checks | quiescence via `live_cookies_`, `live_control_sqes_`, ledger `all_class_a_recovery_retired`, `event_owed` | unchanged | yes |
| 15 | Test seams `inject_cqe_for_test`, `live_cookie_for_offset_for_test`, `peek_next_cookie_for_test` | raw cookie in baseline | seam | tests | test injects the value it read back | `live_cookie_for_offset_for_test` returns the packed identity; `peek_next_cookie_for_test` keeps exposing the raw counter | two tests updated: `backend_contract_conformance_test.cpp` `canceled_metadata_op_reports_the_canceled_terminal` reads the identity seam instead of `peek_next_cookie_for_test() - 1`; `uring_request_core_integration_test.cpp` `stale_cookie_cannot_reach_a_reused_slot` reads the live identity for each request and injects the first request's identity after the slot is reused (the previous version compared against an unconsumed `peek_next_cookie_for_test()` and was vacuous) |
| 16 | Mutant-only overflow path (`SLUICE_URING_PROGRESS_MUTANT_OVERFLOW_ABSENCE_SETTLES_INFLIGHT`) | `route.cookie` / `make_control_cookie(route.cookie)` | compile-time mutant | test | raw `user_data` comparison in the visible CQ | packed identity compared unchanged | compile-time research mutant only; not in the production profile |

## Domain narrowing (requires an explicit owner decision)

The packed layout spends 15 bits on the router slot. In exchange:

| Domain | Baseline | H1 candidate | Consequence |
|---|---|---|---|
| `UringConfig::request_capacity` | `[1, UINT32_MAX]` (`src/async/uring_backend.cpp:150-159`) | `[1, 32768]` | configurations above 32768 are rejected at setup with `std::invalid_argument` |
| Operation-cookie horizon | `2^63 - 1` | `2^48 - 1` | explicit `std::terminate` at exhaustion, before any wrap can alias |

Both are visible product-domain changes. REQ-01 permits explicit failure before
wrap, and BOUND-01 makes capacity a configured, validated budget, but neither
requirement decides that the narrower domain is acceptable. That decision is
the owner's, not an optimization detail. A runtime slot-width split
(`cookie_bits = 63 - ceil(log2(capacity))`) would widen the cookie for small
capacities at the cost of a variable shift/mask; it does not remove the
capacity ceiling for large capacities and is not measured here.

## Negative controls (research-only)

`tests/uring_identity_envelope_test.cpp` (built only in the isolated candidate
worktree) exercises:

1. identity round-trip and the `slot=32767, cookie=2^48-1` envelope stays below the control tag;
2. `request_capacity = 32768` is accepted and `32769` is rejected;
3. a forged slot in an otherwise valid identity is dropped, and the correct identity still completes;
4. an identity from a retired slot is dropped after the same router slot is reused, and the new identity completes;
5. a control CQE carrying the wrong target does not release the live control reference; the exact target does;
6. a control-tagged CQE with no submitted control state terminates (invariant violation);
7. cookie exhaustion terminates before a wrapped cookie can alias a live slot.

All seven pass on E5. Controls 3 and 4 are the ones that specifically
distinguish H1's decode-and-verify from a slot-only lookup; control 5 exercises
the control-target identity, which is orthogonal to the operation-slot decode.
A slot-only mutant (`entry.in_use && entry.cookie == identity` reduced to
`entry.in_use`) was built in the candidate worktree and fails exactly
`stale_cookie_cannot_reach_a_reused_slot` (20/21 tests pass); the log is
committed as `results/tests/h1_slot_only_mutant.log`.

## Remaining limitations

- The kernel cancel path is exercised through the control-SQE/control-CQE
  identity checks and the exact cancel target value; the matrix records the
  kernel-matching rule, not a kernel trace.
- The 48-bit horizon is a semantic product limit, not an implementation bug.
  At a sustained 1 M operations/s it is reached in ~8.9 years; at 10 M/s in
  ~326 days. The backend terminates rather than aliasing.
- Poison/Class-A recovery is verified by the existing conformance suite and by
  the identity controls; H1 does not change the recovery decision rules.
