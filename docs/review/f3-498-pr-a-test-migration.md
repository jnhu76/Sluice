# F3 / #498 PR A canonical test-consumer migration

Status: implementation candidate; owner #474/#402. Baseline
`a9165f0a997f46d65411967100f87a1cb2627b2b` includes adopted audit #499.
This slice changes tests only, under v1-r4 W-01/W-02/W-03, ARCH-01/02,
SEM-01..06, INV-01/02, REQ-02/04/06, HANDLE-01/02, HOST-02,
CANCEL-01, LIFE and MIG-01/02. It grants no installed-API removal approval.

The seven runtime-semantic test TUs now use canonical File with
AsyncIoContext/RequestScope; the scope owns actual Request bindings, waits
through the progress contract and acquires/takes terminal results. No
ApplicationRuntime, Scheduler, TaskResultSlot, runtime waiter or completed-return
File wrapper participates. Buffers and native resources are declared outside
and outlive their scopes; early-return cleanup drains accepted requests before
borrowed objects can die. The small test helper only waits for and consumes an
already admitted scope ticket. It has no submission, task or result authority.
A 10-second test wait bound detects lack of progress; scope cleanup still settles
accepted work if that wait reports timeout/error/interruption.

`async_sync_admission_test.cpp` and `progress_source_ownership_test.cpp` had
include-only File-wrapper edges. Those includes are removed; their canonical
and controlled backend assertions remain. `uring_backend_smoke_test.cpp` was a
real indirect runtime consumer, not include-only, and all eight cases migrated.
Its existing backend-availability NOT-RUN disposition is retained honestly.

## Mapping and observation ownership

The following table applies to every named case in its TU; the exact old/new
case inventory below prevents a smoke-test consolidation from hiding loss.
The rows describe observations rather than private Completion representations.

