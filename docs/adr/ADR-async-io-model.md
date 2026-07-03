# ADR: Async I/O model for sluice

**Status: sluice-CORE-016D.** Architecture Decision Record.
**State: Accepted (design only — no code in this job).**
**Implementation: DEFERRED behind the sync-first readiness gate** (`docs/sync-before-async-readiness-gate.md`, added in the 016G sync-first planning patch). The async **model decision** below is unchanged; only the start of async **coding** waits until the blocking baseline is engineered (jobs 017S–023S, `docs/sync-io-next-jobs.md`). See `docs/sync-io-model-gap-audit.md` for why.
**Decides:** the async model sluice will adopt, its public API shape, its backend
boundary, and what is explicitly deferred.

- Supersedes nothing; sits above `docs/io-uring-readiness-gate.md` (012D, the
  *spike* gate) and `docs/io-uring-spike.md` (013, the experiment).
- Rests on `docs/async-source-inventory.md` (016A),
  `docs/async-problem-statement.md` (016B), and
  `docs/async-design-alternatives.md` (016C).
- Preconditions before any code: `docs/async-readiness-gate.md` (016E).
- Implementation split: `docs/async-next-jobs.md` (016F).

This ADR makes **one** recommendation. It introduces **no dependency** without
explicit evaluation (§11). It makes **no universal performance claim**.

---

## 1. Context

sluice v0.1 is blocking-first and release-ready. The experimental io_uring spike
(013) proved the kernel seam but is **synchronous-over-uring** — it blocks the
caller per op, so it cannot express the multi-stream, few-thread workloads async
is for (016B W1–W4). Production io_uring promotion is blocked because sluice has
**no async runtime, no cancellation model, and no repeated liburing measurement**
(016A §4). This ADR designs the async model *before* code is added, per the
job's design-first boundary.

Hard boundaries (non-negotiable):

```text
Do not change the default backend (BlockingIoContext stays default).
Do not plug io_uring into BlockingIoContext.
Do not implement production async in this job.
Do not add networking / timers / process APIs / mmap / group commit.
Do not make universal performance claims.
Do not break existing Reader/Writer semantics.
Do not introduce a dependency unless this ADR explicitly evaluates it.
```

## 2. Decision (accepted)

Adopt a **completion-based async model**, backend-agnostic at the core,
structured as **three layers**. This job accepts only the **low-level foundation
API (L1)**; the ergonomic end-user API (L2) — including coroutines — is
explicitly deferred.

```text
   L2  AsyncReader / AsyncWriter / coroutine wrapper        DEFERRED ergonomic public
       (built on L1)                                         (later, separate ADR/job)
                              ▲
   L1  Completion<T> + AsyncIoContext                        ACCEPTED low-level foundation
       submit_read/write/sync_data/sync_all; poll; wait_one  (public, power-user surface)
                              ▲
   L0  AsyncBackend interface                                INTERNAL seam
       submit / poll / wait_one / cancel                     (never public-facing)
       ──────────────────────────────────────────────────────
            ┌──────────────────┬───────────────────┬──────────────────────┐
   backends│ FakeAsyncBackend │ ThreadPoolBackend │ UringAsyncBackend    │
            │ (deterministic)  │ (fallback)        │ (gated, validated    │
            │                  │                   │  separately)         │
            └──────────────────┴───────────────────┴──────────────────────┘
```

| Layer | Name | Status | Audience |
|---|---|---|---|
| **L0** | `AsyncBackend` | internal | backend implementers |
| **L1** | `Completion<T>` + `AsyncIoContext` | **ACCEPTED** as the *low-level async foundation API* | tests, the L2 layer, advanced callers needing control |
| **L2** | `AsyncReader`/`AsyncWriter`/coroutine wrapper | **DEFERRED** | ordinary end users |

> The API this ADR accepts is the **low-level async foundation API (L1)**, not
> the final ergonomic public API. L1 is intentionally a power-user surface:
> explicit completion objects, explicit `poll`/`wait_one`, explicit ordering
> composition. This is consistent with 016C, which rates the pollable completion
> queue "excellent as a foundation, too low-level as the public API": it is the
> right *foundation*, and L2 is the future *public* shape that hides it. L2 is
> out of scope until L1 is proven by jobs 017–019.

