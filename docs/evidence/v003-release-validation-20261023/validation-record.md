# Sluice v0.0.3 release validation record

Date: 2026-10-23. All gates executed on the frozen release candidate
`release/v0.0.3` head `3a108176eef9c75ef12d87471606cf9da75a7c82`, in
isolated worktree and build directories on the native E5 host. Owner
checkout untouched.

## Identity

| item | value |
|---|---|
| Base (master at validation start) | `5a75e3033ac7e525e1e4a04095a2edc4f76ed07d` |
| Release candidate head | `3a108176eef9c75ef12d87471606cf9da75a7c82` |
| RC delta vs master | `CHANGELOG.md` (+93) and `tests/external_loop_stop_order_test.cpp` (2+/2−) only |
| Production content vs master | identical (`git diff 5a75e303..3a108176 -- src include apps bench xmake.lua xmake/` empty) |
| Working tree at validation | clean; `xmake` config per cell, `--ccache=n` |

## Environment (probed on site)

Bare-metal E5 (Xeon E5-2666 v3), Fedora Linux 44, kernel
`7.2.11-200.fc44.x86_64`, GCC 16.2.1 20260819, clang 22.1.8, xmake
3.1.2+HEAD.5d21e9d, system liburing 2.13, 20 logical cores.

## R5 — 8-cell release matrix: 8/8 PASS

Per cell: `xmake f -m <mode> --liburing=<y|n> --toolchain=<gcc|clang>
--ccache=n --builddir=<cell-dir> -y`, then `xmake build -y -g test`, then
`xmake test -y`; full stdout/stderr receipts and per-stage exit codes kept
(set manifest sha256 `d433e6a7e04068db5738e31760f5eaab4682f750a3dec98d2400f0f0b28a9eb1`).

| cell | configure | build | test | tests passed |
|---|---|---|---|---|
| debug-n (gcc) | 0 | 0 | 0 | 58/58 |
| debug-y (gcc) | 0 | 0 | 0 | 78/78 |
| release-n (gcc) | 0 | 0 | 0 | 58/58 |
| release-y (gcc) | 0 | 0 | 0 | 78/78 |
| asanubsan-n (clang) | 0 | 0 | 0 | 58/58 |
| asanubsan-y (clang) | 0 | 0 | 0 | 78/78 |
| tsan-n (gcc) | 0 | 0 | 0 | 58/58 |
| tsan-y (gcc) | 0 | 0 | 0 | 78/78 |

Zero failed, zero skipped in every cell (skip lines counted: 0). The new
test count relative to v0.0.2 (56/74) is +2 (n cells:
`request_core_init_closure_test`, `external_loop_stop_order_test`) and +4
(y cells: additionally `uring_owed_counter_parity_test`,
`uring_pending_control_parity_test`).

### First-round finding (test-only, fixed on the RC)

The first matrix round (RC `e6b10432`) failed both asanubsan builds at
compile time (exit 255): under the registered
`SLUICE_EXTERNAL_LOOP_MUTANT_STOP_FLAG_AFTER_WAKE` mutant build,
`kDriverExitBudget` in `tests/external_loop_stop_order_test.cpp` becomes an
unused const variable and clang `-Werror -Wunused-const-variable` rejects
the TU; GCC (all six gcc cells) does not enable that warning in C++, which
is why the round otherwise passed. Classification: test-only compile
defect in a manually-executed mutant build that had never been compiled
under clang (H2b recorded it on GCC/WSL2); no sanitizer-runtime interposer
conflict, no production code involved. Fix (RC `3a108176`): the constant
moved into the fixed-order `#if` arm (the mutant arm keeps its inline
deadlines; `kCompletionBudget` stays shared — fixture start waits under
it). Both arms syntax-checked under clang `-Wall -Werror`, and all 8 cells
re-ran clean on `3a108176`. The failed-round receipts are retained
(matrix round-1 set) as the reproduction record.

## R6.2 — clean-room package verification: PASS (both profiles)

`python3 scripts/verify_v2_package.py --mode release [--liburing]
--manifest-out … --archive-baseline-out …`, plus
`python3 scripts/test_verify_v2_package.py` (self-test). Exit 0 for all
three. Manifests record `VERIFICATION_HEAD 3a108176…`, clean tree,
GCC 16.2.1, kernel 7.2.11. All 24 gates True in each profile:
exactly-11-header install set, per-header standalone compile, archive
baselines, clean-room core-only (P0) and async consumers with
angle-bracket includes and no source/build-tree dependency, the contract
workload battery (lifetime, admission, cancel effect, observer, progress,
shutdown, durability, backend availability, W-03 external loop, W-02
pipeline), `V2_P1_URING_EXPLICIT_NOT_SUPPORTED` (n profile) /
`V2_P2_URING_RUNTIME` (y profile), ODR public-layout macro-independence,
and the negative probes (removed headers unavailable, seam unavailable,
mutant defines not installed, async symbols not in core).

