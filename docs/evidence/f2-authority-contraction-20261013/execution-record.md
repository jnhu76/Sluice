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

## Validation executed

| Gate | Evidence |
|---|---|
| Six configuration profiles (Debug/Release × no-ring/real-ring, native Clang ASan+UBSan and TSan scopes) | `matrix/` runner plus the captured profile trees under the execution root; per-target build/run logs, `results.tsv`, effective `xmake.conf`, toolchain cache naming the compiler program actually used, and the raw `xmake test -v` log |
| Versioned B-01 v5 clean-room dual-profile package capture | `b01-v5-candidate/` (`reproduce.py`, `persist.py`, `commands.json`, `summary.json`, `raw-package-evidence.tar.gz`, `raw-manifest.json`) |
| Installed-header standalone compile, link and cross-TU ODR probes | `installed-probes/` (`probes.json`, per-header logs, the negative control on the before prefix) |
| Named mutation builds | 64 mutant targets, two runs each, compared against the same campaign on the baseline worktree; the kill sets are identical and no mutant target compiles a test source edited by this slice, so the set is invariant by construction |
| TLA+ gate | `formal/` — full `scripts/verify_tla.sh` run against the pinned v1.7.4 TLC jar (SHA-256 verified) |
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

## Remaining limitations

- Installed public surface break: seven installed headers and three compiled
  objects are retired. The risk/disposition record is prepared; the slice stays
  unmerged while the owner's separate decision on that break is pending.
- The mutation campaign is unchanged by commit 5: no mutant target compiles the
  edited test source.
- The TLA+ gate was run on the tree whose production code and models are
  identical to the final head; commit 5 changes only a test file. The gate log
  records the tree it ran on.
- F5 and every phase beyond F2 are not started here. Optional W-04 remains
  unsupported and no F2 obligation depends on it.
