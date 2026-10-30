# K1 — Completion → publication → observer

Verdict: **`NO_CHANGE_JUSTIFIED` under the campaign materiality band (4.79 %)**.
The lane found real, structurally provable redundancy (3.098 mutex
acquisitions/op), removed it without violating an obligation, and still measured
only −2.29 % CPU on the one form where the effect exists. **The verdict is
band-dependent**: the same measurement is above the W-03-d16 session A/A band
(max 1.88 %) and would be material under that band. The band's provenance is
disclosed in §8; if a reader rejects the band, this result is `INCONCLUSIVE`.

## 1. Question

Between the CQE and the user-observable result, which mechanisms are required
and which are only the current implementation's intermediate handoff?

## 2. Mechanism inventory

| # | Mechanism | Location | Authority | Classification |
|---|---|---|---|---|
| M1 | `reap_cqes` — bounded batch reap, pass boundary at entry, overflow flush | `src/async/uring_completion.cpp:200` | CQ ring shared memory | unavoidable |
| M2 | `handle_one_cqe` — one `dispatch_mtx_` per CQE, router lookup, control branch | `uring_completion.cpp:99-144` | `dispatch_mtx_` + router | unavoidable (routing, control) |
| M3 | `finalize_operation_terminal_` — `offer_terminal`, `release_execution`, publication push, router retirement | `uring_completion.cpp:23-96` | RequestCore terminal / lifetime | unavoidable for 1–2; 3–4 are handoff/bookkeeping |
| M4 | `publication_pending_` | `include/sluice/async/detail/uring_backend.hpp:322`; push `uring_completion.cpp:92`; drain `uring_backend.cpp:646-657` | `dispatch_mtx_`-protected multi-producer FIFO | lock-domain / pass-boundary mechanism, **not** a superfluous state |
| M5 | publication drain — one `dispatch_mtx_` per entry, `owed` gate | `uring_backend.cpp:638-686` | `dispatch_mtx_` | per-entry lock is avoidable (K1-a); `owed_count_` gate is the existing derived counter |
| M6 | `publish_one` — `begin_publication`, unlocked kind read pinned by `publication_inflight`, `complete_publication` | `uring_backend.cpp:578-608` | RequestCore publication boundary (REQ-04) | unavoidable |
| M7 | `deliver_event` — `claim_observer_delivery`, `on_ready`, `retire_observer_delivery` | `uring_backend.cpp:610-614` | OBS-02 | unavoidable when an observer is armed; no-op state queries when not (K1-b) |
| M8 | `take` — scope scan, `consume_public_result`, slot reclaim | `src/async/request_scope.cpp:252`; `request_core.cpp:326-347,603-636` | RequestCore, `binding_live`, generation | unavoidable |
| M9 | router reverse lookup | `uring_transport.cpp:551` | router | #580 closed, not reopened |
| M10 | `retire_router_entry_` | `uring_transport.cpp:559` | router / cookie free list | unavoidable (late control CQE) |

### Why the completion phase cannot publish directly

1. `handle_one_cqe` holds `dispatch_mtx_`; `on_ready` is a host hook and
   THREAD-02 forbids host hooks under core/backend locks. The hook may re-enter
   submit/cancel, which take the same lock.
2. PROG-01 requires a bounded pass and explicitly allows an *entry snapshot* of
   queued publications. `pass_bound` is that boundary.
3. Four producers (finalize, poison, cancel, undispatched retirement) need one
   lock-free consumer.
4. The unlocked `delivery_[slot].kind` read depends on `publication_inflight`
   pinning the slot; inlining publication into the completion critical section
   would move that read under the lock and change the pinning argument.

`publication_pending_` is therefore **not** an unnecessary intermediate state.
What is unnecessary is the per-entry lock used to empty it, and the
claim/retire pair executed when no observer exists.

## 3. Per-completion cost (measured)

- `dispatch_mtx_`: 2.5 acquisitions/op at d16 (2 submit-side + ~2.5
  completion/pass aggregate); the K1 segment is one lock in `handle_one_cqe`
  plus one per publication pop.
- `RequestCore::mutex_`: 12.4 acquisitions/op; 7 of them are in the completion
  segment (offer, release, begin, complete, claim, retire, consume) plus
  ready-lookups.
