# Buffered fast path design note

This is a **design placeholder**, not an implementation task. It captures the
fidelity gap between this C++ core and Zig `std.Io`'s streaming model, and lists
the options for closing it — without choosing or building any of them yet.

## Background: how Zig `std.Io` buffers

1. Zig's `std.Io.Reader` keeps buffer state **inside the reader interface**
   (`r.buffer`, `r.seek`, `r.end`). Every reader is inherently a buffered reader;
   there is no separate `BufferedReader` wrapper.
2. Because the buffer lives in the reader, Zig-style streaming
   (`Reader.stream`, `Reader.streamExact`) can **drain already-buffered bytes
   directly to a writer** without an extra copy through a separate scratch
   buffer. When data is already buffered, the writer reads from the reader's
   own buffer — a zero-copy fast path.

## Where this core stands today

3. This C++ phase deliberately uses **external** `BufferedReader` /
   `BufferedWriter` wrappers (approved by the task design table), keeping the
   base `Reader`/`Writer` abstractions unbuffered.
4. `copy_all` therefore always stages bytes through the caller-provided scratch
   buffer (`reader.read_some(scratch) → writer.write_all(scratch)`).
5. This is **correct** and matches the spec's bounded-copy contract, but it is
   **not** the full Zig fast-path model: wrapping a `BufferedReader` around the
   source buys nothing for `copy_all` today, because the buffered bytes are
   hidden behind the wrapper and the copy loop won't read them directly.

## Future options (deliberately not chosen)

None of these is implemented. The choice should be driven by **benchmark data**
from a later optimization phase, not by guesswork now:

- Expose `Reader::peek_buffered()` + `Reader::consume_buffered(n)` so a
  `BufferedReader` can hand its already-filled bytes to the copy loop without a
  scratch round-trip.
- Add an optional virtual `stream_to` fast path on `Reader` that a buffered
  reader can override to skip the scratch copy.
- Add a `BufferedReader`-specific fast path (non-virtual) that callers opt into.
- Add `readv` / `writev` (scatter-gather) support to reduce syscalls on the file
  backend — orthogonal but related.

## Non-goals for this phase

- No zero-copy buffered fast path is implemented yet.
- No `readv` / `writev` yet.
- No benchmark integration yet — the eventual choice must be data-driven.
