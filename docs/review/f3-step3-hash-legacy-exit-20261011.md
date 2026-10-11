# F3 Step 3 — HASH physical legacy consumer exit

Scope: #402/#474. Baseline `c2331d70f9f9deeaa79780aa4cf035d4e7f6fbee`.
Tested production head `e5c016a451b8084599156d376a02de8abf083e53`. This is one independent rollback unit;
the other application exits in its own PR. No upstream merge is authorized here.
Authority: PROD-03, W-01, INV-01, ARCH-02, SEM-02..05, ERR-01, LIFE-01,
VERIFY-01 and MIG-01/02; adopted M-H D-H1/D-H2/D-H5. Production owner:
`apps/sluice-hash`. Workload: sequential bounded direct regular-file processing.

## Approved external change

The human owner explicitly selected “批准两应用方案 A” in the Step 3 chat
on 2026-10-11. This approval replaces the earlier workers=2..64 bridge disposition
for this app. Remove `--workers`, including explicit value1: when encountered,
report `--workers has been removed; omit it to use direct execution`, print usage,
and return 1 before opening files. Missing/malformed/old-valid values
receive that explicit diagnosis. The equals spelling was already unsupported;
its diagnostic changes. Existing early `--help` return remains. Help omits the
flag; README migrates commands by removing it and its argument. No advisory
period, silently ignored parameter or replacement pool is introduced.

The user-authorized app-local engine removal deliberately removes
`hash_files_with_backend` and the workers argument of `hash_files`. In-tree
callers were tests; no production caller requires injection. These headers are
not installed SDK APIs. Unknown external app-header use is not zero use: the
README explicitly discloses the source break and direct-call migration, with
no injected replacement. The CLI compatibility approval does not authorize an
installed SDK retirement. #475 HOLD/KEEP surfaces remain.

## Consumer closure and contraction

Before: main -> `hash_files(workers2..64)` -> legacy engine -> task helper ->
RuntimeBuilder/ApplicationRuntime/RuntimeTaskContext/TaskResultSlot; reads use
await_read_at and Completion. Default workers1 was already direct. The exported
app-local injected API selected the same engine even with workers1.
After: main -> `hash_files` -> existing app scan/hash loop -> canonical
`blocking::read_at`. No app runtime includes, calls, injected seam or scheduler
remain. No execution mechanism has moved to a compatibility directory.

| Measurement | Baseline | Candidate |
|---|---:|---:|
| Production app C++/header lines, all files | 626 | 531 |
| Engine TU non-system Sluice header closure | 41 | 5 |
| App and consumption target library deps | core + async | core |
| Unstripped same-flags executable bytes | 352728 | 51064 |
| Unstripped async symbol lines | 772 | 0 |

Unstripped measurements use g++ C++20/O2, the same baseline Release/noliburing
canonical archives, baseline async+core and candidate core-only. They are a
controlled link comparison, not the size of xmake's stripped Release output.
Each fresh matrix worktree builds the app and consumption test before any async
consumer, and proves `libsluice_async.a` does not exist at that point. Removed
API header and old-declaration link probes fail for the named missing symbol.
The baseline executable contains that injected symbol. All measurements and
resolved header names are in [hash-closure-metrics.json](f3-step3-hash-evidence-20261011/hash-closure-metrics.json).

Authority-contraction audit: remove the runtime task wrapper, completion object,
result-slot publication, cancel polling, worker config and injection ownership
from this app. They served only the removed bridge. No production authority is
added. File/buffer lifetime remains scoped to synchronous calls and RAII.
Private exception translation retains bad_alloc/no_space, system_error native
errno/canonical mapping and fallback/backend_error; it adds no task/result
publication authority. SDK RuntimeBuilder/ApplicationRuntime/TaskResultSlot and
canonical ThreadPool/io_uring remain necessary for retained consumers and are
not removed by this slice.

## Oracle and evidence ownership

The new CLI oracle failed on baseline because accepted `--workers 1` opened an
unserviced FIFO and timed out; candidate rejects before open. Existing direct
EOF/multi-buffer/order/error oracles remain. The test-only pread shim invokes
real SYS_pread64 with maximum17 bytes and can inject EIO for one selected inode
at offset>=34. Independent expected outputs verify short progress, EOF and
later-file isolation; GREP also checks partial output. It is not a production
backend or installed adapter.

The sole injected source callsite was the closed-first/successful-second
consumption oracle. Its direct case remains, together with known/empty digests,
multi-chunk hashing and input order (six cases). hashlib checks multiple buffer
sizes. HASH exit mapping remains0/1/2/3; direct execution has no cancel source.

Canonical admission/cancellation/error/physical-retirement evidence remains
with async_sync_admission_test, the E1 ThreadPool/uring conformance suites,
backend core-cutover tests and RequestScope suites. HASH retains its direct read-error isolation oracle; the removed injected
execution form is not a backend conformance owner. Required W-02/W-03
backend obligations retain canonical owners. COPY/TAIL consumption, pipeline,
directory-sync, lifecycle and CLI oracles remain and are exercised in the matrix.

## Release allocation fixture corrective

