# K2 — Submit → admission → dispatch

Verdict: **`ELISION_CANDIDATE_FOUND`**. The lane measured a large end-to-end
gain for the deferred-submission *mechanism* (paired class
`END_TO_END_GAIN` in the same-thread forms), but its own arm is ungated and
regresses D=1, split-owner tails and 64 KiB. The only implemented form gate
belongs to K4-b. K2-a and K4-b are **one mechanism, two gates**; the campaign
records `ELISION_PERF_SUPPORTED` once, under K4.

## 1. Question

Does a submission from `RequestScope` to a kernel SQE perform duplicated
validation, duplicated lock acquisition, duplicated state transitions, or work
derivable from existing state?

## 2. Mechanism inventory

| # | Mechanism | Location | Authority / obligation |
|---|---|---|---|
| M1 | Scope slot reserve, linear scan | `src/async/request_scope.cpp:46-55` | scope-local capacity |
| M2 | Public precondition (closed + access) | `src/async/async_io_context.cpp:24-26,250-253` | SEM-03 steps 1–2 |
| M3 | `access_mtx_` acceptance serialization | `async_io_context.cpp:255` | THREAD-01 |
| M4 | Stats (`tally_submit`, `update_max_outstanding`) | `async_io_context.cpp:228-244,260-261` | stats only; adds one core lock when enabled |
| M5 | Backend validation (zero/range/`kMaxNativeTransfer`/dst) + closed/access recheck | `src/async/uring_backend.cpp:94-137,430-437` | SEM-03 steps 3–4; the closed/access recheck duplicates M2 |
| M6 | `fatal_error_` unlocked early read | `uring_backend.cpp:436-437` | diagnostic fast path; the admission gate is `admission_open_` under the core mutex |
| M7 | `RequestCore::reserve` | `src/async/detail/request_core.cpp:165-182` | REQ-02 admission authority |
| M8 | `prepared_ops_[slot]` write | `uring_backend.cpp:479-486` | physical: read again at terminal finalize |
| M9 | `delivery_[slot]` init | `uring_backend.cpp:488-500` | must precede `accept` (see §4) |
| M10 | `RequestCore::accept` | `request_core.cpp:184-212` | REQ-02 linearization point |
| M11 | Inline drain + `io_uring_submit` | `src/async/uring_transport.cpp:43-102` | mechanism, not contract (BACKEND-04) |
| M12 | `claim_execution` | `request_core.cpp:371-382` | CANCEL-01 exactly-once execution |
| M13 | cookie/router install | `uring_transport.cpp:161-194,526-538` | CQE identity (#580 domain) |
| M14 | `TransportLedger::append` | `src/async/uring_internal.hpp:167-188` | BACKEND-03 |
| M15 | `submit_transport_locked` → `io_uring_submit` | `uring_transport.cpp:218-235` | kernel boundary |
| M16 | `account_transport_result_locked` | `uring_transport.cpp:237-279` | ledger reconciliation |
| M17 | `dispatch_retry_remains_locked_` | `uring_transport.cpp:211-216` | PROG-01 honest residue |
| M18 | `service_pending_controls_locked_` | `uring_transport.cpp:695-711` | CANCEL-01 sticky intent |
| M19 | Conditional `signal_ready_progress` | `uring_transport.cpp:98-101` | PROG-02 |

Per submission (W-02, stats off): **6 mutex acquisitions** (`access_mtx_` ×1,
core ×3, `dispatch_mtx_` ×2) + **1 `io_uring_enter` with 1 SQE** + ~7 structural
writes. With stats on, a seventh core lock (`outstanding_summary`). The count is
constant across D=1/4/16/64; the total per-op lock table is in
[mechanism-ledger.md](mechanism-ledger.md) §3–§4.

## 3. K2-A — repeated validation

| Layer | Checks | Verdict |
|---|---|---|
| Context | `fd < 0`, closed, access | public precondition |
| Backend | closed, access, zero length, range, `kMaxNativeTransfer`, `dst == nullptr` | only the first two repeat; the rest have no other execution point |
| RequestCore | `admission_open_`, capacity, generation | admission authority — not a duplicate |

The duplicated `closed`/`access` branches are pure functions over an immutable
by-value op copy, so they cannot disagree. Removing them would save two
predictable branches, <1 ns/op. **Real redundancy, no candidate.** The
counterfactual would be a shadow arm recording both verdicts and asserting zero
disagreement; it was not built because the magnitude cannot cross any threshold.

## 4. K2-B — reserve/accept/dispatch handoff

Each of the six acquisitions corresponds to a distinct authority transition or
a proven interleaving constraint:

- `access_mtx_` — THREAD-01 acceptance serialization (also the drive-entry gate).
- core `reserve` — admission (closed + slot + generation).
- core `accept` — linearization (descriptor, borrow facts, `execution_refs`,
  `outstanding_++`).
- core `claim_execution` — exactly-once execution arbitration against a
  pre-dispatch cancel; the loser rolls back `sq.sqe_tail` and removes the
  dispatch entry (`uring_transport.cpp:151-159`).
- `dispatch_mtx_` ×2 — `delivery_` init and the SQ-side drain/flush.

**The `delivery_` init cannot move after `accept`.** If it did, a concurrent
`cancel_key` could observe the accepted request, win `won_before_execution`, and
push a publication between `accept` and the init; the driver's `publish_one`
would read the *previous generation's* `delivery_[slot].kind`. The current order
is the protection, and the `event_owed` residual check
(`uring_backend.cpp:493-496`) protects the owed control reference's pin on the
slot.

`reserve`/`accept` separation is REQ-02's transaction shape (reservation ≠
acceptance, with a tested rollback path), so it cannot be fused.

