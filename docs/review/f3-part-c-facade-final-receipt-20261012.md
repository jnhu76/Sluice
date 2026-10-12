# Part C production facade retirement candidate receipt

Base master cb977a12b951098d65942f4b4020189c7dad1a5e. Candidate is stacked on
independent #503 waiter migration and #500 COPY preparation. Apps already use
canonical execution; five waiter oracles now directly own retained Scheduler/
Fiber resources. C2 physically removes unused facade implementation and
installed declarations only in its Draft candidate. Root PROD-02/03,
MIG-01/02, HOST-01 and retained REQ/LIFE/OBS/SHUT obligations govern this slice;
v1-r4 target behavior/scope is unchanged.

Production delta: three sources and five headers in the
[exact per-surface decision packet](f3-part-c-facade-risk-decision-20261012.md).
Remove facade forward/friend edge and stale context comment; preserve public
Scheduler tag access/private helper, Future/WaitPolicy/default_wait_policy,
Completion, canonical requests/context/backends and host/Fiber substrate.
No general runtime is introduced. Group threaded behavior, standalone
EventedWaitPolicy purpose, TaskResultSlot and exception conversion are separately
disclosed exclusions; retained apps/host preserve required exception categories.

## Server evidence

All edits/git/builds/runs occur on jnhu@192.168.31.75 in independent roots.
Final complete Debug/Release n suites pass 53/53 targets each; real y suites
pass 67/67 each. Final C1 strengthened assertions are integrated and retested.
This covers controlled waiter races, request/observer delivery, accepted-borrow
RequestScope settlement, Fiber/record teardown, bounded COPY, TAIL stop/wait,
HASH/GREP direct and host/backend tests. No sleep/retry inflation or weakened
assertion. The initial malformed-friend edit failed four builds; original and
corrected-input reruns are preserved separately.

Two independent Clang22 y roots run runtime_waiter_observer_test,
request_core_protocol_test, request_scope_test, stackful_host_test,
app_copy_pipeline_oracle_test, app_tail_lifecycle_oracle_test. ASan/UBSan and
TSan pass all six targets with instrumented argv/runtime symbols.
The known static TSan/RequestScope allocation-override conflict is not PASS:
this campaign uses the proven shared runtime with --target=x86_64-redhat-linux-gnu,
-shared-libsan and runtime rpath, then executes full assertions. No sanitizer
was disabled. [Raw matrix logs/source/config manifest](../evidence/f3-part-c-c2-matrix-20261012/).

[Fresh versioned B-01 v4 candidate](../evidence/f3-part-c-b01-v4-candidate-20261012/REPRODUCE.md)
captures exact code 1d0c368f and master with independent n/y build/install,
objects/archive/symbols, all installed probes and package hashes:
49/36/78 -> 46/33/73 in both profiles. Retained compiler dependencies match;
Completion submit/result/reset, default Future/WaitPolicy, Scheduler/Fiber/host
remain linkable/runnable. Real ring executes in y; n is UNAVAILABLE.
Final docs/dependency synchronization leave captured code/build inputs identical.

## Contraction and remaining authority

Legacy facade construction/admission/lifecycle counters, driver epochs, Group
task/future/stack ownership and convenience result mailbox disappear with their
source/header owners, and are not recreated. Request terminal authority,
backend physical work, observer delivery pins, Scheduler records and Fiber
stacks remain independent physical obligations and retain actual evidence.
Public execution-tag state and #475 Scheduler/Future compatibility stay outside
this exact surface request.

All five host obligations remain executable under #503. Historical model/audit
reports remain dated evidence; they are not new proof of this candidate.
No historical F1/B-01 assets are changed. Rollback C2 alone restores paired
files/facade friend edge; C0/C1 remain usable migrations. Partial approval
requires KEEP restoration and a new scoped package capture.

Independent final exact-HEAD review is pending. F0 per-surface breakage and F5
package authorization/adoption are PENDING. #502 covers none of these surfaces.
#475 fifteen retained surfaces and Future/WaitPolicy KEEP are unchanged.
No merge is authorized. Master-only GitHub CI does not run on stacked bases;
server matrix is separate evidence. Master remains 49/36/78. Candidate
contraction is verified Draft evidence, not adopted F3/F5 retirement.
F3_FULL_RETIREMENT=NO; OPTIONAL_W04_SUPPORTED=NO; V1_RELEASE_READY=NO.
