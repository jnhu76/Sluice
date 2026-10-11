# F3 #498 remaining test preparation

This bounded test-only preparation starts from master
a9165f0a997f46d65411967100f87a1cb2627b2b. It migrates the two write-borrow
metric selftest phases in `tests/app_copy_pipeline_oracle_test.cpp` onto the
already supported canonical owned-driver surface. It moves no production
authority, removes no installed header or archive member, and does not claim
physical ApplicationRuntime retirement, PR C completion, F5 approval, W-04
support, or release readiness. Preparation began 2026-10-11; the final evidence
and receipt completed 2026-10-12.

## Contract and authority

REQ-01/03/05/06, HANDLE-01/02/03, LIFE-01/02, HOST-02 and PROG-01/02
govern acceptance, settlement, physical borrow retirement and caller-owned
driving. The root remains
[`explicit-io-v1-final-decision.md`](../explicit-io-v1-final-decision.md).
The adopted COPY RequestScope slice and the F3 Part C audit in
[`v1-conformance.md`](../roadmap/v1-conformance.md) establish the current
consumer baseline. The prior audit
[`f3-runtime-postmerge-audit-20261011.md`](f3-runtime-postmerge-audit-20261011.md)
remains dated evidence; this receipt does not expand its deletion authority.

The phase with two disjoint write buffers owns two accepted requests in a
capacity-two RequestScope before waiting for either. The independent controller
must observe peak physical write-borrow occupancy two with no overlap.
The phase with one backend slot uses a capacity-one RequestScope and consumes
the first terminal request before initiating the second. The controller pins
the second operation in staged-but-unreleased state and must still observe one
live borrow. The existing result, pending-empty, protocol and overlap assertions,
controller-held physical retirement, and watchdog remain intact.

RuntimeTaskContext, TaskResultSlot, TwoWriteTask, SlotReuseWriteTask and
run_task_to_result are removed from these two phases. Tickets select the
scope-owned Request; they do not carry terminal authority. A scope explicitly
finishes before its runner publishes its result. Every early error remains
inside the scope lifetime, so previously accepted work is settled. No new
runtime helper, flag, lease or fallback authority is introduced.

## Ownership and concurrency map

| State | Owner and access | Ordering and lifetime |
| --- | --- | --- |
| File and payload arrays | Calling test; runner reads immutable write source | Both outlive runner join and scope/context destruction |
| Context and RequestScope | Runner thread only | Scope owns accepted Requests and existing exclusive ProgressOwner; scope is destroyed before context |
| Backend record, physical borrow metric, staged outcome | Existing ScriptedShared/ScriptedBackend | Existing mutex and backend lifetime gate serialize controller and driver; physical release precedes public publication |
| Result optional | Runner writes; calling test observes after join | Existing release/acquire done flag and thread join publish result; no concurrent mutation |
| Watchdog done flags | Test-only progress observation | Existing release/acquire flags; watchdog diagnoses stalled external completion, never authorizes borrowed storage release |

No production state becomes redundant in this slice. Runtime task closure,
result publication wrapper and per-operation Completion bindings become
unnecessary for these two metric phases only. Backend physical execution refs,
canonical publication, slot-generation identity and borrow-release accounting
remain independent physical obligations. Existing Scheduler wait records and
observer routing are retained for the host tests described below.

## Remaining waiter evidence boundary

All five `tests/runtime_waiter_observer_test.cpp` cases remain unchanged.
OBS-01/02/03/04, REQ-03/05/06, LIFE-01, HANDLE-03 and SHUT-04 own their
canonical obligations, while retained Scheduler/Fiber owns their host routing
and task lifetime. Their existing ApplicationRuntime convenience dependency
must be addressed before physical runtime deletion; none is silently removed
or declared equivalent to a polling-only scope test.

| Existing case | Controlled schedule and required observation | Canonical evidence that covers only the core part | Remaining host obligation |
| --- | --- | --- | --- |
| completion_wake_rides_the_observer_adapter | Worker claim gate holds physical completion until exactly one Scheduler wait record exists; release publishes payload; delivery retires the wait record before join | request_core_protocol_test: delivery_claim_is_exclusive_and_at_most_once; observer_registration_pins_reclaim_until_retirement | Actual queued wake resumes a live Fiber, no host wait record survives drain/join |
| observer_cancel_wakes_the_fiber_and_the_operation_still_completes | Worker claim gate pins operation; registered waiter cancellation wakes Fiber with canceled wait disposition; operation subsequently publishes real bytes and drains | request_core_protocol_test: observer_cancellation_never_cancels_the_operation | Cancellation wakes suspended Fiber and cleanup retains Completion/borrow until physical settlement |
| already_terminal_await_returns_without_suspending | Zero-length submit publishes inline; await returns zero with no host wait record | request_core_protocol_test: attach_after_publication_returns_already_terminal | Scheduler ready edge creates no suspension or wait record |
| wait_record_exhaustion_fails_the_await_before_observer_attachment | Host wait capacity one; two accepted reads; second await reports no_space with no observer attached, accepted_live remains two; after first completion second settles through normal await | Existing core attach/publication tests prove resolution and bounded registration; RequestScope tests prove owned cleanup, not this Scheduler capacity refusal | Reserve-before-attach behavior and retained accepted work after host pool exhaustion; both Fibers settle and all pins reclaim |
| cancel_during_the_staged_delivery_window_completes_normally | Worker park then backend DeliveryClaimedPauseGate pins delivering observer; concurrent cancel completes while pinned and refuses, record remains live; release delivers payload and reclaims | request_core_protocol_test: cancel_during_delivery_is_distinguishable_and_delivery_completes | Scheduler cancel result and live wait record cannot retire while the staged wake still references them |

