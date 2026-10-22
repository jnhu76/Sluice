# Independent Review A (semantics/concurrency) — Sluice E1 pending-control productization

- Reviewer role: INDEPENDENT Reviewer A (semantics/concurrency). Did not author the change.
- Date: 2026-10-23
- Worktree: `/home/jnhu/sluice-e1-product`, branch `perf/e1-pending-control-zero-scan`, HEAD `92b42cfb` (working tree clean; only `build/` touched, which is ignored). Baseline for diffing: `859e4b56` (master).
- Production-code commit: `f62adb91` (later commits `91bb4a92`, `a9491dc7`, `92b42cfb` are docs/evidence only; `git diff f62adb91..HEAD -- src include xmake tests` is empty, so all receipts recorded at `f62adb91` are valid for HEAD).

## Commands actually run (exit codes)

| Command | Exit |
|---|---|
| `git -C /home/jnhu/sluice-e1-product diff 859e4b56..HEAD --stat` | 0 |
| `git -C /home/jnhu/sluice-e1-product status --short` | 0 (clean) |
| `xmake config -m release --liburing=y -y` ; `xmake build -y uring_pending_control_parity_test` | 0 / 0 |
| `./build/linux/x86_64/release/uring_pending_control_parity_test` | 0 (7/7 ok) |
| `xmake config -m tsan --liburing=y -y` ; `xmake build -y uring_pending_control_parity_test` | 0 / 0 |
| `./build/linux/x86_64/tsan/uring_pending_control_parity_test` × 5 runs | 0 each, stderr empty (0 tsan reports) |
| `xmake config -m release --liburing=y -y`; for each of the 5 mutants: `xmake build -y uring_pending_control_mut_<name>` then `timeout 120 ./build/linux/x86_64/release/uring_pending_control_mut_<name>` | build 0; run exit 1 each (receipts below) |
| `xmake build -y uring_external_loop_test` ; `./build/linux/x86_64/release/uring_external_loop_test` | 0 / 0 (9/9 ok) |
| `xmake build -y uring_progress_race_test` ; run | 0 / 0 (23/23 ok, adjacent regression) |
| `xmake build -y uring_owed_counter_parity_test` ; run | 0 / 0 (8/8 ok, adjacent regression) |
| `nm -D build/linux/x86_64/tsan/uring_pending_control_parity_test \| grep -c __tsan` | 28 (instrumentation verified) |
| `diff` of `docs/evidence/.../e1-production.diff` vs actual `git diff 859e4b56..HEAD` over the 5 production files | 0 — byte-identical (modulo index lines) |

Note on the mandated tsan command sequence: `xmake config -m tsan -y` alone silently resets the liburing option (default `false`), which makes all uring targets disappear; and the tsan binary is emitted to `build/linux/x86_64/tsan/`, not `.../release/`. Following the literal instruction path runs a stale release binary and tests nothing under tsan. I re-ran with `--liburing=y` and the correct output directory. See P3-1.

## 1. Scope

`git diff 859e4b56..HEAD --stat` shows exactly:

- Production: `include/sluice/async/detail/uring_backend.hpp` (+11), `src/async/uring_test_seams.hpp` (+19), `src/async/uring_transport.cpp` (+41/-3), `tests/uring_pending_control_parity_test.cpp` (new, 519), `xmake/tests.lua` (+23).
- Documentation (expected/allowed): `docs/roadmap/v2-conformance.md` (ledger G6 row + dated mechanism-justification/authority-contraction section, required by AGENTS.md engineering order step 6) and `docs/evidence/e1-pending-control-productization-20261023/**`.

Nothing else in `src/`, `include/`, `apps/`, or root `xmake.lua` changed. The exported `e1-production.diff` is byte-identical to the actual production diff.

`progress_source.hpp` contains a `control_pending_nolock_()` — verified to be an unrelated, pre-existing ProgressSource epoch mechanism (unchanged by this diff), not `RouterEntry::control_pending`.

## 2. Write-point closure rebuilt from source (not from the author's document)

