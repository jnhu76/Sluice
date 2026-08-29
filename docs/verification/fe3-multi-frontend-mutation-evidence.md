# FE-3/FE-4 multi-frontend mutation evidence (M1–M5)

Purpose: prove the FE-3 slice/mixing tests FAIL on deliberately nonconforming
behavior of the shared authorities they claim to guard (same methodology as
the Phase C2x mutation campaigns; AGENTS.md §16/§17 spirit). Each mutation is
a single-point production/seam edit applied to the FE-3 closeout tree, the
affected test binaries are relinked and run, the RED result is recorded, and
the mutation is reverted (tree restored to `fe48e19` + the new M5 coverage
case; final state re-verified GREEN).

Baseline: `feat/frontend-semantic-reuse` @ `fe48e19` (FE-3 complete).
All runs: Linux Clang Debug (`xmake f -m debug --toolchain=clang`).

| ID | Site (file:law) | Mutation | RED evidence (actual) |
|----|-----------------|----------|----------------------|
| M1 | `scheduler.cpp` `publish_wait_winner_locked` — the ONE winner-kind publication tail | `deferred:` branch publishes nothing (delivery obligation dropped) | `fe2_stackless_event_pov_test` FAILED (`deferred_depth_for_test(sched) == 1` — transit never committed); `fe3_stackless_queue_slice_test` aborted (`terminate called without an active exception` — stranded parked continuation); `fe3_stackless_condition_slice_test` FAILED (`deferred_depth == 1`); `fe3_stackless_rwlock_slice_test` aborted (`~AsyncRwLock: destroyed with active writer` — the granted deferred writer never resumed/released) |
| M2 | `scheduler_condition.cpp` `condition_cancel_wait` — winner-tail publication | revert to direct `cond_node.fiber()` fiber publication (the pre-slice shape) | `fe3_condition_deferred_cancel_loser_exactly_once` aborted (`terminate` — the cancelled deferred waiter is never delivered) |
| M3 | `scheduler_rwlock.cpp` write ladder inline claim — `writer_owner = actor` commit | ownership commit dropped | `fe3_rwlock_deferred_writer_owns_releases` aborted (`~AsyncRwLock: destroyed with active writer` — `owned_by(&actor_a)` check failed, early return with the lock still held) |
| M4 | `scheduler_fe2_test_seam.cpp` `condition_wait_deferred_core_` — the L7 eligibility commit (`record.arm()` inside the resolver-excluded CS) | `record.arm()` dropped on `authorized` | `fe3_condition_deferred_notify_one_own_reacquire` aborted (armed-state check failed → early return → registered node destroyed) |
| M5 | `scheduler_queue.cpp` pop ladder `resolved_inline_grant` (the Q-LIV-1 grant obligation of the blocking admission ENTRY) | downgraded to `resolved_inline` (entry skips the opposite-role grant) | FIRST attempt RED-flagged a COVERAGE GAP: no FE-3 case ran a BLOCKING admission entry as the resolver (existing resolvers were `try_pop`/`try_push`, whose reconcile lives in QueuePort itself). Repair: new slice case `fe3_q_fiber_blocking_pop_grants_deferred_producer` (fiber `port.pop()` against a parked deferred producer). With the case added: mutation RED (stranded deferred producer — no `ALL TESTS PASSED`); reverted run GREEN 13/13 |

Notes:
- M5's first attempt is retained in the record deliberately: the mutation
  campaign did its job — it found the uncovered law site, the coverage was
  added, and the law is now guarded (RED→GREEN pair above).
- M1–M5 were each reverted before the next was applied; after the final
  revert the full queue slice passes 13/13 and the tree is back to the
  committed state plus this evidence file and the new queue-slice case.