| OLD_TEST | OLD_CALL_PATH | REQUIRED_CONTRACT | OLD_OBSERVATION | NEW_CANONICAL_CALL_PATH | NEW_OBSERVATION | NEGATIVE_CONTROL | REMOVED_RUNTIME_DEPENDENCY |
| --- | --- | --- | --- | --- | --- | --- | --- |
| file_read_test | run_task_to_result → RuntimeTaskContext → await_read_at / submit_read | SEM positional bytes, EOF/short I/O, access and closed-resource precedence; REQ-04 acquired publication; File ownership/close/move | returned count/bytes/error, resource validity; Completion idle/outstanding adapter state | File → AsyncIoContext → RequestScope.submit_read → wait_for/take; File open/close/move stays direct | same byte/offset/count/error and fd EBADF checks; admitted ticket and acquired terminal; explicit short-read canary added | wrong offset/length/count corrupts existing byte checks; short-read overwrite changes untouched canary; zero-dispatch and lifetime controls below | task-result, runtime-task/context, await File wrapper |
| file_write_test | runtime task → await_write_at / submit_write | positional bytes, extension, access; accepted borrow settlement | returned count, pread/content/stat, Completion adapter state | scope.submit_write → wait_for/take | same POSIX content/size/offset and count; rejection must be before acceptance with no outstanding work | wrong offset/count fails POSIX probes; accepting closed/wrong-access submission fails explicit admission observation | same |
| file_sync_data_test | runtime task → await_write_at / await_sync_data | SEM-06 data sync, errno propagation; settlement before close | written bytes, content, sync success/error; idle/reusable Completion | scope.submit_write / submit_sync_data → wait_for/take; successive independently owned Requests reuse one scope slot | same content and success; /dev/null EINVAL; closed-file initiation rejected; successive capacity=1 requests settle | fsync/fdatasync trace discriminates operation family; semantic_durability_reference_test is the independent durability oracle; wrong errno fails /dev/null | same |
| file_sync_all_test | runtime task → await_write_at / await_sync_all | SEM-06 full sync; same lifetime/error obligations | content, success, backend errno and Completion reuse | scope.submit_write / submit_sync_all → wait_for/take | same independent observations, successive owned Requests | same trace/reference oracle; wrong errno/failing sync fails cases | same |
| explicit_file_ref_test | runtime task → context submit / await helper / cancel_waiter | native provenance, multiple outstanding ops, accepted settlement, operation cancel and resource lifetime | bare-fd compile rejection; byte slices/content; terminal and follow-up; access errors; declaration-vs-OS access distinction | RequestScope.submit_* with File/NativeFileRef; context.cancel(ticket.id); wait_for/take | same compile assertions, three independent accepted operations and byte observations; canceled or successful terminal then slot re-use; raw read-only fd declared read-write must be admitted then return backend_error/EBADF | bare-fd constructibility fails static_assert; wrong slices/content fail; rejection instead of OS failure fails explicit interop assertion; controlled cancellation/lifetime tests below | runtime waiter cancellation, await drain, task result and await helper |
| file_access_precedence_test | direct blocking File; runtime await wrappers; context Completion/RequestHandle and existing owned Request cases | SEM-03 invalid resource before access, access before zero/offset; no-op before range and request admission | exact error priority, counting-backend non-entry, zero result | direct cases retained; wrapper cases → scope.submit_read/write with admission/error checks; pre-existing compat/canonical context cases retained | same precedence errors; wrapper replacements must reject before acceptance, outstanding=0; canonical accepted zero tickets; existing CountingBackend negative observations retained | reference_case V01/V02/V03 and validation_precedence oracle; accepting invalid scope submission is failure; backend entry counters expose misordering | wrapper/task-result runtime edges only |
| uring_backend_smoke_test | run_task_to_result → real Uring backend + RuntimeTaskContext.submit/await_completion | actual real-ring read/write/sync, zero/range precedence and borrow settlement | byte/count/success and initiation rejection | scope.submit_* → owned Request → progress wait/take | same read/count/sync/error assertions; independent pread write-byte check added; rejected offsets must leave outstanding=0 | unavailable real ring stays NOT-RUN; wrong write data fails added pread; uring cutover zero-dispatch mutant below | task runtime and indirect File wrapper closure |
| async_sync_admission_test / progress_source_ownership_test | existing canonical context/backend seams | sync admission and progress source ownership | existing error, capacity and ownership assertions | unchanged canonical paths | unchanged | existing controlled tests unchanged | include-only async/file.hpp |

Legacy `Completion::idle()` after a wrapper returns and reuse of that same
Completion are adapter-specific observations. Canonical non-empty operations
own Requests; rejection and publication are asserted instead of mirroring
Completion layout. Canonical zero operations still occupy a Request slot and
publish at acceptance; empty-buffer cases check a valid ready ticket. This is
an invocation difference (INV-01/SEM-03), not permission to discard admission.

The migrated real-backend resource cases establish successful acquired settlement
before close and keep the File/buffer alive. Neither their old `outstanding()`
nor the new valid-ticket check proves the operation is still physically pending.
That independent adverse-interleaving evidence remains owned by
`request_scope_test::{destructor_settles_on_early_return_with_drain_policy,
v18b_second_submission_rejection_keeps_first_owned,
v19_timeout_preserves_responsibility_until_cleanup,
cancel_won_before_execution_settles_as_a_canceled_terminal}` and
`threadpool_core_cutover_test::{claim_chain_and_running_cancel_do_not_release_the_borrow,
last_borrow_access_precedes_final_execution_retirement}`. Their controlled
backend/barrier schedules expose premature borrow retirement independently.
`public_request_test` and `request_core_publication_test` retain public terminal
and observer/publication probes. Existing `request_scope_mut_commit_drops_request`
and timeout-release mutants (exact names in xmake/tests.lua) are negative controls
for these obligations; this migration does not alter the production protocol.

Zero data effect is not inferred from a zero-byte result alone:
`threadpool_core_cutover_test::zero_op_publishes_at_acceptance_without_dispatch`
checks the syscall counter unchanged, ready-at-acceptance and core binding;
`uring_core_cutover_test` has the corresponding zero-dispatch oracle, killed by
`uring_cutover_mut_zero_op_dispatch`. `semantic_reference_case_test` V03 also
checks full-table admission rejection despite a zero-length operation.
`semantic_short_io_reference_test` and `semantic_backend_conformance_test`
remain the independent backend-neutral short-I/O/effect oracles.