**In one sentence:** the **foundation** is *explicit completion objects over a
pollable/awaitable completion queue* (Option 2 from 016C, borrowing Zig's
caller-provided `Completion` idea — 016A §3); io_uring is one backend for it; a
thread-pool fallback (Option 5) provides portability; a deterministic fake
provides testability (016B W5). **Coroutines (Option 3) are deferred** as part
of the L2 layer — not the foundation, not now.

### Why this and not a coroutine-first model

Coroutines (016C Option 3) give the prettiest caller code but combine the worst
default buffer-lifetime safety (a coroutine can suspend holding a reference to a
caller buffer that then goes out of scope) with the largest up-front runtime cost
(allocator, awaitable machinery, executor, tooling/ABI assumptions). Building
async on coroutines *first* would force a big-bang runtime and bake in a
discipline-only lifetime contract. By making the completion object explicit,
buffer lifetime becomes a *visible, auditable* relationship (the completion owns
references to the buffers for exactly as long as the op is outstanding), and the
runtime can grow in small jobs (016F). Coroutines can later wrap this layer with
no change to the foundation.

## 3. Low-level foundation API shape (L1)

Sketch (illustrative — final names/signatures are a 017/018/018B implementation
detail, **not** committed here):

```cpp
namespace sluice::async {

// A single outstanding operation's state, caller-provided so allocation is
// decoupled from submit (mirrors Zig std.Io Completion — 016A §3).
template <class T> class Completion;          // T = size_t for read/write/sync

// Op descriptors: buffers/result + an EXPLICIT offset (positional I/O — §6 P1).
// read/write carry an offset; sync ops carry no buffer (§: W4 durability).
class ReadOp;      // { handle; span<byte>       dst; uint64_t offset; }
class WriteOp;     // { handle; span<const byte> src; uint64_t offset; }
class SyncDataOp;  // { handle; }                    // fdatasync (W4)
class SyncAllOp;   // { handle; }                    // fsync     (W4)

class AsyncIoContext {                        // the L1 foundation (parallels IoContext)
public:
    // Submit an op into the caller-owned Completion. Does NOT block.
    // Returns void on success; submit-time errors (queue full, op invalid,
    // Completion not ready — §5 L8) return SYNCHRONOUSLY as Result<void>.
    // The caller already holds the Completion, so no reference is returned.
    Result<void> submit_read (ReadOp      op, Completion<size_t>& storage);
    Result<void> submit_write(WriteOp     op, Completion<size_t>& storage);
    Result<void> submit_sync_data(SyncDataOp op, Completion<void>& storage);
    Result<void> submit_sync_all (SyncAllOp  op, Completion<void>& storage);

    // Non-blocking: reap only completions that are immediately ready.
    size_t poll();

    // Block the driver thread until at least one completion is ready, then reap.
    // (No deadline — timers/time-based completion are out of scope, 016B O2.)
    Result<size_t> wait_one();

    // (Cancellation is §7 — minimal first, fuller later via job 021.)
};

}  // namespace sluice::async
```

Two reaping primitives instead of one `drain`, because a real runtime needs both
a non-blocking probe and a blocking wait, and `drain(/* optional deadline */)`
would smuggle in a timer API this job excludes (016B O2):

```text
poll()              non-blocking; reaps immediately-ready completions only.
                    Returns the count reaped. Never blocks. (Fake: caller drives.)
wait_one()          blocking; waits the driver thread until >=1 ready, then reaps.
                    Returns the count reaped or a backend error. No deadline.
```

Properties the API **must** have (each is a gate item in 016E):

```text
A1. Completion<T> is an explicit, caller-owned object (not a heap future).
A2. submit_* does not block; it records the op as outstanding against its buffer.
    submit_* returns Result<void>: submit-time errors are synchronous (§8 E5).
A3. Completions are reaped ONLY inside poll()/wait_one() (single reaping family
    => ordering is defined, §6).
A4. Completion-side errors flow through Completion<T>::result() as Result<T>/
    IoError (no exceptions, §8); submit-side errors flow through submit_*'s
    Result<void>.
A5. The API is file-I/O only: read/write/sync_data/sync_all. No net/timer/
    process/mmap surface (016B O1–O5).
A6. The API is opt-in and lives in namespace sluice::async; BlockingIoContext
    and Reader/Writer are untouched.
A7. Read/Write primitives are POSITIONAL by default (carry an explicit offset) —
    see §6 P1. Implicit-shared-file-position ops are deferred.
```

## 4. Internal backend boundary

