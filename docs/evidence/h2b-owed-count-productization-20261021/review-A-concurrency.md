# Review A — Concurrency and Semantics: H2b owed-count skip (#564)

Reviewer: independent general-purpose agent (background), 2026-10-21, on
branch `perf/uring-owed-count-skip` at `4edb957d`, against master `c8970702`.
Adversarial brief: actively construct counterfactual interleavings that could
break the invariant, attack the Scheme-B unreachability claim and the W-03
oracles; read-only.

## VERDICT: CORRECTNESS_APPROVED

Nits N1–N6 listed; none merge-blocking on correctness grounds. N1's
documentation correction is applied in `review-a-n1` (this PR).

## 1. Invariant parity — SOUND under all enumerated interleavings

Complete write-site enumeration (grep-verified across include/ and src/ — no
other writers exist):

| Transition | Site | Pairing |
|---|---|---|
| Init | uring_backend.hpp:192,294; uring_backend.cpp:163 | 0 == 0 |
| Zero-op publish | uring_backend.cpp:565–573 | flag set + `++owed_count_` in ONE `dispatch_mtx_` CS |
| Sweep consume | uring_backend.cpp:657–669 | flag clear + `--owed_count_` in ONE `dispatch_mtx_` CS |
| Submit reset | uring_backend.cpp:488–500 | unpaired flag clear, but guard + write in same CS and the guard calls `[[noreturn]]` `uring_core_handoff_fail_fast()` (fail_fast.hpp:27) — the unpaired transition never survives |

Every read of `owed_count_` is under `dispatch_mtx_` (651, 664;
`owed_visible_locked_` at 313 under 311; seams uring_test_seams.hpp:91–99).
Flag and counter move together inside single critical sections of one mutex,
so the invariant holds at every protected observation point by construction.
Per-transition results:

- Duplicate publish / reset: guard read and reset write are one CS; the
  noreturn fail-fast means the unpaired clear cannot be observed.
- Cancellation of an owed request: zero-ops are terminal at accept
  (request_core.cpp:212–217); cancel resolves
  `terminal_chosen → already_terminal` (272–274) and touches no control refs
  or delivery records. Parity scenario 6 covers it.
- Shutdown with owed outstanding: the settlement driver keeps passing because
  counter>0 → `immediate_work_remains` true (313); convergence additionally
  requires the authoritative scan (`internal_work_retired`, 365–373) and
  `control_refs == 0` (async_io_context.cpp:457–465). Destructor fail-fasts
  on residue (338–357). No path clears delivery flags on shutdown
  (`poison_and_recover_locked_`, `retire_undispatched_after_poison_locked_`
  never touch `delivery_`).
- Concurrent submits during a pass: all mutations serialized by
  `dispatch_mtx_`; increments are one-to-one with false→true transitions.
- Owed-during-pass: flag+counter land together under the mutex regardless of
  sweep position; deterministically exercised by the
  DeliverySweepEpiloguePauseGate scenarios.

The single `owed_count_` read at 651 and mid-sweep increments — conservative
miss, not a correctness problem: the increment at 571 strictly precedes
`signal_ready_progress()` at 575–576; `wait_one`/`drive_settlement_` snapshot
the ProgressSource token before each pass (async_io_context.cpp:597, 498) and
`wait_if_unchanged` revalidates the epoch under the same mutex before
parking, so any signal issued after the snapshot defeats the park;
`poll_progress` re-reads the counter after `poll()` returns (306 → 311–313),
so a sweep-missed increment surfaces as same-pass residue. Worst case is
one-pass deferral, never stranding.

K=0 skip cannot hide nonzero owed: counter==0 implies all flags false at any
protected observation. Exactly-once is carried by the per-slot flag, not the
counter — two concurrent sweeps or a reentrant `poll()` from an `on_ready`
callback (`deliver_event` runs outside the lock) cannot double-deliver and
cannot deadlock.

Non-interference: `publish_one`'s unlocked `kind` read (585–589) cannot race
the sweep — queued slots never carry `event_owed`. Transport/completion
control refs pin only router entries created by `dispatch_after_accept`
(non-zero ops); a zero-op slot's sole control ref is the owed pin acquired at
555, released only at 671.

## 2. Scheme-B unreachability — HOLDS; no CORRECTNESS_REJECTED path found

