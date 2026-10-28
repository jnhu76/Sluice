# Direct Hot-Path Ledger — Machine Work per Operation (P1 Synthesis)

Companion to `DIRECT_COST_FLOOR.md`. One row per public Direct hot path at
baseline **B1** (commit `77e5399a45998dea396699fa2e063c94a665945a`, archive
sha256 `d1e351a5…07f0`), merging the six P1 static audits with the executed
ladder campaign. Legend as in `DIRECT_COST_FLOOR.md` §0 (`[RUN]` /
`[TRANSCRIBED]` / `[UNCERTAIN]`).

**Columns.** *Machine work* = executed instructions on one legal success-path
call (includes branches, syscall `call`, `ret`; audit counting convention,
`objdump -d/-dr` of frozen archive objects; per-op detail in the audit doc).
*Baseline* = matched-native comparator compiled in-session with identical
flags (static authority for absolute counts). *Syscalls/op* = exact strace
counts from the campaign (`[RUN]`, per-cell `p1-raw/*` records). *Copies* =
userspace payload copies (kernel performs one copy per data-transfer syscall
by definition of the transfer; the audits certify no additional userspace
copy: caller span is passed directly to the syscall, no `memcpy`/`memmove`
on any success path; the only wide stores are fixed-size state staging).
*PMU Δ* = whole-process `instructions:u` cross-arm delta per timed op
(diagnostic caliber; absolute values approximate, deltas are the signal).
*Verdict* = §5.5 wall rule with session MDEs (read 5.535 / write 15.695 /
meta 10.216 / cold 1416.185 ns).

## 1. Single-op data paths

