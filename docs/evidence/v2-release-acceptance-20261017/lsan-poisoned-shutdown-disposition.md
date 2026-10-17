# RG1 — LeakSanitizer finding, io_uring poisoned-shutdown path: fixed

Ledger row G7 "LeakSanitizer finding, poisoned-shutdown path" (formerly GAP
— release blocker). Fix: commit `ff7d6dde`
(`test(shutdown): isolate the unresolved-borrow lifetime witness in a
forked child`); no production code changed.

## Root cause (test-structural, not a production defect)

Scenario `poison_with_in_flight_cqe` in `tests/shutdown_lifecycle_test.cpp`
verifies the SHUT-03/BACKEND-03 poison semantics: a shutdown of a poisoned
context with a genuinely in-flight kernel borrow must return
`health_unresolved` (never a fabricated terminal), keep the real borrow
alive, and refuse `execution_closed`. The scenario intentionally keeps the
`PoisonedState` witness reachable past process teardown so the invariant is
observable — under LeakSanitizer that deliberate retention is reported as a
leak (direct: `PoisonedState` via `make_unique` at the scenario's
construction site; indirect: the whole still-poisoned context graph —
statx buffers, ring, borrow ledger, queues, slot table, Requests). The
leak is the test's retention of a live contract object, not an
unreleased production resource: the context is contractually not
destroyable while the borrow lives, so there is nothing production could
free.

Pre-existence: the same report at baseline `5c1b14db` is on record in
`docs/evidence/pr-b-boundary-contraction-20261016/matrix/asanubsan-y-rg1-lsan-disposition.txt`.

## Fix

The witness now runs in a forked child (`poison_unresolved_health_witness`)
which asserts the oracle facts and exits with `std::_Exit` — bypassing the
atexit leak check exactly so the deliberate retention is observable without
being a leak finding. The parent turns the child's exit code into the
verdict: 0 passes, 1 fails the named check, 2 (io_uring genuinely
unavailable) skips, 3 (the scenario failed to stage) fails the named
check; a fork or waitpid failure fails too. All oracle content is preserved:
no fabricated terminal, `health_unresolved` not faked as success, the real
borrow not released early, fail-fast destruction guards untouched.

## Evidence

- RED (pre-fix, GCC ASan+LSan, debug, liburing=y, baseline `c1facc52`):
  the LSan report naming the scenario and allocation site —
  `lsan-red-capture/rg1-lsan-red-stderr.txt` (direct leak `PoisonedState`
  at the witness construction; 19 allocations / 4016-byte summary
  region), matching the PR B baseline capture byte-for-byte in site and
  shape. GREEN and control captures are in the same directory with a
  provenance README.
- GREEN (post-fix): `shutdown_lifecycle_uring_test` passes under
  ASan+LSan+UBSan in the recorded matrix cells `asanubsan-n` and
  `asanubsan-y` (clang static runtime, leak detection on), and in every
  other cell; the full suite is green in all eight cells of
  `matrix/matrix.json`.
- No suppressions, no `detect_leaks=0`, no test removal: the scenario and
  its assertions are intact; only the process isolation changed.

Re-evaluation trigger: none — the forked-witness structure is the fix, and
the sanitizer cells now run the scenario leak-free at every pass.
