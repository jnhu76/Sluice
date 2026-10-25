# Correctness review

This document reviews the semantic legality of every elision arm. It is an
audit of archived receipts and obligations, not a fresh execution: no test was
rebuilt or re-run for this PR (see §6 for the exact boundary).

## 1. Review basis

| Source | Use |
|---|---|
| v2 root requirement IDs (REQ-01…06, HANDLE-01, OBS-01…03, PROG-01…04, THREAD-01/02, CANCEL-01, BACKEND-03/04, SHUT-01/02, LIFE-01, SEM-03) | obligation list |
| `docs/explicit-io-contract.md` at `bdfd0d8d` | normative text |
| `evidence/*/base-*.log`, `restored-green-*.log`, `mut-*.log` | receipts |
| `evidence/ab/*/oracle-gate.txt` | per-run workload oracle |
| `evidence/*/mutation-receipts-verified.txt`, `mutants/status.txt` | receipt summaries |
| lane mapping documents | mechanism/authority analysis |

## 2. Obligation coverage

Every arm was checked against the full obligation list. "Preserved" means the
arm does not remove the mechanism; "gated" means the mechanism is skipped only
under a proven precondition.

| Obligation | K1(a+b) | K2-a | K3-a | K4-b | Combo |
|---|---|---|---|---|---|
| Exactly-once terminal (REQ-03, HANDLE-01) | preserved (`offer_terminal` untouched) | preserved | preserved | preserved | preserved |
| Borrow lifetime / retirement (LIFE-01) | preserved | preserved | preserved | preserved | preserved |
| Admission/accept/rollback (REQ-02) | preserved | preserved | preserved | preserved | preserved |
| Delivery-init before accept (cancel interleaving) | preserved | preserved | preserved | preserved | preserved |
| Late control CQE routability | preserved | preserved | preserved | preserved | preserved |
| Publication boundary (REQ-04) | preserved; only the drain lock shape changes | preserved | preserved | preserved | preserved |
| Armed observer state machine (OBS-02/03) | **gated** on `queue_armed_observer`; armed path unchanged; sink call **not** elided | n/a | n/a | n/a | gated |
| Cancellation / control (CANCEL-01) | preserved | preserved | preserved | preserved | preserved |
| Kernel/ledger accounting, partial submit (BACKEND-03) | preserved | preserved; reconciled when deferred | preserved | preserved; residue advertised | preserved |
| Bounded pass (PROG-01) | entry snapshot (explicitly permitted) | preserved | preserved (guard needed) | preserved | preserved |
| No lost wake (PROG-02) | preserved | residue signal retained; mutation-tested | preserved | residue signal retained; mutation-tested | preserved |
| Honest residue | preserved | `dispatch_retry_remains` extended to deferred SQEs | preserved | preserved | preserved |
| Shutdown quiescence (SHUT) | preserved; destructor witnesses unchanged | preserved; flush at pass | preserved | preserved | preserved |
| Poison / Class-A recovery | preserved | preserved; mutation-tested | preserved | preserved; mutation-tested | preserved |
| Bounded memory/capacity | preserved; preallocated batch | preserved | preserved | preserved | preserved |

## 3. Mutation receipts (BASE GREEN → MUTANT RED → RESTORED GREEN)

**Formal three-state receipts exist for 11 mutant builds**, not 12 or 20:
5 K2-a mutants + 6 K4-b mutants (the two sets share 5 failure classes; K4-b adds
the retry-residue oracle). K3 has one formal guarded-mutant receipt (killed by
two oracles) plus two kills of an unguarded draft for which **no mutant build
and no restored-green log exists** — the guard itself was the fix. **K1 has no
dedicated mutant binary**; its only kill is an over-elided build caught by two
tests, after which the corrected build passes 9 tests with no formal
restored-green mutant log. The earlier "twelve distinct" count included the
draft kills and is retracted.

### K4-b (topology+depth-gated deferral)

