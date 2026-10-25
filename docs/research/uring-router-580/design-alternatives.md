# R3 — Alternative designs: gate-blocked record and design-level analysis

## Gate outcome

R2 ended `NO_TRIGGER_NO_CHANGE` (see `trigger.md`): no real-kernel lookup
hotspot at any measured shape, hot path O(D/2) independent of C, 1 lookup/op,
~1.6% of user cycles at the worst artificial shape. Per the issue's stop rule
and the protocol's Exit A, **Candidate A and Candidate B are not implemented**.
This file records what they are and why they cannot pay under the measured
workload ratios, so the decision is auditable and the designs do not need to be
re-derived if a future workload re-opens the question.

Candidate 0 (production linear scan) remains the baseline and the only
admitted implementation.

## Candidate A — Exact cookie secondary index (bounded open-addressed hash)

Design sketch (not built): `hash(cookie) → bucket probe sequence`, buckets
holding `{cookie, router_index, state}` with exact 64-bit cookie comparison on
every hit; insert at dispatch, erase at retirement, tombstones on erase with
bounded rehash-free cleanup because live occupancy ≤ C guarantees an empty
bucket exists within the table.

Cost structure against measured ratios (Q = 1.00 lookups/op, U = 1 insert +
1 erase/op, M ≈ 0):

- Every op pays: 1 hash + probe + exact verify (save vs ~D/2 in-cache probes)
  **and** 1 insert + 1 erase (random-index writes into a 2-3× C-sized table →
  cache lines that the CQE path would otherwise not touch, plus tombstone
  bookkeeping).
- At D=64 the scan it replaces is ~34 sequential probes over ~2-3 hot cache
  lines (the live region is contiguous-by-LIFO); the index replaces that with
  a random write + random read into a larger structure. Expected net at
  production shapes: negative to neutral; the only regime where it wins is
  C ≫ D on the *lookup* side — but the measured hot path never exhibits
  C-dependence, so that regime does not exist on the CQE path.
- Failure-mode surface it adds: stale-entry misrouting (M2/M3/M5/M7 mutants in
  the protocol's list), tombstone exhaustion, rehash-free invariant proofs,
  overflow checks — all to accelerate a 1.6%-of-user-cycles path.
- Verdict: rejected by gate; would also fail R4 break-even at measured Q/U.

## Candidate B — Alternative full-domain representation

- **Sorted compact cookie→slot array + binary search**: full-domain, no
  narrowing. Lookup O(log D) expected ≈ 6 probes at D=64 — *not better* than
  the measured 8.5-33.8 linear probes over contiguous hot lines, and insert /
  erase become O(D) shifts on **every** operation. Strictly worse than both
  baseline and Candidate A at these ratios. Rejected without implementation.
- **Stable slot+generation mapping** (cookie encodes the router slot): this is
  H1's family; it necessarily narrows the cookie field or the capacity domain
  (`h1-identity-matrix.md` §Domain narrowing). The issue forbids resurrecting
  it and forbids domain narrowing. Rejected by constraint, not by measurement.
- Verdict: no Candidate B passes both the full-domain constraint and the
  cost-ratio test; per protocol ("不要为了凑齐两个候选而接受不安全的设计")
  the honest record is that the gate closed before B needed a build.

## Work-elimination findings carried forward (R1)

- **WE-1** — `TransportLedger::Entry` gains a `router_index` field captured at
  append time (both append sites already hold it). Eliminates the S1
  (`mark_consumed_control_locked_`) and S6 (poison Class-A) lookups entirely;
  both sites already verify cookie+handle afterwards, so identity authority is
  unchanged. Measured S1 cost is O(D) and small (32.5 avg probes per consumed
  control at r=0.2); WE-1 is recorded as analysis — not built this round.
- **WE-2** — reverse the `find_live_router_index_` scan direction to match the
  LIFO live region. One-line, no new state, identity rule unchanged (exact
  slot+generation equality, unique live match). This is the only change whose
  measured cost basis is C-dependent (992.5 probes/cancel at C=1024 vs 32.3 at
  C=64). Built as arm `we2` and measured in R6 as **EXPLORATORY_ONLY /
  NOT_ADMITTED** evidence; productization would need its own native round and
  owner decision, exactly like any other mechanism.

All candidates and findings preserve: full 64-bit cookie domain `[1, 2^63)`,
no capacity narrowing, exact-identity verification at every consumer, control
tag semantics, ledger/poison recovery authority, and CQE `user_data` semantics.
