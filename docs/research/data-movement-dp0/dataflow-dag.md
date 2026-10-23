# DP-0 data-plane object inventory and DAG (R0)

All sizes/offsets machine-verified against the frozen build
(compiler-inventory.md). This is the map the audit reasoned over; the
candidate analyses (dp1/dp2/dp3) cite back to these rows.

## Type inventory (per-request objects)

| object | size | storage | producer → consumer | copies per op (base) | lock domain | authority |
|---|---|---|---|---|---|---|
| `ReadOp`/`WriteOp`/`FileInfoOp` | user-held | user stack | user → context submit | 0 (read in place) | user thread | user owns buffer |
| `RequestKey` {ContextIdentity u64, SlotIndex u32, Generation u64} | 24 B | by value | minted at reserve → threaded through every protocol step | by-ref everywhere; 1 by-value re-materialization per publish call | mutex | core (index+generation) |
| `Request<T>` handle | 3 ptr-ish fields | user stack | context → user | 0 | user thread | borrow of core+backend |
| `Slot` (arena record) | 168 B (3 lines) | `std::vector<Slot>` arena, one mutex | lifecycle below | zeroed 2×/recycle | core mutex (all fields) | core |
| `RequestDescriptor` (in Slot @0x28, 32 B) | 32 B | slot | accept ← submit args → backend SQE build | 1 store + SQE field copies | mutex | core (backend caches its own SQE view) |
| `BorrowFacts` (in Slot @0x48, 24 B) | 24 B | slot | accept → **never read in production** | 1 store + zeroing | mutex | duplicated in backend `prepared_ops_` (live authority) |
| `IoOutcome` (in Slot @0x60, 32 B) | 32 B | slot | offer_terminal ← backend completion → publication, observe/consume | 1 store, 2 loads, 2 stack copies | mutex | core |
| `FileInfo` (in Slot @0x80, 40 B) | 40 B | slot | offer_terminal ← backend statx result (file_info/size ops only: `has_metadata` gate; zero-ops bypass offer_terminal) → publication, observe/consume | 1 store on metadata ops only, 2 loads, 2 stack copies; 40 B zeroing at reserve/release init sites on every op | mutex | core |
| `PublicationPayload` | 120 B | caller stack | backend publish form + begin_publication → publisher reads `.id` only | init (120 B) + copy-in (96 B) per op | none (local) | — (derived snapshot) |
| `RequestResultData` | 72 B | caller stack (`Request<T>`) | default-init + observe/consume fill → `result_of_` | init (72 B) + slot copy-in | none (local) | — |
| backend `prepared_ops_` entry | per in-flight op | backend map | accept → completion | 0 (in-place) | backend mutex | backend |
| `Result<T>` payload | ≤40 B | caller stack | `result_of_` ← RequestResultData | 1–2 moves/copies | user thread | — |
| W-01 blocking path | `Result<size_t>` | stack | pread → stores | 0 | none | file fd |

## Per-op lifecycle DAG (async uring D1, base arm)

```
submit_read(op)
  └─ core.reserve()            [mutex]  ~14 stores re-zeroing ~132 B of slot   (DP-3A)
  └─ core.accept(key,desc,borrow) [mutex] descriptor+borrow stores
  └─ backend: prepared_ops_[ud] = borrow view; SQE fields ← op
claim_execution (backend)       [mutex]  phase/refs
  └─ io_uring_enter(sqe)        — kernel
kernel completes CQE
poll() → backend reap
  └─ core.offer_terminal(key,outcome)   [mutex] 32 B outcome store (+40 B metadata if file_info/size)
  └─ backend.erase prepared_ops_
  └─ begin_publication(key,&payload)    [mutex]
        payload ← 24 B args + slot 32 B + slot 40 B                        (DP-1)
        caller 120 B payload init before the call                          (DP-1)
  └─ complete_publication(key)  [mutex]  phase
  └─ deliver_event(key)         [mutex]  observer_phase/flags
user try_result()/take_result()
  └─ observe/consume            [mutex]  72 B RequestResultData ← slot 32+40 B  (DP-2)
  └─ result_of_(data) → Result<T>       second copy of consumed fields           (DP-2)
  └─ core.discard/reclaim       [mutex]
  └─ release_slot_              [mutex]  16 stores, 3 lines                      (DP-3A)
```

Blocking W-01: `precheck → pread → Result<size_t>` stores. Nothing else.

## Where the bytes actually move per op (base, non-metadata `Result<size_t>`)

| movement | bytes | notes |
|---|---|---|
| slot zeroing (reserve) | ~132 B | re-zeroes what release zeroed |
| slot zeroing (release) | ~168 B | 16 stores, 3 lines |
| descriptor + borrow stores | 56 B | borrow write-only |
| outcome store + payload copy-in | 32+32 B | outcome copied twice |
| metadata load-side copy-in (payload + bridge) | 40×2 B | **unread by `size_t` consumers**; the slot-side producer store does not execute (`has_metadata` gate — erratum 2026-10-23); metadata zeroing is counted in the zeroing rows above |
| payload init | 120 B | pure dead weight |
| RequestResultData init + fill | 72+72 B | 40 B of it metadata |
| Result<T> materialization | 8 B used | from the 72 B |

The three candidates remove: DP-1 the 120 B init + 96 B copy-in + 72 B slot
re-loads; DP-2 the 40 B metadata round-trips and (dp2b) the 72 B local init;
DP-3A the ~132 B re-zeroing. All are user-space, mutex-bracketed, physically
executed work (gimple-sra-dse.md §2).
