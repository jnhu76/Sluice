# #595 R5 Phase A — Code-Finding Verification C1–C4

Role: code-finding verifier (C1–C4). Date: 2026-11-06 (UTC). Baseline:
`/home/jnhu/sluice-595-e5/r5/worktree` at `f3b25329bc8a64a201961c426a58adacfb8fdf08`
(read-only; `git rev-parse HEAD` + empty porcelain checked this session).

Method: source reading of the R4_ORIGINAL tree plus archived R4 diagnostic
artifacts (read-only under `/home/jnhu/sluice-595-e5/r4/`). No build, no
counting probe, no disassembly was needed: the static loop structure plus the
archived per-decision diag lines and per-arm smoke counters already pin every
claim at the source level. Where a number is a *source-level inspection count*
(not a measured instruction/time count) it is labeled as such. Nothing in this
session ran a timed measurement; the MEASUREMENT.lock was therefore not taken.

Paths cited below without a directory prefix are relative to the r5 worktree
root. `r4/…` means the read-only R4 archive at
`/home/jnhu/sluice-595-e5/r4/…`.

---

## C1 — `batch_shaped_prefix(rest)` rescans the remaining span on every re-entry

**Verdict: CONFIRMED.**
Fix class: **mechanical cleanup candidate** (bounded scan + naming split),
equivalence PROVEN (argument below; proposed for R4_CLEANUP).

### Structure (source)

- `bench/r3_ready_batch.hpp:189-196` — `batch_shaped_prefix` walks from the
  front of the span while `ops[count].len == 4096 && ops[count].dst != nullptr`.
  It stops at the first ineligible member or at the end, so with an
  all-eligible span it inspects the **entire remaining span**.
