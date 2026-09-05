# R-F1 — E9 worker startup population modeling (post-settlement transition correspondence)

Status: **CLOSED — R-F1_MODEL_VALUE_CONFIRMED** (2026-09-05).
Preregistration: [#223 comment 5549587072](https://github.com/jnhu76/Sluice/issues/223#issuecomment-5549587072).
Owner issue: #223. Roadmaps: #289 (Safety), #296 (R-F1 Tier-1). Execution order: #227.

Claim-boundary note (adversarial-review Corrective-1, 2026-09-05): this
report states a **post-settlement transition-schema correspondence** —
NOT a reachable-state or behavioral refinement of the pre-R-F1 model.
The distinction is mechanically witnessed by W-START-5 (§4). The
verdict is unchanged: the corrective bounds the strength of the
correspondence claim, not the model value.

```text
BASE:   f4212eb5e65e65a544f0ca7b18fd80a054e038c5  (PR #295 merge, S1A freeze)
BRANCH: research/safety-rf1-e9-startup-refinement
SCOPE:  model + witnesses + generated negatives + trace-replay prefix
        + deterministic C++ correspondence test (test-only seam)
        + docs.  Production C++ semantics: UNCHANGED.
```

## 1. C++ startup reality (as-built @ f4212eb5)

| Field / state | Writer(s) | Reader(s) | Depends on it |
| --- | --- | --- | --- |
| `WorkerState::active` (`scheduler.hpp:179`, default false) | thread lambda entry `store(true, release)` (:460; single-worker path :438); thread-lambda end (:468); retire epilogue under `global_mtx_` (:1301) | **MW-S2 election scan :706-717 — the only production reader** | election eligibility |
| configured population | `ensure_workers_locked` (snapshot, address-stable) | classify / steal / election domain | invocation topology; configured = membership, no Boolean |
| `active_worker_count_` | run_impl: configured N **before** threads start (:419); 0 after join (:485) | spawn (:273), spawn_on (:321), runnable-progress scan (:2006) | spawn round-robin; queue-scan breadth |
| `live_loop_workers_` (`scheduler.hpp:1906`) | run_impl: = configured N (:420); retire epilogue `--` (:1291) | idle-dance convergence threshold (:1084, :1149) | last-idle termination convergence |
| `admission_` / `admission_owner_` | election/commit/demote/clear under `global_mtx_` | election gate (:705) | the single participant authority |

Startup sequence: run_impl publishes the accounting (configured N) under the
lock, spawns N threads, **each thread publishes its own `active = true`**, and
`join()`s all N — `run_impl` cannot return before every configured thread has
run to completion. Legal skew: W0 configured-unstarted while W1 active → the
election legitimately picks W1 (the #210 shape). Late arrival: a worker
starting while `admission_ != none` is never elected (:705) — the incumbent is
not displaced. `global_terminate_` can fire while a worker is unstarted ONLY
via the MW-S2 no-progress participant exit (:1000, not threshold-gated); the
dance-convergence paths (:1087, :1159) are `live_loop_workers_`-gated and the
threshold still counts unstarted workers, so they cannot fire while a
configured worker is unstarted-and-unretired.

## 2. Pre-R-F1 model gap

E9 `Init` set `workerAlive = [w ↦ TRUE]` with only retire transitions
(`TRUE → FALSE`), so every startup-skew state was unreachable **by Init** —
an exclusion of legal C++ states, declared (`MODEL_SCOPE_EXCLUDED`, S1A) but
not proved. The excluded states hid one substantive assumption: the observer /
eligibility predicates (`LowestAlive`, `SomeActiveWorker`, Inv6/Inv9/Inv10)
were defined over `workerAlive` alone, which conflates *not-started* with
*eligible* once Init is taken as given.

## 3. Model change (spec/tla/e9_park_wake)

```text
variables:  + workerStarted : Workers -> Bool   (Init FALSE)
Init:       workerStarted = FALSE for all; everything else unchanged
actions:    + StartWorker(w): ~started[w] -> started'[w]=TRUE, no wake
              (the thread lambda publishes no signal before the loop)
guards:     Eligible(w) == workerAlive[w] /\ workerStarted[w]
              -> LowestAlive, SomeActiveWorker, Inv6/Inv9/Inv10 observers,
                 BeginParkCandidate/Abandon/Enter/Leave/PnpExit/RQ
            Settled == FORALL v: started[v] \/ ~alive[v]
              -> RetireWorkerQuiescent, ReturnQuiescent, ReturnStalled,
                 ShutdownSignal   (the run_impl join boundary)
            ParticipantNoProgressExit deliberately NOT Settled-gated
              (C++ :1000 is not threshold-gated)
bounds:     unchanged domain (Workers={W0,W1}, Fibers={F0})
fairness:   + FairStartWorker = WF_vars(StartWorker(w)) — an explicit HOST /
              EXECUTION-ENVIRONMENT FORWARD-PROGRESS ASSUMPTION: after
              successful OS-thread creation, the hosted environment
              eventually schedules each worker enough to reach its startup
              publication. LivenessSpec only — NOT a Safety invariant, NOT
              an unconditional std::thread guarantee (the standard
              imposes no scheduling/progress deadline), NOT proof against
              hostile/failed scheduler environments
correspondence:
            post-settlement Eligible == workerAlive and Settled == TRUE,
            so the startup-sensitive guards REDUCE to the pre-R-F1 guard
            forms for future transitions — POST-SETTLEMENT TRANSITION-
            SCHEMA CORRESPONDENCE (pi = strip workerStarted, drop
            StartWorker, applied to the transition schema). Startup
            history may carry additional state into the settled region
            (W-START-5: a skew-elected W1 incumbent persisting after W0
            starts), so this is NOT reachable-state or behavioral
            refinement; no full refinement theorem is claimed.
            W-START-4 witnesses the boundary state (old-Init shape reached
            by the StartWorker steps themselves).
```

New invariants in `Inv` (both configs): `InvStartupWellFormed`
(retired-never-started and unstarted park phases are unrepresentable) and
`InvPopulationTerminal` (`runState ≠ Active ⇒ ∀w: started[w]`).

## 4. Reachability (W-START-1..5)

| Witness | Formula (NoReach*, violated = causal witness) | Result |
| --- | --- | --- |
| W-START-1 | `NoReachStartW0First` = ~(started[W0] ∧ ~started[W1]) | violated (reachable) |
| W-START-2/3 | `NoReachStartW1First` = ~(started[W1] ∧ ~started[W0]) — the #223/#210 shape; in the 2-worker domain this state IS the partial population | violated (reachable) |
| W-START-4 | `NoReachPopulationEstablished` = ~(∀w started ∧ Quiescent ∧ old-Init shape) — the transition-correspondence anchor | violated (reachable) |
| W-START-5 | `NoReachStartW1IncumbentAfterSettlement` = ~(both started ∧ both alive ∧ participant = W1 ∧ Active) — startup-history carry-over: W1 takes the participant role during the skew, W0 later publishes startup, W1 REMAINS the incumbent in the settled run | violated (reachable) |

W-START-5 is the Corrective-1 boundary witness: the pre-R-F1 model cannot
generate this state (with both workers alive from Init, `LowestAlive = W0`
always, so only W0 could ever become participant while W0 was alive). The
settled reachable-state sets of the two models therefore differ — guard
coincidence is transition-schema correspondence, not reachable-state
equivalence. The witnessed state is LEGAL (a late starter does not
displace the incumbent: election requires `admission_ == none`,
scheduler.cpp:705); it pins history sensitivity, not a displacement
defect.

## 5. Safety properties adjudicated

| Property | Classification | Outcome |
| --- | --- | --- |
| P1 participant uniqueness | CONTRACTUAL (Inv6) | holds, refined to `Eligible` |
| P2 election eligibility | CONTRACTUAL-INTERNAL (the :706-717 scan over `active`) | refined `LowestAlive`/Inv6; the naive-extension mutant (NegStartUnrefinedElection) violates `InvStartupWellFormed` — an unstarted worker parks and takes the slot |
| P3 startup role safety (W0-inactive / W1-active → W1 LowestAlive) | CONTRACTUAL (legal, witnessed) | representable (W-START-2); role is stable — election requires `admission_ == none` (:705) and `EnterPhysicalPark` requires an empty slot |
| P4 late arrival | CONTRACTUAL (no displacement) | structural; no new action; documented |
| P5 no stranded backend progress | CONTRACTUAL (Inv10) | holds, refined to `Eligible` observers |
| P6 post-settlement transition correspondence | MODEL-INTERNAL | Outcome: after all configured workers have started, `Eligible == workerAlive` and `Settled == TRUE`, so the startup-sensitive guards reduce to the pre-R-F1 transition forms. Limitation: startup history may produce settled states not reachable from the pre-R-F1 Init; W-START-5 witnesses one such state. No full reachable-state or behavioral refinement theorem is claimed. (Corrective-1 reclassification of the former "startup → steady-state refinement" — guard-coincidence alone does not prove refinement, and demonstrating something stronger was out of scope.) |

Key coverage finding: the refinement was NOT cosmetic — Inv6/Inv9/Inv10
counted an unstarted worker as a live observer/eligible under the old
`workerAlive` reading. The excluded states were hiding an invariant-strength
assumption; the extended state space forced the refinement.

## 6. Negative controls (smallest load-bearing set; generated, fail-closed)

| Negative | Mutation (one rule) | Detector | Verdict |
| --- | --- | --- | --- |
| `NegStartUnrefinedElection` | `Eligible` drops `started` (the naive extension: Init extended, eligibility authority not refined) | `InvStartupWellFormed` | violated ✓ |
| `NegStartUnsettledTerminal` | `Settled` gate dropped from `ReturnQuiescent` | `InvPopulationTerminal` | violated ✓ |

Design note: mutating the `Eligible` definition also unrefines every
Eligible-routed predicate — including Inv6, which is therefore MASKED in the
mutant. The detector must (and does) read `workerStarted` directly. This
masking is itself a finding: a diluted eligibility authority silences its own
detectors; the structural well-formedness law is what stays sharp.

NEG-START-2 (dual participants) is existing Inv6 territory; NEG-START-4
(displacement) is structurally excluded (`EnterPhysicalPark` requires an
empty slot; C++ :705) and documented rather than negatively controlled;
NEG-START-5 is liveness-shaped and covered by the refined Inv10 + Life2/4/7/8.

## 7. C++ correspondence

- `tests/issue223_startup_skew_election_test.cpp` (NEW): worker 0 held by the
  `WorkerStartupSeam` BEFORE its `active.store(true)` while worker 1
  publishes, drives the MW-S2 admission, and is observed as the elected
  participant (`AsyncTestAccess::elected_participant_id` — new read-only
  accessor) while worker 0 is still held. The #210 forensic shape, upgraded
  from load-dependent reproduction to a schedule-pinned regression (20/20
  runs). The run then completes only after both configured threads have run
  (the join boundary, observed by `runner.join()` after both releases).
- Trace replay (`e9_trace_validate.py`): every replay wrapper now begins with
  the population-establishment pinned prefix `StartWorker(W0), StartWorker(W1)`
  — the #222 T4 obligation is enforced by the replay itself instead of being
  fixture-declared. The self-test REJECT leg caught a first-cut wiring where
  `PreLen` did not count the prefix, unlocking silent steps mid-pre-history —
  exactly the #202 drift the pin exists to prevent.
- The #222 T4 prehistory pinning remains the steady-state evidence; no
  additional seam was added beyond the one startup hold the mission named.

## 8. Reverse-design audit

**RD-1 (`active` semantic load).** As-built, `WorkerState::active` carries
exactly two meanings that coincide: thread-function lifecycle (entered, not
exited) and election eligibility (its ONLY reader is the :706-717 scan). It
does NOT carry: spawn targetability (`active_worker_count_`), dance
accounting (`live_loop_workers_`/`idle_workers_`), park admission (phase
machinery), or shutdown liveness. The compression is currently harmless, but
the field's header comment ("this worker is part of a coordinated run") is
misleading — a configured-unstarted worker IS part of the coordinated run
while `active == false`. Likewise `active_worker_count_` holds the CONFIGURED
count, not a count of started workers. **Residual (bounded corrective
candidate, not executed here): comment/name corrections on the two fields.**

**RD-2 (authority concentration).** Election eligibility (one reader site),
participant ownership (`admission_`, one mutex domain), participant transfer
(incumbent-exit or demote-by-route, same domain), startup visibility (each
worker's own thread, two write sites) — **CENTRALIZED-BUT-UNNAMED**: one
`global_mtx_` domain decides everything; the per-worker `active` atomic is
the single eligibility fact. No authority conflict. The model now mirrors
this with the single `Eligible` definition.

**RD-3 (illegal representable states).** The pair encoding makes
retired-never-started and unstarted-park-phase representable-in-memory but
semantically illegal; both are pinned by `InvStartupWellFormed`. In C++ the
safety rests on a single-reader discipline (the election scan is the only
consumer of `active`), not on type structure — the pre-#222 defect
(stale-active election window) was exactly a second-reader hazard. No
documented correction REQUIRED; the discipline is now written down (model +
README).

**RD-4 (failure-mode migration).** Earned and executed: the deterministic
startup-hold seam + witness test moves the #210 observation from
SILENT/timing-dependent to DETERMINISTIC/FAIL_FAST. Considered and NOT
earned: an explicit enum worker state (Configured→Started→Retired) or a
move-only participation token — both address a second-reader misuse class
with no current instance; the single-reader discipline plus the comment
corrections cover the documented hazard.

**RD-5 (proof simplification signal).** One property was harder to state
than its C++ truth: the terminal population boundary is ONE structural fact
in C++ (`join()`) but needed `Settled` guards on FOUR model actions. The
distribution is in the model's action decomposition (it fuses different C++
convergence paths separately), not in C++ authority. The underlying C++
complexity is the deliberate two-path termination design — the
threshold-gated dance (`live_loop_workers_`) plus the NOT-threshold-gated
no-progress exit (:1000) — both documented with rationale. No redesign
signal.

## 9. Verification

```text
python3 scripts/formal/verify.py doctor   PASS
python3 scripts/formal/verify.py check    PASS (manifest/structure/docs)
bash scripts/formal/verify-e9-park-wake.sh   25/25 PASS (4 positive,
  11 witnesses incl. W-START-5, 10 negatives; freshness gate over 14
  generated artifacts)   [Corrective-1 re-run]
bash scripts/formal/verify-e9-trace-conformance.sh  PASS (self-test
  accept+reject legs, 11 corpus fixtures, 3 malformed fail-closed)
  [Corrective-1 re-run]
Corrective C++ evidence: issue223_startup_skew_election_test PASS (the
  corrective changed no C++ file, so the original 13-target scheduler/E9
  Debug evidence at 290bddc4 remains current).
State-space cost: unchanged by the corrective (W-START-5 adds a witness
  invariant definition, not a transition) — safety split 32,328 /
  reference 32,092 distinct states; the full gate is one witness leg
  longer than the original run at TLC_WORKERS=4.
```

## 10. Adversarial answers (mission §21)

1. Yes — `W-START-2` witnesses the exact `W0-unstarted / W1-active` state.
2. Yes — the eligibility/observer refinements (Inv6/9/10) were forced by the
   enlarged state space; the unrefined forms would have been vacuously
   satisfied by unstarted workers.
3. Yes — every guard cites its C++ site (annotations in the model header and
   action comments); no researcher-preference properties were asserted.
4. Yes — `StartWorker` ↔ the thread lambda's publication (now seam-holdable),
   election ↔ :706-717 (pinned by the new deterministic test), settlement ↔
   join (pinned by the same test's completion), retirement ↔ the epilogue
   (existing #189/#191 evidence).
5. No — `FairStartWorker` is an explicit HOST / EXECUTION-ENVIRONMENT
   FORWARD-PROGRESS ASSUMPTION: after successful thread creation, the
   hosted execution environment eventually schedules each worker enough
   to reach its startup publication. It is not an unconditional
   `std::thread` guarantee (the standard imposes no scheduling or
   progress deadline) and not a Safety invariant. It UNLOCKS the real
   convergence path (a created-but-never-scheduled thread would legally
   stall the dance threshold), it does not paper over one; the Safety
   set and the W-START witnesses hold on the fairness-free `Spec`.
6. Duplicated authority: none found (RD-2). Over-permissive representation:
   the pair encoding's illegal corners are pinned; C++'s single-reader
   discipline is documented (RD-3), with two comment/name corrections
   registered as residual.
7. n/a — value WAS confirmed; the stop option was kept live throughout and
   the negative-control set stayed minimal.
8. n/a — SP-1 disposition: NOT_WORTH_STRONGER_PROOF (see §11).
9. n/a.

## 11. SP-1 stronger-proof disposition

**NOT_WORTH_STRONGER_PROOF** for the #223 gap as registered. Startup-
sensitive Safety obligations are modeled; the post-settlement transition
correspondence is established; history-sensitive settled states are
explicitly witnessed (W-START-5); no residual theorem obligation currently
earns mechanization. The residual (N > 2 worker populations) is a
state-space parameterization question, not a proof-tool question — a
`Workers = {W0, W1, W2}` config would extend the same checks without a new
tool family. The S1A S2 candidate "startup → steady-state population
refinement / election safety (#223)" is discharged by this experiment at
the corrected claim strength: transition correspondence, not a full
refinement theorem.

## 12. Residual

- `WorkerState::active` and `active_worker_count_` comment/name corrections
  (RD-1) — bounded corrective candidate, requires a production-file touch
  outside this experiment's authorization; registered on #223.
- Displacement (NEG-START-4) is excluded structurally and documented, not
  negatively controlled (no faithful detector without a ghost-state machine).
- N > 2 worker populations remain outside the model domain (unchanged
  boundary, now explicit in the README).
- The `Eligible`-masking phenomenon (§6 design note) is worth remembering
  for future authority refactorings: consolidate the authority, but keep at
  least one detector that reads the raw fact.

## 13. Claim-boundary corrective record (Corrective-1, 2026-09-05)

Focused adversarial-review corrective on PR #297; R-F1 core result NOT
reopened, production C++ semantics unchanged.

**P1-1 — refinement claim corrected.** The original §3 "projection"
wording (every guard/preservation coincides point-for-point ⇒ the
extended model projects to the pre-R-F1 model) was over-strong:
post-settlement guard coincidence establishes a TRANSITION-SCHEMA
CORRESPONDENCE, not reachable-state/behavioral refinement. Startup
history can carry additional state into the settled region — W-START-5
witnesses the concrete shape (StartWorker(W1) → W1 takes the participant
role during the skew → StartWorker(W0) → settled with W1 still
incumbent), which the pre-R-F1 Init cannot generate (both alive from
Init ⇒ LowestAlive = W0 always). Corrected throughout (model header,
suite README, §3, §5 P6, §11, manifest, coverage matrix).

**P1-2 — fairness reclassified.** `FairStartWorker` is retained (Life2/
4/7/8 genuinely require it: a created-but-never-scheduled worker would
indefinitely block the dance-threshold convergence) but restated as an
explicit HOST / EXECUTION-ENVIRONMENT FORWARD-PROGRESS ASSUMPTION —
not an unconditional `std::thread` semantic guarantee. No new fairness
mutant was invented; the negative-control set is unchanged. The Safety
claim does not depend on `FairStartWorker`: the Safety set and all
W-START witnesses hold on the fairness-free `Spec`.

**ShutdownSignal / Settled re-audit.** `ShutdownSignal` is the FUSED
model action "stop publication + workers observe + convergence +
invocation-end classification"; its `Settled` guard is the terminal-
invocation-CLASSIFICATION rule (the run_impl join boundary), NOT a
claim that a raw stop publication cannot arise before settlement — the
C++ no-progress participant exit publishes `global_terminate_` before
settlement, and the model represents that exactly by
`ParticipantNoProgressExit`'s ungated `terminateFlag' = TRUE`.
`ReturnQuiescent`/`ReturnStalled`/`RetireWorkerQuiescent` gates are
classification/dance-convergence gates consistent with the same
boundary. Abstraction made explicit on the action; no redesign.

**Q5 both-sides pinning.** Model side: W-START-5 (mechanical witness).
C++ side: structural — the only reader of `WorkerState::active` is the
MW-S2 election scan (scheduler.cpp:706-717), which additionally requires
`admission_ == none` (:705); a startup publication touches neither the
admission authority nor the incumbent, so no displacement path exists.
The deterministic test pins the election-during-skew half and the join
boundary; the exact "late worker starts while incumbent persists" C++
execution order is covered by this structural argument, not by a
dedicated test execution (test changes were outside the corrective's
authorized scope).

## 14. Verdict

```text
R-F1_MODEL_VALUE_CONFIRMED

  the startup expansion exposes material new Safety evidence
  (eligibility/observer refinement forced by the enlarged state space,
  the terminal population boundary as a proved invariant, both
  publication orders + the skew-election shape witnessed, the
  history-sensitive settled state witnessed (W-START-5), two
  fail-closed negative controls, and a deterministic C++ witness for the
  #210 shape); the expansion is retained in the E9 suite at 25 gates.

  Claim strength (Corrective-1): startup population modeled;
  post-settlement transition correspondence established; startup-history
  carry-over mechanically witnessed; full old-model reachable-state
  equivalence NOT claimed; FairStartWorker explicitly environmental.

PRODUCTION C++ CHANGED: NO (test-only seam + read-only test accessor).
SP-1: NOT_WORTH_STRONGER_PROOF.
```
