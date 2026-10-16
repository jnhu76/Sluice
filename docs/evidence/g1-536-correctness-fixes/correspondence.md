# #536 formal correspondence — what the fixes change relative to the models

The question this note answers: do the correctness fixes on top of the frozen G1
adoption set invalidate the #524/G1 formal receipts, and if the modeled
transition relation is untouched, what is the precise argument?

## 1. The model text is identical

Every formal asset involved is byte-identical to the tree the G1 receipts were
taken at (`0891be17`):

```text
$ git diff --stat 0891be17 HEAD -- formal/ scripts/verify_tla.sh scripts/verify_formal.sh
(empty)

4ad6dbe5cd4af19e12a846f06818622e099a37899474206fdd6984e445725aa1  formal/tla/RequestCore.tla
82c31d615fdeb73d947342b2a8990072c738069cfa2e1363455a53314f7f8edf  formal/tla/RequestCore.cfg
062c8c3e6b873a5bfd518856952ceacd942dab2f08bf9e6145c34be4f771f09d  formal/tla/RequestCoreLive.cfg
2e38fabae8a4224e98b34124675c325f5fd8353f4ce9661c9ced7bae3d90a756  scripts/verify_tla.sh
936a262061c914694dfd669a543be24573c45d5aa0ff20a8b96b23d01e050e88  tla2tools.jar (pinned v1.7.4)
```

The first four hashes equal the #524 `correspondence-after` table and the jar
hash equals the pinned value; therefore the model instance under check is the
same instance, and a re-run must reproduce the same verdict unless the
environment changed. This note does not rest on that alone: the affected gates
were re-run at the fixed head and their raw logs are in `formal/raw/tlc/`.

## 2. What the fixes change in the production code

Two commits, five files (`git diff --stat 54631b5f 64e3241b`):

| Commit | Change | Touches a modeled action? |
|---|---|---|
| `b88750e3` | `ThreadPoolBackend::poll_progress` freezes its pass boundary at entry: one `work_mtx_` hold drains the queued publications and snapshots the owed delivery records into construction-time storage; it delivers at most those entries, and reports queued publications *and* owed events as immediate work | no |
| `64e3241b` | the suppression test's temporary file gets an explicit RAII lifetime plus a descriptor-count assertion | test-only |

Nothing else changed: no `RequestCore` member, no observer protocol call, no
`ProgressSource` state, no shutdown path, no public header of the seam types.

## 3. Why no modeled transition changes

The `RequestCore` model's action vocabulary maps one-to-one onto `RequestCore`
members (`Accept`, `ClaimExec`, `PhysicalOutcome`, `BeginPublish`,
`CompletePublish`, `CancelRequest`, `AcquireCtl`/`RetireCtl`, `Consume`,
`DiscardPublic`, `Reclaim`, `Reserve`/`Rollback`). `poll_progress` is not in
that vocabulary: it is the driver-side *scheduler of* those actions. The fixes
change how many of them one invocation performs and in what groups, never what
any one of them does:

1. **Publications.** Both versions take the same entry snapshot
   (`publication_pending_.size()` at entry) and call `publish_one` on exactly
   those entries in the same FIFO order. The pre-fix version pushed them into a
   `std::vector` that grew on demand; the fixed version pushes into a
   construction-time `BoundedHandleRing`. The sequence of modeled
   `BeginPublish`/`CompletePublish` pairs and its order are identical.
2. **Owed deliveries.** Pre-fix, the pass took owed records one at a time,
   lowest slot index first, until none were owed — so an owed event *created
   during* delivery (a delivery callback submitting work that completes
   immediately) was absorbed into the same invocation. Fixed, the entry
   snapshot is taken once, lowest slot index first, and events created later
   stay owed for the next pass. For the entry set the two orders coincide; the
   difference is only that later-produced events form a later pass.
3. **Control reference pairing.** Both versions perform
   `deliver_event(key, kind)` immediately followed by
   `release_control(key)` for each owed entry, one release per delivery. The
   one-control-ref-per-owed-event pairing (`DeliveryRecord::event_owed` pairs
   with a control ref on `owed_key`) is unchanged, so `RetireCtl` and the
   invariants that depend on it (`InvReleasedNotLive`, `InvReclaimedNotLive`)
   are untouched.