## Exact old/new case inventory

### file_read_test.cpp

| OLD_TEST case | NEW_TEST case | Disposition |
| --- | --- | --- |
| `read_returns_bytes_at_offset` | `read_returns_bytes_at_offset` | preserved contract observation; adapter-only state re-owned as described above |
| `read_past_end_returns_zero` | `read_past_end_returns_zero` | preserved contract observation; adapter-only state re-owned as described above |
| `read_empty_buffer_returns_zero_without_operation` | `read_empty_buffer_returns_zero_without_data_effect` | preserved contract observation; adapter-only state re-owned as described above |
| `outstanding_read_resource_valid_until_terminal` | `outstanding_read_resource_valid_until_terminal` | preserved contract observation; adapter-only state re-owned as described above |
| `close_releases_once_and_object_stops_representing_resource` | `close_releases_once_and_object_stops_representing_resource` | preserved contract observation; adapter-only state re-owned as described above |
| `moved_file_owns_nothing_and_target_keeps_resource` | `moved_file_owns_nothing_and_target_keeps_resource` | preserved contract observation; adapter-only state re-owned as described above |
| `open_create_new_rejects_existing` | `open_create_new_rejects_existing` | preserved contract observation; adapter-only state re-owned as described above |
| `truncate_requires_writable_access` | `truncate_requires_writable_access` | preserved contract observation; adapter-only state re-owned as described above |
| `truncate_replaces_contents_for_writable_access` | `truncate_replaces_contents_for_writable_access` | preserved contract observation; adapter-only state re-owned as described above |
| `read_after_close_reports_invalid_state` | `read_after_close_reports_invalid_state` | preserved contract observation; adapter-only state re-owned as described above |
| `await_read_on_write_only_file_rejected_upfront` | `scoped_read_on_write_only_file_rejected_upfront` | preserved contract observation; adapter-only state re-owned as described above |
| added | `short_read_reports_only_available_bytes` | short read count/bytes and untouched-tail canary |

### file_write_test.cpp

| OLD_TEST case | NEW_TEST case | Disposition |
| --- | --- | --- |
| `write_replaces_bytes_at_offset` | `write_replaces_bytes_at_offset` | preserved contract observation; adapter-only state re-owned as described above |
| `write_can_extend_file` | `write_can_extend_file` | preserved contract observation; adapter-only state re-owned as described above |
| `write_empty_buffer_returns_zero_without_operation` | `write_empty_buffer_returns_zero_without_data_effect` | preserved contract observation; adapter-only state re-owned as described above |
| `write_after_close_reports_invalid_state` | `write_after_close_reports_invalid_state` | preserved contract observation; adapter-only state re-owned as described above |
| `write_with_read_only_access_fails` | `write_with_read_only_access_fails` | preserved contract observation; adapter-only state re-owned as described above |
| `write_result_reports_bytes_written` | `write_result_reports_bytes_written` | preserved contract observation; adapter-only state re-owned as described above |
| `write_keeps_resource_and_buffer_valid_until_terminal` | `write_keeps_resource_and_buffer_valid_until_terminal` | preserved contract observation; adapter-only state re-owned as described above |
| `file_access_query_reports_open_mode` | `file_access_query_reports_open_mode` | preserved contract observation; adapter-only state re-owned as described above |

### file_sync_data_test.cpp

| OLD_TEST case | NEW_TEST case | Disposition |
| --- | --- | --- |
| `sync_data_after_write_succeeds` | `sync_data_after_write_succeeds` | preserved contract observation; adapter-only state re-owned as described above |
| `sync_data_completion_is_reusable` | `sync_data_successive_requests_settle` | preserved contract observation; adapter-only state re-owned as described above |
| `sync_data_on_read_only_file_succeeds` | `sync_data_on_read_only_file_succeeds` | preserved contract observation; adapter-only state re-owned as described above |
| `sync_data_on_write_only_file_succeeds` | `sync_data_on_write_only_file_succeeds` | preserved contract observation; adapter-only state re-owned as described above |
| `sync_data_after_close_reports_invalid_state` | `sync_data_after_close_reports_invalid_state` | preserved contract observation; adapter-only state re-owned as described above |
| `sync_data_on_device_reports_backend_error` | `sync_data_on_device_reports_backend_error` | preserved contract observation; adapter-only state re-owned as described above |