- `bench/r3_ready_batch.hpp:442-448` — the adapter's `while (begin <
  ready.size())` loop computes `record.eligible_prefix = batch_shaped_prefix(rest)`
  at line 448 **before** the `if (policy.budgeted)` branch at line 455. The
  scan therefore runs unconditionally on every loop iteration for **every**
  batching arm — including R (non-budgeted, `arm_policy` line 163) and SHADOW
  (budgeted, line 165).
- Re-entry: budgeted non-shadow arms continue via `chunk_continue`
  (lines 564-567, stop `input_exhausted`), R/SHADOW/Q2/Q8 via `cap_reuse`
  (lines 561-563, stop `cap`); each re-entry re-enters line 448 with
  `rest = ready.subspan(begin)` (line 443).

### Claimed inspection counts — checked statically

All-eligible span of N members, quantum q, every presentation fully accepted
(`untouched_begin == present_len`, stop `input_exhausted`/`cap`): iteration i
scans the whole remaining span, so total member inspections are
N + (N−q) + … This is a **source-level loop/inspection count**, not a dynamic
instruction or time measurement.

| Case | Claimed | Recomputed from the loop structure | Match |
|---|---|---|---|
| N=16, q8 | 16+8 = 24 | 24 (2 presentations: 8+8) | yes |
| N=16, q2 | 16+14+…+2 = 72 | 72 (8 presentations of 2) | yes |
| N=64, q8 | 288 | 64+56+48+40+32+24+16+8 = 288 | yes |
| N=64, q2 | 1056 | 2·(32+31+…+1) = 1056 | yes |

Preconditions hold in W03: every op has `len = block_size (4KiB)` and
`dst = buffers[slot].data()` (`bench/bench_w03_external_loop.cpp:407-412`),
and the backend's `batch_eligible_read` is the same predicate
(`src/async/uring_backend.cpp:437-439`).

### Dynamic corroboration (archived R4 artifacts, read-only; no new run)

- `r4/logs/r4_diag_lines_sample.log` (S arm, W03-uring-4k-d16-r3d16cap,
  measured): per-16-op-group decision sequence
  `ready/eligible = 16,14,12,10,8,6,4,2` with `cause=chunk` — direct evidence
  that each re-entry rescans the entire remaining span (eligible == ready at
  every step) and that the total is 16+14+…+2 = 72 per group.
- `r4/logs/w03_smoke_Q2.log`: 64 ops → `presentations=32 chunk_reentries=28`
  (8 decisions per 16-op group). `r4/logs/w03_smoke_Q8.log`:
  `presentations=9 chunk_reentries=4` (2 batch decisions per group + 1
  `remaining_single` immediate). Arithmetic matches the table above.
- R also executes the scan unconditionally: `r4/logs/w03_smoke_R.log` shows
  `presentations=11 cap_reentries=4 untouched=18` — full-span presentations
  accepted 8 (cap) then re-entered, so R scans 16 then 8 per full group
  (24 inspections), while its budget counters (`q1/q2/q8=0`) confirm the
  budget block never ran: only line 448 consumed the scan.
- SHADOW too: `r4/raw/diagnostics/logs/b00-p0-SHADOW-S-a1.log` (d16, 20000
  ops = 1250 groups): `presentations=2501 cap_reentries=1221
  shadow_decisions=2501` — one full-rest scan (plus one `outstanding()` read
  and one full table evaluation) per presentation, ≈2 per group.
  `r4/logs/w03_smoke_SHADOW.log` (64 ops): `presentations=11 cap_reentries=5
  shadow_decisions=11`.

### Consumers of `eligible_prefix` (closed set; grep over the whole tree)

`batch_shaped_prefix` and `eligible_prefix` appear only in
`bench/r3_ready_batch.hpp`:

1. line 227 — `front_ineligible` row tests `in.eligible_prefix == 0`;
2. line 497 — non-shadow batch: `present_len = min(decision.quantum,
   input.eligible_prefix)`;
3. lines 346, 544 — the DecisionRecord trace field (diagnostics only; the
   record is consumed only when `trace_ != nullptr`, line 537).

`decision.quantum` is bounded by 8 on every reachable path: base sets 8
(line 265), `clamp2` takes `min(quantum, 2)` (line 269), pinned quanta are 2
or 8 (lines 172-175, applied at line 278), immediate sets 1 (line 284),
authority sets 0 (line 286). `ready_batch_cap = 8`
(`include/sluice/async/detail/uring_backend.hpp:92`).

### Equivalence argument (bounded scan), PROVEN

Define `bounded_prefix(ops, B)` with `B = 8`: scan at most B leading members;
the returned value equals `min(ep_full, B)` and is exact whenever the scan
stops before B.

Proposed change (mechanical, R4_CLEANUP): in the adapter loop, compute a
**bounded** eligible prefix `B = 8` for the policy input; keep the **full**
scan only when `trace_ != nullptr` (diagnostics); for non-budgeted arms (R)
skip the scan entirely when there is no trace sink. Rename so the two concepts
are distinct (e.g. `policy_eligible_prefix_bounded` vs the record's full
`eligible_prefix`); no silent redefinition of the existing trace field.

Claim: for every arm, with the trace sink absent (all timed runs), the change
is observationally identical to the original.

Proof, case by case, from the closed consumer set:

- (a) `front_ineligible` (use 1): `min(ep_full, B) == 0 ⟺ ep_full == 0`
  because `B ≥ 1`. `evaluate()` reads `eligible_prefix` nowhere else, so the
  Decision (action, quantum, row) is identical.
- (b) `present_len` (use 2): for any `q ≤ B`,
  `min(q, ep_full) = min(q, min(ep_full, B))`: if `ep_full ≤ B` the two
  arguments are equal; if `ep_full > B` then `min(q, ep_full) = q` and
  `min(q, B) = q` since `q ≤ B`. Hence the presented span
  `rest.first(present_len)` is byte-identical, and everything downstream —
  `context_submit_ready_reads` call, accepted identities/order, error member,
  `untouched`/`untouched_begin`, `stop`, `flush_attempts`,
  `residue_advertised` notification, slot reservation/release, control
  boundaries — is unchanged because the backend input is unchanged
  (`bench/r3_ready_batch.hpp:527-529`, `include/sluice/async/detail/test_access.hpp:49-105`).
- (c) SHADOW: `present_len` is never derived from `eligible_prefix` (the
  shadow branch at lines 482-485 does not touch it), so only (a) applies; the
  counterfactual row/quantum logged by SHADOW is identical.
- (d) Non-budgeted arms (R): `eligible_prefix` feeds no control flow
  (lines 521-525); the scan is dead work unless tracing. Skipping it changes
  nothing observable.
- (e) Trace path: when `trace_ != nullptr` the original full scan is kept, so
  diag lines (e.g. `eligible=16`) are byte-identical to history; the bounded
  value never reaches the record.

Constraints from the round's ask, each satisfied:

- *Does not validate error members earlier than the original*: the bounded
  scan touches `ops[0..min(ep_full,B)-1].len/.dst`, a **subset** of the
  members the original reads (`ops[0..ep_full-1]`); it reads `op.file` never.
- *No eligibility caching across lifetimes*: the bounded prefix is recomputed
  per iteration from the live span; nothing is memoized across iterations or
  across `NativeFileRef`/borrow lifetimes (which is why bounded-scan is
  preferred over a "resume from last eligibility" memo).
- *Naming split, no silent counter redefinition*: the trace field keeps its
  full-prefix meaning; the bounded value gets its own name.
- *No simultaneous policy changes*: quantum, suffix singles, refill, and
  completion order are untouched (the diff touches only the scan at line 448
  and its uses at 458/497 inputs).

One caveat to record in R4_CLEANUP: `B = 8` must stay pinned to
`max(ready_batch_cap, max pinned quantum)`; if a future arm pins a quantum
above 8, (b) breaks. Bind `B` to a named constant next to the budget table.

Since equivalence holds, no candidate patch/counterexample file is written
(`c1-candidate.patch` not needed).

---

## C2 — pressure predicate `completed>0 || immediate_work_remains` does not mean "completion pressure now"

**Verdict: CONFIRMED.**
Fix class this round: **naming / comments / docs / wrong test assertions
only** (keep S_ORIGINAL behavior byte-identical). Changing the predicate is a
policy experiment, out of scope here.

### `completed>0` is past processing amount, not pending work

- `pass.completed = poll()` (`src/async/uring_backend.cpp:314`); `poll()`
  returns the number of results **published during that pass** (publications
  at lines 861-880 plus owed events delivered at 886-908, `++published` at
  879/907). It counts work the owner pass *did*, i.e. a historical quantity at
  any later decision point.
- The harness stores the **final** pass of the drain loop:
  `bench/bench_w03_external_loop.cpp:231-245` loops `poll_progress` until
  `!immediate_work_remains`, keeping `last_pass_completed` /
  `last_pass_immediate_work` of the last iteration (lines 239-240), then
  publishes them under `shared.mutex` (lines 254-257).
- The predicate: `bench/r3_ready_batch.hpp:474-476`
  `record.pressure = facts.pressure_known && (facts.last_pass_completed > 0 ||
  facts.last_pass_immediate_work)`.

### The counterexample state is the common steady state

Claim checked: `completed>0 ∧ immediate_work_remains=false ∧ no unpublished
completions` — reachable and dominant. A final drain pass reaps the last
CQEs, publishes them inside the pass (pass boundary captured before
publishing, `src/async/uring_backend.cpp:863-869`), delivers owed events;
afterwards `publication_pending_` empty, `owed_count_ == 0`, CQ not ready and
no overflow → `immediate_work_remains = false`
(`src/async/uring_backend.cpp:320-321,333`;
`has_immediate_physical_work` at 293-301), while `completed>0`.

Archived proof: every post-startup S group in
`r4/logs/r4_diag_lines_sample.log` shows
`row=completion_pressure pressure=1 pass_completed=16 pass_immediate=0
active=0` at its first decision — at that instant the submitter had already
collected all 16 results (window drained before refill), nothing was
published-pending, nothing kernel-in-flight, yet pressure=1. R4's aggregate:
completion_pressure in 29,973/29,998 = 99.92% of S decisions
(`r4/RESULTS.md:199`, verified in `r5/analysis/env-context.md` §7 item 9).

### `immediate_work_remains` scope — completion-side only

`src/async/uring_backend.cpp:320-321,333`:
`immediate_work_remains = !publication_pending_->empty() || owed visible ||
CQ-ready-or-overflow`. It does **not** include dispatch backlog or pending
controls — `dispatch_retry_remains` is a separate `ProgressPass` field
(`src/async/uring_backend.cpp:325`, `include/sluice/async/async_io_context.hpp:75`),
and `pending_control_count_` is not part of the formula (controls are serviced
inside a pass at `src/async/uring_backend.cpp:855`). So the second disjunct is
genuinely completion-family; the mislabeled part of the signal is
`completed>0` (history), not `immediate_work_remains`.

### `pressure_known` means "ever sampled", not "fresh"

`shared.pressure_seen` (`bench/bench_w03_external_loop.cpp:158`) is set once
when any pass completes (line 257) and never cleared; the submitter copies it
as `facts.pressure_known` (line 421). It is a monotonic liveness witness of
the channel, not a freshness flag; the values can lag by an arbitrary number
of driver cycles.

### Same stale snapshot reused across all decisions of a group

`DecisionFacts` is copied **once per refill group** under `shared.mutex`
(`bench/bench_w03_external_loop.cpp:415-422`) and passed by const reference
into `Adapter::submit` (`bench/r3_ready_batch.hpp:407-408`); every while-loop
iteration (all chunk/cap re-entries) re-evaluates from that same copy
(`bench/r3_ready_batch.hpp:449-476`). Visible in the archive: within one S
group, `pass_completed=16` is frozen across 8 chunk decisions while `active`
grows 0→14 (`r4/logs/r4_diag_lines_sample.log`). The tail suffix singles
(`bench/r3_ready_batch.hpp:578-590`) make no decision at all, so "reuse" is a
property of the in-group re-entry chain.

### Near-constant truth is caused by the caller's completion-driven mode

The submitter parks until `shared.completions > watermark`
(`bench/bench_w03_external_loop.cpp:522-539`) and only then refills; the facts
it reads are, by construction, from the pass that published exactly those
completions (`completed>0` always). The 99.92% truth rate is therefore a
structural artifact of the completion-driven harness, not evidence of pending
completion pressure. Confirmed as mechanism, with the archived counts as the
quantitative anchor.

Allowed corrections this round (classification): rename the field/row (e.g.
`owner_pass_completed_work` / `owner_pass_had_activity` instead of
`pressure`), fix the comments at `bench/r3_ready_batch.hpp:135-145` and the
harness publication site, and correct any test/doc assertion that reads the
signal as "pending completions". The predicate itself stays.

---

## C3 — reusing the same lock objects still adds lock acquisitions

**Verdict: CONFIRMED.**
Fix class: **documentation / caliber correction** (report the counts and the
SHADOW−R bound caveat); no code change this round — any lock-path change is a
policy/perf experiment.

### Inventory (submitter path; all citations this session)

Per refill **group** (one `Adapter::submit` call; d16 → 16 ops):

| Lock | Acquisitions | Who / where | Arm dependence |
|---|---|---|---|
| `SharedLoop::mutex` (harness) | **+1 per group** | facts snapshot, `bench/bench_w03_external_loop.cpp:417-422`, gated by `reads_pressure_facts()` (`bench/r3_ready_batch.hpp:375-378`) | only S/SNI/SNP/SHADOW; R/Q2/Q8/B/A0 take none here |
| `SharedLoop::mutex` | ~2–3 per driver wake cycle + cv-wait machinery | `bench/bench_w03_external_loop.cpp:201,214/219,248,523-539` | arm-independent |

Per **decision** (while-loop iteration) of a budgeted arm that reads inflight:

| Lock | Acquisitions | Who / where |
|---|---|---|
| `AsyncIoContext::access_mtx_` | **1** | `AsyncIoContext::outstanding()` `src/async/async_io_context.cpp:837-840` |
| `RequestCore::mutex_` (core) | **1** | `outstanding_summary()` `src/async/detail/request_core.cpp:93-96` |

So **`outstanding()` is not a free read — it takes two locks** (context
`access_mtx_`, then core `mutex_`), called once per decision from
`bench/r3_ready_batch.hpp:467-471` by S/SNP/SHADOW (R never: it skips the
budget block).

Per **presentation** (one `context_submit_ready_reads`):

| Lock | Acquisitions | Where |
|---|---|---|
| `access_mtx_` | 1 (held across the whole call) | `include/sluice/async/detail/test_access.hpp:69` |
| `dispatch_mtx_` | 1 (batch path; held across admit loop + dispatch + transport submit) | `src/async/uring_backend.cpp:504-590` |
| core `mutex_` | 1 extra when stats enabled (`note_outstanding`) | `src/async/uring_backend.cpp:459-465,610` → `request_core.cpp:93-96` |

Lock order documented at `include/sluice/async/detail/test_access.hpp:47`:
`access_mtx_ → dispatch_mtx_ → core mutex_`.

Per accepted **request** (batched presentation path):

- core `mutex_` ×3: `reserve` (`src/async/uring_backend.cpp:666` →
  `src/async/detail/request_core.cpp:165-182`), `accept`
  (`src/async/uring_backend.cpp:726` → `request_core.cpp:184-210`),
  `claim_execution` via `dispatch_one_locked`
  (`src/async/uring_transport.cpp:156-157`).
- Driver-side completion path per request (arm-independent): `offer_terminal`,
  `release_execution`, `begin_publication`, `complete_publication`, observer
  claim/retire — one core `mutex_` each, several under `dispatch_mtx_`
  (`src/async/uring_backend.cpp:801-830`, `request_core.cpp:346-484`).
- Settle path per request (arm-independent): `Request::ready()` → `lookup`
  (core `mutex_` ×1, `include/sluice/async/request.hpp:130-131` →
  `request_core.cpp:251-258`); `take_result()` → `consume_public_result`
  (core `mutex_` ×1, `request.hpp:146-151` → `request_core.cpp:323-344`).

### The two specific sub-claims

- *"新增一次 shared.mutex 获取"* — CONFIRMED: exactly one extra
  `shared.mutex` acquisition per refill group on the submitter thread for
  S/SNI/SNP/SHADOW vs none for R/Q2/Q8/B/A0
  (`bench_w03_external_loop.cpp:417-422`).
- *"outstanding() 是否额外持锁"* — CONFIRMED: `access_mtx_` + core `mutex_`
  per call (`async_io_context.cpp:837-840`, `request_core.cpp:93-96`), i.e.
  the S decision loop repeatedly acquires the same `access_mtx_` the submit
  path uses, per chunk re-entry.

### Frequency asymmetry and the SHADOW−R bound (do not misapply)

Dynamic, archived: at d16, S makes ≈8 decisions per 16-op group (q2 chunking;
`r4/logs/r4_diag_lines_sample.log`; `r4/logs/w03_smoke_Q2.log`
presentations=32 for 64 ops) → ≈8 `outstanding()` double-lock reads per group.
SHADOW makes ≈2 decisions per group (cap re-entry only;
`r4/raw/diagnostics/logs/b00-p0-SHADOW-S-a1.log`: 2501 presentations for 1250
groups) → ≈2 reads per group, i.e. SHADOW reads at roughly **R's presentation
frequency**, ~1/4 of S's read frequency at d16. R reads nothing.

Consequently the R4 SHADOW−R instrumentation figure (CPU +1.02%, one-sided-95
upper +1.94%, p95 +1.95%, p99 +1.22%; `r4/RESULTS.md:182`, verified in
`r5/analysis/env-context.md` §7 item 19) bounds the cost of
"read `outstanding()` + evaluate + record" at **~2 decisions per group**, and
cannot substitute for S's per-decision read cost at ~8 decisions per group.
Reports must keep the combined caliber "read + evaluate + record" and state
the per-arm decision frequency next to any such number.

### Research counters / DecisionRecord in the timed path

For budgeted arms the timed path (no `SLUICE_595_R4_DIAG_LINES`, `trace_ ==
nullptr`) still constructs the full `DecisionRecord` (~30 fields) and executes
the ~10 `r4_*` counter increments per decision
(`bench/r3_ready_batch.hpp:446-517`; only the trace push at line 537 is
gated). Non-budgeted R pays only the small record at lines 521-525. These
costs are part of the measured S/SHADOW arms; they must **not** be deleted
before any comparison against historical R4 numbers (deleting first would
invalidate SHADOW−R and S−R references). Listed here; no change this round.

---

## C4 — `active_data` is the core's accepted-but-**unpublished** count, not "kernel inflight"

**Verdict: CONFIRMED** (name/semantics mismatch; the ask's sharper hypothesis
"accepted-but-uncollected" is refuted in the narrower direction — see below).
Fix class: **naming + documentation only** (research fields/reports; no
production rename, no new global atomic counter).

### Counter lifecycle (source)

`ctx_.outstanding()` → `backend_->outstanding()`
(`src/async/uring_backend.cpp:929-930`) → `core_->outstanding_summary()`
returns the `outstanding_` counter (`src/async/detail/request_core.cpp:93-96`).

- `++outstanding_` at **accept** — the reserved→accepted transition, before
  any validation of transport, kernel, or completion
  (`request_core.cpp:202` inside `accept()`, lines 184-210).
- `--outstanding_` at **complete_publication** — the instant `published` is
  set (`request_core.cpp:472-473`).
- **No** decrement at terminal-chosen (`offer_terminal`, lines 346-365), at
  CQE/kernel completion, or at collection (`consume_public_result`, lines
  323-344, does not touch `outstanding_`).

So the count covers, in the RequestCore state vocabulary: **accepted ∧ not
yet published** — spanning accepted-but-undispatched (dispatch backlog),
prepared, kernel-consumed (真 kernel inflight), kernel-completed-but-unpublished,
and terminal-chosen-but-unpublished. It **excludes** published-but-uncollected
results (already decremented at publication). Hence it is *narrower* than
"accepted-but-uncollected": a request whose result is published and waiting
for `take_result()` is **not** counted.

### What zero does and does not infer

- `active_data == 0` ⟹ no accepted-unpublished work ⟹ no undispatched
  accepted backlog, and — **absent live cancel/control entries**, which
  survive publication until their control CQE (`include/sluice/async/detail/uring_backend.hpp:210-219`)
  — no kernel in-flight operations. In W03 (no cancels) zero does imply a
  fully drained pipeline at the observation instant.
- `active_data == 0` does **not** imply: the SQ ring is empty (control SQEs
  after cancels), that no published-uncollected results exist, or anything
  about notification/eventfd state.
- `active_data > 0` does **not** imply kernel inflight > 0 (could be pure
  dispatch backlog, or completed-but-unpublished work between passes).

This matches the R4 SNP reading: zero-inflight moments coincide with drain
phases (the diag shows `active=0` exactly at `cause=first` decisions,
`r4/logs/r4_diag_lines_sample.log`), consistent with the archived SNP anomaly
interpretation (`r5/analysis/env-context.md` §7 item 14).

### Rename suggestion (research surface only)

- `DecisionRecord.active_data` / `budget::Input.active_data` →
  `unpublished_accepted` (or `accepted_unpublished`).
- Row `inflight_zero` (`bench/r3_ready_batch.hpp:236-240`) →
  `no_unpublished_accepted`; arm flags `read_inflight` /
  `inflight_hint_active` → `read_unpublished` / `unpublished_hint_active`.
- Research docs/reports must state: zero = "no accepted-unpublished work"
  (drain-phase signal), not "kernel idle"; nonzero is not "kernel busy".
- No new global atomic counter to preserve the old name's meaning (per the
  round constraint); `outstanding_summary()` remains the single derived
  summary of `occupancy().outstanding` (`request_core.hpp:180-183`).

---

## Checks run / not run this session

Run (read-only): source reads cited above; `git -C r5/worktree rev-parse HEAD`
and porcelain; greps closing the consumer sets of `batch_shaped_prefix` /
`eligible_prefix`; archived R4 diag/log reads
(`r4/logs/r4_diag_lines_sample.log`, `r4/logs/w03_smoke_{S,Q2,Q8,R,SHADOW}.log`,
`r4/raw/diagnostics/logs/b00-p0-SHADOW-S-a1.log`).

Not run (and why stated): no counting probe or disassembly — the static
structure plus archived per-decision lines already decided every claim at the
source level, so a probe would add nothing; no timed measurement, so no
MEASUREMENT.lock receipt exists for this subtask; no build was produced (no
scratch directory needed).

Verification layer note (per the honesty rule): all "N + (N−q) + …" figures
are **source-level inspection counts** (loop trip counts and per-iteration
scan lengths derived from the code and cross-checked against archived
counters), not measured dynamic instruction counts or latencies.
