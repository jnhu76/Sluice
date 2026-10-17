# F6 Rounds 4–5 — Package Surface and Current-Claim Integrity (frozen master 821c489f)

## R4. Installed package surface (re-derived, not trusted from receipts)

Method: fresh `xmake f -m debug --liburing=n -c -y`, build, then
`xmake install -o /tmp/f6-prefix` for both targets; every number below was
recomputed from the resulting prefix and from the debug archives at
`build/linux/x86_64/debug/`.

| Claimed (F5 closeout) | Re-derived (F6) | Match |
|---|---|---|
| INSTALLED_HEADERS = 25 | 25 (9 core-side: `sluice/*.hpp` + `blocking/*` + `detail/*`; 16 async-side: `async/*.hpp` + `async/detail/*` minus `context_identity.hpp`) | yes |
| ASYNC_ARCHIVE_MEMBERS = 13 | 13 objects in `libsluice_async.a` (11 `src/async/*.cpp` + 2 explicit `src/async/detail/*.cpp`) | yes |
| LIBRARY_OBJECTS = 15 | 15 total (2 core + 13 async) | yes |
| archives deterministic (`ar -D`) | members carry zeroed mtime/uid/gid | yes |

Usage requirements and declarations match the artifacts:
- `SLUICE_HAS_LIBURING` is exported as a **public** define of `sluice_async`
  exactly when the profile resolves, because `uring_backend.hpp`'s class
  layout is `#if`-guarded on it (ODR usage requirement; xmake/libraries.lua).
- `SLUICE_LIBURING_PROFILE` is exported unconditionally as the config
  identity define (#520), so both profiles compile to distinguishable,
  honest configurations.
- Consumers must take the defines via the library's public requirements, not
  hand-copy them; no conflicting declaration path exists in the installed
  tree.
- A stale pre-F5 `libsluice_async.a` (37 members) found under
  `build/linux/x86_64/release/` is a local build artifact, not a package
  claim; nothing cites it.

## R4. Clean-room consumers (installed prefix only, no source/build tree)

Raw inputs and outputs: `raw/cleanroom-consumers.txt`, `raw/w01_direct.cpp`,
`raw/w02_scope.cpp`, `raw/w03_external_loop.cpp`. All three compile with
`-Wall -Wextra -I/tmp/f6-prefix/include`, link only against the installed
archives, and run to completion; `ldd` shows only libc/libstdc++/libgcc/libm.

- **W-01 direct**: links `-lsluice_core` only (`sluice_async` never named) —
  the direct/core firewall holds end-to-end at the installed surface.
- **W-02 scope**: `File`/buffers → context → scope ordering, capacity-1
  second-submit rejection (V18), canonical `wait_for → take → finish`.
- **W-03 external loop**: `progress_notification_fd()` borrow,
  `acknowledge_progress_notification()`, mandatory `detach_progress_host()`
  before shutdown. The clean-room first attempt without the detach got the
  contracted `invalid_state` refusal — recorded as positive contract
  evidence, not a failure.

Known cosmetic residual: one `-Wextra` unused-parameter warning originates
in the installed `detail/reference_ready_sink.hpp` (F6-F09). The project
gate compiles at `-Wall -Werror`, so this does not break any current gate.

## R4. W-04 profile

W-04 (StackfulIoHost) is **not shipped/supported**: its headers install as
OPTIONAL_CANDIDATE-labeled closure, construction fails honestly without
liburing, no support claim exists anywhere current, and ADR-0003 carries
`DESIGN_AUTHORITY_ONLY` while ADR-0004's host-facing clauses stay
candidate-scoped. Therefore `W04_EXTERNAL` is recorded as
`N/A_WITH_NEGATIVE_PACKAGE_PROOF` (conditional gate: "conditional on
shipped/supported profile" — the profile is not shipped), not an
unconditional PASS.

## R5. Current-claim integrity (receipts vs actual state at the frozen tree)

| Claim (source) | Actual | Verdict |
|---|---|---|
| 66/66 tests, liburing=y (F-phase receipts) | re-run 66/66 at frozen tree | consistent |
| 52/52 tests, liburing=n (F5 receipt) | re-run 52/52 after clean `xmake f --liburing=n -c` | consistent |
| F2/F3/F5 CLOSED, F4 ADOPTED_SCOPED (ledger dated receipts) | merge SHAs and evidence directories all present on master | consistent |
| #519/#520/#521 pre-F6 infra | all three merged/closed pre-F6; referenced by master head commits | consistent |
| #402 CURRENT overlay ("F5 post-merge CI outstanding; #519/520/521 open; F6 PENDING") | overlay is stale as a projection (all resolved/merged since); dated receipts remain accurate | overlay-only staleness (F6-F01); receipts authoritative |
| TLA safety subset (Observer/ProgressSource/Shutdown) | re-run clean at frozen tree (numbers in raw/tla-results.txt) | consistent |
| RequestCore liveness (E2 receipt: ~26.6M states, 2h50m) | fresh re-run launched; status honestly recorded (F6-F04) | receipt stands; re-run supplementary |
| Lean corpus reused for v1 closure | REUSE = 0 per formal evidence map; no current claim cites Lean | consistent |
| #475 external use of retired surfaces | status UNKNOWN (no evidence either way in-repo) | not zero; recorded for release planning (F6-F18) |

Reverse claim scan (claims with no current evidence, or evidence with no
claim): the only model-side mismatch is `RequestCore.tla`'s `ReleaseBind`
compatibility flavor (F6-F03); the only doc-side staleness items are
F6-F01/F05/F06/F07/F15; no current ledger receipt cites a retired asset as
closure evidence.