### file_sync_all_test.cpp

| OLD_TEST case | NEW_TEST case | Disposition |
| --- | --- | --- |
| `sync_all_after_write_succeeds` | `sync_all_after_write_succeeds` | preserved contract observation; adapter-only state re-owned as described above |
| `sync_all_completion_is_reusable` | `sync_all_successive_requests_settle` | preserved contract observation; adapter-only state re-owned as described above |
| `sync_all_on_read_only_file_succeeds` | `sync_all_on_read_only_file_succeeds` | preserved contract observation; adapter-only state re-owned as described above |
| `sync_all_on_write_only_file_succeeds` | `sync_all_on_write_only_file_succeeds` | preserved contract observation; adapter-only state re-owned as described above |
| `sync_all_after_close_reports_invalid_state` | `sync_all_after_close_reports_invalid_state` | preserved contract observation; adapter-only state re-owned as described above |
| `sync_all_on_device_reports_backend_error` | `sync_all_on_device_reports_backend_error` | preserved contract observation; adapter-only state re-owned as described above |

### explicit_file_ref_test.cpp

| OLD_TEST case | NEW_TEST case | Disposition |
| --- | --- | --- |
| `multiple_outstanding_ops_on_one_canonical_file` | `multiple_outstanding_ops_on_one_canonical_file` | preserved contract observation; adapter-only state re-owned as described above |
| `explicit_native_ref_over_raw_fd` | `explicit_native_ref_over_raw_fd` | preserved contract observation; adapter-only state re-owned as described above |
| `cancel_through_file_referenced_op_reaches_terminal` | `cancel_through_file_referenced_op_reaches_terminal` | preserved contract observation; adapter-only state re-owned as described above |
| `file_derived_ref_rejected_at_submit_on_wrong_access` | `file_derived_ref_rejected_at_submit_on_wrong_access` | preserved contract observation; adapter-only state re-owned as described above |
| `file_derived_write_ref_rejected_at_submit_on_read_only_file` | `file_derived_write_ref_rejected_at_submit_on_read_only_file` | preserved contract observation; adapter-only state re-owned as described above |
| `interop_declared_access_is_a_claim_not_validation` | `interop_declared_access_is_a_claim_not_validation` | preserved contract observation; adapter-only state re-owned as described above |
| `closed_file_derived_ref_rejected_as_invalid_state` | `closed_file_derived_ref_rejected_as_invalid_state` | preserved contract observation; adapter-only state re-owned as described above |
| `zero_length_op_succeeds_before_offset_validation` | `zero_length_op_succeeds_before_offset_validation` | preserved contract observation; adapter-only state re-owned as described above |

### file_access_precedence_test.cpp

