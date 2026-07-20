# E13 Select Formal Model Core — Corrective-1 Delta Review 1

This is an **independent, adversarial, narrow-scope delta review** of the
four-commit Corrective-1 against the originally reviewed PR #17 head. It is
performed by an independent reviewer; it does not trust the author's
self-assessment PASS verdict without re-running every gate and independently
probing the rejected trace and its action fragments.

## A. Verdict

```text
E13-SELECT-FORMAL-MODEL-CORE-1-CORRECTIVE-1-DELTA-REVIEW-1:
PASS

REVIEWED CANDIDATE HEAD:
b4afe6c3736b3af73a8d0c82e994040874c24ac9

ORIGINAL REVIEWED HEAD:
e61a0b4971758a819d8ba3b94f3e3147b34c4bc1

BASE:
246505a1de2ffa2840ab404adc76a2839d4ae069

P0 CONTRACT ROLLBACK:
CLOSED

REGISTRATION ROLLBACK DOMAIN:
BUILDING / REGISTERING ONLY

POST-SUSPENSION ROLLBACK:
UNREACHABLE

TERMINAL WAITING CALLER:
UNREACHABLE

R9 PARTIAL REGISTRATION ROLLBACK:
PASS — CAUSAL WITNESS VERIFIED

CONTRACT REFINEMENT:
PASS

CENTRAL CLAIM REFINEMENT:
PASS

EVENT/TIMER ADAPTER REFINEMENT:
PASS

R1–R12:
PASS

SOURCE-SAFE VERIFIER:
PASS

SOURCE SENTINEL PRESERVATION:
PASS

CONFIG/DOCUMENT DRIFT:
CLOSED

GITHUB REVIEW FEEDBACK:
ADDRESSED OR NON-BLOCKING

E13 FORMAL CORE PR #17:
ACCEPTED

PR #17 MERGE:
AUTHORIZED

PR #18 FORMAL SAFETY IMPLEMENTATION:
AUTHORIZED

PRODUCTION IMPLEMENTATION:
DENIED
```

The Corrective-1 closes the historical P0-1 (post-suspension rollback
destroying a `Waiting` caller) and the historical P1 (formal helper deleting
source-tree artifacts), and closes the P2 documentation drift. All 21 verifier
gates were independently re-run from a clean `mktemp` workspace; every gate
matched or exceeded the author's self-reported metrics. A reviewer-only TLA+
probe module (kept under `/tmp`, outside the repository) independently
demonstrated that the rejected post-suspension-rollback terminal state is
unreachable while `ContractSuspendCaller`, the pre-suspension rollback, the
`Aborted` terminal, and post-abort `DestroyOperation` each remain genuinely
reachable — i.e. the fix is not a vacuity. The R9 witness was extracted from a
fresh TLC error trace and is a real strict-subset partial-registration rollback
with every registered arm Retired / Released / authority-closed and every
unregistered arm Detached / None.

The verdict does **not** authorize production implementation. Production
remains `DENIED` until PR #18's layered safety, negative models,
non-vacuity, and refinement are independently reviewed.

## B. Candidate binding

```text
REPOSITORY:
jnhu76/Sluice

PR:
https://github.com/jnhu76/Sluice/pull/17 — OPEN, MERGEABLE, mergeStateStatus=CLEAN

PR headRefOid (GitHub):
b4afe6c3736b3af73a8d0c82e994040874c24ac9

LOCAL HEAD after git reset --hard origin/feat/e13-select-formal-model-core:
b4afe6c3736b3af73a8d0c82e994040874c24ac9

EXPECTED CANDIDATE HEAD (from review request):
b4afe6c3736b3af73a8d0c82e994040874c24ac9

MATCH:
YES — candidate head bound to the immutable PR tip.

BRANCH:
feat/e13-select-formal-model-core
```

The PR head did not move during review. The local HEAD, the GitHub `headRefOid`,
and the expected candidate head are byte-identical.

## C. Baseline

Review time: 2026-07-20 (Asia/Shanghai). TLC2 Version 2.19 of 08 August 2024
(rev: 5a47802); OpenJDK 25.0.3; `TLC_WORKERS=1`.