A new internal interface (not public-facing; in `detail/` or `async::detail::`)
decouples the L1 shape from how completions are produced:

```text
AsyncBackend {
    submit_read/write/sync_*(Op, Completion&)   // hand op to the backend
    poll()                       -> size_t       // non-blocking reap
    wait_one()                   -> Result<size_t> // blocking reap (>=1 ready)
    cancel(Completion&)                         // §7
}
```

Concrete backends (each independently abortable):

- **FakeAsyncBackend** (016F job 019): completions produced on demand; drives W5
  deterministic tests. No kernel, no threads. Ships first.
- **ThreadPoolBackend** (016F job 020A): wraps the existing blocking
  `read_some`/`write_some`/`sync_*` on worker threads; the portable fallback
  where io_uring is absent. Reuses `Result<T>`/`IoError` verbatim.
- **UringAsyncBackend** (016F job 020B): real async io_uring — many SQEs in
  flight, CQEs reaped in `poll`/`wait_one`. Reuses the build gate from 013
  (`SLUICE_HAS_LIBURING`).
  **Validated separately** under `docs/io-uring-liburing-validation.md` (014C);
  its promotion has its own abort conditions.

This mirrors how sluice already keeps the POSIX backend behind `IoContext`: the
*use* of a handle is backend-agnostic; the *choice* of backend is centralized.
Async does the same one layer up.

## 5. Ownership and buffer lifetime rules

The single most important behavioral change vs blocking. Stated as rules so they
are testable (016E gate item 1). L1–L6 cover the core; **L7–L11 (added in the
016 review patch) cover the `Completion` lifecycle edge cases that cause
use-after-free if left implicit.**

```text
L1. A buffer passed to submit_read/submit_write is BORROWED for the lifetime of
    the outstanding op, not just the submit call.
L2. "Outstanding" spans from successful submit to the moment poll()/wait_one()
    marks the Completion ready (or the op is cancelled, §7).
L3. (refined below in L3a/L3b) — what the caller may/may not do with the buffer
    while outstanding, split by op direction.
L4. Completion<T> is caller-owned; the runtime never allocates a Completion.
L5. fds/handles are NOT owned by the async layer (caller owns open/close), same
    as the 013 spike (016A §1, §2). See L10 for the outstanding-op corollary.
L6. AsyncIoContext/AsyncBackend own the submission machinery; they are move-only
    and non-copyable, matching sluice's move-only file handles.
```

### L3a/L3b — buffer access while outstanding (split by op direction)

The blanket "use after submit and before completion is UNDEFINED" is too coarse:
read-destination and write-source buffers have *different* permitted access. The
refined, testable contract:

```text
L3a. WriteOp (source buffer): the source memory region must remain ALIVE and
     BYTE-STABLE until the Completion is ready. The caller may inspect the bytes
     read-only, but MUST NOT free, reallocate, move, MODIFY, or reuse the region
     as a write target. (Modification could race the in-flight write.)
L3b. ReadOp (destination buffer): the destination memory region must remain ALIVE
     and EXCLUSIVELY OWNED by the operation until the Completion is ready. The
     caller MUST NOT free, reallocate, move, reuse, or read the region as if it
     were result data. (Contents are undefined until ready.)
L3c. Both: the region's ADDRESS must be stable for the op's lifetime.
```

### L7–L11 — Completion and handle lifecycle (review patch)

```text
L7.  A Completion<T> is ADDRESS-STABLE while outstanding. It MUST NOT be moved,
     destroyed, or reused (re-submitted) until it is ready. (Moving an
     outstanding Completion would invalidate the backend's pointer to it.)
L8.  Submitting into a Completion that is not ready (still outstanding, or
     already used and not reset) returns IoError::invalid_state synchronously
     from submit_* (it does not silently overwrite).
L9.  Calling Completion::result() before the Completion is ready is a contract
     violation: debug-mode assertion failure; release-mode behavior returns
     IoError::invalid_state (never returns stale/garbage as if it were a result).
L10. The fd/handle referenced by an op MUST remain valid until the Completion is
     ready. Closing a handle with outstanding ops referencing it is a caller
     contract violation (the op would touch a closed/invalid descriptor).
L11. Destroying an AsyncIoContext that still has outstanding Completions is a
     contract violation: debug-mode assertion failure; release-mode
     invalid_state. Destruction does NOT implicitly cancel or drain — silent
     teardown of in-flight ops would hide bugs.
```

