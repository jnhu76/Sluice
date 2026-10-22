# DP-0 generated-assembly findings (R1 static analysis)

The ten static questions of the audit, answered from `objdump -drwC` of the
frozen production objects (base arm, be28f275). Machine excerpts for the
load-bearing passages: `results/receipts/assembly-excerpts.md`. Slot offsets
below are machine-verified (base `request_core.o`): phase 0x00, generation
0x08, flags 0x10–0x17, observer_phase 0x18, execution_refs 0x1c,
control_refs 0x20, RequestDescriptor 0x28 (32 B, crosses the 0x40 cache-line
boundary into line 1), BorrowFacts 0x48 (24 B), IoOutcome 0x60 (32 B),
FileInfo 0x80 (40 B), slot size 0xa8 = 168 B, core mutex at core+0x58.

## 1. Does publication materialize the payload? (DP-1)

Yes, twice. Each uring/threadpool inline publication: (a) constructs a
120-byte `PublicationPayload` on the caller stack (base
`publish_zero_op_inline`: `mov $0xf,%ecx; rep stos %rax,(%rdi)` — 15 qwords
— plus 6 field stores); (b) `begin_publication(key, out)` fills the out
payload with the 24 B id from the key argument and, from the slot, the
descriptor op/offset/length (extending the copy to bytes 0x18–0x2f), the
32 B IoOutcome and the 40 B FileInfo (12 SIMD moves,
`results/receipts/assembly-excerpts.md` §A). The publisher
consumes only `payload.id` (24 B) — which equals the `RequestKey` it already
had in a register. Without LTO the compiler cannot remove either half
(gimple-sra-dse.md §2).

## 2. What does the result bridge copy? (DP-2)

`RequestBridge::observe/consume` (base) copy 32 B IoOutcome + 40 B FileInfo
from the slot into the caller's `RequestResultData` (72 B, default-initialized
in `Request<T>::try_result/take_result` — 3×`movaps` + `movq` of zeroing),
then `result_of_` copies the consumed fields again into `Result<T>`. For
`Result<std::size_t>` carriers the 40 B metadata round-trip is entirely
unread. For `Request<FileInfo>` it is required (mutation-verified: removing
it fails 4 conformance assertions — `results/receipts/m1-dp2-mutation-failures.txt`).

## 3. How much zeroing happens per slot recycle? (DP-3A)

Twice. `release_slot_` writes 16 stores covering all 3 cache lines
(§C of the receipt); `reserve()` then re-zeros ~132 B with ~14 stores over
the same lines before pushing the slot into the state machine. dp3a removes
the second zeroing (1 store remains: `phase = reserved`).

## 4. Is Slot.borrow read in production?

No. `BorrowFacts` (24 B at 0x48) is stored by `accept` and re-zeroed/
generation-reset by `release_slot_`, but no production path loads it; the
backend's `prepared_ops_` map carries the live borrow authority. Per op:
24 B dead stores + 24 B dead zeroing on cache line 1. Authority-contraction
candidate recorded in dp3-slot-layout.md (not implemented as an arm; it is a
state-removal question under the mechanism-justification gate, and the test
API `SlotObservation` reads it — the gate must decide whether that consumer
justifies the bytes).

## 5. Does W-01 (blocking read) move anything it need not?

No. `blocking::read_at` machine code: precheck → `pread` syscall →
`Result<size_t>` stores into the caller's slot. No control-plane object is
constructed. Byte-identical negative control across all six arms
(`b3c403c0…`).

## 6. Where do syscalls sit relative to the copies?

The copies sit entirely in user space between the mutex critical sections;
the uring submit/poll syscalls pass only pointers/indices. The publication
payload and result-bridge copies are therefore pure user-side overhead per
op, not part of any kernel contract.

## 7. What crosses a compiler boundary that DSE must respect?

Every `pthread_mutex_lock/unlock` call. Same-TU probes show DSE would delete
many of these stores if the lock were modeled as plain C++ (gimple-sra-dse.md
§4); the real calls are what make the double-zeroing and payload copies
physically executed.

## 8. Alignment/layout facts relevant to cache lines

Slot = 168 B = 3 lines; the 32-byte RequestDescriptor at 0x28 straddles
lines 0/1; IoOutcome straddles nothing (0x60–0x7f, line 1); FileInfo
0x80–0xa7 occupies line 2 (+4 bytes into line 3 at 0xa8? No — 0x80+0x28 =
0xa8 = slot end; FileInfo ends exactly at the slot boundary). For a
64-slot core (10.5 KB) the whole arena is L1-resident; for 4096-slot
configurations (672 KB) it is L2-resident — the layout analysis in
dp3-slot-layout.md uses these numbers and marks all of it analytical
(no PMU cache counters on this host, per hardware-attribution.md).

## 9. Did the arms change what was intended, and only that?

Verified per arm by machine-code diff/inspection:
- dp1/dp1dp2b: payload `rep stos` init gone from backends; `begin_publication`
  copy block still present but null-guarded (shared function retained for the
  metadata-consuming publication form).
- dp2: 4-arg observe/consume present; flag-guarded metadata copy visible
  (§D of the receipt).
- dp2b: split 5-arg observe/consume present with **zero** 0x80/0x90/0xa0
  accesses; no `RequestResultData` init in non-metadata instantiations.
- dp3a: reserve reduced to the single phase store (§B of the receipt);
  release_slot_ unchanged.
- dp1dp2b: both mechanisms simultaneously (combo section of the receipt).

## 10. What does the uring threadpool-free D1 loop actually execute per op?

For zop (single-poll deterministic case): submit (`io_uring_enter` wrapper)
→ publish (base: payload init + copy; dp1: null call) → take_result mutex
round-trip → `poll()` → mutex round-trip in deliver/observe path → consume →
release. The base/dp1/dp2b/dp3a measured deltas (27.5/8.8/16.5 instructions
per op, 15/15 pairs each) account for the removed payload/bridge/zeroing
instructions exactly; everything else in the loop is untouched, which is
what makes zop a valid per-candidate instrument.
