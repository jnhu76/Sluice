# F2 independent adversarial review — verdicts and dispositions

Two adversarial reviews ran against the slice, both read-only, both briefed
against the repository rules in `AGENTS.md`. Review A covered the production
diff; review B covered evidence and test-coverage integrity. This file records
what each found and what happened to every finding, so the disposition can be
checked item by item.

## Review A — production code

Verdict: **the F2 contraction itself is sound**. Exactly one settlement
authority (`RequestCore` slot plus `Request<T>`), no surviving second state
machine or cached summary that can disagree, publication strictly ordered before
any public observation, reference accounting (`execution_refs` / `control_refs` /
`publication_inflight` / binding / observer) covering slot reuse, at-most-once
observer delivery, and no accepted request lost on the traced backend paths. The
`TerminalResult` → `IoOutcome` substitution and the zero-op publication reorder
were confirmed behaviour-preserving (the latter removes a baseline
unsynchronised record read).

| Finding | Severity | Disposition |
|---|---|---|
| `Scheduler::await_request` mapped every refused attach other than `already_terminal` to `invalid_state`, so a published request read as a failure after `close_admission()` (the pre-F2 await consulted `Completion::ready()`). | HIGH | **Fixed** (commit `fix(scheduler): report a settled result after admission closes`) with a deterministic regression scenario that fails on the pre-fix mapping. |
| io_uring: a poison landing between `accept()` and the ring push can strand an accepted request until the caller cancels it; `shutdown()`/the destructor then spin rather than converge. | MEDIUM | **Recorded, not fixed**: byte-identical at the baseline; fixing it is a separate protocol change. See `known-preexisting-gaps.md`. |
| Ledger claimed `cancel_intent` arbitrates terminal arbitration; it is written and read only by the protocol seam. | LOW | **Ledger corrected.** |
| Ledger claimed a ThreadPool `fatal_error_` next to the core health verdict; the ThreadPool backend has no poison state. | LOW | **Ledger corrected.** |
| `RequestCore::release_public_binding` and `acquire_execution` have no production caller. | LOW | `release_public_binding` is a second release edge and was **retired** (commit `refactor(core)!: leave one release edge for the public binding`); `acquire_execution` is classified in the ledger as protocol-test surface. |
| `AsyncIoContext::retire_delivery` dereferences `core_` without the guard its rewritten siblings have. | INFO | **Recorded** as a pre-existing asymmetry reachable only from a moved-from context. |

## Review B — evidence and coverage integrity

Verdict: the migration is largely faithful, but the evidence package as it then
stood was **not sufficient and not correctly bound** to the reviewed revision.

| Finding | Severity | Disposition |
|---|---|---|
| The restructured R14 cell kept the "obligation survives an acknowledging drain" half but dropped the "a later signal still wakes a parked host" half, and the record overstated what was preserved. | BLOCKER | **Fixed**: the wake re-assertion half is restored deterministically (parked host, quiescent fd, next submission released by the new signal only), the load measurements including the first post-restructure failure are recorded, and the README no longer claims nothing was dropped. |
| `gates.json` did not exist although the docs cited it, and the captures named different SHAs (matrix at one head, package capture at its parent). | BLOCKER | **Fixed**: every capture was re-run at one head and `collect-gates.py` asserts the head agreement and writes `gates.json`; a capture bound to another revision fails the collector. |
| `run-matrix.py` recorded a failing `xmake test` without failing the run, and a shrunken build set passed silently. | HIGH | **Fixed**: the runner asserts a zero `xmake test` exit, asserts the expected target count per profile, and names the one documented unavailability (TSan `request_scope_test`) only when the build log carries that exact reason. |
| The two cutover publication scenarios lost their release step on rename. | MEDIUM | **Assessed**: the public API cannot release mid-publication by design (the canonical edge requires published; the mutant that accepts inflight is killed), and the failure mode stays covered at the protocol level by `request_core_publication_test`/`public_request_release_violation_test`. The two scenarios are re-expressed on the canonical verdicts with their safety content retained. |
| Package harness: `endswith("/" + name)` on relative source paths could never match; the removed-definition regex was only asserted absent after. | MEDIUM | **Fixed**: before-side positive controls for both (the sources and the pattern must be observable before the contraction). |
| Installed-probe docstring claimed the before prefix runs the probes; the ODR claim overstated what weak-symbol folding allows the probe to detect. | MEDIUM | **Fixed**: the docstring now states what the control is and what the probe can and cannot detect. |
| Installed-surface record omitted the scheduler/observer `Completion` spellings, `BatchOp`, the 16 retired fail-fast diagnostics and the retired release edge. | MEDIUM | **Fixed**: all added to `installed-surface-risk-disposition.md`. |
| The committed versions of the evidence docs were older drafts and referenced a directory that does not exist. | LOW | **Fixed**: the docs are committed as reviewed and the stale reference is gone. |
| A dead runtime clause in an F4 probe, empty `logs/`/`formal/` directories, `persist.py` dropping `environment.txt`. | LOW/INFO | Empty `logs/` removed; the probe clause is retained as a guard on default-`RequestId` validity; `environment.txt` is recorded by the capture and copied by `persist.py` when present. |

