# F3 postmerge runtime retirement audit and next physical slice

Status: POSTMERGE_PACKAGE_AUDIT_COMPLETE; retirement execution entry prepared,
PHYSICAL_CONTRACTION_AUTHORIZED=NO. This is a documentation/evidence slice.
Production baseline is master `0d365624661b2b6569dd4bfa68fe095f8cd59ece`.
COPY #496 was adopted at `3530890b2147db38daa9b5813de002c158343090`;
TAIL #497 was adopted at the baseline above. Technical independent APPROVE
and GitHub owner review COMMENTED are distinct records; the user's explicit
owner merge authorization and the actual merge receipts establish adoption.
The prior conditional TAIL review is not retrospectively relabeled APPROVED.

Authority: v1-r4 PROD-01/02/03, ARCH-01/02, HOST-01/02/03, REQ, LIFE,
PROG, SHUT, MIG-01/02 and GOV-02; F0 package policy §1.7; #475.
Latest #475 T2 disposition remains A=BOUNDED_HOLD,
B/C=KEEP_INSTALLED_COMPAT, external risk pending F5. Policy closure does not
authorize physical removal. W-04 OPTIONAL_SUPPORTED=NO and
V1_RELEASE_READY=NO remain unchanged.

## Actual package and link evidence

Two fresh git-archive snapshots of this exact baseline were independently
configured in Debug, liburing=n and y; each has its own project root, .xmake,
build directory and installed prefix. Both library builds, four app builds,
and both core/async installs succeeded. Actual resolved xmake source/object
paths were checked against existing files. Both profiles contain 50 core+async
objects, 37 async archive members and exactly 79 installed headers. These
counts equal the before snapshot at `5833f525b5b271d6a62547c30e4d7cd5499cce41`;
the runtime objects and installed facade have not physically contracted.

GNU ar member lists and defined/undefined demangled nm records are captured
for both archives and profiles. The installed public header compiler dependency
closures contain Request=10, AsyncIoContext=18, StackfulIoHost=23 and
task_result=35 project headers; all four compile in both profiles.
The host closure reaches Fiber/fiber_ctx and Completion through AsyncIoContext.
Request reaches no legacy runtime facade. task_result still reaches
ApplicationRuntime/Scheduler through its callable runtime convenience.

A fresh installed-prefix consumer constructs ThreadPoolBackend,
AsyncIoContext and StackfulIoHost, links the installed archives and executes
successfully in both profiles. Its linker maps and unstripped nm records show
Fiber/fiber_ctx and host dependencies but no ApplicationRuntime, RuntimeBuilder,
RuntimeTaskContext, Scheduler, Group/Future/WaitPolicy or TaskResultSlot symbols.
The four actual Debug app binaries are unstripped; their full nm records likewise
show no symbols from these legacy runtime families. COPY links core+async;
HASH/GREP/TAIL link core. Source consumers, compiler closure, archive
members, undefined dependencies, app link commands and linked symbols are
combined evidence; a zero text search alone is not retirement proof.

The liburing=y compile log records SLUICE_HAS_LIBURING and the real backend
objects, with system pkg-config liburing 2.13. This audit does not construct a
real ring or repeat behavioral/sanitizer matrices. Their adopted receipts
remain the COPY/TAIL evidence; this new audit concerns actual package closure.

## Family and authority disposition

| Family and concrete identities | Current role | Physical disposition |
| --- | --- | --- |
| File/Request/RequestScope/AsyncIoContext/RequestCore/progress/backends | REQUIRED_CANONICAL, with REQUIRED_INTERNAL_SUBSTRATE for physical/detail carriers | retain canonical operations, observers, ownership, progress and retirement |
| Completion (`include/sluice/async/completion.hpp`, context/backend delivery interfaces) | REQUIRED_INTERNAL_SUBSTRATE; public compatibility arm also COMPATIBILITY_PENDING_EXTERNAL_GATE | retain; nullable canonical submits do not erase compiled signatures or delivery obligations |
| Fiber/fiber_ctx (`src/async/fiber.cpp`, `fiber_ctx.cpp`; installed matching headers) | OPTIONAL_HOST_INTERNAL; installed host layout requires both | retain independent StackfulIoHost task/stack lifetime and switching |
| ApplicationRuntime/RuntimeBuilder/RuntimeTaskContext (`application_runtime.cpp/.hpp`) | LEGACY_PRODUCTION_CONSUMER in SDK adapter closure; COMPATIBILITY_PENDING_EXTERNAL_GATE at installed exposure | candidate only after SDK/test/package gates |
| await_op_helpers (`await_op_helpers.cpp/.hpp`) and completed-return adapters (`src/async/file.cpp`, `include/sluice/async/file.hpp`) | LEGACY_PRODUCTION_CONSUMER through runtime adapters; installed compatibility | migrate/retire named test and public API edges first |
| Group/Future/WaitPolicy (`group.cpp/.hpp`, `future.hpp`, `wait_policy.cpp/.hpp`, `evented_wait_policy.hpp`) | REQUIRED_INTERNAL_SUBSTRATE of the currently retained legacy runtime, installed compatibility | no independent canonical need demonstrated; still required until that runtime closure retires |
| Scheduler (`scheduler*.cpp`, `scheduler.hpp`), synchronization/select/queue closure | REQUIRED_INTERNAL_SUBSTRATE of retained legacy runtime and separate F4 compatibility consumers | outside the first facade slice; perform separate F4 consumer/evidence audit |
| TaskResultSlot / run_task_to_result (`task_result.hpp`) | TEST_ONLY actual remaining instantiations; callable installed convenience is COMPATIBILITY_PENDING_EXTERNAL_GATE | nine runtime test TUs must migrate/re-own; bare mutex/CV slot is not justification for a new public generic framework |
| test seam objects `mutex_test_seam.cpp.o`, `queue_test_seam.cpp.o`, `scheduler_fe2_test_seam.cpp.o` and installed seam headers | TEST_ONLY evidence roles physically present in current archive/package | installed compile obligations remain (mutex.hpp includes mutex_test_seam.hpp unconditionally); separate F4 evidence/internalization gate, even if a seam object has no executable symbols |

