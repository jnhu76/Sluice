# PR B verification record (B4)

Head at record: see `git log` for the evidence commits under
`docs/evidence/pr-b-boundary-contraction-20261016/`. Baseline for every
comparison: `5c1b14db328f3588ddb5a275d264eb01aac7e188` (PR #544 merge).

Environment: g++ 16.2.1, clang 22.1.8, xmake v3.0.9, Linux 7.2.9 fc44
x86_64, system liburing 2.13 (`--uring-prefix /usr`). During this campaign a
system update removed the `libasan` runtime rpm that gcc's `libasan.so`
linker script references, so the sanitizer cells pin the clang toolchain,
which carries a static sanitizer runtime. The F5-era gcc sanitizer runs
(2026-10-13) predate that environment change.

## 1. Deterministic matrix (`matrix/`)

Per-cell configure/build/test logs, `xmake.conf`, sha256 digests and
`matrix.json` (which records the exact `argv` of every configure, build,
test and app-build command — the reproduction recipe; no runner script is
retained, matching the F5 matrix precedent):

| Cell | Toolchain | Tests | Result |
|---|---|---|---|
| debug-n | gcc | 56 | 0 failed |
| debug-y | gcc | 74 | 0 failed |
| release-n | gcc | 56 + 4 apps | 0 failed, apps build |
| release-y | gcc | 74 + 4 apps | 0 failed, apps build |
| asanubsan-n | clang | 56 | 0 failed |
| asanubsan-y | clang | 74 | 1 failed — RG1 (see below) |
| tsan-n | clang | 56 | 0 failed |
| tsan-y | clang | 74 | 0 failed |

App oracles (copy pipeline, tail lifecycle/CLI) are part of the test group
and ran in every cell; the four app binaries (`sluice-copy`, `sluice-hash`,
`sluice-grep`, `sluice-tail`) additionally build in both release cells.

Re-run at the review-fix head (commits `b3de9262`..`51dc830f`): all eight
cells reconfigured and rebuilt from the fix tree with the recorded argv;
the table is unchanged, `matrix.json` carries the fresh exit codes and log
digests, and `asanubsan-y-rg1-lsan-disposition.txt` gains the fix-head
capture (same scenario, same site and byte counts as the PR-head capture).

### asanubsan-y disposition (pre-existing RG1, not a PR B regression)

`shutdown_lifecycle_uring_test` fails only under LeakSanitizer with a leak
in `poison_with_in_flight_cqe` — the uring poisoned-shutdown path that
release gate RG1 already tracks. F5's matrix never ran an asanubsan-y cell,
so PR B's wider matrix surfaces the recorded finding for the first time.
`matrix/asanubsan-y-rg1-lsan-disposition.txt` holds both captures: the same
scenario and allocation site report at the baseline commit `5c1b14db`
(one-line source offset from the test migration; the leaked byte count
differs only by the recorded `Request` layout growth — §5), so PR B neither
introduced nor changed the finding. The same test passes in every other
cell including tsan-y. RG1 remains a blocking release gate; no exception is
claimed here.

## 2. Clean-room v2 package verification

`scripts/verify_v2_package.py`, both profiles, every gate PASS at the
review-fix head (24 gates per profile run); manifests and
archive/object/symbol baselines recorded next to this file, with the
manifest now rewritten after the last gate so it records every verdict:

- `v2-package-manifest-noliburing.json` — 11-header exact-set prefix,
  11/11 sole-include compiles, workloads and micro-contracts on the public
  factories, `w03 uring` reports the explicit `not_supported` outcome
  (exit 2), ODR probe agrees across opposite macro views, the seam
  negatives compile the real installed headers with pinned diagnostics,
  and the installed mutation regions match the pinned D1 template seams.
- `v2-package-manifest-liburing.json` — same surface, real io_uring
  selection (`w03 uring` exit 0), archive carries the io_uring external
  references and the `make_uring_context` symbol.

The verifier's own behavior is self-tested
(`scripts/test_verify_v2_package.py`, 14 tests): the seam gate must fail
when an installed header re-exposes the backend definition, a probe
failing without its expected diagnostic must not count as the negative
result, and an injected installed mutant region must be flagged.

## 3. Mutation spot-check (`mutation/`)

The two B2 release-edge mutants whose code moved with the seam
internalization still die on their named oracles. Re-killed at the
review-fix head (gcc release profile, direct binary invocation):

- `public_request_mut_release_forgets_binding`: the review fix moved this
  mutation out of installed `request.hpp` into
  `detail::RequestBridge::discard` in `request_core.cpp` so no installed
  header changes consumer compilation. The moved mutant is killed by
  `discard_releases_the_published_binding`: the discarded binding stays
  live, the slot never re-admits, and teardown hits the outstanding-
  bindings diagnostic.
- `public_request_mut_nonterminal_release_detaches` (mutant already in
  `request_core.cpp` with the bridge): killed by
  `destroy_nonterminal_request` — "the always-on diagnostic did not
  terminate".

The remaining installed mutation regions are the two D1 function-template
seams in `request_scope.hpp` (they cannot leave the header); the package
verifier pins that exact set, so any new or removed installed region fails
`NEGATIVE_V2_MUTANT_DEFINES_NOT_INSTALLED`.

## 4. Formal-model correspondence preservation

PR B is a representation-only change for every active model; no modeled
action, guard, interleaving or fairness assumption changed:

- **RequestCore.tla** (target: `src/async/detail/request_core.cpp` +
  `request.hpp`): the protocol core (`request_core.cpp` mechanism set) is
  untouched; the public template edges became out-of-line
  `detail::RequestBridge` calls that invoke the same RequestCore
  transitions the inline code invoked (ready/observe/consume →
  observe/consume on the core; discard → release path; cancel → backend
  arbitration). Evidence the mapping still bites: the deterministic suite
  passes in all 8 matrix cells and both release-edge mutants above are
  killed through the moved code.
- **ObserverCore.tla** (target: request_core observer delivery state):
  after the review fix the correspondence is literal. The model's
  `ProcessDelivery` runs the `delivering → retired` edge (incrementing
  `delivery_retires`, freeing the host record) as the delivery owner
  processes the returned hook; the C++ previously let any caller acquire
  that edge through the public `retire_delivery`, which no modeled action
  performed. `deliver_event` in both backends now retires after its hook
  returns, and the public surface exposes only the retirement query
  (`delivery_retired`), matching the model's retirement finality: a
  delivering registration is never retirable from outside. The
  deterministic interleaving on the delivery-claimed pause gate (RED at
  `e34965a5`, GREEN at `b3de9262`) is the executable witness; the
  clean-room `contract_observer` consumer checks atomic resolution,
  cancellation, the retirement query after the terminal, duplicate and
  unknown-id dispositions through the public vocabulary alone.
- **ProgressSource.tla** (target: progress source + notification fd): no
  mechanism change; `AsyncBackend::ProgressPass` was renamed/moved to the
  public `ProgressPass` in `async_io_context.hpp` (same fields, same
  producer). The progress race/loop/pass-bound/owed-residue suites are
  unchanged and green in all cells.
- **ShutdownCore.tla** (target: async_io_context five-state shutdown):
  `AsyncIoContext` keeps its exact member set and teardown order (design
  record §"chosen design"); only the construction edge moved behind
  `detail::make_context`/named factories. `shutdown_lifecycle_test` (9
  threadpool + 17 uring scenarios) is unchanged and green in all cells
  except the recorded RG1 LSan finding, which pre-dates PR B (§1).

Models with no live C++ target (EventCore, SemCore, MutexCore, CondCore,
RwCore, QueueCore, SelectCore — the F6 dispositions) are unaffected: PR B
deleted host-side code no model mapped to.

## 5. Public-layout note

`sizeof(Request<std::size_t>)` grew 40 → 48 and the stored identity is the
public `RequestId` (32 bytes; unchanged) rather than the internal
`detail::RequestKey`. This is an allowed breaking API change under D2
(public vocabulary in the handle), adds no allocation (Request remains a
value type; no per-request heap allocation was added — the bridge calls are
plain out-of-line calls), and the installed-surface ODR probe pins the new
layouts across TUs and macro views. `AsyncIoContext` (96) and `RequestId`
sizes are unchanged from baseline.

## 6. Review-fix record (2026-10-16)

An external review of PR #545 at `e34965a5` raised two P1 and two P2
findings; this section records the fixes and their evidence at the
review-fix head.

| Finding | Fix | Evidence |
|---|---|---|
| P1: the observer retirement authority sat with whichever caller reached the core; `retire_delivery` granted `delivering → retired` while the pipeline hook was still on the stack, and a delivered registration pinned its slot forever without a public acquire call | Delivery owner retires after its hook returns in both backends; the public surface only queries (`delivery_retired`); `RequestCore::observer_retirement_acquired` is a read of the existing phase authority | RED counterexample at `e34965a5` on the delivery-claimed pause gate (`review-fix/observer-retirement-red.log`: mid-hook retirement granted, post-completion query false); fix commit `b3de9262`; GREEN seam suites (`threadpool_core_cutover_test` 17/17 incl. the new interleaving: query false while the delivery is claimed and the hook has not run, cancellation in-progress, hook-runs-once, owner-side retirement, slot reclaims with no public acquire; log captured in `review-fix/cutover-17-of-17.log`); 74/74 full group (liburing=y) |
| P1: the seam negative probes hand-wrote an incomplete `AsyncBackend` and any nonzero exit counted as the expected failure, so the gate could not detect a re-exposed backend | Probes include the real installed headers and pin diagnostics (`incomplete type`, `private`); the missing-file probes pin theirs; the gate is extracted for mutation self-testing | Commit `d8c57cd5`; self-tests compile a mutated staged install: re-exposing the backend definition fails the gate because the probe compiles, and a diagnostic mismatch is rejected (14 tests total) |
| P2: each P1 consumer ran twice (gate decided by one run, return code echoed from another) | Single execution with the return code captured for both the gate and the recorded detail | Commit `47a58e87` |
| P2: `SLUICE_B2_MUTANT_RELEASE_FORGETS_BINDING` lived in installed `request.hpp`, so a consumer define changed Request destruction | Mutation moved to `detail::RequestBridge::discard` in `request_core.cpp`; the mutant skip keeps the same externally observable failure | Commit `3bace3d4`; both release mutants re-killed with refreshed logs. The verifier now pins the complete installed mutation-region set (`NEGATIVE_V2_MUTANT_DEFINES_NOT_INSTALLED`, commit `51dc830f`), whose first run surfaced the two D1 template seams still in `request_scope.hpp` |

Re-verification at the fix head: full 8-cell matrix rerun (§1), both
package profiles re-verified with all 24 gates passing (§2), the observer model
correspondence restated (§4), the moved B2 mutants re-killed (§3), and the
RED counterexample retained as the regression's origin record.

### Independent review (round 2) dispositions

Two independent subagent reviews of the fix range (`e34965a5..21b09ec9`)
returned APPROVE-WITH-NITS with no critical findings and no counterexample
interleaving in non-mutant builds; their actionable findings are applied:

- The `delivery_retired` contract now states its precondition (query while
  the binding is retained; after reclaim the identity stops resolving) —
  the conservative direction of error is retained (never premature release).
- The package consumer's cancel-retirement idiom no longer re-cancels for
  an already-retired registration: it asserts the cancel disposition and
  waits for the retirement query, and the already-terminal attach branch
  expects `not_registered` from a cancel with no registration.
- The `RequestCore` retirement query is `const` like its sibling queries.
- Evidence honesty: the gate count is stated per profile (24, manifests
  now recording every verdict), the RED log carries its reproduction recipe
  (worktree at `e34965a5` plus the scenario spelling the pre-fix API), the
  cutover 17/17 run is captured (`review-fix/cutover-17-of-17.log`), the
  mutant kill logs carry their configuration and command, and the
  interleaving wording states the observed window (delivery claimed, hook
  not yet run) rather than "mid-hook".
- Authority contraction (AGENTS mechanism gate): the dead
  `retire_delivered_episodes` sweep removed; the shutdown settlement keeps
  its owed-delivery drain and registration-convergence obligation.
