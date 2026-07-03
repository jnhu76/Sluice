# io_uring readiness gate

**Status: CPPIO-CORE-012D.** Decides whether cppio is ready for an experimental
io_uring spike (013) and records the chosen first slice, risks, and abort
conditions. This is a gate, not an implementation.

## Decision

**READY** for a narrow, isolated, experimental spike — with hard abort
conditions (§7). The MVP blocking core, measurement hooks, microbench harness,
copy-strategy layer, and flush/sync/durability separation are all in place; the
backend boundary (`IoContext`) gives the spike a place to live without disturbing
the default backend.

## 1. Preconditions

The checklist cppio must satisfy before any io_uring code lands:

```text
Blocking baseline exists                      ✓ (001-003)
IoContext boundary exists                     ✓ (009)
measurement hooks exist                       ✓ (004, +per-job stats)
microbench harness exists                     ✓ (010)
copy strategy layer exists                    ✓ (007)
flush/sync/durability semantics are separated ✓ (008)
tests are green                               ✓ (32/32 debug + release)
examples are green                            ✓ (all smoke ok)
Zig parity gaps are documented                ✓ (012B inventory, 012C audit)
```

## 2. Current satisfied prerequisites

All nine preconditions are met as of CPPIO-CORE-011 (closeout in
`docs/mvp-closeout.md`). The spike therefore does not need to invent missing
infrastructure: it can sit behind the existing `IoContext` seam and reuse the
existing bench harness and stats structs.

## 3. Missing prerequisites (deliberately out of scope for the spike)

These are **not** satisfied and the spike must not pretend otherwise:

- No async/evented runtime.
- No cancellation model.
- No scheduler / task / future abstraction.
- No generic Reader/Writer uring backend (only a narrow write slice is spiked).

The spike stays viable by being **synchronous-over-uring**: submit, then block
on completion (`io_uring_submit_and_wait`), so no async runtime is needed.

## 4. Risks

```text
partial completion        — handle by looping until all bytes done
short read/write          — handle by resubmitting the tail
buffer lifetime           — caller-owned buffer must outlive the call; documented
submission/completion ownership — the batch owns its ring; fd is not owned
kernel support variance   — gate on CPPIO_HAS_LIBURING + runtime feature probe
liburing availability     — optional build gate; skip cleanly if absent
cancellation ambiguity    — NONE: the spike is synchronous, no cancel surface
durability confusion      — flush≠sync: the spike does not imply durability
```

## 5. Chosen narrow spike

**Option A: batched file writes.**

```text
It is narrower than generic copy,
does not require full async runtime,
does not require network,
can reuse Writer semantics,
can compare against FileWriter/write_vec/buffered WAL paths.
```

Rejected alternatives:
- Option B (many independent file reads) — needs more completion-ownership
  plumbing than a single batched write.
- Option C (copy strategy experimental path) — too broad for a first spike;
  couples to the strategy layer before write-only behavior is proven.

## 6. Explicit non-goals

```text
No production backend.
No default backend switch (BlockingIoContext stays the default).
No generic Reader/Writer uring backend.
No async runtime / scheduler / cancellation.
No networking, timers, or mmap.
No durability claim — uring write completion ≠ fsync.
No broad performance claim — at most a local, workload-specific bench row.
```

## 7. Abort conditions

Stop the spike and revert to blocking-only if **any** of these occurs:

- The normal (no-liburing) build breaks.
- The blocking backend's behavior or tests change.
- Buffer-lifetime discipline cannot be enforced cleanly (would require
  overwriting Reader/Writer ownership semantics).
- Partial-completion / short-write handling cannot be made correct without an
  async runtime.
- liburing cannot be made a clean optional dependency.
- The spike requires touching default-backend selection.

The spike proceeds into 013 only behind these conditions.
