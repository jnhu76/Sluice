# F1 Requirement Evidence Matrix (#402 Phase F1)

- **Status**: `F1_D_REQUIREMENT_MATRIX_COMPLETE` candidate. This matrix maps
  root requirements to observations, clean-room external probes, internal
  deterministic evidence, fault seams, mutants and formal assets **as of the
  F1 baseline**. It claims no new conformance beyond what the cited evidence
  already establishes; where a row's internal evidence is a ledger-recorded
  correspondence from an earlier slice, that is stated per row and F1 did not
  re-derive it.
- **Baseline**: `PRODUCTION_BASELINE_SHA = a34d96c64f5ac8647b7d0f57c5511e68301b0f0e`
  (merge-base with origin/master == F0 adopted SHA; production diff empty);
  external probe evidence produced by `scripts/verify_f1_package.py` on this
  branch (both profiles from the final verifier state, clean verification
  head; manifests + archive/object/symbol baselines:
  `f1-package-manifest-{noliburing,liburing}.json`,
  `f1-archive-baseline-{noliburing,liburing}.json`).
- **Layer vocabulary**: A = clean-room installed-prefix consumer;
  B = internal deterministic test/fault seam; C = formal model (TLA+);
  D = real kernel/backend.

`CURRENT_STATUS` values: PASS (evidence recorded, both layers where claimed) ·
KNOWN_GAP (external observation weaker than the contract; the discriminating
evidence lives in B/C/D) · NOT_APPLICABLE_WITH_REASON (no external observation
is possible or meaningful).

## 1. VERIFY-04 mandatory counterexample suite (V01–V27)

