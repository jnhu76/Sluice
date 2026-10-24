# R1 — Source and Authority Audit

Baseline: `3df10dc44d84799cccb18d29f20149d2983137ec`. All line numbers refer
to that tree. Full-reference inventory was established by
`grep -rn` over tracked `*.cpp`/`*.hpp`; every read and write of each counter
is listed below — there are no others.

## Arm TP — `ThreadPoolBackend::syscall_count_`

### Reference inventory (complete)

| Kind | Site | Content |
|---|---|---|
| Field declaration | `include/sluice/async/detail/threadpool_backend.hpp:239` | `std::atomic<std::uint64_t> syscall_count_{0};` — **unguarded**, present in production layout, placed after `workers_` and before the `SLUICE_ASYNC_INTERNAL_TESTING` gate block |
| Sole write (RMW) | `src/async/threadpool_backend.cpp:431` | `syscall_count_.fetch_add(1, std::memory_order_relaxed);` |
| Sole read | `src/async/threadpool_test_seams.hpp:102` | `syscall_count_for_test()` body: `return syscall_count_.load();` (seq_cst load) |
| Getter declaration | `threadpool_backend.hpp:72` | inside `#if defined(SLUICE_ASYNC_INTERNAL_TESTING)` block |

Test assertion sites (16 total):
`tests/request_scope_test.cpp:885,990,1133,1189`;
`tests/threadpool_request_core_integration_test.cpp:265,289,380,422,463,487,493,522,569,943`;
`tests/public_request_test.cpp:313,414`.

### Call DAG (as required by the campaign brief)

```
worker_loop()                                   threadpool_backend.cpp:383
  └─ wait(work_cv_) → claim slot under work_mtx_
  └─ core_->claim_execution(key)                :403   (fail → retired_unclaimed, NO syscall, NO increment)
  └─ op = prepared_ops_[slot]; ++active_workers_ :406-407
  └─ run_syscall(op, &metadata)                 :429   (the actual pread/pwrite/fsync/fdatasync/fstat — blocking)
  └─ syscall_count_.fetch_add(1, relaxed)       :431   ← TARGET
  └─ { lock work_mtx_; --active_workers_; }     :432-435
  └─ core_->offer_terminal(physical_outcome)    :451   terminal selection
  └─ core_->release_execution(key)              :454   execution release
  └─ publication_pending_.push_back(h)          :458-461 publication
  └─ signal_ready_progress()                    :463   progress notification
```

The counter sits between the physical syscall and the `active_workers_`
retirement. `active_workers_` itself (a plain `std::size_t` mutated under
`work_mtx_`) is protocol state — it feeds `internal_work_retired()`
(`:835`) and shutdown drain (`:865`) and is **not** part of this candidate.

### Triggering conditions and frequency

- Incremented exactly once per **completed physical blocking syscall on a
  worker thread after a successful `claim_execution`** — read, write,
  sync_data, sync_all, file_info, size all funnel through `run_syscall()`.
- Not incremented for: zero-length requests (`publish_zero_op_inline`,
  `threadpool_backend.cpp:341` region — no worker, no syscall), cancellation
  before the claim (`retired_unclaimed`), admission/dispatch failures,
  publication/delivery work, shutdown drain activity.
- Production frequency = number of physical ops: W-02 4 KiB × 20 000 requests
  → 20 000 relaxed RMWs per measured repetition (plus warmup), issued by
  W1/W4/W8 worker threads respectively.

### Verified non-influence (each audited at source level)

| Concern | Finding |
|---|---|
| Worker scheduling / wakeup | `work_cv_` waits only on `stopping_`/`dispatch_` — counter never read |
| Cancellation | `cancel_key` path never reads the counter; cancel-before-claim produces no increment by construction |
| Shutdown / drain | `stop_execution`/`retire_execution_resources` read `active_workers_`, `dispatch_`, `publication_pending_` — not the counter |
| RequestCore authority | claim/offer_terminal/release_execution are driven by `outcome`/`candidate`, not the counter |
| Error propagation | `run_syscall` outcome carries errors; counter value unused |
| Physical-operation accounting in production | No production reader exists (sole reader is `syscall_count_for_test`) |
| Publication | `publication_pending_`/`publish_one` never touch it |
| Progress/liveness | `signal_ready_progress` independent of it |
| Public statistics | `AsyncStats` (see below) never reads it |