The chain: publish acquires the owed control ref before setting the flag
(555 → 568); the only release of that ref is the sweep after clearing the
flag (662 → 671); `reclaimable_` requires `control_refs == 0`
(request_core.cpp:588–598); only `release_slot_` pushes to the free list
(607–631), reached only via guarded `try_reclaim_` or `rollback` (222–230,
reserved-phase only — never owed); `reserve()` pops only free slots
(158–190). Attacks enumerated, all closed: cancel touches no refs;
`discard/consume_public_result` clear only `binding_live`; `release_execution`
has the premature-rejected guard (407–412); generation advance occurs only
inside `release_slot_`, after the flag is clear; `force_generation_for_testing`
(694–700) is test-only and frees nothing; the fail-fast fires noreturn, so
the subsequent unpaired `record.event_owed = false` (498) is unreachable in
the drift case. Empirically: the M4 mutant makes the state reachable and the
guard fires (SIGABRT 134, reproduced). The guard is load-bearing evidence,
not defensive insurance.

## 3. W-03 oracles — genuine; no sleeps-as-sync, no vacuous passes

- No sleeps-as-sync: pause gates + atomic wait/notify only; the 150 ms
  `poll(2)` calls are negative assertions (fd must stay quiet);
  `concurrent_producers`' 2M-iteration bound is a work bound whose break
  requires the exact delivery count; post-loop assertions enforce
  exactly-once (equality, not ≥).
- Ack-doesn't-consume-owed (S1): submit 1 → fd readable → ack (drains) →
  submit 2 → fd readable again → ONE pass delivers `completed == 2`
  (equality) → settle pass delivers 0 → fd quiet through the 150 ms timeout.
  Exactly-once proven in both directions.
- S2 deterministically constructs publish-during-pass, asserts wake
  persistence on the borrowed nfd, honest residue verdict, owner transfer,
  settled quiet backend.
- S3 requires `wait_one` to serve owed inside its own pass (`completed == 1`,
  not `deadline_expired`) and to return quiescent progress-0 afterwards —
  exercising the new counter verdict in `wait_one`'s idle return.
- S4 composes observer + real kernel completion + owed + `shutdown(drain)`
  convergence; rounds bounded, each round requires a successful service.
- All five mutant receipts independently reproduced on this tree:
  M1=142, M2=1 (`FAIL: counter_tracks_zero_op_reads`), M3=1 (residue verdict
  oracle — not a counter comparison), M4=134, M5=134. M5's kill path: dropped
  wake → S1's first `fd_readable` fails → early return skips
  `host.stop_and_detach()` → `~AsyncIoContext` progress-binding fail-fast →
  SIGABRT. Deterministic. Clean suites: parity 8/8, residue 3/3, external
  loop 9/9, all exit 0.

## 4. Nits and dispositions

| # | Nit | Disposition |
|---|---|---|
| N1 | README §8 understated the drift failure mode: M1 is a settlement **hang** (driver parks on the notification fd; `settlement_converged_` false while `immediate_work_remains` false), so post-change liveness of the settlement driver depends on parity | **Fixed** in this PR: README §8 rewritten (parity is liveness-load-bearing); ledger PROG-01 row notes it |
| N2 | Comment at uring_backend.cpp:491–493 sits on the AGENTS.md line (invariant note vs narration) | **Kept after adjudication**: the pinning chain is invisible to local structure and the guard otherwise reads as deletable dead code — exactly the local-mistake category AGENTS.md permits; substantive home is README §3 + ledger (both present) |
| N3 | Scenario comments narrate beyond naming (parity test 70–72, 205–208, 286–289; external loop S1/S2) | **Accepted**: consistent with the existing suites' style; no semantic contract is created in comments |
| N4 | Publish strictly between two per-slot lock holds inside a live sweep covered only statistically | **No action** (reviewer: correctness follows from the same pairing argument; pause-gate tests cover the deterministic neighbors) |
| N5 | New scenarios leak `FILE*` via inline `::fileno(tmpfile())`, matching pre-existing pattern | **Accepted as consistent with suite pattern**; harmless at process exit, asan cell green |
| N6 | Docs commits not part of the two reviewed commits | **Satisfied by PR composition**: the ledger/README updates are commits `5956624d`+`1fb4c9c2` on the same PR, and the PR is the merge unit |

## 5. Hygiene

No new allocation, mutex, atomic, thread, or configuration. Seams are locked
inline readers; xmake additions follow the existing `uring_progress_target`
pattern (mutants `set_default(false)`, never regular tests); the
request_core.cpp change is a pure `#if defined(...)` seam wrap with identical
production semantics. BACKEND-04 respected: destructor and
`internal_work_retired()` retain the authoritative scan; the counter decides
only the sweep skip and the residue verdict. Commit split is clean.