Enforcement strategy (incremental):

```text
- Debug-mode assertions for L7–L11 (outstanding-when-destroyed, result-before-
  ready, submit-into-non-ready, context-destroyed-with-outstanding).
- The fake backend (job 019) explicitly tests "buffer reused after completion is
  fine; buffer reused while outstanding is a contract violation", and "outstanding
  Completion destroyed/moved is caught".
- ASan/UBSan runs (the project already has sanitizer modes — xmake.lua) are the
  primary tool for catching real lifetime bugs in benches/examples.
- Registered io_uring buffers (which pin kernel state) are DEFERRED: the
  UringAsyncBackend starts with plain SQE buffers and only considers registered
  buffers after a lifetime+teardown contract is added in a later job.
```

## 6. Completion model and ordering

```text
O1. Completions are produced only inside poll()/wait_one() (single reaping family).
O2. Within one reap, completions are surfaced in the order their backends
    report them ready; no global FIFO across backends is promised.
O3. Per-backend completion order:
      Fake       = submit order (deterministic; test-controllable on demand)
      ThreadPool = unspecified (worker-thread race)
      Uring      = CQE reap order ONLY. No semantic FIFO is promised across
                   independent SQEs — INCLUDING SQEs targeting the same fd —
                   unless the caller explicitly composes ordering by waiting,
                   chaining, or a future ordered-submission feature (link/drain
                   SQE flags). Do NOT assume same-fd write completion FIFO.
O4. There is NO implicit global ordering across independent ops on independent
    fds (or even on the same fd for Uring). Callers needing ordering compose it
    (e.g. wait_one the previous Completion before submitting the next), exactly
    as with blocking.
O5. Partial progress: a read/write Completion<size_t> reports bytes transferred
    (may be short), mirroring read_some/write_some. An "all" variant (loop until
    complete) is a derived helper, not a primitive — same factoring as blocking.
```

### P1 — Positional I/O is the default

```text
P1. Async file read/write primitives are POSITIONAL by default: ReadOp/WriteOp
    carry an explicit offset (pread/pwrite style). This is what makes W1/W2
    ("many files OR many offsets") expressible without ordering hazards.
P2. Implicit shared-file-position async ops (no offset, shared fd cursor) are
    DEFERRED: multiple async ops sharing one implicit fd cursor introduce
    ordering and state races that positional I/O avoids by construction.
P3. SyncDataOp/SyncAllOp carry no buffer and no offset (they operate on the whole
    file behind the handle); their ordering vs in-flight writes is governed by O3
    and by explicit caller composition (await the writes, then submit the sync).
```

These are stated now so cancellation (§7), the durability ops (§3, W4), and
tests (§10) have a defined basis.

## 7. Cancellation model (minimal first)

Cancellation is required (016B C6) but a full model is deferred to job 021. The
**minimal** semantics this ADR commits to:

```text
X1. IoError::Code::canceled already exists (016A §1) — reuse it; no new vocab.
X2. cancel(completion) requests cancellation of one outstanding op.
X3. Cancellation is BEST-EFFORT and ASYNCHRONOUS: the op may still complete
    successfully, may complete with an error, or may complete with
    IoError::canceled. The Completion is marked ready in poll()/wait_one()
    EXACTLY ONCE (no double-completion).
X4. Once cancel is requested, the caller must still reap the Completion (via
    poll/wait_one) to observe the final result; cancel does not synchronously
    free the buffer.
X5. Buffer lifetime (L1–L3c) holds until the Completion is ready, INCLUDING the
    cancellation path.
X6. Deferred to job 021 / later: structured cancel protection (Zig
    CancelProtection — 016A §3), group/batch cancel, and io_uring
    IORING_OP_ASYNC_CANCEL race handling. None of these are in scope for the
    foundation (017–019). NOTE: job 021 lands BEFORE the io_uring backend
    (020B) so that cancel-race handling exists before a backend that needs it —
    see 016F ordering.
```

The minimal model is "you can request cancel; you will get a defined terminal
result; buffers stay safe." That is enough to make async correct; the richer
model is layered later.

## 8. Error model

