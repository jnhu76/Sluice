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

## FE-CORRECTIVE-1 corrective-round mutations (C1–C4)

Same methodology, applied to the corrective BASE `0085626` during the
FE-CORRECTIVE-1 round (commits `e57c16e`/`f53d7ac`/`c5cb90d`/`0cc738b`);
each fix's witness fails when the pre-corrective shape is restored, and the
mutation is reverted byte-clean. (The corrective PR body refers to these as
its "M1–M4"; they are relabeled C1–C4 here to avoid colliding with the
FE-3/FE-4 M1–M5 rows above.) Exit codes are the shared death-test constants
(`tests/death_test_runner_posix.hpp`): 86 = deterministic-terminate fail-fast
boundary, 87 = child reached an "unreachable" return, 88 = child detected the
mutation shape.

| ID | Site (file:law) | Mutation (restore pre-corrective shape) | RED evidence (actual) | GREEN witness at `e8d6e32` |
|----|-----------------|------------------------------------------|----------------------|-----------------------------|
| C1 | `scheduler.cpp` `defer_publication_locked` — transit-insertion failure containment | drop the `noexcept` + named fail-fast wrap so a storage failure at the insertion escapes as `bad_alloc` AFTER the terminal winner is irreversible | `fe2_publication_atomicity_death_test` PUB1: the child catches the escaping `bad_alloc` in its own try/catch (delivery obligation stranded, no gate can see the loss) and exits 88. Healthy tree: the containment fires at the same edge (exit 86) in Debug AND Release | PUB1 fail-fast contained + PUBCTL healthy-path exactly-once (depth 1, woken node, exactly-once take/consume) |
| C2 | `scheduler_fe2_test_seam.cpp` Queue deferred cores — `active_port_calls_` pin transfer | release the pin at `await_suspend` (pre-corrective shape) instead of transferring it to the awaiting frame until `await_resume` result conversion | `async_queue_lifecycle_death_test` QD1: with every other teardown precondition already zero, `begin_teardown()` RETURNS inside the committed-winner→result-consumption window (child exit 87) — the port may die under the suspended continuation. Restored run: `begin_teardown` fail-fasts in that window | QPIN-1/QPIN-2 phase witnesses (pin==1 parked→winner→publication-pending; 0 only after consumption) + QD1 fail-fast |
| C3 | `scheduler_rwlock.cpp` fiber write entries — recursive-owner check position | move the `writer_owner` read back BEFORE `global_mtx_` (pre-corrective shape) | TSan reports the data race on `writer_owner` under the two-worker turnover stress case | `rwlock_write_admit_locked` consumes the ONE check under G for BOTH frontends; death child RW (recursive blocking write) + G-guarded test observers (`rwlock_writer_active_for_test` / `rwlock_owned_by_for_test`) |
| C4 | `include/sluice/async/wait_node.hpp` `WaitResume::fiber(nullptr)` — token normalization | allow the incoherent null-fiber-kind token to construct again | the static-assert witness FAILS TO COMPILE (the null-skip guards in the legacy expire/cancel tails become load-bearing again instead of being structurally dead) | null pointer normalizes to `Kind::none` at the single construction point; winner-kind tails never observe a null fiber-kind |

Bounds of the evidence: C1–C4 prove the corrective WITNESSES are sensitive to
exactly the restored defects — they do not prove absence of other defects, and
C1's GREEN posture is a named process-terminal boundary (transit storage may
allocate; failure after winner commit is terminal), not an allocation-free
publication tail.
