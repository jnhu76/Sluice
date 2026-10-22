# DP-3 — slot layout, initialization and lifecycle (candidate DP-3A measured; DP-3B/3C analytical)

## Slot facts (machine-verified)

168 B, 3 cache lines: phase/generation/flags/refs + RequestDescriptor
(0x28, straddling lines 0–1) on line 0; BorrowFacts (0x48) + IoOutcome
(0x60) on line 1; FileInfo (0x80) on line 2. One core mutex guards every
field. Arena of N slots is contiguous; 64-slot cores = 10.5 KB (L1), 4096
slots = 672 KB (L2). **All cache-line statements here are analytical** — this
host exposes no usable cache counters (hardware-attribution.md).

## DP-3A — redundant reserve() re-initialization (measured arm)

Base re-zeros ~132 B of the slot in `reserve()` (~14 stores) that
`release_slot_` already zeroed before the free-list push — a genuine double
write of the same bytes under the same mutex. dp3a reduces `reserve()`'s
free-list path to a single store (`phase = reserved`; receipt §B).

- **Measured (zop): −16.5 instructions/op, 15/15 pairs** (main and fixedbt
  families agree to the digit: 0.516 % both) — matches the ~13–16 predicted
  from the removed stores.
- Spinning cases: weak-positive, mixed sign balance (thr64 +0.38 % 9/6,
  cons4k +1.09 % 8/7, finfo +1.14 % 11/4, constp +1.34 % 9/6, write4k
  +6.48 % 9/6)
  — at the self-pair variance floor; recorded as indicative only.
- Release-side zeroing is **kept**: it is what makes the free-list push
  safe against stale-field reads by future acquirers and the
  `SlotObservation` test oracle; collapsing it was not tested in this round
  (it would move re-initialization fully to the acquire side without
  reducing bytes — the double-write has a single-writer-per-transition
  structure, not an obvious dead half).
- w01ctl: 0.000 %/−0.000 % (exact).
- Correctness: 76/76; the slot state machine's protocol tests
  (reserve→accept→…→reclaim, generation checks) are unchanged and green.
- **Classification: ELIMINATED_CONFIRMED** (the re-zeroing is gone; the
  effect is exact on the deterministic instrument).

Residual per-op zeroing after dp3a: `release_slot_`'s 16 stores (3 lines)
— see DP-3C for the only remaining line-2 traffic.

## DP-3B — hot/cold slot split (analytical; NOT implemented)

Idea: move FileInfo (40 B) out of `Slot` into an index-aligned cold array,
shrinking the hot record to 128 B (2 lines).

Assessment against the measured evidence:

1. After DP-1 (no payload) + DP-2B (no consumer copies), line 2's remaining
   traffic for non-file_info ops is: the `offer_terminal` metadata store,
   `begin_publication`'s slot load (dp1 skips via null out — so this read is
   already gone on inline paths), and `release_slot_`'s two zeroing stores.
   The split cannot remove the store or the zeroing (they address the slot
   by index; a cold array is touched just the same). **The split saves
   nothing that DP-3C (below) does not already remove at the source.**
2. Footprint: 168→128 B = 24 % smaller arena. At benchmarked capacities the
   arena is already L1-resident; the benefit would only appear at very large
   `request_capacity` (672 KB → 512 KB at 4096 slots, L2) — no benchmarked
   workload in this study operates there (w02 diagnostics vary capacity up to
   64; the saturating cases use 16–64).
3. Cost: an extra indirection or index computation at every metadata access,
   a second allocation (or a fused arena — the task forbids adding heap
   allocation), and a second set of zeroing sites. Under the root's
   "smallest mechanism" discipline the split is dominated.

**Classification: ANALYTICAL_ONLY_NOT_IMPLEMENTED — dominated by DP-3C on
traffic; footprint benefit unproven at any exercised capacity.**

## DP-3C — specialized payload storage / don't-write-unread-metadata (analytical; NOT implemented; sketch)

Idea: the producer side of line 2. `offer_terminal` stores the backend's
40 B FileInfo into the slot on **every** op; only file_info ops ever read
it. The backend knows the op kind statically at publish time
(`publish_one`/`publish_zero_op_inline` vs the file_info publisher), and the
slot's own `descriptor.op` already carries the same fact (existing authority
— no new state).

Sketch: gate the metadata store (and release_slot_'s two line-2 zeroing
stores) on `descriptor.op == file_info`, or split `offer_terminal` into
with/without-metadata forms selected by the existing descriptor. Effects
per non-file_info op: −40 B stores, −40 B zeroing, line 2 untouched over
the whole lifecycle (with dp1+dp2b already removing the loads).

Overlap checks required by the task:
- With `statx_buffers_`/prepared storage: the metadata's source is the
  backend's statx buffer; the slot copy is the only redundant write. The
  backend's prepared entry does not need the metadata (it is produced by the
  kernel after the backend's bookkeeping) — no overlap conflict.
- With dp2b: complementary, not overlapping — dp2b removes consumer-side
  loads/copies, DP-3C removes producer-side stores. Together with dp1 they
  reduce line-2 traffic to zero for non-file_info ops.
- Risk: `SlotObservation` and cancellation/diagnostic paths read slot
  metadata for in-flight ops; the gate must confirm which of those consumers
  are load-bearing before a product patch (this is precisely the
  mechanism-justification work, and why it was not rushed into an arm in
  this round).

**Classification: ANALYTICAL_ONLY_NOT_IMPLEMENTED — concrete sketch, derived
from existing authority, with named consumers to clear through the gate.**

## Authority-contraction audit for this slice (required by AGENTS.md)

- Mechanisms that became redundant after dp1/dp2b/dp3a: the publication
  payload snapshot (removed; `begin_publication`'s out-form retained but
  currently consumerless), the `RequestResultData` local for non-metadata T
  (removed in dp2b), reserve-side re-initialization (removed in dp3a).
- Mechanisms examined and **kept** because they protect real obligations:
  `release_slot_` full zeroing (single-writer-per-transition re-init;
  free-list safety), the generation checks (ABA protection), BorrowFacts
  (dead in production but read by the `SlotObservation` test API — removal
  is a gate decision with a named-consumer analysis, recorded in
  field-liveness-matrix.md residual 1).
- No slice moved production authority: all arms are derived-snapshot
  removals; the protocol state machine is untouched (76/76 protocol tests
  green on every arm).
