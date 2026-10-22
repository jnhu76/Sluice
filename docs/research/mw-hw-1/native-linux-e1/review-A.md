# Review A — Semantics / Concurrency / Correctness (Gate R8)

**Reviewer:** independent review agent A — semantics/concurrency.
**Date:** 2026-10-22.
**Scope:** E1 `pending_control_count_` derived summary and zero-skip gate
(source audit), W-03 real-kernel notification-protocol correctness,
deterministic/mutant/adversarial evidence reproduction, frozen-reducer and
verdict-logic correctness, P5 sticky-state honesty, adversarial hunt for
semantic holes the campaign may have missed. Measurement methodology and
attribution are Review B's responsibility and were not re-adjudicated here.

**Campaign identity (all recomputed by me, all match):**

- BASE worktree `/home/jnhu/sluice-e1-e5/base` @ `381756370980f75e4beaf47310db4b8b9a99053d` (clean).
- Candidate worktree `/home/jnhu/sluice-e1-e5/candidate` @ `2caead59f9317665510af72968439f1252bc5bb9` (clean, branch `research/native-linux-e1-pending-control-skip`).
- E1 patch `docs/research/mw-hw-1/patches/e1-pending-control-skip.patch`
  sha256 `ea217da4252a885f73889ba28b569f848bf050f8913b61fa69fa8bd1b385695f` — recomputed, identical.
- `results/ab.jsonl` sha256 `076dd301…bc6d45` matches `provenance.txt` and the
  `evidence/` copy; 990 rows = 11 cfg × 2 arms × 15 reps × 3 phases + 15 latency rows.

## Independently reproduced

All commands run as the normal user on E5; no source, patch, evidence or
script file was modified. Scratch builds in `/tmp/rev-a`; named xmake targets
only in the candidate worktree.

| # | Command | Result |
|---|---|---|
| 1 | `xmake build -y uring_pending_control_parity_test` (candidate) | rc=0 |
| 2 | `xmake run uring_pending_control_parity_test` | rc=0; `ok` × 5, `all 5 uring pending-control parity tests passed` |
| 3 | `xmake build -y uring_pending_control_mut_missing_increment` / `..._missing_decrement` | rc=0 both |
| 4 | run `uring_pending_control_mut_missing_increment` | **rc=134**; `ok zero_pending_steady_state`, then `sluice::async::Request: destroying or overwriting a nonterminal request responsibility…` + `terminate called without an active exception` (incidental destructor abort — this is exactly why the named-oracle companion exists) |
| 5 | run `uring_pending_control_mut_missing_decrement` | **rc=1**; `FAIL: sqe_pressure_cancel_sets_and_keeps_pending` |
| 6 | g++ rebuild of `e1_mutant_oracle_test.cpp` against candidate sources, healthy + both `-DSLUICE_URING_MUTANT_PENDING_CONTROL_*` variants | healthy rc=0 `all e1 mutant-oracle checks passed`; missing_increment **rc=1** `FAIL parity oracle: pending_control_count_ diverged from the router scan after the sticky set (missing increment)`; missing_decrement **rc=1** `FAIL parity oracle: count did not retire with the issued intent (missing decrement)` |
| 7 | sha256 of my three freshly built oracle binaries vs `scripts/e1_mutant_oracle_{healthy,missing_increment,missing_decrement}` | all three pairs **byte-identical** |
| 8 | g++ rebuild + run of `e1_adversarial_test.cpp` against candidate sources | rc=0; binary byte-identical to `scripts/e1_adversarial_healthy`; output: `enter concurrent_cancel_storm`, `enter poison_with_pending_intent`, `enter shutdown_after_pending_intent_settles`, `probe shutdown_cancel_then_drain_wedged_probe: converged=0 request_terminal=0` |
| 9 | run `scripts/e1_wedged_probe_base` (BASE arm) | `probe base_wedged: converged=0 request_terminal=0` — same non-convergence as E1, reproducing correctness.md scenario 6 from the BASE side |
| 10 | `python3 scripts/e1_verdict_selftest.py` | rc=0; **14/14** counterfactuals pass |
| 11 | `python3 scripts/e1_verdict.py --w02 results --w03 results-w03/w03-combined.jsonl --p5 results-p5/p5.jsonl --semantic results/semantic-report.json --out /tmp/rev-a/verdict-rerun.json` | rc=0, stderr clean (`issues: []`); `w02=PASS w03=PASS p5=NOT_MEASURED overall=CONDITIONAL`; output **identical** to frozen `results/verdict.json` (all blocks, including semantic) |
| 12 | W-03 artifact integrity: sha256 of `w03.jsonl`, `w03-verify.jsonl` vs `provenance-w03.txt`; `cmp` of `cat w03.jsonl w03-verify.jsonl` vs `w03-combined.jsonl` | shas match recorded values (`3b949905…`, `38652227…`, combined `9ed02432…`); **pure concatenation** confirmed; 190 = 180 + 10 rows |
| 13 | W-03 verify-row provenance: parse all rows | 10 rows, 5 scenarios × 2 arms, every row `phase=verify mode=verify ok=true no_lost_wake=true`; perf file contains 0 verify-phase rows (no relabeling) |

