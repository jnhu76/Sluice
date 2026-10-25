# K4 — SQE/CQE batching and the kernel boundary

Verdict: **`ELISION_PERF_SUPPORTED` for the topology+depth-gated deferred
transport flush (K4-b), measured only at D=4/16/64**. Paired classification:
`END_TO_END_GAIN` for the gated W-02 4 KiB **D≥4** claim. K4-a (batch reap lock
domain + batch `cq_advance`) is **`NO_CHANGE_JUSTIFIED`** with no arm built and
only a partial upper bound.

Two scope corrections from independent review are incorporated:
**D=2/D=3 were not measured** (the earlier "D≥2" wording over-claimed; the
observed ≈2/D enter pattern predicts no enter reduction at D=2), and the
**cancel flush is not in `issue_running_cancel_locked_`** (see §6).

## 1. Question

Are there too many kernel interactions, fragmented batches, or per-item fixed
costs paid while CQEs are already reaped in batches?

## 2. Structural asymmetry

| Side | Measurement | Interpretation |
|---|---|---|
| CQE reap | 11.76 CQEs/`peek_batch_cqe` at d16, 28.88 at d64, 3.47 at d4 | already batched; batch = D-window, bounded by the pass entry |
| SQE submit | **1.00 SQE/enter at every D**, `sq_ready = 1.00`, 1.00005–1.00007 enters/op | never batched |

The submit side never forms a batch because `dispatch_after_accept` drains and
flushes inline in the submitter's critical section
(`src/async/uring_transport.cpp:86-88`). The pass-start flush
(`src/async/uring_backend.cpp:631`) finds an empty ledger and returns without a
syscall. This is by construction of the #585 flush accounting, not an accident
of caller cadence: even the saturating fill shape reports 1.00 SQE/enter.

## 3. K4-A — underfilled submit batches

Facts established by measurement, not by counting `io_uring_submit` calls:

| Quantity | Value |
|---|---|
| SQEs prepared/enter | 1.00 (all D) |
| SQEs submitted/enter | 1.00 (all D) |
| `io_uring_enter`/op | 1.00005–1.00007 |
| Kernel-accepted SQEs | equals prepared (ledger pop exactly 1, zero reconciles) |
| Pending ledger | empty between ops |
| SQ-full retries | 0 in steady state |

The Phase-A hypothesis "amortization is limited by caller refill cadence" was
**wrong for D>1** and is retracted: steady-state `passes/op` is 0.127 at d16
and 0.030 at d64, so deferring the flush leaves ≈8 and ≈33 SQEs to accumulate
between passes. Deferral therefore has real batch headroom at D≥4, and none at
D=1 (passes/op = 1.34 > 1).

## 4. K4-B — per-CQE coordination

`reap_cqes` peeks a batch of up to 32, then calls `handle_one_cqe` per CQE. The
per-item costs are:

| Item | Per CQE | Removable? |
|---|---|---|
| `dispatch_mtx_` acquisition in `handle_one_cqe` (`uring_completion.cpp:100`) | 1 lock pair | structurally yes (split `_locked`, batch scope); no arm built |
| `io_uring_cqe_seen` (`uring_completion.cpp:220`) | 1 release store to `cq.khead` | yes: `io_uring_cq_advance(take)` once per batch; data is copied before advancing |
| `offer_terminal`, `release_execution` | 2 core acquisitions | **no**: per-request exactly-once / borrow arbitration, can race cancel |
| publication pop | 1 `dispatch_mtx_` per entry | yes (K1-a; measured, sub-threshold) |
| router lookup | 1/CQE, ≈live/2 probes | closed by #580 |

Measured locks per CQE (K1 boundary; there is exactly one CQE per logical op in
every measured form, so per-CQE and per-op counts coincide): `dispatch_mtx_`
2.5, `RequestCore::mutex_` 12.4, `access_mtx_` 1.3 at d16 — see
[mechanism-ledger.md](mechanism-ledger.md) §3.

