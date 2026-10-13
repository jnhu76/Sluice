# R14 oracle load sensitivity — attribution record

This directory records why `threadpool_external_loop_test`'s
`host_acknowledgement_racing_new_signal_never_strands` scenario was restructured
in commit `test(external-loop): settle R14 on the published obligation, not on a
wake` (branch `f2-clean-authority`), and why that change is not a weakened
oracle.

## The observation that started it

The first six-profile matrix run failed exactly one `xmake test -v` row — this
scenario in the release + real-liburing profile — while all 67 direct target runs
in the same profile passed. The machine was simultaneously running the TLA+ gate
(ten TLC workers, load average ≈ 35), so the failure had to be attributed before
it could be accepted or dismissed.

## Measurements

`raw/f2-release-y-summary.json`, `raw/baseline-release-y-summary.json` —
twenty concurrent copies of the two trees' release+liburing test binaries, three
rounds each (60 runs per tree), interleaved, under thirty CPU burners:

| Cell | Library | Runs | Failures |
|---|---|---|---|
| caller-owned `Completion` (pre-F2 cell) | pre-F2 `0ed16588` | 30 | 0 |
| Request-based shared cell (migrated cell) | pre-F2 `0ed16588` | 30 | 6 |
| Request-based shared cell (migrated cell) | F2 | 30 | 6 |

`raw/quiet-interleaved.json` — the same three-way comparison on a quiet machine
(load ≈ 4), twenty concurrent copies per burst, three rounds: 0 failures for
every cell. The instrumented single-suite comparison that first showed the
asymmetry is in `raw/f2-release-y-summary.json` and
`raw/baseline-release-y-summary.json`.

`raw/f2-wake-probe.cpp` — the property itself, measured directly: a progress
owner blocked in `poll()` and a zero-op submitted from another thread, twenty
rounds, on both trees. Both trees: 0.12 ms mean wake latency, 0 timeouts, 0
missed wakes. No production wake was lost; the notification machinery in
`include/sluice/async/detail/progress_source.hpp` is byte-identical in both
trees.

`raw/f2-race-probe.cpp` — the same scenario with timestamps, used to see where a
2000 ms poll timeout comes from.

Conclusion: the failure rate follows the test cell, not the library. The old
cell pre-armed a caller-owned destination before submitting, so the host could
observe readiness even when its own acknowledgement had already consumed the
incoming wake. The migrated cell publishes the binding only when `submit_read`
returns, so an acknowledgement that wins the race leaves the host with nothing
to observe until its own poll timeout — the assertion measured scheduler
latency, not the protocol.

## The restructured cell

States the fact under test (the submitting thread published the obligation)
before asserting, then requires one acknowledged drive to deliver it, the
request to be ready, the notification fd to be quiescent afterwards and the
zero-length result to be consumable. Nothing was dropped: the pre-restructure
cell also asserted delivery, readiness and settlement, but only inside a
wake-count bound that no protocol obligation supports. A submitter that never
ran now drains and reports a clean failure instead of aborting in the context
destructor.

The first post-restructure check under the same load (thirty burners, ten
concurrent copies, three rounds) still produced four failures, all of them the
suite's pre-existing teardown abort when a host wait expires under absurd load,
including one exit where the reason was the submitter never being scheduled
within the wait; the pre-F2 binary produces the same abort signature on the
baseline tree, and the scenario now drains and reports a clean failure instead
of aborting when that happens. After the scenario was extended to re-assert the
wake (see below), the same check reported zero protocol failures.

## What the restructure preserved and what it initially dropped

The restructure replaced a wake-count budget with a deterministic statement of
the first half of R14 — an acknowledging drain must not strand an obligation
that is already published — and the load measurements above show that half is
now measured against a fact rather than against scheduler placement. It
initially dropped the second half: that a *later* signal must still wake a
parked host after an acknowledgement has drained a readiness. That half is
restored deterministically in the same scenario by parking a second host in
`poll(2)` with the fd driven quiescent, then submitting from the main thread:
the parked host can only be released by the new signal, so a drained readiness
provably cannot swallow the next one. No timeout budget is involved and no
assertion was dropped in the final form.
