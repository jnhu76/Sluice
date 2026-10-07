# tests/package — clean-room external consumers (F1)

This directory holds the F1 clean-room package baseline (#402 Phase F1). The
consumers here are **not** registered with the xmake test graph: they must
never build against the repository `include/`, `src/` or `build/` trees. They
are compiled and run only by `scripts/verify_f1_package.py`, against an
installed prefix produced by `xmake install`.

- `docs/review/f1-clean-room-package-baseline.md` — the F1 record (gates,
  installed-set decisions, audit results, adversarial review).
- `docs/review/f1-requirement-evidence-matrix.md` — requirement → observation →
  probe → internal evidence → formal asset mapping.
- `docs/review/f1-package-manifest-{noliburing,liburing}.json` — frozen
  machine-readable manifests (before-contraction baseline for F2–F5).

## Clean-room rules

1. Consumer sources use angle-bracket includes only (the verifier rejects
   quoted includes, which could reach source-tree headers relative to the
   consumer file).
2. Compile/link commands reference the installed prefix only; the verifier
   records every command and fails if any include/library path flag resolves
   into the repository. Consumer builds run with a whitelisted environment,
   and an include/library path variable (`CPATH`, `LIBRARY_PATH`, …) pointing
   into the repository aborts the run.
3. The macro view (`SLUICE_HAS_LIBURING`) is supplied by the verifier from the
   manifest so every consumer TU sees the same layout the archive was built
   with (ODR rule).

Profile tiers are packaging labels: P0/P1/P3 carry the CANONICAL surfaces,
P1 additionally carries the OPTIONAL_CANDIDATE host closure (the H-30/H-31
rows: `cancel.hpp` + `fiber.hpp` + `fiber_ctx.hpp`) and the
COMPATIBILITY/INTERNAL/TEST_ONLY headers the current public closure reaches.
No OPTIONAL_SUPPORTED claim exists anywhere in F1.

## Running

```sh
python3 scripts/verify_f1_package.py \
    --manifest-out docs/review/f1-package-manifest-noliburing.json \
    --archive-baseline-out docs/review/f1-archive-baseline-noliburing.json
python3 scripts/verify_f1_package.py --liburing \
    --uring-prefix /path/to/liburing/prefix \
    --profile-name P2-liburing \
    --manifest-out docs/review/f1-package-manifest-liburing.json \
    --archive-baseline-out docs/review/f1-archive-baseline-liburing.json
```

Each run writes the manifest and the archive/object/symbol baseline to the
recorded paths and prints the gate summary; exit status 0 = all gates PASS.
`--check-frozen` reproduces a frozen manifest at the current head (the full
verification reruns, then the fresh manifest must match the frozen one outside
the volatile provenance/timestamp fields and the archive-baseline sha256 must
match):

```sh
python3 scripts/verify_f1_package.py \
    --check-frozen docs/review/f1-package-manifest-noliburing.json \
    --manifest-out /tmp/f1-check-manifest.json \
    --archive-baseline-out /tmp/f1-check-baseline.json
```

A freeze run requires a clean worktree at start: the recorded
`VERIFICATION_HEAD` must identify the exact content that was built, installed
and probed.

## Consumer map

| Consumer | Profile | Exercises |
|---|---|---|
| `w01_direct.cpp` | P0 (core lib only) | W-01 workload + V25 clean-room firewall (async types stay incomplete) |
| `w02_pipeline.cpp` | P1 | W-02 workload: bounded positional ops, short I/O, capacity exhaustion, second-submit failure, exception/early-return cleanup, best-effort cancel |
| `w03_external_loop.cpp` | P1/P2 (argv: `threadpool\|uring`) | W-03 workload: application-owned epoll loop, notification fd, interruption, ordered teardown; uring reports explicit UNAVAILABLE when the profile is absent |
| `w04_stackful_candidate.cpp` | P1 (+P2) | W-04 candidate host (OPTIONAL_CANDIDATE only): sequential-looking tasks, H1/H2/H4/H5 externally observable paths |
| `contract_request_lifetime.cpp` | P1 | REQ-01/HANDLE-01..03: ready-unconsumed, try/take, move/self-move, stale identity, release+resubmit |
| `contract_admission.cpp` | P1 | SEM-03/ERR-01: precedence, zero-op request, full table, admission close |
| `contract_cancel_effect.cpp` | P1 | CANCEL-01/ERR-02: cancel dispositions, effect preservation |
| `contract_observer.cpp` | P1 | OBS-01..03: attachment resolution, delivery, cancellation, duplicate, V08 subset |
| `contract_progress.cpp` | P1 | PROG-01..04: no-lost-wake through epoll, wake-on-submit, fd re-borrow (V23 subset) |
| `contract_shutdown.cpp` | P1 | SHUT-01..03: retained results survive execution close, idempotent re-shutdown |
| `contract_durability.cpp` | P1 | SEM-06/07: request-path metadata ops, resize coverage through request sync (V27) |
| `contract_backend_availability.cpp` | P1/P2 | PROD-02/BACKEND-02: named construction, explicit unavailability, no silent fallback |
| `odr_two_tu_{a,b}.cpp` | P1/P2 | ODR discriminator: two TUs observe the SAME `Result<std::size_t>`/`Request<std::size_t>` specializations plus the macro-sensitive `UringAsyncBackend` layout under one shared macro view; the binary cross-checks both TUs' facts |
| `negative_odr_view_{a,b,main}.cpp` | negative (P2) | two TUs compiled with OPPOSITE `SLUICE_HAS_LIBURING` views must report a divergent `sizeof(UringAsyncBackend)`; the binary fails (deterministically) when it observes the mismatch |
| `negative_async_symbols_in_core.cpp` | negative | link against core lib only MUST fail (async symbols are not in the core archive) |

Evidence layering (F1 §18 of the phase brief): these consumers are Layer A
(public observable contract). Race/fault discrimination that cannot be made
deterministic from outside stays with the internal deterministic tests and
fault seams (Layer B) and the TLA+ models (Layer C); the requirement matrix
records the correspondence per row.