K4-a would batch the first two. It was not built. The withdrawal argument is
**partial and is stated as such**: all mutex symbols together are 3.4 % of
cycles (~370 cycles/op), so the per-CQE lock pair is below the campaign band;
but the `cqe_seen` khead store is **not a mutex and is not covered by that
bound**. No measurement bounds the store part. The decision rests on the mutex
bound plus K4-b's W-03 result locating the real cost in the kernel/driver
mechanism, not on a complete upper bound for the whole K4-a candidate.

## 5. K4-C — ledger and submission accounting

| Operation | Normal path | Verdict |
|---|---|---|
| `append` | 1/op before submit; validates physical position monotonicity | obligation: records "prepared but not consumed"; needed by rc<pending and rc<0 reconciliation |
| `pop` after consumed | 1/op, sequence check | obligation |
| `reconcile_ledger_with_kernel_locked_` | 0 in steady state | **must not be deleted** on frequency grounds; only mechanism for partial submit / retryable error (BACKEND-03) |
| control reconciliation (`mark_consumed_control_locked_`, O(router) lookup) | 0 in steady state | same |
| empty/nonempty test | 1/op at pass start | free |
| `submit_flushes_` | 1 relaxed add per flush, test-only reader | test evidence materialized on the production path; removing saves one relaxed add, below any measurable threshold; recorded, not proposed |
| `remove_exact` | queue-front, O(size) shift, size ≤ 64 | small, bounded, recorded |

Normal-path ledger work is the price of the kernel/ledger correspondence
obligation. The only confirmed "test state on the production path" is
`submit_flushes_`, and it is not worth a change.

## 6. The K4-b arm

Mechanism: `dispatch_after_accept` defers the kernel flush to the next
pass-start `submit_transport_locked()` **only when**
`submitter_drives_progress_locked_() && core_->outstanding_summary() > 1`.
Otherwise (first/isolated submission, D=1 shape, split-owner submitter) the
baseline inline flush is kept.

The topology predicate is a bounded derived summary: the two most recent
distinct threads observed driving progress passes, recorded under
`dispatch_mtx_` in `poll()` (`include/sluice/async/detail/uring_backend.hpp:285-308`;
`src/async/uring_backend.cpp:622`). It is **not** an authority and not a strict
same-thread detector — a mixed-use submitter that has driven progress within the
last two distinct drivers can still defer. Both flush choices are semantically
complete, so this is a performance risk, not a correctness risk.

Preserved unchanged:

- residue advertisement (`advertise_residue`, `uring_transport.cpp:115-122`)
  including `unsubmitted_after_dispatch`;
- SQ-full recovery inside `dispatch_one_locked` (and the SQ-full recovery flush
  at `issue_running_cancel_locked_`, `uring_transport.cpp:647`);
- pass-start flush in `poll()`;
- `dispatch_retry_remains_locked_()` and the 10 ms bounded retry.

**Added work per deferred submit (missing from an earlier version).** Every
deferred submission makes `unsubmitted_after_dispatch` true, so
`signal_ready_progress()` runs (`uring_transport.cpp:96-101`): it takes the
progress mutex, increments the epoch, and may write the eventfd
(`include/sluice/async/detail/progress_source.hpp:252-264`). This is one of the
costs the removed enter pays for; the census does not separate it from the
residue path. The win is still large, but the "added work" answer must include
it.

**Correction — the cancel flush.** `issue_running_cancel_locked_`
(`uring_transport.cpp:620-693`) prepares the cancel SQE and appends it to the
ledger; it does **not** call `io_uring_submit` on the normal path. `cancel_key`
(`:584-618`) only signals progress. The cancel SQE is therefore flushed by the
same mechanisms as any other SQE: the next inline `dispatch_after_accept`
(baseline) or the next pass-start `submit_transport_locked` (deferred form),
plus the SQ-full recovery branch (`:647`). A deferral delays cancel-SQE
submission to the next pass; this is exactly the path that was never measured
(`control_cqe = 0` in every census arm). The K4-b research README's phrase
"cancel flush (`issue_running_cancel_locked_`)" is imprecise and is corrected
here.

