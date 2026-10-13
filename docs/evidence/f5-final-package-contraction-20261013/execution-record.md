# F5 final package & public surface contraction — execution record

Implementation head: `a23a94cdfbfa90f6ad6aff2d1b6706c305a4245c` (branch
`f5-final-package`). Execution baseline: `14a27a83821a94e6fc5a9684b48fcd296d7de891`
(F2 final exit master). Capture bindings: the B-01 v6 summary and the installed
probes name the base and implementation SHAs directly; the matrix records the
worktree head it ran under in `matrix/toolchain.log` and `matrix/matrix.json`
(and its content is consistent only with the F5 tree: 66 ring-profile tests and
no `runtime_waiter_observer_test`). The branch head adds documentation-only
commits after the implementation head, so no capture is rebound to a tree whose
compiled inputs differ (`git diff a23a94cd..HEAD -- include/ src/ apps/ xmake/
xmake.lua tests/` is empty at every documentation-only head referenced here).

Status: **CANDIDATE — the installed-surface break requires the owner's decision
(`installed-surface-risk-disposition.md`); nothing here is merged or
authorized.** F2/F3/F4 closure records are unchanged by this slice.

## Commit structure

```
14a27a83 (baseline: F2 merged/closed)
  └─ 7b118b0c docs(evidence): record F5 live package closure and dispositions
      └─ f4904d5a refactor(async)!: retire the dormant Family A sync wrappers
          └─ f5b4f6e4 refactor(async)!: retire Family B future/wait_policy after the Group exit
              └─ 0524114c refactor(async)!: retire the legacy scheduler/select/sync closure
                  └─ 549bad34 build(package): contract the install surface to the final closure
                      └─ a23a94cd test(evidence): add F5 B-01 v6 capture and installed-probe harnesses
```

Each retirement commit is a self-contained dependency family and an
independently revertible rollback unit. `f4904d5a`/`f5b4f6e4` retire families
with no in-tree consumer; `0524114c` retires the one remaining dependency
family (legacy Scheduler and its physical closure, which included the seam
targets and the internal headers); `549bad34` contracts the install globs and
removes the never-installed experimental tree; `a23a94cd` adds only evidence
harnesses.

## Requirement mapping

| Authority | Executed |
|---|---|
| MIG-02 dormant public synchronization primitives (outside v1) | Family A (`condition.hpp`, `semaphore.hpp`, `async_queue.hpp`) removed from tree after the #475 T1–T3 re-examination; T4 is the owner packet in this directory |
| MIG-02 Group/Future/WaitPolicy row (retain internal substrate only where a retained adapter needs it) | Family B (`future.hpp`, `wait_policy.hpp`, `wait_policy.cpp`) removed; the recorded necessary condition (Group + EventedWaitPolicy exit) was met at the F3 part-C cut and no surviving consumer existed |
| MIG-02 legacy multi-worker runtime row (not a required v1 product) | Family C plus the scheduler/select substrate removed: 14 runtime TUs, 12 headers, the D-13 seam headers/TUs, and the runtime waiter/observer test |
| HOST-01 public runtime-aware Event/Semaphore/Mutex/Condition/RwLock/Queue/select | every such header leaves the tree; no shim, alias or forwarding wrapper is added |
| W-01 / W-02 / W-03 canonical | retained untouched: File/blocking closure, `Request<T>`/`RequestCore`, `RequestScope`, ThreadPool and io_uring backends, observer/progress/cancel/shutdown surfaces |
| W-04 optional host (conditional N/A) | `StackfulIoHost` closure (`stackful_io_host.hpp`, `fiber.hpp`, `fiber_ctx.hpp`, 3 members) retained and verified to depend only on Fiber/FiberCtx, not on the removed runtime; support claims audited (`w04-negative-support-audit.md`); `OPTIONAL_W04_SUPPORTED=NO` |
| M-A4 experimental final-tree disposition | `include/sluice/experimental/` and `src/experimental/` removed from the tree; never installed, built by no target, superseded by canonical `uring_backend` |
| PROD-03 / GOV-02 | statuses in this record and the ledger F5 section use the defined vocabulary; no candidate result is reported as merged or released |