RETIRE_ELIGIBLE=0 at this baseline under all F0 §1.7 conditions.
The candidate paths below become eligible only after the named gates close;
the approved consumer migrations do not resolve unknown external use.
The exact 79-header inventory carries a per-path role classification in
[installed-header-classification.json](../evidence/f3-runtime-postmerge-20261011/installed-header-classification.json).
Roles describe the captured closure, not universal historical usefulness.
[Source/object classification](../evidence/f3-runtime-postmerge-20261011/source-object-classification.json)
gives exact resolved sources and objects, including
the retained scheduler family and all test seam objects.

## Concrete next deletion PR entry

[Execution issue #498](https://github.com/jnhu76/Sluice/issues/498) records the
five-source/eight-header facade batch and its nine test migration obligations,
with the narrower File-wrapper unit below available as the first coherent cut.

Smallest coherent source/archive/installed API batch:
`src/async/file.cpp` and `include/sluice/async/file.hpp` together.
Its current archive member is exactly `file.cpp.o`. Source-only deletion
while continuing to install callable declarations leaves a broken API and
is not a valid slice.

Before or together with that production change, migrate the retained
observations in `tests/file_read_test.cpp`, `file_write_test.cpp`,
`file_sync_data_test.cpp`, `file_sync_all_test.cpp`,
`file_access_precedence_test.cpp` and `explicit_file_ref_test.cpp`
to canonical File/Request paths. Preserve admission/access precedence,
completed-return result/error, composition and durability observations;
do not reproduce the private wrapper implementation.
Remove include-only wrapper dependencies in
`tests/async_sync_admission_test.cpp` and
`tests/progress_source_ownership_test.cpp`; inspect
`tests/uring_backend_smoke_test.cpp` for its indirect wrapper closure.
Update their `xmake/tests.lua` source/dependency declarations when needed.
The library/header globs in `xmake/libraries.lua` must match the resulting
supported tree; resolved build and installed-prefix absence are the oracle.

Owner: #474/#402 F3 implementation owner, with #475/F5 package/external
risk owner. Entry requires explicit per-surface deprecation/no-equivalent or
canonical migration disclosure and owner acceptance of UNKNOWN_EXTERNAL_USE,
or a bounded deprecation window, under F0 §1.7. Retained evidence and
formal/docs claims must be mapped to their new owners. #475 does not enumerate async/file.hpp among its exact fifteen surfaces;
its HOLD/KEEP policy applies to the retained future/wait/Scheduler closure.
The broader F0 §1.7 compatibility gate still applies to the File wrapper,
so this entry is concrete planning, not an authorized physical patch. Verify both backends' affected production oracles and clean
installed consumers, then publish a new B-01 versioned package cut showing
`file.cpp.o` and `sluice/async/file.hpp` absent, retained canonical/host closure
complete, and the exact public/source incompatibility accepted.

The next facade batch is five sources:
`src/async/application_runtime.cpp`, `file.cpp`,
`await_op_helpers.cpp`, `group.cpp`, `wait_policy.cpp`;
and eight installed/source headers under `include/sluice/async/`:
`application_runtime.hpp`, `file.hpp`, `await_op_helpers.hpp`,
`task_result.hpp`, `group.hpp`, `future.hpp`, `wait_policy.hpp`,
`evented_wait_policy.hpp`. It requires migration/re-ownership of all nine
runtime-semantic test TUs: the six above plus
`uring_backend_smoke_test.cpp`, `runtime_waiter_observer_test.cpp` and
`app_copy_pipeline_oracle_test.cpp`. The latter's TwoWriteTask and
SlotReuseWriteTask still use runtime to independently validate scripted-backend
borrow peak and slot reuse; move those observations to canonical owned Requests.
Runtime-only waiter cases may retire only when OBS/publication/cancel
obligations remain covered by controlled canonical regressions.
This is not blanket test deletion. Preserve Scheduler/sync/select until the
separate F4 closure is approved, and preserve Completion and Fiber/fiber_ctx.

No production authority moves in this audit, and no existing runtime mechanism
is eligible for removal yet. The later facade slice can contract redundant
runtime construction/task-result/group/wait ownership after test migration;
canonical physical completion retirement and optional task/stack reclamation
remain independent obligations.

COPY workers retain actual physical ThreadPool worker meaning and accepted
bounds/default. TAIL workers=1..64 remain validated inert compatibility state;
F3/F5 owns deliberate removal/rejection or meaningful semantics before its
release exit. These are different outstanding CLI obligations.

## Dated audit baseline and reproducibility

This is a dated postmerge package audit, identifier
`F3_POSTMERGE_20261011_0d365624_DEBUG_N_Y`, not a replacement or rewrite of
historical F1 evidence. The package has not changed; a new B-01 versioned
package cut remains a gate for the future authorized deletion implementation.

[Machine evidence and exact reproduction](../evidence/f3-runtime-postmerge-20261011/REPRODUCE.md)
includes both resolved source/object manifests, all installed headers,
ar members, compiler dependency closures, linked runtime-family symbols,
raw SHA-256 manifest and a compressed raw capture with build/install/link
logs, complete defined/undefined nm output and host maps. Independent source
consumer inspection is preserved beside these records. No C++ edit, runtime
deletion, header uninstallation, push or merge is performed by this audit.
