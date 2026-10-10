# F3 HASH: default direct execution, bounded compatibility bridge

Phase F / #402, execution carrier #474, PR #492. Root v1-r4 governs:
PROD-03, ARCH-01, W-01, INV-01, SEM-01, LIFE-01, MIG-01/MIG-02;
adopted D-H2/D-H5 at master `22606ae281faca3acd2d59087f942de2971e4b0d`.
This is one production slice, not runtime retirement or v1 release acceptance.

## Scope and authority contraction

The owner explicitly approved in this task: workers=1 (including the default)
uses canonical blocking File; workers=2..64 retain the legacy runtime path.
Injected backend calls retain their selected backend and runtime path even with
workers=1. Help/README disclose the split; no worker count becomes a silent no-op.
The same hash loop handles both invocation forms. No new installed/public
library authority, runtime state, fallback, task framework or File authority.

Default execution no longer constructs ApplicationRuntime, Completion storage,
ThreadPoolBackend, requests or runtime/backend workers. Buffer/File use ends
synchronously with each read. Legacy per-file Completion still spans every
await and is destroyed only after the loop returns; cancellation checks retain
their old positions. Files remain owned until the engine returns. Existing
RAII close behavior, CLI regular-file filtering, order, digest output, per-file
error isolation and valid worker counts are retained. Allocation-failure behavior
is not newly proven: result/path allocation can still throw.

D-H2 uses one app-local constexpr `valid_config`, derived from the existing range
constants, in CLI/engine entry checks. It materializes no state. CLI out-of-range
buffers now return usage exit 1 before any file open, replacing late per-file exit 2.
Engine rejection remains `invalid_state`. Separate checks remain necessary for
CLI, ordinary engine and injected-backend entry points; they no longer own
separate range definitions. Backend validity and retained cancellation guards
still protect real entry/lifetime obligations and cannot be deleted.

Candidate comparison is [machine-readable](f3-hash-candidate-selection-20261011.json).
Pool/stream deletion still lacks per-surface external-risk/deprecation acceptance;
F2 protocol contraction has larger publication/retirement dependency cuts.
No library runtime state/object/header is eligible for removal yet: HASH's
multi-worker/injected paths, other apps, host and compatibility consumers remain.

## Exact evidence and reproducibility

WSL base: `22606ae281faca3acd2d59087f942de2971e4b0d`; original dirty local
master/index preserved. Branch `fix/f3-hash-direct-default` in
`/tmp/sluice-f3-hash-direct`. No C++ build/test ran on WSL.
SSH BatchMode public-key authentication succeeded to `jnhu@192.168.31.75`.
Attachment path `~/Source/sluice` returned exit 1 (case mismatch); actual protected
repository is `/home/jnhu/Source/Sluice`, clean at `7f5eed6a` on initial inspection.
Only fetch/worktree metadata changed there; builds used this task's detached worktrees.

Production candidate: `498589416c9854e5fdff9c5f3b70e09c3952d35c`.
Final fixed test candidate: `14758dcdbe8072d6cfad45b1f187e61b26fe8bbc`.
The only delta between those commits is the regression's File construction.
Remote worktrees `/home/jnhu/Source/sluice-f3-hash-14758dcd-{debug,release}-{n,y}`
identify the four final runs. Their SHA and clean status are asserted by the
[recorded runner](f3-hash-evidence-20261011/remote-fixed-matrix.sh).
Commands, configurations, stdout and exits are in [raw evidence](f3-hash-evidence-20261011/).

| Evidence | Result / actual scope |
|---|---|
| Debug + no-liburing | PASS, runner exit 0 at 14758dcd |
| Release + no-liburing | PASS, runner exit 0 at 14758dcd |
| Debug + real liburing | PASS, runner exit 0 at 14758dcd |
| Release + real liburing | PASS, runner exit 0 at 14758dcd |
| Focused suite | 6 HASH cases, including read-error continuation through direct/multi-worker/injected paths |
| Affected direct suite | blocking_file_read_test, direct_cursor_position_test, direct_w01_consumer_probe, all four profiles |
| CLI oracle | Python hashlib digest oracle, empty/multi-chunk inputs, printed order/open-error isolation, workers1/2/64, numeric errors, early bounds with FIFO |
| Negative control | Old HEAD --buffer-size 1 with FIFO times out in 3s (oracle exit 1); candidate rejects bounds1/4095/67108865 before open, exit 1 |
| Thread syscall discriminator | Old workers1 trace: 5 clone/clone3 attempts; final workers1: 0; workers2: 7 attempts in each profile. These are syscall attempts, not claimed successful-thread counts |
| GCC ASan/UBSan | UNAVAILABLE: xmake sanitizer link exit255; missing /usr/lib64/libasan.so.8.0.0 |
| Clang ASan/UBSan | UNAVAILABLE for complete app build: link exit255, missing clang22 asan_static/asan/asan_cxx archives; standalone empty link probe alone did not establish app availability |
| TSan | NOT_RUN: no shared-state publication, wake, cancel or shutdown protocol changed; no TSan PASS claimed |
| Independent review | APPROVE/0 blockers at 06f82622 and delta APPROVE/0 blockers at 14758dcd; final documentation/head review receipt belongs on PR |
| CodeRabbit CLI | NOT_COMPLETED: connection failed (WebSocket closed); independent reviewer above supplies review, not a fabricated CodeRabbit approval |