All writes to `RouterEntry::control_pending` and every `pending_control_count_` access, from grep + full read of `src/async/uring_transport.cpp` at HEAD:

| Site (uring_transport.cpp) | Trigger | Flag edge | Counter | Lock domain (verified by reading every caller) |
|---|---|---|---|---|
| `:573-578` `retire_router_entry_` | route retired holding sticky intent | true→false | −1 | callers `uring_completion.cpp:95,137,140` (under `handle_one_cqe`'s `dispatch_mtx_`), `uring_transport.cpp:440,517` (inside `poison_and_recover_locked_`, whose callers `account_transport_result_locked_`/overflow-flush path are under `dispatch_mtx_`) |
| `:628-633` `issue_running_cancel_locked_` early return | cancel while control_state != none (coalesced) | true→false | −1 | `cancel_key` (`:587` lock) and `service_pending_controls_locked_` (locked callers below) |
| `:653-665` SQE-exhausted sticky arm | no SQE after in-function retry submit, sticky retained | false→true, only if not already pending | +1 exactly once per edge | same |
| `:666-669` non-sticky clear arm | same exhaustion under `SLUICE_BACKEND_CONTRACT_MUTANT_STICKY_INTENT_DROPPED` only | true→false | −1 | same |
| `:681-687` prepared arm | SQE obtained, control_state→prepared | true→false | −1 | same |

Reads of the count: `:701` (the zero gate) and `src/async/uring_test_seams.hpp:105` (test snapshot). Reads of the flag: `:707` (authoritative scan predicate) and the test snapshot `:114`. There are no other references in `src/`, `include/`, or `apps/` (grep-verified).

Callers of `service_pending_controls_locked_`: `uring_transport.cpp:88` (`dispatch_after_accept`, lock at `:49`) and `uring_backend.cpp:622` (`poll`, lock taken in the enclosing block). Both under `dispatch_mtx_`. No `_locked_` callee on these paths re-acquires the mutex (no recursive-lock hazard); `router_` is never resized inside the service loop (`issue_running_cancel_locked_` prepares SQEs but never retires), so the `router_[i]` reference in the scan is stable.

Structural invariants I verified independently:

- INV-1 parity: each flag edge is paired with exactly one counter edge in the same critical section; slot reuse always passes through `retire_router_entry_` (the only `cookie_free_list_.push_back` is `:580`), and `dispatch_one_locked:191` re-initializes with `RouterEntry{}` whose `control_pending` is `false` (`uring_backend.hpp:186`), so a reused slot contributes 0 to both sides.
- INV-2 exclusivity (`control_state != none ⇒ !control_pending`): pending is set true only at `:653-665`, textually dominated by the `control_state != none` early return at `:628`; `control_state` leaves `none` only at `:681` (prepared), where the flag is cleared in the same block. The `prepared→submitted` (`:294`) and `submitted→none` (`uring_completion.cpp:117`, poison `:430/:492`) transitions never write the flag. Hence poison recovery resetting `control_state` cannot strand a pending flag, and the `:628-633` clear is defensive-unreachable in production (fails toward parity if INV-2 were ever violated) — matching the author's INV-2 proof. No discrepancy found between my matrix and `transition-closure.md` (the author's W1/W2/W3/W4/W5 numbering maps to my sites in different order; content identical, line numbers accurate).

## 3. Non-authority

Grep of all readers of `pending_control_count_`: production reader is exactly the zero-count gate at `uring_transport.cpp:701`. `internal_work_retired()` (`uring_backend.cpp:361`) reads dispatch queue, publication queue, event-owed scan, `live_cookies_`, `live_control_sqes_`, ledger — not the counter. `retire_execution_resources()` and both destructors (`uring_backend.cpp:48,338`) do not read it. Settlement (`uring_completion.cpp`) never reads it. RequestCore (`request_core.cpp`) has no reference. Non-authority holds.

## 4. Sticky-intent preservation (CANCEL-01)

