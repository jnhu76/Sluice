# F0 Package Profile & Compatibility Policy (#454, parent #402)

- **Status**: `PACKAGE_PROFILE_POLICY_DEFINED` + `COMPATIBILITY_DISPOSITION_DEFINED` candidate. Revision 2 resolves Round-1 findings P1-1/P1-2 (§4.1 there). Revision 3 closes the Round-2 residual P1 — the OPTIONAL_CANDIDATE public-closure rows H-30/H-31 and the clean-room packaging rule (§2); record: `f0-role-profile-census.md` §4.2.
- **Baseline**: `989d5c3fa5fa803c05ac87ebfe6c99db9d87564b`, root v1-r3 (see `f0-role-profile-census.md` §0 for provenance; row IDs below reference its §2 census)
- **Authority order**: root > adopted ADR (ADR-0004 only) > #402 phase policy > ledger/evidence > implementation. This file is a **policy definition for later phases**, not a deletion authorization; every disposition below is `CANDIDATE` until the owning ticket decides.

## Current adoption overlay — 2026-10-11

F0 policy was adopted via #455 at `a34d96c6`; its opening baseline/profile
measurements and §4 candidate table below are dated inputs, not new HEAD facts.
ADR-0003 is now ADOPTED DESIGN_AUTHORITY_ONLY via #476. Root v1-r4 and the §1.5/§1.7
negative-exclusion procedure were adopted via #488 at
`d4973100a31dccaa098cf4227f84e6c9c0afdee7`. Current terminal dispositions and
per-consumer exits are in [Phase M final adoption](m-phase-final-adoption-20261011.md):
D-09/D-10 = A3a/B1, D-11 = P2 with bounded P3 transition, D-12 = unadmitted
stream-product exclusion, D-13 = no automatic installed TEST_ONLY entitlement.
The legacy source/build/install bridge remains until each F4/F5 gate. #475's
A/B/C HOLD/KEEP policies and §1.7(2)–(5) remain effective. No supported W-04,
per-surface breakage approval or immediate contraction follows from this overlay.

## 1. Compatibility disposition (`COMPATIBILITY_DISPOSITION_DEFINED`)

### 1.1 What earns `COMPATIBILITY_ONLY`?

A surface earns `COMPATIBILITY_ONLY` when **all** of:

1. MIG-02 assigns it a compatibility/retire-after-audit disposition (`Completion<T>`, `legacy Reader/Writer/FileReader/FileWriter/IoContext`, runtime completed-return helpers, dormant public primitives, experimental headers), or it is a spelling retained only so existing callers keep compiling;
2. it has (or must be assumed to have, absent contrary evidence) consumers outside the retained test corpus;
3. no root requirement names it as the v1 surface.

`COMPATIBILITY_ONLY` is **not** earned by: zero in-tree consumers alone, age, formalization history, or presence in `include/`. Conversely, absence of a compatibility promise does not by itself authorize removal — removal requires the consumer audit + migration evidence of MIG-01 Phase F.

Classification result (row IDs from `f0-role-profile-census.md` §2):