The one convenience lock is the stats path: `max_outstanding` could be
maintained inside `accept`'s critical section. That would add a stats mechanism
to RequestCore; it is registered, not proposed.

## 5. K2-C — prepared operation duplication

| Encoding | Fields | Lifetime | Reader | Removable? |
|---|---|---|---|---|
| `PreparedUringOp` (48 B) | fd/buffer/len/offset/kind | submit → **terminal finalize** | dispatch prep + finalize classification | no |
| `DeliveryRecord.kind` | kind | submit → deliver | `publish_one` | no |
| `RequestDescriptor` (24 B) | op/offset/length/zero_op | accept → publication payload | `begin_publication` | no |
| `BorrowFacts` (24 B) | fd/buffer/length | accept → reclaim | **none in this tree** | root-named obligation (REQ-01/02); open question Q1, not deletable on "no consumer" grounds |
| `RouterEntry` (32 B) | cookie/handle | dispatch → route/retire | completion | no |
| `TransportLedger::Entry` (40 B) | cookie/handle/position | dispatch → consume | reconciliation | no |
| SQE (64 B) | all op facts | prep → kernel | kernel | no |

Similar fields, different lifetime partitions; no whole block is removable. The
one inline round-trip (`prepared_ops_` written then read on the same stack) must
still be written because finalize reads it.

## 6. Rejection, capacity-full and rollback

| Path | Behaviour | Location |
|---|---|---|
| Admission closed | `reserve` fails → `invalid_state` | `request_core.cpp:165-182`; `uring_backend.cpp:449-456` |
| Capacity full | `reserve` fails → `would_block`; scope reservation released | same |
| `accept` fails | rollback path releases the reservation and slot | `request_core.cpp:214-226` |
| SQ full during drain | flush inline, then retry `get_sqe` once | `uring_transport.cpp:141-149` |
| Submit returns retryable (EINTR/EAGAIN/EBUSY) | ledger reconciled against `io_uring_sq_ready`; `dispatch_retry` advertised; 10 ms bounded retry | `uring_transport.cpp:262-268`; `async_io_context.cpp:22,632-652` |
| Submit returns fatal | reconcile then poison + Class-A recovery | `uring_transport.cpp:271-278,337-524` |

