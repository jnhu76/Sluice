# Phase G Compliance Gate — backend-ready progress wake integration

**Phase:** G (final async-foundation phase; roadmap "backend-ready wake
integration")
**Design:** `docs/design/phase-g-backend-progress-wake.md`
**Status:** **BLOCKED (G1)** — the implementation and instrumentation are in,
but the park-window forensic finding (`docs/design/phase-g-backend-progress-wake.md`
§8) proves the unbounded wake-domain park violates the park-window invariants
(deterministic reproducer in `phase_g_backend_progress_wake_test`). Phase G is
NOT complete; G2–G7 are deferred pending an approved fix design. Gate 4 rows
below record only what was actually executed on 2026-08-14.
**Authority:** ADR-execution-model §9.4.7/§9.4.7.1 (MIXED-WAKE; P5 seam
reserved); AGENTS.md §4.4/§10/§13.1/§13.2/§14; Constitution AC-6 (polling
justification), AC-7, AC-10, AC-13, AC-14, AC-15.

---

## Gate 0 — Architecture Classification

| Field | Decision |
|---|---|
| Affected capability | MW-S2 MIXED-WAKE progress park; Scheduler wake-domain bridge; backend wait-source bounded park; wake-domain park timeout policy |
| Affected layer | L1 AsyncIoContext (bounded wait_one + split-wait capability accessor); E7-E13 Scheduler (MW-S2 park domain, signal_wake_locked bridge, deadline hint); backend wait sources (bounded wait_for_change — ReadyWaitSource / UringWaitSource) |
| Classification | Corrective (closes the E9-reserved P5 seam; completes the roadmap's final wake-integration phase). **No new divergence:** DIV-04/DIV-05 reclassified per roadmap authorization ("the only phase that may reclassify DIV-04/DIV-05") |
| Governing ADR | ADR-execution-model §9.4.7.1 (bounded observation interval is protocol authority; P5 "reserved if P3 proves insufficient") — Phase G makes P3's MIXED-WAKE observation interval unnecessary for split-wait production backends |
| Conformance map change | No backend row reclassification (all four backends unchanged in contract); the reference backends' poll-driven classification is documented (G4) |
| Constitution rules | AC-6 (2ms re-justified: removed for production; retained only as the reference-backend observation interval), AC-7, AC-10, AC-13, AC-14, AC-15 |

### Zig conformance / divergence classification

- DIV-04 (decoupled wake domains — backend does not directly wake the
  Scheduler): **AMENDED** (Approved -> Approved with the Phase G bridge). The
  decoupling is preserved — the backend still never touches the Scheduler; the
  bridge runs through the backend-neutral `BackendWaitSource` interrupt seam
  that Phase D4 already defined. The "up to 2ms observation latency" cost term
  is removed for split-wait production backends.
- DIV-05 (2ms bounded observation interval as protocol authority):
  **AMENDED** (Approved -> Approved, scope narrowed). The interval remains
  protocol authority ONLY for poll-driven reference backends (Fake/Sync) whose
  readiness cannot self-notify; it is no longer a production-path authority.

---

## Gate 1 — Ownership and State Machine

### 1.1 MW-S2 park-domain decision (changed rule)

```text
MW-S2 participant COMMITTED (E7 two-phase admission, unchanged):
  split-wait backend (wait_source() != nullptr)
    -> park domain = BACKEND (ctx_.wait_one()), backend-only AND MIXED-WAKE
       (external wakes reach the park via the interrupt bridge)
  non-split-wait backend (reference/legacy)
    -> park domain = SCHEDULER when external_wake_possible_locked()
                     (MIXED-WAKE; bounded observation park — E9 behavior)
       park domain = BACKEND otherwise (unchanged)
```

| Transition | Authority | Lock domain | Alloc | Failure | Wake | Shutdown |
|---|---|---|---|---|---|---|
| MW-S2 commit -> arm_committed_wait | elected participant | G -> B (wait source arm) | none | n/a (arm is a registration) | control wakes after commit are observed by the next wait_one (D4-RM14) | request_stop -> interrupt observed (unchanged, now also MIXED) |
| park in wait_one (split-wait) | elected participant | B (wait source park) | none | wait_source failure -> fail-fast (UringWaitSource poll error) | backend progress epoch / ring fd; interrupt bridge (control epoch) | control interrupt (one-shot) |
| interrupted, no reap, external waits remain | participant (Phase D) | G (reclassify) | none | n/a | re-park (stay resident) — NEW rule | stop converges at MW-S3 via stop predicate |
| interrupted, no reap, backend-only | participant (Phase D) | G | none | n/a | terminate run (unchanged; driver re-entry) | unchanged |
| Scheduler wake publication -> bridge | signal_wake_locked (any producer) | W then B (leaf) | none | n/a | parked wait_one returns interrupted | control/stop semantics unchanged |

### 1.2 Lock-order table (AGENTS.md §13.1; new edges bold)

```text
G -> A        classify_locked / drain poll                         [unchanged]
G -> B        arm_backend_wait_commit (MW-S2 commit)               [unchanged; now also MIXED]
G -> W        signal_wake_locked                                   [unchanged]
G -> W -> B   signal_wake_locked -> interrupt_backend_waiters      [NEW]
G -> I        route_runnable_locked                                [unchanged]
A -> B        wait_one snapshot / poll / wait_for_change           [unchanged]
A -> L        backend reap / register / cancel                     [unchanged]
L -> (release) -> sink.on_ready                                    [arena contract]
B is a leaf: never acquires G, A, W, or L while held.
```

Cycle proof: B (backend wait source mutex) is a leaf with inbound edges only
from A and G (via arm and the new W edge); no wait-source path ever acquires a
Scheduler lock. W -> B is one-directional (signal_wake_locked acquires B only
after releasing nothing that B needs; B never acquires W). The eventfd write
under B (UringWaitSource) and the CV notify (ReadyWaitSource) are non-blocking
and call no Scheduler/user code.

### 1.3 The bridge is a notification, not a completion

`interrupt_backend_waiters` / `interrupt_all` semantics (unchanged from
Phase D4): bumps the control epoch under the wait-source mutex, then wakes
parked pollers. It never publishes a Completion, never touches a RequestSlot,
never records a terminal, never cancels I/O, never releases a borrow or slot,
and never routes a Fiber (G-I2). Reap remains the sole Completion-ready
publication boundary; the Scheduler drain remains the sole Fiber-routing
authority (G-I1).

---

## Gate 2 — Resource and Failure Model

| Resource | Capacity / allocation | Full behavior | Reclamation |
|---|---|---|---|
| BackendWaitSource bounded park | no new storage (epochs already exist) | wait_for_change(observed, max_park): ReadyWaitSource `cv.wait_for`; UringWaitSource poll timeout (clamped to INT_MAX ms) | none (no new container) |
| `backend_wait_active_` gate | one atomic bool | set at MW-S2 commit, cleared after wait_one returns; skip-bridge optimization only (epoch protocol is the authority) | n/a |
| interrupt path | **zero allocation** (mutex + epoch + notify/eventfd write) | never fails; never blocks | n/a |
| accepted terminal path | unchanged (allocation-free per Phase D/E proof) | the bridge adds no post-accept allocation dependency (G-I7) | n/a |

OOM: no new allocation site; the accepted -> terminal -> reap -> route path
remains allocation-independent (Phase E slice 12 / Phase D2 no-alloc evidence
re-run in Gate 4).

---

## Gate 3 — Wake / Progress / Shutdown Model

### 3.1 Progress notification protocol

```text
split-wait backend-ready publication (worker record_terminal / CQE reap)
  -> backend wait source progress epoch / ring fd        (prompt, per-backend)
  -> wait_one() returns progress                         (no polling)
  -> participant reaps -> ReadyRoutingSink -> drain -> Fiber

Scheduler wake publication (routing / flag / select / waitqueue / deadline /
wake handle / termination)
  -> signal_wake_locked (wake epoch + cv notify)
  -> interrupt_backend_waiters (control epoch + wake)    (THE BRIDGE)
  -> parked wait_one returns interrupted -> final poll -> re-drain -> re-park
```

- **Lost-wake closure:** the snapshot -> poll -> wait_for_change epoch
  protocol (existing); the D4-RM14 arm_committed_wait commit-to-park handshake
  (existing, now also MIXED); the D4-RM13 invocation-level control baseline
  (existing); the level-triggered ring/control fd (Uring, existing).
- **Coalescing:** multiple backend-ready -> one or more epoch advances; the
  drain reaps all; 1:1 wake is never required (G-I3).
- **Progress vs control distinction:** progress epoch (backend readiness) and
  control epoch (interrupt) remain separate; a control wake never fabricates
  readiness and a progress wake never looks like shutdown (G-I5).

### 3.2 Shutdown convergence

```text
backend progress || runtime stop || close_admission || participant park
```

- request_stop -> interrupt_backend_waiters (direct, unchanged) + bridge
  wakes; participant re-drains, reaps remaining readiness, reaches MW-S3, and
  the stop predicate terminates the run; driver re-enters on the control
  epoch (unchanged).
- The D4-RM14 armed baseline now also protects the MIXED commit-to-park
  window (previously the MIXED park had no backend arm; the stop-vs-commit
  race is closed for both domains).
- No busy-spin: the interrupted wait_one is one-shot per invocation; a future
  invocation snapshots the advanced control epoch and parks normally.

---

## Gate 4 — Evidence (executed commands)

Fill-in section — every row below is recorded from actually executed runs on
the implementation head.

| Evidence | Command | Result (executed 2026-08-14) |
|---|---|---|
| Production libraries | `xmake build sluice_core sluice_async` (Clang Debug) | build ok (guarded forensics excluded from production) |
| Internal-testing library | `xmake build sluice_async_internal_testing` | build ok |
| Clang Debug full suite | clean rebuild, 163 debug binaries, parallel (`xargs -P 16`, per-binary `timeout 300`) | 160 PASS; `sluice_copy_integration_test` FAIL (pre-existing env, fails on HEAD baseline too); `application_runtime_drain_starvation_test` SIGABRT; `sluice_copy_pipeline_integration_test` TIMEOUT — the latter two are the G1 defect (design §8) |
| Phase G regression | `SLUICE_TEST_FILTER=phase_g_quiescent_not_last_idle_signals_domain` ×30 | 30/30 PASS |
| Phase G forensics canary | `SLUICE_TEST_FILTER=phase_g_park_window_forensics_drain_stall` ×10 | EXPECTED RED while G1 BLOCKED: ~half rc 70 (full park-ledger dump of the stranded-runnable + consumed-epoch stall), ~half rc 134 (`request_arena_invalid_terminal_fail_fast` at teardown — outstanding request, design §8 second mode) |
| Adjacent sanity | `application_runtime_test`, `async_io_context_split_wait_c2e_test`, `async_stats_wait_race_test` | PASS |
| Clang Release full suite | `xmake f -m release --toolchain=clang -y && xmake build -g test && xmake test -v` | NOT EXECUTED (deferred with G2–G7) |
| ASan/UBSan | `xmake f -m asanubsan --toolchain=clang -y && xmake build -g test && xmake run -g test` | NOT EXECUTED (deferred) |
| TSan | `xmake f -m tsan --toolchain=clang -y && xmake build -g test && xmake run -g test` | NOT EXECUTED (deferred) |
| Real-liburing | `xmake f -m debug --toolchain=clang --with-liburing=true -y && xmake build -g test && xmake test -v` | NOT EXECUTED (deferred) |
| Backend conformance | `python3 scripts/verify-backend-conformance.py` | NOT EXECUTED (deferred) |
| Formal models | `python3 scripts/formal/verify.py check` | NOT EXECUTED (deferred; e9_park_wake/e9_wake_handle_lifetime transport unchanged) |
| Docs / architecture validation | `python3 scripts/check-doc-links.py`, `python3 scripts/verify-architecture-docs.py`, `git diff --check` | `git diff --check` clean; doc scripts NOT EXECUTED for this draft |
| Mutation evidence | per-mutation RED runs | NOT EXECUTED (deferred with G2–G7) |

---

## Known limits / residual risk

- **G1 BLOCKED (primary risk)**: the unbounded wake-domain park has an
  un-closed commit-to-sleep window — see
  `docs/design/phase-g-backend-progress-wake.md` §8 for the four violated
  park-window invariants, the evidence-backed timeline, and the structural
  contributors (worker-0-only MW-S2 election; backend completion not crossing
  into the wake domain; terminate-path inbox stranding; no predicate backstop
  for route-to-owner runnables). No behavioral fix is included in this change
  (forensics only); a timeout/cap re-arm is explicitly not an acceptable
  repair.
- Reference backends (Fake/Sync) keep the bounded Scheduler-domain
  observation interval in MIXED-WAKE (their readiness is poll-driven and
  cannot self-notify). This is a documented reference-backend classification
  (G4), not a production path.
- The bridge adds one atomic load to every Scheduler wake publication when no
  backend participant is parked, and a control-epoch bump + notify when one
  is parked (the prompt-wake cost that replaces the 2ms observation interval).
