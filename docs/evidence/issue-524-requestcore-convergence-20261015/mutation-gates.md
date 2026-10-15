# #524 mutation-gate migration

The old `MutReleaseResolvable` mutated the deleted pre-F2 release edge. This
record states the disposition of every affected gate, the new mutant, and the
evidence that each gate still bites for its own reason.

## Gate inventory change

| Gate (`scripts/verify_tla.sh`) | BASE | Converged |
|---|---|---|
| `rcore-mut-release-resolvable` (`RequestCoreMutReleaseResolvable.cfg`, must violate `InvReleasedNotLive`) | present | **removed** — the mutated action no longer exists |
| `rcore-mut-reclaim-ignores-pub` (must violate `InvReclaimConditions`) | `run_violate` | **`run_clean`** — subsumption witness (see below) |
| `rcore-mut-discard-keeps-bind` (`RequestCoreMutDiscardKeepsBind.cfg`, must violate `InvReleasedNotLive`) | — | **added** |

Nothing else moved: gate inventory diff BASE → converged is exactly those four
lines, 153 top-level gates on both sides, and all 171 `run_*` invocations still
resolve to an existing cfg naming an invariant the module defines (reviewer C's
mechanical check, plus the same check re-run at the model head). The cfgs that
reference the deleted constant were all migrated, and every RequestCore cfg
declares exactly the module's constant set (21 names, 23 cfgs).

## Why `MutReleaseResolvable` retires rather than being renamed

A mutant is an assertion about a *transition*: "if this edge forgets its
obligation, the named invariant catches it". With the edge gone, the mutant has
no transition to mutate, so renaming it onto a surviving edge would silently
change the claim being made. Instead:

- the edge's fault class is re-expressed on the **discard** edge by the new
  `MutDiscardKeepsBind`, and remains expressed on the **consume** edge by the
  existing `MutConsumeKeepsBind`;
- the mutant and its cfg are deleted, and its disappearance is recorded here
  and in the conformance ledger's dated #524 section — the historical B1-1
  record of its original kill is left untouched as a dated record.

## The new mutant: `MutDiscardKeepsBind`

Fault: public discard records the release but leaves the public binding live.

```tla
  /\ IF MutDiscardKeepsBind
       THEN UNCHANGED bind
       ELSE bind' = [bind EXCEPT ![s] = FALSE]
```

It is the only place the flag is read; its cfg is the reference cfg with the
single flag flipped, so the kill also proves that exactly one reference
invariant breaks. Observed kill:

```text
Error: Invariant InvReleasedNotLive is violated.
trace: Reserve → Accept → CancelRequest → RetireExec → BeginPublish →
       CompletePublish → DiscardPublic
violating state: phase[s0] = accepted, gen[s0] = 0, bind[s0] = TRUE,
                 stage[<s0,0>] = 3, discarded[<s0,0>] = TRUE, pub[s0] = "done",
                 pubDoneIds = {<s0,0>}, exec = ctl = 0, term[s0] = "canceled"
```

### Reachability

The kill is a real counterexample, not a vacuous one: the violating trace only
uses steps that are enabled in the reference model, and the mutant is killed
in 3,126 states generated (preflight) / 4,904 (reviewer B's independent run,
which reached the same violating state from the same action sequence).

### Distinctness from its siblings

| | mutated action | guard/effect mutated | violating action in the trace | violating state | kill |
|---|---|---|---|---|---|
| `MutConsumeKeepsBind` | `Consume` | forgets `bind' = FALSE` | `Consume` | `consumed = TRUE`, `bind = TRUE`, `pub = "done"` | `InvReleasedNotLive` |
| `MutDiscardKeepsBind` | `DiscardPublic` | forgets `bind' = FALSE` | `DiscardPublic` | `discarded = TRUE`, `bind = TRUE`, `pub = "done"` | `InvReleasedNotLive` |
| `MutDiscardInflight` | `DiscardPublic` | accepts an inflight publication | `DiscardPublic` | `bind = FALSE`, `pub = "inflight"`, `pubDoneIds = {}` | `InvDiscardAfterPublish` |

