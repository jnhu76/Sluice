# F2 authority contraction — execution record

Owner slice: #402 F2 (Request/Result/Identity authority contraction, clean
break). Baseline master: `0ed16588b7101f108aa7282074ba3d568f9d34e9`
(F3 and F4 closed). Worktree: independent worktree `/home/jnhu/Source/Sluice-f2`
on branch `f2-clean-authority`. The original `master` worktree was not modified.
Draft PR: jnhu76/Sluice#516 (unmerged; the installed/public API break still
needs the owner's separate decision — see `installed-surface-risk-disposition.md`).

Implementation head under test is the branch head recorded in
`docs/evidence/f2-authority-contraction-20261013/gates.json`; the SHA in that
file is authoritative for every capture in this directory.

## Commit structure

1. `refactor(async)!: key-canonical scheduler await and observer attach` —
   `Scheduler::await_request(RequestId)` / `cancel_waiter(RequestId)`;
   `AsyncIoContext::attach_observer(RequestId)` / `cancel_observer(RequestId)`;
   `runtime_waiter_observer_test` migrated; the `WaitRecord` completion
   back-pointer removed.
2. `refactor(async)!: retire Batch and the busy-poll op helpers` —
   `batch.hpp`/`batch.cpp`, `op_helpers.hpp`/`op_helpers.cpp` deleted; the two
   carried invariants re-expressed on `wait_one` (sticky health verdict through
   the `health_failure` outcome with the claimed owner released; zero-length
   submission settles at submission with no driving demand).
3. `refactor(async)!: remove the caller-owned Completion authority and residual
   arena vocabulary` — `completion.hpp`, `request_handle.{hpp,cpp}`,
   `request_arena.hpp`, `request_slot.hpp`, `submit_transaction.hpp` deleted;
   the backend vtable is core-only (`submit_*(Op) -> Result<detail::RequestKey>`);
   `Completion`-typed context/backend spellings removed; the uring chain's
   `TerminalResult` replaced by `IoOutcome`; `SlotHandle` collapsed into
   `RequestReservation`; `SubmitStage` moved to `detail/request_core.hpp`
   (testing only); 16 arena/completion fail-fasts removed; all canonical
   consumers migrated.
4. `test(package): observe backend availability without the retired identity
   probe`.
5. `test(external-loop): settle R14 on the published obligation, not on a wake`
   — see "Load-sensitivity investigation" below.
6. `test(conformance): scope the ring control tag to the ring profile` — a
   pre-existing Clang `-Werror` break in the no-liburing branch of
   `tests/backend_conformance_e1_test.cpp` (the baseline fails identically at
   the same line), surfaced because the F2 matrix is the first configuration
   that builds that test under Clang without liburing.
7. `test(external-loop): keep the waited binding alive across the drain guard`
   — a stack-use-after-scope that this slice's migration introduced and
   AddressSanitizer found: the drain guard dereferences the bindings it settles
   from its destructor, and the migrated scenario declared one of them after the
   guard. The baseline form held that work in a `Batch`, which owns its storage.
8. `fix(scheduler): report a settled result after admission closes` — a real
   behavioural regression of this slice, found by the production review:
   `await_request` mapped every refused attach other than `already_terminal` to
   `invalid_state`, but `register_observer` answers `admission_closed` before it
   looks at the slot, so an already-published request reached the caller as a
   failure after `close_admission()`. The fallback now asks the canonical
   readiness, as the pre-F2 await did through `Completion::ready()`. The
   regression test is deterministic and fails on the pre-fix mapping.