Supplementary test-only head `f7be3fad356ec7aa98f0bb5f4f2b6aba4c8615fe` adds six `[[gnu::noinline]]`
annotations to the allocation-count fixture replacement new/delete functions.
Master reproduces GCC16 Release Werror=mismatched-new-delete when the optimizer
sees the fixture malloc/free implementation through inline allocation boundaries.
Both sides must retain their replacement-function boundary; annotating unsized
delete alone moves the warning to the inlined new/malloc side. The controlled
O3/Wall/Werror probe and actual Release RequestScope suite pass with all six
boundaries retained. Counter/allocator bodies and every assertion are unchanged;
no warning flag is disabled. Independent review APPROVE for this test-only fix.

The complete committed delta from the earlier app production matrix head is
only `tests/request_scope_test.cpp`. The input witness records that delta and the
full baseline-to-current changed-file list. Completed app/backend profiles are
retained; affected RequestScope tests rerun in Debug, remaining Release targets
resume, and Release/liburing executes from the supplementary head. This is a
shared test-fixture corrective in both independent app PRs, not shared app
production authority or an installed package change.

## Package scope

This is an app-local code/build/test change. All163 tracked canonical input
entries (include/src, xmake root/libraries/helpers, package consumers and verifier)
are byte-identical to baseline; the exact map and full changed-file list are in
[the input witness](f3-step3-hash-evidence-20261011/hash-input-witness.json).
Full xmake diff review confirms dependency changes are scoped to this app and
its consumption target, with no global flags, library rules, source globs or
usage-requirement changes. The application production diff is nonempty and is
not represented by the package verifier's narrow src/include-empty field.

Fresh baseline and candidate package verifications pass in both profiles:
79 installed headers, core13/async37 objects, archives/object contents/symbols/
bytes and header checksums equal; usage requirements, negative probes and ODR
checks equal. See [package-equivalence.json](f3-step3-hash-evidence-20261011/package-equivalence.json).
No installed SDK/production library changes occur, so no B-01 re-freeze is
triggered. Historical F1 assets remain unchanged. These current comparisons are
scoped evidence, not adoption of a new library freeze or release-ready claim.

Reproduce the four server configurations using the retained runner:

```sh
bash docs/review/f3-step3-hash-evidence-20261011/run-matrix.sh "$PWD" hash
```

The runner creates isolated detached worktrees and preserves logs; it performs
Debug/Release x noliburing/real-liburing builds, app/core-only closure and negative
probes, CLI/error/short-read oracles, canonical backend suites and COPY/TAIL
regressions. semantic_backend_conformance_test and uring_public_consumer_probe are registered
only in real-liburing configurations and are exercised there; E1 suites cover
both current backend owners.

Package replay in fresh clean worktrees for baseline and tested head:

```sh
python3 scripts/verify_f1_package.py --manifest-out /tmp/package-n.json \
  --archive-baseline-out /tmp/archive-n.json
python3 scripts/verify_f1_package.py --liburing --manifest-out /tmp/package-y.json \
  --archive-baseline-out /tmp/archive-y.json
```

Compare both archive JSON documents exactly per profile, plus HEADERS,
PREFIX_LAYOUT, PUBLIC_DEFINES, PUBLIC_INCLUDE_PATHS, PUBLIC_LINK_FLAGS,
PUBLIC_LINK_LIBS, CONFIG_REQUIREMENTS, NEGATIVE_PROBES and ODR_DISCRIMINATOR;
volatile provenance is retained but is not required to equal a different head.

## Remaining consumers and next entrance

In this independent PR the other HASH/GREP app retains its baseline bridge until
its own PR lands. After both exits, COPY remains a bounded pipeline/runtime
consumer and TAIL owns the long-lived runtime start/stop/wait path. SDK runtime
helpers and runtime_waiter_observer_test remain compatibility consumers; canonical
ThreadPool and io_uring remain required mechanisms. No blanket zero-runtime
claim is made for the repository.

COPY's next smallest entry is a bounded canonical RequestScope slice preserving
COPY-B depth/concurrency, ordered writes/errors/counters, accepted-request drain,
atomic temp/rename/directory-sync and its own workers compatibility decision.
TAIL's next entry is an app-owned driver preserving -n, descriptor follow,
truncation, signal stop and every-exit settlement; no new public settlement API.
Neither next step is implemented here.

Rollback this app's commits independently; it restores the removed bridge and
workers semantics without changing the SDK or the other application's exit.
No sanitizer result or universal interleaving proof is claimed. CodeRabbit CLI
is unavailable on the server; independent agent review supplies the scoped
technical review, not a fabricated CodeRabbit receipt. F3_FULL_RETIREMENT remains
NOT_DONE and APPLICATION_RUNTIME_LIBRARY_REMOVED remains NO.

## Completed server matrix

| Configuration | Result |
| --- | --- |
| Debug × noliburing | PASS |
| Release × noliburing | PASS |
| Debug × real liburing | PASS |
| Release × real liburing | PASS |

The [matrix receipt](f3-step3-hash-evidence-20261011/matrix-summary.json) records
absolute server log paths and SHA-256 checksums. Debug and Release/noliburing
combine the production-head matrix with the isolated six-annotation fixture
correction; Release/real-liburing ran the corrected head fresh. Real-liburing
supplements include semantic_backend_conformance_test and uring_public_consumer_probe.
All affected app, COPY/TAIL, core, ThreadPool and real io_uring tests pass.
The removed app API fails both header compilation and symbol linkage; the
application builds with core alone before any async archive is built.
