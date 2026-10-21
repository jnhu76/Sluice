# H2b productization — derived owed-event count eliminates empty sweeps (#564)

Correctness-preserving performance change for the io_uring backend. The
`poll()` delivery sweep previously scanned all `capacity_` delivery records on
every pass; H2b adds a derived counter `owed_count_` so the sweep runs only
when at least one owed event exists, and `poll_progress()`'s
`immediate_work_remains` verdict consults the same summary.

- Issue: #564 (references #563, #559, #561, PR #560, PR #562 / M2 research)
- Branch: `perf/uring-owed-count-skip`; BASE = master `c8970702`
  (`c8970702aad4fa74d4053d00ae941409e709ce67`)
- Commits: `d3603249` perf(uring): skip empty owed-event sweeps;
  `4edb957d` test(uring): admit owed-count parity and W-03 oracles;
  docs commit (this record + ledger).
- PR: held OPEN for owner merge. Auto-merge is forbidden.

Affected root IDs: PROG-01 (finite pass bound + honest residue — the
uring owed-residue row), PROG-02 (no lost wake — unchanged and re-evidenced),
W-02/W-03 invocation forms (unchanged), THREAD-01 (progress owner; concurrent
submit — unchanged), BACKEND-04 (mechanism-not-contract: `owed_count_` is a
derived summary and is NOT settlement, shutdown or destruction authority —
`~UringAsyncBackend` and `internal_work_retired()` keep the authoritative
`any_event_owed_locked_()` scan).

## 1. The O(C) scan and the minimal change

Source of the scan: `UringAsyncBackend::poll()` iterated
`for i in [0, capacity_)` over `delivery_` records on every pass, taking and
releasing `dispatch_mtx_` per slot, to find `event_owed` records left by
inline-published zero-length ops. In scan-heavy shapes (4 KiB reads,
C = 1024, depth 1) most passes have zero owed records, so the scan is pure
overhead proportional to capacity.

Change (see `h2b-production.diff`):

1. New member `std::size_t owed_count_ = 0;` next to `delivery_`.
2. `publish_zero_op_inline`: inside the existing single `dispatch_mtx_`
   critical section that sets `record.event_owed = true`, also `++owed_count_`.
3. `poll()` sweep: read `owed_count_ != 0` once under `dispatch_mtx_`; run the
   per-slot sweep only when nonzero; decrement inside the same critical
   section that clears `record.event_owed`.
4. `poll_progress()`: `immediate_work_remains` uses
   `!publication_pending_->empty() || owed_visible_locked_()` (the counter
   guarded by `SLUICE_URING_MUTANT_OWED_EMPTY_VERDICT_ALWAYS`).
5. `submit_request` delivery-record reset: if a freshly reserved slot still
   carries `event_owed`, call `detail::uring_core_handoff_fail_fast()` (P3
   below). No silent normalization.
6. Test seams `owed_count_for_test()` / `owed_scan_for_test()` (locked
   readers) next to the existing `*_for_test` seams.

No dynamic allocation, no new mutex/atomics/threads, no configuration, no
public API change. `any_event_owed_locked_()` remains the authority;
`owed_count_` never decides settlement, shutdown or destruction.

## 2. Invariant and transition table (P1)

Invariant I: at every `dispatch_mtx_`-protected observation point,
`owed_count_ == Σ_i [delivery_[i].event_owed]`.

