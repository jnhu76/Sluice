# F3 COPY-B canonical owned pipeline

This is narrow implementation/evidence for COPY at production baseline
`5c6bc4f97a9362a0e2f6eb7765b3e084aafa72f7` and final production/test baseline
`c94c7770335e4e01e4f1f5876740cb0e792a4f88`, derived from root v1-r4
REQ-01..06, HANDLE-01, LIFE-01/02, HOST-02/03, PROG-01/02/04 and SHUT-01..04.
Adopted M-H D-H3/D-H5, T2-08, OB-1 and OB-2 remain the app obligations.
Implementation owner is F3/#474 (family arm #402). No root behavior is amended.

## Current-to-target responsibility

Previously one ApplicationRuntime task used RuntimeTaskContext and per-slot read
and write Completion bindings, plus TaskResultSlot and legacy await helpers.
The current calling thread runs the pipeline and owns one AsyncIoContext. A
RequestScope owns each accepted Request and the sole result binding, reserving
its preallocated tracking capacity before acceptance. Slot tickets are lookup
identities and never own settlement. The backend/core owns physical execution,
immutable terminal choice and publication. The scope owns progress while alive;
shutdown on the same thread follows scope destruction.

At most one accepted operation belongs to each app buffer slot. Tracking capacity
is depth; read-ahead still submits depth reads initially, and the ordered writer
uses the consumed read's freed tracking slot. All reads are consumed before sync
submission. `finish()` closes the scope permanently, so successful finish follows
sync rather than preceding it. Failed chunk processing explicitly finishes and
discards read-ahead results, preserving the primary error. Sync rejection/error
unwinds through scope cleanup. Exception translation occurs after the scope has
settled; no task result is published while buffers remain borrowed.

## Lifetime and synchronization

```text
caller File / destination native descriptor owners
  -> PipelinedCopyTask -> fixed PipelineSlot buffers
    -> AsyncIoContext -> backend/core/progress resources
      -> RequestScope -> owned Request binding
        -> ScopeTicket identity in app slot
        -> backend borrow touch -> terminal publication/acquire -> result take
      <- scope settle/destruct, all result bindings released
    <- explicit same-thread shutdown + context destruction
  <- buffer destruction
<- caller may close Files/native resources
```

Buffer data is exclusively backend-owned from acceptance through acquired public
terminal. App cursors/state/tickets and stats are confined to the calling driver
thread. Backend/core publication supplies the synchronization for read data.
The scripted oracle controller uses its existing mutex/gate/core handoff; record
staging does not imply borrow retirement. The exception regression gates a staged
read, triggers a later write-submit exception, observes cleanup's cancel attempt,
then verifies the app has not returned and the borrow remains live. The throwing
write submission is rejected before its own acceptance; the already accepted
read is the outstanding borrowed operation. This does not claim a post-acceptance
backend exception. Releasing the
held physical operation permits publication, scope settlement and error return.
Cancellation alone is never used as a retirement witness.

## Worker compatibility decision

Before this slice `--workers` selected Scheduler worker threads (default one,
range 1..64), while the ThreadPoolBackend was constructed with its default four
I/O workers. After this slice it selects ThreadPoolConfig.worker_count, retaining
default one and the accepted range. Help and README name I/O backend workers.
This is an explicit D-H5 decision: configured workers now bound actual I/O
execution; no Scheduler is retained merely to carry a numeric option.
The injected-backend entry point validates the same compatibility parameter;
the supplied backend owns its physical configuration. Buffer/depth/worker bounds
are checked before default backend construction. Request capacity stays 64.

## Mechanism justification and contraction

