# F3 #498 PR B narrow candidate verification receipt

DRAFT_NONADOPTED; IMPLEMENTED_UNVERIFIED pending independent final review.
Final unmerged stack base: PR A d38430cf3ed770e2705c20eefa5c1fcd75bec6d2
(code3f846afa plus evidence-only final receipt). Rebased B snapshot1df5326b35c837532febd8aff8a2d1bbc5167844
is byte-identical across production/build/tests/package consumers to the tested
pre-rebase candidate. [Final rebase mapping](../evidence/f3-498-pr-b-final-matrix/final-rebase-equivalence.json)
records the exact comparison; no unchanged tests were repeated. Captured original
provenance remains unchanged. Original corrected candidate snapshot
41fc1b45dce2723e61a2a2703a035cbd0cc5c382. Narrow branch diff removes only
src/async/file.cpp and include/sluice/async/file.hpp from production. Source,
installed header and four out-of-line symbols contract together. This candidate
creates no runtime authority/state and leaves all Completion, RequestCore,
ApplicationRuntime/Scheduler, optional host Fiber/fiber_ctx obligations intact.

Authority: SEM-01/03/04/05/06, ERR-01, REQ, LIFE, PROG, SHUT, HOST,
MIG-01/02, GOV-02; F0 section 1.7. Workload: positional File read/write and
sync_data/sync_all canonical semantics, retained package and optional host closure.
[Per-surface migration/deprecation/external-risk packet](f3-498-async-file-retirement-candidate.md)
remains pending for EACH named surface. No owner approval, deprecation-window
choice, F5 package adoption or versioned release is inferred from this receipt.

## Exact evidence and source mapping

Initial full matrix snapshot 9f8c6b3dcc5b1f72588523ec98263ddb03f98d35 built
in six independent worktrees/config/build/output directories. Actual verbose
compile argv and toolchain caches resolve /usr/bin/g++ for Debug/Release and
/usr/bin/clang++ for sanitizers, with xmake ccache=false. They never resolve the
Fedora ccache wrappers. Each liburing=n/y profile has its own real configuration.

Supplemental A corrections removed an inert scope ticket that failed Clang22
-Werror and restored the one-byte smoke write-range input accidentally omitted
during test migration. No assertion or allocation oracle was weakened. The
initial failures remain in raw evidence. Rebase of this unpushed B branch onto
A3f846afa yields corrected candidate41fc1b45. Exact input comparison shows only
tests/request_scope_test.cpp and tests/uring_backend_smoke_test.cpp changed
across production/build/tests. Scope was re-executed in all six configurations;
smoke was re-executed in both real-uring configurations. All other relevant
source inputs remain byte-identical, so unchanged full matrices were not repeated.
Reruns use explicit /usr/bin compiler paths and CCACHE_DISABLE=1.

| Configuration | Effective result after corrected-input reruns |
| --- | --- |
| Debug n | 40/40 target executions PASS |
| Debug real liburing y | 52/52 target executions PASS |
| Release n | 40/40 target executions PASS |
| Release real liburing y | 52/52 target executions PASS |
| Clang22 ASan+UBSan n | 13/13 PASS, including corrected scope |
| Clang22 static TSan n | 12 target executions PASS; scope static link unavailable |
| Clang22 shared TSan n, fresh scope configuration | Scope25/25 assertions PASS; all original allocation/lifetime assertions retained |

Static TSan scope compilation succeeds after the inert-variable correction;
linking fails because Fedora Clang22 tsan_cxx defines strong global new/delete
and the scope test independently overrides those operators for its allocation
oracle. Raw linker diagnostics are retained. This is a configuration limitation,
not a TSan scope pass; no allocation assertion or instrumentation was disabled.
A distinct fresh worktree at5afa2749 then executed the scope test with the
installed shared TSan runtime:25/25 assertions PASS. fsanitize=thread remains
enabled; nm proves instrumented calls and ldd proves libclang_rt.tsan.so loaded
from the explicit Clang22 rpath. No test code or allocation assertion changed.
[Shared-scope evidence](../evidence/f3-498-pr-b-final-matrix/shared-tsan-scope.json)
records exact flags/SHA/runtime/raw hashes. This supplies all13 relevant TSan
targets across two honest runtime configurations; it does not relabel the failed
static scope configuration as passing.

[Matrix evidence](../evidence/f3-498-pr-b-final-matrix/summary.json) contains
initial and rerun SHAs/statuses, effective per-target results and actual compiler
programs. raw-matrix-evidence.tar.gz contains build/run/config logs, dependency
and symbol closures, toolchain caches, instrumentation symbols and drivers;
raw-sha256.json binds uncompressed captures. Original failed target rows remain
visible alongside the corrected-input reruns.

## Versioned package candidate and reproducibility

[Fresh v2 package receipt](../evidence/f3-498-async-file-candidate-v2/REPRODUCE.md)
records an actual complete fresh-dir orchestrator execution, then a consumer-only
clean-environment correction rerun using its unchanged built prefixes. The
orchestrator places its own scripts, archives exact requested before/after commits,
generates all positive/negative link source inputs, compares tracked package
inputs, and writes actual statuses/provenance before finalization. Installed
consumer source inputs come from EACH archived stage/profile, never an unchecked
working-tree consumer. Old-symbol links and real-uring runs use the clean
PATH/HOME/LANG environment without compiler/link search-path variables.

Both profiles: headers79→78, objects50→49, async members37→36, only
async/file.hpp/file.cpp.o removed. Installed W01/W02/W03 threadpool and host
construction/link/run pass; W03 real uring y passes, n explicitly unavailable
exit2. Removed-header/four-name syntax controls and independent old-signature
symbol link controls discriminate baseline presence from candidate absence.
No-library-rebuild final input mapping is recorded in
[final-input-equivalence.json](../evidence/f3-498-async-file-candidate-v2/final-input-equivalence.json):
all library/header/build and installed-consumer inputs are unchanged between
captured9f8c6b3d and corrected41fc1b45. Initial v1 dated captures and all historical
F1/B01 files remain unchanged; dated input refs preserve old SHAs after rebase.

## Remaining boundaries

Independent final review is pending. EXTERNAL_RISK_PENDING and F5_PENDING
remain separate owner obligations; UNKNOWN_EXTERNAL_USE is UNKNOWN for all four
symbols. No source/install/link breakage is adopted. W04 optional support,
whole-facade/runtime/Scheduler retirement and V1 release readiness are not claimed.
The remaining runtime waiter/COPY helper consumers still exist and pass the
retained matrix; this cut cannot justify removing their implementation substrate.
