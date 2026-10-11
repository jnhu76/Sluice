# Postmerge runtime semantic consumer and physical retirement audit

Baseline: 0d365624661b2b6569dd4bfa68fe095f8cd59ece, /home/jnhu/f3-runtime-retirement-audit. `git diff c3217fa9 HEAD -- include src apps tests xmake.lua xmake` empty. Read-only inspection; no build or deletion performed. Root PROD-01/02/03, ARCH-01/02, HOST-01/02/03, MIG-01/02 and F0 package policy §1.7 apply. #475 latest T2 policy remains A BOUNDED_HOLD, B/C KEEP_INSTALLED_COMPAT; exclusion policy adoption is not physical deletion authorization.

## Exact metrics and actual consumers

Count unit is distinct source path/target, never textual occurrence or runtime invocation count. APP_RUNTIME_CONSUMERS_IN_APPS=0: no app production C++ TU uses ApplicationRuntime/RuntimeBuilder/RuntimeTaskContext, Scheduler, Group/Future/WaitPolicy or TaskResultSlot. COPY uses canonical RequestScope; TAIL uses owned canonical driver; HASH/GREP direct File.

RUNTIME_FACADE_SDK_CONSUMER_CPP_FILES=2, outside its defining application_runtime.cpp: src/async/file.cpp (four completed-return File adapters) and src/async/await_op_helpers.cpp (await_take/drain/read_once/read_fill/write_exact). RUNTIME_FACADE_SDK_CONSUMER_TEMPLATE_HEADERS=1: include/sluice/async/task_result.hpp, run_task_to_result constructs RuntimeBuilder, owns ApplicationRuntime, submits RuntimeTaskContext callback, waits TaskResultSlot, shuts down. The bare TaskResultSlot template itself uses mutex/CV/optional only, but all actual in-tree instantiations belong to runtime tests; no independent generic slot production consumer was found. Includes, friend declarations and negative incomplete-type probes are not consumers.

RUNTIME_SEMANTIC_TEST_TUS=9 and TEST_TARGETS=9 (same basename):
- tests/file_read_test.cpp → file_read_test
- tests/file_write_test.cpp → file_write_test
- tests/file_sync_data_test.cpp → file_sync_data_test
- tests/file_sync_all_test.cpp → file_sync_all_test
- tests/explicit_file_ref_test.cpp → explicit_file_ref_test
- tests/file_access_precedence_test.cpp → file_access_precedence_test
- tests/uring_backend_smoke_test.cpp → uring_backend_smoke_test (liburing=y only)
- tests/runtime_waiter_observer_test.cpp → runtime_waiter_observer_test (own full async source/seam build, not production library link)
- tests/app_copy_pipeline_oracle_test.cpp → app_copy_pipeline_oracle_test. IMPORTANT: TwoWriteTask and SlotReuseWriteTask still invoke run_task_to_result to self-test scripted-backend peak-borrow and slot reuse accounting, even though production COPY no longer uses runtime. Preserve those independent observations while moving support self-tests to canonical Requests/owned driver.

The other eight exercise actual runtime submission/await/result/teardown or waiter publication/cancellation, rather than include-only closure. Tests/direct_w01_consumer_probe.cpp, tests/request_core_consumer_probe.cpp and tests/package/consumers/w01_direct.cpp merely assert excluded incomplete types. tests/package/consumers/w02_pipeline.cpp mentions runtime in a comment. tests/progress_source_ownership_test.cpp and tests/async_sync_admission_test.cpp include async/file.hpp but do not invoke completed-return File wrappers. std::future in TAIL oracle and application runtime is not sluice::async::Future.

## Family dependency boundary

ApplicationRuntime implementation constructs Scheduler and Group; Group is the actual owner of evented Fiber tasks and Future<void>, EventedWaitPolicy, and default threaded wait policy. Future/WaitPolicy have real executable/header-template uses through group.cpp/group.hpp/future.hpp/evented_wait_policy.hpp, not independent production app consumers. Scheduler is also the implementation owner of the legacy synchronization/select family: scheduler.cpp, scheduler_park_wake.cpp, scheduler_timer.cpp, scheduler_event.cpp, scheduler_semaphore.cpp, scheduler_mutex.cpp, scheduler_condition.cpp, scheduler_rwlock.cpp, scheduler_queue.cpp, select.cpp, select_event.cpp, select_timer.cpp and queue_port.cpp. Treat this as a separate F4 closure; leaf runtime facade retirement does not require deleting it simultaneously.