| OLD_TEST case | NEW_TEST case | Disposition |
| --- | --- | --- |
| `blocking_read_closed_with_empty_buffer_reports_invalid_state` | `blocking_read_closed_with_empty_buffer_reports_invalid_state` | preserved contract observation; adapter-only state re-owned as described above |
| `blocking_write_closed_with_empty_buffer_reports_invalid_state` | `blocking_write_closed_with_empty_buffer_reports_invalid_state` | preserved contract observation; adapter-only state re-owned as described above |
| `blocking_read_at_closed_with_empty_buffer_reports_invalid_state` | `blocking_read_at_closed_with_empty_buffer_reports_invalid_state` | preserved contract observation; adapter-only state re-owned as described above |
| `blocking_write_at_closed_with_empty_buffer_reports_invalid_state` | `blocking_write_at_closed_with_empty_buffer_reports_invalid_state` | preserved contract observation; adapter-only state re-owned as described above |
| `blocking_read_on_write_only_with_empty_buffer_reports_invalid_argument` | `blocking_read_on_write_only_with_empty_buffer_reports_invalid_argument` | preserved contract observation; adapter-only state re-owned as described above |
| `blocking_write_on_read_only_with_empty_buffer_reports_invalid_argument` | `blocking_write_on_read_only_with_empty_buffer_reports_invalid_argument` | preserved contract observation; adapter-only state re-owned as described above |
| `blocking_read_at_on_write_only_with_empty_buffer_reports_invalid_argument` | `blocking_read_at_on_write_only_with_empty_buffer_reports_invalid_argument` | preserved contract observation; adapter-only state re-owned as described above |
| `blocking_write_at_on_read_only_with_empty_buffer_reports_invalid_argument` | `blocking_write_at_on_read_only_with_empty_buffer_reports_invalid_argument` | preserved contract observation; adapter-only state re-owned as described above |
| `blocking_read_on_closed_write_only_reports_invalid_state` | `blocking_read_on_closed_write_only_reports_invalid_state` | preserved contract observation; adapter-only state re-owned as described above |
| `blocking_write_on_closed_read_only_reports_invalid_state` | `blocking_write_on_closed_read_only_reports_invalid_state` | preserved contract observation; adapter-only state re-owned as described above |
| `blocking_read_at_on_closed_write_only_reports_invalid_state` | `blocking_read_at_on_closed_write_only_reports_invalid_state` | preserved contract observation; adapter-only state re-owned as described above |
| `blocking_write_at_on_closed_read_only_reports_invalid_state` | `blocking_write_at_on_closed_read_only_reports_invalid_state` | preserved contract observation; adapter-only state re-owned as described above |
| `blocking_read_at_empty_buffer_ignores_unrepresentable_offset` | `blocking_read_at_empty_buffer_ignores_unrepresentable_offset` | preserved contract observation; adapter-only state re-owned as described above |
| `blocking_write_at_empty_buffer_ignores_unrepresentable_offset` | `blocking_write_at_empty_buffer_ignores_unrepresentable_offset` | preserved contract observation; adapter-only state re-owned as described above |
| `blocking_resize_on_closed_read_only_reports_invalid_state` | `blocking_resize_on_closed_read_only_reports_invalid_state` | preserved contract observation; adapter-only state re-owned as described above |
| `blocking_resize_on_closed_file_with_unrepresentable_size_reports_invalid_state` | `blocking_resize_on_closed_file_with_unrepresentable_size_reports_invalid_state` | preserved contract observation; adapter-only state re-owned as described above |
| `await_read_at_closed_with_empty_buffer_reports_invalid_state` | `scoped_read_closed_with_empty_buffer_reports_invalid_state` | preserved contract observation; adapter-only state re-owned as described above |
| `await_write_at_closed_with_empty_buffer_reports_invalid_state` | `scoped_write_closed_with_empty_buffer_reports_invalid_state` | preserved contract observation; adapter-only state re-owned as described above |
| `await_read_at_on_write_only_with_empty_buffer_reports_invalid_argument` | `scoped_read_on_write_only_with_empty_buffer_reports_invalid_argument` | preserved contract observation; adapter-only state re-owned as described above |
| `await_write_at_on_read_only_with_empty_buffer_reports_invalid_argument` | `scoped_write_on_read_only_with_empty_buffer_reports_invalid_argument` | preserved contract observation; adapter-only state re-owned as described above |
| `await_read_at_on_closed_write_only_reports_invalid_state` | `scoped_read_on_closed_write_only_reports_invalid_state` | preserved contract observation; adapter-only state re-owned as described above |
| `await_write_at_on_closed_read_only_reports_invalid_state` | `scoped_write_on_closed_read_only_reports_invalid_state` | preserved contract observation; adapter-only state re-owned as described above |
| `await_read_at_empty_buffer_ignores_unrepresentable_offset` | `scoped_read_empty_buffer_ignores_unrepresentable_offset` | preserved contract observation; adapter-only state re-owned as described above |
| `await_write_at_empty_buffer_ignores_unrepresentable_offset` | `scoped_write_empty_buffer_ignores_unrepresentable_offset` | preserved contract observation; adapter-only state re-owned as described above |
| `submit_read_rejects_closed_empty_reference_before_backend` | `submit_read_rejects_closed_empty_reference_before_backend` | preserved contract observation; adapter-only state re-owned as described above |
| `submit_write_rejects_closed_empty_reference_before_backend` | `submit_write_rejects_closed_empty_reference_before_backend` | preserved contract observation; adapter-only state re-owned as described above |
| `submit_read_rejects_illegal_access_with_empty_buffer_before_backend` | `submit_read_rejects_illegal_access_with_empty_buffer_before_backend` | preserved contract observation; adapter-only state re-owned as described above |
| `submit_write_rejects_illegal_access_with_empty_buffer_before_backend` | `submit_write_rejects_illegal_access_with_empty_buffer_before_backend` | preserved contract observation; adapter-only state re-owned as described above |
| `submit_read_rejects_closed_illegal_access_as_invalid_state` | `submit_read_rejects_closed_illegal_access_as_invalid_state` | preserved contract observation; adapter-only state re-owned as described above |
| `submit_write_rejects_closed_illegal_access_as_invalid_state` | `submit_write_rejects_closed_illegal_access_as_invalid_state` | preserved contract observation; adapter-only state re-owned as described above |
| `submit_read_rejects_illegal_access_before_offset_validation` | `submit_read_rejects_illegal_access_before_offset_validation` | preserved contract observation; adapter-only state re-owned as described above |
| `submit_write_rejects_illegal_access_before_offset_validation` | `submit_write_rejects_illegal_access_before_offset_validation` | preserved contract observation; adapter-only state re-owned as described above |
| `submit_read_request_rejects_illegal_access_with_empty_buffer_before_backend` | `submit_read_request_rejects_illegal_access_with_empty_buffer_before_backend` | preserved contract observation; adapter-only state re-owned as described above |
| `submit_write_request_rejects_illegal_access_with_empty_buffer_before_backend` | `submit_write_request_rejects_illegal_access_with_empty_buffer_before_backend` | preserved contract observation; adapter-only state re-owned as described above |
| `submit_read_request_rejects_closed_illegal_access_as_invalid_state` | `submit_read_request_rejects_closed_illegal_access_as_invalid_state` | preserved contract observation; adapter-only state re-owned as described above |
| `submit_write_request_rejects_closed_illegal_access_as_invalid_state` | `submit_write_request_rejects_closed_illegal_access_as_invalid_state` | preserved contract observation; adapter-only state re-owned as described above |
| `submit_zero_length_read_completes_zero_despite_unrepresentable_offset` | `submit_zero_length_read_completes_zero_despite_unrepresentable_offset` | preserved contract observation; adapter-only state re-owned as described above |
| `submit_zero_length_write_completes_zero_despite_unrepresentable_offset` | `submit_zero_length_write_completes_zero_despite_unrepresentable_offset` | preserved contract observation; adapter-only state re-owned as described above |
| `submit_read_with_unrepresentable_offset_rejected_at_admission` | `submit_read_with_unrepresentable_offset_rejected_at_admission` | preserved contract observation; adapter-only state re-owned as described above |
| `submit_write_with_unrepresentable_offset_rejected_at_admission` | `submit_write_with_unrepresentable_offset_rejected_at_admission` | preserved contract observation; adapter-only state re-owned as described above |

