# R4 — Mutation Receipts

## TP-M1 — suppress the internal test counter increment after a real syscall

- **Mutant:** in `worker_loop()`, replace
  `syscall_count_.fetch_add(1, std::memory_order_relaxed);` with a
  suppression comment (diff recorded at `patches/mutant-tp-m1.diff`; applied
  transiently in worktree `/tmp/sluice-575/mut-tp`, never committed).
- **Killer:** `threadpool_request_core_integration_test` (seam build,
  macro-active).

### Receipt (BASE GREEN → MUTANT RED → RESTORED GREEN)

```
# BASE (pre-mutant, same worktree, baseline source):
all 17 threadpool core integration tests passed

# MUTANT (xmake run threadpool_request_core_integration_test):
FAIL [last_borrow_access_precedes_final_execution_retirement] the physical syscall has completed
FAIL [publication_visibility_waits_for_the_epilogue_and_release_reclaims] the whole episode needed exactly the one physical syscall
FAIL [bounded_workers_execute_the_syscalls_not_the_submitter] every syscall ran on a worker
3 of 17 threadpool core integration tests failed
(exit code 1)

# RESTORED (git checkout -- src/async/threadpool_backend.cpp, rebuild):
all 17 threadpool core integration tests passed
```

**Kill validity:** all three failures are the intended
`syscall_count_for_test()` assertions (`== 1` exact-total and `== 4`
multi-worker totals). No timeout, no abort, no unrelated teardown failure.
The other 14 scenarios contain no exact-positive counter assertion, which is
consistent with the oracle design (only counting-relevant scenarios assert
counts).

## UR-M1 — suppress the internal test counter increment after a real transport submission attempt

- **Mutant:** in `submit_transport_locked()`, replace the
  `submit_flushes_.fetch_add(1, std::memory_order_relaxed);` line with a
  suppression comment (transient in `/tmp/sluice-575/mut-ur`).
- **Killer:** `uring_flush_count_oracle_test` (the new research oracle,
  seam build with the fake-submit hook; the increment precedes the hook call
  so the hook path exercises the identical site).

### Receipt (BASE GREEN → MUTANT RED → RESTORED GREEN)

```
# BASE (pre-mutant, same worktree):
[uring_flush_count_oracle_test] all checks passed

# MUTANT (xmake run uring_flush_count_oracle_test):
FAIL [uring_flush_count_oracle] one accepted data submission flushes transport exactly once
FAIL [uring_flush_count_oracle] completion and polling add no further flush attempts
FAIL [uring_flush_count_oracle] two sequential data submissions flush transport exactly twice
[uring_flush_count_oracle_test] 3 check(s) failed
(exit code 1)

# RESTORED (git checkout -- src/async/uring_transport.cpp, rebuild):
[uring_flush_count_oracle_test] all checks passed
```

**Kill validity:** all three failures are the intended positive
`submit_flushes_for_test()` assertions.

### Oracle-gap finding (material to the campaign)

Against the same UR-M1 mutant, the **pre-existing**
`uring_request_core_integration_test` stayed green:

```
21 of 21 uring core integration tests passed, 0 not run
```

Every pre-existing UR counter assertion is a zero-value or delta-unchanged
check (R1 reference inventory), so a counter that never increments satisfies
them all. The new oracle is therefore not redundant: it closes a real
observation gap and is required for the fail-closed test-observability claim
of the FULL arm. This finding is recorded as a deliverable of the campaign
(the oracle test is committed on the research branch, and at closeout was
productized on master via test-only PR #585, squash merge
`1df8b4b82f9078dab824a4559684c40503d3e2cd`; the UR-M1 counterfactual on
that base — BASE GREEN → three counter-assertion failures, exit 1 →
RESTORED GREEN — is receipted in PR #585's description and review-receipt
comment, including an independent re-execution by the test-correctness
reviewer).

## Cross-check: Release binary has no RMW while the test binary still counts

- Per-arm disassembly (R3): OP-ONLY and FULL arm objects contain **zero**
  target locked RMWs; the linked Release benchmarks lack the target sites.
- Per-arm oracle runs (R3): `uring_flush_count_oracle_test` and the full
  78-test suite (including all TP exact-count assertions) pass on every
  patched arm — the internal test build still counts.
