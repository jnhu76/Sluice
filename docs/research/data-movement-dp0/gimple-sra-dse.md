# DP-0 GIMPLE/SRA/DSE findings (R1)

Pass inventory and capture commands: [compiler-inventory.md](compiler-inventory.md).
The controlled probes below were run with this exact toolchain; probe sources
and dumps live in the session's `/tmp/dp0-ext/` (probe designs and results
summarized here verbatim; regenerate via the commands given).

## 1. Why the audit reads `270t.optimized`, not C++

GCC 15.2.0's pipeline relevant to data movement (dump suffixes verified from
actual files): early ESRA `039t.esra` → early DSE `045t.dse1` → local modref
`055t.modref1` → IPA-SRA `088i.sra` → main SRA `138t.sra` → DSE
`125t.dse2`/`158t.dse3`/`213t.dse5` → store-merging `220t.store-merging` →
final GIMPLE `270t.optimized`. Every statement below cites what survives at
`270t.optimized` (and, for store-death timing, at which pass it dies).

## 2. Probe A — dead field stores against a 120-byte sink (models DP-1)

Design: `struct Blob` = 15 × long = 120 B; a builder fills all 15 fields
individually, then hands `&b` to a sink reading only `b.head` — the exact
shape of `begin_publication`'s payload (120 B materialized, 24 B `id`
consumed). Four sink variants (extern / same-TU inlineable / same-TU
noinline / same-TU external linkage noinline):

1. **Extern sink (different TU, no LTO — the production configuration):**
   no store is removed. All 120 bytes are written at the call boundary;
   store-merging only re-packs them into 7×16 B vector stores + 1 scalar
   (`movaps`×7 + `movq`). DSE cannot act: the callee body is invisible.
2. **Same-TU inlineable:** the entire construction vanishes (`g_keep ={v}
   4369; return;`); `-fopt-info-all` shows the inline followed by SRA/DSE.
3. **Same-TU noinline:** stores still die — ipa-cp builds a `.constprop.isra`
   clone and ipa-modref records a precise access on parm 0
   (`Recording base_set=2 ref_set=1 Parm 0 …`), enabling DSE at `045t.dse1`
   (store count 1→0 exactly at that pass; recompiling with
   `-fno-ipa-modref` makes the store survive — verified). `noinline` is not
   a compiler boundary.
4. **Same-TU external linkage noinline:** stores also die. The gate is
   callee-body availability within the compilation, not linkage.

**Load-bearing consequence:** under the frozen no-LTO config, Sluice's
cross-TU `begin_publication` cannot have its payload copy eliminated by the
compiler, and the callers cannot have their payload construction eliminated
either (the sink — `begin_publication` — lives in `request_core.cpp`, the
payload construction in the backends). This is why DP-1 requires a source
change and why the effect is real physical work
(results/analysis.md: zop −27.5 instructions/op measured, 15/15 pairs).

## 3. Probe B — scalar zero-store merging (models DP-3A accounting)

`struct Big` (21 × long = 168 B — Slot's size) zeroed field-by-field at -O3,
baseline x86-64: 15 scalar stores → 7×16 B `movups` + 1 `movq` (whole 168 B:
10 `movups` + `movq`). No `memset` call at this size; `-mavx2` would widen
further. Consequence: instruction-level, one `movups` is 1 instruction but
16 bytes of stores; per-op "store count" and "bytes" must both be quoted
(the dp3 receipt does: dp3a removes ~13 scalar-equivalent stores ≈
131 B of re-zeroing per acquire, measured 16.5 instr/op at zop — the delta
between store count and instruction count is the merging + address
arithmetic).

## 4. Same-TU modeling hazard (negative result the audit must respect)

If Sluice code ever models a "consumer reads back the slot" check in the same
TU with plain C++ loads, GCC may delete the producer's stores against it
(probe A cases 2–4). Only `volatile`, atomics, asm, or a real TU/syscall
boundary are respected. Sluice's cross-thread slot access is already behind
`pthread_mutex_t` (external calls — never eliminable), which is why the
release_slot_/reserve double-zeroing is real executed work and not an
optimization artifact: the mutex calls are the boundary that keeps DSE honest.

## 5. What this means for each candidate

| candidate | GIMPLE/assembly fact | consequence |
|---|---|---|
| DP-1 | payload init (`rep stos` 15×8 B) + 12-move copy block survive at 270t across TUs | must be removed at source level; was (dp1) |
| DP-2 | `RequestResultData` init (3×`movaps`+`movq`) and metadata moves are inlined into the header-instantiated `Request<T>` code; SRA removes nothing because the copies are real | header shape decides; measured (dp2/dp2b) |
| DP-3A | reserve's re-zeroing survives (mutex boundaries); release_slot_'s zeroing likewise | source-level removal only; was (dp3a) |
| W-01 | `blocking::read_at` final GIMPLE keeps only the precheck + pread + Result stores; no dead control-plane objects exist to remove | clean; confirmed by byte-identical negative control |

Kernel-side io_uring facts cited in the external comparison (SQE/CQE shared
memory, `io_uring_initialize_sqe` per-slot reinit) are contract-verified from
liburing source; kernel-source internals are marked
IMPLEMENTATION_UNVERIFIED there.