Direct Scheduler construction could remove the facade dependency while retaining
these exact host oracles, but requires explicit Fiber stack ownership, task
initialization, driver service and shutdown sequencing currently supplied by
ApplicationRuntime. That is a separate tested migration boundary. Replacing it
with a new runtime-shaped test manager would introduce another authority and
would not establish the canonical observer contract by itself. The above core
case names are an evidence ownership map, not new verification results.

## Reproduction and captured results

Remote host: `jnhu@192.168.31.75`, Fedora Clang 22.1.8.
The final isolated captures use `/home/jnhu/.local/bin/xmake`
3.0.9+20260621; the original supplementary captures used `/usr/bin/xmake`
3.0.6+20260117.
All git, editing, builds and runs occur on that remote host.
The authoring checkout is `/home/jnhu/Source/Sluice-f3-498-remaining-tests`.
[Raw logs](../evidence/f3-remaining-test-prep-20261011/) retain the exact
configuration, build and case output. The authoritative `isolated-*.log`
captures use six different detached source worktrees at exact tested commit
`5e1be4232bc59a2e397cc6f6fe0c218beb200917`. Each has its own
source root, `.xmake` configuration and `build/` directory. Compiler caching
is explicitly disabled with `--ccache=n`; the regular profiles are forced
rebuilt after that setting. The worktree paths are
`/home/jnhu/Source/Sluice-f3-498-remaining-isolated-PROFILE`, where PROFILE
is debug-n, debug-y, release-n, release-y, asanubsan-n or tsan-n.

The older unprefixed profile logs share one source/configuration checkout
with separate build directories; they are supplementary evidence and do
not satisfy the isolated-source matrix. The negative controls remain
supplementary causal evidence for the unchanged test bytes.

Four profiles run the full 24-case COPY pipeline oracle, including the real
io_uring content/sync case in enabled profiles:

```sh
git worktree add --detach /home/jnhu/Source/Sluice-f3-498-remaining-isolated-debug-n 5e1be4232bc59a2e397cc6f6fe0c218beb200917
cd /home/jnhu/Source/Sluice-f3-498-remaining-isolated-debug-n
/home/jnhu/.local/bin/xmake f -y -m debug --liburing=n -o build --ccache=n
/home/jnhu/.local/bin/xmake build -r -j 4 app_copy_pipeline_oracle_test
/home/jnhu/.local/bin/xmake run app_copy_pipeline_oracle_test
# Repeat from a different detached source worktree for every other profile.
```

Two temporary support-only negative controls were applied and restored.
Capping physical borrow peak at one fails the two-simultaneous-borrows
assertions. Omitting recycled-slot `rec.borrow_released = false` fails
`slot-reuse staged-unreleased write stays borrow-live`.
Both test binaries exit 1 (xmake wrapper reports 255), and the restored
selftest passes. Their logs describe the precise mutations. No weakened
assertion or mutation remains. Existing controlled exception and canceled-result
settlement cases retain their accepted-held-borrow boundaries.

Sanitizer results and any configuration limitation are recorded in the
isolated-asanubsan-n and isolated-tsan-n logs; only actual instrumented
execution counts as sanitizer evidence. The first generic Clang link fails because xmake's
x86_64-unknown-linux-gnu triple selects a runtime directory absent on Fedora.
The installed runtime is under x86_64-redhat-linux-gnu. The corrective
configuration explicitly selects that linker target and retains the same
sanitizer mode; sanitizer symbols are recorded from the linked binary.


The corrected ASan/UBSan and TSan configurations each pass these three
controlled cases: borrow_live_write_peak_selftest,
exception_cleanup_waits_for_held_borrow, and
canceled_result_settles_held_readahead. The first case now runs with no
ApplicationRuntime/Fiber selftest machinery and is included in TSan evidence.
The latter two retain the canonical adopted production settlement path.
The captured symbols confirm actual sanitizer runtime linkage; no sanitizer
was disabled to work around the initial toolchain triple mismatch.

Neither finite cases nor these sanitizers establish unbounded liveness or
the five retained Scheduler wake/cancellation/teardown obligations. Only the
pipeline test consumer migration is validated by this slice.

## Exact source provenance

The isolated matrix tests commit
5e1be4232bc59a2e397cc6f6fe0c218beb200917. The evidence/receipt correction
commit changes documentation only: include/, src/, apps/, tests/, xmake/ and
xmake.lua are byte-identical to that tested commit. The Git tree/object IDs
for those paths, in that order, are captured in
tested-source-trees.txt. Thus the later documentation head carries the same
tested C++/test/build inputs; the receipt does not claim that newly captured
evidence had already existed in the tested commit.