| Class | Mutant / build | Oracle | Result |
|---|---|---|---|
| missed wake | `SLUICE_K4_DEFER_MUTANT_RESIDUE_SIGNAL_DROPPED` | `k4_residue_wake_test` | rc=1, `FAIL: deferred SQE was not flushed promptly after the peer submission`; restored rc=0 |
| missing retry | retry-unadvertised build | `k4_retry_residue_wake_test` | rc=1, `FAIL: the residue did not wake the owner to retry the flush promptly`; restored rc=0 |
| false eligibility | `SLUICE_K2_E18_MUTANT_DEFER_PAST_POISON` | `k2_e18_poison_eligibility_test` | rc=134, `FAIL: a reservation accepted during the poison window never reached a terminal` + nonterminal contract fail-fast; restored rc=0 |
| missing retry / lost residue | `SLUICE_URING_PROGRESS_MUTANT_TRANSPORT_RETRY_UNADVERTISED` | `uring_progress_race_test` | rc=1 (fails at `u2_k15_retryable_submit_failure_does_not_strand_owner`); restored rc=0, 23/23 |
| batch/ledger accounting | `SLUICE_BACKEND_CONTRACT_MUTANT_SUBMIT_BATCH_INVISIBLE` | `backend_contract_conformance_uring_test` | rc=1, 5 failures (per-entry kernel visibility, wrapped-head cardinality); restored rc=0, 20/20 |
| batch/ledger accounting | `SLUICE_BACKEND_CONTRACT_MUTANT_MASKED_SQ_CARDINALITY` | `backend_contract_conformance_uring_test` | rc=134, `FAIL [full_ring_zero_consumed_hard_error_settles_every_entry]` + contract fail-fast; restored rc=0 |

The combo build re-ran all six mutants: all still fail red, which is the
positive evidence that the K1 merge did not silently disarm the K4 oracles
(`ab/combo-…/mutants/status.txt`).

**Oracle limitations (added after independent review).**

- `k4_residue_wake_test` does **not** deterministically prove that the residue
  signal is the only wake source. It arms the peer op after
  `sleep_for(100 ms)` (`bench/research/key-path-593/patches/k4b-final.diff:520`)
  rather than waiting on a prepark counter, then asserts `sq_ready == 1`
  (`:528`). The owner may not yet be parked (it could flush during a pass) or
  could already have flushed, so `flushed_promptly` (`:539-546`) and `woken`
  (`:560-561`, which only requires `WaitKind::progress`) can pass without the
  signal being the wake cause. The mutant is expected to be RED, but the oracle
  is racy; it is evidence, not a proof. The retry test does use a prepark
  counter (`:728-742`) and is deterministic.
- `k4_retry_residue_wake_test` exercises an **inline-flush** retryable failure:
  the comment at `k4b-final.diff:744-746` states that the submitting thread has
  never driven progress, so the third dispatch keeps the inline flush. It does
  not exercise a deferred **pass-start** flush with `ledger.size() > 1` that
  returns EAGAIN/partial. That deferral-shaped retry has no test.
- `uring_progress_race_test::u2_k15` covers a generic retryable submit failure,
  but not the K4-b gate shape and not multiple ledger entries.

### K2-a (ungated deferral)

| Class | Mutant | Oracle | Result |
|---|---|---|---|
| missed wake | `SLUICE_K2_E18_MUTANT_RESIDUE_SIGNAL_DROPPED` | `k2_e18_residue_wake_test` | rc=1, `FAIL: deferred SQE was not flushed promptly`; restored rc=0 |
| false eligibility | `SLUICE_K2_E18_MUTANT_DEFER_PAST_POISON` | `k2_e18_poison_eligibility_test` | rc=134; restored rc=0 |
| missing retry | `SLUICE_URING_PROGRESS_MUTANT_TRANSPORT_RETRY_UNADVERTISED` | `uring_progress_race_test` | rc=1, `FAIL: poison_transition_wakes_parked_owner`; restored rc=0, 23/23 |
| batch/ledger | `SUBMIT_BATCH_INVISIBLE` | `backend_contract_conformance_uring_test` | rc=1, 5 failures; restored rc=0 |
| batch/ledger | `MASKED_SQ_CARDINALITY` | same | rc=134; restored rc=0 |

Note: the same retry-unadvertised mutant surfaces at a different test in the
K4-b build (`u2_k15` instead of `poison_transition_wakes_parked_owner`). Both
are genuine RED receipts; the difference is a consequence of the gate changing
which operation is in flight when the mutant fires. It is recorded rather than
smoothed over.

### K3-a (pass-tail fusion)

| Class | Mutant | Oracle | Result |
|---|---|---|---|
| lost pass / stale summary | unguarded first draft (no version) | `a_zero_op_after_sweep_is_immediate_residue` | RED: `FAIL: a_zero_op_after_sweep_is_immediate_residue` |
| lost pass / stale summary | unguarded first draft | `zero_op_from_another_thread_during_pass_is_served` | RED |
| lost pass (guarded build) | `SLUICE_K3A_MUTANT_TAIL_STALE_IGNORES_VERSION` | `uring_owed_residue_test` and `uring_external_loop_test` | both RED; restored GREEN |

