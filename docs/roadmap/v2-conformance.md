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
| Overall conformance status | **PARTIAL.** The retained execution paths (direct, ThreadPool, io_uring, scope, observer, progress, shutdown) are implemented with scoped verified evidence at recorded commits; the public-boundary contraction (owner decisions D1–D3) and release validation are open |

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
| SEM-01..04, ERR-01 (shared oracle, open/close, precedence, offsets) | `detail/file_semantics.hpp`, `file_resource.*`, `blocking/file.hpp`, `src/blocking_file.cpp` | Shared oracle serves direct and both request paths; 0 recorded divergences | Root §6/§7 | VERIFIED (carried) | v1 campaign semantic suites at `dd83595d`/`b870a7c0` (Git history `c001bb1e`); in-tree `semantic_*` suite; request-path errno re-diff `request_error_domain_test` (#531 matrix liburing=y 68/68, liburing=n 53/53) | Header `detail/file_semantics.hpp` still installed until PR B (G9) | PR B (install only) |
| SEM-05 short I/O / EOF / composition | `blocking/file.hpp` (read_exact/write_all), request primitives | Direct compositions + short-count rules on both backends | Root §6 SEM-05 | VERIFIED (carried) | A2 record (v1 ledger, Git history); `semantic_short_io_reference_test` in tree | — | — |
| SEM-06 durability | blocking + request sync paths | sync_data/sync_all incl. resize coverage (V27) on direct and request paths | Root §6 SEM-06 | VERIFIED (carried) | F6 requirements matrix (`docs/evidence/f6-final-correspondence-20261014/`); `semantic_durability_reference_test` in tree | Durability claims rest on documented OS/filesystem guarantees; release matrix enumeration outstanding (G10) | Release gate |
| SEM-07 metadata/identity | `file_resource.*`, request `file_info`/`size` | kind/size/identity incl. request-path statx ops | Root §6 SEM-07 | VERIFIED (carried) | A2 + E1 records; `semantic_reference_case_test`, e1 uring suite | — | — |
| ERR-02 effects | effect/measurement vocabulary, backends | confirmed-bytes vs unknown-effects reporting incl. cancel races (V14/V15) | Root §7 ERR-02 | VERIFIED (carried) | E1 records; #526/#531; `semantic_effect_outcome_test` in tree | — | — |
| W-01 direct utility + core-only build (V25) | `sluice_core` | Direct W-01 works with no context/worker/request table; core-only consumer builds | Root §3 W-01 | VERIFIED (carried) | A2 record; `direct_w01_consumer_probe` + F1 core-only clean-room builds | — | — |

### G2 Request acceptance, identity, publication and reclaim

| Requirement IDs | C++ owner | Current state | Target | Status | Evidence (SHA/config) | Missing proof or gap | Next owner |
|---|---|---|---|---|---|---|---|
| REQ-02..06, HANDLE-01..03 (acceptance transaction, lifecycle, publication, reclaim, settlement liveness, handles) | `src/async/detail/request_core.*`, `include/sluice/async/request.hpp` | Context-owned core on both backends; public `Request<T>`; occupancy() is the settlement authority; derived `outstanding_summary()` non-authoritative | Root §8/§9 | VERIFIED (carried) | B1-B/C/D + B2 records (v1 ledger, Git history); `RequestCore.tla` safety+liveness PASS re-run at `64e3241b`, re-proved post-#524 convergence at frozen `28f0fe33` (`docs/evidence/issue-524-requestcore-convergence-20261015/`); `public_request_test`, `request_core_protocol_test` in tree | Model coverage is bounded to the modeled identity domain; see REQ-01 row | — |
| REQ-01 identity domain / exhaustion (V24 boundary) | `request_core.*`, backends | io_uring enforces identity-domain/stale-cookie discipline; core and ThreadPool admission paths do not enforce the domain exhaustively | REQ-01: no aliasing within valid domain; explicit failure before wrap | **GAP** | v2-c4 §23.1 standing item (Git history `c001bb1e`); ProgressSource model covers its own token-exhaustion boundary only | Core/TP identity-domain enforcement or a proved equivalence + exhaustion evidence | Implementation slice (post-PR-B planning) |

### G3 RequestScope, bounds, settlement, W-02

| Requirement IDs | C++ owner | Current state | Target | Status | Evidence (SHA/config) | Missing proof or gap | Next owner |
|---|---|---|---|---|---|---|---|
| §16 scope obligations, W-02, V18/V19 | `include/sluice/async/request_scope.hpp` | Owned-driver bounded scope: reserve-before-acceptance, settle on normal/early/exception exits | Root §16 | VERIFIED (carried) | D1 record #398 (six killed mutants, V18-A/B/C + V19 both backends, human final review APPROVE) | — | — |
| BOUND-01/02 budgets incl. resource-bounded progress storage | core + backends; entry-snapshot stores merged in `dd83595d` | Capacities validated at setup; pass accounting storage resource-bounded; admission obeys serialized contract | Root §18 | VERIFIED (carried) | G1 A/B + PR #536 oracles at `dd83595d` (negative pre-fix failures recorded in PR #536); V28 discriminator live 2026-10-16 (below) | — | — |

### G4 Observer and no-lost-wake progress

| Requirement IDs | C++ owner | Current state | Target | Status | Evidence (SHA/config) | Missing proof or gap | Next owner |
|---|---|---|---|---|---|---|---|
| OBS-01..03 (attach/deliver/retire) | core observer phases, `detail/observer_protocol.hpp` | Generation-armed at-most-once delivery; bounded nonthrowing hooks | Root §10 | VERIFIED (carried) | C1-A..C1-H records; `ObserverCore.tla` safety floor + conditional liveness, five C++ mutation seams killed | Publication happens-before visibility argued by core-mutex serialization, not formal proof (C1-B) | — |
| PROG-02 no-lost-wake (ThreadPool) | `detail/progress_source.hpp`, `threadpool_backend.cpp` | Epoch/token park-wake handshake; suppression bit only while pending wake stands; 14-row settlement-notification matrix complete after #480/#481/#484 | Root §11 PROG-02 | VERIFIED (carried) | C2 campaign records; #481 (merge `31dc948e`), #484 (merge `51eaa8b7`) with named oracles and killed mutants; TP evidence does not extend to other backends | io_uring notification oracle: see G6 | — |
| PROG-01 pass bound + honest residue (V28/V29) | `threadpool_backend.cpp`, `uring_backend.cpp` | TP entry-snapshot bounded pass; uring bounded phases + owed-delivery residue report (PR #543) | Root §11 PROG-01 | VERIFIED (live 2026-10-16) | `xmake run threadpool_progress_pass_bound_test` PASS (exit 0); `xmake run uring_owed_residue_test` 3/3 PASS — this host, xmake debug, Linux 7.2.9 fc44, liburing present, tree `c001bb1e`; merged oracles at `dd83595d`/`b870a7c0` | — | — |

### G5 ThreadPool execution

| Requirement IDs | C++ owner | Current state | Target | Status | Evidence (SHA/config) | Missing proof or gap | Next owner |
|---|---|---|---|---|---|---|---|
| BACKEND-02/03 ThreadPool profile (six request ops, explicit setup failure, fault paths) | `src/async/threadpool_backend.cpp` | All six ops incl. metadata on workers; construction throws rather than degrading; cancel dispositions incl. `physical_interruption_unsupported` | Root §15 | VERIFIED (carried) | E1 records + F6 matrix; xmake targets `backend_conformance_e1_threadpool_test`/`_uring_test` from `tests/backend_conformance_e1_test.cpp` + killed `e1_conformance_mut_*` builds; scoped to recorded commits/configs | Release-matrix rerun (G10) | Release gate |

### G6 io_uring execution

| Requirement IDs | C++ owner | Current state | Target | Status | Evidence (SHA/config) | Missing proof or gap | Next owner |
|---|---|---|---|---|---|---|---|
| BACKEND-02/03 io_uring profile (statx metadata, sticky cancel intent, poison convergence, availability gate) | `src/async/uring_backend.cpp`, `detail/uring_submit.hpp` | Required profile incl. statx probe gate; `-ECANCELED` terminal with unknown remainder; transport/completion split | Root §15 | VERIFIED (carried) | B1-C/E1/C2-C records; xmake target `backend_conformance_e1_uring_test` from `tests/backend_conformance_e1_test.cpp` (15 tests, real kernel WSL2 6.18.40.1, liburing 2.14, gcc 15.2.0, tmpfs) | Environment divergence: recorded real-kernel evidence is WSL2; release matrix must cover the declared Linux configurations (G10) | Release gate |
| PROG-02 io_uring settlement notification | `uring_backend.cpp` + ProgressSource | Structural argument only: no deterministic io_uring settlement-notification oracle | Root §11 PROG-02 for both backends | **GAP** | Standing item recorded at `b870a7c0` (v2-c4 §23.1, Git history) | Deterministic uring notification/no-lost-wake oracle or a reviewed equivalence argument | Implementation slice |
| PROG-01 uring owed-delivery residue (V29) | `uring_backend.cpp` | Owed-delivery events reported as immediate residue; fixed by PR #543 | Root §11 PROG-01 | VERIFIED (live 2026-10-16) | `tests/uring_owed_residue_test.cpp` merged at `b870a7c0`; live 3/3 PASS this host (config above) | — | — |

### G7 Shutdown, cancellation and destruction

| Requirement IDs | C++ owner | Current state | Target | Status | Evidence (SHA/config) | Missing proof or gap | Next owner |
|---|---|---|---|---|---|---|---|
| CANCEL-01/02 operation cancel + deadlines | core + backends | Dispositions `won_before_execution`/`requested`/`already_terminal`/`not_found` + `physical_interruption_unsupported`; raced success never rewritten (V14); unknown effects (V15) | Root §14 | VERIFIED (carried) | E1 records; e1 suites both backends; `RequestCore.tla` `InvSingleWinner`/`InvCancelWinRequiresUnclaimed` | — | — |
| SHUT-01..04 lifecycle, V20/V21/V22/V23/V26 | `src/async/async_io_context.cpp` (ADR-0004 spelling) | Five-state settlement driver; idempotent shutdown; owner-checked; ExecutionClosed ≠ Destroyable; notification retirement before fd close | Root §17 | VERIFIED (carried + live 2026-10-16) | E2/PR #453 merge `eb7bf055`; `shutdown_lifecycle_test` 9 TP / 17 uring scenarios; `ShutdownCore.tla`; 171-gate TLA run attempt 2 `VERIFY_TLA: PASS`; live run PASS this host (config above) | — | — |
| LeakSanitizer finding, poisoned-shutdown path (io_uring) | `tests/shutdown_lifecycle_test.cpp` scenario `poison_with_in_flight_cqe` | Known test-side finding on the uring poisoned-shutdown path; classification rests on #536 commit records; not fixed | No leak on any shutdown path at release | **GAP — release blocker** | Reproduced at `dd83595d` (v2-c3/c4 records, Git history `c001bb1e`); scenario exists in tree at the assessed commit | Fix or formally approved exception before the first v2 release; currently neither exists | Release gate |

### G8 Formal models, mutations and C++ correspondence

VERIFY-06 is the normative scope table. Distinguished below: model safety,
model liveness, mutation sensitivity, C++ transition mapping, production
execution evidence.

| Requirement IDs | C++ owner | Current state | Status | Evidence (SHA/config) | Missing proof or gap | Next owner |
|---|---|---|---|---|---|---|
| RequestCore model (REQ/VERIFY-02 S1–S3, S6; L1/L5) | `src/async/detail/request_core.cpp` | Safety: 20 invariants incl. `InvBindImpliesPublished`; liveness L1+L5 per identity; #524 convergence removed the retired `ReleaseBind` flavor and migrated mutation gates | VERIFIED (carried, model) | `formal/tla/RequestCore.tla`; safety PASS 31,289,609 gen / 5,359,732 distinct / depth 53; liveness PASS (12 branches = 2 slots × generations 0..2 × {L1,L5}); frozen `28f0fe33` (`docs/evidence/issue-524-requestcore-convergence-20261015/`) | Bounded instances (2 slots, gens 0..2); not C++ memory ordering; no unbounded identity domain | — |
| ObserverCore model (OBS, L3) | core observer phases | Safety floor (at-most-once per generation, cancel isolation, retirement finality) + conditional liveness; 5 mirrored mutation seams killed | VERIFIED (carried, model) | `formal/tla/ObserverCore.tla` (C1-H, #433); cfgs with `PROPERTY` liveness + liveness mutants | Host callback internals and kernel timing unmodeled | — |
| ProgressSource model (PROG-02, V24) | `detail/progress_source.hpp` | Epoch/token park-wake handshake; reachable-state obligations re-proved on corrected close semantics; `MutRepeatCloseSignals` discriminator | VERIFIED (carried, model) | `formal/tla/ProgressSource.tla` (C2-E campaign + re-closure records) | Model predates the C1 suppression mechanism: no suppression action exists in the model; the suppression rule's coverage is the PROG-02 equivalence argument, not a model action; single-boolean readiness | — |
| ShutdownCore model (SHUT, L4) | `async_io_context.cpp` | Five-state lifecycle, `InvCanceledRequiresTargeting`, LiveOpsSettle/LiveShutdownCloses under explicit fairness; per-property negative controls | VERIFIED (carried, model) | `formal/tla/ShutdownCore.tla` (E2); part of the 171-gate run `VERIFY_TLA: PASS` | Host teardown/application consumption timing unmodeled | — |
| Mutation mirrors / correspondence (VERIFY-01/06) | test seams `SLUICE_*_MUTANT_*` | Compile-time protocol mutations killed by named invariants/oracles; in-tree C++ mutation builds recorded across B2/E1/E2/#531/#532 | VERIFIED (carried, campaign-scoped) | 71/72 campaign record (survivor: documented F2 path-dependent case, guard-caught — recorded on PR #536, not in tree); per-slice kills named in carried records | Full campaign is campaign-scale, not per-commit; re-run obligations follow VERIFY-06 on model/transition change | — |
| Historical Lean/FCB1 corpus (`formal/Sluice/**`, Event/Mutex/… TLA+) | retired primitives | Proves retired synchronization-primitive family; zero file-I/O/liveness content | DEFERRED_OUT_OF_SCOPE | Superseded v1 formal evidence map dispositions (Git history `c001bb1e`) | None required; no v2 claim rests on it | — |

No model result is a production execution claim: C++ transition mapping is
recorded per campaign (correspondence headers and the #524
correspondence-before record), and production evidence lives in G1–G7 rows.

### G9 Public API and installation boundary (owner decisions D1–D3)

| Requirement IDs | C++ owner | Current state | Target | Status | Evidence (SHA/config) | Missing proof or gap | Next owner |
|---|---|---|---|---|---|---|---|
| Appendix B install surface (D2) | `xmake/libraries.lua` | 25 headers installed (incl. 4 host/cancel, 7 seam/detail, 3 core detail) | 11 installed public headers | **GAP (contraction pending)** | Install rules at `c001bb1e` (verified against Appendix B rows) | Physical 25→11 contraction + installed-header/build audit | PR B |
| Host bundle removal (D1, §16) | `src/async/stackful_io_host.cpp` etc., 4 public headers, host tests | Bundle still present in tree (uncontracted residue pending deletion) | Physically deleted, core-only build unaffected | **GAP (removal pending)** | Tree at `c001bb1e`; consumers = host tests/seams (D2 record, F6 negative package proof) | PR B deletes code + consumers and records the audit | PR B |
| Internal-type leakage / `AsyncBackend` seam (D2, BACKEND-01) | `include/sluice/async/async_io_context.hpp` | `AsyncBackend` public abstract class (line 63 area); context ctor takes caller-constructed `unique_ptr<AsyncBackend>`; `detail::` types appear in installed headers | No public extension protocol; selection explicit without constructing internal backend objects or including detail headers; factory spelling unfrozen pending implementation review | **GAP (seam contraction pending)** | Headers at `c001bb1e` | Seam-type refactor in PR B; `AsyncBackend` not installable after | PR B |
| Installed-consumer compile/link under target surface (VERIFY-01 build row) | package rules | Clean-room consumer builds exist only for the 25-header era at recorded commits | Standalone consumer compile/link incl. core-only on the contracted surface | NOT_ASSESSED (surface does not exist yet) | F1 records (v1 ledger, Git history; 79-header era) + F5 25-header captures | Re-establish after PR B on the 11-header surface | PR B |

### G10 External event loop W-03 and release acceptance

| Requirement IDs | C++ owner | Current state | Target | Status | Evidence (SHA/config) | Missing proof or gap | Next owner |
|---|---|---|---|---|---|---|---|
| W-03 pollable progress source both backends (PROG-03) | `detail/progress_source.hpp`, backends | eventfd-based pollable handle per backend; fd never a second drain authority; external `poll(2)` integration exercised | Root §11 PROG-03 | VERIFIED (carried) | C2-B/C2-C/C2-D records (owner outcomes, sticky control, external-host lifetime, detach acknowledgement) | External-host example runs on both backends are release-evidence items (G10 release matrix) | Release gate |
| Release acceptance: W-01/W-02/W-03 + both backends, PROD-02 matrix | whole library | Not yet executed against v2 | Release evidence enumerating compiler/arch/kernel/liburing/filesystem; W-01–W-03 pass on ThreadPool and a successfully constructed io_uring backend | **NOT_ASSESSED (release gate)** | — | Full matrix at release head; also G7 LSan disposition and D3 compatibility disclosure | Release gate |

## Release Gate

Tied to v2 requirements, not to v1 phase completion. All gates are blocking
for the first v2 release:

| Gate | Requirement | Condition |
|---|---|---|
| RG1 | G7 LSan row | The LeakSanitizer finding on the uring poisoned-shutdown path is fixed with a regression oracle, or granted a formal owner-approved exception with recorded reasoning |
| RG2 | D1/D2 (root §23.4) | PR B merged: host bundle deleted; install surface contracted 25→11; installed-header/build audit recorded |
| RG3 | VERIFY-01 build row | Standalone consumer compile/link (core-only and async) passes on the contracted 11-header surface |
| RG4 | REQ-01 | Request identity-domain enforcement or proved equivalence, with exhaustion evidence, recorded in this ledger |
| RG5 | PROG-02 | io_uring settlement-notification evidence beyond the structural argument (deterministic oracle or reviewed equivalence) |
| RG6 | PROD-02 | Release evidence enumerates tested compiler/architecture/kernel/liburing/filesystem configurations; W-01/W-02/W-03 pass on ThreadPool and io_uring in that matrix |
| RG7 | D3 | Release notes disclose removed/internalized headers, the closed extension seam, and the absence of compatibility shims |

## Provenance of carried evidence

Rows marked `carried` rest on v1-campaign records preserved in Git history at
`c001bb1e` (`docs/roadmap/v1-conformance.md` and `docs/evidence/**` of that
commit) plus the correspondence check recorded in the header: the assessed
tree's C++ is identical to `b870a7c0`, the baseline those records conclude
on. They establish their stated properties at their stated commits and
configurations — nothing broader. A carried claim is re-based (re-run or
correspondence-reargued) when the code it names changes; PR B will
invalidate every row whose owner it touches.