## R6.3 — formal gates: PASS (re-run on this candidate)

- `bash scripts/verify_tla.sh` → `VERIFY_TLA: PASS`, exit 0. Re-run rather
  than carried: `src/async/detail/request_core.cpp` changed in this delta
  (DP-1 observation metadata, DP-3A reserve-reset removal), so the frozen
  `28f0fe33` carry does not apply to v0.0.3. The modeled transitions are
  unchanged (reserve still draws a fully cleared slot; publication
  out-form retained), and the fresh PASS covers RequestCore safety
  (31,289,609 generated / 5,359,732 distinct states, depth 53) and the
  12-branch liveness plus every named-mutant and coverage negative control.
  rcore-live wall ≈ 37 min with `SLUICE_TLA_WORKERS=8` (TLC progress
  timestamps 22:41→23:18 local; temporal-branch sweeps 1–2 min each).
- `bash scripts/verify_progress_source_model.sh` → exit 0, "all expected
  outcomes reproduced" (including the `MutRepeatCloseSignalsPark`
  restored-pump discriminator).

## R6.1 — correctness, mutations and applications

The matrix exercises Direct File semantics, RequestCore lifecycle, slot
generation, publication, observer delivery/retirement, ThreadPool and
io_uring backends, cancellation/sticky intent, W-03 no-lost-wake, shutdown
and retained results, zero-op, poison/recovery and RequestScope; the four
reference applications are accepted below. H2b counter parity
(`uring_owed_counter_parity_test`), E1 pending-control parity
(`uring_pending_control_parity_test`), the DP-3A initialization-closure
oracle (`request_core_init_closure_test`) and the #571 deterministic
stop-order regression (`external_loop_stop_order_test`) all run inside the
matrix cells.

### Named mutation gates: 19/19 killed on this candidate, signatures as registered

All mutation targets re-run on the RC (every one is new or touched by this
delta, so no carry-forward was used). Binaries from the release-y cell;
binary sha256 per target in the receipt set (manifest
`09dc0e82b5a2b60d9a81870478d1ccf080a6d07dfa42b43fac59728a81af7863`).

| mutant | exit | observed kill (registered signature) |
|---|---|---|
| external_loop_stop_order_mut_stop_flag_after_wake | 2 | "STOP_FLAG_AFTER_WAKE detected: driver consumed the request_stop wake … re-parked in epoll_wait(-1) with stop=false" |
| request_core_mut_release_skips_outcome_clear | 1 | named closure-oracle assertion failures (stale outcome into re-acquire) |
| request_core_mut_release_skips_metadata_clear | 1 | metadata assertion of the closure oracle |
| request_core_mut_release_skips_observer_clear | 1 | stale `queued` observer phase blocks reclaim |
| request_core_mut_rollback_without_release | 134 | "rolled-back slot is clean with generation+1", "the reservation identity is dead", "rolled-back slot returns to service", then teardown abort |
| request_core_mut_exhausted_slot_reused | 134 | repeated "the pre-saturated generation is served" / "the slot re-enters the free list at the saturated generation", then abort |
| request_core_mut_constructor_dirty_slots | 1 | first-reserve/accept assertions on dirty constructed slots |
| uring_publish_mut_foreign_key | 142 | 8/9 scenarios pass, then the harness `alarm(120)` bounded-liveness watchdog (lost observer retirement) — same class as the DP-1 record |
| uring_publish_mut_begin_skipped | 134 | "core handoff reached a protocol-unreachable state (invariant violation)" |
| uring_owed_mut_missing_increment | 142 | empty-output watchdog kill — byte-identical to the H2b registered receipt for this mutant (drift-below parks settlement; supervision alarm) |
| uring_owed_mut_missing_decrement | 1 | "FAIL: counter_tracks_zero_op_reads" |
| uring_owed_mut_empty_verdict_always | 1 | "FAIL: a_zero_op_after_sweep_is_immediate_residue" |
| uring_owed_mut_reclaim_ignoring_control_refs | 134 | 4 scenarios pass, then the submit-reset fail-fast ("core handoff … invariant violation") |
| uring_owed_mut_zero_op_wake_dropped | 1 | "FAIL: external_host_sees_userspace_publication_wake" |
| uring_pending_control_mut_missing_increment | 1 | "FAIL parity oracle at sticky_lifecycle: derived …=0 but authoritative router scan=1" |
| uring_pending_control_mut_missing_decrement | 1 | parity oracle, derived 1 / scan 0 |
| uring_pending_control_mut_gate_always_skips | 1 | released pressure did not issue the pending control SQE (lost intent) |
| uring_pending_control_mut_double_increment | 1 | parity oracle, derived 2 / scan 1 |
| uring_pending_control_mut_retire_stale_count | 1 | parity oracle at repeated_cancel |
| healthy controls (5 suites) | 0 | external_loop_stop_order_test, request_core_init_closure_test, uring_external_loop_test, uring_owed_counter_parity_test, uring_pending_control_parity_test |