First xmake attempt used two target arguments and exited255; corrected to one
build call per target. Added test at49858941 incorrectly used File{} (no default
constructor), so its four matrices failed compilation;14758dcd fixes the test
using actual open/close/move. Those failures are retained, not reported as PASS.

## B-01 and actual package comparison

Baseline and candidate both ran unmodified `scripts/verify_f1_package.py` in
separate release no-liburing and real-liburing worktrees. All gates PASS (exit0):
79 standalone installed headers, external W01/W02/W03/contract consumers,
real-kernel io_uring, optional-candidate probe, ODR and negative package probes.
This does not declare optional W04 supported or broader v1 conformance.

New measured manifests/archive records are in the evidence directory. Baseline
head is22606ae2; candidate package head is49858941. `git merge-base HEAD origin/master`
correctly pins22606ae2. `PRODUCTION_DIFF_EMPTY=true` in that verifier means
**src/include only**, not all production files: apps and xmake/apps.lua really
changed. The package libraries/install inputs have no diff; both archives are
byte-identical before/after in each profile, including every object hash and
symbol inventory. Final test/docs-only successors require input-equivalence and
exact-head test receipts; no historical frozen manifest has been overwritten or
repurposed. B-01 is PASS for this unchanged SDK slice; no claim that a new
src/include production baseline has been frozen.

System liburing2.13 was exposed through task-owned symlinks under
`/tmp/sluice-f3-hash-evidence-20261011/uring-prefix` to /usr/include/liburing{.h,/}
and /usr/lib64/liburing libraries. Initial --uring-prefix /usr failed because
CPLUS_INCLUDE_PATH=/usr/include breaks GCC include_next<stdlib.h>; retry with
that narrow prefix passed. No system package/security configuration changed.

| Measured quantity | Before | After |
|---|---:|---:|
| Resolved library source files (recorded compile commands) | 50 | 50 |
| Installed public headers | 79 | 79 |
| Core archive members | 13 | 13 |
| Async archive members | 37 | 37 |
| Defined-symbol records, no-liburing | 1316 | 1316 |
| Defined-symbol records, liburing | 1400 | 1400 |
| External undefined symbols, no/liburing | 98 / 109 | 98 / 109 |
| main.cpp Sluice header closure | 34 | 3 |
| cli_parse.cpp Sluice header closure | 34 | 3 |
| hash_task.cpp Sluice header closure | 40 | 41 |
| Runtime reachable in those 3 TU closures | 3 | 1 |
| Default invocation constructs runtime/backend | yes | no |

`PRODUCTION_FILES_REMOVED=0`, `PRODUCTION_FILES_MODIFIED=4` (2 app cpp,1 app hpp,
1 xmake file), `PUBLIC_HEADERS_REMOVED=0`, `COMPILED_OBJECTS_REMOVED=0`,
`LEGACY_SYMBOLS_REMOVED=0`, `NEW_PUBLIC_AUTHORITIES=0`,
`NEW_UNJUSTIFIED_RUNTIME_STATES=0`. Library-wide consumer edges were not recounted;
the 3→1 value explicitly scopes header reachability to the three audited HASH TUs.
Compiled/install burden does not fall until retained consumers and risk gates close.

## Downstream ownership and next slice

#459 (M_DECISION_COMPLETE), #466/#467 (NO_REMAINING_WORK in M-only scope) were
closed with lossless #402 handoff and #491 adoption evidence. #474 stays OPEN for
F3 execution; #461/#462 stay OPEN for F4/F5 execution plus external-risk acceptance.
#463/#475 were already CLOSED as decision tickets; #475 HOLD/KEEP obligations
remain active. No other OPEN M child appeared in the audit; #458 stayed CLOSED.

Next executable slice: GREP D-H1/D-H2 oracles and direct migration with its own
explicit workers compatibility decision; complete HASH legacy worker/backend
exit before removing runtime linkage. Per-surface pool/stream deprecation risk
remains a human decision, not implicitly granted by this app migration.
`V1_RELEASE_READY=NO`.