The RMW is `relaxed`: it provides no ordering, no fencing and no
synchronization that the following `work_mtx_` lock does not already provide.
Its removal cannot change any interleaving.

**Classification (Mechanism Justification Gate):** derived summary /
test-only evidence. It represents the fact "a physical syscall completed",
which is independently established by the worker reaching
`offer_terminal(physical_outcome)`. Owning authority for that fact is the
RequestCore terminal state, not the counter. It materializes a test-only
summary of an already-owned fact; in a production build (no reader) it is
unjustified runtime state.

## Arm UR — `UringAsyncBackend::submit_flushes_`

### Reference inventory (complete)

| Kind | Site | Content |
|---|---|---|
| Field declaration | `include/sluice/async/detail/uring_backend.hpp:324` | `std::atomic<std::uint64_t> submit_flushes_{0};` — **unguarded**, in the tail atomics block together with `live_cookies_`/`live_control_sqes_` |
| Sole write (RMW) | `src/async/uring_transport.cpp:222` | `submit_flushes_.fetch_add(1, std::memory_order_relaxed);` |
| Sole read | `src/async/uring_test_seams.hpp:75-76` | `submit_flushes_for_test()`: relaxed load |
| Getter declaration | `uring_backend.hpp:96` | inside `#if defined(SLUICE_HAS_LIBURING) && defined(SLUICE_ASYNC_INTERNAL_TESTING)` |

Test assertion sites (6 total):
`tests/uring_request_core_integration_test.cpp:406,755,768,904`;
`tests/shutdown_lifecycle_test.cpp:1138,1156`.

### Call DAG

```
submit paths (submit_read/…/submit_size, cancel, retry)
  └─ under dispatch_mtx_ ("*_locked" methods)
     ├─ dispatch_after_accept() epilogue        uring_transport.cpp:87   [flush after preparing the batch]  ← common hot path
     ├─ dispatch_one_locked()                  :143  [SQ full → flush to free SQ slots, retry get_sqe]
     ├─ cancel-control preparation (issue_running_cancel_locked_) :647  [flush before preparing a control SQE]
     └─ poll progress pass                      uring_backend.cpp:631  [flush before reap_cqes]

submit_transport_locked()                       uring_transport.cpp:218
  ├─ early-out: fatal_error_ present OR ledger null/empty → return 0 (NO increment)
  ├─ submit_flushes_.fetch_add(1, relaxed)      :222   ← TARGET
  ├─ io_uring_submit(&ring)  (or test hook)     :224-232
  └─ account_transport_result_locked(rc, …)     :233   ledger pop / control mark / retry / poison
```

### What the counter precisely represents

**Transport flush attempts with a non-empty transport ledger**: calls that
pass the early-out and therefore actually invoke `io_uring_submit` while SQEs
are pending. It is **not** any of:

- successful submit calls (`rc > 0`) — failures and retryable results also count;
- accepted SQEs (`rc` value) — a flush consuming N SQEs counts 1;
- `io_uring_enter` invocations — `io_uring_submit` may loop enter internally;
- submitted requests — cancel-control flushes also count.

The existing tests use it as "did (any) transport flush happen" (zero vs
non-zero, before/after deltas) — consistent with flush-attempt semantics. No
test asserts an exact positive total, so no test depends on the finer
distinctions above; the audit records them to prevent future conflation.

### Verified non-influence

All four callers run under `dispatch_mtx_`; every one discards the return
value except through `fatal_error_`/ledger side effects that are independent
of the counter. SQE submission counts, ledger accounting, retry behavior,
poison/recovery, CQE delivery and cancellation control processing read state
that excludes `submit_flushes_`. Shutdown quiescence
(`uring_backend.cpp:350-372`) reads `live_cookies_`/`live_control_sqes_` —
sibling atomics that ARE protocol state and are **not** part of this
candidate; the counter is not part of that accounting.

Production frequency: at minimum once per submit that leaves pending SQEs,
plus SQ-full refills and poll-pass flushes. W-02 4 KiB D1: ≈ 20 000 per
measured repetition.

**Classification:** derived summary / test-only evidence. The fact "a
transport flush was attempted" is independently established by
`account_transport_result_locked` consuming the ledger against kernel state.
Owning authority: the transport ledger + kernel SQ ring. In a production
build the counter has no reader.

## Public accounting separation