### Reference applications: accepted on both release profiles

Fixtures: 8 MiB random binary and a 2000-line text with 111 `line_19` rows
(digests in the receipt set). On release-y (and release-n where noted):
`sluice-copy` default / `--pipeline-depth 4 --buffer-size 65536 --workers
2` / `--pipeline-depth 2 --sync all` all byte-identical to the input
(cmp exit 0), including the liburing=n binary; `sluice-hash` equals the
fixture sha256; `sluice-grep line_19` counts 111 = coreutils grep;
`sluice-tail -n 20` byte-identical to coreutils tail; `sluice-tail -n 0
-f` observes appended data after start.

## R6.4 — performance sentinels: PASS

`SLUICE_BENCH_JSONL=… <builddir>/linux/x86_64/release/bench_*` on the
release-y build, stats accounting off (`SLUICE_BENCH_STATS` unset → 0),
each run gated by the built-in CAL oracle self-test; exit 0 each:
`bench_w01_direct` (145 rows, "failing repetitions=0"),
`bench_w02_pipeline` (145 rows, full default case set),
`bench_w03_external_loop` (55 rows, both backends).

Workload consistency: case identities match the recorded E5 campaigns
(`W02-uring-4k-d1`, `W02-uring-4k-d16`, `W02-uring-64k-d16`,
`W02-threadpool-4k-d16-w4`, …). Medians this run (unpinned):
uring-4k-d1 10 095 ns/op, uring-4k-d16 2 719 ns/op, uring-64k-d16
11 291 ns/op, threadpool-4k-d16-w4 3 265 ns/op. For calibration, the
DP-3A gate campaign (pinned `taskset -c 2`) recorded 6 894–7 026 ns/op
(4k-d1) and 2 964–3 063 ns/op (4k-d16) — same magnitude; the residual is
core-placement variance on the latency-sensitive d1 shape, not a workload
or runner change (same case definitions, same bench version).

**Performance-claim boundary.** No v0.0.2-vs-v0.0.3 paired A/B was run
for this release, so no whole-version performance claim is made. The
release notes cite each mechanism's scoped, already-validated evidence:
H2b formal E5 A/B (headline 0.495 at 4 KiB/C1024/D1, all 11 configs
≤ 0.994), E1 product A/B (W-02 headline 0.698/0.620, user-space cycles
0.350x, native P>0 cost NOT_MEASURED with the seam bound 0.938–0.961 as
the recorded evidence class), DP-1 compiler proof (120-byte payload init
gone, frame 232→96 B), DP-3A direct incremental A/B against the
master+DP-1 baseline (wall 1.00142 [0.99349, 1.00539], −20.9 instr/op).
Mechanism presence on this candidate rests on the empty production diff
vs master `5a75e303` plus these sentinels.

## Release receipt integrity

Full receipt sets (matrix: configure/build/test argv+exit+log per cell,
host versions, source SHA; gates: formal, package, mutants, apps,
sentinels) were retained locally with SHA256SUMS manifests:
matrix `d433e6a7e04068db5738e31760f5eaab4682f750a3dec98d2400f0f0b28a9eb1`,
gates `09dc0e82b5a2b60d9a81870478d1ccf080a6d07dfa42b43fac59728a81af7863`.
No raw logs, JSONL or runners are committed with this release; this
record carries the verdicts, counts and commands needed to reproduce.

## Reviews

- Review A (correctness/contract): APPROVE — recorded in the merge PR.
- Review B (release engineering/evidence): APPROVE — recorded in the
  merge PR.