- The gate at `:701` returns early only when `pending_control_count_ == 0`; when the count is nonzero the loop at `:704-710` is the unchanged full authoritative router scan calling `issue_running_cancel_locked_` for every `in_use && control_state==none && control_pending` route.
- SQE-exhausted cancel still records intent (`:653-665`): flag set once, count +1 once; a repeated cancel re-enters with the flag already true and does not re-increment; when an SQE is finally obtained the intent is consumed by preparing the real `IORING_OP_CANCEL` (`:679-687`), decrementing once.
- `cancel_key` still `signal_ready_progress()` for `requested` dispositions, so the external loop returns to `poll()` → `service_pending_controls_locked_()`; W-03 external-loop suite passes 9/9 on a real ring.
- The sticky-lifecycle protocol oracle confirms end-to-end: intent survives 3 pressure passes (derived stays 1, no control-state advance), issues exactly one control SQE after pressure release (`live_control_sqes_for_test()==1`), and settles through the real control path with terminal responsibility.

## 5. Snapshot seam concurrency

`pending_control_snapshot_for_test()` (`src/async/uring_test_seams.hpp:101-118`) takes one `std::lock_guard<std::mutex> lk(dispatch_mtx_)` and, inside that single critical section, reads `pending_control_count_` AND performs the authoritative `router_` scan (also capturing `live_routes`, `prepared_controls`, `submitted_controls`). One critical section, not two getters — the test-side race the author flagged is absent. The declaration is inside `#if defined(SLUICE_HAS_LIBURING) && defined(SLUICE_ASYNC_INTERNAL_TESTING)` (`uring_backend.hpp:94+`) — test-only, no public API change. (Pre-existing `live_cookie_for_offset_for_test` is lock-free, but the new test calls it only at quiescent points — never concurrently with cancelers; the concurrent monitor uses the locked snapshot only.)

## 6. Mutation gate — receipts (my own runs, release, liburing=y)

| Mutant | Exit | Named FAIL line (stderr) |
|---|---|---|
| `uring_pending_control_mut_missing_increment` | 1 | `FAIL parity oracle at sticky_lifecycle: derived pending_control_count_=0 but authoritative router scan=1` |
| `uring_pending_control_mut_missing_decrement` | 1 | `FAIL parity oracle at sticky_lifecycle: derived pending_control_count_=1 but authoritative router scan=0` |
| `uring_pending_control_mut_gate_always_skips` | 1 | `FAIL protocol oracle: released pressure did not issue the pending control SQE (lost intent)` |
| `uring_pending_control_mut_double_increment` | 1 | `FAIL parity oracle at sticky_lifecycle: derived pending_control_count_=2 but authoritative router scan=1` |
| `uring_pending_control_mut_retire_stale_count` | 1 | `FAIL parity oracle at repeated_cancel: derived pending_control_count_=1 but authoritative router scan=0` |