Recomputed W-03 per-rep paired medians from the raw rows (my own pairing, not
the reducer's): D1 0.8508, D8 0.6146, 64KiB 0.9118 — all under the 1.03
bound; gate outcome identical to the reducer's rank-paired values.

## E1 mechanism audit (source-level)

Full state-machine read of candidate `src/async/uring_transport.cpp`,
`src/async/uring_completion.cpp`, `src/async/uring_backend.cpp`,
`src/async/uring_test_seams.hpp`, `include/sluice/async/detail/uring_backend.hpp`:

- **Exactly one set site and four clear sites for `control_pending`**, all
  under `dispatch_mtx_` (`issue_running_cancel_locked_` is reachable only from
  `cancel_key` (locked, :585) and `service_pending_controls_locked_`
  (locked call sites :88, :622); `retire_router_entry_` is reachable only from
  `finalize_operation_terminal_` :95, the control-CQE path :140, and the
  Class-A poison path :440, all under the mutex):
  1. sticky set :653-658 — `if (!route.control_pending)` guard, `++pending_control_count_`;
  2. prepared :675-680 — clear + decrement;
  3. control already prepared/submitted :626-631 — clear + decrement (defensive; see below);
  4. `retire_router_entry_` :573-576 — clear + decrement before the slot
     returns to the free list.
  The fifth textual write (:659-661, non-sticky clear) is dead in every build:
  `sticky_intent_retained()` is a compile-time constant (`uring_internal.hpp:69-77`),
  so `control_pending` can only be true when the constant is true, in which
  case the `else if` never fires. It preserves parity in all configurations.
- **Invariant `control_pending ⇒ control_state == none`** holds: the set site
  executes only after the :626 early return, and the only site that moves
  `control_state` away from `none` (:674) clears the flag in the same locked
  critical section. Hence site 3 is unreachable defensive code, and the
  retire-time decrement covers the real interleaving (original CQE settles the
  route while a sticky intent is still parked — exercised by
  `pending_control_retired_with_route` and the mutant-oracle retire scenario).
- **Zero-skip gate cannot drop a needed scan:** the gate (:691) is preceded by
  the fatal-error check (:689, poison semantics unchanged) and returns only
  when the count is 0; since the single set site increments under the same
  mutex, count==0 implies no router entry has `control_pending`. No path sets
  the flag without incrementing (the mutant macros are the only exceptions and
  exist precisely to be killed). When count>0 the authoritative router scan
  runs verbatim — the counter never decides which entry is serviced.
- **No counter bypass via slot reuse:** the wholesale `route = RouterEntry{}`
  at :191 touches only free-list slots, which entered the free list exclusively
  through `retire_router_entry_` (flag already cleared, count already
  decremented). No other wholesale router write exists.
- **No underflow/ABA:** every decrement is flag-guarded and mutex-serialized;
  the count is global so cookie/generation aliasing cannot pair a stale clear
  with a new set. `signal_ready_progress()` fires after the locked state
  change is released, so a woken service thread observes the updated count
  (mutex release/acquire ordering).
- **Seam audit:** `pending_control_count_for_test` / `pending_control_scan_for_test`
  and all other seams only read under the mutex; no test seam writes router
  flags. The perf/W-03/P5 harnesses link the production libraries without
  `SLUICE_ASYNC_INTERNAL_TESTING` hooks (`make_uring_context` path), so the
  gate's real-kernel behavior is exercised unmasked (W-03 gains, mechanism
  scans `scans=0 skips=100%` on the E1 arm).

