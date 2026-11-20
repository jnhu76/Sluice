# AUD-001 canceled_ops lock-domain synchronization (2026-11-21)

P0 remediation record for audit finding AUD-001 (#643). Branch
`fix/643-aud001-canceled-count-lock-domain`, based on master
`9c630d1f5ff14da01a09d5c9cada8a2aa5ca2279`. Independent of the AUD-002
PR (`fix/643-aud002-fatal-error-locked-probe`).

## 1. Baseline

- Base SHA: `9c630d1f5ff14da01a09d5c9cada8a2aa5ca2279` (master, 2026-11-21).
- Host: Fedora 44, kernel 7.3.3-200.fc44.x86_64, liburing 2.13,
  GCC 16.2.1, xmake 3.1.2.
- Pre-fix baseline suites: release `--liburing=n` 60/60; release
  `--liburing=y` 81/81.

## 2. As-built counter protocol (revalidated at the base SHA)

`AsyncStats` (`include/sluice/measurement.hpp:89`) is a plain struct of
eleven `std::uint64_t` counters; `canceled_ops` has no synchronization of
its own. Production increment sites at the base SHA:

1. `ThreadPoolBackend::cancel_key()` — `tally_canceled()`
   (`threadpool_backend.hpp:232`, `++stats_->canceled_ops`) called at
   `src/async/threadpool_backend.cpp:806`, **after** the `work_mtx_`
   critical section of `cancel_key` has closed.
2. `UringAsyncBackend::cancel_key()` — `bump(stats_,
   &AsyncStats::canceled_ops)` (`src/async/uring_internal.hpp:62`)
   called at `src/async/uring_transport.cpp:602`, **after** the
   `dispatch_mtx_` critical section of `cancel_key` has closed.
3. `UringAsyncBackend::finalize_operation_terminal_()` — the same `bump`
   at `src/async/uring_completion.cpp:78`, **inside** `dispatch_mtx_`
   (via `handle_one_cqe`), taken when a kernel-canceled operation's CQE
   settles.

Every other `canceled_ops` writer already runs under the backend mutex
that the surrounding path holds. Reader classification (audit v1.1
correction): the three bench programs read the counter only after
`driver.join()` (W-03 `workload_w03.cpp:456`) or after `ctx.shutdown()`
(W-02 `workload_w02.cpp:317`/`:456`, pipeline A/B `:339`), i.e. sequenced
behind full settlement — **not** concurrent readers. The full
out-of-production enumeration: `apps/` contains no `AsyncStats` access;
`bench/bench_common.hpp` only formats scalar copies at output time; the
dated research A/B harnesses (`docs/research/mw-hw-1/...`,
`docs/evidence/h2b-.../zero_burst_ab.cpp`) diff stats only after
`ctx.shutdown()`. The proven defects are therefore unsynchronized
**write-write** pairs, not the audit's write-read framing; the audit's
atomic-counter rationale is weaker than stated, while the race finding
itself stands.

THREAD-01 (contract §13, "request cancel / ID cancel: Yes with submit,
poll, admission close") makes two concurrent winning cancels a legal
execution. The shutdown driver's `cancel_outstanding_for_stop_()`
(`async_io_context.cpp:455`) calls the same `backend_->cancel_identity`
→ `cancel_key` path, so a public cancel concurrent with the driver's
stop-cancels forms the same pairs.

## 3. Validated failure interleaving

Reproduced deterministically by `tests/cancel_stats_race_test.cpp`:

- ThreadPool case: one worker is parked at the worker-claimed pause gate
  holding the first read, so two later submissions stay queued and
  unclaimed; two pre-created threads call `cancel_key_for_test` on them,
  both return `won_before_execution`, and both execute the unlocked
  `tally_canceled()` (`:806`) — a write-write pair on `canceled_ops`.
- io_uring case: `UringConfig{4, 2}` with a switchable transport hook;
  two filler submissions occupy both prepared slots, so C and D remain in
  the dispatch queue; two cancel threads both win and both execute the
  unlocked `bump` (`:602`) — a write-write pair, additionally the cross
  pair against the completion-side bump at `uring_completion.cpp:78`
  taken under `dispatch_mtx_` by the owner.

Symbolized TSan RED (5/5 runs, two race reports each; full log
`tsan-red-prefix.log`):

```text
Write of size 8 ... ThreadPoolBackend::tally_canceled()
  include/sluice/async/detail/threadpool_backend.hpp:234
  <- cancel_key  src/async/threadpool_backend.cpp:806   (lock released)
Previous write of size 8 ... tally_canceled() :234
  <- cancel_key  src/async/threadpool_backend.cpp:806   (other thread)

Read of size 8 ... bump()  src/async/uring_internal.hpp:62
  <- cancel_key  src/async/uring_transport.cpp:602       (lock released)
Previous write of size 8 ... bump() :62
  <- cancel_key  src/async/uring_transport.cpp:602        (other thread)
```

The suite also asserts the semantic oracle `stats.canceled_ops == 2`
(exactly-once counting per winning cancel) and full settlement of all
submitted requests (accepted-work, cancellation terminals, physical
counts).

## 4. Root cause

The cancel path tallied the metric on a different serialization domain
than the counter's owner: the increment executed after releasing the
backend mutex while every other `canceled_ops` writer holds that mutex.
Concurrent legal cancels (and legal cancel-vs-owner-completion
interleavings on io_uring) therefore performed unsynchronized
read-modify-writes on a plain `std::uint64_t` — a data race (UB) whose
benign-adjacent manifestation is a lost update (two wins counted once).

## 5. Minimal implementation

`src/async/threadpool_backend.cpp` and `src/async/uring_transport.cpp`:
move the `won_before_execution` increment inside the already-held
critical section of `cancel_key` (TP: `work_mtx_`; uring:
`dispatch_mtx_`), immediately after the publication-pending push; the
post-lock `signal_ready_progress()` ordering is untouched.
`production.diff` is the complete production change — two moved lines,
no new state, no new lock, no public-header change. This also brings the
driver's `cancel_outstanding_for_stop_` tallies under the same lock, and
orders the uring cancel-side bump against the completion-side bump at
`uring_completion.cpp:78`.

Alternatives rejected:

- **Relaxed-atomic `AsyncStats::canceled_ops`**: changes the installed
  public header (all `AsyncStats` copies/snapshots would need a
  user-defined copy constructor; bench programs copy the struct),
  introduces an RMW on a different domain while every other counter
  stays plain, and addresses no proven reader race (bench reads are
  sequenced per §2). It would also silently narrow the documented
  thread-model question to one field.
- **io_uring-only fix**: leaves the identical TP pair, which the same
  reproducer exposes; incomplete.
- **Synchronized snapshot/copy accessor on `AsyncStats`**: new public
  mechanism with no current consumer (no concurrent reader exists in
  tree); violates the no-speculative-framework rule.

The `AsyncStats` thread-model statement (whole-struct reads are safe
behind settlement; concurrent cancels are now exactly counted) is
recorded here and in the ledger, not as C++ comments.

## 6. Tests executed and RED/GREEN evidence

Deterministic regression: `tests/cancel_stats_race_test.cpp`, registered
as `cancel_stats_race_test` (all profiles) and `cancel_stats_race_uring_test`
(liburing only), compiled with `SLUICE_ASYNC_INTERNAL_TESTING` from the
same production TUs as the existing integration targets.

- TSan, debug, base SHA + test only: 5/5 runs report the two pairs above
  (`tsan-red-prefix.log`).
- TSan, debug, fixed: 10/10 runs, 0 race reports, PASS
  (`tsan-green-postfix.log`).
- Release fixed: 10 consecutive full-suite runs green (see matrix).
- The exactly-once assertions (`canceled_ops == 2`, both terminals
  `canceled`, claimed read completes with 4 bytes) pass on every build;
  on the base tree the value check depends on the RMW interleaving, so
  the RED evidence is the TSan race, not the value check.

Matrix (all 0 failed):

| Config | Tests |
|---|---|
| release, liburing=y | 83/83 (×10 consecutive) |
| debug, liburing=y | 83/83 |
| release, liburing=n | 61/61 |
| debug, liburing=n | 61/61 |
| asan+ubsan, liburing=y | 83/83 |
| tsan, liburing=y | 83/83 |

Benchmark output consistency: fixed-tree W-02 (17 rows) and W-03 (7
rows) JSONL carry `stats_canceled_ops` in every row (value 0 in these
workloads), JSON parses — `bench-w02-stats.jsonl`,
`bench-w03-stats.jsonl`. Installed-package verification:
`scripts/verify_v2_package.py --mode release --liburing` run in a clean
worktree of the committed PR head — all gates PASS including
`V2_B_PROVENANCE_RECORDED` (`package-verify-clean.log`; the primary
worktree is dirtied by pre-existing untracked `docs/evidence/`
directories, hence the clean-tree rerun). No installed header or
archive baseline changed.

## 7. Public compatibility assessment

- No installed header changes; `AsyncStats` layout, copyability and all
  public signatures untouched.
- Observable behavior change: none for sequenced readers (same counts,
  same order); for concurrent legal cancels the count is now exact
  instead of racy-possibly-lost — a defect repair, not a semantic change.
- The thread-safety contract of the other ten counters is unchanged and
  not silently altered: they remain safe under their existing lock
  domains and whole-struct reads remain sequenced-reader territory, as
  before this fix.
- Test-only surface: new `SLUICE_ASYNC_INTERNAL_TESTING` targets.

## 8. Performance effect (focused measurement)

The increment moved inside an already-held mutex — no added
synchronization operation — so a focused A/B was run rather than a
claimed-neutral argument: deterministic unclaimed requests with the
worker parked at the claim gate, one public `Request::cancel` per
request, 200 000 iterations per run, `taskset -c 2`, 25 interleaved
base/fixed pairs (one-shot probe kept in `/tmp` per repository hygiene;
workload fully described above and in `cancel-path-ab.txt`).

- base median 606.7 ns/iter (range 599.9–654.2), fixed median 608.9
  (range 601.1–620.4);
- median ratio fixed/base 1.0036, bootstrap95 [0.9965, 1.0079] —
  interval spans 1.0, no resolved regression;
- no runtime improvement is claimed for this correctness fix.

## 9. Remaining assumptions and limitations

- The regression's race verdict requires the TSan build; non-TSan builds
  run the functional/deterministic-schedule case only.
- `AsyncStats` remains a plain struct: concurrent whole-struct readers
  are still outside any guarantee (unchanged contract). W-03's
  `AsyncStats before = stats` delta pattern copies other counters
  (`poll_calls`, `completed_ops`) that concurrent writers update; in W-03
  the copy is taken before the driver is joined — a pre-existing
  test-side pattern outside this PR's scope, recorded here as an audit
  note, not fixed.
- The audit's original reader-race examples were corrected in §2
  (bench reads are sequenced); this correction is recorded for the
  audit-provenance reconciliation and does not weaken the write-write
  finding that motivated the fix.
- Formal models do not cover statistics counters; evidence class is
  deterministic C++ + TSan + focused A/B, matching the audit's stated
  validation class for this finding.

## 10. Review status

PR: branch `fix/643-aud001-canceled-count-lock-domain` (separate from
the AUD-002 PR; both touch `xmake/tests.lua` and
`docs/roadmap/v2-conformance.md` in disjoint regions). Not merged;
Owner gate pending.