```text
BASE_COMMIT:
246505a1de2ffa2840ab404adc76a2839d4ae069

MERGE_BASE:
246505a1de2ffa2840ab404adc76a2839d4ae069

CANDIDATE_HEAD:
b4afe6c3736b3af73a8d0c82e994040874c24ac9

CORRECTIVE_COMMITS (e61a0b4..b4afe6c):
6b40ac2 formal(e13): restrict rollback to pre-suspension registration
bfc7d66 formal(e13): add terminal caller-state regression gate
3f2c219 tools(formal): isolate TLC execution from source tree
b4afe6c docs(e13): synchronize core configs and corrective evidence

CORRECTIVE_CHANGED_FILES (21):
docs/formal/e13-select-formal-core-design.md
docs/formal/e13-select-formal-core-plan.md
docs/reviews/E13-SELECT-FORMAL-MODEL-CORE-1-CORRECTIVE-1-REVIEW-REQUEST.md   (A)
docs/reviews/E13-SELECT-FORMAL-MODEL-CORE-1-INDEPENDENT-REVIEW-1.md
docs/spec/e13_select/E13Select.scene_2mix.cfg
docs/spec/e13_select/E13Select.scene_inline.cfg
docs/spec/e13_select/E13Select.scene_rollback.cfg
docs/spec/e13_select/E13Select.scene_staletimer.cfg
docs/spec/e13_select/E13Select.scene_suspended.cfg
docs/spec/e13_select/E13SelectCentralClaim.cfg
docs/spec/e13_select/E13SelectCentralClaim.reach.cfg
docs/spec/e13_select/E13SelectCentralClaim.tla
docs/spec/e13_select/E13SelectContract.cfg
docs/spec/e13_select/E13SelectContract.reach_inline.cfg
docs/spec/e13_select/E13SelectContract.reach_suspended.cfg
docs/spec/e13_select/E13SelectContract.rollback_regression.cfg            (A)
docs/spec/e13_select/E13SelectContract.tla
docs/spec/e13_select/E13SelectEventTimer.cfg
docs/spec/e13_select/E13SelectEventTimer.tla
docs/spec/e13_select/README.md
tools/formal/verify-e13-select-core.sh

FULL_PR_CHANGED_FILES (30, all under docs/ or tools/):
(see Section N — no include/src/tests/examples/benchmarks/build-policy file)

WORKTREE STATUS:
clean tracked tree; pre-existing untracked tests/test_t3_simple.cpp and
tla2tools.jar remain untracked and untouched.
```

`git diff --check origin/master...HEAD` and
`git diff --check e61a0b4..HEAD` both report **no whitespace error**. The
seven historical blank-line-at-EOF warnings (P2-1) are gone.

Pre-existing local files `tests/test_t3_simple.cpp` (AsyncCondition smoke
test, fs timestamps 2026-07-15, before the candidate) and `tla2tools.jar` are
documented in multiple historical commits as user assets. They are untracked,
not part of PR #17's committed file list, and were not modified, staged,
committed, or deleted by this review.

The ignored `docs/spec/e13_select/states/` directory is a TLC-metadir remnant
covered by the repository `states/` ignore rule; it is not treated as review
evidence or as a proposed PR file.

## D. Historical authority

The historical independent review
(`E13-SELECT-FORMAL-MODEL-CORE-1-INDEPENDENT-REVIEW-1.md`) was read in full.
Its verdict is preserved unchanged:

```text
E13-SELECT-FORMAL-MODEL-CORE-1-INDEPENDENT-REVIEW-1 = REQUEST-CHANGES
ABSTRACT SELECT CONTRACT: REJECTED
PR #18: DENIED
PRODUCTION IMPLEMENTATION: DENIED
```

The corrective diff modified this historical file
(`docs/reviews/E13-SELECT-FORMAL-MODEL-CORE-1-INDEPENDENT-REVIEW-1.md`), so
its diff was inspected line-by-line to confirm the verdict was **not** rewritten
to PASS. The changes are baseline-binding recordkeeping only:

- The stale "PR #17 not found / HEAD = origin/master" prose (which described the
  pre-publish state) was replaced with the now-auditable PR #17 binding
  (`https://github.com/jnhu76/Sluice/pull/17`, head `e61a0b4`, OPEN).
- The obsolete P1 ("baseline unverifiable: no PR head") was removed because the
  PR now exists; the subsequent P1 was renumbered P1-2 → P1-1.
- The blocking P0-1 (Contract rollback) and the P1 (formal helper source-tree
  deletion) findings are preserved verbatim.

No `REQUEST-CHANGES` verdict, no `REJECTED`, no `DENIED` label was flipped to
PASS or APPROVED. This is housekeeping, **not** a governance violation. The
corrective review request explicitly states it does not rewrite the historical
verdict.

## E. Rollback-domain review (Gate 1)

`docs/spec/e13_select/E13SelectContract.tla`:

