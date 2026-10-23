# DP-1 publication-payload elimination — compiler proof (WSL2, GCC 15.2.0)

Baseline: master `be28f275` + benchmark foundation B0 `9445bc82`
(branch `perf/bench-w03-stop-race-stats-wiring`). Candidate: this branch
(DP-1 frozen patch `docs/research/data-movement-dp0/results/receipts/e5/dp1.patch`
plus named mutant seams). Compiler: `g++ (Ubuntu) 15.2.0`, release `-O3`,
`-std=c++20`, NDEBUG, no LTO — the shipped release configuration.

Reproduction:

    g++ -std=c++20 -O3 -DNDEBUG -DSLUICE_HAS_LIBURING -I include -I src/async \
        -S src/async/uring_backend.cpp -o out.s
    # threadpool TU: same flags without SLUICE_HAS_LIBURING, file src/async/threadpool_backend.cpp
    # function bodies extracted between the label and the .size directive
    # (extraction recorded in the campaign report; files in this directory)

## Findings

| function | arm | frame | `rep stos` (payload init) | calls | new/malloc |
|---|---|---|---|---|---|
| UringAsyncBackend::publish_one | base | 232 B | 1 (`movl $15,%ecx; rep stosq` = 120 B) | 8 | 0 |
| UringAsyncBackend::publish_one | dp1 | 96 B | 0 | 8 | 0 |
| UringAsyncBackend::publish_zero_op_inline | base | — | 1 | — | 0 |
| UringAsyncBackend::publish_zero_op_inline | dp1 | — | 0 | — | 0 |
| ThreadPoolBackend::publish_one | base | 232 B | 1 | 8 | 0 |
| ThreadPoolBackend::publish_one | dp1 | 96 B | 0 | 8 | 0 |
| ThreadPoolBackend::publish_zero_op_inline | base | — | 1 | — | 0 |
| ThreadPoolBackend::publish_zero_op_inline | dp1 | — | 0 | — | 0 |

- The 120-byte payload initialization is physically executed in every base
  publish function (`movl $15, %ecx; rep stosq`) and is absent in every dp1
  function.
- The call sequence is unchanged (context → begin_publication →
  complete_publication → claim_observer_delivery → retire_observer_delivery);
  begin_publication still receives its `PublicationPayload*` parameter (now
  `nullptr`), so the out-form of the publication protocol is retained.
- No new `malloc`/`operator new` appears; no equally sized hidden temporary is
  reconstructed (publish_one's stack frame shrinks 232 B → 96 B, the payload's
  120 B slot plus bookkeeping).
- GIMPLE dumps (`.007t.gimple`) for the same TUs are retained in the campaign
  worktree and referenced by the PR description.

Binary identity for the W-01 negative control is asserted by the DP-0 E5
study (byte-identical `bench_w01_direct` across arms) and re-checked in this
campaign's E5 stage.

## Named mutant kills (both recorded manually, never run as regular tests)

- `uring_publish_mut_begin_skipped`
  (`SLUICE_URING_MUTANT_PUBLICATION_BEGIN_SKIPPED`): skips the
  `begin_publication` call; the first submission reaches
  `complete_publication` → `not_in_flight` → `uring_core_handoff_fail_fast`.
  Killed with the named fail-fast "core handoff reached a protocol-unreachable
  state (invariant violation)", rc=134 (SIGABRT).
- `uring_publish_mut_foreign_key`
  (`SLUICE_URING_MUTANT_PUBLISH_DELIVERS_FOREIGN_KEY`):
  `BOUNDED_LIVENESS_KILL` — delivers a wrong-generation ReadyEvent; the real
  key's observer phase is never retired, settlement never completes, and the
  external-loop harness's own `alarm(120)` watchdog terminates the run
  (rc=142 after test 8/9 passed). This is the bounded-supervision kill for
  the lost-retirement state, not an eager identity check: no oracle rejects
  the foreign key up front.
- Premature reclaim is owned by the existing `public_request_test`
  publication-epilogue pause-gate oracle (pauses `publish_one` between grant
  and completion, requires pending for the consumer) and the existing
  `SLUICE_REQUEST_CORE_MUTANT_RECLAIM_IGNORES_CONTROL_REFS` mutant of the
  owed-counter parity suite; both pass on this arm unmodified.