I found **no fifth live transition, no unwinding path that retires without
decrementing, no ABA window, and no waiter/counter ordering hazard.** The
mechanism is a correct derived summary with a sound zero-skip gate.

**Named-oracle rule:** the stock parity suite kills `missing_decrement` with a
named test failure (rc=1) but kills `missing_increment` only via the
incidental destructor abort (rc=134). The protocol's named-oracle requirement
is discharged by `e1_mutant_oracle_test.cpp`, which I rebuilt from source and
which kills both mutants with their exact named parity messages and rc=1
(plus fail()+`_Exit` so no destructor can preempt the verdict). Requirement met.

**Adversarial scenarios assessment:** all four are non-vacuous.
`concurrent_cancel_storm` proves idempotence of the sticky set under 8
latched concurrent cancels (count stays exactly 1; the `!route.control_pending`
guard is what is really under test, since the mutex serializes transitions).
`poison_with_pending_intent` drives a real `-EINVAL` submit poison with a
sticky intent present and proves the intent retires only through route
retirement with parity intact. `shutdown_after_pending_intent_settles` proves
no stale count survives settle+drain. The wedged probe is honestly labeled a
behavioral-parity probe, not a gate, and I reproduced both arms' identical
non-convergence. Note: the binary's final line `all e1 adversarial scenarios
passed` is unreachable (the probe `_Exit(0)`s first); the pass signal is the
probe line plus exit 0.

**W-03 `no_lost_wake` oracle judgment:** real, bounded-liveness. Each verify
scenario submits work through the production context, the driver parks in a
real `epoll_wait(-1)` on the real notification fd, and the submitter waits on
a 10s-bounded condition for `request.ready()`. A genuinely lost notification
on a single-work scenario leaves the driver parked forever and the bounded
wait expires (`no_lost_wake=false`, `ok=false`); a merely delayed wake inside
the bound passes, which is the correct semantics for a liveness obligation.
`cancel_pending_pipe` exercises the real running-cancel control path; both
cancel-wins and read-wins outcomes are accepted. The runner rc-gates the
verify phase per arm (`run_e1_w03.sh` :125-128), so verify rows cannot be
silently skipped; `DriverHandle` teardown (`request_stop(drain)` before join)
wakes the parked owner, and `alarm(600)` bounds the process. Residual
weaknesses, none load-bearing: perf rows hardcode `no_lost_wake=true`
(external_loop_ab_e1.cpp :919/:925/:931) but the reducer only reads
`phase=="verify"` rows, so the constant is inert; `perf_phase` closes
`driver.epoll_fd` manually and then the `DriverHandle` destructor closes the
same fd again (benign EBADF on this path, but sloppy); the `idle_park_wake`
spin check tolerates +2 passes, a small fudge band.

**P5 honesty:** `P_POSITIVE_COST=NOT_MEASURED` is the correct status under the
frozen reducer's own rule (`read_p5`: `p_events_total == 0` ⇒ NOT_MEASURED
"P>0 never actually occurred") — every perf row has `p_events=0` because on
liburing 2.13 the cancels in the congestion storm resolve before a control
intent is needed. The mechanism explanation (internal CQ drain keeps the
app-visible SQ ring unsaturated, so `get_sqe` never fails on the control
path) is coherent with the sampled series (`p_max=0`,
`control_sqe_observations=0`, parity 0 violations) and with the mechanism
scans. The perf A/B (0.9907 per-rep median, 9/15 below 1) is correctly
treated as P=0 color, not P>0 cost. Recording NOT_MEASURED rather than
PASS/FAIL is the honest outcome.

## Findings

1. **P2 — Quantitative probe claims in `pending-control-pressure.md` and
   `decision.md` are not backed by archived evidence.**
   `pending-control-pressure.md` §"What was attempted" and §"Why sticky states
   do not occur here" claim "300+ cycles … `control_sqe_observations` up to
   42" and "the formal probe: 4 runs, 420+ samples"; `decision.md` repeats
   "4 seam-probe campaigns, 420+ counter samples". The only archived probe
   artifact (`evidence/p5-probe.jsonl` = `results-p5/p5-probe.jsonl`, single
   run) shows `samples=2, p_positive_samples=0, control_sqe_observations=0`,
   and `p5-sticky-not-observed.txt` records its rc=1. No file anywhere in the
   campaign trees contains a probe line with 420+ samples or 42 control-SQE
   observations (searched both worktrees and the docs branch). The
   *directional* conclusion (0 sticky observations, 0 parity violations) is
   supported by the archived artifact, and the verdict is unaffected (P>0 is
   NOT_MEASURED either way), but an evidence claim in a gate deliverable must
   be reproducible from the archive. Fix in-round: archive the round-log probe
   lines (with hashes) or reword the claims to what the archived artifact
   supports. Related note: the archived formal probe's sampling density is
   thin (2 monitor samples over a 1000-cancel storm — storm and monitor
   threads share the single pinned CPU with a busy poll loop), so the
   "sticky never occurs" conclusion rests mainly on the mechanism argument;
   this is acceptable only because the round does not trade on that conclusion.
2. **P3 — Reducer W-03/P5 pairing deviates from the protocol's per-rep
   pairing.** `e1_verdict.py:232` and `:282` sort each arm and zip rank-wise
   instead of pairing rep *i* with rep *i* (protocol §5 "true per-rep
   pairing"). W-02 does use true per-rep pairing (`cells[(rep,cfg,arm)]`).
   Verified outcome-neutral: per-rep medians 0.8508/0.6146/0.9118 vs
   rank-paired 0.8587/0.6171/0.9206, identical gate decisions, and the
   documented P5 0.9907 is in fact the per-rep median (the reducer never
   computes a P5 ratio on this data). Should be fixed or explicitly noted
   before the reducer is reused.
3. **P3 — Negative-control "no benefit" clause is not mechanically enforced.**
   Protocol: negative controls "must show no benefit and no harm"; the reducer
   enforces only ≤1.03 for all non-headline configs. A negative control with a
   large spurious benefit would pass. Observed controls are neutral
   (4096-16-1 = 0.9962, 1048576-64-8 = 0.9958), so no in-round impact.
4. **P3 — Selftest counterfactual coverage is partial for a fail-closed
   reducer.** 14/14 pass, but there is no counterfactual for `base_sha ==
   e1_sha` identity drift (check exists at `e1_verdict.py:92-93`),
   `arm-identity.txt` lacking hashes (:101-102), a W-03 verify row with
   `no_lost_wake=false` (the CORRECTNESS_FAIL path), a P5 `invariant_ok=false`
   row, or the headline `pairs_below_1 < 12` leg failing in isolation (the
   1.10× inflation breaks median and count legs simultaneously). Protocol
   requires "14 cases must pass", which is met; these are coverage notes.
5. **P3 — W-03 verify-row floor `len(verify) >= 3`** (`e1_verdict.py:227`) is
   not protocol-derived (the protocol constrains only "every verify row
   no_lost_wake=true"). Moot at 10 actual rows.
6. **P3 — Adversarial harness cosmetics** (documented so future readers do not
   misread): unreachable final success line; adversarial/probe raw outputs are
   not in `evidence/` (I re-derived both arms' probe behavior myself, so the
   correctness.md claims are verified, but the raw lines cited as "recorded in
   the round log" are not archived — same class as Finding 1, lower stakes);
   the storm's parity monitor reads count and scan in two separate locked
   calls, which could theoretically false-flag on a between-call mutation —
   impossible in this scenario (mutations stop before the threads start) and
   the error direction is fail-closed.

No P1 findings. I specifically hunted for and ruled out: a fifth live
`control_pending` transition; error/poison unwinding that retires a flagged
entry without decrementing; ABA between sticky-set and retire via slot/generation
reuse; waiter wakeup vs counter ordering; production-only paths masked by test
seams (perf/W-03/P5 harnesses use production libraries; seams exist only in
the deterministic suites, where they are the declared mechanism under test).

## Protocol deviations

- Declared in protocol.md and verified clean: w03-combined concatenation
  (pure, sha recorded), p95 provenance, tracefs NOT_RUN.
- Undeclared deviations found: the W-03/P5 rank-pairing vs per-rep pairing
  (Finding 2) and the verify-row floor (Finding 5). Neither changes any gate
  outcome on this data (I recomputed both ways).
- `results/r1-out/` referenced by the round task does not exist in
  `results/`; the raw suite outputs are flat `r1-out-*.txt` files (archived
  under `evidence/r1-out/`). Cosmetic.

## Verdict

**APPROVE_WITH_CONDITIONS.** The E1 mechanism is semantically correct as
claimed: I verified by reading every `control_pending`/`control_state`
transition that the derived count is maintained at the single set site and all
clear sites under `dispatch_mtx_`, that the zero-count gate can never skip a
needed scan, and that the counter stays non-authoritative; I reproduced the
parity suite, both mutant kills (including the named-oracle kills from freshly
built, byte-identical binaries), the adversarial scenarios, the BASE/E1 wedged
parity, the 14/14 selftest, and the exact frozen verdict from the raw evidence,
and my independent per-rep recomputation confirms every gate decision. W-03's
`no_lost_wake` is a genuine bounded-liveness oracle fed by genuine verify-phase
rows, and `P_POSITIVE_COST=NOT_MEASURED` (hence overall `CONDITIONAL`, never
`NATIVE_GO`) is the honest protocol-mandated outcome. The single P2 is an
evidence-hygiene defect (unarchived probe sample-count claims), not a
correctness defect; it must be fixed in-round (archive the probe lines or
reword the claims) and the reducer pairing deviation noted or fixed before the
reducer is reused, but neither changes the verdict as stated.

## Residual limitations

- Tracepoint-level kernel attribution was not verifiable by me (tracefs
  unreadable for a normal user; protocol forbids sudo) — accepted as NOT_RUN
  per protocol.
- I did not re-run the W-02/W-03/P5 campaigns themselves (each takes pinned-CPU
  time and the artifacts are hash-pinned and fail-closed); I verified artifact
  integrity, reducer reproduction, and recomputed the statistics from raw rows.
- The W-03 `no_lost_wake` scenarios cannot prove the absence of a lost
  notification that is masked by an unrelated subsequent wake; they prove the
  preregistered bounded-liveness obligation on the exercised interleavings.
- The sticky (P>0) path's real-kernel cost remains unmeasured on this stack;
  that is exactly what the round's CONDITIONAL verdict and the decision.md
  follow-up obligation record, and nothing in this review changes it.
- The mech instrumentation binaries (`pipeline_mech_*`) were spot-checked via
  `mechanism-scans.txt` consistency (BASE iters == calls × capacity exactly,
  e.g. 169255 × 1024 = 173,317,120) but their build was not independently
  re-derived; attribution details are Review B's scope.
