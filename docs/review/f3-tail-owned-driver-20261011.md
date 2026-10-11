# F3 Step 4 — TAIL app-owned driver (2026-10-11)

Candidate scope: #402/#474 T2-08 and OB-3; canonical root v1-r4.
Requirements: W-01, PROD-03, ARCH-02, INV-01, SEM-02..05, ERR-01,
LIFE-01, THREAD-01, HOST-01, CANCEL-01/02, VERIFY-01, MIG-01/02.
The adopted D-H4 ruling is implemented through the existing Result<TailResult>;
no new public settlement API is introduced.

Baseline: 5833f525b5b271d6a62547c30e4d7cd5499cce41.
Production/test SHA: 143e8d4525877eb8dabdd49d8054ff8d5b239acc.
This follow-up changes documentation and removes stale test comments only.
Rollback unit is the independent TAIL branch; COPY does not depend on it.

## Workload and responsibility

One engine owns one File, one bounded scan buffer and one execution thread.
The driver invokes canonical blocking File operations directly. ApplicationRuntime,
RuntimeTaskContext, Scheduler, TaskResultSlot and Completion are absent from this
application's execution and resolved link closure. No canonical async request is
accepted in this execution choice; direct invocation holds its physical File and
buffer borrow until the syscall returns.

The lifecycle is: validate options and construct task storage/callback copies;
create the driver under the state mutex; commit started and thread identity;
perform direct reads and callbacks; select TailResult; external wait joins;
consume result once; destruction requests stop and joins before destroying File,
task buffer or shared state. Start failure returns an outer Result error and
leaves the engine retryable. Task/read/callback errors appear inside TailResult.
Join errors return the outer Result error with the OS code. A repeated or
competing wait returns invalid_state, as does self-wait.

The mutex serializes lifecycle admission and stop predicate. The same mutex
protects condition-variable enrollment, so stop before park cannot be lost.
Notifications wake the EOF polling wait immediately; there is no 50 ms rescue
loop. While a syscall or output callback actually blocks, stop records intent
but cannot interrupt that operation. Join and destruction wait for its physical
return. No universal wall-clock stop bound is claimed for an arbitrary filesystem
or callback. The held-pread oracle proves no settlement/File close precedes release;
the held-callback oracle proves no destruction precedes callback return.
Application callers must finish concurrent external calls before destruction;
callbacks cannot destroy their own engine.

## Mechanism justification and contraction

| Mechanism | Owner / invariant / classification | Why materialized |
|---|---|---|
| driver thread and join | Impl; physical lifetime obligation | Running code can retain File/buffer/callback borrows independently of stop state |
| mutex | DriverState; physical synchronization obligation | start, stop and wait may legally race |
| condition variable | DriverState; optional host internal wake | EOF poll must yield CPU and stop must wake without waiting full poll interval |
| stop | DriverState; application authority | Persistent stop intent cannot be inferred from terminal result or started |
| started | DriverState; compatibility state | Failed start must be retryable; pre-start stop has no effect |
| waited | DriverState; compatibility state | Exactly one wait owns join/result; competing and repeated wait reject |
| driver_id | DriverState; derived diagnostic summary | Checks self-wait under mutex without racing join access to std::thread |
| result | Impl; task outcome authority | Published by thread completion/join; no second request authority |

The runtime's result slot, task context, scheduler/fiber, backend ownership and
completion adapters no longer protect this consumer. Their repo/install retirement
requires separate consumers/package audit and authorization. File, stop state,
thread ownership and join remain necessary physical obligations. Public include
and install contracts are unchanged. workers=1..64 remains a validated compatibility
option without creating extra threads, per adopted D-H2/D-H5.

## Independent observations

