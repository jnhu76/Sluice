# DP-3 — slot layout, initialization and lifecycle (candidate DP-3A measured; DP-3B/3C analytical)

## Slot facts (machine-verified)

168 B, 3 cache lines: phase/generation/flags/refs + RequestDescriptor
(0x28, straddling lines 0–1) on line 0; BorrowFacts (0x48) + IoOutcome
(0x60) on line 1; FileInfo (0x80) on line 2. One core mutex guards every
field. Arena of N slots is contiguous; 64-slot cores = 10.5 KB (L1), 4096
slots = 672 KB (L2). **All cache-line statements here are analytical** — this
host exposes no usable cache counters (hardware-attribution.md).

Erratum (2026-10-23, second review): every "line N" below is a
**slot-relative** 64 B window, not a physical line. 168 is not a multiple
of 64 (168 mod 64 = 40), so consecutive slot starts drift by 40 B across
the arena and the slot offset 0x80 does not map to any fixed physical
cache line. No statement here may be read as a physical-line claim.

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

Residual per-op zeroing after dp3a: `release_slot_`'s 16 stores (3 slot-
relative lines) — for non-metadata ops the only remaining unconditional
line-2 movement (see the corrected DP-3C; the producer metadata store
claimed by the original text does not exist on non-metadata ops).

## DP-3B — hot/cold slot split (analytical; NOT implemented)

Idea: move FileInfo (40 B) out of `Slot` into an index-aligned cold array,
shrinking the hot record to 128 B (2 lines).

Assessment against the measured evidence:

1. After DP-1 (no payload) + DP-2B (no consumer copies), line 2's remaining
   traffic for non-metadata ops is: `release_slot_`'s two zeroing stores,
   and nothing else — the `offer_terminal` metadata store named by the
   original text is withdrawn by the DP-3C erratum below (the store is
   `has_metadata`-gated and does not execute on non-metadata ops).
   The split cannot remove the zeroing either (both layouts address the
   slot/array by index). With DP-3C's premise corrected, the earlier
   "the split saves nothing that DP-3C does not already remove" statement
   is withdrawn: neither candidate has an implemented arm and this host has
   no cache/TLB PMU data, so no cross-workload domination is claimed.
2. Footprint: 168→128 B = 24 % smaller arena. At benchmarked capacities the
   arena is already L1-resident; the benefit would only appear at very large
   `request_capacity` (672 KB → 512 KB at 4096 slots, L2) — no benchmarked
   workload in this study operates there (w02 diagnostics vary capacity up to
   64; the saturating cases use 16–64).
3. Cost: an extra indirection or index computation at every metadata access,
   a second allocation (or a fused arena — the task forbids adding heap
   allocation), and a second set of zeroing sites. Under the root's
   "smallest mechanism" discipline the split is dominated.

**Classification: ANALYTICAL_ONLY_NOT_IMPLEMENTED — no implemented arm and
no cache/TLB PMU evidence; under the analyzed capacities and traffic model
there is no demonstrated reason to implement the split, and the earlier
"dominated by DP-3C" comparison is withdrawn (see DP-3C erratum).**

## DP-3C — metadata write-site audit (analytical; NOT implemented; redefined
## by the 2026-10-23 erratum)

**Erratum (2026-10-23, second review).** The original sketch claimed
`offer_terminal` stores the backend's 40 B FileInfo into the slot on
**every** op. Baseline source refutes this:

- `request_core.cpp` gates the store: `if (candidate.has_metadata)
  { slot->metadata = candidate.metadata; }`, and
  `TerminalCandidate::has_metadata` defaults to `false`.
- `uring_completion.cpp`: the byte-op (read/write) success branch never
  sets the flag; only the metadata-op branch (`file_info`/`size`) does.
- `threadpool_backend.cpp`: same gate, `file_info || size` only.
- Zero-ops never reach `offer_terminal` at all: `accept()` makes them
  terminal and the backend publishes them inline.

The claimed per-non-metadata-op 40 B producer store therefore does not
exist, and no measured result in this study depended on it (the DP-1, DP-2B
and DP-3A receipts and verdicts are untouched by this correction). The
original gate sketch (`descriptor.op == file_info`) would additionally
have been a correctness break: `size` ops produce metadata too, and
`FileSize` results require `metadata.size` (field-liveness-matrix.md ●).

**Redefined candidate.** First separate the two metadata objects and their
write sites, then audit each site for machine work that non-metadata ops
actually execute:

- `Slot::metadata`: reserve-side zeroing (base only; already removed by
  dp3a), `offer_terminal`'s gated store (metadata ops only), release-side
  zeroing (`release_slot_`, every op).
- `TerminalCandidate::metadata`: caller-stack object, filled only on
  `file_info`/`size` completions.

On current source the only unconditional per-non-metadata-op line-2
movement is `release_slot_`'s 40 B zeroing (part of the 16-store re-init;
dp3a already removed the reserve-side twin). A successor candidate targeting
that site must clear the same named consumers through the mechanism gate
(`SlotObservation`, cancellation/diagnostic paths) **plus** size-op metadata
liveness: `FileSize` reads `metadata.size`, and the specialized 8-byte size
carrier remains the recorded alternative (field-liveness-matrix.md
residual 4).

Overlap checks from the original sketch are superseded by this redefinition;
the statx-buffer overlap note remains valid but is now about where the
metadata-op copy originates, not about a per-op elimination.

**Classification: ANALYTICAL_ONLY_NOT_IMPLEMENTED — corrected source model;
no implemented arm; no measured figure. The DP-3B/DP-3C dominance
comparison is withdrawn: neither has an arm, and the remaining shared
target (release-side zeroing) is unmeasured.**

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
