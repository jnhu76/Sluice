# DP-0 field-liveness matrix (R0) — required fields per result type, with counterfactuals

"Liveness" here is *required by the observable result or by a named
contract consumer* (conformance tests, `SlotObservation` test API, public
`Result<T>` semantics) — not merely "written". The matrix is the audit's
answer to "which bytes must exist per op type", and the two counterfactual
arms (dp2, dp2b) are its executable form.

## Required-field matrix by op/result type (six types)

Legend: ● required for the result value; ○ carried by protocol but unread by
any production/test consumer; ✓ written today.

| field group | W-01 blocking read | async read `size_t` | async write `size_t` | async zero-op `size_t` | async `FileInfo` | async `FileSize` |
|---|---|---|---|---|---|---|
| outcome.succeeded | ● (Result) | ● | ● | ● | ● | ● |
| outcome.error | ● | ● | ● | ● | ● | ● |
| outcome.effect.confirmed_bytes | ● | ● | ● | ● (unused value) | ○ | ○ |
| descriptor{op,offset,length,zero_op} | n/a | ✓ → SQE | ✓ → SQE | ✓ → SQE | ✓ → SQE | ✓ → SQE |
| borrow{fd,buffer,length} | n/a | ○ written, never read | ○ | ○ | ○ | ○ |
| metadata.kind | n/a | ○ | ○ | ○ | ● | ○ |
| metadata.size | n/a | ○ | ○ | ○ | ● | ● |
| metadata.identity | n/a | ○ | ○ | ○ | ● (conformance: device+inode) | ○ |
| PublicationPayload beyond id | n/a | ○ | ○ | ○ | ○ | ○ |
| RequestResultData.metadata | n/a | ○ | ○ | ○ | ● | ● |

Reading: for the four `size_t`-carried forms (read/write/zero-op and the
blocking read), the 40-byte `FileInfo` and the payload body are **required by
nothing**. `FileSize` reads 8 of the 40 metadata bytes. Only `FileInfo`
results consume metadata wholly (and `identity` only because the conformance
suite asserts kernel device+inode).

Erratum (2026-10-23, second review): this paragraph originally said the
metadata is "written three times each (slot store, payload copy, result
copy)" per `size_t` op. The slot-store member of that claim is wrong
against baseline source: `offer_terminal`'s metadata store is gated on
`candidate.has_metadata`, set only for `file_info`/`size` completions
(`request_core.cpp`, `uring_completion.cpp`, `threadpool_backend.cpp`), and
zero-ops are made terminal at `accept()` and never reach
`offer_terminal`. What every `size_t`-carrying op does execute on base:
the publication payload copy (DP-1), the result-bridge copy (DP-2,
removed for non-metadata T by dp2b), and 40 B of `Slot::metadata`
zeroing at the `reserve()`/`release_slot_()` initialization sites.
The required-by-nothing conclusion above is unchanged.

## Counterfactual A (dp2 arm): runtime flag gates the metadata copy

`observe/consume(core, id, &data, include_metadata)` with
`include_metadata = (T is FileInfo || FileSize)`. Executable form of the
matrix column split: `size_t` forms skip the 40 B copy; metadata forms keep
it. **Measured**: instruction-neutral on zop (−12.1 instr/op, 0/15 pairs —
the saved copy costs less than the added flag plumbing and the unchanged
72 B local init). Oracle: forcing the flag false for metadata types fails
4 conformance assertions (`results/receipts/m1-dp2-mutation-failures.txt`) —
the matrix's ● entries are load-bearing and test-protected.

## Counterfactual B (dp2b arm): type-directed overload split

`observe/consume(core, id, bool*, IoError*, EffectReport*)` for non-metadata
T — no `RequestResultData` exists at all on those paths (72 B init gone,
40 B copies gone); metadata types keep the original overload. **Measured**:
zop −8.8 instr/op (14/15). Oracle: misrouting metadata types to the split
overload is a **compile error** in every consumer TU
(`results/receipts/m1-dp2b-mutation-summary.txt`) — the matrix is enforced
by the type system in this shape.

## Residual facts the matrix exposes (not implemented; recorded for the gate)

1. `BorrowFacts` is dead in production for every op type (authority lives in
   the backend). 24 B store + 24 B zeroing per op. Removal is a
   mechanism-justification question (the `SlotObservation` test API reads it
   and cancellation paths may consult it) — an authority-contraction audit
   item, not a measured candidate.
2. `PublicationPayload` beyond `.id` is dead for every op type in the inline
   publish paths — DP-1's finding; the `out != nullptr` publication form
   remains for any caller that needs the snapshot (none found in production
   code; the parameter is retained in dp1).
3. `outcome.effect.confirmed_bytes` is carried into `FileInfo`/`FileSize`
   results without being read — harmless (32 B slot field) but the matrix
   marks it ○ for those columns; a future specialized file_info publication
   (corrected DP-3C in dp3-slot-layout.md) need not write it, and must not
   drop the metadata write for `size` ops: `FileSize` requires
   `metadata.size`, so any op-kind gate must be `file_info || size`, not
   `file_info` alone.
4. `FileSize` could be served by an 8-byte carrier instead of the 40-byte
   metadata copy; not implemented (would add a third overload for one type —
   cost/benefit recorded as negative at current metadata volumes).
