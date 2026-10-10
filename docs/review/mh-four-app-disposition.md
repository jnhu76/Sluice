# M-H four-app consumer disposition (#474, Phase M closeout wave 2)

> **Decision overlay (subsequent to this evidence snapshot):** [owner-selected Clean Minimal V1](m-phase-clean-minimal-v1-owner-direction.md) chooses **KEEP_REFERENCE_APP_BEHAVIOR / MIGRATE_AWAY_FROM_LEGACY_RUNTIME**, *not* indefinite KEEP of ApplicationRuntime/Scheduler in v1. D-H2 moves to early shared app-local validation; D-H4 uses existing `Result<TailResult>` rather than adding a public settlement type. All observed facts and counterexamples below remain historical evidence; no F3 code migration has yet run.

- **Status**: DISPOSITION_INPUT_COMPLETE — PENDING_OWNER_DECISION on the five
  flagged items (D-H1..D-H5 below). Nothing in this record is adopted by
  itself; it is the decision-ready input the owner ruling turns into the M-H
  family disposition.
- **Baseline**: live master `94b5cfcdace6ee15c75c909dae09665eda2d1a26`
  (verified at slice start; working evidence gathered on this tree).
- **Authority order applied**: root v1-r3 > ADR-0003 (ADOPTED —
  DESIGN_AUTHORITY_ONLY) > #402 current text > F0/F1 adopted artifacts > #458
  reset (KEEP/ISOLATE/REIMPLEMENT/REMOVE framing) > ledger/tests > live
  implementation.