The three traces share the prefix
`Reserve → Accept → CancelRequest → RetireExec → BeginPublish`
and diverge at their own mutated action; the violating *witness states* differ
in which ghost records the release and in whether the binding is live or the
publication complete. They are therefore three different faults, not one fault
counted three times. Reviewer B reached the same conclusion independently and
also verified that the kill cannot depend on any other flag (the flag is read
once, in `DiscardPublic`, and every other flag is FALSE in its cfg).

Together, the two release edges of the current protocol each carry their own
"forgot the invalidation" mutant, which is the coverage the retired mutant used
to provide for the (now deleted) third edge. The new mutant also mirrors a real
C++ mutation seam: `SLUICE_B2_MUTANT_RELEASE_FORGETS_BINDING`
(`include/sluice/async/request.hpp:149`), the discard path that drops the
binding release.

## The reclassified witness: `MutReclaimIgnoresPub`

At BASE this mutant was *killed*, and its kill trace ran through the deleted
edge:

```text
Reserve → Accept → CancelRequest → RetireExec → BeginPublish → ReleaseBind →
Reclaim   (the logged reclaim record carries pub = "inflight")
```

With the edge gone, a released binding always has `pub = "done"`, so the guard
the mutant drops is implied by the surviving `~bind[s]` guard, and the mutant's
cfg must now complete cleanly. Reclassified as the repository's second
subsumption witness (the first is `MutReadyBeforePayload`), under the
established pattern: the cfg must complete CLEANLY, and the clean pass is the
certificate of the subsumption.

Why this is a real gate and not a weakened one:

1. The mutant relation is a strict superset of the reference relation, so
   `Reach_mutant ⊇ Reach_ref`; the run reports counters byte-identical to the
   reference run (`31,289,609 generated / 5,359,732 distinct / depth 53`),
   which forces `Reach_mutant = Reach_ref` — the dropped guard adds nothing.
2. The subsuming fact is *named*, not asserted in prose: the new
   `InvBindImpliesPublished` is asserted in the reference cfg and in this
   witness's cfg.
3. It keeps teeth. Under `MutDiscardInflight = TRUE` (an inflight-tolerant
   release edge present again) the same invariant list reports
   `Invariant InvBindImpliesPublished is violated` (1,598 states); with the new
   invariant removed from the list the same model reports
   `Invariant InvReclaimConditions is violated` again (3,949 states). So the
   witness fails — loudly — if an inflight-tolerant release path is ever
   reintroduced, which is the regression it is there to catch.
4. `InvReclaimConditions` did not become an orphaned invariant: it still has a
   live killer, `MutReclaimIgnoresCtl` (a logged reclaim with `ctl > 0`).

## Complete mutant matrix at the model head

Every RequestCore mutant cfg was re-run on the converged model with the gate's
own verdict semantics (exact named kill, never a bare exit code): 19 violate /
temporal gates PASS in the R5 preflight, and the two subsumption witnesses
(`ready-before-payload`, `reclaim-ignores-pub`) plus the reference safety run
complete cleanly. The reference liveness gate is not part of this matrix and
was taken separately as its own receipt: **PASS** on the frozen snapshot
(`raw/tlc/liveness.log`, exit 0, 24 min 20 s, L1 and L5 over the complete state
space). Reviewer B reproduced the full battery plus the two witnesses
independently; reviewer C reproduced the mechanical
cfg/constant/gate-resolvability checks and three of the cfgs by TLC.

No expected kill changed: each mutant still reports exactly the invariant (or
temporal violation) its gate names, including the two whose cfg lists place the
expected invariant late (`MutDiscardInflight` → `InvDiscardAfterPublish`,
`MutConsumeUnpublished` → `InvConsumeAfterPublish`), so no kill was shadowed by
an earlier list entry.
