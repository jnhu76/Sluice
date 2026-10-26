# PHASE_I_COST_ATTRIBUTION — Sluice #600 (E5 Native Linux)

Answers required by the task, in order:

1. **What does Sluice do beyond the necessary path?** On every legal `read_at` call it
   (a) materializes the `Precheck{optional<IoError>, bool}` verdict struct through
   memory (≈27 instructions: two 24-byte stack copies with SSE, then reloads two branch
   bytes), and (b) re-tests the offset bound a second time in `checked_posix_offset`
   (2 instructions) after `precheck_data_op`'s `range_is_valid` already established it.
   Composition additionally re-runs the full validation inside every `compose` step and
   materializes `CompositionState` per iteration (≈45–60 instructions, +32 ns/step).
2. **What has the compiler already eliminated?** The three source range conditions fuse
   into two compares + one or/sign test; `Result<off_t>`'s scaffolding folds away;
   `precheck_data_op` inlines fully; `read_at` inlines into the compose loop. The
   duplicate offset compare and the Precheck memory round-trip are NOT eliminated.
3. **What must stay (v2 semantics)?** Closed→invalid_state, access→invalid_argument,
   length-0→Result{0} without syscall, range/transfer→invalid_argument, null-buffer→
   invalid_argument, EINTR retry, errno mapping, Result<size_t> return ABI, composition
   short-read/EOF/no-progress/effect-certainty rules — ≈18 wrapper instructions plus the
   ABI shape any matched implementation also pays (proven by A1 emitting the same).
4. **What excess can be safely deleted?** The verdict-dispatch restructure that keeps
   every check and error order but branches on the verdict enum in registers (removes
   the Precheck memory round-trip), and the offset-cast fusion replacing the second
   bound test with `static_cast<off_t>` on the already-validated execute path (D1).
   For composition, an internal pre-validated primitive invoked by `compose` under the
   outer precheck's established invariants (D2).
5. **Are there worthwhile optimization candidates?** Yes — measured, reproducible:
   +12.8 ns on the no-kernel 0 B path (t=148), +15.8…+19.3 ns on 1 B…4 KiB syscall
   paths (t=12–20, 16/16 pairs), +32 ns per composition step. Against ≈530–990 ns
   syscall-bound warm ops this is 1.6–3.6%.

## Preregistration vs outcome

Preregistered (before the paired series): A2−A1 ≈ +8–15 ns from ≈28 instructions —
detectable above the ≈2.5–3 ns A/A floor. Outcome: +15.8…+19.3 ns (slightly above the
instruction-only estimate; the stack round-trip adds store-forwarding latency beyond
retired-instruction cost). 64 KiB predicted <0.5% and below noise: confirmed (t=−0.66).

## Verdict

**ELISION_CANDIDATE_FOUND**

- C1 (D1-class, single mechanism): validation-path restructure of the four data ops
  (`read_at`, `write_at`, `read`, `write`) — verdict dispatch replaces the Precheck
  struct materialization; offset/size cast fusion removes the duplicate bound test
  (also applies to `resize`'s `checked_posix_offset` under `precheck_resize`).
  Public interface, error codes, error order, zero-length semantics unchanged.
- C2 (D2): composition pre-validated step primitive under the outer precheck's
  invariants. Mechanism-only benefit for composed calls; dududu P2 uses `read_at`.
- D3 (ABI/Result fast path): NO CHANGE — the sret/spill/store shape is emitted
  identically for the matched native control; it is the necessary cost of the public
  `Result<size_t>` contract, not excess over the necessary path.

## Reproduction

- Worktree: `/home/jnhu/Source/Sluice-600-wt` @ BASE_SHA, branch `sluice-600-direct-research`.
- Workspace: `~/sluice-600-direct-research/` (bench/, scripts/, evidence/, phase1/).
- Build: `xmake f -m release -o build-600 --liburing=n -y && xmake build sluice_core`.
- Ladder: `bench/ladder.cpp` (+`paired.sh`, `scripts/analyze_pairs.py`), fixture
  `mkfixture.cpp` → `/tmp/sluice600/data64m.bin` (64 MiB tmpfs).
- Governor/perf setup: performance governor, nmi_watchdog=0, paranoid=1.
- Old #593 and dududu R1 evidence untouched.