```text
E1. No exceptions for I/O results. Errors flow as Result<T>/IoError, identical
    vocabulary to blocking (016A §1).
E2. A Completion<T>::result() is Result<T>: the bytes transferred (read/write),
    or an IoError (eof, canceled, no_space, backend_error, …).
E3. errno->IoError mapping reuses the existing from_errno_value (016A §1), so a
    UringAsyncBackend's CQE res maps to the same codes a blocking write would.
E4. Partial-progress errors propagate immediately even after partial bytes
    (mirrors write_all/read_exact semantics — 016A §1).
E5. Submit-time errors (e.g. queue full, invalid op, submit-into-non-ready
    Completion — §5 L8) are returned from submit_* synchronously as Result<void>,
    not deferred to the Completion.
```

## 9. Coexistence

### 9a. With blocking Reader/Writer (hard: must not break)

```text
- Reader/Writer, FileReader/FileWriter, BlockingIoContext: UNCHANGED.
- Async lives in namespace sluice::async and adds new types; it edits no
  existing header's behavior.
- The blocking tests stay green verbatim (016E gate item 9; count per 016E §3).
- Reader/Writer semantics (read_some 0==EOF, write_some 0-on-non-empty==failure,
  vector stop-on-short) are the contract async's derived helpers must honor.
```

### 9b. With experimental io_uring (013)

```text
- The 013 spike (UringWriteBatch/UringIoContext, synchronous-over-uring) STAYS
  in namespace sluice::experimental, unchanged. It is not deleted and not
  promoted in this job.
- UringAsyncBackend (job 020B) is NEW code under the async seam; it does not
  replace the spike. The spike remains a reference point and a comparison row.
- ThreadPoolBackend (job 020A) is likewise new code under the seam — the
  portable fallback. Neither 020A nor 020B touches the 013 spike.
- Neither async nor the spike is plugged into BlockingIoContext.
- io_uring promotion to production async has its OWN validation + abort
  conditions (014C, 016E gate item 7); async is otherwise backend-agnostic.
```

## 10. Test strategy

```text
T1. FakeAsyncBackend (job 019) drives all async logic deterministically: it is
    the primary unit-test vehicle (no kernel, no threads, no flakiness).
T2. ThreadPoolBackend tests (job 020A) run real blocking ops asynchronously;
    used to prove the seam, not to assert performance.
T3. UringAsyncBackend tests (job 020B) are build-gated (SLUICE_HAS_LIBURING) and
    SKIP CLEAN without liburing, exactly like the 013 spike tests.
T4. Buffer-lifetime + Completion-lifecycle contract tests (fake): verify
    "completion-ready => buffer reusable", "outstanding => buffer in use",
    "outstanding Completion moved/destroyed => caught" (§5 L7–L11), and
    "result() before ready => invalid_state" (L9).
T5. Cancellation tests (job 021): cancel before completion, cancel after
    completion (no-op), cancel during a long op (fake-controlled timing). These
    land BEFORE 020B so cancel semantics exist before the uring backend.
T6. Ordering tests assert O1–O5 (incl. the conservative Uring CQE-order-only
    rule, O3) against each backend where defined.
T7. Positional-I/O tests: independent ReadOp/WriteOp on the same fd at different
    offsets complete independently (no implicit cursor coupling) — §6 P1.
T8. Blocking regression: the existing tests run unchanged (count: see 016E §3).
T9. Sanitizer coverage: async benches/examples run under ASan/UBSan/TSan modes
    (xmake.lua already defines them) to catch lifetime/race bugs.
```

## 10b. Observability — AsyncStats

Async needs its own measurement struct, following sluice's existing rule
(caller-owned, nullable, never global — 016A §1). Without it, job 022's benches
have nothing to count and the foundation is unobservable.

```cpp
struct AsyncStats {                 // lives in namespace sluice (like UringStats)
    std::uint64_t submit_calls        = 0;
    std::uint64_t submitted_ops       = 0;
    std::uint64_t poll_calls          = 0;
    std::uint64_t wait_calls          = 0;
    std::uint64_t completed_ops       = 0;
    std::uint64_t canceled_ops        = 0;
    std::uint64_t completion_errors   = 0;
    std::uint64_t short_completions   = 0;  // completions with fewer bytes than requested
    std::uint64_t max_outstanding     = 0;  // high-water mark of in-flight ops
    std::uint64_t queue_full_retries  = 0;  // submit rejected then retried (L8 path)
};
```

```text
- Attached to AsyncIoContext via a nullable pointer (set_stats), exactly like
  SyscallStats/UringStats. Null = no counting, zero cost.
- Caller-owned storage; must outlive the context. Mirrors UringStats (016A §1).
- Defined now so 017 can wire it and 022 can read it; no separate "stats job".
```