| Mechanism | Fact/authority and reason | Classification / derivation | Replacement and removal evidence |
| --- | --- | --- | --- |
| One ScopeTicket per slot | Identity of the scope-owned accepted request; REQ-01/HOST-02 | Derived lookup summary, materialized to select the request to wait/take | Replaces two Completion objects; controlled buffer reuse/order and settlement oracles |
| RequestScope capacity=depth | Maximum retained app requests, one per buffer slot; HOST-02 | Physical tracking obligation, preallocated before acceptance | Replaces per-Completion application settlement; exception/retirement regression |
| Scope cancel-then-drain policy | Explicit exceptional cleanup with live borrowed read-ahead; HOST-02 | Existing canonical physical lifetime obligation | Replaces runtime teardown dependence; held-borrow cancellation discriminator |
| Private config predicate | Shared legal app bounds before backend or buffer allocation | Derived validation, no new runtime state | Existing validation consolidated; invalid-argument oracle |
| Test throw flag/cancel count | Controlled exception injection and observed cleanup entry | Test-only evidence under existing shared mutex | None; needed to distinguish premature return from real retirement |

Existing slot state, offsets, filled/written cursors, EOF summary and primary error
remain app algorithm facts. They cannot disappear with settlement authority because
the scope does not know chunk order, write suffixes or the app's EOF policy.
TaskResultSlot, task closure, runtime workers and read/write Completion bindings
are absent from COPY production. SDK ApplicationRuntime/Scheduler/Completion and
legacy helper files remain for other consumers and TEST_ONLY harness cases.
The two legacy Runtime self-test phases in `borrow_live_write_peak_selftest` remain
harness evidence and are not COPY production consumers. No installed-header or
archive contraction is claimed by this slice.

## Evidence and limitations

Raw profile logs are in `docs/evidence/f3-copy-request-scope/`, with exact baseline,
compiler/kernel, configure/build/run commands and captured case results. Every
profile uses a distinct detached remote worktree and xmake configuration/build tree.
Debug/Release run all 24 COPY pipeline oracle cases, consumption and DirFsync suites in
noliburing and liburing profiles. The liburing pipeline case constructs an actual
UringAsyncBackend and verifies byte identity and sync-all. ASan/UBSan runs the same
three suites; TSan runs controlled new lifetime/cancel cases plus read-window and
primary-error drain cases without the legacy stackful harness self-test.

These tests cover finite executions, not a general liveness proof. Unresolved
health with accepted borrows follows canonical scope/context fail-fast behavior;
this slice adds neither a fabricated terminal nor a hidden polling fallback.
D-H3's excluded unreachable legacy waiter-failure precedence is not frozen or
newly promised. Wait control is acknowledged and retried through canonical
RequestScope.wait_for; no public cancel API or optional host support is added.

safe_output.cpp and the shared POSIX retry implementation are unchanged; OB-2
DirFsync EINTR/error evidence is rerun. CLI subprocess boundary gaps, including
D-H2's remaining pre-side-effect config validation, remain open. The default
backend worker count changes intentionally as recorded above. This record does
not close F3 family retirement, TAIL, W-04 support, F5 installed closure or #475.


The final rerun logs retain the same C++ baseline and confirm all six profiles;
no production code changed after matrix capture. Every xmake invocation runs
from its own profile worktree as well as supplying explicit `-P` and `-o`;
`.xmake` state is never shared across COPY profiles or with TAIL. Clang 22.1.8
ASan/UBSan and TSan instrumentation is recorded from configuration and linked
symbols in `sanitizer-instrumentation.txt`. GCC sanitizer construction failed
on missing `/usr/lib64/libasan.so.8.0.0`; this environment limitation was resolved
for evidence by using Clang, without changing production code.

The six successful CLI scenarios and two genuine RLIMIT_FSIZE failure scenarios
are reproducible with `cli_smoke.py BINARY`. On all four Debug/Release profiles,
SIGXFSZ ignored plus a 64-byte output limit causes an actual write failure:
atomic mode leaves the previous 20-byte destination intact and removes temporary
output; no-atomic mode retains the exact 64-byte source prefix. These are actual
subprocess effects, not a scripted backend prediction. The default CLI backend
remains ThreadPoolBackend; real-ring execution is separately distinguished by
the `real_uring_pipeline_content_and_sync` oracle in both liburing profiles.
Independent final-SHA review remains pending; no merge or physical runtime
retirement is claimed.
