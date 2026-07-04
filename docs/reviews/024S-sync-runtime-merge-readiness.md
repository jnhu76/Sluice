# 024S Sync Runtime merge-readiness checklist

**Status:** Reviewer-facing (sluice-CORE-024S §6). The gate a reviewer (or the
author, pre-merge) walks through before merging `sync-runtime` → `master`.

## Must be true before merge

- [x] Sync backend taxonomy exists: `docs/io/sync-backend-taxonomy.md`
- [x] Production `BlockingIoPool` exists OUTSIDE benchmark support: `include/sluice/blocking_io_pool.hpp` + `src/blocking_io_pool.cpp` (namespace `sluice`)
- [x] Benchmarks use the production pool instead of a duplicate: `bench/support/blocking_io_pool.*` is now a thin adapter over `sluice::BlockingIoPool`
- [x] Sync contract ADR exists: `docs/adr/ADR-024S-sync-runtime-contract.md`
- [x] Partial-I/O / error semantics documented: `docs/io/sync-error-semantics.md`
- [x] `BlockingIoPool` lifecycle documented: ADR §4 (G9) + `docs/io/sync-error-semantics.md`
- [x] Negative tests cover lifecycle and partial/error behavior: `tests/blocking_io_pool_prod_test.cpp` (10 slices) + `tests/sync_contract_negative_test.cpp` (5 slices) + existing `blocking_io_pool_test.cpp` / `fault_test.cpp` / `reader_test.cpp` / `writer_test.cpp` / `file_positional_test.cpp`
- [x] W1–W4 benchmark notes exist: `docs/bench/sync-runtime-bench-notes.md`
- [x] Sanitizer/valgrind commands documented (see "Verification commands" below)
- [x] No async/io_uring/epoll/P2300/actor/green-thread code added (the `src/experimental/uring_*` is pre-existing, gated, off by default, and NOT part of this contract)
- [x] Default path remains blocking sync (no `Reader`/`Writer`/positional semantic change)
- [x] Production multithreaded path is bounded (fixed worker count + bounded queue + backpressure/rejection)

## Reviewer checklist

- [ ] Confirm no async-runtime concepts leaked into sync runtime. **How:** the sync public headers (`include/sluice/reader.hpp`, `writer.hpp`, `file.hpp`, `sync.hpp`, `fault.hpp`, `memory_io_context.hpp`, `buffered_readable.hpp`) must not `#include` anything from `async/` or `<coroutine>`. Grep:
  ```bash
  grep -rn "async/\|<coroutine>\|co_await\|co_return\|io_uring" include/sluice/*.hpp
  ```
  Expected: **no matches** in the sync public surface (`io_uring` may appear only in archived/spike docs and `src/experimental/`).
- [ ] Confirm `BlockingIoPool` is **not** described as an async runtime. It is in `bench/support/`, is not an `IoContext`, and the ADR (G9) + this doc say so explicitly.
- [ ] Confirm durability docs do not overclaim physical persistence. ADR G8 + `docs/sync-durability-model.md` + `docs/results/sync-durability-baseline.md` all carry the "OS/filesystem contract, not physical-media proof" caveat.
- [ ] Confirm partial-I/O behavior is tested: EOF→`eof`, short-read retry, zero-progress→`invalid_state`, zero-length no-op (`tests/sync_contract_negative_test.cpp` slices 1–4).
- [ ] Confirm shutdown/lifecycle behavior is tested: `blocking_io_pool_test.cpp` (`submit_after_shutdown_is_noop`, `shutdown_joins_workers_and_is_idempotent`, `destructor_drains_and_joins`, `exception_in_job_surfaces_at_wait_all`).
- [ ] Confirm benchmark interpretation is conservative: no universal claim, tmpfs-understates-W4 caveat present, debug-build caveat present.

## Verification commands

```bash
# Full test suite (debug)
xmake f -m debug && xmake build -g test && xmake test

# Sanitizer sweeps (all must pass)
xmake f -m tsan       && xmake build -g test && xmake test   # ThreadSanitizer
xmake f -m asanubsan  && xmake build -g test && xmake test   # ASan + UBSan
xmake f -m ubsan      && xmake build -g test && xmake test   # standalone UBSan

# Valgrind memcheck on the concurrency-sensitive + contract binaries
xmake f -m debug && xmake build blocking_io_pool_test file_positional_test sync_contract_negative_test
for t in blocking_io_pool_test file_positional_test sync_contract_negative_test; do
  valgrind --error-exitcode=99 --leak-check=full --errors-for-leak-kinds=definite --quiet \
    build/linux/x86_64/debug/$t && echo "$t: valgrind clean"
done

# Build the bench matrix (does not run; just confirms it compiles)
xmake build -g bench
```

## Out of scope for this merge (do not block on)

- Async runtime, io_uring default backend, P2300 — all explicitly deferred to the `async-runtime` branch (see `docs/async-deferred-until-sync-baseline.md`).
- p50/p95/p99 latency columns in the bench CSV (methodology "add where feasible").
- Real-disk W4 re-measurement (tmpfs-understates caveat is documented).
- Changing `BlockingIoPool::submit`-after-`shutdown` from no-op to error (N10 — current no-op is the contract; changing it is a separate behavior-change task).