All rollback/reconcile/poison paths are mutation-guarded
(`SLUICE_BACKEND_CONTRACT_MUTANT_SUBMIT_BATCH_INVISIBLE`,
`..._MASKED_SQ_CARDINALITY`, `SLUICE_K2_E18_MUTANT_DEFER_PAST_POISON`). They are
never removed on low-frequency grounds.

**Required-investigation gap.** Brief §5 asks for a *concurrent
submission/cancel comparison*. It was **not executed**: every census arm has
`control_cqe = 0`, and the mutation receipts cover a poison window and peer
acceptance, not a simultaneous submit + cancel interleaving. The ordering
argument above is source-level. This is disclosed as an unmet item in
[measurement-review.md](measurement-review.md) §7.

## 7. Competing explanations

**A — the fixed enter cost is real and amortizable.** The submit side's only
non-trivial cost is exactly one enter per op (≈1,720 cycles/op ≈15.8 % of total
cycles from `__io_uring_submit` 5.7 % + syscall entry/exit ≈10 %). Delaying the
flush to the pass boundary lowers enter frequency to the number of passes that
carry transport work; if the per-enter fixed fraction is large enough, the
saving exceeds the band and CPU and wall fall together.

**B — per-SQE cost plus same-CPU worker backfill.** The 1,720 cycles/op is
mostly per-SQE work (SQE copy, io-wq enqueue, CQE post), not per-enter trap
cost; the worker occupies a large share of all cycles on the same logical CPU,
so saved main-thread cycles are filled by the copy chain and wall is set by copy
throughput. Prediction: enters fall, CPU/wall stay flat.

These have opposite predictions on the same arm. A third explanation (mutex
micro-work) was excluded in advance: all mutex symbols are 3.4 % of cycles.

## 8. Counterfactual arm

`bench/research/key-path-593/patches/k2-e18-arm.patch` (+ test re-pins):
`dispatch_after_accept` no longer calls `submit_transport_locked` inline
(`uring_transport.cpp:87`). Dispatch still drains inline; prepared SQEs stay in
the ring and are flushed by the existing pass-start call
(`uring_backend.cpp:631`). `service_pending_controls_locked_` stays inline. The
residue signal is the existing `unsubmitted_after_dispatch` edge, which now
fires on every deferred submit because the ledger is non-empty.

Test re-pins (each a mechanism-specific assertion; the obligations stay
asserted): #585's flush oracle now expects 0 flushes after accept and exactly 1
on the first pass; `u2b_accepted_dispatch_wakes_parked_owner` expects
`sq_ready == 1` for the peer; the poison-window case flushes the armed read
before poisoning; the external-loop W-03 case accepts a residue wake with zero
delivery before the completion.

The arm is **ungated**: it defers at every depth and in every topology, which is
why it is not a candidate.

## 9. Results

### 9.1 enters/op

| Case | baseline | K2-a | change |
|---|---:|---:|---:|
| W02-d4 | 1.00007 | 0.25007 | −75.0 % |
| W02-d16 | 1.00007 | 0.06257 | −93.7 % |
| W02-d64 | 1.00007 | 0.01572 | −98.4 % |
| W02-64k-d16 | 1.00007 | 0.06317 | −93.7 % |
| W02-d1 | 1.00007 | 1.00007 | 0 |
| W03-d4 | 1.00007 | 0.25015 | −75.0 % |
| W03-d16 | 1.00007 | 0.06257 | −93.7 % |
| W03-d64 | 1.00007 | 0.01572 | −98.4 % |

The W-03 figures are the K2-a arm's own census (`ab/K2-a/census`). This
corrects an error in the draft synthesis, which listed K2-a W-03 at 1.00007;
the ungated arm defers in the split-owner form as well, whereas K4-b's topology
gate keeps W-03 inline (1.00007 at every depth). D=2/D=3 were never measured;
the ungated ≈1/D pattern predicts 0.5 enters/op at D=2.

