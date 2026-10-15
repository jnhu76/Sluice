# #524 transition diff — what changed in the transition relation

Model head `28f0fe33`, BASE `9a501d1c`. The change was made deliberately
minimal: no slot count, generation bound, environment branch or checked
property was touched to shrink the state space.

## Code delta (comment-stripped)

Between `git show 9a501d1c:formal/tla/RequestCore.tla` and the converged
module there are exactly **five** code changes:

1. `CONSTANT` list: `-MutReleaseResolvable`, `+MutDiscardKeepsBind`.
2. `DiscardPublic`: `bind' = [bind EXCEPT ![s] = FALSE]` becomes
   `IF MutDiscardKeepsBind THEN UNCHANGED bind ELSE <that update>` — identical
   whenever the flag is FALSE, i.e. in the reference cfg and in every
   pre-existing cfg.
3. `ReleaseBind(s)` definition deleted.
4. `SlotAction(s)`: the `\/ ReleaseBind(s)` disjunct deleted.
5. `InvBindImpliesPublished` added.

`Init`, every other action, `Fair` (all six `WF_vars` conjuncts byte-identical),
`FairSpec`, `Reclaimable`, `L1`, `L5`, and the 19 pre-existing invariants are
unchanged. `TLC`/`GenMax`/`Slots`/`MaxExec`/`MaxCtl` are unchanged.

As a relation, `Next_new = Next_old` minus the `ReleaseBind` pairs. No
transition was added, so `Reach_new ⊆ Reach_old` by construction; the question
the freeze gate has to answer is which states were lost and whether any of them
matters.

## Isolation control (reviewer B)

The behavioural delta is attributable entirely to the deleted disjunct:
BASE `RequestCore.tla` with **only** the line `\/ ReleaseBind(s)` removed
produces `31,289,609 generated / 5,359,732 distinct / depth 53` — bit-identical
to the converged model's counters (`RequestCore.cfg` and
`RequestCoreMutReclaimIgnoresPub.cfg`). The flag-conditional in (2) is
therefore semantically inert at the reference configuration.

Reviewer B also observed a cosmetic difference in TLC's *maximum outdegree*
statistic (11 with the mutant switch present, 10 in the isolation control) with
byte-identical generated/distinct/depth counts. It feeds no gate verdict and is
recorded here rather than explained away.

## Which states were lost

Exactly one family:

```text
phase = "accepted"  /\  bind = FALSE  /\  pub /= "done"
```

equivalently, by the ghost monitors,
`stage[i] >= 3` with `i ∉ pubDoneIds` and `consumed[i] = discarded[i] = FALSE`.
These are the states the retired pre-F2 `Completion` release edge produced.

- At BASE the family is reachable. Reviewer A's witness invariant
  (`stage >= 3 ∧ identity not published`) is violated at BASE in 1659 states,
  trace `Accept → CancelRequest → RetireExec → BeginPublish → ReleaseBind`,
  final state `bind = FALSE, pub = "inflight", stage = 3, consumed = FALSE,
  discarded = FALSE, pubDoneIds = {}`.
- In the converged model the same witness is unreachable over the complete
  reachable set, and reviewer B confirmed the same for the *mutant's* extra
  precondition `phase = accepted ∧ ~bind ∧ exec = 0 ∧ ctl = 0 ∧ pub ≠ "done"`.

Nothing else was lost: no fair action, no invariant antecedent, and no
liveness antecedent became unreachable. `ReclaimableId` (which needs
`~bind ∧ pub = "done"`), `stage >= 2`, `stage = 4` and all six fair actions are
all exercised in the converged model (reviewer B's coverage probes).

## No state lost its last successor

For every state where `ReleaseBind` was enabled,
`phase = accepted ∧ bind ∧ term ≠ "none" ∧ exec = 0 ∧ pub ≠ "none"`:
if `pub = "inflight"` then `CompletePublish` is enabled (its only guard is
`pub = "inflight"`); if `pub = "done"` then `Consume` is enabled. The removal
therefore creates no stuck state and cannot mask a liveness obligation. The
`-deadlock` flag in the gate (which in TLC 2.19 means "do not check for
deadlock") is immaterial for the same reason: a fair-action obligation can
only be starved where that action is enabled, and a terminal state enables no
action at all. Reviewer B verified empirically that the gate's exact flag shape
still reports a temporal violation for a stranded obligation.

## State-space size, old vs converged

| Model | generated | distinct | depth |
|---|---|---|---|
| BASE `RequestCore.cfg` | 153,347,909 | 26,662,372 | 53 |
| converged `RequestCore.cfg` | 31,289,609 | 5,359,732 | 53 |
| BASE minus only `\/ ReleaseBind(s)` | 31,289,609 | 5,359,732 | 53 |

The BASE numbers are the E2/F6 recorded ones for the frozen model
(`docs/evidence/f6-final-closeout-20261015/findings-disposition.md`, F6-F04);
`formal/tla/` and `scripts/verify_tla.sh` are byte-identical from the F6 frozen
content `821c489f` to BASE `9a501d1c`
(`git diff --stat 821c489f 9a501d1c -- formal/tla/ scripts/verify_tla.sh` is
empty), so they describe the same artifact this slice started from. The
reduction is ~4.9× in distinct states and is *entirely* the deleted edge — it
is a consequence of convergence, not a modelling trade made to make a run
faster.