Patch: `bench/research/key-path-593/patches/k4b-final.diff` (877 lines,
sha256 `4e9018d8…eef9`; applied to a fresh worktree at the base and all eight
touched files compared byte-identical).

## 7. Results per depth

enters/op (census + strace double-check):

| Case | baseline | K4-b | change |
|---|---:|---:|---:|
| W02-d4 | 1.00007 | 0.50017 | −49.99 % |
| W02-d16 | 1.00007 | 0.12577 | −87.42 % |
| W02-d64 | 1.00007 | 0.03447 | −96.55 % |
| W02-64k-d16 | 1.00067 | 0.13267 | −86.74 % |
| W02-d1 | 1.00007 | 1.00007 | 0 (gate) |
| W03-d4 / d16 / d64 | 1.00007 | 1.00007 | 0 (topology gate) |

The measured points fit a ≈2/D pattern (first submission of each window inline
plus one flush per pass: 2/4=0.50, 2/16=0.125, 2/64=0.031). **At D=2 that
pattern gives 1.0 enters/op — no enter reduction — while the gate
(`outstanding > 1`) already defers.** D=2 and D=3 were never measured. The
supported claim is therefore **D=4/16/64**, not "D≥2".

Full-binary paired results (15 AB/BA pairs; CI = bootstrap 95 % for the final
rounds; p95 is reported from `pair-results.json` / raw JSONL):

| Case | CPU | wall | p50 | p95 | p99 | negative CPU pairs |
|---|---:|---:|---:|---:|---:|---:|
| W02-d16 | **−28.07 %** [−28.69,−27.34] | −28.08 % | −34.08 % | −39.27 % | −39.97 % | 15/15 |
| W02-d64 | −35.53 % [−35.68,−34.69] | −35.53 % | −41.74 % | −49.94 % | −48.49 % | 15/15 |
| W02-d4 | −13.06 % [−13.69,−12.30] | −13.10 % | −10.58 % | −14.12 % | −17.10 % | 15/15 |
| W02-d16 saturate | −43.06 % [−43.83,−42.71] | −43.27 % | n/i\* | n/i\* | n/i\* | 15/15 |
| W02-64k-d16 | −2.99 % [−3.29,−2.40] | −2.97 % | −2.61 % | **+7.50 %** | **+7.46 %** [+7.14,+8.47] | 15/15 CPU; p99 0/15 |
| W02-d1 | −2.50 % [−3.54,−1.45] | −2.44 % | −6.18 % | −1.98 % | −1.67 % | 15/15 |
| W03-d16 | +0.30 % [−0.47,+0.82] | +0.29 % | +0.13 % | +0.46 % | +0.38 % | 7/15 |
| W03-d4 | **+2.54 %** [+1.32,+4.79] | +2.57 % | +1.35 % | +2.99 % | −0.89 % | 1/15 |
| W03-d64 | −0.09 % [−0.62,+0.28] | −0.11 % | −0.12 % | +0.11 % | +0.07 % | 9/15 |
| threadpool W02 / W03 | +1.19 % / −0.17 % | +1.18 % / −0.12 % | +1.17 % / −0.45 % | +0.52 % / −0.26 % | +0.59 % / −0.29 % | 2/15 / 10/15 |
| A/A W02-d16 | +0.40 % [−0.89,+1.19] | +0.32 % | +0.02 % | — | +0.14 % | 7/15 |
| A/A W03-d16 | −0.13 % [−1.13,+0.48] | −0.13 % | +0.40 % | — | −1.46 % | 8/15 |