### 9.2 End-to-end (15 pairs, full binary)

CIs are bootstrap 95 % (10,000 iterations, seed 593) for this final round.

| Case | CPU | wall | p50 | p95 | p99 | negative CPU pairs |
|---|---:|---:|---:|---:|---:|---:|
| W02-uring-4k-d16 | **−32.20 %** [−32.63,−31.62] | −32.24 % | −35.91 % | −46.66 % | −45.14 % | 15/15 |
| W02-uring-4k-d64 | −37.66 % [−37.88,−36.95] | −37.71 % | −42.49 % | −52.12 % | −50.29 % | 15/15 |
| W02-uring-4k-d4 | −26.31 % [−26.78,−25.95] | −26.31 % | −17.10 % | −37.34 % | −36.21 % | 15/15 |
| W02-uring-4k-d16 saturate | −47.48 % [−48.64,−46.87] | −47.77 % | n/i\* | n/i\* | n/i\* | 15/15 |
| W02-uring-64k-d16 | −3.05 % [−3.34,−2.79] | −3.08 % | −2.73 % | **+7.65 %** | **+6.77 %** | 15/15 CPU; p99 0/15 |
| W02-uring-4k-d1 | **+14.12 %** [+11.75,+14.58] | +14.06 % | **+1297 %** | +14.73 % | +16.24 % | 0/15 |
| W03-uring-4k-d16 | −11.17 % [−11.49,−10.63] | −11.22 % | **+10.55 %** | +4.77 % | **+15.70 %** | 15/15 CPU; p99 0/15 |
| W03-uring-4k-d4 | −1.32 % [−4.35,+0.43] | −1.29 % | **+26.42 %** | −14.84 % | −21.48 % | 10/15 |
| W03-uring-4k-d64 | −12.27 % [−12.80,−11.91] | −12.22 % | +0.81 % | −9.22 % | +0.21 % | 15/15 |
| threadpool W02/W03 | −0.24 %/+0.70 % | −0.27 %/+0.70 % | −0.02 %/+0.67 % | −0.33 %/+4.13 % | −0.18 %/+1.95 % | 9/15 / 4/15 |
| A/A d16 | −0.86 % [−1.34,+0.79] | −0.86 % | −0.40 % | — | −1.85 % | 9/15 |
| A/A W03 | +0.13 % [−0.29,+0.82] | +0.22 % | +0.10 % | — | −0.39 % | 7/15 |

\* saturate latency is not instrumented (`lat_n = 0`); the archived compact
summaries encode it as +0.00 %, a reducer artifact.

The p99 pair ranges are wide (e.g. W-03 threadpool p99 range
[−8.71,+16.56]); the sign counts (0/15 for the 64 KiB and W-03 d16 p99s) are
the stronger evidence than the point estimates, and no formal noise-band test
is claimed for p99.

### 9.3 Why the gain exceeds the enter share

`perf record` (transient sudo) is a **0/1 single run per arm**, not a paired
measurement. It reports main-thread cycles −22.6 % and the io-wq worker's
**absolute** cycles −33.0 % (315.5 M → 211.3 M), with
`rep_movs`/`filemap_get_entry`/`_raw_spin_lock`/`io_worker_handle_work` all
falling. `io_eventfd_signal` and `__io_commit_cqring_flush` appear only in the
E18 arm and are billed per batch. Within the limits of a 0/1 record, batching
amortizes kernel-side per-item fixed cost as well as the syscall trap —
explanation A wins and B's "backfill cancels the saving" prediction is refuted.
The two 0/1 records also disagree on the worker share (32.52 % in the Phase C
attribution run vs 40.35 % in the K2 arm), so the share is quoted as a range
(32.5–40.4 %) and the `rep_movs` share as 12.0–14.2 %.

### 9.4 Micro calibration (corrected)

The micro probe is a **TSC/core-cycle calibration with a known defect**: the
`batched run1` line records only an `instructions` event, not `cycles`, so the
1,845 figure attributed to it is not a cycle count.