9. `refactor(core)!: leave one release edge for the public binding` — a second
   release authority found by review: `release_public_binding` accepted releases
   during `publication_inflight` (the removed caller-owned `Completion`'s rule)
   while `discard_public_result` requires strictly published. Retired; the two
   protocol scenarios that pinned the retired verdicts are re-expressed on the
   canonical ones and keep their safety content.
10. `test(external-loop): re-assert the wake after a racing acknowledgement` —
    restores R14's second half deterministically.
11. `docs(evidence): correct the F2 record after independent review`.
12. The W-03 root-cause closure series (test-only; see `w03-root-cause.md`):
    deterministic reproducer `359155e6`, application handoff eventfd
    `b63a532e`, write-result portability fix `25734314`, receipt `9f919ae2`.
    The production diff is empty — the stall was an application-side handoff
    defect in the scenario's own loop protocol.
13. `fix(uring): retire a post-accept push that lands after poison`
    (`8689a720`) — closes `known-preexisting-gaps.md` §1; see
    `uring-poison-post-accept/README.md`.
14. `test(e1): guarantee pause-gate lifetime through teardown` (`e974988c`) —
    closes `known-preexisting-gaps.md` §3; see `e1-gate-lifetime/README.md`.
    Test-only: no production file changed.
15. `fix(context): reject retire_delivery on a moved-from context`
    (`0bcfda01`) — closes `known-preexisting-gaps.md` §2; see
    `retire-delivery-moved-from/README.md`.
16. `docs(evidence): reconcile rcore-live through exact historical inheritance`
    (`093332ba`) — `formal/rcore-live-reuse.md`; the TLA gate stays
    `INCOMPLETE: rcore-live` in `gates.json` with the reuse identity recorded
    separately.

## Independent review

Two adversarial reviews ran against the slice: one over the production diff
(hidden second authorities, publication/lifetime assumptions, UAF and
stale-generation windows, stranded requests, false ordering claims, unjustified
state) and one over the evidence and test-coverage integrity (lost failure
modes, weakened assertions, vacuous checks, missing binding, undisclosed
surface). Outcome:

- Fixed in the slice: the `await_request` regression (commit 8), the duplicate
  release edge (commit 9), the R14 coverage gap (commits 5 and 10), the
  ASan-detectable lifetime defect the migration introduced (commit 7), the
  matrix runner's silent tolerance of a failing `xmake test` and of a shrunken
  build set, the package harness's vacuous source-path check and missing
  before-side positive controls, the installed-probe documentation, and the
  installed-surface list (scheduler/observer `Completion` spellings, `BatchOp`,
  the 16 retired fail-fast diagnostics, the retired release edge).
- Corrected in the ledger: `cancel_intent` arbitrates nothing and is read only
  by the protocol seam; `ThreadPool` has no poison state of its own (only
  io_uring does); `release_public_binding` is retired and
  `claim_execution`/`acquire_execution` are classified as protocol-test surface.
- Recorded as pre-existing and not fixed here: the io_uring poison-versus-
  post-accept-push window that can strand an accepted request until the caller
  cancels it (byte-identical at the baseline), and the unguarded `core_` in
  `retire_delivery`. See `known-preexisting-gaps.md`. (Superseded at the final
  head: all recorded gaps are corrected there — see "Corrective round"
  below.)
- Confirmed sound by the production review: one settlement authority, no
  surviving second state machine, publication strictly ordered before any public
  observation, reference accounting covering slot reuse, at-most-once observer
  delivery, and no accepted request lost on the traced backend paths.

## Requirement mapping (MIG-02 → this slice)

| Root disposition | Executed |
|---|---|
| caller-owned `Completion<T>`: compatibility only, remove as canonical result authority | Removed entirely; `CALLER_OWNED_COMPLETION_CANONICAL_AUTHORITY = 0` |
| `RequestArena`: converge to bounded core; preserve obligations rather than exact state layout | No instance existed at the baseline commit; the residual vocabulary is deleted and every named obligation is owned by `RequestCore::Slot` |
| `RequestHandle`: separate the public move-only responsibility from internal copied identity | Handle spelling removed; `Request<T>` is the only move-only settlement responsibility and `RequestId` the only copied identity |
| busy-poll helpers: replace with explicit Request/progress/scoped paths | `op_helpers` deleted; both carried invariants re-expressed on the canonical paths |
| Scheduler/Fiber substrate | Retained under the adopted HOLD decision; only its await family was migrated to the canonical identity spelling |
| Optional W-04 host | Untouched; no F2 work forces it in and nothing in this slice depends on it |

## Canonical surface after the contraction

- Submission: `ctx.submit_read(ReadOp) -> Result<Request<std::size_t>>` and the
  sibling `submit_*` overloads; backend vtable
  `submit_*(Op) -> Result<detail::RequestKey>`.
- Settlement: `Request<T>::ready()`, `try_result()`, `take_result()`,
  `discard()`, `cancel()`, `id()`.
- Identity: `RequestId` (context/slot/generation) plus
  `AsyncIoContext::lookup(RequestId) -> RequestReadiness`.
- Observer: `attach_observer(RequestId)` / `cancel_observer(RequestId)`;
  scheduler: `await_request(RequestId)` / `cancel_waiter(RequestId)`.

## Removed mechanisms

See `minimal-state-ledger.md`. Summary: the `Completion<T>` state machine with
its own storage, the dual arena/core binding and `reap_seq`; `RequestHandle` and
its readiness-mapping layer; the dead `RequestArena`/`RequestSlot`/
`submit_transaction` family; `Batch`; the busy-poll `op_helpers`; the 16
arena/completion fail-fast diagnostics; the delivery-record completion fields and
the `WaitRecord` completion back-pointer.

## Execution DAG

```
0ed16588 (baseline master, F3+F4 closed)
  └─ S1 key-canonical scheduler await and observer attach
      └─ S2 retire Batch and the busy-poll op helpers
          └─ S3 remove the caller-owned Completion authority and arena vocabulary
              └─ package consumer updated to the canonical availability observation
                  ├─ S4a R14 oracle restored on the published obligation (test-only)
                  │    └─ S4b ring control tag scoped to the ring profile (test-only)
                  │        └─ S4c the waited binding outlives the drain guard (test-only, ASan)
                  ├─ S5 the await refusal fallback consults canonical readiness (production)
                  │    └─ S6 one release edge for the public binding (production)
                  │        └─ S7 R14 wake re-assertion restored (test-only)
                  │            └─ evidence refresh, captures re-run at this head
                  └─ (S4a..S7 all fold into the single head captured by gates.json)
```

Each production commit is followed by its own deterministic oracle in the same
commit: S5 by `already_ready_await_survives_closed_admission`, S6 by the
canonical verdicts in the two publication scenarios. Every capture in this
directory names the final head; the re-run was triggered by S5..S7.

## Validation executed

| Gate | Evidence |
|---|---|
| Six configuration profiles (Debug/Release × no-ring/real-ring, native Clang ASan+UBSan and TSan scopes) | `matrix/` runner plus the captured profile trees under the execution root; per-target build/run logs, `results.tsv`, effective `xmake.conf`, toolchain cache naming the compiler program actually used, and the raw `xmake test -v` log |
| Versioned B-01 v5 clean-room dual-profile package capture | `b01-v5-candidate/` (`reproduce.py`, `persist.py`, `commands.json`, `summary.json`, `raw-package-evidence.tar.gz`, `raw-manifest.json`) |
| Installed-header standalone compile, link and cross-TU ODR probes | `installed-probes/` (`probes.json`, per-header logs, the negative control on the before prefix) |
| Named mutation builds | 64 mutant targets, two runs each, compared against the same campaign on the baseline worktree; the kill sets are identical and no mutant target compiles a test source edited by this slice, so the set is invariant by construction |
| TLA+ gate | `formal/` — `scripts/verify_tla.sh` run against the pinned v1.7.4 TLC jar (SHA-256 verified). 170 of the 171 TLC configurations finished with `PASS` verdicts and none failed; `rcore-live` was still running when the capture was closed. Its documented cost on this model is ≈18.5 h wall with a ≈40-48 GB heap (see the E1/E2 records in `docs/roadmap/v1-conformance.md`), so the run was capacity-bound rather than verdict-bound. No model file changed in this slice (empty `git diff 0ed16588..HEAD -- formal/`), and the baseline's own full-gate PASS covers that unchanged configuration. The gate is therefore recorded as `INCOMPLETE: rcore-live` in `gates.json`, never as a pass. |
| GitHub CI | run on the branch head recorded in `gates.json` |
| W-01/W-02/W-03 installed consumers | B-01 v5 consumer rows: each links and runs from the installed prefix in both ring profiles |

## Load-sensitivity investigation (commit 5)

The first six-profile matrix run showed `xmake test -v` failing exactly one
scenario in the release+liburing profile: `threadpool_external_loop_test`'s
`host_acknowledgement_racing_new_signal_never_strands`, while all 67 direct runs
in that profile passed. Because the machine was simultaneously running the TLA+
gate (ten TLC workers, load average ≈ 35), the failure had to be attributed
before it could be accepted or dismissed.

Measurements, all reproduced on both trees:

| Condition | Pre-F2 `Completion` cell | Request-based cell on the pre-F2 library | Request-based cell on F2 |
|---|---|---|---|
| Quiet machine, 20 concurrent copies, 60 runs | 0 failures | — | 0 failures |
| 30 CPU burners, 10 concurrent copies, 30 runs | 0 failures | 6 failures | 6 failures |
| Blocked-owner wake latency, 20 rounds, quiet | — | — | 0.12 ms mean, 0 timeouts, 0 missed wakes |
| Blocked-owner wake latency, pre-F2 library | — | 0.12 ms mean, 0 timeouts, 0 missed wakes | — |

The failure rate follows the *test cell*, not the library: the pre-F2 library
with the migrated cell fails exactly as often as F2, and the wake path itself is
intact on both trees. The mechanism is that the old cell pre-armed a
caller-owned `Completion` destination before submitting, so the host could
observe readiness even when its acknowledgement had already consumed the
incoming wake; the migrated cell publishes the binding only when
`submit_read` returns, and an acknowledgement that wins the race then leaves the
host with nothing to observe until its own poll timeout. The assertion it broke
(`wakes <= 2` inside a 2000 ms poll) therefore measured scheduler latency, not
the protocol.

Commit 5 restructures that one cell: it waits for the fact under test (the
submitter published the obligation), then requires one acknowledged drive to
deliver it, the request to be ready, the notification fd to be quiescent and the
zero-length result to be consumable. No assertion was dropped, no wait was
inserted where a protocol transition is required, and the post-restructure cell
no longer reports protocol failures for scheduler starvation. A submitter that
never ran now drains and reports a clean failure instead of aborting in the
context destructor (that abort signature is pre-existing in this suite and
appears on the baseline tree as well under the same load).

## Corrective round at the final head — 2026-10-13

Final implementation head `093332ba1b40b667763d1202200ee899962454ae`
(`gates.json` in this directory). Three corrections, each with its own
deterministic RED/GREEN receipt, plus the TLA reconciliation:

| Correction | Kind | Receipt |
|---|---|---|
| io_uring poison racing the post-accept push (strands an accepted request between accept and queue push when a non-retryable transport failure poisons the backend first) | production: `retire_undispatched_after_poison_locked_` drains the provably kernel-invisible reservation through the failure terminal under `claim_execution` arbitration; exactly-once terminals preserved (a racing pre-dispatch public cancel keeps its own publication) | `uring-poison-post-accept/` |
| E1 `GateGuard` teardown race (stack gate storage could die while a registered pass still owed its exit acknowledgment) | test-only: the guard waits for `exited==true` and worker-side gates are declared before the backend so they outlive worker join; ownership ordering closes the unsynchronized pointer-load window no protocol can close | `e1-gate-lifetime/` |
| `retire_delivery` moved-from `core_` dereference | production: benign rejection matching the sibling entry points' moved-from contract | `retire-delivery-moved-from/` |
| `rcore-live` formal acceptance | evidence: `INCOMPLETE` at this capture, `VERIFIED_REUSABLE` historical proof with byte-identical inputs (`git diff eb7bf055..HEAD -- formal/tla/ scripts/verify_tla.sh` empty) | `formal/rcore-live-reuse.md` |

Authority-contraction audit for the corrective round: no new runtime state,
flag, counter, token, lease or capability probe was added to any class. The
only new flag is the function-local `post_poison_push_retired` wake-necessity
bit in `dispatch_after_accept` (derived: poison fact plus drained queue;
materialized because a wake owed after the post-poison retirement would
otherwise be indistinguishable from the poison wake already signalled). No
existing mechanism became eligible for removal by these corrections; the
retired dispatch-queue seat is reclaimed through the existing
`publication_pending_` path.

Final validation re-run at `093332ba`, every capture naming that head:

- Six-profile matrix: debug-n 53/53, debug-y 67/67, release-n 53/53,
  release-y 67/67, asanubsan-n 18/18, tsan-n 17/18 with the recorded
  `request_scope_test` link exclusion. One intermittent, preserved not hidden:
  the first tsan-n pass failed one scenario
  (`r4_worker_completion_signal_before_final_recheck_is_caught`, no TSan race
  report) while the B-01 clean-room builds ran concurrently on the same host —
  the load-contamination hazard this slice's own protocol warns about. The
  identical matrix binary passed 10/10 on the idle machine and the isolated
  serial re-run passed; the contaminated capture is preserved as
  `tsan-n-concurrent-attempt` next to the clean `tsan-n` in the matrix root.
- B-01 v5 clean-room dual-profile package capture:
  `F2_B01_V5_CANDIDATE_FRESH_PACKAGE_PASS` for
  `0ed16588…` → `093332ba…`, both ring profiles; 56 → 49 installed headers,
  35 → 32 library objects, 33 → 30 `libsluice_async.a` members; persisted in
  `b01-v5-candidate/`.
- Installed probes at the same prefixes: 49 headers compile standalone, the
  cross-translation-unit ODR identity probe links and runs, zero removed
  headers present after, all seven present in the before-prefix control.
- Focused differential stress, `taskset -c 0,1`, ten concurrent copies per
  round, four interleaved rounds per tree, debug + real-liburing, otherwise
  idle 20-core host: 240/240 runs pass (40/40 per suite per tree for
  `uring_progress_race_test`, `backend_conformance_e1_threadpool_test`,
  `threadpool_external_loop_test`; baseline worktree `0ed16588` vs this head).
  Campaign script, environment, results and per-run logs: `final-stress/`
  in this directory (`campaign-runs.tgz`).
- GitHub CI run `38397035018` at `093332ba`: all four jobs success
  (`ci/head-run.json`).
- TLA+ gate retained: 170/171 verdicts PASS, `rcore-live` outstanding,
  recorded as INCOMPLETE; the historical E2 full-gate PASS covers that
  configuration through the verified reuse identity above. The models do not
  cover backend transports, so the A-fix needs no model change.

## Remaining limitations

- Installed public surface break: seven installed headers and three compiled
  objects are retired. The risk/disposition record is prepared; the slice stays
  unmerged while the owner's separate decision on that break is pending.
- The mutation campaign is unchanged by commit 5 and by the corrective round:
  no mutant target compiles the edited test sources.
- The TLA+ gate at this capture is `INCOMPLETE: rcore-live`; acceptance for F2
  is by exact-input reuse of the E2 full-gate proof
  (`formal/rcore-live-reuse.md`), never recorded as a fresh pass.
- W-03 is **CLOSED** (application handoff; production diff empty; see
  `w03-root-cause.md` and `blocker-w03-stall.md`); the focused differential
  stress at the final head re-confirms 0 failures under the recorded
  contention schedule.
- All three pre-existing gaps recorded in `known-preexisting-gaps.md` are
  CORRECTED at this head with deterministic RED/GREEN receipts.
- F5 and every phase beyond F2 are not started here. Optional W-04 remains
  unsupported and no F2 obligation depends on it.