- Queue operations: one push, one pop per op. A per-pass trace histogram
  (**W-02 only**) shows `published == reaped` for every non-empty pass, with no
  cross-pass backlog. The W-03 observe arm has aggregate
  `pub_pending_push = pub_pending_pop = 540,000`, but **no per-pass trace**;
  the per-pass zero-violation invariant is therefore W-02 evidence, not W-03
  evidence.
- Whole user-space budget: 2,572 cycles/op at d16 against 8,293 kernel
  cycles/op. All mutex symbols together: 3.4 % of cycles (~370 cycles/op).
- Locks per CQE (one CQE per op in every measured form): completion-segment
  `dispatch_mtx_` 2 + core 7, plus pass-amortized locks; the all-form totals are
  in [mechanism-ledger.md](mechanism-ledger.md) §3.

## 4. Normal read vs cancellation/reuse

| Path | Extra work | Consequence for K1 |
|---|---|---|
| Normal read | none | 1:1:1 CQE → terminal → publication → take at every D |
| Pre-dispatch cancel (`uring_transport.cpp:584`) | `remove_exact`, core `cancel`, `release_execution`, publication push from the cancel path, conditional user signal | the publication queue acquires a second producer; the drain is uniform |
| In-flight cancel (`uring_transport.cpp:620`) | `acquire_control` pins the slot, cancel SQE, ledger entry, sticky intent if SQ is full | adds three core transitions; **cancellation increases K1 work, never decreases it** |
| Control CQE (`uring_completion.cpp:101-144`) | `release_control`, router entry lives past the original CQE | justifies `terminal_delivered` and late-entry retirement |
| Reuse (`request_core.cpp:603-636`) | generation++, free-slot return; next submit re-initializes `delivery_` and asserts `!event_owed` | generation prevents identity aliasing; cookie is monotonic |

**Required-investigation gap.** Brief §4 asks for a normal-read vs
cancellation/reuse *comparison experiment*. It was **not executed as a load
experiment**: every census arm reports `control_cqe = 0` and zero control
scans, and no cancel/reuse AB/BA arm exists. The table above is source-level
plus deterministic-test evidence, not the requested comparison. This is stated
again in [measurement-review.md](measurement-review.md) §7 as an unmet item.

## 5. Competing explanations

| # | Explanation | Prediction |
|---|---|---|
| H1 | The K1 segment is dominated by uncontended lock instruction + resolve constants (~150–250 ns/op from a static estimate) | An arm changes CPU/op by an amount comparable to ~150–250 ns; visible if above the A/A band |
| H2 | Coherence traffic dominates: the slot-indexed arrays and mutex cache lines bounce between threads in W-03 | The same arm gains on W-03 and is null on W-02 — the effect is per-acquisition cost, not instruction count |
| H3 | Zero-sum / no measurable gain: the segment is a minority of the lifecycle and the kernel segment dominates | Any legal elision stays inside the A/A band |
| H4 | Observer dead weight: the no-observer claim/retire pair is pure removable work and should be visible alone | A K1-b-only arm would show a measurable gain |

H1, H2 and H3 are not mutually exclusive; the arm distinguishes **where** the
effect appears (W-03 vs W-02) rather than proving one mechanism. H4 was folded
into the merged arm because no single-factor split was built.

## 6. Counterfactual arm

One merged test-only arm (`bench/research/key-path-593/patches/k1-elision.patch`,
4 files, +83/−40).

- **K1-a** — the publication phase becomes a single-lock entry snapshot: one
  `dispatch_mtx_` acquisition drains up to the entry-time size into a
  preallocated batch queue, and the owed-present scan is collected into a
  preallocated `owed_batch_` **in the same scope**; `publish_one` / deliver /
  `release_control` still run outside the lock. Precedent: the threadpool
  backend does the same (`threadpool_backend.cpp:556-580`), and PROG-01 permits
  an entry snapshot.
- **K1-b** — `complete_publication` gains an overload that returns the
  `queue_armed_observer` decision it already computes. `deliver_event` runs
  `claim_observer_delivery` / `retire_observer_delivery` only when that flag is
  set or `routing_sink_` is non-null.
- **The sink call is not elided.** The first build removed `on_ready` as well;
  two deterministic tests killed it (`uring_owed_residue_test`,
  `uring_owed_counter_parity_test` — `sink_deliveries` is the delivery oracle).
  The corrected build passes 9 tests. This is a kill, not a formal
  BASE→MUTANT→RESTORED receipt; no restored-green log was produced for it.

### New runtime state introduced by the arm