Consumption: 9 cases; lifecycle: 12 cases; CLI: 11 cases. Existing scan,
last-N offsets, same-descriptor follow, truncation, bounded long-line retention,
partial-line withholding and error observations remain. Added observations include
start-copy exception rollback/retry; stop from initial callback; self-wait rejection;
competing waits; callback exception cleanup; held syscall return before retirement;
active destructor File retention; held callback destruction; 5000 ms EOF-poll
interruption. CLI subprocesses exercise SIGINT/SIGTERM, double signal and held-read
signal settlement. Controlled gates establish unfavorable orderings; negative
windows only observe forbidden early completion, never manufacture progress.

## Remote verification

Host: jnhu@192.168.31.75, Linux 7.2.9-200.fc44.x86_64, GCC 16.2.1,
Clang 22.1.8, liburing 2.13, xmake 3.0.9+20260621.
Raw artifacts: /home/jnhu/f3-tail-matrix-20261011/evidence.
Committed capture: [evidence](f3-tail-evidence-20261011).
Reproducer: [matrix script](../../scripts/verify_f3_tail_owned_driver.sh).

Four completely separate source/profile roots ran Debug/Release ×
--liburing=n/y. Each ran consumption/lifecycle/CLI, direct W-01 probe,
blocking File read, semantic short-I/O reference and ThreadPool E1 conformance;
the two liburing profiles additionally ran real Uring E1 conformance.
30 target executions passed. TAIL links only sluice_core in either profile;
liburing configuration is backend evidence, not a claim TAIL executes via io_uring.
Debug executable and all four profiles' unstripped application object symbols
contain no legacy execution symbols. Actual verbose application link commands
contain sluice_core and no sluice_async. Release executables are stripped;
their empty nm output is not used as contraction evidence.

Additional isolated Clang profiles used --sanitizers=asan,ubsan and
--sanitizers=tsan. All three app targets passed under both (six runs).
Verbose compile/link captures show actual -fsanitize flags; binary symbol probes
confirm static compiler-rt __asan_init / __ubsan_handle_* and __tsan_init.
ASAN_OPTIONS=detect_leaks=1, UBSAN_OPTIONS=halt_on_error=1.
GCC sanitizer exploratory output is excluded; it is not passing evidence.
No thread-creation/join failure fault injection or exhaustive OOM proof is claimed.

Source review at production SHA: APPROVE, no actionable P1/P2 findings.
The receipt in evidence explicitly requires final evidence/docs review; the
coordinator owns binding that independent review to the final branch SHA.
Final prior head `64058ca9` received technical APPROVE; owner review #5483212164
is GitHub COMMENTED with an explicit conditional approval after COPY adoption
and combined verification. The four combined profiles and final narrow independent technical APPROVE are
complete; PR #497 is adopted at merge
0d365624661b2b6569dd4bfa68fe095f8cd59ece under explicit user owner merge
authorization. The GitHub review state remains COMMENTED.
OPTIONAL_W04_SUPPORTED=NO; V1_RELEASE_READY=NO.

## Integration and compatibility debt

COPY #496 is adopted at `3530890b2147db38daa9b5813de002c158343090`;
this integration retains both independent ledger records. COPY workers now
configures physical ThreadPool backend workers; its migration note remains.
TAIL workers is currently validated but inert. #474/#475 F3/F5 owns a deliberate
exit decision before the final release boundary: remove/reject the parameter
or define meaningful semantics. No CLI removal is authorized here.
Stop is observed at read/poll boundaries; callbacks for an already parsed batch
may continue until that batch returns. No per-callback stop bound is promised.

Combined integration at `f717e170331df8e098634fe60462b7dd256f8d7b` passed
all four isolated configurations: 72 target executions and four COPY CLI
suites. [Exact raw evidence and reproducer](../evidence/f3-copy-tail-combined-20261011/REPRODUCE.md).
This receipt update is documentation/evidence only. Final narrow independent
technical APPROVE and PR #497 adoption at
0d365624661b2b6569dd4bfa68fe095f8cd59ece close those coordinator gates.
[Postmerge package audit](f3-runtime-postmerge-audit-20261011.md) records the
separate physical retirement entry; installed/runtime removal is still gated.
