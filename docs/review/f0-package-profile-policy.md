# F0 Package Profile & Compatibility Policy (#454, parent #402)

- **Status**: `PACKAGE_PROFILE_POLICY_DEFINED` + `COMPATIBILITY_DISPOSITION_DEFINED` candidate
- **Baseline**: `989d5c3fa5fa803c05ac87ebfe6c99db9d87564b`, root v1-r3 (see `f0-role-profile-census.md` §0 for provenance; row IDs below reference its §2 census)
- **Authority order**: root > adopted ADR (ADR-0004 only) > #402 phase policy > ledger/evidence > implementation. This file is a **policy definition for later phases**, not a deletion authorization; every disposition below is `CANDIDATE` until the owning ticket decides.

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
| `CANONICAL` | V-01..V-05, R-01..R-05, R-15, H-01, H-03, H-04, H-27, H-28, P-05, P-06 |
| `OPT_SUPPORTED` (shipped optional profile; W-04/ADR-0003 gate) | H-06..H-13, H-16, H-26 |
| `COMPAT` (compatibility/dormant retained surface) | R-06 (`Completion<T>` caller-owned result), R-08 (`Batch`), H-05 (`op_helpers` completed-return + busy-spin), H-14 (`Future<T>`), H-15 (`WaitPolicy`/`EventedWaitPolicy`), H-17 (`Event`), H-18 (`select`), H-22 (5 dormant wrappers), L-01..L-09 (legacy stream world) |
| `EXPERIMENTAL` | L-12 (`experimental/uring_*`) |
| `INTERNAL` | all `detail/` substrates + backend seam + runtime mechanism (V-06..V-09, R-07, R-09..R-11, R-13, R-14, P-01..P-04, H-02, H-19..H-21, H-23..H-25) |
| `TEST_ONLY` | L-10, L-11, R-12 |

### 1.2 Is compatibility installed?