| Case | External clean-room probe (Layer A) | Internal / fault seam (Layer B) | Formal (Layer C) / real-kernel (Layer D) | CURRENT_STATUS |
|---|---|---|---|---|
| V01 closed+zero | `contract_admission` (bad fd → `invalid_state`, `outstanding()==0`; also with a zero buffer, precedence before the no-op short circuit) | `semantic_reference_case_test` (named V01 direct+request rows; A2/B1 ledger VERIFIED) | RequestCore.tla (B1) | PASS |
| V02 illegal access+zero | `contract_admission` (read-only write → `invalid_argument`, including the zero-buffer form) | `semantic_reference_case_test` (named V02 rows) | RequestCore.tla | PASS |
| V03 zero request vs full table | `contract_admission` (request no-op occupies a slot; full table rejects `would_block` while the same direct op succeeds) | `semantic_reference_case_test` (named V03 rows) | RequestCore.tla | PASS |
| V04 acceptance × admission-close race | NOT_APPLICABLE_WITH_REASON — needs a deterministic admission/acceptance interleaving; not manufacturable externally without seams or sleeps | B1 slice seam evidence (ledger B1-A..D; acceptance linearization under serialized authority) | RequestCore.tla (admission-close actions) | PASS (B/C) · KNOWN_GAP (A) |
| V05 dispatch failure after acceptance | NOT_APPLICABLE_WITH_REASON — requires post-accept dispatch fault injection | `ThreadPoolBackend::DispatchFailureInjection` seam (ledger B1-B/C records) | RequestCore.tla | PASS (B/C) · NOT_APPLICABLE (A) |
| V06 terminal vs observer attach | `contract_observer` (already-terminal arm and armed→delivery arm, atomic resolution asserted) | `runtime_waiter_observer_test`, `uring_registration_lifecycle_test` (C1 ledger VERIFIED) | ObserverCore.tla (C1-H) | PASS |
| V07 cancel vs queued/running delivery | `contract_observer` (cancel-before-publication; retired or delivery_in_progress, re-cancel acquires retirement) | same C1 records; staged-delivery cancel regression (C2-D #441) | ObserverCore.tla | PASS |
| V08 binding release while delivery remains | `contract_observer` (result consumed while delivery bookkeeping retires; no buffer access after publication) | `public_request_test`, C1 records | ObserverCore.tla | PASS |
| V09 slot reuse; stale ID | `contract_request_lifetime` (stale lookup/cancel → empty/not_found; fresh submissions unaffected) | `request_core_ownership_test` (B1) | RequestCore.tla | PASS |
| V10 completion between empty poll and sleep | `contract_progress` (drain to quiescence, wake by new submission through epoll) | `threadpool_progress_race_test`, `uring_progress_race_test` (C2 ledger VERIFIED) | ProgressSource.tla (C2-E) | PASS |
| V11 budget expiry after readiness drain | `contract_progress`/`w03` (bounded passes converge; dispatch-retry obligation honored by the public pass report) | same C2 records | ProgressSource.tla | PASS |
| V12 submit wakes sleeping owner | `contract_progress` + `w03_external_loop` (cross-thread submit wakes the parked owner via the notification fd) | C2-B no-lost-wake half (ledger) | ProgressSource.tla | PASS |
| V13 partial io_uring submit / cancel CQE | NOT_APPLICABLE_WITH_REASON — uring transport fault injection | `uring_submit_boundary_test` (fault seams; E1 record incl. real-kernel matrix) | BACKEND-03 evidence (E1 real-kernel, D) | PASS (B/D) · NOT_APPLICABLE (A) |
| V14 cancel vs successful short write | `contract_cancel_effect` (disposition any of 5; success preserves count + effect report; no fabricated rollback) | `semantic_effect_outcome_test`; E1 cancel refinement | RequestCore.tla (cancel actions) | PASS |
| V15 unknown-effect failure | NOT_APPLICABLE_WITH_REASON — needs a fault-injected write with untrustworthy count | `semantic_reference_case_test` (named V15 rows; fault seam) | ERR-02 reference rules (A2) | PASS (B) · NOT_APPLICABLE (A) |
| V16 write-then-sync ordering | `contract_cancel_effect` (sync after completed write covers confirmed bytes) — the *negative* coverage claim (no guarantee for the outstanding write) is not externally falsifiable | `semantic_reference_case_test` (named V16 rows), `semantic_durability_reference_test` (reference byte model; A2 ledger) | SEM-06 tables | KNOWN_GAP (A) by design · PASS (B) |
| V17 observed write/resize + conflicting mutation | NOT_APPLICABLE_WITH_REASON — needs a concurrent external mutator with deterministic observation; supersession discrimination is reference-model evidence | `semantic_reference_case_test` (named V17 rows), durability reference model | SEM-06 tables | PASS (B) · NOT_APPLICABLE (A) |
| V18 second pipeline submission fails | `w02_pipeline` (capacity 1: second submit rejected; settlement of the first on the early-return path is observed through the absence of the always-on nonterminal-release fail-fast plus the healthy post-unwind shutdown — no direct settlement observation) | `request_scope_test` (D1 VERIFIED) | — | PASS |
| V19 wait timeout with I/O running | KNOWN_GAP (A) — the external probe asserts scope cleanup paths but not a timed mid-I/O deadline return; timing-shaped discrimination is internal | `request_scope_test` timeout rows (D1) | — | PASS (B) · KNOWN_GAP (A) |
| V20 shutdown with ready unconsumed results | `contract_shutdown` (consume after execution close; no deadlock; idempotent re-shutdown) | `shutdown_lifecycle_test` (E2 VERIFIED; 9 TP / 17 uring scenarios) | ShutdownCore.tla (171-run gate PASS) | PASS |
| V21 destruction with live binding | NOT_APPLICABLE_WITH_REASON — always-on fail-fast is process-fatal; not catchable in-process | `public_request_release_violation_test`, `shutdown_lifecycle_test` destructor preconditions (E2) | ShutdownCore.tla | PASS (B) · NOT_APPLICABLE (A) |
| V22 backend poison with borrowable buffers | NOT_APPLICABLE_WITH_REASON — poison requires backend fault injection | `shutdown_lifecycle_test` (V22 rows; E2) | ShutdownCore.tla + BACKEND-03 evidence | PASS (B) · NOT_APPLICABLE (A) |
| V23 notification fd detach/reuse | `contract_progress` (interest retired → re-borrow → new wake; old registration deleted first) — the stale-wake-toward-destroyed-context discriminator is stronger internally | `shutdown_lifecycle_test` (V23 rows; mutant M11 discriminator, E2) | ShutdownCore.tla | PASS (B) · KNOWN_GAP (A) honestly recorded |
| V24 generation/token exhaustion boundary | NOT_APPLICABLE_WITH_REASON — exhaustion closes admission before wrap; not reachable without driving the counter domain | B1 identity-domain records; C2-E reachability adjudication | RequestCore.tla / ProgressSource.tla (V24 boundary finding closed) | PASS (B/C) · NOT_APPLICABLE (A) |
| V25 core-only standalone consumer | `w01_direct` clean-room (installed prefix; async types incomplete; `-lsluice_core` only) + negative probe (async symbols not in core archive) | `direct_w01_consumer_probe`, `request_core_consumer_probe` (A2 VERIFIED) | — | PASS |
| V26 released binding; late control pin retires | `contract_request_lifetime` (release → progress pass → capacity returns) + `contract_shutdown` | `shutdown_lifecycle_test` (V26 rows) | ShutdownCore.tla (LiveOpsSettle) | PASS |
| V27 resize coverage on sync | `w01_direct` (direct grow/shrink + sync) and `contract_durability` (direct resize → request-path `sync_data`/`sync_all`; independent reader observes) | `semantic_reference_case_test` (8 named V27 rows) | SEM-06 (v1-r3 amendment; A2 rows) | PASS |

## 2. VERIFY-02 safety obligations (S1–S10)

| ID | External probe correspondence | Primary evidence layer | CURRENT_STATUS |
|---|---|---|---|
| S1 rejected submissions retain no borrow/no effect | `contract_admission` (`outstanding()==0` after rejections; no retained borrow observable) | A + B (B1) | PASS |
| S2 at most one immutable terminal | `contract_request_lifetime` (consume-once; take empties) | B/C (RequestCore.tla) | PASS |
| S3 stale IDs cannot affect reused slots | `contract_request_lifetime` (stale cancel/lookup) | B/C | PASS |
| S4 publication visible only after handoff/retirement | ready-before-reuse observable in `w02`/`w03`/`contract_progress`; full handoff discrimination is B (TSan/review) + C | B/C primary, A subset | PASS |
| S5 caller reuse follows acquired terminal | `w02`/`w03` reuse buffers only after ready/reap | B/C primary, A subset | PASS |
| S6 reclaim predicate | capacity-return probes (`w02` retained-results, `contract_request_lifetime` resubmit) | A subset + B/C | PASS |
| S7 observer failure cannot orphan accepted work | `contract_observer` (cancel/retire paths leave the operation settling) | C1-G (B) + ObserverCore.tla | PASS |
| S8 shutdown cannot destroy referenced owners | ordered teardown in `w03` (unregister → detach → shutdown) and `contract_shutdown` (shutdown refused while the notification interest is live, then completed after the detach) | E2 (B) + ShutdownCore.tla | PASS |
| S9 no sleep past unadvertised obligation | `contract_progress`/`w03` full no-lost-wake loop (arm → ack → poll → wait) | A + C2 (B) + ProgressSource.tla | PASS |
| S10 durability meets SEM-06, no invented snapshot | `contract_durability` + `w01` | A + A2 reference model (B) | PASS |

## 3. VERIFY-03 conditional liveness (L1–L5)

| ID | Model assumption mapping | External/internal observation | CURRENT_STATUS |
|---|---|---|---|
| L1 accepted request eventually publishes | RequestCore.tla (B1; fairness = driver services enabled work) | repeated clean-room runs; `contract_progress`/`w03` converge | PASS (C/B) · A runs are repeat evidence, not proof |
| L2 owner eventually observes actionable event | ProgressSource.tla (C2-E re-closure) | epoll wake probes (`contract_progress`, `w03`) | PASS (C/B/A) |
| L3 registration delivers/retires | ObserverCore.tla (C1-H) | `contract_observer` retirement loops (bounded, never spins on contract) | PASS (C/B/A) |
| L4 execution shutdown converges | ShutdownCore.tla (LiveShutdownCloses; E2 171-run gate) | `contract_shutdown`/`w03` converge under finite accepted sets | PASS (C/B/A) |
| L5 released slot eventually reclaims | ShutdownCore/RequestCore reclaim obligations | capacity-return probes (`w02`, `contract_request_lifetime`) | PASS (C/B/A subset) |

## 4. Family coverage beyond the V-suite (phase-brief §17)

| Family | External probe | Internal evidence anchor (ledger row) | Notes |
|---|---|---|---|
| Result/binding/identity (V08/09/20/21/24/26, HANDLE, REQ) | `contract_request_lifetime`, `contract_shutdown` | B2/HANDLE row (RECORD→pending human review), B1 rows | `RequestHandle` spelling stays UNDECIDED/F2-owned; the probe does not touch it |
| Admission/validation/boundedness (V01–V05, V18) | `contract_admission`, `w02_pipeline` | A1/A2/B1 rows | accept×close race (V04) is Layer B (above) |
| Cancel/effects (V13/V14/V15, CANCEL, ERR) | `contract_cancel_effect` | E1 record; fault seams | V13/V15 externally not manufacturable (recorded) |
| Observer (V06–V08, OBS) | `contract_observer` | C1-A..I rows; ObserverCore.tla | delivery vehicle is the current Completion-based public spelling (P-05) |
| Progress (V10–V12, V23, PROG) | `contract_progress`, `w03_external_loop` | C2-A..E rows; ProgressSource.tla | includes the PROG-03 edge-trigger drain rule via bounded passes |
| Shutdown (V20/V21/V22/V26, SHUT) | `contract_shutdown`, `w03` ordered teardown | E2 record; ShutdownCore.tla | E_REQUIRED_CLOSURE_ADOPTED = PASS upstream |
| Metadata/durability (V16/V17/V27, SEM-06/07) | `contract_durability`, `w01_direct` | A2 rows; durability reference model | coverage-vs-snapshot distinction is model evidence |
| Backend availability (PROD-02, BACKEND-02) | `contract_backend_availability`, `w03` uring variant | E1 real-kernel record; D2 host capability check | no-liburing: installed shell is abstract (compile-time unavailability); liburing: construction succeeds or fails explicitly; no silent fallback exists to select |
| Workloads W-01..W-04 | `w01_direct`, `w02_pipeline`, `w03_external_loop`, `w04_stackful_candidate` | A2/D1/E2/D2 ledger rows | W-04 recorded as OPTIONAL_CANDIDATE evidence only (no support claim, ADR-0003 PROPOSED) |
| Root-DEFERRED runtime family + legacy stream world (H-06..H-18, H-22, H-26, H-29, L-01..L-09) | none by design — manifest header-set diff is the detector | F0 policy §3 consumer register | these surfaces carry no root-visible workload observation (PROD-02 DEFERRED / MIG-02 compat); class removal with headers retained is invisible to F1 gates by design and rides the F2/F3/F4 consumer audits |

## 5. Known gaps and honest limits of the F1 baseline

1. Layer-A probes cannot deterministically manufacture: admission/acceptance
   races (V04), post-accept dispatch failure (V05), unknown-effect faults
   (V15), durability supersession (V17), poison (V22), transport faults (V13),
   generation exhaustion (V24), live-binding destruction fail-fast (V21). All
   of these keep their Layer B/C/D evidence; F1 records the correspondence and
   explicitly does not weaken any internal assertion to make an external probe
   possible (phase-brief §18).
2. V23's external arm exercises re-borrow after ordered detach; the
   stale-wake-toward-destroyed-context discriminator remains the internal M11
   oracle.
3. W-04 evidence is OPTIONAL_CANDIDATE-only: the W-04 tracer and the H5
   exception arm are solid external evidence in both profiles. The
   suspension-shaped arms are structurally ordered and uring-profile-only
   (measured: the threadpool backend preads, so non-seekable fds fail
   immediately and no external suspension exists without liburing): the H1
   arm's failure lands strictly between the sibling's proven suspension and
   its writer-gated real EOF; the H2 arm issues `request_stop` only after the
   suspension is published and gates the writer close on the stop call
   returning, so the task's real EOF terminal (never `canceled`) cannot be
   reached before the stop request existed; the H4 `_for` arm's assertions
   hold under either interleaving of park-window expiry and EOF, so no
   ordering claim rests on timing — the fine-grained expiry-vs-terminal
   interleaving and H3/H6 discrimination stay with the D2 record and
   ADR-0003 (PROPOSED). The no-liburing profile records the explicit
   measured UNAVAILABLE outcome (exit 2) for these arms.
4. The uring profile's external evidence on this machine ran against a
   user-prefix liburing 2.9 on WSL2 (version recorded in the manifest's
   `LIBURING_VERSION` field); the ledger's real-kernel E1/E2 records remain
   the D-layer backend evidence.
5. The E1 threadpool CI stall (recurring finding, #454 §4.2) did not reproduce
   during F1's verification runs (adversarial round and review-round reruns
   included); the standing rerun protocol applies and no F1 assertion was
   weakened for it.
