# S1A — C++ ↔ TLA+/GenMC Formal Coverage

**Status:** LIVE — the frozen coverage matrix for #289 Phase S1A (execution
issue #294).

**Baseline:** S0B merge `d2eeac40bcd3ae15eb39b743def4e945d00e6033` (PR #293,
issue #292). Branch: research/safety-s1a-formal-coverage. The pre-S0B
read-only inventory (branch tip `9c13b597`) survives in git history and issue
#294; all its PROVISIONAL verdicts were re-audited against the frozen S0B
contract (`docs/investigations/s0b-contract-reality.md`, B01–B10 all ALIGNED)
before freezing — no verdict changed classification except where noted.

**What this document is:** the one-place answer, per Safety property F01–F10
(= S0B B01–B10), to: which model covers it, under what assumptions/bounds,
which negative control proves the checker can fail, which C++ test witnesses
the correspondence, and what remains unproved. Two independent judgments per
property: **A = protocol-model coverage**, **B = implementation
correspondence**.

**Claim vocabulary:** `MODEL_COVERS_PROPERTY` / `PARTIALLY_MODELED` /
`MODEL_SCOPE_EXCLUDED` / `CORRESPONDENCE_WEAK` / `UNMODELED` /
`NOT_SUITABLE_FOR_TLA` / `GENMC_REQUIRED` / `UNKNOWN`. Never `PROVED`,
`FULLY VERIFIED`, or `C++ CORRECT` about production.

**Method:** S0B frozen contract → per-property model audit (Init/actions/
invariants/fairness/bounds read against current C++ at `d2eeac40`) →
adversarial questions from the S0B handoff (#262 model-adversary) and
#223 (startup population) → repair where justified → negative control +
C++ bridge for every strengthened model → freeze.

## Repair summary (what S1A changed and why)

| Change | Layer | Driving evidence |
| --- | --- | --- |
| `RecordTerminalKernelCanceled` action + `kernel_canceled` legitimate source + updated `InvCanceledTerminalSource` law (three-class recording-source admissibility) + `NotReach_W6_KernelCanceledNoIntent` witness + verifier W6 gate + manifest updates | TLA model (request-arena) | S0B handoff question #262: the model could NOT represent "no caller cancel + operation CQE returns -ECANCELED + terminal propagated verbatim", and `InvCanceledTerminalSource` structurally encoded `canceled ⇒ cancel-arbitration provenance` — an assumption the C++ does not make (B03/B06: real result wins verbatim) |
| `fake_kernel_canceled_completion_verbatim_without_caller_cancel` regression | C++ bridge | Same; completes an op with kernel-canceled status, no caller cancel; asserts verbatim canceled terminal, exactly-once publication, accounting retirement |
| Worker-population establishment boundary DECLARED (e9 README section, trace-conformance doc boundary #5, manifest e9 notes) | docs (model correspondence boundary) | #223: E9 `Init` assumes full population; C++ multi-worker startup legally has partially activated states (per-thread `active` publication, `scheduler.cpp` run_impl lambda); exclusion was deliberate but undocumented — Option A repair (document + prehistory obligation); startup refinement registered as S2 candidate |
| Stale "V2/V3 not started" claims in `cpp-model-coverage.md` corrected; request-arena matrix row + debt register refreshed (W1–W6) | docs | V2 (#196) and V3 (#197) evidence already exists; the register claimed otherwise |
| CancelToken GenMC kernel: **evaluated, NOT REQUIRED** (decision recorded below) | evaluation | F06 weak-memory face; §8 boundary analysis |

## #262 model-adversary question — answered

> Can the current cancellation / uring models represent: no caller-requested
> cancel + an operation CQE returning -ECANCELED + the terminal result
> propagated verbatim? Does any model incorrectly assume: canceled terminal
> ⇒ caller cancel intent existed?

```text
PRE-REPAIR REALITY (audited at d2eeac40):
  request-arena:  RecordTerminal hard-coded terminalCanceled' = FALSE — the
                  ordinary backend completion could carry NO canceled payload;
                  the only canceled-terminal paths required cancel arbitration
                  (CancelPendingOrEnqueued) or a confirmed interruption with
                  live intent (RecordCanceledConfirmed). ANSWER: could NOT
                  represent the #262 shape.
                  InvCanceledTerminalSource's conjunct
                  (terminalCanceled ⇒ source ∈ LegitimateCancelSources) was
                  vacuous-against-reality in exactly this respect: a verbatim
                  kernel -ECANCELED (legal C++: finalize_operation_terminal_
                  records the real result; B03/B06 freeze) had no
                  representation, so the invariant silently encoded
                  "canceled ⇒ cancel provenance". ANSWER: YES, the assumption
                  existed.
  d1-uring-poison: no cancel semantics at all (transport ledger/poison only)
                   — no assumption. ANSWER: n/a.
  cancel-token-epoch: models the task-layer token word (delivery/epoch), has
                   no I/O-terminal vocabulary — no assumption. ANSWER: n/a.
  e16: "canceled" appears only as lifecycle API result comments — no
                   terminal/intent assumption. ANSWER: n/a.

FIX (this phase, issue #294):
  RecordTerminalKernelCanceled (enqueued/running → backend_ready, terminal
  canceled VERBATIM, consumes any live intent, source "kernel_canceled");
  LegitimateCancelSources += "kernel_canceled"; invariant comment states the
  canceled-terminal recording-source admissibility law (the stamp identifies
  the recording path, not cancellation cause or caller-intent absence);
  NotReach_W6_KernelCanceledNoIntent pins non-vacuity EXISTENTIALLY
  (a reachable canceled terminal from the kernel action with NO intent
  ever recorded — not the universal "every kernel_canceled is intent-free");
  NEG-RA-6 unchanged and still exact (ill-behaved caller still stamps the
  forbidden "cancel_running"). The reference-backend C++ bridge pins the
  shared arena/reap/publication contract for a canceled backend result
  (complete-with-canceled, zero cancel calls → verbatim canceled terminal,
  exactly-once, accounting retired); it does NOT exercise the real io_uring
  CQE translation path.

VERDICT: REPAIRED at #294. Post-repair, the model represents the #262 shape
and the invariant asserts the contract's actual three-class
recording-source admissibility law. The
unresolved root-cause/attribution question of #262 itself (which kernel
path produced the CQE) remains with the later real-incident track, per the
S0B residual — S1A only closes the formal-representation gap.
```

## #223 worker-start population — adjudicated

```text
CURRENT REALITY (d2eeac40): multi-worker path publishes WorkerState::active
  inside each spawned thread (run_impl thread lambda); single-worker path
  publishes from the caller thread. Partially activated populations are
  legal; R2 election legitimately picks the lowest ACTIVE worker in that
  window (#210 role-swap shape; no production defect alleged).
MODEL REALITY: E9ParkWake Init = workerAlive TRUE for all; workerPhase
  "Active"; only TRUE→FALSE (retire) transitions; partial activation
  unreachable from Init.
CAN A VIOLATION LIVE ONLY IN THE EXCLUDED STATE?  None found: the excluded
  states affect only LowestAlive-based role assignment; E9 invariants are
  role-symmetric; #210's primary failure was a test-prehistory artifact
  (#222 repaired via ticket-seam handshake). But that "none" rests on
  argument, not model evidence — which is why the boundary must be explicit.
DELIBERATE?  Historically yes in substance, undocumented in fact — not a
  completed abstraction exclusion until declared.
FIX: Option A — declare the refinement boundary ("begins after worker
  population establishment") in the e9 README, the trace-conformance owner
  doc (boundary #5), and the manifest notes; require every conformance/trace
  prehistory to causally establish population establishment (#222 T4
  pattern; corpus fixtures carry the prehistory field). Option B (model
  StartWorker + startup-skew interleavings) is registered as an S2
  stronger-proof candidate, not built here (no driving defect; higher
  formal value but real cost).
VERDICT: CORRESPONDENCE boundary now DOCUMENTED (was
  CORRESPONDENCE_WEAK-by-omission). E9 green must not be read as covering
  startup skew — stated in all three places.
```

## Property matrix (frozen)

Per-property fields follow the S1A spec: CONTRACT (S0B frozen clause) ·
C++ OWNER · MODEL · PROPERTY · INIT · FAIRNESS · BOUNDS · NEGATIVE ·
C++ BRIDGE · MODEL COVERAGE (A) · CORRESPONDENCE (B) · CHANGE · RESIDUAL.

### F01 request identity / generation / stale reuse (B01 — ALIGNED)

```text
CONTRACT:     stable K=(context,slot,generation); generation++ BEFORE freed
              slot re-enters free list; stale g can never mutate/terminalize/
              observe the new occupant (never wraps).
C++ OWNER:    RequestArena::validate_ / free_slot_locked_ (request_arena.hpp,
              request_slot.hpp); RequestHandle private-ctor authority.
MODEL:        request-arena (single slot, MaxGen=2 state-space cap).
PROPERTY:     InvGenAdvanceOnFree; NEG-RA-2 causal stale-cancel target
              InvTerminalRequiresAccepted; W4 reuse window.
INIT/FAIRNESS/BOUNDS: Init free/gen=0; no fairness (safety); 1 slot,
              gen ≤ 2 (≥1 full reuse cycle explored).
NEGATIVE:     NEG-RA-4 (no-gen-increment), NEG-RA-2 (causal stale cancel).
C++ BRIDGE:   request_lifecycle_scheme_b_test::generation_reuse_stale_attempts;
              request_handle_test; negative-compile gates (6/6, 9, 12/12).
A:            MODEL_COVERS_PROPERTY (single-slot protocol).
B:            STRONG — pure-mutex leaf (zero atomics ⇒ SC abstraction exact);
              arena is the sole identity authority (S0B grep audit).
CHANGE:       none this phase.
RESIDUAL:     multi-slot interleavings = executable-evidence scope (manifest
              coverage_gaps, ON-TOUCH on ready-ring rule change).
```

### F02 admission / submit transaction / rollback (B02 — ALIGNED)

```text
CONTRACT:     successful submit = fully committed lifecycle; failed submit =
              zero residue; no rejection return past the binding→outstanding
              LP.
C++ OWNER:    detail::submit_transaction.hpp ladder + per-backend
              SubmitPolicy; RequestArena reserve/prepare/commit/rollback;
              Completion begin/commit/rollback binding.
MODEL:        request-arena (leaf stages + RollbackPreCommit + Destroy
              guards); GenMC K1 (idle→binding→outstanding CAS chain,
              memory-order face, #197).
PROPERTY:     leaf admission/rollback protocol; P2 (observer of
              `outstanding` sees installed binding) under RC11/RA/SC/TSO.
NEGATIVE:     leaf death tests + C2d injection; GenMC N3 (commit-CAS→relaxed
              REJECTED under rc11+ra).
C++ BRIDGE:   reference_backend_no_alloc_test (transactional rejection under
              throwing operator new); completion_binding_test; backend
              conformance capacity rows.
A:            PARTIALLY_MODELED — the submit-vs-close_admission arbitration
              and multi-stage rollback ladder have no dedicated protocol
              model; the leaf stages and the publication memory-order face
              are covered.
B:            STRONG for what is modeled (S0B grep-audited single-invoker
              publication; transactional rollback witnessed).
CHANGE:       coverage_gaps entry updated: residual (c) split into the
              memory-order face (#197 kernel: COVERED) and the submit-
              transaction protocol face (remains recorded, no driving
              defect → not modeled per AGENTS §7).
RESIDUAL:     `no_space` vocabulary nuance (S0B B02 residual, unchanged).
```

### F03 terminal winner / exactly-once (B03 — ALIGNED)

```text
CONTRACT:     exactly one terminal per accepted generation; first
              backend_ready wins; losers never overwrite/double-push/
              double-account; Scheme-B single-domain arbitration.
C++ OWNER:    RequestArena::record_terminal / cancel under the leaf mutex;
              backend producers (threadpool run_syscall; uring
              finalize_operation_terminal_).
MODEL:        request-arena; e10-waitnode (primitive domain, inherited by
              all e12 suites); e13-select core+safety (winner
              linearization + 29-FAULT restoration).
PROPERTY:     InvNoDoubleTerminal; InvNoDoublePublication;
              InvTerminalRequiresAccepted.
NEGATIVE:     NEG-RA-1; BuggyNoWinner; e13 NEG-C/S/E/T/A + MG sets (best
              negative architecture in the tree).
C++ BRIDGE:   exactly_one_terminal_winner; pending_cancel_wins_before_
              enqueue_then_enqueue_noop; backend_scheme_b_race_test (real
              two-thread race).
A:            MODEL_COVERS_PROPERTY (every domain, negatives everywhere).
B:            STRONG.
CHANGE:       none. RESIDUAL: none beyond F01's multi-slot note.
```

### F04 Completion publication (B04 — ALIGNED)

```text
CONTRACT:     only the designated reap path makes a caller-owned Completion
              ready; ready release-store is the single LP; observer of ready
              sees result + closed registration + retired accounting + ended
              borrow.
C++ OWNER:    RequestArena::reap → CompletionBinding::publish thunk →
              AsyncBackend::publish → Completion::publish_from_reap
              (completion.hpp); grep-audited single invoker.
MODEL:        request-arena (reap-only, NEG-RA-3); f1-wait-record (waiter
              E7-T2 single publication); GenMC K1/K2 (#197).
PROPERTY:     InvPublishedCompleteness; K1 P1 (acquire-ready observer sees
              full payload + reap_seq); K2 P3a/P3b (round-distinguished
              reuse reads).
NEGATIVE:     NEG-RA-3; BuggyDuplicatePublish; GenMC N1/N1b/N2/N3/N4 (N4 =
              pre-registered prediction disproved by the checker).
C++ BRIDGE:   completion_authority_death_test;
              acquire_observer_of_ready_sees_all_effects; kernel = exact
              production-order extraction at pinned revision.
A:            MODEL_COVERS_PROPERTY (protocol + memory-order, dual layer).
B:            STRONG (bounded-kernel claim boundary explicit: no liveness,
              not whole-program, loser-CAS fail-fast paths out of scope —
              documented in the kernel doc).
CHANGE:       none (#197 predates; stale doc claims corrected).
RESIDUAL:     arena/scheduler/progress machinery outside the kernels (their
              TLA suites own it).
```

### F05 borrow / backend lifetime retirement (B05 — ALIGNED)

```text
CONTRACT:     borrow begins at commit, ends at completion-ready publication
              in one critical section; release requires completion_ready.
C++ OWNER:    request_arena leaf (borrowActive window; I7/I18); backend
              prepared-op handoff under the work lock (no pop-before-running
              gap); Uring fixed-file/registered-buffer interactions deferred
              (DIV-09).
MODEL:        request-arena (InvBorrowWindow); d1-uring-poison (transport
              ledger identity / premature-CQE negatives); e9-wake-handle-
              lifetime (handle-lease variant).
NEGATIVE:     d1's masked-slot-identity + premature-original-CQE
              publications (name-asserted).
C++ BRIDGE:   backend_c2c_waiter_borrow_test matrix (windows via
              generation-validated borrow_for_test seam);
              request_waiter_borrow_lease_test.
A:            PARTIALLY_MODELED — no dedicated multi-slot borrow model
              (S0B handoff). Adjudicated: stays executable-evidence; no
              driving defect, single-slot window + C2c matrix carry the
              load (AGENTS §7).
B:            STRONG for the modeled window; SE-2 H01 reconciliation: the
              SE-2 "TLA+ n/m" cell refers to the caller-side buffer-lifetime
              hazard face, NOT to the arena borrow window (which IS modeled
              — InvBorrowWindow). Recorded here so the two ledgers are not
              misread as contradicting each other; the frozen SE-2 JSON
              artifact is a pinned campaign record and is intentionally not
              rewritten.
CHANGE:       none (reconciliation documented). RESIDUAL: DIV-09 uring
              registered resources; multi-slot borrow interleavings.
```

### F06 cancel vs ordinary completion (B06 — ALIGNED)

```text
CONTRACT:     layered cancellation; pending/enqueued cancel may win the
              terminal (Scheme B); running cancel records intent only —
              real result wins verbatim; kernel-owned cancel CQE is
              control-informational; stale generation ⇒ not_found;
              terminal ⇒ already_terminal.
C++ OWNER:    RequestArena::cancel (single disposition authority);
              cancel_intent_ consumed by the winner; uring
              cancel_handle_/issue_running_cancel (one per-slot
              cancel_queued bit); CancelToken/CancelState (task layer);
              per-primitive cancel/reconcile (e12-rwlock etc.).
MODEL:        request-arena (Scheme-B + intent law + NEW
              RecordTerminalKernelCanceled); cancel-token-epoch (single-shot
              per epoch per consumer between the two explicit re-arm
              authorities); e12-rwlock (cancel reconcile + WriterRevoke
              control); e13 (cancel/restore); e11 (cancel/expire).
PROPERTY:     InvCanceledTerminalSource (canceled-terminal recording-source
              admissibility law, three classes, post-#294);
              InvSingleShotPerEpoch; RW cancel/expire reconcile invariants.
INIT/FAIRNESS/BOUNDS: per-suite (arena: no fairness, MaxGen=2; token: no
              fairness clause — rearm() is an explicit advance path).
NEGATIVE:     NEG-RA-6 (exact: forbidden cancel_running stamp, still fires
              post-#294); NEG-CT1..5; WriterRevoke (sensitivity-controlled).
C++ BRIDGE:   request_arena_cancel_intent_test (6 cases — ordinary success
              NOT rewritten); cancel_token_test (ADR RED→GREEN);
              rwlock audit R1–R5; NEW: fake_kernel_canceled_completion_
              verbatim_without_caller_cancel (#262 bridge).
A:            MODEL_COVERS_PROPERTY (protocol layer, now including the
              kernel-canceled shape).
B:            STRONG at protocol layer; weak-memory face EVALUATED
              (argument-based, not GenMC-checked): GenMC kernel NOT
              REQUIRED — the token is a single-atomic-word protocol
              (state_ is the only shared data; request/rearm/clear are
              word-only RMWs), per-consumer CancelState fields are plain
              but thread-confined by fiber ownership, and every
              post-delivery cross-thread action goes through separately
              synchronized APIs. A kernel would have no discriminating
              power (the Completion kernel's N1-class hazards need plain
              payload publication, which the token lacks). Boundary
              recorded: CancelState::acknowledged() is a best-effort
              introspection over a plain field — no cross-thread ordering
              claim. REOPEN triggers (any one): CancelState becomes
              cross-thread visible/read; the token protocol becomes
              multi-word or multi-atomic; correctness starts depending on
              ordering between the token atomic and another shared
              atomic/state; epoch/reuse semantics introduce an
              ABA-sensitive cross-thread relation.
CHANGE:       #262 model repair + bridge (above).
RESIDUAL:     uring control-CQE/deferred-terminal interplay unmodeled (the
              single-driver domain assumption is the documented abstraction
              boundary — S0B handoff, adjudicated: keep, the deferred-
              until-control-retires rule is executable-evidence scope);
              RecordCanceledConfirmed has no production caller today
              (future-backend obligation, PR #125 P1-2 law unchanged).
```

### F07 deadline / timeout arbitration (B07 — ALIGNED)

```text
CONTRACT:     deadline vs resource-grant/complete/cancel precedence per
              primitive (resource-first timed admission; due beats parked).
C++ OWNER:    scheduler_timer.cpp (timer wait); scheduler_rwlock.cpp
              (*_lock_until); scheduler_event.cpp (unmodeled face).
MODEL:        e11-timer-wait (I1–I7; strongest-grounded fairness in the
              tree); e12-rwlock (InvResourceFirstDeadline +
              DeadlinePrecedence sensitivity control — ONLY the due=TRUE
              successor mutates, same mutant passes the other 12 invariants).
NEGATIVE:     NEG-1..6 (real defect classes); DeadlinePrecedence.
C++ BRIDGE:   timer tests (advance_clock deterministic driver); rwlock M3
              timed cases.
A:            MODEL_COVERS_PROPERTY (timer + rwlock faces);
              e12-event set-vs-due admission = PARTIALLY_MODELED (recorded
              ON-TOUCH debt; the pattern is pinned one suite over).
B:            STRONG. CHANGE: none. RESIDUAL: e12-event debt row (trigger
              un-fired); S0B handoff "precedence variants not uniformly
              modeled" stands as recorded debt, not new work.
```

### F08 wait / wake / lost-wakeup closure (B08 — ALIGNED)

```text
CONTRACT:     every progress-enabling change declares persistent predicate/
              producer/consumer/signal/commit-to-sleep closure; Threaded/
              Evented keep distinct physical wait mechanisms.
C++ OWNER:    scheduler_park_wake.cpp + scheduler.cpp (park commit,
              signal_wake_locked, epoch, split-wait bridge); e12 primitive
              admission closures; fiber_ctx.cpp physical switch.
MODEL:        e9-park-wake (repair chain #185/#189/#191; non-vacuity
              witnessed); spawn-wake-epoch (#115 exact negatives);
              e8-suspend-switch; e12-async-condition (lost-notify closure);
              e12-rwlock-scheduler-liveness (#161); e9-trace-conformance
              (C++ traces ⊆ model, corpus).
PROPERTY:     no-lost-wake family (Life1–8), InvNoCauselessReturn (both
              configs), split-wait safety/liveness, drain convergence.
FAIRNESS:     WF on scheduler-controlled actions (producers deliberately
              unfair — justified); SF(TryPop) documented; e10 resolver WF
              and e9-wake-handle SF are labeled boundary assumptions.
BOUNDS:       2 workers; per-suite finite domains.
NEGATIVE:     DrainParks/MixedSource/NoBridge; NegNoSignal/NegNoRecheck
              (exact historical mutants); NegOldEscape/NegNaiveEscape;
              neg_a/neg_b trace rejects.
C++ BRIDGE:   issue115/issue161 deterministic regressions (pre-fix FAIL /
              post-fix PASS); trace corpus with prehistory-pinned fixtures.
A:            MODEL_COVERS_PROPERTY (steady-state; deepest repair history).
B:            STRONG steady-state (trace conformance); #223 startup window:
              MODEL_SCOPE_EXCLUDED — NOW DELIBERATE AND DOCUMENTED
              (boundary declared at S1A; see the #223 adjudication above).
CHANGE:       boundary documentation (3 places) + S2 candidate registration.
RESIDUAL:     backend split-phase epoch protocol (ReadyWaitSource/
              UringWaitSource snapshot→park closure) has no direct model
              (S0B handoff; C++ causal regressions #115/#116/#161 carry it);
              E5-A2 split-config observation return has no model action
              (trace-doc boundary #1, recorded follow-up); park-commit vs
              wake-epoch weak-memory extraction remains an unextracted V3
              candidate (pay only alongside a scheduler-domain model change).
```

### F09 resource / accounting retirement (B09 — ALIGNED)

```text
CONTRACT:     all resources explicitly bounded; capacity pressure returns a
              reportable result (would_block) before acceptance; accounting
              retires exactly once (two distinct counters: slot_in_use,
              accepted_outstanding).
C++ OWNER:    RequestArena accounting (P1-05 two-counter law);
              BlockingIoPool bounds; e12-semaphore conservation; f1 P1-2.
MODEL:        request-arena (InvAccounting + Destroy quiescence);
              blocking-io-pool (bounds + drain negatives); e12-semaphore
              (P1–P10); f1-wait-record (live accounting).
NEGATIVE:     BuggyUnboundedDequeue, BuggyShutdownDiscardsQueued;
              NEG-SEM-1..7.
C++ BRIDGE:   capacity rows (rejects_full, accepts_exact_limit,
              rejection_never_completes, recycles_after_reset);
              semaphore tests.
A:            PARTIALLY_MODELED — multi-slot free-list/accounting
              interference unmodeled (coverage_gaps; deliberate).
B:            STRONG (SE-2 H11: TLA+ DETECTS). CHANGE: none.
RESIDUAL:     S0B handoff "arena counter invariants unmodeled beyond the
              lifecycle suite scope" stands as the multi-slot note above.
```

### F10 shutdown / quiescence (B10 — ALIGNED)

```text
CONTRACT:     destruction requires quiescence; destructor never implicitly
              drains/cancels/publishes; accepted/bound requests at destroy =
              Debug/Release fail-fast.
C++ OWNER:    RequestArena Destroy guards (leaf); AsyncIoContext/
              ApplicationRuntime lifecycle; BlockingIoPool drain; uring
              poison quiescence.
MODEL:        e16-application-runtime (start/stop/drain/close ownership +
              NEG-E16-1..6); request-arena (Destroy + InvDestroyQuiescent);
              blocking-io-pool; d1-uring-poison (control-reference
              quiescence); e12-queue (close/drain B3/B6 + R scenes);
              e12-rwlock-scheduler-liveness (termination convergence).
NEGATIVE:     NEG-E16-2/3/4/5/6; BuggyShutdownDiscardsQueued.
C++ BRIDGE:   application_runtime tests; BlockingIoPoolTest; queue tests;
              #262-family drain-stall teardown test.
A:            PARTIALLY_MODELED — per-domain lifecycles covered; no single
              cross-domain composition model (context+backend+scheduler
              shutdown as one protocol). Adjudicated: e16's ReapTaskIO
              worker-scheduling dependency is documented unmodeled; a
              composition model needs a driving defect or a frozen
              cross-domain contract change.
B:            STRONG per domain (SE-2 H10 migration M3 with documented
              conventional-basis caveat). CHANGE: none.
RESIDUAL:     ThreadPool drain/destruction interleaving with in-flight
              workers relies on executable evidence (S0B handoff, stands);
              abort/cancel-on-shutdown patterns remain unapproved semantics
              (AGENTS §3.7).
```

## Memory-model boundary (§8 layering, frozen)

| Face | TLA SC adequacy | GenMC status |
| --- | --- | --- |
| Completion publication/reset | protocol-order face only | **COVERED** — K1/K2 + 5 rejected controls (#197) |
| CancelToken word | **ADEQUATE** — single-word protocol, thread-confined per-consumer state | NOT REQUIRED (argument-based evaluation #294; reopen triggers in F06/B: cross-thread CancelState, multi-word/multi-atomic token, cross-atomic ordering dependence, ABA-sensitive epoch/reuse) |
| RequestSlot generation/reuse | **ADEQUATE** — pure-mutex leaf, zero atomics | NOT REQUIRED |
| Park-commit vs wake epoch | protocol face covered by e9; memory-order face unextracted | CANDIDATE (unextracted; pay only with a scheduler-domain model change) |
| Worker `active` topology publication | not representable (protocol/topology-level) | Not the right tool; owned by the #223 boundary + S2 candidate |

## Unmodeled / not-suitable (explicit residuals)

- Multi-slot ready-ring ORDER, multi-slot accounting interference, borrow
  interleavings, submit-vs-close protocol face — recorded in
  `spec/tla/manifest.json` `coverage_gaps` with triggers (deliberate,
  executable-evidence scope).
- Uring control-CQE/deferred-terminal interplay — single-driver abstraction
  boundary (documented; F06 residual).
- Backend split-phase epoch protocol (F08 residual) — executable-evidence.
- ThreadPool drain/destruction interleaving (F10 residual) —
  executable-evidence.
- e12-event set-vs-due admission — ON-TOUCH debt.
- NOT_SUITABLE_FOR_TLA: caller-side buffer lifetime discipline (type-system
  face), `no_space` construction-failure vocabulary, CancelState
  cross-thread introspection boundary, weak-memory order itself (V3's job).

## Stronger-proof candidates for S2 (ranked by S1A evidence)

```text
1. STARTUP→STEADY-STATE POPULATION REFINEMENT (#223 Option B)
   property:    transferable-election safety / participant uniqueness /
                no stranded backend progress ACROSS startup skew
   C++ owner:   scheduler.cpp run_impl spawn/active publication + R2 election
   why TLA/GenMC insufficient: E9 declares the boundary (Option A); the
                excluded interleavings are unreachable in-model by
                construction; a StartWorker extension is a genuinely new
                protocol model (optionally + refinement statement), and the
                wake-epoch weak-memory face sits adjacent
   bug relevance: #210 role-swap (test-prehistory artifact — real bug class:
                conformance tests assuming model prehistory)
   candidate families: TLA+ extension (first), mechanized small-scope ADT
                refinement only if TLA under-specifies; GenMC kernel for the
                epoch publication IF extracted state grows
   correspondence required: StartWorker ↔ per-thread active publication;
                election LowestAlive ↔ R2 active-scan
   proof boundary: abstract-protocol only; no whole-scheduler claim

2. CANCELED-TERMINAL RECORDING-SOURCE LAW (strengthened by #294)
   property:    every canceled terminal was recorded through one of exactly
                three source classes (Scheme-B cancel win / confirmed
                interruption / verbatim kernel result — the stamp is the
                recording path, not causal attribution); intent-only
                running cancel never wins
   C++ owner:   RequestArena record_terminal/record_canceled/cancel
   why insufficient: modeled + witnessed, but the Decision-11 clause (b) is
                an environment contract — no model can prove a future
                backend honors it
   candidate families: typestate/capability-typing of record_canceled at
                the C++ level (make the obligation unexpressible) rather
                than a theorem prover; GenMC kernel NOT indicated (F06
                evaluation)
   correspondence required: a capability-typed record_canceled would
                eliminate NEG-RA-6's defect class by construction
   proof boundary: UNREPRESENTABLE-class migration, S1-layer evidence

3. SUBMIT TRANSACTION ROLLBACK CONSERVATION
   property:    failed submit conserves all ownership (zero residue) across
                every failure injection point
   C++ owner:   detail::submit_transaction.hpp ladder
   why insufficient: protocol face unmodeled (F02); the ladder's stage
                lattice is small and regular — a focused TLA model is the
                natural next step IF a defect or contract change ever fires
                the trigger; not worth building today
   candidate families: TLA+ first; separation-logic only if aliasing across
                the arena/completion halves becomes the obstacle
   proof boundary: leaf+ladder protocol, not backend kernels

4. NOT WORTH FORMALIZATION (explicitly): select restoration breadth
   (already 29-mutant covered), e12 primitive resolve-CAS (inherited
   five-fold), durability vocabulary (failure-model doc owns it).
```

## Verification record (actual commands, actual results)

```text
python3 scripts/formal/verify.py check   → PASS (post-manifest-edit)
python3 scripts/formal/verify.py doctor  → PASS
bash scripts/formal/verify-request-arena.sh
  → 16/16 gates: PASS correct lifecycle (full Inv, post-repair state space);
    PASS liveness; CEX NEG-RA-1..6 (all exact, incl. NEG-RA-6 post-law-update);
    PASS wrong-prop ×2; CEX W1..W6 (W6 = new #262 non-vacuity witness)
xmake f -m debug --toolchain=clang -y && xmake build
  reference_backend_arena_lifecycle_test && xmake run ...
  → build ok; ALL TESTS PASSED (incl. the new
    fake_kernel_canceled_completion_verbatim_without_caller_cancel)
Docs/mechanical gates for the doc changes: run with the pre-push gate
  (see the PR's checklist for the as-run record).
SKIPPED: full 26-suite TLC sweep (smoke tier) — change-class rule: only the
  request-arena suite's transition relation changed; e9/other suites are
  doc-note-only this phase. The pre-push formal tier covers the structural
  gates; per-suite runs for untouched suites are not evidence for this diff.
```

## Claim boundary

- **About the abstract models:** request-arena now represents and witnesses
  the kernel-canceled verbatim shape (W6: existentially, the no-caller-intent
  execution) and asserts the contract's three-class canceled-terminal
  recording-source admissibility law under its stated bounds (single slot,
  MaxGen 2, Layer-B WF obligations, no deadlock-freedom claim; the
  `kernel_canceled` stamp identifies the recording path, not cancellation
  cause or caller-intent absence). E9's refinement
  boundary now explicitly excludes worker-startup skew, with the prehistory
  obligation on every conformance/trace test.
- **About C++ correspondence:** every strengthened property has an
  executable bridge (named tests above); the Completion memory-order layer
  remains the #197 bounded-kernel claim; the cancel-token weak-memory face
  is argued adequate, not kernel-checked (the argument is recorded).
- **Unproved:** the S0B residual set (multi-slot scope, uring control-CQE
  interplay, split-phase epoch, ThreadPool drain interleaving, e12-event
  admission precedence), startup-skew safety itself (S2 candidate), and the
  #262 root-cause/attribution question (real-incident track). TLA+/GenMC
  model success does not prove the C++ implementation; it proves the model
  under its assumptions, with the correspondence carried by the named
  bridges.

## STOP

No Coq/Lean/Iris mechanization in this phase's PR. The candidate set above
is the S2 handoff; tool choice happens there, by property/tool fit.