This is a case where the oracles rejected a *mechanism* (no guard) before the
performance arm existed. It is the strongest evidence in the campaign that the
payload oracles have teeth.

### K1 (merged elision)

| Class | Mutant / variant | Oracle | Result |
|---|---|---|---|
| over-elided sink delivery | over-elided build that also skipped the sink `on_ready` | `uring_owed_residue_test`, `uring_owed_counter_parity_test` | both RED (`sink_deliveries` is the delivery oracle); the build was corrected, not the oracle; no formal mutant/restored-green log |
| lost publication | **not mutated** | W-02 per-pass trace | `published == reaped` on 70,061 W-02 passes (W-03 aggregate only) — mechanism evidence, **not** a mutation receipt |
| `publish_batch_` / `owed_batch_` drop or double count | **not mutated** | — | no receipt |
| K1-b false negative (`observer_armed == false` while an observer is armed) | **not mutated** | — | no receipt; the existing observer tests cover the normal armed path only |
| double terminal / early reuse / stale generation | not touched by the diff, **not tested in a K1-shaped interleaving** | — | no receipt |

No dedicated K1 mutant binary exists; the lane's correctness evidence is the
suite plus the over-elision kill. This is a gap in the K1 receipt set and is
stated as such.

## 4. Base-suite evidence

| Arm | Targets green | Notes |
|---|---|---|
| K1 | 9 | `request_core_protocol` 46/46, `request_core_publication` 11/11, race 23/23, external 9/9, registration 4/4, owed-residue 3/3, owed-counter 8/8, pending-control, smoke, public probe |
| K2-a | 13 at arm start; 7 key targets re-run at the end | includes #585 flush oracle re-pinned, `k2_e18_*` |
| K3-a | 11 | **zero existing-test modifications** |
| K4-b | 14 at arm start; 7 key oracles re-run at the end | one re-pin (`u2b_accepted_dispatch_wakes_parked_owner`) |
| Combo | 16 | includes `request_core_protocol`/`request_core_publication` and both K4 wake tests |

Workload-level oracle: every paired run was validated by the frozen G0 checker
(exit code, case set, warmup/measured counts, no NOT_RUN, `max_outstanding ≤
depth`, CAL-oracle self-test) and the per-repetition 64 MiB byte oracle.

| Arm | Run logs | checker OK | CAL oracle OK | invalid | case sets |
|---|---:|---:|---:|---:|---:|
| K2-a A/B | 390 | 390 | 390 | 0 | 13 |
| K4-b A/B | 390 | 390 | 390 | 0 | 13 |
| Combo | **210 total** (60 of them the increment comparison) | 210 (60/60) | 210 | 0 | 5 baseline + 2 increment |

## 5. Residual semantic risks