| # | Transition | Site | Counter effect |
|---|---|---|---|
| T1 | Init | backend construction | `owed_count_ = 0`, all records false — I holds |
| T2 | Zero-op publish | `publish_zero_op_inline`, one `dispatch_mtx_` CS sets flag + `++owed_count_` together; the slot's control reference is acquired (`core_->acquire_control`) before the CS | flag and count move together — I preserved |
| T3 | Consume | `poll()` sweep, one CS per record clears flag + `--owed_count_`, then delivers and releases the control ref | I preserved; count hits 0 exactly when no flags remain |
| T4 | Duplicate publish (slot reset) | `submit_request` reset block under `dispatch_mtx_` | reserved slot must be flag-free (T7); fail-fast otherwise; no counter change |
| T5 | Slot reuse before delivery | `RequestCore::reclaimable_` requires `control_refs == 0`; the owed control ref pins the slot until T3 releases it, so `reserve()` cannot hand out a flagged slot | T4's guard is protocol-unreachable in pristine code; reachable only under the M4 mutant, where it fires (receipt below) |
| T6 | Cancellation of an owed request | zero-op cancel is already-terminal at submit; the publication and owed event stay intact; delivery happens exactly once in T3 | counter untouched — parity suite `cancel_leaves_owed_delivery_intact` |
| T7 | Publish during a pass | `publish_zero_op_inline` may run while a sweep is in progress: the snapshot `owed_present` may already have been read as false | the record flag is authority; `signal_ready_progress()` makes the pass's owner re-poll; the next pass delivers. PROG-02 re-evidenced by W-03 oracles |
| T8 | Shutdown / destruction with owed outstanding | `~UringAsyncBackend` and `internal_work_retired()` keep the full authoritative scan | no counter involvement (BACKEND-04) |
| T9 | Concurrent producers | every counter mutation is inside a `dispatch_mtx_` CS together with its flag mutation | I holds under any interleaving — parity suite `concurrent_producers_keep_exactly_once` (500 ops, C = 64, 2 threads) |

No legal observable state has `owed_count_ == 0` with an undelivered
`event_owed == true` record: the count and the flags only change together
inside one mutex CS (T2/T3), so `owed_count_ == 0` implies all flags false
under the same lock the sweep uses to decide entry.

## 3. The submit-reset branch — Scheme B (unreachable, loud)

Claim: in `submit_request`'s reset block, `record.event_owed` is always false
for a reserved slot. Proof sketch: `publish_zero_op_inline` acquires one core
control reference BEFORE setting `event_owed` (T2); the only release of an
owed control reference is the sweep, strictly after clearing the flag (T3);
`RequestCore::reclaimable_` requires `control_refs == 0`; only
`release_slot_` returns slots to the free list and `reserve()` only pops free
slots — therefore `reserve()` cannot return a flagged slot. The branch is
implemented as a fail-fast (reusing `uring_core_handoff_fail_fast`), not a
silent decrement or an assert: if any future mechanism breaks the pinning
chain, the context terminates loudly instead of double-decrementing or
corrupting delivery. The M4 mutant
(`SLUICE_REQUEST_CORE_MUTANT_RECLAIM_IGNORES_CONTROL_REFS`) makes the state
reachable and the guard fires — proving the guard is load-bearing, not
defensive insurance for a state that could never be observed by any means.

## 4. Counter parity oracle and mutant receipts (P4)

`tests/uring_owed_counter_parity_test.cpp` — the parity oracle grounds the
counter in external observations (publication, delivery counts, control
release, results, slot reuse, shutdown convergence), never in internal call
sequences; `counter_matches_authority` compares the seam value against the
authoritative scan AND requires the scan to be empty iff the count is zero.
Eight scenarios: K=3 zero-op reads incl. no-op re-poll; data ops ignored;
zero-length write owed; K=C=8 full capacity sweeps exactly once; slot reuse
before delivery (C=1, also the M4 killer); cancellation keeps owed delivery
intact; owed-during-pass via DeliverySweepEpiloguePauseGate; 500-op
concurrent producers.

W-03 admission: 4 new scenarios in `tests/uring_external_loop_test.cpp` —
ack does not consume undelivered owed obligations (exactly-once after ack);
zero-op submitted from another thread during a paused pass is served and
leaves honest residue (owner transfer checked); wait_one serves owed without
parking past it; observer + zero-op + shutdown settle together (notification
interest detached before shutdown, per SHUT contract). No sleeps: pause gates
and atomic events only. Suite: 9/9 scenarios, 5 consecutive runs stable.

Mutants (receipts in `mutant-receipts.txt`, run on the clean releasedbg
build; killed deterministically):

