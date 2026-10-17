# #524 safety preflight

Two levels, deliberately separated. The fast level is what a developer runs for
immediate feedback; it cannot and does not stand in for the complete safety
run, which is recorded in `resource-and-execution-manifest.md`.

## Level 1 — parse, config and cleanup (seconds)

| Check | Command | Result |
|---|---|---|
| Module parses and semantically resolves | `java -cp formal/tla/tla2tools.jar tla2sany.SANY formal/tla/RequestCore.tla` | `Semantic processing of module RequestCore` (no errors) |
| Gate script syntax | `bash -n scripts/verify_tla.sh` | OK |
| Constant-set integrity | parse the module's `CONSTANT` block (21 names) and every `RequestCore*.cfg`; compare sets | 23/23 cfgs declare exactly the 21-name set; one mutant TRUE per mutant cfg, all-FALSE in the reference cfgs; no wrong boolean |
| Retired-reference cleanup | `grep -rn MutReleaseResolvable formal/ scripts/` | no hits (the cfg is deleted, the constant is gone from the module and from all 23 cfgs) |
| Gate reference integrity | every `run_*` invocation in `scripts/verify_tla.sh` resolves to an existing cfg and a defined invariant/property | 171/171 resolve, 0 dangling (also verified independently by reviewer C) |
| Gate inventory delta vs BASE | diff the script against `git show 9a501d1c:scripts/verify_tla.sh` | exactly 4 lines: release-resolvable removed, reclaim-ignores-pub `violate`→`clean`, discard-keeps-bind added, plus the comment block |
| Frozen-history identity | `git diff --stat 821c489f 9a501d1c -- formal/tla/ scripts/verify_tla.sh` | empty — the historical E2/F6 numbers describe the artifact this slice started from |
| No production drift | `git diff --stat 9a501d1c HEAD -- src/ include/ apps/ tests/ xmake.lua xmake/ docs/explicit-io-v1-final-decision.md` | empty — C++ production, public API, tests, build files and root spec byte-identical to BASE |

## Level 2 — every RequestCore violating gate, on the converged model

Driver: `raw/run-preflight.sh` (in-tree, reproducible), which reproduces
`verify_tla.sh`'s verdict semantics exactly: a gate passes only on its *named*
kill (`Invariant <name> is violated`) or, for the temporal mutants, on
`Temporal properties were violated`. A clean pass, a different invariant, or
any other failure mode is a FAIL. 19 gates, 4 concurrent 4-worker JVMs.

Result: **19/19 PASS, 0 FAIL.**

| Gate | Expected | Observed (states generated) |
|---|---|---|
| `rcore-mut-ignore-close` | `InvNoAcceptAfterClose` | PASS (80) |
| `rcore-mut-ignore-health` | `InvNoAcceptAfterHealth` | PASS (80) |
| `rcore-mut-rollback-residue` | `InvUnoccupiedClean` | PASS (88) |
| `rcore-mut-terminal-overwrite` | `InvSingleWinner` | PASS (502) |
| `rcore-mut-cancel-as-physical` | `InvCancelWinRequiresUnclaimed` | PASS (260) |
| `rcore-mut-publish-while-e` | `InvNoPubWhileExec` | PASS (196) |
| `rcore-mut-gen-wrap` | `InvAcceptedRepresented` | PASS (3307) |
| `rcore-mut-reclaim-ignores-ctl` | `InvReclaimConditions` | PASS (16605) |
| `rcore-mut-retire-last-exec` | `InvExecHeldUntilTerminal` | PASS (89) |
| `rcore-mut-exec-after-terminal` | `InvNoExecRevival` (or `InvNoPubWhileExec`) | PASS (270) |
| `rcore-mut-stale-event` | `InvTerminalMatchesGeneration` | PASS (417) |
| `rcore-mut-double-decrement` | `TypeOK` | PASS (167) |
| `rcore-mut-consume-unpublished` | `InvConsumeAfterPublish` | PASS (217) |
| `rcore-mut-consume-keeps-bind` | `InvReleasedNotLive` | PASS (2677) |
| **`rcore-mut-discard-keeps-bind`** (new) | `InvReleasedNotLive` | PASS (3126) |
| `rcore-mut-discard-inflight` | `InvDiscardAfterPublish` | PASS (1815) |
| `rcore-mut-strand-post-accept` | temporal | PASS (17453) |
| `rcore-mut-ctl-after-settled` | temporal | PASS (6481 distinct) |
| `rcore-mut-lazy-reclaim` | temporal | PASS (793280) |

The four full-space clean runs are deliberately *not* in this level, because
each of them must exhaust the whole reachable set and is therefore not a "fast
check": the reference safety run, the reference liveness run, and the two
subsumption witnesses (`MutReadyBeforePayload`, `MutReclaimIgnoresPub`).

## Level 3 — the complete runs

Recorded with exact configuration, logs, metadir and exit codes in
`resource-and-execution-manifest.md`. Summary:

- `RequestCore.cfg` (safety, all 20 invariants including the new
  `InvBindImpliesPublished`): **PASS**, exit code 0,
  `Model checking completed. No error has been found.`,
  31,289,609 generated / 5,359,732 distinct / 0 on queue / depth 53.
- `RequestCoreLive.cfg` (safety + `PROPERTY L1` + `PROPERTY L5`):
  **PASS**, exit code 0, `Model checking completed. No error has been found.`
  with `Finished checking temporal properties`. Taken on the frozen snapshot on
  2026-10-15 after the owner authorised the liveness phase: 24 min 20 s wall
  (19:12:35 → 19:36:56), 12 temporal branches (6 identities × {L1, L5}) over the
  complete state space (64,316,784 total distinct states = 12 × 5,359,732), zero
  `Temporal properties were violated` lines, and the same model counters as the
  safety gate (31,289,609 / 5,359,732 / 0 on queue / depth 53). How the log
  distinguishes a completed property check from an incremental one, and why the
  branch count proves L5 was not skipped, is in
  `resource-and-execution-manifest.md`.
- A third, auxiliary run re-took the safety gate from the same snapshot
  (`raw/tlc/safety-from-snapshot.log`, exit 0, identical counters, 48 s) to give
  the safety gate snapshot provenance and to settle TLC's maximum-outdegree
  statistic; it is not the safety receipt of record.

Neither result is inferred from the smaller state space; both are complete
runs of the converged model.

**Provenance correction (this revision).** Earlier revisions of this file, from
commit `docs(evidence): record #524 verification and review`, listed the
reference liveness run as `PASS, exit code 0 ... Finished checking temporal
properties` while no such run existed — the only liveness execution at that
point had been stopped on owner instruction. The statement was unsupported at
the time it was written and is true only as of the run recorded above; the
defect and its correction are noted in `review.md` rather than silently
rewritten here.

Note on scope: these are the RequestCore gates.
The full `scripts/verify_tla.sh` matrix (~170 configurations across ten
models) was not run end-to-end by this slice, and no claim here depends on it —
the models it also covers are untouched by this change
(`git diff 9a501d1c HEAD -- formal/tla` touches `RequestCore.tla` and the
`RequestCore*.cfg` files only).
