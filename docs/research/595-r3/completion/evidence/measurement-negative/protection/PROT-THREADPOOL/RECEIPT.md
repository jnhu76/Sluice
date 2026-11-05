# PROT-THREADPOOL — blocked B; A0-only no-enter functional check

| field | value |
|---|---|
| phase status | **BLOCKED_SEMANTICS_NOT_READY** (B functional comparison not run) |
| cases | `W03-threadpool-4k-{d4,d16,d64}-w4` |
| runs | 3 strace invocations per case (WARMUP=0 MEASURED=1, 20,000 ops) |
| raw | `/home/jnhu/sluice-595-e5/r3-completion-20261105T083818Z/raw/protection/SAFE-A0-W03-THREADPOOL-*/`, `/home/jnhu/sluice-595-e5/r3-completion-20261105T083818Z/raw/protection/DIAG-THREADPOOL-NOCASE/` |

| depth | case | valid | process io_uring_enter | pure_submit | CPU/op (ns) |
|---|---|---|---|---|---|
| d4 | W03-threadpool-4k-d4-w4 | 3/3 | 1 | 0 | 84777.9 |
| d16 | W03-threadpool-4k-d16-w4 | 3/3 | 1 | 0 | 68106.2 |
| d64 | W03-threadpool-4k-d64-w4 | 3/3 | 1 | 0 | 33176.4 |

- Every invocation exits 0 and passes the bench byte oracle (`ops=20000`).
- The only `io_uring_enter` is one startup probe
  `io_uring_enter(fd, 1, 1, IORING_ENTER_GETEVENTS)` from the uring availability
  check (`src/async/uring_backend.cpp:241-257`, `io_uring_submit_and_wait`).
  The no-case baseline (`SLUICE_BENCH_MEASURED=0`) has the same 1 enter:
  total=1, pure_submit=0, to_submit_hist={'1': 1}. The threadpool workload therefore adds
  **0** io_uring enters.
- Thread sampling: d16 max user threads **6**; d16
  `/usr/bin/time -v` max RSS **[5352]** KiB.

## Batch-entry check (static)

`bench/r3_ready_batch.hpp:72` constructs `backend_` by `dynamic_cast<UringAsyncBackend*>`;
`:89` routes `arm_ == Arm::a0 || backend_ == nullptr` to
`submit_context_singles`, and `:142` calls `ctx_.submit_read(op)`. For the
threadpool backend `backend_ == nullptr`, so even the B arm would take the
single-submit path; `:97`/`:110` (the Context batch entry) are unreachable.
B was not executed because `semanticsReady=false`.

## Boundary

- B functional comparison is `BLOCKED_SEMANTICS_NOT_READY`; no B thread/RSS
  comparison is claimed.
