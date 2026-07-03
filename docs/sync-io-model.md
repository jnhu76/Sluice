# Sync I/O model — blocking primitive contract

**Status: SYNC-IO-COMPLETE Phase 3 (sync doc reconciliation).** This is the
*contract* layer: the blocking primitive semantics stated as testable
propositions. It is the authoritative behavioral contract for the synchronous I/O
surface. Existing behavior (v0.1) is recorded as-is; new positional helpers
(`read_at`/`write_at`, job 018S) and derived-helper closeout (job 019S) are marked
**[NEW]** where they extend the surface. Implementation follows the code sessions
(phases 4–5); this doc fixes the contract first.

Companion docs: architecture in `docs/sync-io-architecture.md`; durability in
`docs/sync-durability-model.md`; planning/gaps in `docs/sync-io-model-gap-audit.md`.

## Reader semantics

```text
read_some(dst) -> Result<size_t>
  * Returns the number of bytes read into dst (<= dst.size()).
  * A return of 0 with no error means EOF.
  * Short reads are allowed (0 < n < dst.size()); callers wanting all bytes use
    read_exact.
  * dst is caller-owned; it only needs to outlive the call (blocking returns
    before the caller may reuse the buffer).
  * Errors propagate via Result (IoError); no exceptions for I/O results.
```

Derived: `read_exact(dst) -> Result<void>` — loops until dst is full, or fails on
EOF/error. EOF before any byte is a defined outcome (`IoError::eof`); EOF after
partial bytes is also `eof` (partial progress is abandoned, mirroring the existing
`read_exact` contract).

## Writer semantics

```text
write_some(src) -> Result<size_t>
  * Returns the number of bytes written from src (<= src.size()).
  * Short writes are allowed; callers wanting all bytes use write_all.
  * A return of 0 on NON-EMPTY input is invalid_state / backend failure (zero
    progress is rejected, not silently retried at this layer).
  * src is caller-owned; it only needs to outlive the call.
  * Errors propagate via Result (IoError); no exceptions for I/O results.
```

Derived: `write_all(src) -> Result<void>` — retries short writes until all of src
is written or an error occurs. Zero progress on non-empty remaining input yields
`IoError::invalid_state`.

## Vector I/O semantics

```text
read_vec(dsts)  / read_vec_all(dsts)
write_vec(srcs) / write_all_vec(srcs)
  * Empty slices are skipped (never cause a syscall).
  * Slices are processed strictly in order.
  * stop-on-short: read_vec/write_vec STOP after the first short result (positive
    short or clean EOF) and return total bytes so far. This is the conservative
    vector semantics (NOT read_exact over slices).
  * *_all variants retry the SAME slice on a short result until it is satisfied
    (write_all_vec) or fill every byte (read_vec_all), advancing across slices by
    bytes successfully transferred.
  * zero-progress: on non-empty remaining input, the *_all helpers treat zero
    progress as invalid_state (mirrors write_some's rule).
  * partial-progress error: an error propagates immediately even after partial
    bytes have been transferred.
```

## Positional I/O semantics  **[NEW — job 018S]**

The blocking surface gains positional forms so "many offsets on one fd" is
expressible without a shared cursor (mirrors async ADR P1; closes gap G1/G2):

```text
read_at(offset, dst)      -> Result<size_t>   // pread
write_at(offset, src)     -> Result<size_t>   // pwrite
read_vec_at(offset, dsts) -> Result<size_t>   // preadv
write_vec_at(offset, srcs)-> Result<size_t>   // pwritev
  * explicit offset; NO mutation of any shared file cursor.
  * short read/write allowed (same as read_some/write_some).
  * read_at returning 0 means EOF at that offset.
  * write_at returning 0 on non-empty input is invalid_state / backend failure.
  * vector-at advances across slices by bytes successfully transferred (stop-on-
    short for the vec variants; *_all variants retry — see "Derived positional
    helpers" below).
  * errno mapping reuses existing IoError logic (from_errno_value) and the shared
    detail::retry_on_eintr helper (no new retry loop).
```

**Contract rule:** positional ops do **not** require shared cursor state. Two
`read_at`/`write_at` calls on the same fd at different offsets are independent —
no cursor coupling. This is the blocking analogue of async positional
independence (ADR 016D §6 P1).

## Derived positional helpers  **[NEW — job 019S]**

```text
read_at_exact(offset, dst) -> Result<void>    // loop until dst full or EOF/error
write_at_all(offset, src)  -> Result<void>     // loop on short positional writes,
                                               // advancing offset by bytes written
```

Semantics:

```text
  * write_at_all loops on short positional writes and advances the offset by the
    bytes written each iteration.
  * zero progress on non-empty remaining input => IoError::invalid_state.
  * read_at_exact loops until full or EOF/error; EOF before any byte and EOF
    after partial bytes are both defined (eof), matching read_exact.
```

**Deferred (documented, not ambiguous):** `read_vec_at_all` / `write_vec_at_all`
(positional vector-all). If implementing them is too invasive, single-buffer
positional helpers (`read_at_exact`/`write_at_all`) ship first and vector-at-all
is documented as deferred. The semantics will not be left ambiguous either way —
the decision is recorded in job 019S.

## File handle semantics

```text
  * move-only RAII (FileReader/FileWriter are non-copyable, move-constructible).
  * open/close ownership: the handle owns its fd and closes it on destruction.
  * fd adoption ctor (FileReader(int fd)/FileWriter(int fd)) takes ownership and
    will close on destruction; pass -1 for an empty handle.
  * no implicit sharing guarantee unless documented: two handles on the same fd
    are not specially coordinated.
  * error mapping: open failures preserve the real errno (surfaced via open_error()
    / IoContext at open time); I/O failures map through from_errno_value.
  * direct constructor vs IoContext: the direct FileReader/FileWriter constructors
    defer open errors to first I/O; IoContext surfaces open errors at open time.
```

## Measurement semantics

```text
  * stats structs (SyscallStats/BufferStats/CopyStats/SyncStats/VectorStats/
    UringStats) are caller-owned.
  * the stats pointer may be null.
  * null means no counting (the wired-in call sites guard on null before bumping).
  * never global: the core holds raw optional pointers; callers keep the storage
    alive for the reader/writer/copy operation's lifetime.
```

This rule applies unchanged to any new stats (e.g. a future multi-stream/
concurrency field for benchmarks, gap G6 — still caller-owned, nullable, never
global).

## Compatibility rule

```text
Future async helpers must mirror blocking Reader/Writer observable semantics
where they expose analogous read/write/all/vector/sync helpers.

Concretely: async read/write ops reuse IoError vocabulary, honor the same
partial-progress / EOF / zero-progress rules, and keep stats caller-owned.
The async surface is additive (new types in sluice::async); it does not change
these blocking contracts.
```

## Cross-links

- Architecture: `docs/sync-io-architecture.md`.
- Durability contract: `docs/sync-durability-model.md`.
- Gaps this contract closes (G1/G2/G3): `docs/sync-io-model-gap-audit.md` §2.
- Jobs that realize the [NEW] pieces: `docs/sync-io-next-jobs.md` (018S positional, 019S helpers).
- Existing readv/writev note: `docs/design-readv-writev.md`.
- Async compatibility target: `docs/adr/ADR-async-io-model.md` (016D).
