# Sluice v2 Conformance Ledger

## Header

| Field | Value |
|---|---|
| Governing specification | [`docs/explicit-io-contract.md`](../explicit-io-contract.md), revision **v2-r1** (effective on the merge of its adopting PR; GOV-05) |
| Assessed source commit | Tree `c001bb1eb21f19c045e57558e13e5487335e1e86`. The C++ content is identical to the v2-r1 implementation baseline `b870a7c0` (the commits between them change documentation only) |
| Date of assessment | 2026-10-16 |
| Supported-contract target | v2 contract of the governing root: W-01 direct execution, W-02 bounded scope, W-03 external loop, ThreadPool and io_uring request backends, **11 installed public headers** (root Appendix B) |
| Actual implemented surface | **25 installed headers** (`xmake/libraries.lua` at the assessed commit), including the four host/cancel headers pending deletion and seven seam/detail headers pending internalization; `AsyncBackend` is a public abstract type in `sluice/async/async_io_context.hpp` and context construction takes a caller-built `std::unique_ptr<AsyncBackend>` |
| Evidence scope and limitations | See each row. `VERIFIED` names the property, evidence artifact, configuration and commit; carried-forward claims from the v1 campaign (Git history at `c001bb1e`) were re-checked for source correspondence — the assessed tree's C++ is identical to `b870a7c0` — but the underlying test/model runs were not all re-executed for this ledger. Spot re-executions run for this assessment are marked `live 2026-10-16`. Bounded TLC/Lean results establish only their explored instances |
| Overall conformance status | **RELEASE-READY at PR head (2026-10-17 convergence PR).** The retained execution paths (direct, ThreadPool, io_uring, scope, observer, progress, shutdown) are implemented with scoped verified evidence at recorded commits; the public-boundary contraction (D1–D3) is VERIFIED (PR #545 merge); every release gate RG1–RG7 carries a status (see the Release Gate table) and the release-acceptance matrix is recorded in `docs/evidence/v2-release-acceptance-20261017/`. Statuses stated at the convergence PR's head become effective on its merge (GOV-05). The dated assessment rows below retain their scoped-at-record wording |

This ledger replaces `docs/roadmap/v1-conformance.md` (deleted by the same
PR; its campaign records are dated evidence in Git history). It tracks
implementation and evidence against the v2 root; it cannot relax any
requirement. The v1 root's phases, gates and statuses do not transfer.

## Status vocabulary (GOV-02)

| Status | Meaning |
|---|---|
| NOT_ASSESSED | No complete assessment against the v2 requirement recorded |
| GAP | A specific missing or nonconforming behavior identified with source evidence |
| IMPLEMENTED_UNVERIFIED | A candidate implementation exists; required evidence is incomplete |
| VERIFIED | Named requirement passed the recorded evidence for the stated commits/configurations |
| VERIFIED_AT_PR_HEAD | A qualified VERIFIED used by the release-gate table: the gate's evidence is complete and checked at the convergence PR's head; the status becomes plain VERIFIED when that PR merges |
| DEFERRED_OUT_OF_SCOPE | Not a required v2 capability; no supported-product claim |

A required v2 obligation can never be `DEFERRED_OUT_OF_SCOPE`. `VERIFIED`
never means universal correctness: it is bounded to the stated property,
commit and configuration. Split a row rather than blend evidence classes.

## Groups

Columns: Requirement IDs · C++ owner · Current state · Target · Status ·
Evidence (SHA/config) · Missing proof or gap · Next owner.

### G1 File semantics, errors, durability, W-01

| Requirement IDs | C++ owner | Current state | Target | Status | Evidence (SHA/config) | Missing proof or gap | Next owner |
|---|---|---|---|---|---|---|---|
| SEM-01..04, ERR-01 (shared oracle, open/close, precedence, offsets) | `detail/file_semantics.hpp`, `file_resource.*`, `blocking/file.hpp`, `src/blocking_file.cpp` | Shared oracle serves direct and both request paths; 0 recorded divergences | Root §6/§7 | VERIFIED (carried) | v1 campaign semantic suites at `dd83595d`/`b870a7c0` (Git history `c001bb1e`); in-tree `semantic_*` suite; request-path errno re-diff `request_error_domain_test` (#531 matrix liburing=y 68/68, liburing=n 53/53) | `detail/file_semantics.hpp` left the install set with PR B (G9 row below: uninstalled-header negative probe) | — |
| SEM-05 short I/O / EOF / composition | `blocking/file.hpp` (read_exact/write_all), request primitives | Direct compositions + short-count rules on both backends | Root §6 SEM-05 | VERIFIED (carried) | A2 record (v1 ledger, Git history); `semantic_short_io_reference_test` in tree | — | — |
| SEM-06 durability | blocking + request sync paths | sync_data/sync_all incl. resize coverage (V27) on direct and request paths; the named suite runs in all 8 release-matrix cells | Root §6 SEM-06 | VERIFIED (carried; release-matrix enumeration discharged 2026-10-17) | F6 requirements matrix (`docs/evidence/f6-final-correspondence-20261014/`); `semantic_durability_reference_test` in tree | Durability claims rest on documented OS/filesystem guarantees | — |
| SEM-07 metadata/identity | `file_resource.*`, request `file_info`/`size` | kind/size/identity incl. request-path statx ops | Root §6 SEM-07 | VERIFIED (carried) | A2 + E1 records; `semantic_reference_case_test`, the cross-backend contract-conformance suite | — | — |
| ERR-02 effects | effect/measurement vocabulary, backends | confirmed-bytes vs unknown-effects reporting incl. cancel races (V14/V15) | Root §7 ERR-02 | VERIFIED (carried) | E1 records; #526/#531; `semantic_effect_outcome_test` in tree | — | — |
| W-01 direct utility + core-only build (V25) | `sluice_core` | Direct W-01 works with no context/worker/request table; core-only consumer builds | Root §3 W-01 | VERIFIED (carried) | A2 record; `direct_w01_consumer_probe` + F1 core-only clean-room builds | — | — |

### G2 Request acceptance, identity, publication and reclaim

| Requirement IDs | C++ owner | Current state | Target | Status | Evidence (SHA/config) | Missing proof or gap | Next owner |
|---|---|---|---|---|---|---|---|
| REQ-02..06, HANDLE-01..03 (acceptance transaction, lifecycle, publication, reclaim, settlement liveness, handles) | `src/async/detail/request_core.*`, `include/sluice/async/request.hpp` | Context-owned core on both backends; public `Request<T>`; occupancy() is the settlement authority; derived `outstanding_summary()` non-authoritative | Root §8/§9 | VERIFIED (carried) | B1-B/C/D + B2 records (v1 ledger, Git history); `RequestCore.tla` safety+liveness PASS re-run at `64e3241b`, re-proved post-#524 convergence at frozen `28f0fe33` (`docs/evidence/issue-524-requestcore-convergence-20261015/`); `public_request_test`, `request_core_protocol_test` in tree | Model coverage is bounded to the modeled identity domain; see REQ-01 row | — |
| REQ-01 identity domain / exhaustion (V24 boundary) | `request_core.*`, backends | Identity domain enforced end to end on the real allocation and admission paths: monotone CAS cursor with an exhaustion latch (`0` never live, `UINT64_MAX` yields once), generation-max slot retirement, serialized reserve/accept; no production change required — the pre-campaign gap was evidential | REQ-01: no aliasing within valid domain; explicit failure before wrap | VERIFIED (at `a0874ae2`) | `docs/evidence/v2-release-acceptance-20261017/req01-identity-domain-evidence.md`: concurrent allocation under a raced boundary cursor, address-reuse stale-ID rejection, retired-slot-domain submit rejection on ThreadPool and io_uring; plus carried `generation_exhaustion_retires_slot` / `reuse_advances_generation_without_aliasing` and the RequestCore model's per-identity L1/L5 | Bounded modeled identity domain (model limitation, unchanged); no 2^64-scale execution attempted (boundary-injected) | — |

### G3 RequestScope, bounds, settlement, W-02

| Requirement IDs | C++ owner | Current state | Target | Status | Evidence (SHA/config) | Missing proof or gap | Next owner |
|---|---|---|---|---|---|---|---|
| §16 scope obligations, W-02, V18/V19 | `include/sluice/async/request_scope.hpp` | Owned-driver bounded scope: reserve-before-acceptance, settle on normal/early/exception exits | Root §16 | VERIFIED (carried) | D1 record #398 (six killed mutants, V18-A/B/C + V19 both backends, human final review APPROVE) | — | — |
| BOUND-01/02 budgets incl. resource-bounded progress storage | core + backends; entry-snapshot stores merged in `dd83595d` | Capacities validated at setup; pass accounting storage resource-bounded; admission obeys serialized contract | Root §18 | VERIFIED (carried) | G1 A/B + PR #536 oracles at `dd83595d` (negative pre-fix failures recorded in PR #536); V28 discriminator live 2026-10-16 (below) | — | — |

### G4 Observer and no-lost-wake progress

| Requirement IDs | C++ owner | Current state | Target | Status | Evidence (SHA/config) | Missing proof or gap | Next owner |
|---|---|---|---|---|---|---|---|
| OBS-01..03 (attach/deliver/retire) | core observer phases, `detail/observer_protocol.hpp` | Generation-armed at-most-once delivery; bounded nonthrowing hooks; retirement owned by the delivery owner after its hook returns (PR B review fix, below) | Root §10 | VERIFIED (carried) | C1-A..C1-H records; `ObserverCore.tla` safety floor + conditional liveness, five C++ mutation seams killed; PR B re-base: driver-owned post-hook retirement + public retirement query (`delivery_retired`), deterministic interleaving oracle `retirement_waits_for_the_delivery_hook_to_return` (RED at `e34965a5`, GREEN at `b3de9262`) | Publication happens-before visibility argued by core-mutex serialization, not formal proof (C1-B) | — |
| PROG-02 no-lost-wake (ThreadPool) | `detail/progress_source.hpp`, `threadpool_backend.cpp` | Epoch/token park-wake handshake; suppression bit only while pending wake stands; 14-row settlement-notification matrix complete after #480/#481/#484 | Root §11 PROG-02 | VERIFIED (carried) | C2 campaign records; #481 (merge `31dc948e`), #484 (merge `51eaa8b7`) with named oracles and killed mutants; TP evidence does not extend to other backends | io_uring notification oracle: see G6 | — |
| PROG-01 pass bound + honest residue (V28/V29) | `threadpool_backend.cpp`, `uring_backend.cpp` | TP entry-snapshot bounded pass; uring bounded phases + owed-delivery residue report (PR #543) | Root §11 PROG-01 | VERIFIED (live 2026-10-16) | `xmake run threadpool_progress_pass_bound_test` PASS (exit 0); `xmake run uring_owed_residue_test` 3/3 PASS — this host, xmake debug, Linux 7.2.9 fc44, liburing present, tree `c001bb1e`; merged oracles at `dd83595d`/`b870a7c0` | — | — |

### G5 ThreadPool execution

| Requirement IDs | C++ owner | Current state | Target | Status | Evidence (SHA/config) | Missing proof or gap | Next owner |
|---|---|---|---|---|---|---|---|
| BACKEND-02/03 ThreadPool profile (six request ops, explicit setup failure, fault paths) | `src/async/threadpool_backend.cpp` | All six ops incl. metadata on workers; construction throws rather than degrading; cancel dispositions incl. `physical_interruption_unsupported` | Root §15 | VERIFIED (carried) | E1 records + F6 matrix; xmake targets `backend_contract_conformance_threadpool_test`/`_uring_test` from `tests/backend_contract_conformance_test.cpp` + killed `backend_contract_conformance_mut_*` builds (renamed from the E1-stage spellings on 2026-10-17, see the hygiene record below); scoped to recorded commits/configs; the suite runs in all 8 release-matrix cells at the convergence head (`docs/evidence/v2-release-acceptance-20261017/matrix/matrix.json`) | Discharged 2026-10-17 (G10 release matrix) | — |

### G6 io_uring execution

| Requirement IDs | C++ owner | Current state | Target | Status | Evidence (SHA/config) | Missing proof or gap | Next owner |
|---|---|---|---|---|---|---|---|
| BACKEND-02/03 io_uring profile (statx metadata, sticky cancel intent, poison convergence, availability gate) | `src/async/uring_backend.cpp`, `detail/uring_submit.hpp` | Required profile incl. statx probe gate; `-ECANCELED` terminal with unknown remainder; transport/completion split | Root §15 | VERIFIED (carried) | B1-C/E1/C2-C records; xmake target `backend_contract_conformance_uring_test` from `tests/backend_contract_conformance_test.cpp` (15 tests, real kernel WSL2 6.18.40.1, liburing 2.14, gcc 15.2.0, tmpfs; target renamed 2026-10-17, evidence unchanged); the suite runs in all 4 liburing=y release-matrix cells on Fedora 44 / kernel 7.2.9 / liburing 2.13 at the convergence head (`docs/evidence/v2-release-acceptance-20261017/matrix/matrix.json`), satisfying the declared-Linux-configurations requirement | Discharged 2026-10-17 (G10 release matrix; WSL2 record retained as dated evidence) | — |
| PROG-02 io_uring settlement notification | `uring_backend.cpp` + ProgressSource | Deterministic no-lost-wake oracle suite (23 named oracles) covering 17 notification-timing classes, including a registered final-probe mutation kill and a real-kernel cancel-race/quiescence oracle; no production change required for the gate itself | Root §11 PROG-02 for both backends | VERIFIED (at `6411cff3` + `5fd8e109`) | `docs/evidence/v2-release-acceptance-20261017/prog02-settlement-notification-oracle.md` (coverage table, two-notions separation, mutant discrimination); oracles in `tests/uring_progress_race_test.cpp`, companion `tests/uring_owed_residue_test.cpp`; matrix cells run them every pass | Mutant kill is a registered mutation build, re-run per VERIFY-06 on probe/oracle change (not a regular test) | — |
| PROG-01 uring owed-delivery residue (V29) | `uring_backend.cpp` | Owed-delivery events reported as immediate residue; fixed by PR #543 | Root §11 PROG-01 | VERIFIED (live 2026-10-16) | `tests/uring_owed_residue_test.cpp` merged at `b870a7c0`; live 3/3 PASS this host (config above) | — | — |

### G7 Shutdown, cancellation and destruction

| Requirement IDs | C++ owner | Current state | Target | Status | Evidence (SHA/config) | Missing proof or gap | Next owner |
|---|---|---|---|---|---|---|---|
| CANCEL-01/02 operation cancel + deadlines | core + backends | Dispositions `won_before_execution`/`requested`/`already_terminal`/`not_found` + `physical_interruption_unsupported`; raced success never rewritten (V14); unknown effects (V15) | Root §14 | VERIFIED (carried) | E1 records; the cross-backend contract-conformance suites on both backends; `RequestCore.tla` `InvSingleWinner`/`InvCancelWinRequiresUnclaimed` | — | — |
| SHUT-01..04 lifecycle, V20/V21/V22/V23/V26 | `src/async/async_io_context.cpp` (ADR-0004 spelling) | Five-state settlement driver; idempotent shutdown; owner-checked; ExecutionClosed ≠ Destroyable; notification retirement before fd close | Root §17 | VERIFIED (carried + live 2026-10-16) | E2/PR #453 merge `eb7bf055`; `shutdown_lifecycle_test` 9 TP / 17 uring scenarios; `ShutdownCore.tla`; 171-gate TLA run attempt 2 `VERIFY_TLA: PASS`; live run PASS this host (config above) | — | — |
| LeakSanitizer finding, poisoned-shutdown path (io_uring) | `tests/shutdown_lifecycle_test.cpp` scenario `poison_with_in_flight_cqe` | Test-structural finding fixed by isolating the deliberate unresolved-borrow witness in a forked child (`std::_Exit` bypasses the atexit leak check); scenario, assertions and contract semantics intact; no production change | No leak on any shutdown path at release | VERIFIED (at `ff7d6dde`) | `docs/evidence/v2-release-acceptance-20261017/lsan-poisoned-shutdown-disposition.md`: root cause, pre-existence at `5c1b14db` (PR B capture), GREEN in both asanubsan matrix cells; no suppressions, no `detect_leaks=0`, no test removal | — | — |

### G8 Formal models, mutations and C++ correspondence

VERIFY-06 is the normative scope table. Distinguished below: model safety,
model liveness, mutation sensitivity, C++ transition mapping, production
execution evidence.

| Requirement IDs | C++ owner | Current state | Status | Evidence (SHA/config) | Missing proof or gap | Next owner |
|---|---|---|---|---|---|---|
| RequestCore model (REQ/VERIFY-02 S1–S3, S6; L1/L5) | `src/async/detail/request_core.cpp` | Safety: 20 invariants incl. `InvBindImpliesPublished`; liveness L1+L5 per identity; #524 convergence removed the retired `ReleaseBind` flavor and migrated mutation gates | VERIFIED (carried, model) | `formal/tla/RequestCore.tla`; safety PASS 31,289,609 gen / 5,359,732 distinct / depth 53; liveness PASS (12 branches = 2 slots × generations 0..2 × {L1,L5}); frozen `28f0fe33` (`docs/evidence/issue-524-requestcore-convergence-20261015/`) | Bounded instances (2 slots, gens 0..2); not C++ memory ordering; no unbounded identity domain | — |
| ObserverCore model (OBS, L3) | core observer phases | Safety floor (at-most-once per generation, cancel isolation, retirement finality) + conditional liveness; 5 mirrored mutation seams killed | VERIFIED (carried, model) | `formal/tla/ObserverCore.tla` (C1-H, #433); cfgs with `PROPERTY` liveness + liveness mutants | Host callback internals and kernel timing unmodeled | — |
| ProgressSource model (PROG-02, V24) | `detail/progress_source.hpp` | Epoch/token park-wake handshake; reachable-state obligations re-proved on corrected close semantics; `MutRepeatCloseSignals` discriminator | VERIFIED (carried, model) | `formal/tla/ProgressSource.tla` (C2-E campaign + re-closure records); gated by `scripts/verify_progress_source_model.sh` | Model predates the C1 suppression mechanism: no suppression action exists in the model; the suppression rule's coverage is the PROG-02 equivalence argument, not a model action; single-boolean readiness | — |
| ShutdownCore model (SHUT, L4) | `async_io_context.cpp` | Five-state lifecycle, `InvCanceledRequiresTargeting`, LiveOpsSettle/LiveShutdownCloses under explicit fairness; per-property negative controls | VERIFIED (carried, model) | `formal/tla/ShutdownCore.tla` (E2); part of the 171-gate run `VERIFY_TLA: PASS` | Host teardown/application consumption timing unmodeled | — |
| Mutation mirrors / correspondence (VERIFY-01/06) | test seams `SLUICE_*_MUTANT_*` | Compile-time protocol mutations killed by named invariants/oracles; in-tree C++ mutation builds recorded across B2/E1/E2/#531/#532 | VERIFIED (carried, campaign-scoped) | 71/72 campaign record (survivor: documented F2 path-dependent case, guard-caught — recorded in `docs/evidence/f2-authority-contraction-20261013/` and the PR #536 receipt, not restated in this ledger); per-slice kills named in carried records | Full campaign is campaign-scale, not per-commit; re-run obligations follow VERIFY-06 on model/transition change | — |
| Historical Lean/FCB1 corpus (`formal/Sluice/**`, Event/Mutex/… TLA+) | retired primitives | Proves retired synchronization-primitive family; zero file-I/O/liveness content | DEFERRED_OUT_OF_SCOPE (evidence asset carrying no required v2 obligation) | Superseded v1 formal evidence map dispositions (Git history `c001bb1e`); the in-tree copies (Lean `formal/Sluice/**`, the Event/Mutex/… TLA+ modules, `scripts/verify_formal.sh` and the retired `verify_tla.sh` stages) were removed from the active tree on 2026-10-17 and remain recoverable from Git history | None required; no v2 claim rests on it | — |

No model result is a production execution claim: C++ transition mapping is
recorded per campaign (correspondence headers and the #524
correspondence-before record), and production evidence lives in G1–G7 rows.

### G9 Public API and installation boundary (owner decisions D1–D3)

| Requirement IDs | C++ owner | Current state | Target | Status | Evidence (SHA/config) | Missing proof or gap | Next owner |
|---|---|---|---|---|---|---|---|
| Appendix B install surface (D2) | `xmake/libraries.lua` | 11 installed public headers (precise allowlist for both targets) | 11 installed public headers | **VERIFIED (PR B)** | Allowlist in `xmake/libraries.lua` at PR B head; clean-room exact-set prefix gate + sole-include compiles in `docs/evidence/pr-b-boundary-contraction-20261016/v2-package-manifest-{noliburing,liburing}.json` (all gates PASS, both profiles) | — | — |
| Host bundle removal (D1, §16) | deleted: `src/async/{stackful_io_host,fiber,fiber_ctx,cancel}.cpp`, 4 public headers, host test + w04 consumer + F4 probe | Bundle physically deleted; no dormant retention; core-only build unaffected | Physically deleted, core-only build unaffected | **VERIFIED (PR B)** | Commit `7d97e010` (bundle + exclusive consumers); archive members 13→9; full matrix green both profiles (8 cells, `verification-record.md` §1); deleted-header absence asserted by the package verifier negative probe | — | — |
| Internal-type leakage / `AsyncBackend` seam (D2, BACKEND-01) | `include/sluice/async/detail/{async_backend,threadpool_backend,uring_backend,test_access}.hpp` (uninstalled) | Backend classes, seam and test hooks internal; public factories `make_threadpool_context` / `make_uring_context` (`Result`, `not_supported`); observer vocabulary on public enums; `AsyncIoContext(unique_ptr<AsyncBackend>)` gone | No public extension protocol; selection explicit without constructing internal backend objects or including detail headers | **VERIFIED (PR B)** | Commits `d0a3a52e`/`f873ba23`; negative seam probes reworked at the review fix to compile the REAL installed headers with pinned diagnostics (commit `d8c57cd5`), mutation self-tested (an installed header re-exposing the backend definition fails the gate); macro-view-independent ODR probe in both package manifests; design record `docs/evidence/pr-b-boundary-contraction-20261016/design-record.md` | — | — |
| Installed-consumer compile/link under target surface (VERIFY-01 build row) | package rules (`scripts/verify_v2_package.py`) | Clean-room consumers on the 11-header surface: core-only P0, async workloads + micro-contracts, both profiles; each consumer executed once (review fix, commit `47a58e87`) | Standalone consumer compile/link incl. core-only on the contracted surface | **VERIFIED (PR B)** | `v2-package-manifest-{noliburing,liburing}.json` every gate PASS at the review-fix head (24 per profile; the manifest records every verdict); verifier behavior self-tested (`scripts/test_verify_v2_package.py`, 14 tests); clean-room firewall gates over the recorded command stream | — | — |

### G10 External event loop W-03 and release acceptance

| Requirement IDs | C++ owner | Current state | Target | Status | Evidence (SHA/config) | Missing proof or gap | Next owner |
|---|---|---|---|---|---|---|---|
| W-03 pollable progress source both backends (PROG-03) | `detail/progress_source.hpp`, backends | eventfd-based pollable handle per backend; fd never a second drain authority; external `poll(2)` integration exercised | Root §11 PROG-03 | VERIFIED (carried; release evidence recorded) | C2-B/C2-C/C2-D records (owner outcomes, sticky control, external-host lifetime, detach acknowledgement); external-loop consumer runs recorded per matrix cell in `docs/evidence/v2-release-acceptance-20261017/w01-w02-w03-consumers.txt` (`threadpool_external_loop_test` all 8 cells; `uring_external_loop_test` + `uring_public_consumer_probe` all 4 liburing=y cells) | — | — |
| Release acceptance: W-01/W-02/W-03 + both backends, PROD-02 matrix | whole library | Full 8-cell matrix executed at the campaign head (debug/release × liburing n/y, clang for asanubsan cells, gcc primary incl. tsan): 56/74-test suites, 0 failed in every cell; 4 reference applications built and behavior-accepted; W-01/W-02/W-03 consumer oracles pass in every applicable cell (36/36) | Release evidence enumerating compiler/arch/kernel/liburing/filesystem; W-01–W-03 pass on ThreadPool and a successfully constructed io_uring backend | VERIFIED_AT_PR_HEAD (gate closes at merge) | `docs/evidence/v2-release-acceptance-20261017/`: `verification-record.md`, `matrix/matrix.json` (per-cell argv/exit/log digests/`xmake.conf`), `environment.txt`, `w01-w02-w03-consumers.txt`, app behavior logs, round-0 defect record, GCC libasan collision disposition; D3 disclosure in CHANGELOG.md `[Unreleased]` | Configurations outside the recorded environment are untested by this release (disclosed) | Merge |

## Release Gate

Tied to v2 requirements, not to v1 phase completion. All gates are blocking
for the first v2 release:

| Gate | Requirement | Condition | Status (2026-10-17 convergence PR, `VERIFIED_AT_PR_HEAD` closes at merge) |
|---|---|---|---|
| RG1 | G7 LSan row | The LeakSanitizer finding on the uring poisoned-shutdown path is fixed with a regression oracle, or granted a formal owner-approved exception with recorded reasoning. (PR B's wider matrix surfaces it under asanubsan-y; pre-existence at `5c1b14db` recorded in `docs/evidence/pr-b-boundary-contraction-20261016/matrix/asanubsan-y-rg1-lsan-disposition.txt`.) | VERIFIED_AT_PR_HEAD — fixed at `ff7d6dde` with the fork-isolated witness; disposition `docs/evidence/v2-release-acceptance-20261017/lsan-poisoned-shutdown-disposition.md`; leak-free in the liburing=y asanubsan cell (the scenario's test target is compiled only under `SLUICE_SHUTDOWN_URING`) |
| RG2 | D1/D2 (root §23.4) | PR B substance complete at the PR B head (G9 rows VERIFIED; audits recorded under `docs/evidence/pr-b-boundary-contraction-20261016/`); gate closes when the PR merges | VERIFIED_AT_PR_HEAD (substance carried from PR #545 merge) — closes at this PR's merge |
| RG3 | VERIFY-01 build row | Substance complete: clean-room core-only and async consumer compile/link/run recorded on the 11-header surface in both profiles (`v2-package-manifest-*.json`); gate closes when the PR merges | VERIFIED_AT_PR_HEAD — both profile manifests re-recorded at this campaign's clean head (see verification record §3); closes at merge |
| RG4 | REQ-01 | Request identity-domain enforcement or proved equivalence, with exhaustion evidence, recorded in this ledger | VERIFIED_AT_PR_HEAD — at `a0874ae2`, `docs/evidence/v2-release-acceptance-20261017/req01-identity-domain-evidence.md`; G2 row updated |
| RG5 | PROG-02 | io_uring settlement-notification evidence beyond the structural argument (deterministic oracle or reviewed equivalence) | VERIFIED_AT_PR_HEAD — at `6411cff3`+`5fd8e109`, coverage table + registered mutant kill + real-kernel cancel-race oracle in `docs/evidence/v2-release-acceptance-20261017/prog02-settlement-notification-oracle.md`; G6 row updated |
| RG6 | PROD-02 | Release evidence enumerates tested compiler/architecture/kernel/liburing/filesystem configurations; W-01/W-02/W-03 pass on ThreadPool and io_uring in that matrix | VERIFIED_AT_PR_HEAD — 8-cell matrix, 0 failed, apps behavior-accepted, `docs/evidence/v2-release-acceptance-20261017/verification-record.md`; closes at merge |
| RG7 | D3 | Release notes disclose removed/internalized headers, the closed extension seam, and the absence of compatibility shims | VERIFIED_AT_PR_HEAD — CHANGELOG.md `[Unreleased]` at `7efd86ec` |

## Provenance of carried evidence

Rows marked `carried` rest on v1-campaign records preserved in Git history at
`c001bb1e` (`docs/roadmap/v1-conformance.md` and `docs/evidence/**` of that
commit) plus the correspondence check recorded in the header: the assessed
tree's C++ is identical to `b870a7c0`, the baseline those records conclude
on. They establish their stated properties at their stated commits and
configurations — nothing broader. A carried claim is re-based (re-run or
correspondence-reargued) when the code it names changes; PR B will
invalidate every row whose owner it touches.

### Dated PR B re-basing (2026-10-16)

PR B (boundary contraction) touched the owners of the G9 rows and the
representation under several carried rows. Dispositions, recorded in full in
`docs/evidence/pr-b-boundary-contraction-20261016/` (design record,
verification record, package manifests, matrix, mutation kills):

- G9: all four rows VERIFIED at the PR B head (above).
- Carried rows whose owner changed only in representation (public factories
  replacing direct construction; out-of-line `RequestBridge` calls replacing
  inline template bodies; public observer enums replacing `detail::`
  vocabulary; `ProgressPass` rename) were re-run, not correspondence-argued
  alone: the full deterministic suite passed in all eight matrix cells
  (debug/release/asanubsan/tsan x liburing n/y), and both moved B2
  release-edge mutants still die on their named oracles. Model
  correspondence for RequestCore/ObserverCore/ProgressSource/ShutdownCore is
  representation-only per the verification record §4.
- `Request<std::size_t>` grew 40→48 bytes (stored identity is the public
  `RequestId`); an allowed breaking API change under D2, no added
  allocation, ODR-pinned by the package probe.
- RG1's LSan finding is unchanged from baseline (disposition recorded);
  RG4–RG7 remained open and untouched by PR B. [Historical statement as
  of the PR B re-basing date; all seven gates now carry a status in the
  Release Gate table after the 2026-10-17 convergence PR.]

#### Review-fix re-basing (same date, head after `e34965a5`)

An external review of PR #545 raised two P1 and two P2 findings; all fixes
are recorded in the verification record §6:

- Observer retirement authority moved to the delivery owner
  (`b3de9262`): both backends retire after their hook returns, the public
  form is the retirement query `delivery_retired` (authoritative while the
  Request binding is retained), and the G4 OBS row above is re-based with
  the RED→GREEN interleaving oracle. This is a correspondence restoration,
  not a representation change: the C++ now executes
  `ObserverCore.ProcessDelivery`'s edge by the modeled owner. Authority
  contraction: the shutdown delivering-phase sweep
  (`RequestCore::retire_delivered_episodes`) became unreachable and was
  removed with the slice; settlement keeps its owed-delivery drain and
  registration convergence.
- Package verifier hardened (`d8c57cd5`, `47a58e87`, `51dc830f`): seam
  negatives compile the real installed headers with pinned diagnostics
  and are mutation self-tested; each consumer runs once; the installed
  mutation-region set is pinned (the two request_scope template seams,
  spelled `SLUICE_D1_MUTANT_*` at the time of the pin and
  `SLUICE_REQUEST_SCOPE_MUTANT_*` since the 2026-10-17 hygiene renames; the
  public-request release-path region moved to an internal TU in `3bace3d4`
  and re-killed, now `SLUICE_PUBLIC_REQUEST_MUTANT_*`).
- Re-verification at the fix head: full 8-cell matrix rerun (seven cells
  green; asanubsan-y only the recorded RG1), both package profiles at 49
  gates PASS with refreshed manifests, B2 mutants re-killed.

## Dated repository-hygiene contraction (2026-10-17)

A repository-hygiene round (single PR, base `9845cee1`) renamed the remaining
development-stage identifiers in the active test/build graph and removed
closed-campaign tooling. No requirement changed status, no evidence class was
rewritten, and no recorded verdict was re-dated; affected rows above are
re-worded in place. Summary:

- Renames (pure structural; same injection points, oracles and kill
  commands modulo the new names): `tests/backend_conformance_e1_test.cpp` →
  `backend_contract_conformance_test.cpp` with targets
  `backend_contract_conformance_{threadpool,uring}_test` and mutants
  `backend_contract_conformance_mut_*` (G5/G6 rows updated);
  `tests/{uring,threadpool}_core_cutover_test.cpp` →
  `{uring,threadpool}_request_core_integration_test.cpp` with mutants
  `uring_core_integration_mut_*`; stage-prefixed mutant defines re-spelled
  (`SLUICE_E1_MUTANT_*` → `SLUICE_BACKEND_CONTRACT_MUTANT_*` or
  `SLUICE_{URING_PROGRESS,URING_EXTERNAL_LOOP}_MUTANT_*`,
  `SLUICE_B1C_MUTANT_*` → `SLUICE_URING_CORE_MUTANT_*`,
  `SLUICE_C1_MUTANT_*` → `SLUICE_REQUEST_CORE_MUTANT_*`,
  `SLUICE_C2E_MUTANT_*` → `SLUICE_PROGRESS_SOURCE_MUTANT_*`,
  `SLUICE_E2_MUTANT_M*` → `SLUICE_SHUTDOWN_MUTANT_M*` with the ADR-0004
  M-numbers preserved, `SLUICE_B2_MUTANT_*` →
  `SLUICE_PUBLIC_REQUEST_MUTANT_*`, `SLUICE_D1_MUTANT_*` →
  `SLUICE_REQUEST_SCOPE_MUTANT_*`; the G9 seam allowlist followed).
  Requirement-ID prefixes with current contract meaning (`SLUICE_ERR01_*`,
  `SLUICE_THREAD01_*`, `SLUICE_LIFE_*`) and the W-01/W-02/W-03 workload
  names are unchanged.
- Formal gate: `scripts/verify_tla.sh` shrunk to the three active
  C++-mirrored models; the retired-primitive TLA+ corpus and the Lean FCB1
  corpus left the tree (G8 historical row above);
  `scripts/verify_c2e_progress.sh` → `scripts/verify_progress_source_model.sh`
  (verdict catalogue unchanged).
- Retired closed-campaign tooling: the F0/F3/F4/M/M-R/liburing verifier
  scripts, the `mr-disposition-guard` workflow (the M-R disposition overlay
  closed per `docs/review/m-phase-final-closeout.json`, Git history at the
  hygiene base), `tests/package/f4_retirement/` (the live 11-header boundary
  is owned by `scripts/verify_v2_package.py`, G9), and the dead lefthook
  pre-push configuration (its gate script was deleted in `5f62b55b`).
- Dated evidence: `docs/review/` and the campaign-only `docs/evidence/`
  records left the tree; the five evidence records cited by active rows or
  tooling remain in place unmodified.
- Verification of this round: both build profiles green, clean-room package
  verifier re-run with the renamed seam allowlist, renamed mutants re-killed;
  recorded in `docs/evidence/repo-hygiene-contraction-20261017/`.

## Dated internal-seam dispositions (2026-10-18, #528 closeout)

F6-F16 asked for explicit dispositions of two internal seams that no public
claim depends on. Both were re-derived from source at master `9f22bde6`,
not copied from the F2 record; no production code changed.

### `RequestCore::acquire_execution` — PROTOCOL_TEST_ONLY

- Production callers: none. `src/` contains only the definition
  (`src/async/detail/request_core.cpp:388`). The accepting transaction
  itself takes the first execution reference (`request_core.cpp:210`,
  inside `accept`), and the executor handoff authority is
  `claim_execution` (`src/async/threadpool_backend.cpp`,
  `src/async/uring_transport.cpp`).
- Test callers: `tests/request_core_test_driver.hpp` and the five
  `acquire_execution` sites in `tests/request_core_protocol_test.cpp`
  (positive executor-handoff obligations and negative
  terminal/zero-op refusals). It models an additional executor
  borrow-touch on the same slot, mirroring the `AcquireExec` model action
  recorded by the #524 correspondence
  (`docs/evidence/issue-524-requestcore-convergence-20261015/correspondence-after.md`).
- Exposure: none on the installed surface — `request_core.hpp` is an
  uninstalled `detail/` header. The symbol is present in the shipped
  static archive (recorded in both archive baselines) because
  `request_core.cpp` compiles into `sluice_async`; it is unreachable from
  the 11-header public surface.
- Root obligation: none. REQ-02..06 are discharged through the
  accept/claim/release paths; the v2 root imposes no public semantics on
  this entry point. Classification confirms the F2 record's
  protocol-test-surface reading (`docs/evidence/f2-authority-contraction-20261013/minimal-state-ledger.md`).

### Ready-sink routing seam

```text
READY_SINK_PRODUCTION_BINDING = none attached: no production call site
    invokes AsyncBackend::attach_ready_sink; each backend's default
    backend-owned ReferenceReadySink member receives the events
    (src/async/threadpool_backend.cpp:381, src/async/uring_backend.cpp:590
    — routing_sink_ ? *routing_sink_ : sink_)
READY_SINK_TEST_BINDING = TestAccess::context_set_ready_sink
    (include/sluice/async/detail/test_access.hpp:31, uninstalled),
    serialized against other TestAccess setters by the context access
    mutex; exercised by tests/threadpool_external_loop_test.cpp and
    tests/threadpool_progress_pass_bound_test.cpp
READY_SINK_REBIND_SUPPORTED = no production rebind path exists (no
    production caller). Unlike attach_progress_port, the routing pointer
    carries no install-once invariant and no failure reporting: tests may
    attach, replace and clear it at controlled points, and the attaching
    caller must clear the binding before the sink object is destroyed
READY_SINK_LIFETIME_OWNER = the attaching caller owns the sink object
    (the backend holds a borrowed raw pointer); the backend-owned default
    sink_ member lives with the backend
PUBLIC_API_IMPACT = NONE (all types live in uninstalled detail headers)
```

The `attach_progress_port` install-once contract is deliberately not
projected onto `attach_ready_sink`: the progress port is a
construction-time install with a failure path, while ready-sink routing
is a plain override pointer whose only writers are the test accessors.