| Mutant | Break | Killer | Exit |
|---|---|---|---|
| M1 `SLUICE_URING_MUTANT_OWED_COUNT_MISSING_INCREMENT` | publish does not count | parity suite: counter under-reports; settlement cannot converge; in-test `alarm(120)` fires | 142 (SIGALRM) |
| M2 `SLUICE_URING_MUTANT_OWED_COUNT_MISSING_DECREMENT` | sweep does not uncount | parity suite: `counter_matches_authority` fails (`counter_tracks_zero_op_reads`) | 1 |
| M3 `SLUICE_URING_MUTANT_OWED_EMPTY_VERDICT_ALWAYS` | false empty verdict | residue oracle `uring_owed_residue_test` (`a_zero_op_after_sweep_is_immediate_residue`), NOT a counter comparison | 1 |
| M4 `SLUICE_REQUEST_CORE_MUTANT_RECLAIM_IGNORES_CONTROL_REFS` | reclaim ignores owed pin | parity suite reaches the submit-reset fail-fast → SIGABRT | 134 |
| M5 `SLUICE_URING_MUTANT_ZERO_OP_PUBLICATION_DROPS_WAKE` | lost wake | W-03 external loop suite → progress-binding contract violation → SIGABRT | 134 |

Negative control: M4 deliberately breaks the control-ref precondition and the
system detects it loudly rather than proceeding.

## 5. Validation battery (P6)

Per-cell receipts in `battery-*.txt` (commands + result). Full suite:
`xmake build -y && xmake test`.

| Cell | Result |
|---|---|
| releasedbg + liburing=y | 75/75 |
| release + liburing=y | 75/75 |
| release + liburing=n | 56/56 |
| debug + liburing=y | 75/75 |
| tsan + liburing=y | 75/75 |
| asan + liburing=y | 74/75 — `app_tail_cli_oracle_test` fails identically at BASE `c8970702` (pre-existing ASAN/LD_PRELOAD shim conflict on this host; both trees pass the other 74) |

Process note recorded for honesty: an intermediate reconfiguration in this
session left `sanitizers = "asan"` persisted from the asan matrix cell, which
first surfaced as the tail-shim failure in an ostensibly clean cell and cost
one investigation round (root cause: xmake option persistence, not the
change; base worktree without the flag passed, then `xmake f -c` + rebuild
made all non-asan cells green again). All receipts above were captured AFTER
the clean reconfiguration.

Formal correspondence: no model change. H2b adds an implementation-local
derived summary; the semantic actions PublishOwed / ConsumeOwed / SkipScan map
to existing core model actions (publish, deliver, no-op pass) — the modeled
action alphabet and protocol transitions are unchanged, so the existing
bounded results remain the model evidence; this record documents the mapping.

## 6. Performance (P7)

### 6.1 Provisional paired A/B — WSL2 host (status: PROVISIONAL_WSL2_NOT_E5)

Host: AMD Ryzen 7 5800H, kernel
6.18.54.1-microsoft-standard-WSL2, g++ 15.2.0, `-O3 -DNDEBUG`, CPU 2 pinned,
15 reps, paired interleaved arms with rotation, byte oracle gated every
repetition (870/870 rows ok). Arms: base = worktree at `c8970702`, h2b =
worktree at `4edb957d`, both `-m release --liburing=y`, `sanitizers = ""`
(verified in both worktree configs). Frozen script, provenance and reduced
summary in `provisional-wsl2-ab/` (script, provenance.txt, ab-summary.json,
zero_burst_ab.cpp source; raw 870-row ab.jsonl regenerated by the script).

Median h2b/base ratios (15 pairs each; `cpu_ns_per_op`, wall agrees):

| block | cap | depth | cpu median [min,max] |
|---|---|---|---|
| 4096 | 1024 | 1 | **0.569** [0.495, 0.612] |
| 4096 | 1024 | 8 | 0.664 [0.626, 0.721] |
| 4096 | 256 | 1 | 0.805 [0.771, 1.026] |
| 65536 | 1024 | 1 | 0.681 [0.595, 0.746] |
| 4096 | 64 | 1 | 0.920 [0.881, 0.943] |
| 65536 | 64 | 1 | 0.955 [0.860, 0.993] |
| 4096 | 16 | 1 | 0.980 [0.901, 1.145] |
| 4096 | 16 | 8 | 0.980 [0.895, 1.106] |
| 1048576 | 64 | 1 | 1.002 [0.590, 1.215] (I/O-dominated, 500 reqs) |
| 0 (K>0 zero burst) | 1024 | 8 | 0.936 [0.845, 0.986] |
| 0 (K>0 zero burst) | 1024 | 64 | 0.978 [0.920, 1.141] |