\* saturate latency is not instrumented (`lat_n = 0`); the archived compact
summaries encode it as +0.00 %/+0.00 %, which is a reducer artifact, not a
measurement (see [measurement-review.md](measurement-review.md) §6).

Context switches: d16 vctx/ictx 17,850/16,477 → 4,985/4,532; d64
16,848/16,532 → 1,228/1,221; W-03 13,337/8,295 → 12,881/8,095. Poll/op −16.34 %,
wait/op −30.48 % at d16. LLC-load-misses rise from 0.038 to 0.045/op in the
primary shape, which does not offset a 28 % CPU reduction.

## 8. Why the gain exists (mechanism beyond the syscall)

The end-to-end CPU/wall result is measured on K4-b. The **kernel-side
attribution is not**: the K4-b session had user-space PMU only
(`perf_event_paranoid = 2`), and its `cycles:u` falls only 4.2 % while process
CPU falls 28.07 % — the difference is sys/kernel time. The worker-cycle
evidence (io-wq absolute cycles −33.0 %, 315.5 M → 211.3 M; per-batch
`io_eventfd_signal`/`__io_commit_cqring_flush`) comes from the **K2 ungated**
session's 0/1 `perf record`, whose enter frequency (0.06257/op) differs from
K4-b's (0.12577/op). It is inherited, order-of-magnitude mechanism evidence,
not a K4-b measurement. The K4-b session's own evidence is the
process-CPU/`cycles:u` divergence plus the census context-switch collapse.

The two 0/1 `perf record` runs also disagree on the worker share: the
attribution report gives main 66.29 % / worker 32.52 %, while the K2 arm gives
58.43 % / 40.35 % for the same nominal W-02 d16 full binary. The documentation
uses the range 32.5–40.4 %, not a single point, and the `rep_movs` share
likewise ranges 12.0–14.2 %.

## 9. Remaining deviations and gaps

| Item | Evidence | Status |
|---|---|---|
| Payload dependence | 64 KiB d16 p99 **+7.46 %**, p95 +7.50 %, 0/15 p99 pairs improved, while CPU is only −2.99 % (`cycles:u` +7.9 %, `instr:u` +5.7 %) | p99 exceeds the 4.79 % band, so this is a **material** regression: a payload gate is required |
| W-03 d4 +2.54 %/+2.57 % | enters unchanged; census vctx +5.2 %, `cycles:u` −3.5 %; bootstrap CI [+1.32,+4.79] excludes zero | **below the campaign 4.79 % band and inside the only measured W-03-d4 A/A [−6.14,+6.51]** (`evidence/discrimination/k4/README.md` §4). Not a material regression under the campaign rule; **unattributed**, and no same-session W-03-d4 A/A exists. Recorded as an open deviation, not a productization blocker |
| D=1 −2.50 % | gate still reads `outstanding_summary()` (one core lock) and cannot defer; −2.50 % cannot be a mechanism gain | unexplained favourable deviation (layout/clock); compare with the discrimination round's d1 +1.88 % under the same gate, which shows the sign is not stable across sessions |
| D=2/D=3 | no data | unmeasured; the 2/D enter pattern predicts no enter reduction at D=2, and the gate still pays the core read and residue signal. The gate should be validated there or tightened before productization |
| Shallow-depth gain left on the table | K4-b d4 −13.06 % vs K2-a d4 −26.31 % | the gate's "first submission of each window stays inline" is most expensive at d4; recovering it needs a third gate (window state), i.e. a new mechanism |
| Mixed-driver predicate | derived "two most recent distinct drivers", not strict same-thread | semantics complete; performance risk; no mixed-use load measured |
| Cancel/control load | all census arms idle (`control_cqe = 0`) | unmeasured; the deferred form delays cancel-SQE submission to the next pass |

