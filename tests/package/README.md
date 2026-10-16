# tests/package — clean-room external consumers (v2)

This directory holds the clean-room package consumers for the v2 contracted
surface (PR B). The consumers here are **not** registered with the xmake test
graph: they must never build against the repository `include/`, `src/` or
`build/` trees. They are compiled and run only by
`scripts/verify_v2_package.py`, against an installed prefix produced by
`xmake install`.

- [v2 Architecture and Contract](../../docs/explicit-io-contract.md) — the
  authority for the 11-header install surface (Appendix B).
- [v2 conformance ledger](../../docs/roadmap/v2-conformance.md) — the release
  gate this evidence feeds.
- `docs/review/f1-*` — the superseded F1 clean-room baseline (25-header
  pre-contraction surface). Frozen records are retained as history; they are
  not v2 evidence and must not be reproduced with the retired
  `--check-frozen` protocol.

## Clean-room rules

1. Consumer sources use angle-bracket includes only (the verifier rejects
   quoted includes, which could reach source-tree headers relative to the
   consumer file).
2. Compile/link commands reference the installed prefix only; the verifier
   records every command and fails if any include/library path flag resolves
   into the repository. Consumer builds run with a whitelisted environment,
   and an include/library path variable (`CPATH`, `LIBRARY_PATH`, …) pointing
   into the repository aborts the run.
3. No macro view is imposed on consumers: the installed v2 headers read no
   build-profile macro, and the ODR probe proves public layouts agree across
   opposite `SLUICE_HAS_LIBURING` views.

## Running

```sh
python3 scripts/verify_v2_package.py \
    --manifest-out docs/evidence/pr-b-boundary-contraction-20261016/v2-package-manifest-noliburing.json
python3 scripts/verify_v2_package.py --liburing \
    --uring-prefix /path/to/liburing/prefix \
    --manifest-out docs/evidence/pr-b-boundary-contraction-20261016/v2-package-manifest-liburing.json
```

Each run writes the manifest (and an archive/object/symbol baseline next to
it) and prints the gate summary; exit status 0 = all gates PASS. Evidence for
a v2 claim is a fresh manifest per run, recorded under the PR B evidence
directory.

## Consumer map

| Consumer | Exercises |
|---|---|
| `w01_direct.cpp` | P0 (core lib only): W-01 workload + async types stay incomplete from core headers |
| `w02_pipeline.cpp` | W-02 workload: bounded positional ops, short I/O, capacity exhaustion, second-submit failure, exception/early-return cleanup, best-effort cancel |
| `w03_external_loop.cpp` (argv: `threadpool\|uring`) | W-03 workload: application-owned epoll loop, notification fd, interruption, ordered teardown; uring selection reports the explicit `not_supported` outcome when the profile is absent |
| `contract_request_lifetime.cpp` | REQ-01/HANDLE-01..03: ready-unconsumed, try/take, move/self-move, stale identity, release+resubmit |
| `contract_admission.cpp` | SEM-03/ERR-01: precedence, zero-op request, full table, admission close |
| `contract_cancel_effect.cpp` | CANCEL-01/ERR-02: cancel dispositions, effect preservation |
| `contract_observer.cpp` | OBS-01..03: attachment resolution, delivery retirement (observed through the public retirement outcome, not a callback), cancellation, duplicate, unknown id |
| `contract_progress.cpp` | PROG-01..04: no-lost-wake through epoll, wake-on-submit, fd re-borrow |
| `contract_shutdown.cpp` | SHUT-01..03: retained results survive execution close, idempotent re-shutdown |
| `contract_durability.cpp` | SEM-06/07: request-path metadata ops, resize coverage through request sync |
| `contract_backend_availability.cpp` | PROD-02/BACKEND-02: factory selection, capacity through admission, explicit invalid-config failure, `not_supported` uring absence with no fallback |
| `odr_two_tu_{a,b}.cpp` | ODR discriminator: the verifier compiles TU-B with the OPPOSITE `SLUICE_HAS_LIBURING` view; every public type layout must agree across TUs and views |
| `negative_async_symbols_in_core.cpp` | link against core lib only MUST fail (async symbols are not in the core archive) |

Synthesized negative probes (no consumer source): every uninstalled in-tree
header (the 14 that left the install set plus the new internal seam headers)
must fail to compile from the prefix; the deleted host headers must be absent
from tree and prefix; deriving/constructing `AsyncBackend` or constructing
`AsyncIoContext` from a `unique_ptr<AsyncBackend>` must fail — the backend
seam is not installable surface.

Evidence layering: these consumers are Layer A (public observable contract).
Race/fault discrimination that cannot be made deterministic from outside
stays with the internal deterministic tests and fault seams (Layer B) and the
TLA+ models (Layer C).
