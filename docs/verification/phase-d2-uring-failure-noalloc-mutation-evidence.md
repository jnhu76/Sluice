# Phase D2 Uring failure/no-allocation mutation evidence

**Scope:** real liburing, Clang Debug, branch test/phase-d2-uring-failure-noalloc, baseline
`e7e8c4c949cfe7c7e0302196282ed09fd37114bf`.

Every mutation below was applied as one temporary source change, its exact focused detector was run
to a non-zero RED result, the mutation was restored with `apply_patch`, and the same detector was
rerun GREEN. No mutation marker remains in the tree.

## Results

| Mutant | Temporary defect | Exact detector | Observed RED | Restored GREEN |
|---|---|---|---|---|
| M1 | allocate with `new std::byte` at the start of post-commit `enqueue_after_commit` | `SLUICE_TEST_FILTER=uring_d2_ordinary_size_path_is_allocation_free xmake run uring_d2_failure_noalloc_test` | non-zero; always-throw window terminated on `std::bad_alloc` | PASS |
| M2 | route transient `EINTR/EAGAIN/EBUSY` into permanent poison/recovery | `SLUICE_TEST_FILTER=uring_submit_transient_error_recovers_on_next_poll xmake run uring_submit_failure_test` | exit 1; line 335 `backend.poll() == 0` failed | PASS |
| M3 | route `submit == 0` into a fabricated backend-error recovery | `SLUICE_TEST_FILTER=uring_submit_zero_progress_does_not_change_request_state xmake run uring_submit_failure_test` | exit 1; line 362 `backend.poll() == 0` failed | PASS |
| M4 | call `arena_.record_terminal` for each positive reported submit-prefix operation | `SLUICE_TEST_FILTER=uring_scripted_partial_return_does_not_mutate_request_state xmake run uring_submit_failure_test` | non-zero; later original CQE reached duplicate terminal authority and fail-fast terminated | PASS |
| M5 | after Class-A recovery, terminalize every other live router entry, including older Class-C work | `SLUICE_TEST_FILTER=uring_poison_wait_drains_old_kernel_work_without_resubmitting_class_a xmake run uring_submit_failure_test` | exit 1; mixed-batch `write_recovered` cardinality failed because the old Class-C request was also retired | PASS |
| M6 | skip recovery retirement for every Class-A operation ledger entry | `SLUICE_TEST_FILTER=uring_permanent_submit_failure_retires_physical_batch_and_local_fifo xmake run uring_submit_failure_test` | non-zero; accepted Class-A work remained live and quiescence fail-fast terminated | PASS |
| M7 | bypass fatal-poison/admission checks for new size submissions | `SLUICE_TEST_FILTER=uring_d2_poison_rejects_after_capacity_is_recycled xmake run uring_d2_failure_noalloc_test` | non-zero; after recovered Completion reset freed capacity, the forbidden new request was accepted and quiescence fail-fast terminated | PASS |
| M8 | explicitly call `io_uring_submit` from poisoned wait before the `to_submit=0` enter | `SLUICE_TEST_FILTER=uring_d2_poison_wait_never_submits_quarantined_write xmake run uring_d2_failure_noalloc_test` | exit 1; post-wait `sq_ready_for_test()` dropped below its pre-wait value (the mutant flushed the quarantined staged SQE), so the transport-state invariant `sq_after == sq_before` failed | PASS |
| M9 | omit local-dispatch removal before pending/enqueued cancel wins | `SLUICE_TEST_FILTER=uring_d2_pending_cancel_and_class_a_recovery_have_one_winner_each xmake run uring_d2_failure_noalloc_test` | non-zero; recovery reached the still-armed canceled request and fail-fast reported `local poison retirement lost terminal authority` | PASS |
| M10 | leave a proven Class-A cancel-control reference and its idempotence bit live during recovery | `SLUICE_TEST_FILTER=uring_d2_repeated_cancel_control_is_bounded_and_allocation_free xmake run uring_d2_failure_noalloc_test` | non-zero; original terminal remained deferred behind the leaked control reference and quiescence fail-fast terminated | PASS |
| M11 | omit reserved-slot rollback on malformed size descriptor rejection | `SLUICE_TEST_FILTER=uring_d2_precommit_size_rejections_leave_zero_new_residue xmake run uring_d2_failure_noalloc_test` | non-zero; pre-commit reserved-slot residue caused quiescence fail-fast | PASS |

## M8 detector correction

The first M8 attempt replaced `wait_cqe_without_submit()` with
`io_uring_submit_and_wait()`. The pre-D2 mixed test and an initial D2 copy both remained GREEN: the
older pipe CQE could already be ready during `wait_one()`'s initial non-blocking pass, so the
poisoned blocking-wait helper was not guaranteed to execute. That result was correctly treated as a
**surviving mutant**, not evidence.

D2 added a guarded `before_poison_wait` callback to the existing internal-testing hook bundle. The
new detector starts its pipe writer but does not release the old Class-C CQE until the production
path has entered `wait_cqe_without_submit()`. The primary RED detector is the **transport-state
invariant**: the scripted `-EIO` never entered liburing, so the quarantined Class-A write SQE
remains staged in the application-side SQ (`sq_ready_for_test() == 1`); a correct `to_submit=0`
wait must not consume or flush it, while the M8 `io_uring_submit()` mutant flushes it and `sq_ready`
changes. This is independent of when the kernel would settle a flushed write, so the detector has
no disk-timing false-green window. The file `pread` is retained only as an auxiliary side-effect
detector, not as the deterministic evidence. The hook branch and type member are absent from
production builds.

## Restoration audit

After M11 restoration:

```text
rg -n "D2 MUTANT|MUTANT M[0-9]" src include tests scripts docs
```

returned no matches. The final full focused target and existing Uring submit-failure target remain
part of the final-head gate; this document does not substitute for those clean-tree runs.