- **Prior waves consumed**: e1 COPY-B oracle (PR #477, merged `b0f55285`), e4
  TAIL oracles (PR #478, merged `e44a8707`), e2 README reality alignment (PR
  #479 @ `901acfca`, Gate-0 independent review ACCURATE, merge authorized =
  owner's), e1 settlement (#481/#484/#485/#486, final master `94b5cfcd`).

## 1. Per-app disposition record

All four apps run on the same substrate:
`ApplicationRuntime` + `ThreadPoolBackend` — installed public headers
(`include/sluice/async/application_runtime.hpp`), wrapping `AsyncIoContext` +
`Scheduler` + fiber groups (`src/async/application_runtime.cpp:125-133`,
driver at `:458-480`). ADR-0003 §8 (`docs/adr/0003-narrow-stackful-io-host.md:258`)
classifies this face **COMPATIBILITY_ONLY**: "still compiles and serves
existing apps/tests; no v1 support claim; retirement with #402 after consumer
migration". ADR-0003 §1 (`:65-68`) additionally forbids design adoption from
forcing app CLI changes, and §8 (`:269-271`) requires per-consumer
feasibility/behavior decisions before any COPY-B/TAIL port.

### Common disposition (all four apps)

| field | value |
|---|---|
| DISPOSITION | **KEEP** on the compatibility substrate (candidate): no forced port to `StackfulIoHost`, no CLI contract change, no app source change in this wave |
| AUTHORITY | root MIG-02 "ApplicationRuntime/Scheduler/Fiber" row (retain consumers); ADR-0003 §1/§8 COMPATIBILITY_ONLY; #458 reset (KEEP allowed, does not grant supported-v1); #475 precedent (B/C = KEEP_INSTALLED_COMPAT) |
| CONSUMERS | all eight app census rows isolated under tier-2 unit **T2-08** (`c-x01-001..004` app task carriers, `c-x16-011..014` fiber parks; `docs/review/m-r-consumer-disposition-final.json:28-66,623-661`, POLICY_ISOLATED) |
| PRESERVED_BEHAVIOR | per-app rows below; the three tier-2 obligations OB-1/OB-2/OB-3 (`docs/review/m-r-tier2-isolation-record.md:81-92`) gate every future hand that retypes these apps |
| EVIDENCE | e1 21-case scripted-backend oracle (#477), e4 6+10-case lifecycle/CLI oracles (#478), 10/5/4-case consumption suites, four READMEs reality-aligned (#479), full suite 53/53 noliburing release re-run this wave (Gate-0 record, PR #479 comment 6096627555) |
| OWNER | F3 (family arm: legacy runtime retirement); T2-08 carriers F2+F3 |
| EXIT_TRIGGER | ADR-0003 D2/W-04 implementation verification passes AND the F3 host arm exists; then per-app port decisions (COPY-B/TAIL per-consumer feasibility per ADR-0003 §8:269-271), each re-proving the app consumption suites + COPY-B pipeline + TAIL scan in both profiles (T2-08 exit condition) |
| DOWNSTREAM_F_GATE | F3 (M-H family gate); F2 shares T2-08 carrier retirement |

### Per-app specifics

**sluice-copy** — KEEP. Preserved: Version A/B/C modes, bounded reusable-buffer
pipeline (depth ≤ 64, ≤ 512 MiB product), atomic temp+rename output with
EINTR-retried directory fsync, exit-code contract, strict CLI parsing. Proof:
`app_copy_consumption_test` (10), `app_copy_pipeline_oracle_test` (21, incl.
3 harness self-checks), `app_copy_dir_fsync_test` (4); five mutation families
M1-M5 killed once each (recorded in #477). Obligations OB-1 (pipeline
semantics survive any retype) and OB-2 (atomic commit semantics survive the
X-13 posix_retry move, owner F4) are already recorded in the tier-2 record.

**sluice-hash** — KEEP. Preserved: streaming SHA-256 with bounded memory,
deterministic CLI-order output, per-file error isolation. Internally already
uses the canonical `File` resource through the runtime bridge (census APPS
deep-audit); its only legacy dependency is `run_task_to_result` /
`ApplicationRuntime` (T2-08 chain). Proof: `app_hash_consumption_test` (5).

**sluice-grep** — KEEP. Preserved: bounded streaming scan, deterministic
output ordering, per-file error isolation. Proof: `app_grep_consumption_test`
(4). Carries open decision D-H1 (output separator) below.

**sluice-tail** — KEEP. Preserved: bounded backward scan, follow model,
cross-thread signal stop through the public lifecycle, descriptor-follow
rotation semantics, truncation reset. Proof: `app_tail_consumption_test` (5),
`app_tail_lifecycle_oracle_test` (6), `app_tail_cli_oracle_test` (10, incl. 3
harness self-checks); M1-M5 mutation families killed once each (#478 record).
Known debt recorded in the README: no test observes a `drain()`/`join()`
return value — settlement *completion* is observed, settlement *failure
reporting* has no public channel (D-H4 below). The synchronous poll sleep
inside the runtime task is the quantified maintenance coupling the #474
charter asked to name: at the 200 ms default it costs ~1 scheduler tick / 3 s
idle (measured, README §Follow model); it disappears only under a host-owned
wait (F3 arm).

## 2. `--workers` actual execution semantics (record, no code change)

Measured through the full plumbing this wave (source-verified):

- All four apps submit **exactly one** Runtime task; `--workers` reaches only
  `RuntimeBuilder::workers(n)` → `ApplicationRuntime(worker_count_)` →
  `Scheduler::run_live(worker_count_)` (`include/sluice/async/task_result.hpp:84-98`,
  `src/async/application_runtime.cpp:480`) — it sizes the **scheduler
  fiber-driver thread count**, nothing else.
- It does **not** configure the backend: every app constructs a
  **default** `ThreadPoolBackend` (`ThreadPoolConfig` = 64 slots / 4 workers;
  `copy_task.cpp:322`, `hash_task.cpp:142`, `grep_task.cpp:163`,
  `tail_task.cpp:329`).
- It creates **no app-level parallelism**: hash/grep loop their files
  sequentially inside the single task (`hash_task.cpp:84-93`,
  `grep_task.cpp:94-99`); tail has one follow task; copy's read parallelism
  is governed solely by `--pipeline-depth` (the task body never reads the
  worker count, `copy_task.cpp:48-55`).
- Multi-worker CLI behavior is **preserved** (PROD-02 stays Deferred; the
  #402 ban on silent multi-worker→single-owner conversion is respected).

The four READMEs now document exactly this (#479). No further action owed in
M; any semantic change would be a production decision for F3.

## 3. Three DOC_IMPL decisions + two carried decisions (all PENDING_OWNER)

### D-H1 — GREP multi-file output without `-n` (product decision)

Observed (re-measured this wave on the built binary): `g1.txthello one` —
the path and the line concatenate with no separator
(`apps/sluice-grep/main.cpp:67-71`: path written under `if (prefix_name)`,
`':'` emitted only under `if (prefix_name && args.line_numbers)`). GNU grep
and the app's own earlier README both expected `path:`. Machine-readable
consumers have the unambiguous `-n` shape as a workaround.

Options: (a) rule the concatenated shape an output **defect** and authorize a
minimal writer fix (one production line + one oracle case + README update);
(b) rule it **intended** (README stays as the contract of record).
**Recommendation: (a)** — the evidence (conditioning bug on a switch the
single-file/-n shapes don't exhibit, GNU parity, prior README) points to
oversight, and the fix is one reviewable line. Not executed here: production
change requires owner authorization. Owner: #474 ruling → F3 executes.

### D-H2 — out-of-range `--buffer-size` / `--pipeline-depth` / `--max-line-bytes`: parser usage error (exit 1) vs engine `invalid_state` (exit 2)

Current measured behavior: COPY enforces ranges in `run_pipelined_copy*`
(`copy_task.cpp` limits) → exit 2 `copy failed: invalid_state`; HASH/GREP
enforce in the engine → per-file exit 2 with a **generic** `read error`
diagnostic that names neither the parameter nor the problem; TAIL is the only
app that range-checks everything at parse time (exit 1).

Options: (a) keep layering, fix only HASH/GREP **diagnostics** (name the
parameter; smallest production change; exit-code contract unchanged and
already oracle-recorded); (b) move all ranges into the parsers (uniform exit
1, matches usage text; touches all four CLIs' tested exit-code contract);
(c) keep everything as-is (divergence documented in READMEs).
**Historical wave-2 recommendation (SUPERSEDED by owner-selected target): (a)** — preserves the exit-code contract that e1/e2
evidence froze, removes the misleading diagnostic. This paragraph remains as
pre-decision evidence, **not** the currently chosen D-H2 action.

**Owner-selected D-H2 direction (implementation and revised app contract still pending):**
Use one app-local configuration validator from CLI and engine; fail invalid
configuration before file/temp/output side effects. COPY/HASH boundary usage
becomes exit 1; GREP usage/error stays exit 2 and no-match stays exit 1.
Keep engine-level typed rejection; add boundary/side-effect oracles and
update READMEs alongside the later application fix. Owner: #474 app/F3.

### D-H3 — COPY-B waiter-level error precedence (CONTRACT_DECISION_REQUIRED, carried from e1)

Spec status: **no current authority text constrains waiter-level error
precedence** — root SEM/ERR requirements address operation-level error
semantics; ADR-0003 does not name the copy task's wait discipline. The
reachable arm is oracle-frozen (e1 #477): drained op *results* never override
the primary error (`await_drain` discards `c.result()`,
`src/async/await_op_helpers.cpp:23-30`; `copy_task.cpp:273-284`). The
waiter-*failure* arm (a wait error returned before primary-error selection,
`copy_task.cpp:277/:282`) is source-confirmed but unreachable through the
public single-task API with a conforming backend — its only inputs are
wait-record exhaustion, observer-attach failure on a non-ready completion, or
waiter cancellation, none of which a single-task fiber can produce
(`tests/support/pipelined_copy_oracle_support.hpp:28-38` records the
analysis).

Which branch is observable: today, none, in-tree. The branch becomes
observable in exactly one downstream phase: **F3's T2-08 retype arm** — when
the copy task is re-expressed over `Request<T>` or the runtime retires, a
restructured task could hold multiple live waits, making wait-record
exhaustion reachable.

Options: (a) freeze "waiter failure precedes primary error" as contract now
(freezing unobservable behavior); (b) keep it unspecified on the compat
substrate and bind the decision to the F3 retype slice, requiring that slice
to choose and prove it with an oracle (OB-1 already gates that slice).
**Recommendation: (b)** — matches the e1 finding (deliberately not frozen)
and puts the decision where the behavior first becomes observable. Owner: F3
retype slice. Downstream F gate: F3.

### D-H4 — TAIL settlement-failure visibility (carried from e4)

`TailEngine::wait()` discards `drain()`/`join()` return values; no test — and
no public channel — observes a settlement failure. Closing it needs a public
settlement-outcome surface (a product decision, not a docs fix). Owner: F3
(runtime retirement arm); recorded as an open evidence gap in the TAIL README
and in T2-08's obligations. **PENDING_OWNER.**

### D-H5 — the KEEP itself

The per-app KEEP disposition above is a *candidate* backed by this record;
adopting it (and thereby fixing M-H's family gate input) is the owner ruling
requested on #474. It creates no root conflict: KEEP is consistent with
MIG-02, ADR-0003 §8 and #458; no `AUTHORITY_ALIGNMENT_REQUIRED` item arises
from it. OPTIONAL_SUPPORTED is **not** requested and D2/W-04 remain
unverified (ADR-0003 `:15-17`, inequality chain `:58-63`).

## 4. Un-migrated runtime consumers — owners, exit triggers, protected behavior

| unit | surface | owner | exit trigger | protects (app-observable?) |
|---|---|---|---|---|
| T2-01 | X-16 await/waiter runtime surface | F3 | F3 await retype or runtime retirement; waiter/shutdown/file oracles re-proven; B-04 | indirect (all four apps' await chains) |
| T2-02 | core-cutover publication/observer oracles | F2 | F2 publication-seam contraction, every mutant kill kept | no (tests) |
| T2-03 | shutdown settlement oracle on caller-held carriers | F2 | F2 observer-registration hinge contraction | no (tests) |
| T2-04 | RequestHandle identity + release-path oracle | F2 | F2 identity contraction (D-17) | no |
| T2-05 | compat-spelling pins + forced backend seam | F2 | F2 `submit_*(op, Completion<T>*)` removal, E1 matrix re-run | no |
| T2-06 | `Batch::Slot` value carriers | F2 | F2 Batch re-typing + B-01 same-slice re-freeze | no |
| T2-07 | `identity_of` arena fallback (dormant) | F2 | F2 identity contraction, key-equivalence proven | no |
| **T2-08** | **app task carriers and await chains** | **F2+F3** | **F2/F3 arm closure with app suites + COPY-B pipeline + TAIL scan re-proven, both profiles** | **yes — all four apps (`c-x01-001..004`, `c-x16-011..014`)** |
| T2-09 | F1 publication-epilogue compat sub-spelling | F2 | F2 seam contraction with adopted-face oracle | no (tests) |
| T2-10 | file-surface test carriers | F4 | F4 File contraction, semantic reference oracles re-run | no (but gates M-F tests) |
| T2-11 | installed-package consumer probes | F2 | F2 contraction slice re-freezing both manifests with the probes | no |
| T2-12 | cross-thread submission-observation carve-outs | F2 | F2 carrier retirement, oracles re-derived over adopted spelling | no |
| OB-1/2/3 | COPY-B pipeline / atomic commit / TAIL scan obligations | F2+F3 / F4 / F3 | as recorded per obligation in the tier-2 record | **yes** |

Every `POLICY_ISOLATED` and `COMPAT_TEST_RETAINED` row carries its reason in
`m-r-consumer-disposition-final.json` (75 census rows / 82 subscopes, 0
unexplained; `scripts/verify_mr_disposition.py` recomputes it in CI). No
consumer was deleted to shrink counts in this or prior waves.

## 5. What this record closes

With the owner's D-H1..D-H5 rulings, M-H's family-gate input is complete:
per-app dispositions (KEEP), `--workers` semantics recorded, DOC_IMPL items
decided or assigned with owners/exit triggers, and every un-migrated runtime
consumer carrying owner + exit trigger + protected behavior. The remaining
M-H blockers are then the D2/W-04 verification and the F3 execution arms —
correctly downstream of M, not inside it.