What the archive actually supports
(`evidence/discrimination/k2/micro-core-cycles.txt`):

| Round | core cycles, per-enter differential | TSC, per-enter differential |
|---|---:|---:|
| run 1 | not computable (batched arm has no cycles event) | — |
| run 2 | 1,723 | 3,247 |
| run 3 | 1,862 | 1,814 |

The median of the two valid core-cycle rounds is **1,792.5 cycles/enter**. The
separate N=64 TSC run (`micro-fixed-enter.txt`) gives 3,759.22 fixed out of
4,129.00 per-enter TSC (f = 0.910); on core cycles the equivalent fraction is
≈0.37 (1,792.5 / ≈4,806). The earlier "1,815 cycles/enter, f≈0.91" pairing
mixed a TSC differential with a TSC-derived fraction and is retracted. The
calibration supports an order-of-magnitude claim — low thousands of cycles per
enter, a large fixed share — not a precise constant. The end-to-end A/B result
does not depend on it.

### 9.5 Gate price

The D=1 and W-03 failures are the reason this arm is not a candidate. The
topology+depth gate of K4-b was designed from exactly these failures; its
enters/op are ≈2× K2-a's in the same-thread forms because the first submission
of each window stays inline and each pass adds one flush. Same-thread CPU falls
from −32.20 % to −28.07 % at d16, from −37.66 % to −35.53 % at d64, and from
−26.31 % to −13.06 % at d4 — the gate is most expensive at shallow depth.

## 10. Correctness

- 13 suite targets BASE GREEN at arm start; 7 key targets re-run GREEN at the
  end (including #585's flush-accounting oracle re-pinned to the deferred
  semantics).
- Mutation receipts (BASE GREEN → MUTANT RED → RESTORED GREEN):
  - missed wake — `SLUICE_K2_E18_MUTANT_RESIDUE_SIGNAL_DROPPED` →
    `k2_e18_residue_wake_test` rc=1 ("deferred SQE was not flushed promptly").
  - false eligibility — `SLUICE_K2_E18_MUTANT_DEFER_PAST_POISON` →
    `k2_e18_poison_eligibility_test` rc=134 (reservation accepted in the poison
    window never terminates + contract fail-fast).
  - lost residue / missing retry —
    `SLUICE_URING_PROGRESS_MUTANT_TRANSPORT_RETRY_UNADVERTISED` →
    `uring_progress_race_test` fails `poison_transition_wakes_parked_owner`.
  - batch/ledger accounting ×2 — `SUBMIT_BATCH_INVISIBLE` and
    `MASKED_SQ_CARDINALITY` → `backend_contract_conformance_uring_test` red.
- Every run passed the frozen G0 checker and byte oracle; 390/390 valid.

## 11. Verdict

**`ELISION_CANDIDATE_FOUND`.** The mechanism's end-to-end value is measured
(paired class `END_TO_END_GAIN`), but this arm is not a deployable candidate:
D=1 regresses +14.12 % (p50 ×14.1), W-03 d16 p99 regresses +15.70 %
(notifications +51.6 %), W-03 d4 p50 +26.42 %, and 64 KiB p99 +6.77 %. The only
implemented gate is K4-b's; therefore K2 and K4 are not two independent
supported elisions and the campaign records `ELISION_PERF_SUPPORTED` **once**,
under K4. This reclassification is a response to independent review (the earlier
text attached `ELISION_PERF_SUPPORTED` to this ungated arm).

## 12. Limitations

- tmpfs only; the io-wq share and the kernel-side amortization are host and
  filesystem dependent.
- `cpu_s` is the benchmark process; the io-wq worker is not in it (it is in
  wall).
- Kernel attribution is a 0/1 transient-sudo `perf record`, not a paired
  kernel-PMU A/B.
- D=2/D=3 unmeasured.
- Cancel/control load not exercised; the required concurrent submission/cancel
  comparison is missing.