**No.** No install/export/package target exists at the baseline (census §1.2; ledger row `NOT_ASSESSED`). Nothing is installed, so "installed compatibility surface" is currently an empty set. If Phase F introduces an install rule, this policy classifies per profile at that point; physical future installation must not be conflated with supported-canonical status. (The app READMEs' "installed/public headers only" wording is currently inaccurate and is recorded as a DOC_CLAIM correction — DAG X-18.)

### 1.3 Is compatibility included by canonical umbrella headers?

**No umbrella header exists.** All 81 headers are peer includes; no canonical header transitively includes a `COMPAT` surface as part of its own contract **except**:

- `async_io_context.hpp` (canonical hub) includes `completion.hpp` because its own submit/cancel/observer spellings still accept `Completion&` (compat overloads on the canonical context);
- `application_runtime.hpp`/`stackful_io_host.hpp` include the host runtime world (their own profile);
- `request.hpp` — the **canonical** result carrier — directly includes `detail/request_core.hpp` (an INTERNAL edge, recorded in census §1.4).

Consequence: today, including canonical async headers **transitively compiles** the compatibility carrier and the detail substrate. Compatibility is not quarantined behind an opt-in include; this is a factual exposure record for F1–F4 planning, not a defect verdict.

### 1.4 Does compatibility participate in W-01/W-02/W-03/W-04?

| Workload | Compatibility participation |
|---|---|
| W-01 (ordinary utility) | Must be satisfiable **without** any compatibility surface (direct `File` + `blocking::` only). Verified as a census fact: `blocking/file.hpp` + `file_resource.hpp` depend on nothing async. |
| W-02 (bounded pipeline) | Canonical path is `RequestScope` (R-15, D1 VERIFIED) + `Request<T>`. `Batch` (R-08, COMPAT) is a parallel compatibility spelling; `RequestScope` does not use it. |
| W-03 (external loop) | Canonical path is `AsyncIoContext` + `ProgressOwner` + notification fd. No compatibility surface participates; the (removed) `BackendWaitSource`/`WaiterToken` vocabulary is `ALREADY_MIGRATED`. |
| W-04 (stackful host) | Shipped in two spellings: `StackfulIoHost` (narrow, ADR-0003 PROPOSED) and `ApplicationRuntime` + `await_op_helpers`/`async/file.hpp`/`task_result` (multi-worker runtime + `Completion`-based conveniences = compatibility-spelled host conveniences). The `Completion&` parameters inside otherwise-OPT_SUPPORTED helpers (H-06/H-07) and in `Scheduler::await_completion`/`RuntimeTaskContext::await_completion` (H-08/H-10) are compatibility edges riding on the optional-supported profile. |

### 1.5 What source compatibility is actually promised?

**None is promised by any current authority.** GOV-02 assigns normative force only to the root; the root freezes behavior, not C++ names (names are ADR-01-delegated and ADR-0004 explicitly maps `IoContext → AsyncIoContext` "until the F-phase rename"). No document, issue, or ledger row promises that a given header, class name, or signature survives any future commit. The only retention obligations in force are MIG-01/MIG-02 process obligations: replacements and consumer migration precede retirement.

### 1.6 Is any ABI compatibility actually promised?

**No.** No stable-ABI claim exists anywhere in the authority chain (PROD-03 explicitly excludes "stable plugin ABI" from v1 scope). Static libraries are consumed from source at the same commit; `SLUICE_HAS_LIBURING` is an ODR-critical public define precisely because no ABI stability is assumed.

Both forbidden conclusions are recorded (per #454 §8):
- ❌ "no ABI promise ⇒ break anything freely" — MIG-01 Phase F still requires consumer audit + migration evidence before retirement;
- ❌ "shipped ⇒ promised" — shipment without install rules and without a promise document is not a promise.

### 1.7 What is the exit condition for a `COMPAT` surface?

A compatibility surface becomes **retirable** when, per MIG-01 Phase F:

1. its replacement is adopted (ledger evidence, not just merged — `replacement exists ≠ replacement adopted`);
2. every in-tree consumer edge recorded in `f0-consumer-migration-dag.md` has migrated or is explicitly re-owned by a later phase;
3. the installed-header/build audit (ledger row `NOT_ASSESSED` → assessed) covers the surface;
4. no formal/docs claim depends on it as current evidence (formal map `NOT_CURRENT_CLOSURE_EVIDENCE` items excepted);
5. `UNKNOWN_EXTERNAL_USE` has been resolved by an explicit decision of the phase owner (documented acceptance of breakage risk, or a deprecation window) — because no compatibility policy exists, **this decision is a #402 human-review obligation, not an agent default**.

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
| `OPTIONAL_SUPPORTED` | Shipped optional profile (stackful host family + its conveniences); supported only if its gate (W-04, D2 review, ADR-0003 adoption) completes; otherwise `EXPERIMENTAL` by PROD-02/MIG-03 |
| `COMPATIBILITY` | Retained pre-v1 / dormant surface per §1.1; no promise; retirable under §1.7 |
| `EXPERIMENTAL` | Clearly isolated unsupported experiments (LIFE-03/MIG-02); must not be described as v1 conforming |
| `INTERNAL_ONLY` | `detail/` mechanism + backend seam + runtime mechanism; consumer-visible today only via the public include path; must never be consumed directly by new external code |
| `TEST_ONLY` | Public-tree test doubles/seams; exist for evidence, not product |
| `HISTORICAL_RESEARCH` | Archived authorities, research results, historical models (GOV-03; formal map dispositions) |

Per-profile obligations (current facts, not aspirations):

| Attribute | CANONICAL | OPTIONAL_SUPPORTED | COMPATIBILITY | EXPERIMENTAL | INTERNAL_ONLY | TEST_ONLY | HISTORICAL_RESEARCH |
|---|---|---|---|---|---|---|---|
| INSTALLED (today) | NO | NO | NO | NO | NO | NO | NO |
| PUBLIC_HEADERS (today) | yes (include path) | yes | yes | yes | yes (detail/, de-facto transitive) | yes | docs/formal trees |
| TARGET_LINKABLE (today) | `sluice_core`+`sluice_async` | same + app/test binaries | via the same libs | **no target at all** (orphan TUs, FA-2) | inside the two libs | inside the two libs | not build products |
| CANONICAL_DOCS | root + README target sections | ADR-0003 (PROPOSED) + D2 ledger | MIG-02 disposition rows only | LIFE-03/MIG-02 constraints | ARCH/BACKEND/REQ/OBS/PROG rules | none | supersession banners |
| WORKLOAD_ACCEPTANCE | W-01–W-03 (+V01–V27 where evidenced) | W-04 gate | none required; must not regress canonical evidence | none | exercised through canonical tests | test-only | none |
| REQUIRED_EVIDENCE | ledger VERIFIED per slice | W-04 tracer + D2 rounds + ADR adoption | consumer audit + migration evidence before any change | root amendment before any supported claim | follows the owning slice | mutation/oracle records | none (retained for provenance) |

The load-bearing asymmetry: **`physically installed ≠ supported canonical`** (currently nothing is installed, so the discriminating case is future) and **`not installed ≠ internal by definition`** (today everything public is merely on an include path; `EXPERIMENTAL` headers are neither installed nor internal).

## 3. External-use uncertainty register

There is no repository compatibility/API policy document (GREP of docs/, ADRs, README: none found). Therefore, per #454 §8:

| Family | IN_TREE_CONSUMERS | UNKNOWN_EXTERNAL_USE |
|---|---|---|
| CANONICAL surfaces | recorded in census §2 | `unknown` (no policy ⇒ cannot assert zero external use, but canonical surfaces are not removal candidates anyway) |
| `Completion<T>` + `Batch` + `op_helpers` | 15 test files include `completion.hpp`, **25 test files name the type**; **4 apps construct/settle `Completion` in task code** (copy/hash/grep/tail via `await_op_helpers`); `Batch`: 1 test; `op_helpers`: 1 test | `unknown` — and the app usage means Completion retirement **cannot** be scoped as a test-only migration (DAG X-01) |
| Host/runtime family (H-06..H-13, H-16, H-26) | 4 apps + host tests + scheduler TUs | `unknown` |
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
| D-16 | `StackfulIoHost` fiber exposure (H-09 boundary) | KEEP_PUBLIC, **fix header boundary first** (F3 prerequisite) | OPT_SUPPORTED | — | n/a (it is the replacement) | src 1 + tst 1 | n/a | HIGH | census §1.4 |
| D-17 | `RequestHandle` (R-05) | KEEP_PUBLIC (identity token; MIG-02 separation) | CANONICAL | implemented (RECORD); adoption state pending ledger | n/a | src + tst | n/a; `identity_of` arena fallback is D-02's precondition | HIGH | census R-05; DAG review C-5 |

`KEEP_PUBLIC` rows are intentionally few: the CANONICAL + OPT_SUPPORTED row sets of §1.1 (listed there by reference to the census rather than duplicated).