- `ContractRollbackEnabledDomain` (lines 152–155) is exactly
  `contract_phase = "Building"` /\ `winner = NoArm` /\ `caller_state = "Running"`.
- `ContractBeginRollback` (lines 367–374) requires that domain and otherwise
  only advances `contract_phase` to `"Rollback"`. It does **not** touch
  `caller_state`, `winner`, or any publication counter.
- `ContractRegistrationRollbackDisabledAfterSuspension` (lines 157–158)
  separately asserts `caller_state = "Waiting" => ~ContractRollbackEnabledDomain`.

Consequence: rollback can begin only while `contract_phase = "Building"`,
which is reachable only **before** `ContractFinishRegistration` (the only
`Building -> Selecting` transition, line 200) and therefore before
`ContractSuspendCaller` (which requires `Selecting`, line 233), before any
`ContractLinearizeWinner`, before `WinnerLinearized`/`Closing`/`Completed`.

The rollback domain is now strictly **registration-failure rollback before
registration completion, before caller suspension, before winner
linearization**. Post-suspension cancellation, user cancellation, shutdown
cancellation, and whole-Select timeout remain deferred (and are now
provably unreachable through this path — see Gate 5).

```text
GATE 1: PASS — rollback is pre-suspension registration rollback only.
```

## F. Refinement propagation (Gate 2)

### Central Claim (`E13SelectCentralClaim.tla`)

- `CentralRollbackEnabledDomain` (lines 119–123) requires
  `central_phase = "Registering"` /\ `contract_phase = "Building"` /\
  `winner = NoArm` /\ `caller_state = "Running"`. The `"Admission"` and
  `"Armed"` central phases are no longer rollback-enabled.
- `CentralBeginRollback` (lines 232–236) uses that domain and invokes
  `ContractRefinement!ContractBeginRollback`, so the concrete Central action
  refines the Contract action.
- `CentralRegistrationRollbackDisabledAfterSuspension` (lines 125–126) is
  conjoined into `CentralInv` (lines 128–133), so the disabled-after-suspension
  law is enforced at this layer too.

### Event/Timer concrete layer (`E13SelectEventTimer.tla`)

- `EventTimerRollbackEnabledDomain` (lines 251–257) requires:
  `registration_count > 0` (some arm is registered),
  `\A i \in Arms : adapter_phase[i] # "Registering"` (no arm is mid-registration),
  `central_phase = "Registering"`, `contract_phase = "Building"`,
  `caller_state = "Running"`, `winner = NoArm`.
- `BeginRollback` (lines 715–727) uses that domain and invokes
  `CentralRefinement!CentralBeginRollback`; it only flips `rollback_started`.
- The concrete rollback seam (`RollbackEventArm` / `RollbackCancelTimer` /
  `RollbackRetireTimer` / `CloseAdapterAuthority` / `FinishRollback`) never
  mutates `caller_state`, `winner`, or any publication counter.