**No-mechanism controls and attribution.** Two shapes cannot be changed by the
gate: W-02 d1 (gate cannot defer) and the threadpool backends (the diff does not
touch `ThreadPoolBackend`). They measure −2.50 % (uring d1) and +1.19 %
(threadpool W-02). The signs differ, so this is not a uniform bias; it is
binary-layout/clock noise of order ±2.5 % in either direction. It cannot explain
a 28 % gain, but it does mean K4-b effects below ~3 % (e.g. the 64 KiB CPU
−2.99 %) should not be attributed to the mechanism without a paired control.

## 10. Correctness

- 14 suite targets BASE GREEN plus 7 key oracles re-run; one test re-pin
  (`u2b_accepted_dispatch_wakes_parked_owner` now expects the peer SQE deferred,
  `sq_ready == 1`) archived separately.
- Six mutation receipts RED then restored GREEN: missed wake
  (`SLUICE_K4_DEFER_MUTANT_RESIDUE_SIGNAL_DROPPED` → `k4_residue_wake_test`),
  retry residue (`k4_retry_residue_wake_test`), false eligibility / defer past
  poison (`k2_e18_poison_eligibility_test`, rc=134), race retry
  (`uring_progress_race_test`), and the two batch/ledger mutants
  (`backend_contract_conformance_uring_test`, 5 failures / rc=134).
- 390/390 runs passed the G0 checker and byte oracle.
- The #585 flush oracle is re-pinned for the deferred semantics in the research
  arm; a product change would need to redefine "flush on every transport" with
  Owner review.
- **Not covered by any receipt:** a deferred ledger with `size() > 1`
  undergoing an rc<pending partial submit, overflow flush, or shutdown; early
  reuse/stale generation in the accepted+claimed-but-unflushed window; a
  deferred multi-entry pass-start retry; and a cancel storm. These are
  obligation-mapping statements, not deferral-shaped evidence (see
  [correctness-review.md](correctness-review.md) §3 and §5).
- **Oracle quality:** `k4_residue_wake_test` (missed-wake) waits 100 ms rather
  than on a prepark counter and accepts any `WaitKind::progress` wake, so it is
  racy and does not prove the residue signal is the only wake source.
  `k4_retry_residue_wake_test` is deterministic but tests the inline-flush
  shape, not the deferred pass-start shape.

## 11. Verdict and productization preconditions

`ELISION_PERF_SUPPORTED` (form-gated, **measured D=4/16/64**). Before any
product proposal:

1. a payload gate (or a measured explanation of the 64 KiB p99 +7.46 %);
2. validation or tightening of the depth gate at D=2/D=3, where the 2/D enter
   pattern predicts no enter gain;
3. attribution of the W-03 d4 +2.54 % (sub-threshold, but unexplained) and a
   same-session W-03-d4 A/A;
4. re-measurement of the selected variant against K2-a's ungated form on both
   topologies (the gate choice is not settled: K2-a is better in W-03 d16 CPU
   but has a p99 regression, K4-b is flat but tail-neutral);
5. the measured composition with K1 (done — see
   [cross-path-interactions.md](cross-path-interactions.md)) and a decision on
   whether the K1 detail-header change is worth its ~2.6 % W-03 effect;
6. cancel/control load, the mixed-driver form, and the deferred-partial-submit /
   overflow / shutdown cases;
7. the mechanism justification gate for the derived topology summary, the
   per-submit `outstanding_summary()` read, and the per-deferred-submit
   `signal_ready_progress()` call;
8. re-validation on a real filesystem/device and a second host.

## 12. Limitations

- tmpfs only; the kernel-side amortization is filesystem, kernel and topology
  dependent.
- One socket, one kernel, SMT sibling not isolated.
- User-space PMU only in the A/B session; kernel attribution inherited from the
  K2 ungated 0/1 records.
- The 0/1 perf records are single runs per arm, not paired measurements.
- No cancellation storm, no D=2/D=3 points, one payload beyond 4 KiB.
- `cpu_s` excludes the io-wq worker (wall includes it).