## 11. Dependencies (explicit evaluation)

```text
D1. liburing                — OPTIONAL, build-gated (SLUICE_HAS_LIBURING), reused
                              from 013. Required ONLY for UringAsyncBackend (job
                              020B). Default build: zero new dependency. EVALUATED: accept.
D2. C++20 coroutines         — NOT USED now. If/when the deferred coroutine
                              layer lands, the compiler/ABI/tooling requirement
                              is evaluated in its own ADR/job. EVALUATED: defer.
D3. std::execution (P2300)   — NOT USED. Rejected in 016C as too heavy / not
                              widely available / Reader/Writer-incompatible.
                              EVALUATED: reject.
D4. A thread-pool lib        — NOT USED. The fallback backend uses std::thread
                              / the existing primitives only. EVALUATED: reject
                              external dep; use std.
D5. Anything else            — NONE introduced by this ADR.
```

No dependency enters without an explicit row here.

## 12. Risks

```text
R1. Buffer-lifetime bugs    — the core async risk. Mitigation: explicit
                              Completion ownership + L7–L11 lifecycle (§5),
                              fake-backend contract tests (T4), sanitizer runs
                              (T9). Residual: real.
R2. Cancellation races      — cancel vs in-flight completion. Mitigation:
                              minimal "exactly-once terminal result" semantics
                              (§7); job 021 lands BEFORE 020B so full race
                              handling exists before the uring backend needs it.
R3. io_uring kernel variance— feature/version differences. Mitigation: build
                              gate + runtime probe (reused from 013); UringBackend
                              (020B) validated separately (014C).
R4. Coroutine deferral      — callers may want ergonomic coroutines now.
                              Mitigation: L1 is poll/wait-able today; coroutine
                              wrapper (L2) is additive later.
R5. Scope creep             — async drifts into a general runtime. Mitigation:
                              016B O1–O10 are explicit out-of-scope; API is
                              file-I/O only (§3 A5).
R6. Performance regressions — async complicates the blocking comparison.
                              Mitigation: blocking baseline is frozen (016B §5);
                              async benches are separate (job 022); no universal
                              claim (016B C9).
R7. API churn               — the sketch in §3 is illustrative; early jobs may
                              reshape it. Mitigation: live behind sluice::async
                              and a seam; no caller depends until 017–019 settle.
R8. Implicit-ordering bugs  — callers wrongly assume same-fd write FIFO on
                              uring. Mitigation: O3 explicitly promises CQE reap
                              order ONLY (no per-fd FIFO); P1 makes ops
                              positional so ordering is caller-composed.
```

## 13. Migration plan

Revised ordering after the 016 review patch (019 before 018 so the op model has
a deterministic test vehicle; 021 before 020B so cancellation exists before the
backend that needs cancel-race handling):

```text
Phase 0 (this job, 016): DESIGN ONLY. Docs 016A–016F land; no code; tests green.
Phase 1 (017):  Completion<T> + AsyncIoContext + AsyncBackend interface (L0/L1
                skeleton) + AsyncStats. No real backend, no coroutine, no uring.
Phase 2 (019):  FakeAsyncBackend — deterministic test vehicle. (Before 018 so the
                op model can be tested against it.)
Phase 3 (018):  ReadOp/WriteOp + derived "all" helpers, positional by default (P1).
Phase 4 (018B): SyncDataOp/SyncAllOp — overlapped durability (W4).
Phase 5 (020A): ThreadPoolBackend — portable fallback behind the seam.
Phase 6 (021):  Cancellation spike — flesh out §7 toward structured cancel.
Phase 7 (020B): UringAsyncBackend prototype behind SLUICE_HAS_LIBURING; measured
                against blocking and against the 013 spike.
Phase 8 (022):  Async bench harness; workload-specific evidence only.
Phase 9 (later, separate ADR): L2 ergonomic API (AsyncReader/AsyncWriter +
                optional coroutine wrapper) on top of the proven L1 foundation.
```

Each phase is independently abortable (§15). No phase changes the default
backend or touches BlockingIoContext. The fake (019) is reached before any real
backend, so all async *logic* is testable with no kernel and no threads.

## 14. What remains experimental / what is deferred