## Package identity after the contraction

| Metric | Before (`14a27a83`) | After (`a23a94cd`) |
|---|---|---|
| Installed headers | 49 | 25 |
| Library objects | 32 (2 core + 30 async) | 15 (2 core + 13 async) |
| `libsluice_async.a` members | 30 | 13 |

Identical in both ring profiles (B-01 v6 `summary.json`, four keys). The
installed package is now headers + the two static archives only; the sole
public usage requirement remains the liburing-propagated `SLUICE_HAS_LIBURING`
define and link (`xmake/libraries.lua:52-64`), which every consumer and probe
verifies per ring profile.

## Removed mechanisms and the authority-contraction audit

- 23 installed headers removed from the tree; 1 (`async/detail/context_identity.hpp`)
  de-installed only, still compiled and still reachable by library TUs.
- 17 archive TUs removed: 14 legacy-runtime members + 3 D-13 seam members
  (`mutex_test_seam`, `queue_test_seam`, `scheduler_fe2_test_seam`).
- 3 internal headers of the removed runtime retired with it
  (`scheduler_internal.hpp`, `scheduler_test_access.hpp`, `queue_detail.hpp`).
- `async/detail/fail_fast.hpp`: 21 removed diagnostics (select/scheduler/
  async-mutex/rwlock/condition/wait-queue/evented families, `evented_admission_check`
  and the orphaned `group_lifetime_fail_fast`); 15 retained (context, request,
  scope, host, threadpool, uring, identity) — all rewrite of dead branches; no
  retained declaration lost a caller.
- `tests/runtime_waiter_observer_test.cpp` and its target block removed with
  its subject; RequestCore observer coverage remains in the canonical suites.