4. **Critical sections.** The fixed pass takes `work_mtx_` once to snapshot
   instead of once per delivered event. Modeled actions are atomic, so a
   narrower or wider critical section around *bookkeeping* is not a modeled
   transition. What matters for the protocol is that deliveries still run
   outside the lock: both versions call `deliver_event` after releasing
   `work_mtx_`, so no observer callback runs under it.
5. **Records cleared before delivery.** The fixed pass clears all snapshot
   records' `owed_key` up front instead of one at a time. `owed_key` is written
   by the worker completion path and read only by the pass (`grep owed_key`
   finds exactly those sites), so no other party can observe the difference.
   The same is true of `event_owed`, which besides the pass is read only by
   `any_event_owed_locked_()` (quiescence and residue reporting).

## 4. The liveness assumption the fix has to keep

The `RequestCoreLive.cfg` obligations are `L1` (accepted → published) and `L5`
(reclaimable → reclaimed) under weak fairness of `ClaimExec`,
`PhysicalOutcome`, `RetireExec`, `BeginPublish`, `CompletePublish` and
`Reclaim`. The C++ realization of that fairness assumption is: **while work
remains, the driver keeps running passes.** The residue report is what carries
that obligation across the driver boundary, and this is exactly where the fix
had to be careful:

* A pending owed delivery means the request is published
  (`complete_publication` ran) but its observer event is not yet delivered, and
  the C2 counter has already been decremented — so the pending delivery is *not*
  visible in `accepted_work_remains`. Only `immediate_work_remains` can report
  it.
* Pre-fix this was consistent because the owed loop was exhaustive: a pass could
  not leave an owed event behind. Bounding the pass without extending the report
  would have created a state — residue that the driver cannot see — in which
  `AsyncIoContext::wait_one` returns "no progress" and the W-03 external loop
  parks. The fix therefore extends `immediate_work_remains` to
  `!publication_pending_.empty() || any_event_owed_locked_()`, computed under
  the same lock that the drain used.
* With that report, a pass that leaves either queue non-empty makes the driver's
  wait return without parking, so the next pass runs and the enabled action is
  still eventually taken. The bounded pass is a stuttering refinement of the
  unbounded one with respect to the modeled variables: identical action
  sequence over the entry snapshot, remainder deferred to a pass the driver is
  obliged to run.

`UringAsyncBackend::poll_progress` already reported residue this way
(`pass.immediate_work_remains = !publication_pending_->empty() ||
has_immediate_physical_work()`) before this slice; the ThreadPool backend now
matches that shape for its own two queues. The fix moves the two backends
closer to one modeled driver contract, not further from it.

## 5. The notification side is not touched

`ProgressSource` code is unchanged by both commits. The C1 suppression rule
(a notification write may be skipped only while the counter is provably
non-zero, cleared only where the counter is zero) is what makes "an owed event
exists ⇒ the fd is readable until acknowledged" hold, which is what lets a
parked driver wake for the deferred pass. `ProgressSource.tla` is unchanged.

## 6. Verdict and its scope

**Correspondence preserved.** The modeled transition relation over the modeled
variables is unchanged, so the #524/G1 receipts remain valid for the fixed
head, and the gates were re-run at it for reproducibility rather than to re-earn
the proof:

| Gate | Fixed head | #524/G1 record |
|---|---|---|
| `RequestCore.cfg` safety | see `formal/raw/tlc/safety.log` | PASS, 31,289,609 generated / 5,359,732 distinct, depth 53, 48 s |
| `RequestCoreLive.cfg` safety+liveness | see `formal/raw/tlc/liveness.log` | PASS, 12/12 temporal branches, 64,316,784 total distinct states, 24 min 20 s |

Scope limits, stated so they are not mistaken for more than they are:

* the models are bounded instances; they do not prove C++ memory ordering, the
  pass's own atomicity against concurrent submitters, kernel behavior, or
  wall-clock progress — the pass bound and residue report are C++ evidence
  (`raw/negative-oracle/`, `raw/positive-oracle/`), not model results;
* the fairness assumption above is an assumption, not a theorem: a driver that
  stops calling `poll_progress` while residue remains violates PROG-01 and the
  model cannot see it;
* the historical 71/72 mutation record's survivor is unchanged in scope and is
  a mutation-campaign statement, not a formal one — it is reproduced in
  `raw/mutation/` for this head.
