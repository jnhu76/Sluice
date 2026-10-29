# R3 — Three-Arm Candidate Design

## Arm construction

Five isolated worktrees at baseline `3df10dc4`; each candidate arm carries
an uncommitted patch. The archived `patches/*.diff` files (whose SHA-256
values below identify the **production-code component** of each arm — the
only component that affects the measured binaries) plus one shared,
arm-identical component: the `tests/uring_flush_count_oracle_test.cpp`
registration in `xmake/tests.lua` (research-branch commit `45a34296`),
copied identically into every arm so the oracle runs everywhere. The live
worktree diffs therefore contain that registration hunk as well; it touches
no production object.

| Arm | Worktree | Patch | Files |
|---|---|---|---|
| BASE | `/tmp/sluice-575/base` | none (baseline) | — |
| TP-OP-ONLY | `/tmp/sluice-575/tp-oponly` | `patches/tp-oponly.diff` (SHA-256 `74b07bc9…fd23`) | `src/async/threadpool_backend.cpp` (+2) |
| TP-FULL | `/tmp/sluice-575/tp-full` | `patches/tp-full.diff` (`4bd2a67d…b1b`) | above + `include/sluice/async/detail/threadpool_backend.hpp` (+3) |
| UR-OP-ONLY | `/tmp/sluice-575/ur-oponly` | `patches/ur-oponly.diff` (`4ac343cf…fe27`) | `src/async/uring_transport.cpp` (+2) |
| UR-FULL | `/tmp/sluice-575/ur-full` | `patches/ur-full.diff` (`37ba7460…ab00`) | above + `include/sluice/async/detail/uring_backend.hpp` (+3) |

Patch shapes (the R1-preferred mechanism — the existing
`SLUICE_ASYNC_INTERNAL_TESTING` seam macro, no new switches, no new public
surface):

```cpp
// TP-OP-ONLY / TP-FULL, worker_loop():
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
                syscall_count_.fetch_add(1, std::memory_order_relaxed);
#endif

// TP-FULL additionally guards the field declaration:
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
    std::atomic<std::uint64_t> syscall_count_{0};
#endif

// UR-OP-ONLY / UR-FULL, submit_transport_locked():
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
    submit_flushes_.fetch_add(1, std::memory_order_relaxed);
#endif
// UR-FULL additionally guards the submit_flushes_ field the same way.
```

No ThreadPool × io_uring combination arm exists: the two counters are studied
independently, as required.

## Contrast semantics

- **Contrast A (BASE → OP-ONLY):** removes the atomic RMW from the production
  build while leaving class layout, field offsets and all sibling state
  untouched. Isolates the synchronization/instruction cost.
- **Contrast B (OP-ONLY → FULL):** additionally deletes the field, changing
  class size and tail-field offsets. Isolates layout effects. Any Contrast-B
  delta is **not** attributed to synchronization removal.

## Verified arm properties (fail-closed assembly oracle)

Exact-pattern disassembly scan (`^\s+[0-9a-f]+:\s+lock\s`) of the production
`sluice_async` objects and the linked Release benchmarks:

| Arm | `threadpool_backend.cpp.o` lock RMWs | `uring_transport.cpp.o` `submit_flushes_` RMWs | linked `bench_w02` target-site scan |
|---|---|---|---|
| BASE | 1 (`lock addq $1,0x190(%r15)` in `worker_loop`) | 4 (0x120: out-of-line + 3 inlined) | `0x190`×1, `0x120`×4, `0x130`×1 present |
| TP-OP-ONLY | **0** | 4 (unchanged) | `0x190` absent |
| TP-FULL | **0** | 4 (unchanged) | `0x190` absent |
| UR-OP-ONLY | 1 (unchanged) | **0** | `0x120` absent |
| UR-FULL | 1 (unchanged) | **0** | one `0x120` remains — the **relocated** `live_cookies_` (see below) |

UR-FULL layout shift (by design of Contrast B): deleting `submit_flushes_`
moves `live_cookies_` from +0x128 to +0x120 and `live_control_sqes_` from
+0x130 to +0x128. The two surviving RMW pairs in `uring_transport.cpp.o`
(`add/sub` at 0x120 and 0x128) were re-attributed by function:
`dispatch_one_locked`/`retire_router_entry_` (live_cookies_) and
`mark_consumed_control_locked_`/`poison_and_recover_locked_`
(live_control_sqes_) — **the protocol atomics survive in every arm**, only
the observation counter disappears. TP-FULL similarly shifts nothing that
follows it except the (already test-only) gate block.

## Layout measurements (production macro set)

`sizeof` probe compiled against each arm's production headers+library
(`g++ -std=c++20 -O2 -I include -I src/async -D SLUICE_LIBURING_PROFILE=on
-D SLUICE_HAS_LIBURING -D NDEBUG`, source under `scripts/layout_probe.cpp`):

| Arm | `sizeof(ThreadPoolBackend)` | `sizeof(UringAsyncBackend)` |
|---|---|---|
| BASE | 408 | 320 |
| TP-OP-ONLY | 408 | 320 |
| TP-FULL | **400** | 320 |
| UR-OP-ONLY | 408 | 320 |
| UR-FULL | 408 | **312** |

Each backend object is heap-allocated exactly once per context, so the FULL
arms save 8 bytes per context and shift only tail fields.

## Build isolation verification

- Production `sluice_async` objects are compiled without
  `SLUICE_ASYNC_INTERNAL_TESTING` in every arm (verified via
  `compile_commands.json`: library TU defines are only
  `SLUICE_LIBURING_PROFILE=on`, `SLUICE_HAS_LIBURING`, `NDEBUG`).
- Every seam test target (including the new `uring_flush_count_oracle_test`)
  compiles its **own copy** of the backend TUs with the macro and links only
  `sluice_core` — no test-layout class crosses into a production-layout
  binary, and no production binary links test-layout objects. This is the
  pre-existing isolation property (R1) and the patches do not alter it: in
  each binary every TU sees the same (consistent) class definition.
- Public ABI: installed headers (Appendix-B allowlist) do not include the
  detail headers that declare these fields; the public surface is
  byte-identical across arms (the only changed headers are internal detail
  headers).

## Deterministic suite status (arm green)

`xmake test` on every arm (Release profile, real ring): **all green** on
BASE, TP-OP-ONLY, TP-FULL, UR-OP-ONLY and UR-FULL — 78/78 at the moment of
the arm runs (before the oracle registration was propagated) and 79/79 on
re-run with the oracle registered (Review A re-executed both FULL arms at
79/79). The suite includes the
seam builds whose counter assertions (16 TP sites, 6 UR sites) all passed on
the patched arms — the internal test builds still count, because the seam
macro re-enables the guarded increments.

New research oracle committed on the research branch:
`tests/uring_flush_count_oracle_test.cpp` (registered in `xmake/tests.lua`
as `uring_flush_count_oracle_test`). It pins the **positive** flush-count
semantics that the pre-existing suite only implied (all pre-existing UR
assertions are zero/delta-unchanged checks; a suppressed increment would
have passed them — this gap is what the new oracle closes for R4's UR-M1
mutant). It passes on BASE and on all four arms.

## Binary authenticity

Per-arm `libsluice_async.a`, `bench_w02_pipeline`, `bench_w03_external_loop`
SHA-256 values: `patches/arm-identity.txt`. Regeneration:
`cd /tmp/sluice-575/<arm> && xmake f -m release --liburing=y && xmake build -g bench -y`.

Research arms remain isolated worktrees with uncommitted patches; they are
not production commits (productization, if earned, is a separate focused PR).