Authority-contraction audit (Mechanism Justification Gate, roadmap #390):

- The removed runtime carried the only state in the tree that represented
  fiber scheduling, park/wake registries, the timer pump, multi-source
  suspension and runtime-aware synchronization. After the four-app retirement
  and the F2/F3/F4 cuts, none of those facts had an authority that any
  canonical path consulted; canonical archive objects reference none of their
  symbols (per-member undefined-symbol audit in `closure-manifest.json`).
- The mechanisms that must remain despite looking correlated are recorded, not
  collapsed: the RequestCore acquisition/terminal/publication/borrow/control
  pins, the observer claim/retire protocol, and the physical closure of the
  W-04 host (`Fiber`/`FiberCtx`) which represents a real retained optional
  workload rather than scheduler substrate.
- No production authority moved in this slice; it removes code that no
  authority consulted. Consequently no additional mechanism became eligible for
  removal, and this slice records that state explicitly rather than proposing a
  speculative next cut.

## Validation executed

Every row names the exact artifact under this directory.

| F5 gate | Evidence |
|---|---|
| Six configuration profiles (Debug/Release × no-ring/real-ring, native Clang ASan+UBSan and TSan) | `matrix/run-matrix.py`, `matrix/matrix.json`, `matrix/*.log`, per-profile `xmake.conf` and toolchain cache, `matrix/toolchain.log`. Result: debug-n 52/52, debug-y 66/66, release-n 52/52, release-y 66/66, asanubsan-n 52/52, tsan-n 51/51; `F5_MATRIX_SIX_PROFILES_PASS`. Real kernel io_uring path: the ring-only tests (`uring_backend_smoke_test`, `uring_progress_race_test`, `uring_registration_lifecycle_test`, `semantic_backend_conformance_test`, `uring_public_consumer_probe`) execute and pass in the `-y` profiles with no `NOT RUN` marker in the logs; the B-01 y capture records `w03_external_loop(uring): PASS` while the n capture records `UNAVAILABLE (io_uring profile not built into this package)`; and `uring_backend.cpp` throws when `io_uring_queue_init` fails, so a passing ring run is a real ring run. (`summary.json`'s `real_ring` field is the ring-labelled profile marker, not itself the measurement.) |
| TSan single unavailability | `matrix/tsan-n-request_scope_test-link-attempt.log`: `request_scope_test` supplies strong `operator new/delete` overrides that collide with the TSan C++ runtime (`multiple definition of 'operator new'`); the same outcome was recorded by the F2/F3 matrices, the source is byte-identical to the base revision (asserted in `run-matrix.py`), and its target configuration is untouched by this slice (`git diff 14a27a83..HEAD -- xmake/tests.lua` contains no change to that target) |
| Versioned B-01 v6 clean-room package capture | `b01-v6-candidate/` — `reproduce.py` (Git-archive clean-room sources, own `.xmake`/build dir/prefix per profile, `--ccache=n`, consumers compiled against the installed prefix only), `summary.json`, `commands.json`, `inventory.lua`, `raw-manifest.json`, `raw-package-evidence.tar.gz`, `excluded-artifacts.json`, `toolchain/`. Result: `F5_B01_V6_CANDIDATE_FRESH_PACKAGE_PASS 14a27a83… a23a94cd…`, 32/30/49 → 15/13/25 in both ring profiles |
| Negative installed-surface probes with positive controls | `raw-package-evidence.tar.gz`: 23 `removed-header-*.log` compile attempts fail with `No such file or directory` in the after prefix and are empty (success) in the before prefix; 10 `old-entry-*.log` consumers compile against before-headers and link against the F5 archive, failing with `undefined reference` naming the removed symbols; before-stage controls show the same probes compiling, linking and (constructor-real probes) running |
| `PACKAGE_USAGE_REQUIREMENTS_AND_DECLARATIONS_MATCH_ARTIFACTS` | B-01 v6 consumers (W-01/W-02/W-03 + 8 contract families + ODR two-TU consumer) build and run from the installed prefix in both ring profiles with the macro view matching the archive's build; install surface is headers + 2 archives with no other declared artifacts to contradict (`xmake/libraries.lua`); `installed-probes/probes-{n,y}/probes.json` |
| `PUBLIC_HEADER_TEMPLATE_MULTI_TU_ODR_LINK_AUDIT` | `installed-probes/run-probes.py` on the after prefixes: every one of the 25 installed headers compiles standalone in both macro views; two translation units instantiate the canonical request/result surface and compare `RequestId` type information and the addresses of two header-inline functions after separate compilation — identical, and the probe runs clean; before-prefix control recorded |
| `ARCHIVE_OBJECT_SYMBOL_ROLE_AUDIT` | `closure-manifest.json` per-member role classification; `raw-package-evidence.tar.gz` `*-members-*/*.symbols` plus `defined-async.log`/`undefined-async.log`; asserted in `reproduce.py`: no defined symbol of any removed member survives in either after archive and no retained member gained an undefined reference to one; D-13 seams carry zero defined symbols in both profiles |
| `RETIRED_ROLE_CODE_IN_CANONICAL_TARGET` | 0: `libsluice_async.a`/`libsluice_core.a` hold only the retained closure; the removed members are absent from the archive list, the retired-role diagnostics are absent from the release archive's defined symbols, and the experimental tree is in no build target |
| `REQUIRED_EXTERNAL_WORKLOADS` | W-01/W-02/W-03 consumers and the 8 contract families link and run from the installed prefix in both ring profiles (B-01 v6); W-04 stays conditional (see below) |
| `CANONICAL_COMPAT_EXPERIMENTAL_PROFILE_BOUNDARIES` | experimental headers absent from the tree and from every prefix (B-01 assertions + before-prefix presence control); canonical/compat boundary is the installed-header set itself, with the compat families removed rather than shipped as headers |
| `SUPPORT_CLAIMS_MATCH_SHIPPED_ARTIFACTS` | `w04-negative-support-audit.md`: no unqualified W-04/multi-worker/host support claim remains in README, README.zh-CN, the ledger pin or docs; README "Current implementation" now states the retired runtime and the retained canonical closure |
| `F5_INSTALL_BUILD_CONTRACTED` | `xmake/libraries.lua` install globs bounded to the closure with `remove_headerfiles` for the private identity header; `549bad34` is the single install-surface commit; B-01 v6 measures the contraction in four clean-room configurations |
| W-04 (optional) | `CONDITIONAL_NA_UNSUPPORTED`: the host closure is retained, compiles and its consumer builds, but no support is claimed; `OPTIONAL_W04_SUPPORTED=NO` |
| GitHub CI | run on the branch head recorded in the closeout packet |

## Pre-existing environment findings (not caused by this slice)

1. **Reusing one `-o` build directory across a ring-profile switch links no-ring
   stub objects into ring-profile consumers.** Captured on the branch head in
   `known-preexisting/stale-builddir-reuse/`: full no-ring test run, in-place
   reconfigure to `--liburing=y`, then the full ring test run fails to link
   `uring_public_consumer_probe` with `undefined reference to
   UringAsyncBackend::UringAsyncBackend(UringConfig)`. The reused directory's
   `libsluice_async.a` defines only the stub constructor
   `UringAsyncBackend(unsigned int)`, while the isolated ring profile's archive
   additionally defines `UringAsyncBackend(UringConfig)` and the isolated
   no-ring archive matches the stub exactly (`archive-symbols.txt`). The
   failure was also observed at the baseline `14a27a83` during F5-F in a
   temporary scratch worktree (removed after the check). The F5 matrix
   therefore configures each profile into its own build directory
   (`matrix/run-matrix.py`), the same isolation F2/F3 used.
2. **`xmake -a` fails at the head and at the base under Clang.** In the
   `asanubsan` profile, a full `xmake -a` build stops at
   `request_scope.hpp:204: private field 'policy_' is not used
   [-Werror,-Wunused-private-field]` while compiling `src/async/request_scope.cpp`.
   Captured at both the base (`/tmp/f5-clangcheck`) and the head as
   `known-preexisting/clang-mutant-a-{base,head}.log`. The two `policy_` uses
   are both inside mutation-guarded arms
   (`SLUICE_D1_MUTANT_DESTRUCTOR_SKIPS_SETTLE`,
   `SLUICE_D1_MUTANT_FINISH_CANCELS_CLEANUP`), and `request_scope.hpp`,
   `request_scope.cpp`, `request_scope_test.cpp` and the mutation-target
   configuration are untouched by this slice
   (`git diff 14a27a83..HEAD --` those paths is empty except the unrelated
   `runtime_waiter_observer_test` block removal in `xmake/tests.lua`). The
   registered-test path is unaffected: `matrix/asanubsan-n.log` reports 52/52,
   and F5's earlier cold sanitizer run compiled the mutation targets in the
   same profile without the error; mutation targets are documented as manually
   executed, never regular tests.
3. **`request_scope_test` cannot link under TSan.** Recorded by F2 and F3 as
   well; see the gate row above for the capture and the byte-identity control.

## Remaining limitations and owner decisions

- The installed-surface break itself is PENDING: F0 §1.7(5) per-surface owner
  decisions, the #475 T4/E1 external-use decision and merge authorization are
  requested in `installed-surface-risk-disposition.md`. No row may be reported
  as authorized removal before that decision.
- External use is UNKNOWN for every surface, not zero (F0 §1.5).
- The W-04 host is retained but unsupported; no support claim is made or implied.
- F6 and v1 release readiness are not claimed; `V1_RELEASE_READY=NO`.
- The date of every measurement in this directory is 2026-10-13.
- Residuals recorded by the review round (not blockers, not cleaned silently):
  the F4 retirement probe text still references removed headers while F4's
  verifier compiles it against its own F4-era prefix; the retained W-04 host
  header `fiber.hpp` still declares Scheduler-era members with no retained user
  (removal is a further per-surface decision on the pending host); the F1-era
  verifier text still lists `scheduler.hpp` under configuration-dependent
  headers; and the Lean/TLA models naming retired headers stay HISTORICAL per
  the formal evidence map. Machine form: `RESIDUALS` in
  `closure-manifest.json`.
- Review round: two independent adversarial reviews of head `3aa3b857` (the
  documentation head whose compiled inputs equal `a23a94cd`); their findings,
  dispositions and the re-check are recorded in `independent-review.md`.