```text
EXPERIMENTAL (stays behind a gate):
  - UringAsyncBackend (job 020B) — gated, validated separately, not default.
  - ThreadPoolBackend (job 020A) — opt-in, not default (a real fallback, not
    experimental in the uring sense, but never auto-selected).
  - The 013 synchronous-over-uring spike — unchanged, still experimental.

DEFERRED (not in this ADR's scope):
  - L2 ergonomic API (AsyncReader/AsyncWriter + coroutines) — later ADR (Phase 9).
  - Structured cancellation / cancel protect  — job 021.
  - Group/batch cancel + ordering             — job 021+.
  - Registered io_uring buffers/files         — later (lifetime contract first).
  - Implicit-shared-file-position async ops   — deferred (P2); positional only.
  - Networking/timers/process/mmap/group commit — out of scope (016B O1–O5).
  - Universal performance claims              — forbidden (016B C9).
```

## 15. Abort conditions

Stop and fall back to blocking-only (revert the async work, keep 016 docs as a
record) if **any** of these occurs:

```text
AB1. The default (no-liburing) build breaks.
AB2. Any blocking test changes behavior or fails (the full suite, count per 016E §3).
AB3. BlockingIoContext or Reader/Writer semantics change.
AB4. Buffer-lifetime / Completion-lifecycle discipline (§5 L1–L11) cannot be made
     testable/clean (would require breaking Reader/Writer ownership — 012D §7).
AB5. Cancellation "exactly-once terminal result" (§7) cannot be made correct.
AB6. The async API cannot be introduced without a global scheduler (016E gate 6).
AB7. io_uring async cannot be made a clean optional backend (validation 014C
     fails) AND no other backend is viable — then async stays on the fake/thread
     pool and io_uring promotion is abandoned.
AB8. liburing becomes a required dependency for the default build.
AB9. The work expands scope into networking/timers/mmap/group commit (016B O1–O5).
AB10. Positional-I/O (P1) cannot be enforced, leaving same-fd async ops racing
     on an implicit cursor.
```

## 16. Decision summary

```text
ACCEPTED   : Completion-based async (L1 low-level foundation API) over a
             poll/await completion queue, behind an AsyncBackend (L0) seam.
             File-I/O only, opt-in, additive. L2 ergonomic API DEFERRED.
OPS        : ReadOp/WriteOp (positional, P1) + SyncDataOp/SyncAllOp (W4).
BACKENDS   : FakeAsyncBackend (tests, job 019, first), ThreadPoolBackend
             (fallback, job 020A), UringAsyncBackend (gated, job 020B).
REAP       : poll() non-blocking + wait_one() blocking. No drain()/deadline.
SUBMIT     : submit_* returns Result<void> (submit-time errors synchronous).
LIFETIME   : buffer L1–L3c (read-dst vs write-src) + Completion L7–L11.
ORDERING   : no implicit FIFO, not even per-fd on uring (O3); caller-composed.
COROUTINES : DEFERRED to L2 (Phase 9, separate ADR) — not the foundation.
DEFAULT    : UNCHANGED — BlockingIoContext stays default; Reader/Writer untouched.
CANCEL     : MINIMAL now (exactly-once terminal result via IoError::canceled);
             structured model in job 021 (BEFORE 020B).
OBSERVE    : AsyncStats, caller-owned/nullable (§10b), wired in 017.
DEPS       : liburing optional (job 020B only); nothing else added.
```

## 17. Cross-links

- ⛔ **Sync-first readiness gate (blocks implementation of this ADR):** `../sync-before-async-readiness-gate.md` (016G). The async *decision* here is accepted; async *coding* is deferred until that gate is GREEN.
- Sync gap audit (why implementation is deferred): `../sync-io-model-gap-audit.md` (016G).
- Sync-first job cards (must complete before async jobs): `../sync-io-next-jobs.md` (017S–023S).
- Inventory: `../async-source-inventory.md` (016A).
- Problem statement: `../async-problem-statement.md` (016B).
- Alternatives: `../async-design-alternatives.md` (016C).
- Readiness gate (the async-side gate, after the sync-first gate): `../async-readiness-gate.md` (016E).
- Next jobs (blocked behind the sync-first gate): `../async-next-jobs.md` (016F).
- io_uring spike: `../io-uring-spike.md` (013).
- io_uring spike readiness gate (prior): `../io-uring-readiness-gate.md` (012D).
- liburing validation runbook: `../io-uring-liburing-validation.md` (014C).
- IoContext (the blocking seam this parallels): `../design-io-context.md` (009).
