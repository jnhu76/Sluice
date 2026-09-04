# COPY-X0-AUDIT — as-built Copy semantics audit (G1-Control Candidate 2)

- Campaign: COPY-X0 (working name), G1-Control Candidate 2 under #227 / #259
- Baseline: `master` = `ecd84259` (merge of PR #280, fixed-file C0 closure)
- Audit date: 2026-09-03
- Host (execution environment): WSL2, kernel `6.18.33.2-microsoft-standard-WSL2`,
  AMD Ryzen 7 5800H, 8 logical CPUs; substrates actually available: tmpfs
  (`/tmp`, 7.9 GiB) and ext4 (`/dev/sdd` at `/`, 848 GiB free). **No btrfs on
  this host.**
- Method: direct source read at the baseline SHA (no inference from names,
  docs, or history); public contract cross-checked against
  `docs/reference/api.md`. Kernel mechanism facts from the local
  `copy_file_range(2)` man page (version-bound, see §6) plus Context7/man7
  cross-check.

This audit recovers the real current semantics of every Copy-adjacent surface.
It does NOT freeze the campaign semantic floor (that is the preregistration's
job) and does NOT authorize any production change.

Classification vocabulary used throughout (goal §5):

```text
PRIMITIVE CONTRACT            — promised semantics of read/write primitives
COMPOSED COPY CONTRACT        — promised semantics of a composed copy operation
IMPLEMENTATION DETAIL         — real but not promised; may change
CURRENT ACCIDENTAL BEHAVIOR   — observable today, promised by nobody
UNKNOWN                       — not determinable from source; needs a probe
```

---

# 1. Inventory of Copy surfaces at `ecd84259`

Three distinct Copy-adjacent surfaces exist. None of them calls a kernel
transfer mechanism today.

| # | Surface | Files | Mechanism |
|---|---------|-------|-----------|
| C1 | Sync core `sluice::copy_all` | `include/sluice/copy.hpp`, `src/copy.cpp`, `include/sluice/copy_strategy.hpp`, `src/copy_strategy.cpp` | userspace scratch loop over abstract `Reader`/`Writer` |
| C2 | `sluice-copy` application | `apps/sluice-copy/*` (esp. `copy_task.cpp`) | fd-level async positional pipelined read/write via Runtime + backend |
| C3 | Primitive file I/O | `include/sluice/file.hpp`, `src/file.cpp`, `src/reader.cpp` | `read`/`pread`/`readv`, `write`/`pwrite`/`writev`, `fsync`/`fdatasync` |

Production call sites for `copy_file_range` / `sendfile` / `splice`: **ZERO**.
`SendfileDeferred` / `SpliceDeferred` / `FileRangeDeferred` exist only as
reserved enum values (§4).

Prior application-level copy evidence: COPY-AB-1 (`research/tax0/TAX0-COPY-AB1.md`,
issue #257) — router A/B on the real sluice-copy engine; wall throughput NOT
material, instructions/byte material only in capacity-skewed regimes; measured
on Host-0 (Fedora 44, kernel 7.1.9, Xeon E5-2666 v3) — **a different host from
the COPY-X0 execution host**. Chunk-size evidence CHUNK-E0
(`research/chunk-e0/CHUNK-E0-H0-REPORT.md`, #270) is likewise HOST-LOCAL to
Host-0 (sweet region ≥128 KiB chunk, d1 knee 384 KiB, plateau entry 768 KiB at
d8; MAD ≤ 1.1% for cells ≥64 KiB). Neither set of numbers may be imported as
a "fair best value" on the WSL2 host without a host-local qualification —
handled in the preregistration.

---

# 2. C3 — PRIMITIVE CONTRACT (Reader / Writer / file backend)

These are the semantics COPY-X0's rule (goal §7) declares **rigid**: primitive
`read`/`write` are NOT transformation grants. Recorded here because the Copy
semantic floor must not silently demand more (or less) than the primitives
promise.

## 2.1 `Reader` / `Writer` interfaces (`include/sluice/reader.hpp`, `include/sluice/writer.hpp`)

- `Reader::read_some(dst)` — short reads allowed; `0` means EOF (clean stop,
  not an error). `Writer::write_all(src)` — all-or-error; loops `write_some`
  across short writes; **zero progress on a non-empty write is a backend
  failure** (`invalid_state`), never an infinite retry (writer.hpp:21).
- Vector variants `read_vec` / `write_vec` / `read_vec_all` / `write_all_vec`
  with stop-on-short + skip-empty semantics (measurement.hpp:111-120 records
  the fallback split).
- `BufferedReadable` capability interface (peek_buffered / consume_buffered)
  exists solely for `copy_all`'s buffered fast path (§3).

## 2.2 `FileReader` / `FileWriter` (`include/sluice/file.hpp`)

- Positional family does NOT change the shared file offset:
  `read_at` = `pread` (file.hpp:92-95), `read_vec_at` = `preadv`,
  `read_at_exact` = loop-to-exact (file.hpp:100-103); `write_at` = `pwrite`
  (file.hpp:174-178), `write_vec_at` = `pwritev`, `write_at_all` = loop across
  short writes, zero-progress = error (file.hpp:183-186).
- Non-positional `read_some`/`write_all` advance the shared file offset
  (`read`/`write`).
- Durability: `SyncableWriter::sync_data()` (fdatasync) and `sync_all()`
  (fsync) are distinct; `flush()` is NOT durability.
- EINTR authority: `src/file.cpp` — all eligible POSIX syscalls route through
  `detail::retry_on_eintr` (file.cpp:34, 75-76, 640); there is exactly one
  retry loop, not per-caller copies.

Classification: all of §2 is PRIMITIVE CONTRACT (documented in
`docs/reference/api.md` and sync-io-model.md).

---

# 3. C1 — COMPOSED COPY CONTRACT: sync `copy_all`

Source of truth: `src/copy.cpp:28-232` (strategy-aware overload); convenience
overloads delegate (copy.cpp:234-251). Public docs `docs/reference/api.md:136-204`.

## 3.1 Promised semantics (from code + api.md)

| Dimension | As-built behavior | Classification |
|---|---|---|
| Return value | `Result<uint64_t>` = total bytes moved on success | COMPOSED COPY CONTRACT |
| Bytes accounting | `total` accumulates only bytes that were fully written (`write_all` returned) | COMPOSED COPY CONTRACT |
| Loop shape | buffered fast path first (if strategy allows + reader implements `BufferedReadable`), else scratch: `read_some(scratch[0..min(scratch,left)])` → `write_all(got)`; repeat | COMPOSED COPY CONTRACT (shape) |
| EOF | `read_some` returning 0 → clean stop, return `total` (eof_stops) | COMPOSED COPY CONTRACT |
| Limit | `CopyLimit::bytes(n)`: copy at most n; **EOF before n is success**; `nothing()`/`bytes(0)` → immediate success 0 without touching endpoints (copy.cpp:107-111) | COMPOSED COPY CONTRACT (limit.hpp:9-11) |
| Reader error | propagate the `IoError` immediately; **partial byte count is lost** (error result carries no progress) | COMPOSED COPY CONTRACT |
| Writer error | propagate immediately; in the buffered fast path **nothing is consumed** on write failure (copy.cpp:147-155) | COMPOSED COPY CONTRACT |
| Zero-progress write | impossible to observe from copy_all (write_all all-or-error); a no-progress non-empty write inside write_all fails as backend failure | INHERITED PRIMITIVE CONTRACT |
| Scratch | empty scratch + non-zero/unlimited copy → `invalid_state` (copy.cpp:118-119) | COMPOSED COPY CONTRACT |
| Offsets | shared file offsets of FileReader/FileWriter advance (non-positional read/write); no offset parameters exist on this surface | COMPOSED COPY CONTRACT (absence) |
| Strategy selection | explicit `CopyOptions.strategy`; `Auto` currently == `BufferedFirst` (documented "may change after measurement", copy.cpp:66-74) | COMPOSED COPY CONTRACT |
| Capability probe | `dynamic_cast<BufferedReadable*>` once, only when fast path allowed (copy.cpp:124) | IMPLEMENTATION DETAIL (explicitly guarded by strategy) |
| Default scratch | 8 KiB internal array (copy.cpp:245) | IMPLEMENTATION DETAIL |
| Defensive reader check | reader returning more than asked → `invalid_state` (copy.cpp:203-209) | CURRENT ACCIDENTAL BEHAVIOR (defensive) |
| Stats | `CopyStats` counters: loop iterations, bytes, stop reasons, fast/scratch split, strategy selection counters (measurement.hpp:51-80) | IMPLEMENTATION DETAIL (observability, caller-owned) |
| Decision | `CopyDecision{requested, selected, reason, used_buffered_fast_path, used_scratch_path, unsupported_requested}` (copy_strategy.hpp:50-57) | COMPOSED COPY CONTRACT (the answerable-decision surface) |
| Durability | none — copy_all performs no sync of any kind | COMPOSED COPY CONTRACT (absence) |
| Cancellation | none — sync loop, no cancel observation | COMPOSED COPY CONTRACT (absence) |

## 3.2 Structural finding F-1 (load-bearing for COPY-X0)

`copy_all` operates on abstract `Reader&`/`Writer&`. `copy_file_range` requires
two **file descriptors** with regular-file semantics. Therefore the current
composed surface **cannot express** a kernel-transfer mechanism without
either (a) a new fd-level composed Copy operation, (b) a capability probe
(like `BufferedReadable`, e.g. an fd-reveal interface), or (c) breaking the
abstraction. The fd-level composed copy that exists today is the sluice-copy
application (§4), which is app code, not library surface.

## 3.3 Structural finding F-3

On any error, `copy_all` discards the partial byte count (the `Result` error
carries only `IoError`). A Copy contract that wants progress-on-error
accounting must declare it explicitly; the application surface (§4.2) does
keep `bytes_copied` in its stats but returns the error without stats on the
failure path too (copy_task.cpp:369-379).

---

# 4. CopyStrategy reserved slots — the designed-but-empty place

`include/sluice/copy_strategy.hpp:22-31`:

```cpp
enum class CopyStrategy {
    Auto, BufferedFirst, Scratch,
    VectorDeferred,    // reserved slot; NOT implemented
    FileRangeDeferred, // reserved slot; NOT implemented  <-- this campaign's slot
    SendfileDeferred,  // reserved slot; NOT implemented
    SpliceDeferred,    // reserved slot; NOT implemented
};
```

`UnsupportedStrategyPolicy` (copy_strategy.hpp:34-37): `ReturnInvalidState`
(default — return `invalid_state`, touch nothing) or `FallbackToAuto` (mark
unsupported, then run Auto).

## 4.1 Fallback precedent (load-bearing for goal §14)

The fallback path is **observable by construction**, not silent:
`CopyDecision.unsupported_requested = true`, `reason =
"deferred_fallback_to_auto"`, `strategy_deferred_fallback_calls++`
(copy.cpp:45-53). The reject path likewise (`reason =
"deferred_not_implemented"`, `strategy_deferred_rejected_calls++`,
copy.cpp:55-63). So the current design precedent is: **fallback is legal only
when explicitly requested AND explicitly reported**. There is no `automatic`
hidden-policy mode today, and `Auto` resolves to exactly one named strategy.

## 4.2 Finding F-2

`FileRangeDeferred` is a reserved slot whose name anticipates exactly the
mechanism under study. COPY-X0's research prototype (B3) is the semantics
this slot would need to honestly declare; the slot's existence is design
intent evidence, not implementation.

---

# 5. C2 — COMPOSED COPY CONTRACT: `sluice-copy` application

Source: `apps/sluice-copy/copy_task.cpp` (Version B pipelined task; Version A
= depth 1). This is an application, not a public library surface; it is the
as-built fd-level composed Copy.

| Dimension | As-built behavior | Classification |
|---|---|---|
| Object | `run_pipelined_copy(src_fd, dst_fd, buffer_size, pipeline_depth, workers, SyncPolicy)` — whole-file identity copy | app COMPOSED COPY CONTRACT |
| Offsets | fully positional: `ReadOp{fd,dst,len,off}` / `WriteOp{fd,src,len,off}`; chunk grid `i*buffer_size`; shared file offsets untouched | app COMPOSED COPY CONTRACT |
| Partial reads | retried within slot via `await_read_fill` (copy_task.cpp:156-171); a partial tail is DATA, then EOF flag | app contract |
| Partial writes | `await_write_exact` retries remainder (copy_task.cpp:193-203); **zero progress with data remaining = deterministic `backend_error`** (copy_task.cpp:184-189) | app contract |
| EOF | `read == 0` for a chunk; empty (pure-EOF) slots retired without writing (copy_task.cpp:294-297) | app contract |
| Write ordering | strictly ascending chunk offsets; at most one write outstanding (copy_task.cpp:272-291) | app contract |
| Cancellation | observed at cooperative boundaries via `ctx.cancel_token()` → terminal `canceled` (copy_task.cpp:226-230, 245-248) | app contract |
| Errors | first meaningful error wins; Phase-3 drains outstanding completions; secondary errors discarded (copy_task.cpp:213-222, 353-379) | app contract |
| Overflow | chunk-offset advance checked (`add_would_overflow`) → `invalid_state` (copy_task.cpp:317-321) | app contract |
| Durability | `SyncPolicy`: none / data (`fdatasync`) / all (`fsync`), post-data, clean path only (copy_task.cpp:381-400) | app contract |
| Shutdown | all Completions idle before context teardown; sync slot released by `reset()` (copy_task.cpp:395-400) | app contract |
| Resource bounds | explicit caps `kMaxBufferSize`/`kMaxPipelineDepth`/`kMaxWorkers`/`kMaxPipelineBytes` + overflow product check before any allocation (copy_task.cpp:434-446) | app contract |
| Stats | `sluice_copy::CopyStats{read_ops, write_ops, short_writes, bytes_copied, sync}` | IMPLEMENTATION DETAIL |

## 5.1 Finding F-4

The application surface already embodies a defensible Copy semantic floor for
fd-level identity copy: positional explicit offsets, partial-progress loops
with deterministic zero-progress failure, first-error-wins, explicit
durability policy, bounded resources, quiescent shutdown. A B3 research
prototype does not need to invent a floor; it needs to **declare which of
these rows survive under a kernel-transfer mechanism**.

---

# 6. Kernel mechanism semantics — `copy_file_range(2)`

Source: local man page on the execution host + man7/Context7 cross-check.
Version-bound: behavior described is for the man page as shipped on
kernel 6.18-era systems; host kernel here is 6.18.33.2 (WSL2), i.e. post-5.19
semantics apply. glibc: since 2.30 the wrapper has NO userspace fallback
(ENOSYS if kernel lacks support) — no hidden mechanism substitution from libc.

| Dimension | Mechanism behavior | Note |
|---|---|---|
| Signature | `ssize_t copy_file_range(fd_in, off_in*, fd_out, off_out*, len, flags=0)` | flags must be 0, else EINVAL |
| Offsets | NULL off_* → current file offsets used AND advanced; non-NULL → positional, file offsets untouched, off_* updated by bytes moved | positional mode available |
| Return | bytes copied on success (may be < len: partial); 0 at EOF-of-source (with off_in at end); -1/errno on error | partial-progress protocol exists |
| Max per call | kernel caps per-call length (MAX_RW_COUNT, ~2 GiB) — verify empirically (probe) | UNKNOWN on host until probed |
| Regular files only | EINVAL otherwise; EISDIR for directories | no pipes/sockets — splice is the different mechanism there |
| Same file | legal only if ranges do not overlap; overlap → EINVAL | aliasing rule |
| O_APPEND dest | EBADF | append-mode destinations rejected |
| Sparse source | **may expand holes** (man NOTES: users may need SEEK_DATA/SEEK_HOLE loop) | S8 load-bearing |
| Cross-filesystem | since 5.19: allowed when both filesystems same type AND support it; else EXDEV. EOPNOTSUPP (5.19+) when filesystem lacks the op. 5.3–5.18 had a generic kernel path that could **report success without copying on some virtual filesystems** (BUGS) | S5/S6 load-bearing; 6.18 host should not hit the 5.3–5.18 bug, must be probed |
| Durability | none — no sync implied; data may be merely in page cache | S10 |
| Reflink | mechanism may legitimately implement copy acceleration (reflink/server-side copy) — observable differences possible in allocation/extents | S8-adjacent |

UNKNOWN (must be probed empirically on this host, not assumed):
- whether tmpfs and ext4 on WSL2 6.18 support `copy_file_range` at all
  (EOPNOTSUPP/ENOSYS possibility), same-fs and cross-fs;
- actual per-call cap value;
- EOF return shape at exact end-of-file (0 vs short);
- cross-fs tmpfs→ext4 and ext4→tmpfs disposition (EXDEV vs copy).

These unknowns are exactly what semantic fixtures S5/S6 exist to resolve; the
preregistration freezes the fixtures BEFORE probing.

---

# 7. Semantic-dimension classification summary (goal §5/§6 rows)

| Goal dimension | Sync `copy_all` | sluice-copy app | `copy_file_range` |
|---|---|---|---|
| bytes copied | returned total (success only) | stats.bytes_copied + success | return value per call |
| source offset advancement | shared offset advances (no positional mode) | explicit positional, untouched | positional mode: untouched (non-NULL) |
| destination offset advancement | same | explicit positional | same |
| EOF | read_some==0 → success | read==0 → chunk retire, success | 0 at source EOF (to verify) |
| partial progress | hidden inside write_all / read_some retry loops | explicit retry, counted (short_writes) | partial return is first-class |
| zero progress | impossible via write_all (fails) | deterministic backend_error | UNKNOWN (0 without error possible?) — S4 |
| source error | propagate, lose partial count | first-error-wins, drain | errno |
| destination error | propagate, consume nothing (buffered path) | first-error-wins, drain | errno (ENOSPC/EFBIG/EPERM) |
| unsupported mechanism | deferred slots → invalid_state / explicit fallback | n/a | EOPNOTSUPP/ENOSYS/EXDEV — S5/S6 |
| cross-filesystem | invisible (Reader/Writer) | invisible (works, or read/write errors) | first-class error or legal copy — S6 |
| destination existing contents | overwrite from current offset | overwrite per positional writes | overwrites requested range (man) |
| aliasing | n/a (abstraction) | n/a (app assumes distinct fds) | same-file non-overlap legal; overlap EINVAL |
| cancellation | none | cooperative canceled | not interruptible per se — OUT unless composed loop declares |
| durability | none | explicit none/data/all | none |
| shutdown/lifetime | n/a (sync) | quiescent drain contract | n/a (sync call) |
| sparse source | transparent (bytes copied; holes read as zeros) | same | **may expand holes** — declared difference candidate |

# 8. Findings list (carried into preregistration)

- **F-1**: `copy_all`'s `Reader&`/`Writer&` abstraction cannot express
  `copy_file_range`; the only fd-level composed copy today is app code.
- **F-2**: `FileRangeDeferred` reserved slot + observable-fallback precedent
  (`UnsupportedStrategyPolicy`) already exist — B3's shape is
  "fill a reserved slot", not "add a framework".
- **F-3**: `copy_all` errors discard partial progress counts.
- **F-4**: sluice-copy app already fixes a credible fd-level Copy floor
  (positional, partial-progress loops, zero-progress failure, first-error,
  explicit durability, bounded resources, quiescent shutdown).
- **F-5**: zero kernel-transfer call sites in production; no hidden policy
  anywhere; `Auto` resolves to exactly one named strategy.
- **F-6**: chunk-size and prior copy A/B evidence are HOST-LOCAL to a
  different host; the WSL2 host requires its own B0 chunk qualification
  before any formal comparison (goal §10 fairness).
- **F-7**: host substrate reality is tmpfs + ext4 (no btrfs); the C0
  substrate-identity failure mode is the standing precedent for fail-closed
  substrate gates (PR #280 Corrective-2).
- **F-8**: sparse-hole expansion and cross-fs disposition are the two
  mechanism behaviors most likely to break honest equivalence; both are
  UNKNOWN on this host until probed.

# 9. What this audit does NOT establish

- No performance claim of any kind (no measurement performed).
- No kernel-support claim on the WSL2 host (unprobed).
- No semantic-floor freeze (preregistration's job).
- No production-code authorization: COPY-X0 is research-only.