| State | Kind | Bound | Justification / risk |
|---|---|---|---|
| `publish_batch_` (`BoundedDispatchQueue`) | derived handoff buffer | `request_capacity` | lets the pass drain under one lock; preallocated at construction (`k1-elision.patch:93`) |
| `owed_batch_` (`vector<OwedDelivery>`) | derived handoff buffer | `request_capacity` | same; `resize` at construction (`:94`) |
| `OwedDelivery` struct | derived record | per owed event | carries key/kind/armed out of the lock |
| `DeliveryRecord::observer_armed` | derived summary | 1 bit/slot | carries the `complete_publication` decision to the zero-op owed path |
| `complete_publication(id, bool*)` overload | interface shape change | — | installed detail header, requires Owner review |

These are **new runtime state** and were missing from an earlier version of the
ranking's "no new state" claim. They add no authority: each is a copy of a fact
already decided under the core lock or in the same pass. They still have to pass
the mechanism-justification gate as derived summaries (TERM-01/BACKEND-04).

### Semantic reorder in K1-a

The arm collects the owed batch **before** the publish loop. Baseline order is:
publication drain/publish, then owed sweep (`uring_backend.cpp:659-681`). In the
arm, a zero-op owed event created by a publication hook **during this pass** is
collected by the next pass, whereas baseline may deliver it in the same pass.
PROG-01 permits this (post-pass facts; entries created during a pass are the
next pass's immediate work), and the owed gate advertises it, but **no dedicated
oracle covers "publication hook creates an owed event inside the same pass"**.
This is a behavior change, not just a lock fusion.

### Owed scan lock hold (K1-a risk)

When `owed_count_ != 0`, the arm holds `dispatch_mtx_` across
`for (i = 0; i < capacity_; ++i)` (`k1-elision.patch:201-216`). Baseline took
and released the lock per owed slot (`uring_backend.cpp:659-681`): more lock
traffic, but a short hold each time. `request_capacity` may be as large as
`UINT32_MAX` (`uring_backend.cpp:150-156`), so the arm's hold is O(capacity)
with no work-proportional bound. Brief §4 K1-B forbids widening a lock scope
in a way that introduces progress blocking. **All measured K1 loads had
`owed_count_ == 0`**, so the expanded branch never executed in the A/B. This is
an implementation/coverage limitation of the arm; it is one more reason the
result is `NO_CHANGE_JUSTIFIED` rather than a candidate.

### `observer_armed` invariant (correction)

The arm's comment says the flag is "stable while `event_owed` pins the slot".
That is imprecise. `event_owed` is only set by the zero-op path
(`uring_backend.cpp:553-572`). The real stability argument is that
`published` is the linearization point: `register_observer` returns
`already_terminal` once `slot->published` is true
(`src/async/detail/request_core.cpp:508-511`), so no observer can arm after the
publication decision. For the normal path the flag is a local returned by
`complete_publication` and used immediately; for the zero-op path it is stored
in `DeliveryRecord`. In addition, `DeliveryRecord::observer_armed` is **not
cleared** when a slot is re-initialized (`uring_backend.cpp:497-499` clears
`kind`, `event_owed`, `owed_key` only), so it is a stale derived field. It is
masked because it is only read when `event_owed` is true, and re-initialization
fails fast if `event_owed` is still set. The code is currently safe; the
documented invariant was wrong and the stale field is a latent maintenance
hazard.

Mechanism sanity (observe build): K1 completion-segment acquisitions
**3.209 → 0.111**/op (W-03 d16; W-02 3.254 → 0.130). The delta decomposes as
claim/retire −2.000, per-entry pop lock −0.889, C/E fold −0.209. W-02 per-pass
`published == reaped` holds on 70,061 traced passes; W-03 has aggregate
push/pop parity only.

## 7. Results

15 interleaved AB/BA pairs per case, full binary, `taskset -c 2`, `cpu_us` =
process user+sys of the benchmark. CIs here are min–max pair ranges (the K1
reducer does not bootstrap).

| Case | Mode | CPU | wall | p50 | p95 | p99 | negative CPU pairs |
|---|---|---:|---:|---:|---:|---:|---:|
| W03-uring-4k-d16 | elide vs full | **−2.29 %** [−4.86,−0.99] | −2.31 % | −1.95 % | −1.54 % | −2.23 % | 15/15 |
| W03-uring-4k-d16 | A/A | +0.18 % [−1.17,+1.88] | +0.12 % | — | — | — | 7/15 |
| W02-uring-4k-d16 | elide vs full | −0.43 % [−1.68,+4.49] | −0.38 % | 0.00 % | +0.82 % | **+3.07 %** | 11/15 |
| W02-uring-4k-d16 | A/A | −0.64 % [−3.04,+2.06] | −0.67 % | — | — | — | 8/15 |

The W-02 p99 point estimate is +3.07 % (4/15 pairs improved). It is inside the
same-session pair range but the sign does not support "non-regressing" on
every metric.

Cross-thread size bound: undistorted futex tracepoints are ≈0.27/op in **both**
arms (no reduction); `perf sched` context switches 313 vs 302 (≈0.005/op). The
removed acquisitions were uncontended fast paths; there was no removable
blocking.

**The 70.7 ns/op effect is not an independent cap.** Dividing it by 3.098 gives
22.8 ns per removed acquisition, which is a restatement of the same
measurement. The independent bound is the all-mutex share: 3.4 % of total
cycles ≈ 370 cycles/op across every mutex site, which is below the 4.79 % band.
An earlier version of this record called 3.1 × 22.8 ns ≈ 2.3 % a "hard cap";
that was circular and is retracted.

## 8. Threshold and verdict

- The campaign materiality band is **4.79 %**. Its provenance: it is the
  maximum A/A drift measured in the baseline round for `W03-uring-4k-d4`
  (`evidence/baseline/baseline-summary.md:126`, written 22:52), i.e. the value
  predates the K1 arm (23:58–00:11). It is **not defined in the brief**
  (`grep -c '4.79' brief.md` = 0), and the first archived artifact calling it
  "pre-registered" is the K1 README itself (00:11). The label
  "pre-registered" is retracted; what can be verified is that the value was
  measured before the arm, and that the same band was applied to every lane.
- Under the band, 2.29 % < 4.79 % ⇒ `NO_CHANGE_JUSTIFIED`.
- Under the W-03-d16 session A/A band (median 0.18 %, max 1.88 %) the same
  effect is material and reproducible (15/15). The result is therefore
  **threshold-dependent**; a reader who rejects the campaign band should read
  this lane as `INCONCLUSIVE`, not as "no effect".
- Productization would also require the `complete_publication` return-shape
  change in the installed detail header.
- The merged arm cannot attribute the effect between K1-a and K1-b; no
  single-factor arm exists.
- The arm's owed-scan lock hold (§6) was never exercised because all measured
  loads had no owed events. That alone disqualifies it as a product candidate
  on current evidence.

## 9. What was preserved

Exactly-once terminal, borrow retirement, publication begin/complete, armed
observer state machine, cancel-during-delivery pinning, the sink delivery call,
late control CQE routing, destructor quiescence, `pass_bound` boundedness, and
the `owed_count_` gate. With all of those preserved, the removable work still
buys only 2.29 %.

## 10. Mutation coverage (honest inventory)

| Failure class | Receipt |
|---|---|
| Over-elided sink delivery (K1-b over-reach) | two tests RED (`uring_owed_residue_test`, `uring_owed_counter_parity_test`), corrected build then 9 tests GREEN; **no formal MUTANT build or restored-green log** |
| Lost publication | **not mutated** |
| `publish_batch_`/`owed_batch_` drop or double count | **not mutated** |
| K1-b false negative (`observer_armed` false while armed) | **not mutated** |
| Double terminal / early reuse / stale generation | not applicable to the K1 diff, but not tested in a K1-shaped interleaving either |

This is a coverage gap, not a claim of correctness. See
[correctness-review.md](correctness-review.md) §3.

## 11. Reopen conditions

Re-measure on a new baseline if a product form of the K4 deferred flush lands
(it restructures the same pass) or if #578 produces a sustained split-owner form
with a different cost profile. The revival must re-run the Step-4 protocol with
a pre-run threshold registration; this verdict may not be carried over.

## 12. Limitations

- Warm tmpfs. W-03 effects are dominated by cache-line work; multi-socket/NUMA
  would raise per-acquisition cost but not the acquisition count.
- Only W-02/W-03 d16 were measured; 64 KiB, saturate and cancel forms were not.
- The per-pass publication parity is W-02 only; W-03 has aggregate parity.
- Cancellation/reuse was not exercised at load; the required comparison
  experiment is missing.
- The owed-scan lock expansion was never exercised (no owed events in any
  measured load).
- Tail values are per-pair medians; p99 pair ranges are wide, so sign counts are
  the stronger evidence than the point magnitude.