Completion is STILL physically used by canonical AsyncBackend virtual submit signatures (nullable Completion pointers), AsyncIoContext compatibility overloads, ThreadPoolBackend publication/cancel, UringBackend compatibility delivery and substantial backend/protocol tests. Canonical submission can use nullptr, but this does not remove compile/type and delivery dependencies. Completion removal is an independent backend/context/test API contraction, never justified by zero app runtime use.

Fiber and fiber_ctx are STILL used directly by optional StackfulIoHost (stackful_io_host.hpp embeds Fiber in TaskSlot; stackful_io_host.cpp reconstructs Fiber and reads FiberState). StackfulIoHost has no direct semantic adapter use of ApplicationRuntime, RuntimeBuilder, RuntimeTaskContext, Scheduler, Group/Future/WaitPolicy, or Completion. Its current installed compile closure DOES depend transitively on Completion: stackful_io_host.hpp includes async_io_context.hpp, which includes completion.hpp. Removing Completion therefore requires context/backend header contraction even for this narrow host; no direct use is not compile independence. Preserve Fiber/fiber_ctx, cancel and fail_fast substrate until separately re-owned. W-04 supported status remains NO; reachability is not support evidence. No canonical File/RequestCore backend requires Scheduler task execution.

Legacy File adapters and await helpers are part of the legacy runtime facade; old Reader/Writer/stream/Batch/task-pool consumers belong to F4 and have separate scope/package gates. Do not infer those closures are removable from this nine-TU audit.

## Actionable physical deletion entry

Smallest coherent File-wrapper source+installed API unit: retire src/async/file.cpp and include/sluice/async/file.hpp after migrating retained wrapper observations in file_read/write/sync_data/sync_all/access_precedence and explicit_file_ref tests to canonical File/Request semantics; uring_backend_smoke uses runtime context and must be checked for indirect wrapper inclusion. Remove include-only dependency in async_sync_admission/progress_source_ownership tests. Archive delta: file.cpp.o absent from libsluice_async.a. Source-only deletion while shipping callable wrapper declarations would leave an unusable installed API; it is not a legitimate closure.

Minimum coherent runtime facade+lifetime batch after all nine semantic test consumers migrate/re-own:
Production source deletion: src/async/application_runtime.cpp, src/async/file.cpp, src/async/await_op_helpers.cpp, src/async/group.cpp, src/async/wait_policy.cpp.
Installed/source header deletion: include/sluice/async/application_runtime.hpp, file.hpp, await_op_helpers.hpp, task_result.hpp, group.hpp, future.hpp, wait_policy.hpp, evented_wait_policy.hpp.
Expected archive-member removals: application_runtime.cpp.o, file.cpp.o, await_op_helpers.cpp.o, group.cpp.o, wait_policy.cpp.o (verify toolchain member spelling from actual archive; these are source-to-object identities).
Test paths above are migration/re-ownership work, not blanket deletions. runtime_waiter_observer_test may retire runtime-specific cases only when their relevant OBS/publication/cancel obligations remain covered by canonical controlled regressions. Update xmake/tests.lua declarations/own-source closure and xmake/libraries.lua install/source policy as needed; current globs automatically consume whatever remains, so declaration changes alone cannot prove physical absence.

Retain Scheduler/sync/select closure for separate scoped F4 audit; retain Completion backend/context bridge; retain Fiber/fiber_ctx/StackfulIoHost substrate and canonical File/Request/RequestScope/ProgressSource/observer/request-core/context-identity/backends. Remove Scheduler ApplicationRuntime forward/friend declaration as cleanup only; it is not a consumer blocker. Any stale async_io_context comment is not a dependency.

Mandatory entry conditions: explicit phase-owner authorization resolving F0 §1.7 external-use/deprecation risk for named surfaces (currently PENDING), package/profile F5 closure (PENDING), retained in-tree tests migrated or justified retired, formal/docs obligations mapped, and B-01 new versioned package freeze with clean installed-prefix consumer/negative-header/archive evidence in both liburing profiles. Historical B-01/F1 manifests must remain immutable. Unknown external use is unknown, never zero. A risk-approved narrow wrapper batch may precede full facade retirement; current #475 HOLD/KEEP policy still blocks either physical uninstallation until scoped conditions are resolved.