| Class | Rows |
|---|---|
| `CANONICAL` | V-01..V-05, R-01..R-04, R-15, H-01, H-03, H-04, H-27, H-28, P-05, P-06 |
| `OPTIONAL_CANDIDATE` (optional-profile candidate; support gated on W-04 + ADR-0003 adoption) | H-09 (`StackfulIoHost`/`IoTaskContext` — the only configuration PROD-02 admits as an optional build profile; single-owner stackful adapter only) **plus its current public-closure exposure rows H-30 (`CancelToken` public-signature dependency) and H-31 (`Fiber`/`fiber_ctx` public compile/layout exposure), which inherit the H-09 gate** |
| `OPTIONAL_SUPPORTED` | — **empty today**; no shipped surface may claim this class until the H-09 gate completes |
| `COMPAT` (compatibility/dormant retained surface) | R-06 (`Completion<T>` caller-owned result), R-08 (`Batch`), H-05 (`op_helpers` completed-return + busy-spin), **H-06, H-07, H-08, H-10, H-13, H-16, H-26, H-29 (root-DEFERRED multi-worker runtime world per PROD-02 — not optional-supported)**, H-14 (`Future<T>`), H-15 (`WaitPolicy`/`EventedWaitPolicy`), H-17 (`Event`), H-18 (`select`), H-22 (5 dormant wrappers), L-01..L-09 (legacy stream world) |
| `EXPERIMENTAL` | L-12 (`experimental/uring_*`) |
| `INTERNAL` | all `detail/` substrates + backend seam + host substrate mechanisms (V-06..V-09, R-07, R-09..R-11, R-13, R-14, P-01..P-04, H-02, **H-11 `Fiber`, H-12 `fiber_ctx` as mechanisms**, H-19..H-21, H-23..H-25) |
| `UNDECIDED (F2-owned spelling)` | R-05 (`RequestHandle`: the copied-identity/state-probe capability is MIG-02-required; the spelling's disposition is **UNDECIDED** — retain \| fold into `RequestId`/context lookup \| internalize, decided by F2) |
| `TEST_ONLY` | L-10, L-11, R-12 |

### 1.2 Is compatibility installed?

**No.** No install/export/package target exists at the baseline (census §1.2; ledger row `NOT_ASSESSED`). Nothing is installed, so "installed compatibility surface" is currently an empty set. If Phase F introduces an install rule, this policy classifies per profile at that point; physical future installation must not be conflated with supported-canonical status. (The app READMEs' "installed/public headers only" wording is currently inaccurate and is recorded as a DOC_CLAIM correction — DAG X-18.)

### 1.3 Is compatibility included by canonical umbrella headers?

**No umbrella header exists.** All 81 headers are peer includes; no canonical header transitively includes a `COMPAT` surface as part of its own contract **except**:

- `async_io_context.hpp` (canonical hub) includes `completion.hpp` because its own submit/cancel/observer spellings still accept `Completion&` (compat overloads on the canonical context);
- `application_runtime.hpp`/`stackful_io_host.hpp` include the host runtime world (their own profiles);
- `request.hpp` — the **canonical** result carrier — directly includes `detail/request_core.hpp` (an INTERNAL edge, recorded in census §1.4).

Consequence: today, including canonical async headers **transitively compiles** the compatibility carrier and the detail substrate. Compatibility is not quarantined behind an opt-in include; this is a factual exposure record for F1–F4 planning, not a defect verdict.

### 1.4 Does compatibility participate in W-01/W-02/W-03/W-04?

| Workload | Compatibility participation |
|---|---|
| W-01 (ordinary utility) | Must be satisfiable **without** any compatibility surface (direct `File` + `blocking::` only). Verified as a census fact: `blocking/file.hpp` + `file_resource.hpp` depend on nothing async. |
| W-02 (bounded pipeline) | Canonical path is `RequestScope` (R-15, D1 VERIFIED) + `Request<T>`. `Batch` (R-08, COMPAT) is a parallel compatibility spelling; `RequestScope` does not use it. |
| W-03 (external loop) | Canonical path is `AsyncIoContext` + `ProgressOwner` + notification fd. No compatibility surface participates; the (removed) `BackendWaitSource`/`WaiterToken` vocabulary is `ALREADY_MIGRATED`. |
| W-04 (stackful host) | PROD-02 distinguishes two host configurations and this policy keeps them separate: the **narrow single-owner stackful adapter** (`StackfulIoHost`, H-09) is the only optional-profile *candidate* (`OPTIONAL_CANDIDATE`; support claim gated on W-04 pass + ADR-0003 adoption — ADR-0003 is PROPOSED, so nothing claims W-04 support today), while the **multi-worker task host** (`ApplicationRuntime`/`Scheduler` + `await_op_helpers`/`async/file.hpp`/`task_result`, H-06..H-08/H-10/H-26) is root-**DEFERRED** and classified `COMPAT` with `Completion`-spelled conveniences. The `Completion&` parameters inside those helpers and in `Scheduler::await_completion`/`RuntimeTaskContext::await_completion` are compatibility edges of the DEFERRED world, not optional-supported surface. |

### 1.5 What source compatibility is actually promised?

**No current v1 root/ADR establishes source spelling stability, but a pre-v1 publication DID use a deliberate-deprecation convention.** The historical v0.0.1 API reference calls selected interfaces *stable-ish* and says removal or semantic change should be deliberate across minor work, while its release notes call the version experimental/not SemVer-governed. Read **both** facts: neither creates a permanent v1 API promise; neither permits unannounced deletion of previously exposed names. Document the affected header/symbols, explicitly deliberate the deprecation and migration/no-equivalent path, and have the phase owner resolve scoped external-use risk before physical removal. GOV-02 assigns normative force only to the root; the root freezes behavior, not C++ names (names are ADR-01-delegated and ADR-0004 explicitly maps `IoContext → AsyncIoContext` "until the F-phase rename"). No document, issue, or ledger row promises that a given header, class name, or signature survives any future commit. The applicable retention and retirement obligations include MIG-01/MIG-02's **adopted replacement or qualified adopted exclusion** (only for non-required capabilities), in-tree consumer migration/re-ownership, historical deliberate-deprecation disclosure, scoped external-use risk disposition, relevant formal/docs/test evidence and installed-package/build closure under F0 §1.7(2)–(5) and #402. No permanent preservation of legacy source spelling, installation or ABI is implied.

### 1.6 Is any ABI compatibility actually promised?

**No.** No stable-ABI claim exists anywhere in the authority chain (PROD-03 explicitly excludes "stable plugin ABI" from v1 scope). Static libraries are consumed from source at the same commit; `SLUICE_HAS_LIBURING` is an ODR-critical public define precisely because no ABI stability is assumed.

Both forbidden conclusions are recorded (per #454 §8):
- ❌ "no ABI promise ⇒ break anything freely" — MIG-01 Phase F still requires consumer audit + migration evidence before retirement;
- ❌ "shipped ⇒ promised" — shipment without install rules and without a promise document is not a promise.

### 1.7 What is the exit condition for a `COMPAT` surface?

A compatibility surface becomes **retirable** when, per MIG-01 Phase F:

1. its replacement is adopted (ledger evidence, not just merged — `replacement exists ≠ replacement adopted`), **or**, only for a PROD-03 capability not admitted to required v1 scope, an explicit GOV-04-adopted MIG-02 **negative product/exclusion disposition** documents why no replacement is needed, the affected behavior and the migration/no-equivalent consequence; required W-01–W-03 capabilities cannot use this exception;
2. every in-tree consumer edge recorded in `f0-consumer-migration-dag.md` has migrated or is explicitly re-owned by a later phase;
3. the installed-header/build audit (ledger row `NOT_ASSESSED` → assessed) covers the surface;
4. no formal/docs claim depends on it as current evidence (formal map `NOT_CURRENT_CLOSURE_EVIDENCE` items excepted);
5. `UNKNOWN_EXTERNAL_USE` has been resolved by an explicit decision of the phase owner (documented **per-surface** acceptance of breakage risk, or a bounded deprecation window) after inspecting historical public descriptions, including v0.0.1 stable-ish/deliberate-deprecation text — **this decision remains a #402 owner-review obligation, not an agent default**. A global v1 minimality preference alone does not resolve any unenumerated U-family exposure.

The exception in (1) changes only the **adoption form** (approved exclusion rather than fictitious replacement) for a non-admitted capability. Conditions (2)–(5), the #402 family gates, #475's adopted A/B/C package policy and every required lifetime/settlement oracle remain in force. An adopted exclusion is `DECIDED_TO_EXCLUDE_FROM_V1`, not `SAFE_TO_UNINSTALL` or `RELEASE_READY`. Any later change of profile/install symbols triggers B-01 versioned package evidence; never rewrite historical F1 manifests.

### 1.8 Can compatibility adapters own semantic authority?

**No — single canonical authority per semantic fact.** A compatibility adapter may translate, delegate, or forward, and may hold *derived* state only; it must never become a second source of truth for result settlement, native-resource ownership, progress, or identity (root ARCH-02, ADR-02 "Caller Completion as final canonical storage"). Concrete current-fact implications:

- `Completion<T>` may remain a storage carrier for compat callers but its binding-release authority must continue to route to `RequestCore` (today it does, via `install_core_binding_for_backend`; the legacy `RequestArena` release path and the `identity_of` arena fallback are the residual exceptions recorded in R-11 / DAG X-02);
- task-world result cells (`Future<T>`, `TaskResultSlot<T>`, `Task<T>`, `Batch` cells) are **storage cells of their own worlds**, not second request-settlement authorities; they are recorded as such in census §3 and must never be promoted to request-result truth without a root amendment;
- `FileReader/FileWriter` (L-01) currently **do** define independent open/close/move/sync/vectored behavior — recorded in the census as a second-native-authority *candidate*; whether that independence has a retained consumer obligation is exactly the F1 consumer-audit question;
- no new compatibility surface may be introduced that duplicates an authority the canonical surface already owns.

## 2. Package profile policy (`PACKAGE_PROFILE_POLICY_DEFINED`)

Profiles (superset of the six required classes; `SHIPPED` facts vs `SUPPORTED` claims are deliberately separated):

| Profile | Definition |
|---|---|
| `CANONICAL` | Required v1 surface per root; target of the convergence; workload acceptance defined by W-01–W-03 + required profiles |
| `OPTIONAL_CANDIDATE` | Implemented **narrow single-owner stackful adapter** (H-09) shipped as an optional-profile candidate; its support claim is gated on W-04 pass + D2 review + ADR-0003 adoption (PROD-02). Until the gate completes it is a candidate, not a supported product profile; if the gate never completes it degrades to `EXPERIMENTAL` by PROD-02/MIG-03. **Public-closure rule:** the profile's surface set includes every header/type required to compile `stackful_io_host.hpp` today — exposure rows H-30 (`cancel.hpp`) and H-31 (`fiber.hpp`/`fiber_ctx.hpp`) — even where the underlying mechanism's target disposition is KEEP_INTERNAL: **until F3 repairs the H-09 header boundary, a clean-room OPTIONAL_CANDIDATE host package must carry all of them.** |
| `OPTIONAL_SUPPORTED` | Optional build profile **after** its gate completes. **Currently empty**: PROD-02 admits only the single-owner stackful adapter into this class and marks the multi-worker task host DEFERRED, so no shipped surface claims it today. |
| `COMPATIBILITY` | Retained pre-v1 / dormant surface per §1.1 — including the root-DEFERRED multi-worker runtime world; no promise; retirable under §1.7 |
| `EXPERIMENTAL` | Clearly isolated unsupported experiments (LIFE-03/MIG-02); must not be described as v1 conforming |
| `INTERNAL_ONLY` | `detail/` mechanism + backend seam + host substrate mechanisms (`Fiber`/`fiber_ctx` as mechanisms); consumer-visible today only via the public include path; must never be consumed directly by new external code |
| `UNDECIDED` | Capability recorded as required by MIG-02, but the current public spelling's disposition is owned by a later phase; today exactly one row: `RequestHandle` (F2) |
| `TEST_ONLY` | Public-tree test doubles/seams; exist for evidence, not product |
| `HISTORICAL_RESEARCH` | Archived authorities, research results, historical models (GOV-03; formal map dispositions) |

Per-profile obligations (current facts, not aspirations):

| Attribute | CANONICAL | OPTIONAL_CANDIDATE (H-09) | OPTIONAL_SUPPORTED | COMPATIBILITY | EXPERIMENTAL | INTERNAL_ONLY | UNDECIDED (R-05) | TEST_ONLY | HISTORICAL_RESEARCH |
|---|---|---|---|---|---|---|---|---|---|
| INSTALLED (today) | NO | NO | — (empty) | NO | NO | NO | NO | NO | NO |
| PUBLIC_HEADERS (today) | yes (include path) | yes | — | yes | yes | yes (detail/, de-facto transitive) | yes | yes | docs/formal trees |
| TARGET_LINKABLE (today) | `sluice_core`+`sluice_async` | same + host tests | — | via the same libs | **no target at all** (orphan TUs, FA-2) | inside the two libs | inside the two libs | inside the two libs | not build products |
| CANONICAL_DOCS | root + README target sections | ADR-0003 (**PROPOSED**) + D2 ledger row (`IMPLEMENTED_UNVERIFIED`) | — | MIG-02 disposition rows only | LIFE-03/MIG-02 constraints | ARCH/BACKEND/REQ/OBS/PROG rules | MIG-02 capability requirement + #402 F2 scope note | none | supersession banners |
| WORKLOAD_ACCEPTANCE | W-01–W-03 (+V01–V27 where evidenced) | W-04 gate only — **not claimable today** | W-04 pass required (class currently empty) | none required; must not regress canonical evidence | none | exercised through canonical/host tests | none (not a workload surface) | test-only | none |
| REQUIRED_EVIDENCE | ledger VERIFIED per slice | W-04 tracer + D2 review + ADR-0003 adoption **before any supported claim** (PROD-02) | the completed H-09 gate record | consumer audit + migration evidence before any change | root amendment before any supported claim | follows the owning slice | explicit F2 disposition record (retain \| fold \| internalize) | mutation/oracle records | none (retained for provenance) |

The load-bearing asymmetries: **`physically installed ≠ supported canonical`** (currently nothing is installed, so the discriminating case is future), **`not installed ≠ internal by definition`** (today everything public is merely on an include path; `EXPERIMENTAL` headers are neither installed nor internal), and **`target disposition ≠ current package closure`** — a mechanism whose destination is INTERNAL (H-11 `Fiber`, H-12 `fiber_ctx`) or whose vocabulary serves the DEFERRED runtime (H-16 `CancelToken`) is still part of the OPTIONAL_CANDIDATE package closure today (H-30/H-31) until the F3 boundary work lands.

## 3. External-use uncertainty register

There is no repository compatibility/API policy document (GREP of docs/, ADRs, README: none found). Therefore, per #454 §8:

| Family | IN_TREE_CONSUMERS | UNKNOWN_EXTERNAL_USE |
|---|---|---|
| CANONICAL surfaces | recorded in census §2 | `unknown` (no policy ⇒ cannot assert zero external use, but canonical surfaces are not removal candidates anyway) |
| `Completion<T>` + `Batch` + `op_helpers` | 15 test files include `completion.hpp`, **25 test files name the type**; **4 apps construct/settle `Completion` in task code** (copy/hash/grep/tail via `await_op_helpers`); `Batch`: 1 test; `op_helpers`: 1 test | `unknown` — and the app usage means Completion retirement **cannot** be scoped as a test-only migration (DAG X-01) |
| Host/runtime family — root-DEFERRED multi-worker runtime (H-06..H-08, H-10, H-13, H-16, H-26, H-29) | 4 apps + host tests + scheduler TUs | `unknown` |
| Optional-profile candidate host + its public-closure exposure rows (H-09, H-30, H-31) | src 1 + host tests 1 (compile `stackful_io_host.hpp`, which pulls `cancel.hpp`/`fiber.hpp`/`fiber_ctx.hpp`) | `unknown` |
| Concrete backends (H-27/H-28) | apps construct `ThreadPoolBackend` directly (6 TUs); uring backend construction gated on availability | `unknown` for direct construction |
| Legacy stream world (L-01..L-09) | src-core factories + tests; 0 apps | `unknown` |
| Dormant wrappers (H-22) + `Future`/`WaitPolicy`/`Event`/`select` | 0 direct includes (condition/semaphore/async_queue), 1 (async_mutex via condition), **10 src TUs (async_rwlock, mechanical)** | `unknown` |
| Experimental (L-12) | 0 (orphan TUs) | `unknown` |
| Test doubles (L-10/L-11) | tests only | assumed none (test-shaped API), still recorded |

No row may treat `IN_TREE_CONSUMERS = 0` as "no consumers"; all removal decisions route through §1.7.

## 4. Candidate disposition table (`CANDIDATE DISPOSITION ONLY`)

Nothing here authorizes deletion; `FINAL_DECISION_OWNER` is #402 (with #454 as executor) in every row. Rollback unit = the dedicated retirement PR (one family per PR).

| ID | Surface (census row) | Candidate | PROFILE | SUPPORTED_OR_ONLY_PHYSICALLY_REACHABLE | REPLACEMENT | CONSUMER_STATE | COMPAT_EXIT_CONDITION (§1.7) | CONFIDENCE | EVIDENCE |
|---|---|---|---|---|---|---|---|---|---|
| D-01 | `Completion<T>` caller-owned result (R-06) | REMOVE after migration | COMPAT | physically reachable + used by apps | `Request<T>` (B2 record) | 15 direct-include test files; 25 test files name the type; **4 apps** (CALL+TYPE_LAYOUT); `Scheduler::await_completion`/`RuntimeTaskContext::await_completion`/`cancel_waiter`; both backends' publish path; `op_helpers`/`await_op_helpers`/`async/file.hpp`/`Batch` | migrate apps + tests + re-type the await/cancel machinery (DAG X-01/X-16) + drop `Completion&` overloads + backend publish path | HIGH | census R-06; DAG X-01/X-16; MIG-02 |
| D-02 | `RequestArena` substrate (R-11) | REMOVE after D-01 | INTERNAL | compile-time-reachable via Completion release path + identity fallback + seams; **production-unarmed** (no arena instance in src) | `RequestCore` (B1 records) | completion.hpp, request_handle.cpp (`identity_of` fallback), submit_transaction.hpp, 2 test probes | re-own the `identity_of` fallback (or prove dead once core bindings are universal) + reroute release path | HIGH | R-11; DAG X-02/X-09(C-9) |
| D-03 | `submit_transaction` (R-12) | REMOVE with D-02 | TEST_ONLY | seam-only | core-side acceptance (B1) | seam include only | — | HIGH | R-12 |
| D-04 | `Batch` (R-08) | REMOVE or COMPAT-port | COMPAT | physically reachable | `RequestScope` (D1 VERIFIED) | 1 test | migrate the single test; decide scope ownership | MEDIUM | R-08 |
| D-05 | dormant wrappers: `AsyncCondition`, `Semaphore`, `AsyncRwLock`, `AsyncQueue<T>` (H-22) | REMOVE | COMPAT (wrapper-dormant) | classes uncalled by any caller | `Scheduler::` internals remain as substrate (HOST-01 permits internal retention) | condition/semaphore/async_queue: 0 includes; **async_rwlock: 10 scheduler TUs include the header** (out-of-line methods live there) — removal cost is 10 TU edits; async_mutex: 1 header include | §1.7 external-use decision + the 10-TU mechanical migration for AsyncRwLock | MEDIUM (external use unknown) | census H-22; DAG review B-2 |
| D-06 | `AsyncMutex` wrapper (H-22) | KEEP_INTERNAL (demote) | COMPAT | reachable via condition.hpp only | scheduler internals | hdr 1 | same as D-05; **must precede F3** (includes `fiber.hpp`, holds `Fiber*` — DAG X-17) | MEDIUM | census H-22 |
| D-07 | `Future`/`WaitPolicy`/`EventedWaitPolicy` (H-14/H-15) | KEEP_INTERNAL while Group retained; REMOVE with Group retirement | COMPAT | reachable via Group only | none needed (Group consumes) | hdr:1 (group.hpp) | Group disposition decision | MEDIUM | census H-13/H-14 |
| D-08 | `Event`/`select` wrappers (H-17/H-18) | KEEP_INTERNAL while Scheduler retained | COMPAT | live scheduler machinery, wrapper consumed in-tree | none needed | src-async 10-12 | Scheduler disposition decision | MEDIUM | census H-17/H-18 |
| D-09 | `FileReader`/`FileWriter` (L-01) | RETIRE-FROM-CANONICAL candidate; two decisions first | COMPAT | physically reachable; no apps | canonical `File` + `blocking::` — **expressiveness gap: no vectored (readv/writev) op exists canonically; L-01 holds the repo's only native `read_vec/write_vec` and the only native `Writer::write_all_vec` implementation** (wal `write_record_vec` depends on it) | `IoContext` factory (only production caller) + 6 test files, 4 of which are named ledger oracles (semantic_range_test/B1 records/shutdown_lifecycle_test) whose migration requires ledger re-recording | (1) second-authority question (X-06); (2) vectored-IO capability decision (root amendment or accepted degradation); (3) evidence re-record for oracle tests | MEDIUM | census L-01/L-06; DAG X-06/X-07/X-10; review A-6/C-10 |
| D-10 | `IoContext`/`BlockingIoContext` factory (L-04) | REMOVE with D-09; rename collision recorded | COMPAT | physically reachable | canonical `File::open` + `blocking::` | src 1 + tests via L-01 | with D-09 | MEDIUM | census L-04 |
| D-11 | `BlockingIoPool` (L-05) | CANDIDATE UNDECIDED | COMPAT | physically reachable | ThreadPoolBackend (canonical request world) | src 1 + tests | workload/capability audit first | LOW | census L-05 |
| D-12 | `wal`/`copy`/`buffer`/`observed`/`measurement`/`limit`/`iovec`/`copy_strategy`/`sync` (L-06..L-09) | CANDIDATE UNDECIDED (no v1 workload admits them) | COMPAT | physically reachable | canonical File ops for the file cases | src + tests only | root amendment or retirement per audit; D-09's vectored decision binds `wal::write_record_vec` | LOW | PROD-03, MIG-02 |
| D-13 | `fault.hpp`/`memory_io_context.hpp` (L-10/L-11) | KEEP (relocate or INTERNALIZE candidate) | TEST_ONLY | test-reachable only | — | tests; L-11 has 0 includes | n/a (not product) | HIGH | FA-5 |
| D-14 | experimental uring write surfaces (L-12) | REMOVE or ISOLATE (decide at install-rule time) | EXPERIMENTAL | not even compiled | canonical uring backend | 0 | LIFE-03 requires root amendment for any support | MEDIUM | FA-2, LIFE-03 |
| D-15 | `op_helpers` completed-return helpers (H-05) | RETIRE after re-typing; **carries the live MIG-02 busy-poll obligation**: `one_step` spin loop (`while (!c.ready()) (void)ctx.poll();`, `op_helpers.cpp:18-24,84-90`) | COMPAT | physically reachable | `Request<T>` + `RequestScope` | src 1 + tst 1 | replacement must (a) settle on every exit and (b) eliminate the busy-spin in favor of the progress/wait contract | HIGH | census H-05; DAG review C-7 |
| D-16 | `StackfulIoHost` fiber exposure (H-09 boundary) | KEEP_PUBLIC candidate, **fix header boundary first** (F3 prerequisite); support claim gated on W-04 + ADR-0003 adoption (PROD-02) | OPTIONAL_CANDIDATE | — | n/a (it is the replacement) | src 1 + tst 1 | n/a (boundary fix is a prerequisite of F3, not of retention) | HIGH | census §1.4 |
| D-17 | `RequestHandle` (R-05) | **UNDECIDED — owned by F2** per #402 (retain \| fold into `RequestId`/context lookup \| internalize); F0 records capability and dependencies only | UNDECIDED (spelling); the copied-identity/state-probe capability is MIG-02-required | implemented (RECORD); adoption state pending ledger | the capability is already carried by `RequestId` + core lookup (R-02/R-09) — F2 evaluates whether the spelling adds state-probe value | src + tst | F2 disposition record + `identity_of` arena fallback re-ownership (D-02 precondition) | HIGH (facts) | census R-05; DAG review C-5 |

No `COMPAT`-family row above is KEEP_PUBLIC. The only retention-flavored candidates are the OPTIONAL_CANDIDATE host boundary (D-16) and the explicitly UNDECIDED `RequestHandle` spelling (D-17, F2-owned); retention of CANONICAL/OPTIONAL_CANDIDATE rows needs no entry here — it follows from the census §3 ID sets.