5/5 killed by named parity/protocol oracles with exit 1 — no sanitizer aborts, no crashes, no hangs, no destructor terminates (the test's `fail()` uses `std::_Exit(1)` precisely so a named verdict cannot be preempted). My receipts are byte-identical to `mutant-receipts.txt` (which also records distinct binary SHA-256s per mutant; production code unchanged between the receipts commit and HEAD).

Counterfactual oracle independence (step 8):

- The parity oracle recomputes the summary from the router flags inside the test seam — it shares no counting logic with production. Any implementation maintaining such a derived summary with a missing/duplicated edge fails the recomputation; this holds for any correct-by-construction alternative, not just this code shape.
- `double_increment` is killed through the service loop re-entering the sticky arm on an already-pending route (the actual hazard class "increment not idempotent"), not by mirroring call order.
- `gate_always_skips` keeps parity (count consistent) and is killed purely by the externally observable liveness property "the remembered cancel is issued once SQEs are available", observed through the pre-existing `live_control_sqes_` atomic — independent of the gate mechanism; an implementation servicing intents by any other path would still pass.
- `retire_stale_count` is killed by the scan counting only `in_use` entries after retirement — identity/slot-reuse independent.

## 7. Test quality notes

7 scenarios, all deterministic: SQE pressure via a switchable submit hook (hold/release), CQEs injected via `inject_cqe_for_test`, `std::latch`-gated 8-thread cancel storm with an in-lock snapshot parity monitor, no sleeps (grep confirms; the only "sleep" match is the comment saying none is used), `alarm(180)` bound. Adjacent pre-existing cancel-path suites still pass (`uring_progress_race_test` 23/23, `uring_owed_counter_parity_test` 8/8), so the counter did not perturb existing behavior.

## Findings

### P1 — none.

### P2 — none.

### P3 (notes / nice-to-have)

1. **P3-1 tsan reproduction trap (pre-existing repo behavior, evidence-ergonomics only).** `xmake config -m tsan -y` silently drops `--liburing=y` (option default false), hiding all uring targets ("not a valid target name"), and tsan binaries are emitted to `build/linux/x86_64/tsan/`, not `.../release/`. Anyone following the natural command sequence runs a stale release binary and reports a false tsan pass (I hit this myself). The author's `battery.txt` records `mode=tsan liburing=y: pass=5` without the exact path/flags. Recommendation: record the exact tsan command line (with `--liburing=y` and the tsan output path) in the evidence README. Not introduced by this change; my own correctly-instrumented tsan runs (5×, 0 reports) substantiate the claim.
2. **P3-2 forward reference to review artifacts.** `docs/evidence/e1-pending-control-productization-20261023/README.md` R8 row cites `review-A-concurrency.md` / `review-B-performance.md`, which do not yet exist in the tree. The ledger gates the row on "Owner merge gate", so this is expected pre-merge state, but the files must be added (or the row corrected) before merge so the index does not cite nonexistent artifacts.
3. **P3-3 committed binaries under docs/evidence** (`product-ab/results/{extloop,pipeline}_{base,e1}`, `seam-bound/results/seam_{base,e1}`, `native-p5-probe/probe`; ~840 KB total). AGENTS.md says to commit evidence with provenance (present) rather than collectors; prebuilt A/B binaries are defensible provenance but blur "commit evidence, not tools". Note only; consistent with the sibling H2b evidence directory pattern.
4. **P3-4 `xmake test` integration not exercised in this xmake version.** `xmake test uring_pending_control_parity_test` reports "nothing to test" in xmake 3.1.2 despite `add_tests` registration in `xmake/tests.lua` — identical behavior for the pre-existing sibling targets (`uring_owed_counter_parity_test` et al.), so it is a pre-existing tooling quirk, not a regression; direct binary execution is the mandated verification path and works.

## Explicit answers

- Write-point closure: **COMPLETE** (5 sites, every flag edge paired with one counter edge in the same `dispatch_mtx_` critical section; slot reuse settles through retirement).
- Lock-domain violations: **NO** (all writers and both readers of the count are under `dispatch_mtx_`; verified per caller chain; no recursive re-acquisition).
- Sticky-intent preserved: **YES** (gate opens at count>0 with the unchanged full authoritative scan; sticky set/consume edges intact; W-03 9/9; gate mutant killed by the liveness oracle).
- Non-authority violated: **NO** (sole production reader is the `:701` gate; settlement, `internal_work_retired`, `retire_execution_resources`, destructor and RequestCore never read it).
- Mutants 5/5 killed by named oracles: **YES** (receipts in section 6; exit 1 each; matches author's recorded receipts).
- Scope clean: **YES** (exactly the 5 production files; docs/evidence + ledger row are the expected documentation delta; nothing in `apps/` or root `xmake.lua`).
- Snapshot seam single-critical-section: **YES** (one `dispatch_mtx_` guard covers derived count and authoritative scan plus the auxiliary fields).

## Verdict rationale

The mechanism is the smallest one satisfying the contract (one `size_t` + five guarded edge updates + one gate read), preserves CANCEL-01 sticky semantics exactly, keeps the router flags authoritative, is fully covered by an independent parity oracle and a liveness oracle that kill all five registered defect classes deterministically, and passes release, tsan and adjacent regression suites in my independent runs. No P1/P2 findings.

VERDICT: APPROVE