### uring_backend_smoke_test.cpp

| OLD_TEST case | NEW_TEST case | Disposition |
| --- | --- | --- |
| `read_op_through_canonical_file` | `read_op_through_canonical_file` | preserved contract observation; adapter-only state re-owned as described above |
| `write_op_through_canonical_file` | `write_op_through_canonical_file` | preserved contract observation; adapter-only state re-owned as described above |
| `sync_data_op_through_canonical_file` | `sync_data_op_through_canonical_file` | preserved contract observation; adapter-only state re-owned as described above |
| `sync_all_op_through_canonical_file` | `sync_all_op_through_canonical_file` | preserved contract observation; adapter-only state re-owned as described above |
| `zero_length_read_completes_zero_despite_unrepresentable_offset` | `zero_length_read_completes_zero_despite_unrepresentable_offset` | preserved contract observation; adapter-only state re-owned as described above |
| `zero_length_write_completes_zero_despite_unrepresentable_offset` | `zero_length_write_completes_zero_despite_unrepresentable_offset` | preserved contract observation; adapter-only state re-owned as described above |
| `read_with_unrepresentable_offset_rejected_at_admission` | `read_with_unrepresentable_offset_rejected_at_admission` | preserved contract observation; adapter-only state re-owned as described above |
| `write_with_unrepresentable_offset_rejected_at_admission` | `write_with_unrepresentable_offset_rejected_at_admission` | preserved contract observation; adapter-only state re-owned as described above |