No config regresses at median; the K>0 zero-op burst (negative performance
workload) is neutral-to-better, confirming the K>0 delivery path keeps its
baseline. Wins scale with capacity exactly where the O(C) scan predicts.
Higher `poll_calls` on h2b in some configs are expected: cheaper polls allow
more driver iterations over the same phase. 0 allocations both arms.

Limitations of this run: WSL2 host, not the E5 native Linux environment;
`perf` hardware counters unavailable (perf_event_paranoid=2, no interactive
sudo in this autonomous session) so instructions/cycles attribution was not
captured; CPU frequency not governed. Provenance note (Review B finding B1):
the campaign script predated the per-arm hash capture step, and the base
worktree's persisted xmake config was mutated after the campaign by the
sanitizer incident documented in §5 — the sanitizer-cleanliness of the
campaign arms is therefore substantiated by binary inspection (zero ASAN
symbols in all four campaign binaries; `nm` symbol delta between arms is
exactly one symbol), and the formal E5 script below restores the
`arm-identity.txt` hash step and refuses to run on a sanitizer-tainted
config.

### 6.2 Formal E5 acceptance — NOT_RUN

The E5 credentials are supplied by the execution environment's interactive
authentication and were not available in this autonomous session (same
disposition as M2's `W03_AB = NOT_RUN`). The formal protocol is frozen as a
runnable script, `run_e5_ab.frozen.sh` in this directory (Review B findings
B1/B3): it parameterizes worktree root / output dir / reps / cpu, records
per-arm worktree SHA + binary/library SHA-256 into `arm-identity.txt`, refuses
a sanitizer-tainted config, runs the same 9 data + 2 zero-burst configs with
rotation and the byte-oracle gate, captures `perf stat`
(task-clock, instructions, cycles) on the 4096/C=1024/d=1 headline when
counters are permitted, and states the acceptance rule in advance:

- median `cpu_ns_per_op` ratio ≤ 1.03 for every config, AND
- sign test over the 15 paired ratios: at most 10 of 15 pairs above 1.03 per
  config (majority-regression rejected at the 5% level).

BASE is the unmodified current master — the E5 numbers must be regenerated,
not imported from M2's recorded SHA-specific data. The K>0 zero burst is
mandatory in every run. Owner steps: create `base`/`h2b` worktrees (base =
`c8970702`, h2b = the merge SHA), configure both
`xmake f -m release --liburing=y -y`, then run the script; record the PASS/FAIL
verdict in the ledger PROG-01 row.

## 7. Reviews

See `review-A-concurrency.md` and `review-B-performance.md` in this
directory (independent reviews; verdicts recorded verbatim with findings and
resolutions).

## 8. Remaining limitations

- Formal E5 A/B acceptance is NOT_RUN until the owner executes §6.2.
- The asan cell's tail-shim conflict is a pre-existing host issue, recorded
  with base-reproduction evidence.
- `owed_count_` remains a derived summary: correctness never depends on it
  (the delivery records and the authoritative scan decide). Its parity is,
  however, liveness-load-bearing, not merely a performance hint: were the
  counter to drift below the true owed count, `immediate_work_remains` could
  read false while the authoritative scan still sees owed records and the
  settlement driver would park forever instead of converging — the M1 kill
  demonstrates exactly this failure mode as a hang, bounded by the in-test
  `alarm(120)` (Review A reproduced the parked driver on the notification
  fd). The invariant proof (§2) is what makes the skip safe, the parity
  oracle pins the counter to the authoritative scan and external deliveries,
  and the submit-reset guard converts the one dangerous drift class into a
  loud termination.