## What the review did not change

The production semantics of the contraction: no gate was relaxed, no wait or
retry added in place of a protocol transition, and no mutant's expected killer
changed (the campaign's kill set is identical on the baseline, the pre-review
head and the final head — see `mutants/README.md`).

## Final corrective-round review — 2026-10-13, final head `093332ba`

A third adversarial pass over the corrective round only: the production diff
`996f434f..093332ba` (three files: `uring_transport.cpp` +57/−12,
`uring_backend.hpp` +6 private declaration, `async_io_context.cpp` +3 guard),
the two repaired race DAGs, the new synchronization, the retirement behavior,
the raw RED/GREEN logs, the negative controls, the package implications and
the `rcore-live` inheritance identity. Read-only against the committed tree.

### A-fix (io_uring poison × post-accept push) — re-verified claims

- **Late-claim disposal is consistent with the dispatch path**: on a claim it
  did not win, `dispatch_one_locked` removes the reservation
  (`dispatch_->remove_exact`) and drops it without a `publication_pending_`
  entry; `retire_undispatched_after_poison_locked_` does the same via
  `pop_front` + `continue`. One shape, one obligation.
- **Kernel-invisibility precondition holds**: `dispatch_one_locked` refuses
  under `fatal_error_` before any ring access, so a reservation pushed after
  poison provably never took an SQE and has no ledger entry — the poison
  path's Class-A recovery cannot have resolved it.
- **`fatal_error_` is confined to `dispatch_mtx_`**: the poison write sits in
  a `_locked` function and every access site is under the mutex, so the
  `poisoned_before` read inside `dispatch_after_accept` is race-free.
- **Terminal ownership is exclusive**: with no ledger entry, the only other
  terminal source is a pre-dispatch public cancel, and `claim_execution`
  arbitrates exactly that race; a `chosen`-then-`borrow_touch_fully_retired`
  expectation mirrors the injection-failure block in the same function, and
  any other verdict fails fast rather than fabricating a second terminal.
- **No stranded wake**: the function-local `post_poison_push_retired` bit
  extends the wake condition; the raw GREEN log shows the retirement wake
  published and the in-kernel operation's real outcome delivered.

### B-fix (E1 pause-gate teardown lifetime) — re-verified claims

- A mechanical scan of all 19 backend declarations in the file finds **no
  worker-side gate** (`WorkerClaimedPauseGate`, `WorkerUnclaimedRetirementPauseGate`,
  `BeforeWorkerDequeuePauseGate`) declared after its backend. The single
  post-backend declaration (line 524, `AcceptedPreDispatchPauseGate` in
  `pre_execution_cancel_wins_with_known_zero_effect`) is submitter-side: its
  only passers are threads the scenario itself spawns, the fixed `GateGuard`
  resumes and waits for `exited==true`, the submitter is joined and the gate
  pointer cleared before the gate's storage dies — no worker can reach it.
- The guard's exit wait closes the registered-pass window; gate-before-backend
  ordering closes the unsynchronized pointer-load window, because the context
  destructor joins the workers while every worker-side gate still lives.

### C3, formal inheritance, evidence binding — re-verified claims

- The moved-from guard matches the sibling entry points' contract and the
  ownership oracle asserts it; the RED log is a raw SIGSEGV at the unguarded
  head.
- `git diff eb7bf055..HEAD -- formal/tla/ scripts/verify_tla.sh` is empty at
  the final head (re-run for this review); `RequestCore.tla` SHA-256 prefix
  `ce867f9e` matches the recorded identity table; `gates.json` keeps the
  verdict `INCOMPLETE: rcore-live` — the reuse acceptance lives in
  `formal/rcore-live-reuse.md`, never as a pass.
- `gates.json` asserts the matrix, both B-01 after-stages and the CI run all
  name `093332ba`; the load-contaminated TSan attempt is preserved next to
  the clean serial re-run with its attribution evidence (identical binary
  10/10 idle); the focused differential stress is 240/240 with per-run logs
  persisted (`final-stress/`).
- Package implications: the corrective production diff changes no installed
  header set (49 before and after the round) and no archive member set (30);
  `uring_transport.cpp.o` and `async_io_context.cpp.o` remain members; the
  installed probes were re-run against the final prefixes and pass with the
  negative control.

### Verdict and boundaries

New findings from this pass: **none requiring change**. The three corrections
close their recorded gaps with the minimum mechanism and no new runtime
state.

This review is technical acceptance of the corrective round only. It does not
approve the merge, does not grant `SAFE_TO_UNINSTALL` /
`SAFE_TO_DELETE_FROM_TREE`, and does not substitute for the human owner's
explicit scope-bound decision on the installed-surface break
(`installed-surface-risk-disposition.md`, all rows PENDING). No synthetic
approval is recorded anywhere in this slice, and PR #516 remains Draft.
