# Post-review revision (pre-merge re-review of PR #547)

A fourth, independent pre-merge review of the full PR returned
REVISE_BEFORE_MERGE: P0 = 0, P1 = 2 (evidence accuracy), P2 = 1 (test
failure path). This record names each finding, the fix, and the re-run
evidence. No production behavior changed in this revision; the only
executable change is the failure-path release in
`tests/uring_progress_race_test.cpp`.

## Findings and fixes

1. P1 — `docs/roadmap/v2-conformance.md`, G1 SEM-06 row: the row's status
   said the release-matrix enumeration was discharged while its gap column
   still read `release matrix enumeration outstanding (G10)` and its next
   owner was still "Release gate". Fixed: the discharged clause is removed
   from the gap column; the one real boundary ("durability claims rest on
   documented OS/filesystem guarantees") stays, next owner is `—`. This was
   the one row the third review's G10 sweep missed; the other two rows were
   already recorded discharged.

2. P1 — `docs/evidence/v2-release-acceptance-20261017/req01-identity-domain-evidence.md`:
   the implementation-facts bullet claimed identities are "claimed by CAS
   against the live context registry". No such registry exists. Fixed to the
   real mechanism in `src/async/detail/context_identity.cpp`: one global
   atomic cursor (`g_context_identity_cursor`, initialized to 1) whose CAS
   handout mints each value in `1..UINT64_MAX` at most once; `0` is never
   live (it is the unadopted slot-table value and the sticky exhausted
   marker `kContextIdentityExhausted`); at `UINT64_MAX` the winning CAS
   latches to `0` while yielding the last value once, and
   `allocate_context_identity` then fails fast. The oracles and the
   boundary-injection statement themselves are unchanged and were already
   accurate; only the mechanism description was wrong.

3. P2 — `tests/uring_progress_race_test.cpp`, cancel-race oracle: on an
   unexpected `cancel()` disposition the oracle returned while the owner
   thread was still frozen at the prepark pause gate, leaving the gate
   unreleased and the thread joinable — the thread destructor terminates the
   process instead of reporting the named oracle failure. Fixed: the
   disposition is captured, the gate resumed, the owner joined and the
   prepark counter unregistered first; the failure then reports the
   disposition explicitly. The same release-then-report discipline was
   applied to every other failure exit in the file that could occur while an
   owner thread is live (frozen at a pause gate or bound in a wait):
   the pause-gate oracles now call a single `release_parked_owner(ctx, gate,
   owner, prepark)` recovery, and the counter-observation oracles release a
   pipe byte, join and unregister before returning.

## Re-run evidence (`rerun/`)

Executed at the revision commit, on the same host/toolchain set as the
acceptance matrix (gcc 16.2.1, kernel 7.2.9, liburing 2.13). Every run
directory carries `argv.txt`, `exit.txt`, `log.txt` and `log.sha256.txt`.

| Run | Command | Result |
|---|---|---|
| `debug-y-test` | `xmake test -y` (debug, liburing=y) | exit 0 — 74/74 passed, 0 failed |
| `release-y-test` | `xmake test -y` (release, liburing=y) | exit 0 — 74/74 passed, 0 failed |
| `release-y-race-repeat-1..5` | `uring_progress_race_test` binary, five repeats | exit 0 each — 23/23 oracles passed each repeat |
| `mut-final-probe-run` | registered `progress_source_uring_mut_final_probe_dropped` binary | exit 1 — `FAIL: k1_k2_cqe_before_drain_recovered_by_final_probe` (mutant stays RED) |

The two cells are the release-matrix cells whose suites include the changed
test file (`uring_progress_race_test` exists only in liburing=y, and the
release cells are where the original wake-discrimination defect surfaced in
matrix round 0). Their configure/build argv match the recorded
`matrix/debug-y-*.argv` / `matrix/release-y-*.argv` cells. A docs-only fix
needs no matrix re-run; the test-file change is limited to failure exits
that the success path never takes, so the passing orientation of every
matrix cell is unchanged and the surviving evidence is the suite result
above.

Artifact digests: `tests/uring_progress_race_test.cpp`
sha256 `64af774e68580296919fc24d2b308ad40a57fd1a32cea27b7fd7e7018af09974`;
release-y mutant binary
sha256 `8fe711a15f562d6d75f8f551611fda9aa0a06f94423523acabd46a1fbefabe85`;
release-y race binary
sha256 `b398e68ec0d442ee91dd71752034ab4c6d996f4d6ba27716e079e7f4665c9c20`
(`mut-final-probe-run/argv.txt` names the built path for the mutant run).

## What did not change

No production header, source, xmake configuration or ledger status row
changes semantics in this revision; ledger rows keep their evidence
citations. The acceptance matrix, mutation-kill matrix, package
verification, and TLA records above stay as recorded at their heads; the
only executable delta after the matrix content head `72acc209` is the
failure-path release in `tests/uring_progress_race_test.cpp`, re-validated
here.
