# F3 Step 2 — GREP default direct execution, D-H1/D-H2

Scope: #402/#474; base master 0d580498e8485d44166dac3e1148efb7adf52770
(#492 merged). Root v1-r4 PROD-03, W-01, ARCH-02, INV-01, SEM-02..05,
ERR-01, LIFE-01, CANCEL-01/02, VERIFY-01 and MIG-01/02; adopted D-H1,
D-H2 and D-H5 in the Phase M terminal record. This slice does not close F3,
W-04, F2 or final runtime retirement.

Production owner: apps/sluice-grep. Tested production head:
11dbc1b4386534ec8c1d5f7d3a90df99367f6135. Supplementary test head:
7a997129f82d55709ccf76b502ad442440bebd15 (only expands sink exception
expectations; production/build inputs identical to the four-profile head).
Evidence files are in [f3-grep-evidence-20261011](f3-grep-evidence-20261011).

## Current-to-target change

Before: grep_files unconditionally constructed ThreadPoolBackend and called
run_task_to_result, creating ApplicationRuntime and using RuntimeTaskContext,
Completion and await_read_at for each file. One task scanned files sequentially,
irrespective of runtime worker count.

After: valid default workers=1 allocates the existing reusable read buffer and
app scan state, calls canonical blocking::read_at, and returns completed results
on the caller thread. No runtime, backend, Completion, RequestCore or new progress
owner is constructed. One scan_one template owns the unchanged LineMatcher,
events, offset, carry/EOF/error processing for both reader forms. matcher.cpp
is unchanged. The template is app-private, not a public GREP framework.

workers=2..64 still select ThreadPoolBackend/run_task_to_result.
grep_files_with_backend always selects that legacy engine even at workers=1.
Its cancellation checks remain between files and before each await; each
Completion lives through await_read_at consumption/reset. The reusable buffer
and File owners remain alive through accepted work settlement and runtime join.
No async/core/backend protocol or synchronization primitive changed.

D-H1 now emits the filename separator whenever a filename prefix is present:

| Operands | No -n | With -n |
|---|---|---|
| One | matched line | 12:matched line |
| Multiple | file.txt:matched line | file.txt:12:matched line |

The prefix counts all CLI file operands, including failed opens.
D-H2 shares constexpr valid_config between CLI and both engine entry forms:
buffer 4096..67108864, retained-line cap 1..67108864, workers 1..64.
Decimal parsing separately rejects malformed, signed, zero and overflowing
integers before conversion. Range validation occurs before main opens operands.
Exit codes remain 0 match / 1 no match / 2 error or usage. This fixes adopted
defects, without adding GNU grep compatibility.

## Semantics and ownership audit

Preserved: byte-oriented literal matching, empty pattern/empty file distinction,
LF-only separation, CRLF bytes, NUL/invalid UTF-8 passthrough, nonempty EOF tail,
cross-buffer assembly, skipped over-cap lines and numbering, file/sink order,
per-file read/open error isolation, already emitted stdout and Result/error
representation. Read failure does not call matcher.finish on partial carry.
Canonical closed File yields invalid_state under SEM-03 in both readers.
File RAII close-error reporting remains the existing best-effort policy.

The common scan state is caller-owned in direct mode and owned by the joined
runtime task in compatibility mode. No new shared mutable state or cross-thread
publication mechanism is introduced. Legacy terminal/publication authority
remains in the existing await/runtime adapter. The read completes before
matcher/sink use; the close-from-sink regression closes only after settlement.
Pointers into moved vector storage remain valid under its standard allocator.

Mechanism justification: no new runtime state, guard, flag, counter, token,
lease or capability probe. Existing buffer/offset/matcher/events/results are
physical storage and app processing state retained from the workload.
valid_config derives accepted bounds; reader selection derives the explicit
workers/backend invocation and introduces no independent authority.

Authority-contraction audit: default runtime execution is removed. The legacy
Completion, cancellation token, await/runtime task slot, backend and scheduler
remain necessary for the explicit compatibility branches; none is eligible for
global removal in this slice. No production API, SDK header, library TU or
runtime mechanism is deleted. Exit owner #474/#402 must decide HASH/GREP worker
and injected-backend compatibility, migrate affected T2-08 consumers and reprove
settlement/lifetime/package obligations before physical contraction.

## Server evidence

All C++ edits, builds, tests and commits ran on jnhu@192.168.31.75, in independently
created worktrees. Original /home/jnhu/Source/Sluice stayed clean at 7f5eed6a,
on docs/489-f-w2-1-dual-profile-refreeze. No original branch/working file changed.

Environment: Fedora kernel 7.2.9-200.fc44.x86_64, GCC 16.2.1 20260819,
xmake 3.0.6+20260117, liburing 2.13; repository on btrfs, fixtures on /tmp tmpfs.

| Profile | Result |
|---|---|
| Debug × noliburing | PASS |
| Release × noliburing | PASS |
| Debug × real liburing | PASS |
| Release × real liburing | PASS |

Each profile used its own detached worktree/config/build directory; explicit
--liburing=n/y, no mixed cache/profile. Targets actually built/run:
sluice-grep, app_grep_consumption_test (9 named tests),
direct_w01_consumer_probe, blocking_file_read_test, blocking_file_state_test,
runtime_waiter_observer_test (5 tests). The liburing profiles also passed
uring_public_consumer_probe and all 8 uring_backend_smoke_test cases on the
real liburing/kernel path. The GREP injected-consumption cases use ThreadPool;
the separate smoke supplies real-ring mechanism evidence, not a claim that
default GREP chooses io_uring.

CLI oracle: 47 behavior checks, plus 4 process trace checks per profile.
Baseline failed 8 observations: 5 prefix mismatches and 3 invalid-range FIFO
timeouts. New CLI returned 2 without opening FIFO; openat trace independently
confirmed it. Integer/overflow/bounds, valid minima/maxima, multiple operands,
empty/no-match, CRLF/binary/EOF, overlong lines and cross-buffer output passed.

Consumption: independent fixed byte/line expectations at read buffer sizes
4096, 4097 and 8192 through direct, workers=2/64 and injected ThreadPool.
Partial-output close/read failure preserves the first match and continues to
the next file. Engine invalid configs do not call sink. Controlled canceled
submission proves rejection/error routing, not an in-flight cancellation
schedule. Controlled runtime_error and bad_alloc from sink return backend_error
and no_space on all four invocation forms. They do not prove exhaustive OOM
recovery: allocation while constructing/filling an error vector can still fail,
as in the existing engine. No no-throw app API is claimed.

Thread evidence: baseline default trace contains clone creation. All four
candidate workers=1 traces contain no clone/clone3; workers=2 and workers=64
traces contain successful creation. Logged clone_calls counts clone-bearing
trace lines, including unfinished/resumed fragments; it is not an exact thread
cardinality claim. Raw traces are retained. The review correction adds a
success-only predicate: completed or resumed clone/clone3 returns must contain
a positive child PID. Failed attempts and unfinished fragments do not satisfy
the legacy creation check. Default still requires zero clone-bearing lines. This proves the observed process
execution distinction instead of relying only on the source branch.

GCC ASan/UBSan attempt: UNVERIFIED/UNAVAILABLE at link, missing
/usr/lib64/libasan.so.8.0.0. Clang runtime lookup also did not locate its ASan
archive; no Clang full-app pass is claimed. TSan and new formal models NOT_RUN:
no shared-state/core synchronization transition changed. The existing runtime
waiter/observer cases were rerun in each profile; that is scoped compatibility
evidence, not a full new proof of all cancellation interleavings.

Reproduction: run the committed run-matrix.sh on the server (it creates four
detached worktrees at the invoking branch HEAD). Each invocation allocates fresh
worktree and evidence roots with mktemp, prints their paths, and retains them;
it neither reuses nor deletes existing worktrees or overwrites earlier logs.
Defaults can be relocated with SLUICE_GREP_REPO, SLUICE_GREP_WORKTREE_PARENT
and SLUICE_GREP_EVIDENCE_ROOT. Logs remain below
/home/jnhu/Source/f3-grep-evidence. For baseline discriminator, build sluice-grep
at the base SHA in an isolated worktree, then invoke the candidate's
tests/grep_cli_oracle.py against that baseline binary. Script timeouts only
detect forbidden FIFO blocking; they do not make a protocol test pass.
The supplementary allocation test was rebuilt/run with the CLI oracle in all
four existing profiles after the production/build tree was verified unchanged.

## Package/build impact

GREP_DEFAULT_RUNTIME_CONSUMER_REMOVED = YES
GREP_LEGACY_CONSUMER_REMAINING = grep_files(workers=2..64);
grep_files_with_backend(workers=1..64, explicit AsyncBackend);
run_grep_engine -> run_task_to_result -> ApplicationRuntime/RuntimeTaskContext;
scan_one legacy reader -> await_read_at/Completion;
default legacy backend -> ThreadPoolBackend; existing Scheduler/Fiber closure.

SDK inputs (include/, src/, root build and library/helper build definitions)
are unchanged. xmake/apps.lua only updates its GREP description; target sources,
dependencies and installation rules are unchanged. sluice_async remains linked.

Limited package-equivalence evidence, not a replacement B-01 freeze:
Debug noliburing core/async archive bytes, member lists and defined symbols
match baseline exactly (13 core + 37 async objects). Debug liburing member lists
and defined symbol lists match; archive byte equality is not asserted across
distinct debug-info source paths. Binary NEEDED sets match in both profiles.
Project include closure adds blocking/file.hpp (40 to 41 library headers);
legacy transitive closure remains because compatibility still compiles.
Installed 79 headers match their unchanged source bytes and aggregate digest.
One compact package-equivalence.json captures these facts and limits.
package_compare.py reproduces both components on the recorded server. Its
--repo/--evidence/--liburing-baseline/--liburing-candidate arguments allow
relocation. To recreate the noliburing byte comparison, build the base CLI in
one isolated comparison worktree with -m debug --liburing=n -o build-baseline,
copy build-baseline/linux/x86_64/debug to EVIDENCE/baseline-artifacts, switch
that same comparison worktree to the candidate (keeping the source path
identical), rebuild, then run package_compare.py --repo WORKTREE --evidence
EVIDENCE with the separately built liburing baseline/candidate debug directories.
This preserves debug-info paths for the noliburing byte comparison; the liburing
component deliberately compares object/symbol/link sets instead of archive bytes. No frozen
F1 files were rewritten and no SDK/build-input delta triggers a new B-01 freeze.

## Independent review and remaining scope

Independent reviewer inspected actual production C++ and tests at 11dbc1b4,
returned APPROVE with no blockers, and independently reran Debug noliburing
consumption (9 tests) and the real CLI oracle (47 checks). The final evidence
and supplementary-test head receives a separate narrow review before Ready.

No merge authorization. Next step: HASH_GREP_COMPAT_EXIT. W-04 support,
legacy runtime retirement and F3 family closure remain outstanding.

## Review observations corrected before owner-authorized merge

At the original reviewed head d4f89c26, the same-SHA runner could collide with
its earlier fixed worktree paths and the trace check accepted any clone-bearing
line for legacy creation. Regressions first reproduced both: the runner's second
fixture execution returned 128 for an existing directory; four trace fixtures
failed because failed/unfinished/non-syscall lines were counted and a resumed
return was double-counted.

The corrected runner allocates independent roots and preserves earlier receipts
and worktrees. Its controlled runner test uses actual Git worktrees with inert
build/CLI fixtures; two runs at one SHA produce eight distinct profile worktrees,
retain an existing unrelated fixture worktree and sentinel, and keep eight logs.
That is runner-lifecycle evidence, not a replacement for the real C++ matrix.
Four trace fixtures independently check rejected attempts, unfinished calls,
positive completed/resumed returns and unrelated syscalls. The actual CLI
oracle uses that success-only predicate. Run these focused regressions with
python3 tests/test_grep_matrix_runner.py and
python3 tests/test_grep_trace_oracle.py.
Legacy compatibility/include/link responsibility is unchanged; no family
contraction or W-04 support is claimed.