| Op | Machine work (success) | Static baseline (matched arm) | Syscalls/op | Userspace copies | PMU Δ instr/op | Wall Δ(B1−N1) & verdict |
|---|---|---|---|---|---|---|
| **read_at** | 39 instr / 7 br / 1 call / 3 ld / 8 st / 56-B frame (`p1-audit/positioned-reads.md` §1) | 40 (standalone A1 recount; #600 ledger ≈42 `[TRANSCRIBED]`, convention gap `[UNCERTAIN]`) | 1×pread64 (nonzero), 0 at 0 B — strace 1.000/0.000 `[RUN]` | 0 | +34.2…+39.7 (size-independent ≈+38; +26.6 @1MiB) | 0B +3.338 (sub-MDE, PARITY); 1B…1MiB PARITY; random/cold PARITY; 64B NO_CLAIM (contaminated); 64k NO_CLAIM (bandwidth band) |
| **read** (shared cursor) | 29 / 6 br / 1 call / 3 ld / 7 st / 40-B frame (offset≡0 folds range+transfer into one flag-reusing `jle`) | 35 (B1 leaner; comparator pays 4 callee-saved save/restore — register-allocation variance, same semantics) | 1×read (inherited semantics: native OFD cursor) | 0 | no ladder arm (P0 §4.13 residue) | static PARITY vs matched arm; no wall cell |
| **write_at** | 39 / 7 br / 1 call / 3 ld / 8 st / 56-B frame (`p1-audit/write-primitives.md` §1) | 40 (incl. 1 loop-rotation jmp) | 1×pwrite64, 0 at 0 B — strace 1.000/0.000 `[RUN]` | 0 | +18.0…+19.3 (constant across all 7 sizes) | 0B +2.002 (sub-MDE, PARITY); all sizes PARITY; cold NO_CLAIM |
| **write** (shared cursor) | 29 / 6 br / 1 call / 3 ld / 7 st / 40-B frame | 35 | 1×write | 0 | no ladder arm | static PARITY vs matched arm |

Zero-length and closed-file paths issue **zero syscalls** on both tiers
(strace-verified `[RUN]`), and userspace rejection costs read +2.020 /
write +1.721 / file_info −0.627 ns/op vs matched mirrors (diagnostic cells,
`p1-raw/*-reject-closed-*.txt`) — two orders below the kernel EBADF floors
(409/436/479 ns/op).

## 2. Composition paths

Step accounting: `+N per additional step` applies to short-count steps
(progress-advance loop body); the 1-step figure is a single fully-satisfying
syscall.

| Op | Machine work | Static baseline (matched-native composition control) | Syscalls/op | Userspace copies | PMU Δ instr/op | Wall Δ & verdict |
|---|---|---|---|---|---|---|
| **read_exact_at** | 100 instr 1-step (prologue 29 = baseline parity); +25/short step; excess = per-step re-admission ≈13 + dead `CompositionState` block 8 + double exit materialization ≈9 (`p1-audit/composition-read.md` §1) | 63 (1-step), P1-built N1-matched control | 1×pread64/step (1.000 measured @4 KiB single-step; 1.3333 at the full/partial/at-EOF rotation — identical across tiers `[RUN]`) | 0 (buffer advanced in place per step) | +118.2 (AX−A1x) ≈ 3× the single-op constant | **+18.992 EXCESS** @4 KiB seq (CI [+7.788,+24.326], ≥MDE 5.535); **+16.129 EXCESS** @EOF mix (CI [+6.234,+25.061]) |
| **read_exact** (shared cursor) | 82 1-step (prologue 21 = baseline parity); +19/step; same three mechanisms (≈9/8/≈12) | 51 | 1×read/step | 0 | no ladder arm (never measured — `[UNCERTAIN]` dynamic share extrapolated) | static gap vs matched control; no wall cell |
| **write_all_at** | 101 instr / 14 br / 1 call, 1-step; +44/step; excess = 12 re-validation + ~10 staging & spill + 1 dead 16-B store per iteration (`p1-audit/combined-writes.md` §1) | A1-pattern arm: +29 (1-step) / +17 (per step) | 1×pwrite64/step (1.000 measured `[RUN]`) | 0 | +58.1 (WX−W1x, cross-tier ≈3× the +19 single-op constant); same-caliber composition-increment ratio (WX−W2)/(W1x−W1) = +61.0/+22.0 ≈ 2.8× | +11.392 @4 KiB — **PARITY at wall** (< MDE 15.695, CI spans 0); instruction-caliber excess stands (gap G-WRITECOMP-01) |
| **write_all** (shared cursor) | 82 / 12 br / 1 call, 1-step; +34/step; same mechanisms at smaller amplitude | +22 (1-step) / +11 (per step) | 1×write/step | 0 | no ladder arm | static gap; no wall cell |

EINTR discipline (all four compose loops): the full request is re-issued only
when the attempt reports interruption without a completed count; spill blocks
sit inside the loop in both tiers (SEM-05; shape identical in matched arms).

## 3. Resource and metadata paths

| Op | Machine work (success) | Static baseline | Syscalls/op | Userspace copies | PMU Δ instr/op | Wall Δ & verdict |
|---|---|---|---|---|---|---|
| **File::open** | 57 instr (9-mode constant flag table +6-immediate jump chain is the transformable part, +13 vs baseline) | 44 (`p1-audit/RESOURCE_METADATA_OPS_AUDIT.md` §open) | 1×openat (+0 close); measured pair 1 openat + 1 close per open-close op `[RUN]` | 0 (NUL scan is `memchr`, not a copy) | +74.0 (open+close pair, includes close at Δ0) | pair +11.487 — PARITY (< max(10.216, 15.969)) |
| **File::close** | 18 instr, 0x38 frame; ownership consumed before the single terminal `close`; EINTR → `interrupted`, no retry | 18 = **Δ0**, instruction-for-instruction identical incl. closed no-op path | 1×close (0 when already closed — no-op success) | 0 | (in pair above) | (in pair above) |
| **file_info** | 49 instr / 22 st, incl. 144-B `stat` zero-init (10 instr: `pxor`+9×`movaps`) | 49/22 zero-init matched arm (Δ0 — matched discipline); 39/13 vs a no-init floor (gap G-META-01) | 1×fstat (1.0000 measured `[RUN]`; EINTR re-invoked only) | 0 (field projection + ~12 sret staging ops) | −2.0 | +30.312 — NO_CLAIM middle band (CI spans 0, >max(10.216, 10.134)); instruction parity contradicts the wall point estimate `[UNCERTAIN]` |
| **size** | 31 instr / 14 st (`file_info` inlined; kind/dev/ino dead-eliminated; carries the same zero-init) | 22/5 no-init comparator | 1×fstat (1.0000 measured `[RUN]`) | 0 | −18.0 | −6.609 — PARITY (< 10.216; point estimate Sluice faster) |
| **resize** | 28 / 4 br / 1 call / 7 ld / 5 st / 0x48 frame | 28 = **Δ0** (static +2 all in cold blocks) | 1×ftruncate (re-invoked on EINTR only) | 0 | no ladder arm (state-change group not in kit) | static PARITY (`p1-audit/STATE_CHANGE_PERSISTENCE_AUDIT.md` §3) |
| **sync_data** | 20 / 2 br / 1 call / 4 ld / 3 st / 0x30 frame (incl. `Result<void>` bool round-trip, 3 instr where 1 would do — identical in N1, D3 class) | 19 (Δ = 1 always-predicted layout `jmp`) | 1×fdatasync | 0 | no ladder arm | static PARITY (audit §1) |
| **sync_all** | 20 / 2 br / 1 call / 4 ld / 3 st / 0x30 (byte-symmetric with sync_data except the syscall) | 19 (Δ = 1 layout `jmp`) | 1×fsync (metadata increment carried entirely by syscall choice — zero extra userspace instructions) | 0 | no ladder arm | static PARITY (audit §2) |

## 4. Cross-instrument notes

1. **Static matched-arm parity vs positive PMU ladder deltas.** The static
   baselines above are same-flags standalone compiliations of the A1/N1
   pattern; the ladder N1 arms are in-harness mirrors and B1 is entered
   through the archive boundary (PLT, no cross-TU inlining). Both instruments
   agree on the wall outcome: single-op userspace effects are sub-MDE (≈2–4 ns
   point estimates at 0 B) and the ≈+38/+19 instr/op PMU constants are
   consistent with those magnitudes at ~4 IPC `[UNCERTAIN: not further
   attributed in P1]`.
2. **Composition is where userspace excess becomes measurable.** The compose
   loops are the only hot paths whose B1−N1 wall delta exceeds the session
   MDE with a CI excluding 0 (read_exact_at, two cells), and the only write
   path with a 3× instruction-delta multiplier (write_all_at).
3. **Syscall parity is total between the N1 and B1 tiers**: every measured
   cell shows identical syscall counts for the matched-native and Sluice arms
   (data ops, metadata ops, rejection paths, zero-length paths) — no hidden
   extra syscall exists anywhere in the Direct form `[RUN]`. (N0 floor arms
   differ only by construction: the raw arm pays its own verify-pass re-read
   and has no zero-length short-circuit.)
4. **No userspace payload copy exists on any Direct hot path** — the
   transfer is always the single kernel copy performed by the one syscall
   per op/step.
5. **Metric coverage vs MISSION §3 P1.1 (gap).** This ledger reports
   instructions/branches/calls/loads/stores (static) and `instructions:u`
   cross-arm deltas (PMU). `cycles:u` were captured in every `*-pmu.txt`
   per plan §6.2 but are **not tabulated here**; branch-misprediction
   counts were **never measured** in the campaign (no `perf branch-misses`
   run exists), and effective-vs-overhead byte accounting is only implicit
   (payload = request size; overhead = the instruction/syscall tables).
   See `DIRECT_COST_FLOOR.md` §6.8 — P2 must add these calibers before any
   MISSION P1.1 compliance claim. (Where cycles were inspected: the
   composition cell shows AX wall-slower while executing *fewer* cycles
   than A1x (≈8720 vs ≈8888 cycles/op) — a dependency/IPC signature, not
   extra work.)

## 5. Per-op audit and raw sources

`read_at`/`read`: `p1-audit/positioned-reads.md` §1–2; `write_at`/`write`:
`p1-audit/write-primitives.md` §1–2; `read_exact_at`/`read_exact`:
`p1-audit/composition-read.md` §1–2; `write_all_at`/`write_all`:
`p1-audit/combined-writes.md` §1–2; `File::open`/`File::close`/`file_info`/
`size`: `p1-audit/RESOURCE_METADATA_OPS_AUDIT.md`; `resize`/`sync_data`/
`sync_all`: `p1-audit/STATE_CHANGE_PERSISTENCE_AUDIT.md`. Ladder cells:
`p1-raw/read-sweep-*`, `read-composition-4k*`, `read-eof-mix-4k*`,
`read-random-4k*`, `read-cold-*`, `write-sweep-*`, `write-composition-4k*`,
`write-random-4k*`, `write-cold-4k*`, `meta-open-close*`, `meta-file-info*`,
`meta-size*`, `*-reject-closed-*`, `*-eintr-*`, `aa-*`, `matrix-*`.
