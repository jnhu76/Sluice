# Copy strategy layer

**Status: implemented in CPPIO-CORE-007A–007G.** The strategy boundary is now
explicit and observable: Auto / Scratch / BufferedFirst are implemented, the
four `*Deferred` strategies are reserved slots that report as unsupported, and
`CopyDecision` / `CopyStats` record what ran. This is not a performance claim.

## 1. Scope

> CPPIO-CORE-006 added an implicit buffered fast path inside `copy_all`.
> CPPIO-CORE-007 makes copy path selection explicit and testable.

After 006, `copy_all` had at least two real paths (scratch, buffered fast path)
plus several future/adjacent paths (vector write, file range copy, sendfile,
splice, io_uring) that were either implicit or absent. 007 creates the strategy
boundary so path selection stops being a pile of hidden heuristics and becomes
explicit, testable, and observable:

```
CopyOptions
  → CopyStrategy selection
  → copy_all executes selected existing path
  → CopyDecision / CopyStats explain what happened
```

This task **must not** implement new kernel paths. It only routes the existing
paths through an explicit selection layer.

## 2. Why a strategy layer is needed

- **Testability.** Today the buffered fast path is triggered implicitly by a
  `dynamic_cast` inside `copy_all`. A strategy enum makes it possible to force
  the scratch path (the pre-006 behavior) or force buffered-first, so each path
  can be tested in isolation.
- **Observability.** `CopyDecision` records which strategy was requested, which
  was actually selected, and why (fallback, unsupported, etc.). `CopyStats`
  counts strategy selections. This is the observability CPPIO-CORE-010 will
  consume.
- **Forward compatibility.** Future strategies (vector copy, kernel zero-copy)
  can be added as reserved enum slots that are explicitly *deferred*, returning
  `invalid_state` (or falling back to Auto) rather than silently no-oping or
  pretending to work.

## 3. Existing copy paths

| Path | Where | Used by |
|---|---|---|
| Scratch | `read_some(scratch) → write_all(scratch)` loop | classic `copy_all` (pre-006), non-buffered readers |
| Buffered fast path | drain `peek_buffered()`/`consume_buffered()` | `BufferedReader` readers (006) |

Both are preserved unchanged by 007; they just become explicit strategy choices.

## 4. Strategy enum

```cpp
enum class CopyStrategy {
    Auto,            // default; currently == BufferedFirst
    Scratch,         // force the scratch read/write loop; never use fast path
    BufferedFirst,   // drain buffered bytes first, then scratch (006 behavior)

    VectorDeferred,        // reserved slot; NOT implemented
    FileRangeDeferred,     // reserved slot; NOT implemented
    SendfileDeferred,      // reserved slot; NOT implemented
    SpliceDeferred,        // reserved slot; NOT implemented
};
```

This stage implements only `Auto`, `Scratch`, `BufferedFirst`. The four
`*Deferred` strategies are reserved slots that explicitly report as
unsupported; they do not pretend to work (see §6).

## 5. Auto strategy behavior

For now:

```
CopyStrategy::Auto == CopyStrategy::BufferedFirst
```

because 006 made buffered-first the default behavior. `Auto` is its own enum
value (not an alias) so the default can change later without touching callers.
When the default changes, callers that explicitly want buffered-first should
request `BufferedFirst` directly. This equivalence is documented and tested.

`Auto` does **not** add file-range/sendfile heuristics this stage. Any future
"pick the best path" intelligence goes here, behind measurement (010).

## 6. Unsupported / deferred strategies

```cpp
enum class UnsupportedStrategyPolicy {
    ReturnInvalidState,   // default: return invalid_state, touch nothing
    FallbackToAuto,       // mark unsupported, then run Auto
};
```

For a `*Deferred` strategy:

- `ReturnInvalidState` (default): return `invalid_state`, do **not** touch the
  reader or writer, set `CopyDecision.unsupported_requested = true`.
- `FallbackToAuto`: set `unsupported_requested = true`, set `selected = Auto`
  (which runs as BufferedFirst this stage), set `reason` to explain the
  fallback, then execute Auto behavior.

> `VectorDeferred` does not mean vector copy exists. It is a reserved strategy
> slot. Same for `FileRangeDeferred`, `SendfileDeferred`, `SpliceDeferred`.

## 7. Relation to Zig `std.Io`

Zig `std.Io` does **not** have an explicit copy-strategy enum. Path selection
lives inside the Reader/Writer/vtable model: `Reader.stream` chooses behavior
based on what the concrete reader/writer expose through their vtable. cppio's
base `Reader`/`Writer` are minimal and unbuffered, so 007 introduces an
**explicit external strategy layer** instead. This is an intentional C++ design
divergence: cppio prefers an observable, caller-chosen strategy over an
implicit, vtable-negotiated one. See
`docs/zig-std-io-gap-calibration.md`.

## 8. Measurement and no-performance-claim rule

`CopyDecision` and the `CopyStats` strategy counters (007F) are observability
hooks — *which strategy was selected? which path moved bytes?* — not
throughput/latency numbers. **No benchmark exists yet** (CPPIO-CORE-010). The
relative cost of scratch vs buffered-first vs (future) vector copy is
unmeasured. 007 must not claim one strategy is faster than another.

## 9. Deferred future work

- **Generic vector copy strategy** — `read_vec`/`write_vec` exist (005) but are
  not yet wired into a copy strategy.
- **Kernel zero-copy** — sendfile, splice, copy_file_range. Reserved as slots,
  not implemented. Require a capability boundary (009).
- **io_uring** — requires the 004–011 preconditions.
- **Async backend** — out of scope for the blocking core.
- **`Auto` heuristics** — once 010 measures paths, `Auto` may learn to pick
  between them. Not before.

Zig `std.Io` remains a **design reference only**; not a dependency, no code
copied.
