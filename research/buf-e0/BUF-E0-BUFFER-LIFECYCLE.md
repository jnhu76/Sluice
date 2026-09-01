# BUF-E0 — current buffer lifecycle census (static, CODE FACT only)

Status: CODE FACT census. No performance finding is claimed here; costs are
`COST UNMEASURED` until measured under `BUF-E0-PREREGISTRATION.md`.
Machine-readable form: `buf_e0_census.json`. Execution authority: #263.

Subject: the production per-slot pipeline buffer of the reference async copy
application — `PipelineSlot` in `apps/sluice-copy/copy_task.cpp` (the same
copy task the production CLI drives; the same code TAX-0 COPY-AB-1 measured).

## 1. Real lifecycle, as built

```text
application setup (CLI parse; caller opens src/dst fds)
    ↓
argument validation (public entry defends caps BEFORE any allocation)
    ↓
slot creation   slots.reserve(depth);
                per slot: make_unique<PipelineSlot>(buffer_size)
                  → std::vector<std::byte> buffer(cap)
                  → ONE heap allocation + EAGER ZERO-INIT of all cap bytes
                    (touches every page)          [setup-only, one-time]
    ↓
runtime starts  (run_task_to_result: build/start ApplicationRuntime + backend)
    ↓
first I/O       Phase 1: up to `depth` submit_read at chunk_offset=i*N
                  → backend pread overwrites buffer bytes  [once per slot]
    ↓
reuse loop      after slot's write completes:
                  chunk_offset += buffer_size*depth; filled=0; written=0;
                  submit_slot_read into the SAME buffer
                  (no realloc, no re-init, address stable)   [repeated]
    ↓
drain           (error path) / sync policy (clean path)      [no buffer work]
    ↓
destroy         slots vector destructs → one free per slot   [one-time]
```

Per-step allocation/initialization/page-touch/overwrite/repetition flags are
in `buf_e0_census.json` (`lifecycle_steps`).

## 2. The eight census questions

| # | Question | Answer | Status |
| --- | --- | --- | --- |
| 1 | vector allocation per op or setup-only? | **setup-only** — all slots built once before the Runtime starts | CODE FACT |
| 2 | slot reuse count per lifecycle? | unbounded within one copy: ≈ `ceil(file_bytes / (buffer_size × depth))` chunk cycles per slot (1 GiB file, 1 MiB × d8 → 128 reuses/slot) | CODE FACT |
| 3 | buffer fully overwritten by read? | yes for full chunks (`pread` fills `[0,N)`, overwriting every zero-initialized byte); EOF tail overwrites only `[0,filled)` and the write side never reads beyond `filled` | CODE FACT |
| 4 | short read / partial valid length? | yes — `filled` cursor; short reads retry in-slot; EOF tail is partial | CODE FACT |
| 5 | slot count decided by? | caller `pipeline_depth` (CLI default 1; caps 64 slots / 64 MiB per slot / 512 MiB total) | CODE FACT |
| 6 | capacity == active in-flight depth? | yes — in steady state all `depth` reads are outstanding; single outstanding write by design | CODE FACT |
| 7 | resize/growth? | none — buffer fixed at construction; `slots` is a reserve(depth) pointer vector | CODE FACT |
| 8 | steady-state heap allocation in production? | none in the copy task loops; ThreadPoolBackend uses fixed workers + bounded dispatch storage (setup-time reserve only) | CODE FACT (inspection) |

## 3. Consequences for the experiment design

- The production hypothesis is READ-side: the first read into a fresh slot
  overwrites all N initialized bytes (Q3), so any value the eager zero-init
  wrote is dead data after first read (except the never-written EOF tail).
- The lifecycle cost of storage construction is paid ONCE per copy operation
  (Q1) and is amortized over Q2's reuse count; therefore Phase A/B measure
  the one-time lifecycle and Phase C measures the amortized regime — matching
  the three-phase design in the preregistration.
- Allocation is setup-only, so `buf_e0` measures construction/first-use in a
  fresh-page regime (cold-start truth) and steady-state in a hold regime;
  both regimes are documented in the preregistration (allocator pinning).

## 4. Adjacent sites recorded, out of scope

- `src/wal.cpp` — per-record owned RESULT vectors (record-sized return
  values), not per-slot I/O staging. Different lifecycle role; same eager-init
  mechanism family. Not the Phase 2 subject.
- `examples/mvp_copy_pipeline.cpp` — educational example, fixed 1 KiB buffers.
- `bench/*` — research instruments, not production lifecycle.

## 5. Hypotheses carried into measurement

- **BUF-F01** (eager initialization): `std::vector<std::byte>(N)`'s eager
  zero-init creates a lifecycle cost that is NOT merely shifted to first
  touch. Construction-time zero-init is CODE FACT; the total-cost question is
  COST UNMEASURED.
- **BUF-F02** (per-slot ownership/storage policy): per-slot ownership causes a
  measurable capacity/lifecycle/steady-state penalty. Structure is CODE FACT;
  penalty COST UNMEASURED.
