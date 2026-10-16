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

`scripts/verify_v2_package.py`, both profiles, all gates PASS at committed
heads; manifests and archive/object/symbol baselines recorded next to this
file:

- `v2-package-manifest-noliburing.json` — 11-header exact-set prefix,
  11/11 sole-include compiles, workloads and micro-contracts on the public
  factories, `w03 uring` reports the explicit `not_supported` outcome
  (exit 2), ODR probe agrees across opposite macro views, all negative
  surface probes fail as required.
- `v2-package-manifest-liburing.json` — same surface, real io_uring
  selection (`w03 uring` exit 0), archive carries the io_uring external
  references and the `make_uring_context` symbol.

## 3. Mutation spot-check (`mutation/`)

The two B2 release-edge mutants whose code moved with the seam
internalization still die on their named oracles (built at debug-n, gcc):

- `public_request_mut_release_forgets_binding` (mutant stayed inline in
  `request.hpp`): killed by `discard_releases_the_published_binding` —
  the discarded binding stays live, the slot never re-admits, and teardown
  hits the outstanding-bindings diagnostic (rc 255).
- `public_request_mut_nonterminal_release_detaches` (mutant moved to
  `request_core.cpp` with the bridge): killed by
  `destroy_nonterminal_request` — "the always-on diagnostic did not
  terminate" (rc 255).

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
  the observer state machine is unchanged; `AsyncIoContext` now maps its
  outcomes onto public enums 1:1 (armed/duplicate/already_terminal/
  admission_closed/not_found; retired/delivery_in_progress/
  not_registered/not_found). The clean-room `contract_observer` consumer
  reproduces the OBS-01..03 discriminators (atomic resolution, at-most-once
  delivery via retirement, cancel/retry, duplicate) through the public
  vocabulary alone.
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