- `include/sluice/measurement.hpp` defines the public opt-in accounting
  (`SyscallStats`, `AsyncStats`, `UringStats`, …). `AsyncStats` is wired
  through the context factories (`make_threadpool_context(config, stats*)`,
  `make_uring_context(config, stats*)`) and is incremented in
  `src/async/async_io_context.cpp` (`tally_submit`, `poll_calls`, …) and
  backend sites (`canceled_ops`, `completion_errors` in
  `uring_transport.cpp`). **No public stats field reads either internal
  counter.** `UringStats` is a legacy core-era struct that the async sources
  never populate; it is unrelated to `submit_flushes_`.
- Stats-enabled and stats-disabled factories differ only in the passed
  pointer; the counters exist in both.
- Installed headers (`xmake/libraries.lua` Appendix-B allowlist): the detail
  headers that declare these fields are never installed. Public headers
  (`async/threadpool_backend.hpp`, `async/uring_backend.hpp`) include only
  `async_io_context.hpp`/`result.hpp` — no detail include, no backend class
  exposure. **Neither counter is externally observable in any supported
  production build.** Public `AsyncStats` behavior is untouched by both
  candidates.
- `xmake/bench.lua` bench targets and `xmake/apps.lua` app targets link
  `sluice_async`/`sluice_core` with no `SLUICE_ASYNC_INTERNAL_TESTING` and no
  `src/` include path (verified in the target definitions).

## ODR and layout analysis

- The class definition is compiled per-TU from the detail header. In any
  single binary today there is exactly one macro-consistent definition:
  production binaries compile `threadpool_backend.cpp` /
  `uring_*.cpp` into `sluice_async` **without** the macro; every seam test
  target compiles its **own copy** of those TUs **with** the macro
  (`xmake/tests.lua`: `context_ownership_target`, `threadpool_progress_target`,
  `uring_progress_target` and the contract/shutdown suites) and never links
  the production backend objects. The two builds never meet in one binary.
- Guarding the increment (OP-ONLY) or the field+increment (FULL) with
  `#if defined(SLUICE_ASYNC_INTERNAL_TESTING)` preserves this per-binary
  consistency: the production TU and the seam TU each see a self-consistent
  class. The seam builds already accept a macro-dependent layout (pause gates
  and injection hooks are already `#if`-gated), so a test-only field extends
  an existing, sanctioned divergence rather than creating a new one.
- FULL-arm layout deltas (to be measured in R3): `ThreadPoolBackend` loses an
  8-byte member located between `workers_` and the test gate block;
  `UringAsyncBackend` loses an 8-byte member inside the tail atomics block
  (`submit_flushes_` / `live_cookies_` / `live_control_sqes_`), shifting the
  offsets of the two sibling atomics. All these fields sit in the same tail
  region of the object; heap-allocated once per backend instance, touched by
  the same writers — the practical cache-line effect is measured, not
  assumed.

## Thread ownership and synchronization

- `syscall_count_`: written by worker threads only (1..N, W-02 uses up to 8);
  read only by the test thread through the getter. Under W1 there is a single
  writer (uncontended RMW, still a locked instruction on x86); under W4/W8
  the writers share the cache line (potential coherence traffic).
- `submit_flushes_`: written only by threads holding `dispatch_mtx_`
  (submitters and the poll/progress owner), i.e. effectively serialized by
  the mutex — the RMW is uncontended at the instruction level but still emits
  `lock add` on x86 and writes the line.

## Uncertainties and falsifiers

1. *Hidden production reader* — falsified by the full-reference grep (only
   the sites listed above; the getters' declarations are macro-gated).
2. *A consumer binary mixing macro and non-macro definitions of the backend
   class* — falsified by the `xmake` target audit (no target links
   `sluice_async` while also compiling backend sources with the macro).
3. *Compiler eliminating the RMW in Release* — decided empirically in R2;
   if the operation is absent from the linked Release binary, the candidate
   short-circuits to `NO_CHANGE_JUSTIFIED`.
4. *Tests depending on exact positive totals* — TP tests do assert exact
   totals (`== 1`, `== 2`, `== 4`, `+1` deltas); UR tests assert only
   zero/non-zero/delta-unchanged. The internal-testing build must therefore
   keep exact TP counting semantics (it does: the patch guards only the
   increment, and in the seam build it remains executed identically).

## Conclusion

Both counters are **observation-only derived summaries whose sole consumer
is the internal test seam**; neither carries semantic authority, ordering or
production observability. Semantic elimination from Release is authorized to
proceed to R2/R3 under the `SLUICE_ASYNC_INTERNAL_TESTING` mechanism.