## Authority contraction and package limits

No production authority, flag, token, counter, lease or capability probe is
added or moved. RequestScope is the already adopted canonical cleanup owner;
its capacity (1 for sequential, 3 for concurrent native-ref case) expresses the
concrete admitted workload. The helper and assertions are test-only evidence.
This test migration makes seven test runtime construction/task-result/waiter
edges redundant; it removes those edges. It does not yet make production
ApplicationRuntime/Group/WaitPolicy or their external installed declarations
eligible for deletion. Completion, RequestCore physical retirement and optional
Fiber/host cleanup remain independent obligations. Two remaining runtime test
TUs (`runtime_waiter_observer_test`, `app_copy_pipeline_oracle_test`) stay owned
by the next slice; their evidence is not deleted here.

`xmake/tests.lua` already declares the correct core+async libraries for these
canonical consumers; `progress_source_ownership_test` retains its explicit
controlled source list. No target dependency needs changing. Compiler closure
and linked symbols are checked on the fresh matrix outputs; absence of a source
include alone is not claimed as dependency migration. Production source and
installed-header removal are both zero; historical F1/F3 manifests remain intact.

Remote matrix receipts will be recorded separately with the tested code SHA,
complete configuration, build/run commands, compiler/link instrumentation,
raw logs, status and limitations. Independent review and merge authorization
are separate gates; this document does not assert either.

## Clang sanitizer prerequisite correction

The first fresh ASan/UBSan and TSan builds at `432dd1e9` passed twelve
affected tests, but `request_scope_test` failed compilation under Clang 22
with `-Werror,-Wunused-variable`: the never-read local
`std::optional<ScopeTicket<std::size_t>> ticket` in the existing pre-dispatch
cancel test. Removing that inert declaration is the minimal compile correction;
no assertion, cancellation, scheduling or sanitizer instrumentation changes.
The corrected scope target is rechecked in the four base profiles and both
sanitizer configurations; other passing tests need no repeat because their
source, production library and configuration are unchanged.

The first real-liburing smoke builds at `432dd1e9` also exposed a migration
compile defect: the one-byte input local `src` from the old write-range lambda
had not been carried into its canonical replacement. Restoring that same
constant byte leaves the rejection oracle unchanged. Only the two real-liburing
smoke consumers need rebuild/re-execution after the correction.

## Final remote receipt

The authoritative [2026-10-12 remote receipt](../evidence/f3-498-pr-a-20261012/REPRODUCE.md)
and [structured results](../evidence/f3-498-pr-a-20261012/summary.json) test code
`3f846afa37101546fa151adbd2e19c1b08f04ea0`: four strict fresh-output profiles
PASS (40 n / 52 y each mode), ASan/UBSan 13/13 PASS, and TSan twelve static
consumer suites plus the shared-runtime scope suite 25/25 PASS. Exact runtime
composition and original failures are preserved. Four existing negative
controls are killed; installed canonical/optional-host probes pass. The package
remains 50 objects / 37 async members / 79 headers, with zero migrated-consumer
legacy compiler/link edges. No external-risk adoption or merge is asserted.