A partially-registered subset remains rollable (the `registration_count > 0`
conjunct preserves it; see Gate 4's R9 witness). States that cannot roll back:
`FinishRegistration` completed (central leaves `"Registering"`), caller already
`Waiting` (domain forbids), winner already claim/commit (`winner = NoArm`
forbids), publication already happened (phase has left `"Building"`).

### Refinement obligations (re-run independently)

```text
Central Claim -> Contract (E13SelectCentralClaim.cfg, RefinesContract):
253 generated / 111 distinct / queue 0 / depth 16 — PASS

Event/Timer -> Central Claim (E13SelectEventTimer.cfg, RefinesCentralClaim):
24,761 generated / 8,432 distinct / queue 0 / depth 30 — PASS
```

Both refinements exhausted with empty queues. The rollback-action refinement
mapping is consistent end-to-end (Event/Timer `BeginRollback` → Central
`CentralBeginRollback` → Contract `ContractBeginRollback`), each gated by the
matching enabled-domain; no layer hides a concrete rollback error path by
stutter or by an under-constrained guard.

```text
GATE 2: PASS — Central and Event/Timer rollback domains synchronized,
        both refinements reproduce.
```

## G. Terminal caller invariant (Gate 3)

`E13SelectContract.tla` adds, all conjoined into `ContractInv`
(lines 160–172):

- `BadTerminalWaiting` / `NoBadTerminalWaiting` (lines 135–139):
  forbids `contract_phase \in {"Aborted", "Destroyed"}` /\ `caller_state = "Waiting"`.
- `TerminalCallerStateWellFormed` (lines 141–150):
  `contract_phase \in {"Rollback", "Aborted"} => caller_state = "Running"`,
  and `Destroyed` requires `Running` (None/Inline) or `Resumed` (Suspended).
- `ContractRegistrationRollbackDisabledAfterSuspension` (lines 157–158):
  `caller_state = "Waiting" => ~ContractRollbackEnabledDomain`.

The Central and Event/Timer layers add the matching
`*RegistrationRollbackDisabledAfterSuspension` invariants, each conjoined into
`CentralInv` (lines 128–133) and `EventTimerInv` (lines 262–268). The terminal
`Waiting` prohibition flows from Contract up through both refinements.

The forbidden fake-fix — silently resetting `Waiting -> Running` inside a
rollback action — is absent: no `*BeginRollback`, `*RollbackRelease`,
`*FinishRollback`, or `*DestroyOperation` action writes `caller_state`. The
only actions that write `caller_state` are `SuspendCaller` (Running→Waiting),
`PublishSuspended` (Waiting→Runnable), `ResumeCaller` (Runnable→Resumed),
each gated by a publication/resume protocol.

```text
GATE 3: PASS — terminal Waiting caller unreachable; no Waiting->Running
        silent reset.
```

## H. R9 trace (Gate 4)

`E13Select.scene_rollback.cfg` declares `INVARIANT NotReach_R9` over the full
`E13SelectEventTimer` model on a 2-arm domain. TLC independently reached the
witness (865 generated / 380 distinct / 186 on queue / depth 7). The actual
TLC error trace (extracted by the reviewer, not copied from the author) is:

```text
State 1: <Initial predicate>
         contract_phase="Building", central_phase="Registering",
         caller_state="Running", winner=NoArm, registration_count=0,
         arm_registered={0:F,1:F}, adapter_phase={0:Detached,1:Detached}
State 2: <BeginRegistration line 302>
         arm_registered[0]=T, adapter_phase[0]="Registering",
         registration_count=0 (not yet counted)
State 3: <RegisterEventArm(0,0) line 316>
         adapter_phase[0]="Registered", arm_kind[0]="EventArm",
         registration_count=1, caller_state="Running", winner=NoArm
State 4: <BeginRollback line 716>
         contract_phase="Rollback", central_phase="Rollback",
         rollback_started=T, registration_count=1, caller_state="Running"
State 5: <RollbackEventArm(0) line 730>
         adapter_phase[0]="Finalized", arm_resolution[0]="Released",
         wait_outcome[0]="Cancelled", wait_linked[0]=F,
         finalization_step[0]="Rollback"
State 6: <CloseAdapterAuthority(0) line 657>
         adapter_phase[0]="Retired", authority_open[0]=F,
         retired_arm_count=1
State 7: <FinishRollback line 788>
         contract_phase="Aborted", central_phase="Aborted",
         caller_state="Running", winner=NoArm,
         registration_count=1, retired_arm_count=1,
         result_publication_count=0, runnable_publication_count=0
```

Witness against the R9 contract (lines 957–977):

| Required | Observed (state 7) | Result |
|---|---|---|
| `rollback_started` | TRUE | PASS |
| `central_phase \in {"Aborted","Destroyed"}` | "Aborted" | PASS |
| `caller_state = "Running"` | "Running" | PASS |
| `winner = NoArm` | NoArm (1000000) | PASS |
| `0 < registration_count < MaxArms` | 1, MaxArms=2 | PASS — strict subset |
| `result_publication_count = 0` | 0 | PASS |
| `runnable_publication_count = 0` | 0 | PASS |
| `\A i : arm_publication_count[i] = 0` | both 0 | PASS |
| registered arm: `Retired` + `Released` + `~authority_open` + `Rollback` | arm 0 matches | PASS |
| unregistered arm: `Detached` + `None` + `~authority_open` + `None` | arm 1 matches | PASS |

The witness is genuine partial-registration rollback, not a terminal-state
lookalike: only 1 of 2 arms was registered before `BeginRollback`, and the
unregistered arm is Detached with `arm_resolution = "None"` and no fabricated
authority.

```text
GATE 4: PASS — R9 reachable via genuine strict-subset registration rollback,
        causal witness verified.
```

## I. Rejected-trace regression (Gate 5)

`E13SelectContract.rollback_regression.cfg` (new) is a real positive gate, not
a duplicate of the Contract semantics cfg. It declares exactly:

```text
SPECIFICATION ContractSpec
INVARIANT TerminalCallerStateWellFormed
INVARIANT NoBadTerminalWaiting
INVARIANT ContractRegistrationRollbackDisabledAfterSuspension
CONSTANTS MaxArms = 2, Arms = {0, 1}
```

TLC independently PASSed it (809 generated / 346 distinct / queue 0 / depth 16).
The previously rejected trace —
`SuspendCaller → BeginRollback → RollbackRelease → CloseAuthority →
FinishRollback → DestroyOperation` ending in `Aborted`/`Destroyed` +
`caller_state = "Waiting"` — is now unreachable: `BeginRollback` cannot fire
from `Selecting`/`Waiting`, and the terminal-state invariants would catch any
residual.

### Anti-vacuity — independent reviewer-only probes

To prove the PASS is not achieved by deleting an action or by a state
constraint, a reviewer-only module `E13ContractProbes.tla` (kept under
`/tmp/e13-corrective-review/probes/`, outside the repository) was written. It
`INSTANCE`s `E13SelectContract` unmodified and asserts named inverse
predicates. Six probes were run with explicit private `-metadir`:

| Probe | Expectation | Result | Generated/Distinct/Queue |
|---|---|---|---|
| `NotProbeSuspendHappens` (caller Waiting reachable) | REACHABLE | **REACHED** (good) | 28 / 18 / 8 |
| `NotProbeRollbackRunning` (Rollback + Running reachable) | REACHABLE | **REACHED** (good) | 5 / 4 / 2 |
| `NotProbeAbortedReachable` (Aborted + Running) | REACHABLE | **REACHED** (good) | 13 / 8 / 3 |
| `NotProbeDestroyAfterAbortReachable` (Destroyed + Running after Aborted) | REACHABLE | **REACHED** (good) | 22 / 13 / 4 |
| `NotProbeRejectedTerminalWaiting` (Aborted/Destroyed + Waiting) | UNREACHABLE | **PASS** (blocked) | 809 / 346 / 0 |
| `NotProbeRollbackWhileWaiting` (Rollback/Aborted/Destroyed + Waiting) | UNREACHABLE | **PASS** (blocked) | 809 / 346 / 0 |

Interpretation:

- `ContractSuspendCaller` is still modelled and still fires (probe A reached
  `caller_state = "Waiting"`). Suspension was not deleted.
- The pre-suspension `ContractBeginRollback` still fires (probe B reached
  `Rollback` + `Running`).
- `FinishRollback` → `Aborted` still fires (probe C).
- `DestroyOperation` after a valid `Aborted` still fires (probe D).
- The only thing now blocked is the **combination** `SuspendCaller` →
  `BeginRollback` (probes E and F clean), exactly as required.

The two unreachability probes exhausted the **same 809-state graph** as the
Contract semantics gate, confirming this is the full unconstrained Contract
state space (no `STATE` constraint, no `ASSUME FALSE`, no `Init` crippling).
The gate is a real check, not a state-constraint suppression.

```text
GATE 5: PASS — rejected trace unreachable; anti-vacuity confirmed.
```

## J. Source-safe verifier (Gate 6)

`tools/formal/verify-e13-select-core.sh` (lines 22–45):

- `outroot="$(mktemp -d -t e13-select-core.XXXXXX)"`; `workdir="$outroot/work"`.
- All `*.tla` and `*.cfg` are copied into `$workdir` (lines 33–34); TLC runs
  from `$workdir` (line 36).
- Every per-run `-metadir` is `$outroot/$tag.meta` (lines 40–41).
- All TLC stdout/stderr go to `$outroot/$tag.out` (line 44).
- `cleanup` (lines 24–29) executes only after a two-clause defensive guard:
  `[[ -n "$outroot" ]]` AND a prefix-shape match against
  `/tmp/e13-select-core.*` or `${TMPDIR:-/tmp}/e13-select-core.*`. Only then
  does `rm -rf -- "$outroot"` run.
- No `rm`/`find -delete` targets `$spec`, `$repo`, or any source path.

The defensive guard rejects every dangerous input: empty `outroot` (fails
`-n`), `/` (fails prefix match), the repository directory (fails prefix match),
or any other unexpected path. `bash -n` reports no syntax error; ShellCheck
reports only the pre-existing SC2329 info (cleanup invoked indirectly via
`trap`).

### Sentinel test (independently executed)

```bash
sentinel="docs/spec/e13_select/E13SelectUserTTrace.keep"
printf 'user-owned sentinel\n' > "$sentinel"
TLC_WORKERS=1 tools/formal/verify-e13-select-core.sh   # full 21-gate run
test -f "$sentinel"                                     # -> exists
test "$(cat "$sentinel")" = "user-owned sentinel"      # -> content intact
rm "$sentinel"
```

Result: sentinel **survived** the full run with content intact. Post-run
inspection of `docs/spec/e13_select/` found no new `*TTrace*`, `*.meta`, or
`states` artifact beyond the pre-existing gitignored `states/` remnant. The
pre-existing untracked `tests/test_t3_simple.cpp` and `tla2tools.jar` were
unchanged.

```text
GATE 6: PASS — verifier is source-safe; sentinel preserved.
```

## K. Config/document drift (Gate 7)

- No stale `E13SelectContract.reach.cfg` reference remains in any plan, design,
  README, or cfg. (`grep` over `docs/` and `tools/` finds the token only inside
  the two review documents, where it describes the fix.)
- `E13Select.scene_inline.cfg` claims only R1 and points to
  `scene_inline_timer.cfg` for R2.
- `E13Select.scene_suspended.cfg` claims only R3 and points to
  `scene_suspended_timer.cfg` for R4.
- `E13Select.scene_2mix.cfg` claims only R5 and points to
  `scene_2mix_timer_winner.cfg` for R6.
- `E13Select.scene_rollback.cfg` describes strict-subset partial-registration
  rollback (R9) accurately.
- `E13Select.scene_staletimer.cfg` describes the actual `TimerPumpSkip` witness
  (R10), not mere coexistence.
- Plan markdown headings use `## 任务 N` (MD001-compliant); the historical
  `### 任务 1` warning is gone.
- `git diff --check origin/master...HEAD` is clean; the seven historical
  blank-line-at-EOF warnings are gone.

```text
GATE 7: PASS — config/document drift closed; whitespace clean.
```

## L. GitHub thread disposition

`gh api repos/jnhu76/Sluice/pulls/17/comments` and the `reviewThreads` GraphQL
were read. 11 inline threads exist (5 from `gemini-code-assist`, 6 from
`coderabbitai`); no human reviewer has posted. Thread resolution state was
cross-checked against current code rather than trusted from the `resolved`
flag (several Gemini threads remain GitHub-unresolved despite being fixed,
because the author cannot resolve bot threads).

| # | Bot | Path:line (at e61a0b4 unless noted) | Ask | Disposition |
|---|---|---|---|---|
| 1 | gemini | `E13SelectContract.tla:369` | forbid rollback after suspension | **ADDRESSED — CURRENT CODE VERIFIED** (`ContractRollbackEnabledDomain` Building/NoArm/Running; independently probed unreachable) |
| 2 | gemini | `E13SelectContract.tla:169` | add terminal Waiting invariant | **ADDRESSED — CURRENT CODE VERIFIED** (`NoBadTerminalWaiting`+`TerminalCallerStateWellFormed`+`ContractRegistrationRollbackDisabledAfterSuspension` in `ContractInv`) |
| 3 | gemini | `verify-e13-select-core.sh:30` | TLC temp dir, no source deletion | **ADDRESSED — CURRENT CODE VERIFIED** (mktemp + defensive cleanup; sentinel preserved) |
| 4 | gemini | `e13-select-formal-core-plan.md:19` | stale `reach.cfg` ref | **ADDRESSED — COMMENT OUTDATED** (plan now names `reach_inline.cfg`/`reach_suspended.cfg`) |
| 5 | gemini | `E13Select.scene_2mix.cfg:5` | scene_2mix claims R5+R6 | **ADDRESSED — COMMENT OUTDATED** (now claims only R5, points to R6 cfg) |
| 6 | coderabbit | `e13-select-formal-core-plan.md:13` | MD001 `### 任务 1` | **ADDRESSED — CURRENT CODE VERIFIED** (now `## 任务 1`) |
| 7 | coderabbit | `e13-select-formal-core-plan.md:23` | plan reach filenames | **DUPLICATE** of #4 — **ADDRESSED** |
| 8 | coderabbit | `E13Select.scene_inline.cfg:14` | R2 claim | **ADDRESSED — CURRENT CODE VERIFIED** (now points to `scene_inline_timer.cfg`) |
| 9 | coderabbit | `E13Select.scene_suspended.cfg:4` | R3/R4 drift | **ADDRESSED — COMMENT OUTDATED** (now points to `scene_suspended_timer.cfg`) |
| 10 | coderabbit | `verify-e13-select-core.sh:30` | TLC temp dir | **DUPLICATE** of #3 — **ADDRESSED** |
| 11 | coderabbit | `CORRECTIVE-1-REVIEW-REQUEST.md:32` (at b4afe6c) | record immutable corrective HEAD SHA in §F | **STILL VALID (P2-1)** — see Section O. The cross-reference "the exact SHA is recorded in the author's final self-assessment" is inaccurate: §F contains only verdict labels, no SHA. Non-blocking; the SHA is runtime-resolvable (`git rev-parse origin/...`) and was bound successfully in §B. |

Per the review protocol, this review did **not** reply to or resolve any
GitHub thread (the user has not asked for that). Bot `resolved`/`outdated`
flags were not trusted; each thread was verified against the current candidate
code.

```text
GITHUB REVIEW FEEDBACK: ADDRESSED OR NON-BLOCKING
```

## M. Full TLC evidence

Run: `TLC_WORKERS=1 tools/formal/verify-e13-select-core.sh` from a clean
`mktemp` workspace. 21 gates, exit code 0. Every metric below was re-extracted
from this run, not copied from the author's table. All metrics reproduce the
author's self-assessment exactly.

| Gate | Generated | Distinct | Queue at stop | Depth | Result |
|---|---:|---:|---:|---:|---|
| Contract semantics | 809 | 346 | 0 | 16 | PASS |
| Contract registration-rollback regression | 809 | 346 | 0 | 16 | PASS |
| Central Claim + Contract refinement | 253 | 111 | 0 | 16 | PASS |
| Event/Timer adapters + Central refinement | 24,761 | 8,432 | 0 | 30 | PASS |
| 4-arm bounded mixed root | 71,868 | 17,108 | 0 | 40 | PASS |
| Contract inline reach | 530 | 240 | 43 | 11 | expected witness `NotReachContractInline` |
| Contract reversible-reservation suspended reach | 691 | 299 | 23 | 13 | expected witness `NotReachContractReservationSuspended` |
| Central Claim snapshot tie-break reach | 55 | 31 | 9 | 7 | expected witness `NotReachCentralTieBreak` |
| R1 Event admission-ready inline | 12,358 | 4,430 | 520 | 15 | expected witness `NotReach_R1` |
| R2 Timer admission-ready inline | 14,867 | 5,305 | 493 | 17 | expected witness `NotReach_R2` |
| R3 Event post-suspension | 16,167 | 5,763 | 482 | 18 | expected witness `NotReach_R3` |
| R4 Timer post-suspension | 17,508 | 6,231 | 448 | 19 | expected witness `NotReach_R4` |
| R5 Event winner + Timer loser | 9,274 | 3,389 | 587 | 13 | expected witness `NotReach_R5` |
| R6 Timer winner + Event loser | 10,565 | 3,808 | 554 | 14 | expected witness `NotReach_R6` |
| 3-arm mixed Event winner + Timer losers | 87,437 | 27,074 | 4,130 | 16 | expected witness `NotReach_R5` |
| R7 claim snapshot tie-break | 453,829 | 134,998 | 35,771 | 16 | expected witness `NotReach_R7` |
| R8 same-Event multi-arm broadcast | 10,498 | 3,781 | 558 | 14 | expected witness `NotReach_R8` |
| R9 partial-registration rollback | 865 | 380 | 186 | 7 | expected witness `NotReach_R9` |
| R10 stale TimerRegistration skip | 17,819 | 6,346 | 442 | 19 | expected witness `NotReach_R10` |
| R11 inline result consumption | 13,797 | 4,932 | 501 | 16 | expected witness `NotReach_R11` |
| R12 suspended result consumption | 18,655 | 6,618 | 403 | 20 | expected witness `NotReach_R12` |

Every reachability gate hit its **expected** named inverse invariant (no
deadlock, no other invariant failure, no parse/config failure). Every safety
and refinement gate exhausted with queue 0.

Reviewer-only bounded probes (kept outside the repository under
`/tmp/e13-corrective-review/`):

| Probe | Generated | Distinct | Queue | Depth | Result |
|---|---:|---:|---:|---:|---|
| Contract: suspend reachable | 28 | 18 | 8 | 5 | REACHED (good) |
| Contract: rollback+Running reachable | 5 | 4 | 2 | 2 | REACHED (good) |
| Contract: Aborted+Running reachable | 13 | 8 | 3 | 3 | REACHED (good) |
| Contract: Destroyed+Running after Aborted | 22 | 13 | 4 | 4 | REACHED (good) |
| Contract: rejected terminal Waiting | 809 | 346 | 0 | 16 | UNREACHABLE (good) |
| Contract: rollback while Waiting | 809 | 346 | 0 | 16 | UNREACHABLE (good) |

## N. Scope audit

```text
git diff --name-status e61a0b4..b4afe6c
```

All 21 changed paths are under `docs/` or `tools/`. No file under `include/**`,
`src/**`, `tests/**`, `examples/**`, `benchmarks/**`, or any build-policy path
(CMakeLists, Makefile, meson.build, CI workflow) is touched. The full-PR diff
`246505a..b4afe6c` (30 paths) is likewise confined to `docs/` and `tools/`.

No PR #18 negative model (`NEG-C*`/`NEG-S*`/`NEG-E*`/`NEG-T*`) is introduced.
No Semaphore/Mutex/Queue/Condition/AsyncCondition adapter, no alternative
selection strategy, and no post-suspension cancellation **implementation** is
introduced. The corrective makes post-suspension rollback provably unreachable;
it does not model the deferred cancellation protocol itself.

```text
SCOPE: CLEAN — no production/test/build/API change; no PR #18 content.
```

## O. Findings

### P2-1 — Documentation: corrective-HEAD SHA not recorded in §F of the review request

- Location: `docs/reviews/E13-SELECT-FORMAL-MODEL-CORE-1-CORRECTIVE-1-REVIEW-REQUEST.md:31`
  (§A says "The exact SHA is recorded in the author's final self-assessment")
  vs `...:203–247` (§F "Self-assessment verdict" contains only verdict labels,
  no SHA).
- GitHub thread: coderabbitai #3611871148 (unresolved).
- Reproducible: open §F and search for a 40-hex SHA; none is present.
- Why non-blocking: §A also instructs the reviewer to re-resolve at review time
  with `git rev-parse origin/feat/e13-select-formal-model-core`, which yields
  `b4afe6c...` and matches the GitHub `headRefOid`. The binding in §B of this
  review succeeded using that instruction. The defect is a stale
  cross-reference, not a missing or moved head.
- Suggested fix (author's discretion): either paste the concrete SHA into §F,
  or reword §A to "re-resolve at review time with `git rev-parse`" without
  claiming §F contains it.

No P0 or P1 finding. No collateral semantic regression was introduced by the
corrective delta.

## P. Authorization effect

```text
PR #17 MERGE:
AUTHORIZED

PR #18 FORMAL SAFETY IMPLEMENTATION:
AUTHORIZED — the historical P0-1 and P1 blockers are closed; the corrected
Contract and both refinements reproduce; the rejected post-suspension rollback
trace is independently verified unreachable. PR #18 may now begin its own
layered-safety / negative-model / non-vacuity / refinement work, which will
require its own independent review before any further authorization.

PRODUCTION IMPLEMENTATION:
DENIED — production remains denied until PR #18's layered safety, negative
models, non-vacuity, and refinement are independently reviewed. "PR #18
AUTHORIZED" above authorizes the formal-safety work, not a production build.

ALTERNATIVE STRATEGIES:
NOT IMPLEMENTED / NOT AUTHORIZED

ADDITIONAL ARM TYPES:
NOT IMPLEMENTED / NOT AUTHORIZED
```

The historical `REQUEST-CHANGES` verdict on
`E13-SELECT-FORMAL-MODEL-CORE-1-INDEPENDENT-REVIEW-1` stands as the historical
record. This delta review is a **separate** PASS artifact that supersedes the
historical denial **for the corrected candidate head only**.

## Q. Final repository status

This review turn added only:

```text
docs/reviews/E13-SELECT-FORMAL-MODEL-CORE-1-CORRECTIVE-1-DELTA-REVIEW-1.md
```

No TLA module, cfg, verification script, README, design, plan, historical
independent review, corrective review request, production source, production
test, public API, or build-policy file was modified by this reviewer.
Reviewer-only adversarial probes (`E13ContractProbes.tla` and its cfgs) and
all TLC metadir/output artifacts were kept under `/tmp/e13-corrective-review/`
and are outside the repository. The reviewer-created source sentinel
`docs/spec/e13_select/E13SelectUserTTrace.keep` was removed after the
source-preservation test.

The worktree differs from the reviewed candidate head only in this new review
artifact. The pre-existing untracked `tests/test_t3_simple.cpp` and
`tla2tools.jar` remain untracked and untouched; nothing was staged, committed,
or pushed.

```text
REVIEWED CANDIDATE HEAD:
b4afe6c3736b3af73a8d0c82e994040874c24ac9

REVIEW ARTIFACT COMMIT:
(to be recorded after this file is committed; it is not required to equal
REVIEWED CANDIDATE HEAD and must not be back-dated into it)
```