| Risk | Applies to | Assessment |
|---|---|---|
| Gate predicate is a derived approximation ("two most recent distinct drivers") | K4-b, combo | Both flush choices are semantically complete; a mixed-use submitter can defer when a strict same-thread test would not. **Performance** risk, not correctness. Untested on a mixed-driver load |
| `outstanding_summary()` is non-authoritative | K4-b, combo | Used only as a performance gate; a stale/optimistic read cannot break a flush (the pass-start flush always runs, residue is advertised) |
| Deferral under cancel/control load | K2-a, K4-b | All census arms idle on cancel (`control_cqe = 0`). **Correction:** `issue_running_cancel_locked_` only prepares and ledgers the cancel SQE (`uring_transport.cpp:620-693`); it does not flush on the normal path. The cancel SQE is flushed by the pass-start flush (or the SQ-full branch at `:647`), so a deferred form adds one pass of cancel latency. No load-level cancel storm is covered |
| Deferred partial submit / overflow / shutdown | K2-a, K4-b, combo | No oracle exercises `ledger.size() > 1` with rc<pending partial submit, overflow flush, or shutdown in the deferred shape. The obligation mapping in §2 is **not** a deferral-shaped receipt |
| Deferred pass-start retry | K4-b | `k4_retry_residue_wake_test` tests an inline-flush EAGAIN, not a deferred multi-entry pass-start flush (`k4b-final.diff:744-746`). No test covers that shape |
| Wake-source determinism | K4-b | `k4_residue_wake_test` uses a 100 ms sleep instead of a prepark counter and its `woken` check only requires `WaitKind::progress`; it cannot prove the residue signal was the only wake source (`k4b-final.diff:520,528,539-561`). Racy oracle, not a proof |
| Early reuse / stale generation in the unflushed window | K4-b, combo | The diff does not touch `claim_execution` or generation, but deferral changes kernel visibility timing. No oracle targets the accepted+claimed-but-unflushed window |
| #585 flush-accounting semantics | K2-a, K4-b, combo | Both arms re-pin the oracle for deferred semantics. A product change must redefine "flush on every transport" with Owner review; the research re-pin is not a product decision |
| K1-a O(`request_capacity`) owed scan under `dispatch_mtx_` | K1 | `k1-elision.patch:201-216` holds the lock across the full slot array when `owed_count_ != 0`; baseline took the lock per slot (`uring_backend.cpp:659-681`). `request_capacity` can be `UINT32_MAX` (`uring_backend.cpp:150-156`). All measured K1 loads had `owed_count_ == 0`, so the branch never ran. This is an unmeasured lock-scope expansion, not a proven-safe fusion |
| K1-a owed-before-publish reorder | K1 | A zero-op owed event created by a publication hook during the pass is delivered next pass instead of possibly the same pass. PROG-01 permits it; no dedicated oracle covers it |
| `observer_armed` stale field | K1 | `DeliveryRecord::observer_armed` is not cleared on slot re-init (`uring_backend.cpp:497-499`). Masked by the `event_owed` fail-fast and by reading it only when `event_owed` is set. The correct stability argument is `published` as the linearization point (`request_core.cpp:508-511`), not `event_owed` |
| K1 detail-header return shape | K1, combo | Changes `complete_publication`'s installed detail header; requires Owner review even on revival |
| Single-factor attribution | K1 | The merged arm cannot attribute its effect between K1-a and K1-b; no split arm exists |
| W-03 d4 +2.54 % | K4-b | Below the 4.79 % campaign band and inside the only measured W-03-d4 A/A [−6.14,+6.51]; **unattributed**, and no same-session W-03-d4 A/A exists. Not a material regression under the campaign rule |

## 6. Independent review disclosure

- **What this reviewer re-checked in this session:** read the archived
  `base-*` / `mut-*` / `restored-green-*` logs and confirmed the stated rc and
  failure text; read `patch-verification.txt` (the fresh-worktree `git apply` +
  byte compare was performed by the K4-b session, not repeated here); counted
  the oracle-gate lines (390/390, 390/390, **210/210 total with 60/60
  increment**); recomputed the `k`/`u` cycles/op arithmetic from the raw perf
  logs; recomputed the CPU negative-pair counts and p95 medians from the raw
  per-rep JSONL; reproduced the bootstrap CIs with seed 593; verified the
  70,061-line W-02 trace invariant and the absence of a W-03 trace; read the K1
  patch to verify the owed-scan lock hold, the owed-before-publish reorder and
  the new state; verified the cancel-flush path in the 2x source; compared the
  three census summaries row-by-row.
- **What this reviewer did not do:** rebuild or re-run any test, benchmark or
  mutation; re-derive the byte-oracle internals; inspect the disassembly of any
  variant.
- **Independence:** no separate independent reviewer signed off as an approver.
  The independent review that produced the findings in
  [measurement-review.md](measurement-review.md) §10 was a *review input* to
  this revision, not an approval; this section and the revision response were
  written by the PR author. The independent-review step of brief §14.6 is
  therefore **disclosed as not satisfied as an approval**; the Owner decision
  must not rely on it. The review's findings are dispositioned one by one in
  measurement-review §10.
- The campaign did produce two mechanical cross-checks: the K4-b patch was
  re-applied in a fresh worktree and compared file-by-file, and the combo build
  re-ran all six K4 mutants after merging K1. Neither is human review.

## 7. Verdict

| Candidate | Correctness verdict |
|---|---|
| K4-b | Obligations preserved in the mapped sense; six deferral mutants reject the failure classes; residual risks are performance/coverage, not semantic. **Not** sufficient for productization: cancel/control load, deferred partial submit/overflow/shutdown, early reuse and #585 semantics are open |
| K2-a | Same semantic status, but the ungated form has no mechanism to prevent the measured form regressions; not deployable. Correctness evidence does not make it a candidate |
| K1(a+b) | Obligations preserved; sink-delivery over-elision was caught by two oracles (positive evidence). Detail-header change requires Owner review |
| K3-a | Safe only with a new version guard; the unguarded version was correctly rejected. Withdrawn |
| Combo | Semantic compatibility demonstrated by the merged base suite and all six K4 mutants still red; one textual merge conflict resolved verbatim and archived |
| K4-a, K3-b | No arm, no correctness claim |
