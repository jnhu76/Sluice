# Phase M — owner-directed Clean Minimal V1 decision overlay (2026-10-10)

**Authority and provenance.** This is an owner-directed delegated *disposition selection* made in the connected owner conversation, recorded at [#458 comment 6099156773](https://github.com/jnhu76/Sluice/issues/458#issuecomment-6099156773), following an independent GPT-6 Astra technical analysis. This document is **not** a substitute for a merged GOV-04 root amendment, an adopted #402/F0 rule, a tested code migration, an F1 freeze or release authorization. The previous [wave-2 packet](m-phase-closeout-wave2.md) remains the dated evidence and pre-decision candidate record; its pending registry must not be misread as still lacking a selected target.

- Reviewed production baseline: `94b5cfcdace6ee15c75c909dae09665eda2d1a26`.
- Candidate evidence: PR #487 initial head `4bffbbfa9c9deb38a1fd8fc1341400a2428e7280`.
- Owner instruction: adopt Astra's **Clean Minimal V1 with conservative, reversible family migration**, **no inherited v1 legacy obligations by default**.
- Normative order: root > adopted ADR > #402 phase policy > adopted F0/F1 policy > ledger/evidence > implementation. This file records intent; it does not silently amend higher authority.

## Decision table and WHY

| ID | Selected target | Why / rejected burden | Current allowed action and downstream gate |
|---|---|---|---|
| D-H1 | Fix GREP multi-file separator `path:` | `-n` must not determine whether file-name prefix is delimited; no universal GNU compatibility pledge | F3/app: focused CLI fix, oracle, README; breaking output shape explicitly noted |
| D-H2 | Validate configuration early, using the same app-local predicate in CLI and engine | Illegal options must not first masquerade as file I/O failures; do not create a shared cross-app validation framework | COPY/HASH illegal bound → usage exit 1, GREP invalid/usage stays 2 and no-match stays 1; owner-authorized CLI contract adjustment; tests |
| D-H3 | Do not freeze unreachable waiter-error precedence | Freezing an unobservable legacy branch expands the public contract without workload evidence; accepted-operation settlement remains mandatory | F3/COPY: before any change making branch reachable, re-decide and construct discriminator; not F3-only trigger |
| D-H4 | No new TAIL public settlement API | `Result<TailResult>` already carries errors; duplicating lifecycle authority for obsolete runtime is unjustified | F3/TAIL: preserve every-exit settlement, explicit cleanup-health handling; real UAF/liveness counterexample blocks relevant use immediately |
| D-H5 | Preserve four-app behavior, retire their legacy runtime dependency | Each app submits one task; `--workers` sizes Scheduler and not backend nor per-file parallelism. Scheduler is not a W-01–W-03 requirement | KEEP_SOURCE/BUILD now, POLICY_ISOLATED, NO_V1_SUPPORT_PROMISE. HASH/GREP→direct; COPY→bounded Request/Scope; TAIL→app-owned driver. W-04 optional, not app exit prerequisite. Explicit `--workers` migration |
| D3a | A3a — no canonical vectored admission | No accepted current workload justifies iovec/buffer-lifetime/partial-slice contract; writev single-call property is not WAL crash atomicity | F4: retire legacy vec with its no-root-consumer stream family, not an immediate deletion |
| D3b | B1 — retire independent legacy File semantic authority | Legacy Reader/Writer/FileReader/FileWriter defines conflicting ownership/error/position rules instead of delegating canonical File; no admitted obligation remains | F4: close semantic_range residual and shared retry/DirFsync dependencies, consumers and external risk before removal |
| D-12 | Exclude unadmitted WAL/library-Copy/Buffer/Observed | Cannot invent a product workload from existing old helper implementations; `apps/sluice-copy` is separate | F4: retire with L-01, preserve genuinely shared helpers after per-edge ownership audit |
| D-13 | Exclude test doubles from v1 install | TEST_ONLY is not an installed SDK entitlement; observed current test roots do not require legacy MemoryIoContext | F4/F5: real active test seams internalize only if necessary; rest retires with stream family after package/external decision |
| D4 | P2 final retirement; P3 bounded transition | No PROD-03 workload or resource obligation justifies general BlockingIoPool/Task<T> pool | Exclude from v1; still installed in current compatibility tree until F4/F5 and explicit external-risk decision; no perpetual sunset |
| U-03/U-04 | Deliberate pre-v1 deprecation + bounded, per-surface risk acceptance | v0.0.1 experimental/no SemVer **and** prior `stable-ish / deliberate-deprecation` text both matter; zero in-tree users ≠ zero external use | F4/F5: itemized affected headers/symbols, replacement or NO_EQUIVALENT, release note, targeted owner risk choice; include U-05, BlockingIoPool, and #475 triggers rather than assuming U-03/U-04 covers all |
| F-W2-1 | Versioned dual-profile re-freeze **now** | Installed `threadpool_backend.hpp` changed after F1 frozen production baseline `a34d96c6`; passing verifier self-tests does not mean HEAD provenance pass | Independent compensatory slice with real noliburing+liburing manifest/archive/clean-room/link/ODR/negative evidence; never mutate original `f1-*.json` |

**Anti-burden invariant:** `KEEP_SOURCE != KEEP_BUILD != KEEP_INSTALLED != KEEP_SUPPORTED != SHIP_IN_V1`. An unadmitted capability receives `DECIDED_TO_EXCLUDE_FROM_V1`, not `SAFE_TO_UNINSTALL`. The latter requires the #402/F0 §1.7 family, external-user, consumer and package gates.

## Higher-authority conflicts needing focused PRs

1. **#402 F3 Exit Gate W-04:** it presently demands unconditional `W04_SUPPORTED_HOST_PROFILE=PASS`, conflicting with PROD-02's optional supported host. Gate must be conditioned on *actually shipped as supported*, otherwise `N/A + package isolation proof`. Do not mark deferred host VERIFIED.
2. **F0 §1.7 replacement requirement:** no admitted workload/no replacement must not force development of a fictitious adapter. A formally adopted **negative product/exclusion disposition** may serve as the narrowly scoped alternative to an adopted replacement, but consumer/docs/package/external obligations remain.
3. **#458 per-app exit:** do not make D2/W-04 implementation verification a universal prerequisite for HASH/GREP/COPY/TAIL port. Each retains exact workload/behavior/borrow/stop proofs.
4. **GOV-04 root amendment if necessary:** write explicit non-admission dispositions for vectored, WAL/stream and BlockingIoPool without making any claim of already retired source; revision and §23 log required for substantive changes. The existing root PROD-03 and MIG-02 are not overridden by this decision memo.
5. **#475** A=BOUNDED_HOLD / B,C=KEEP_INSTALLED_COMPAT remains adopted until its named reconsideration triggers. This decision does not cancel that policy without due process.

## Phase M exit gate, not release gate

```text
TARGET_SELECTED = YES (owner-directed)
NORMATIVE_AMENDMENTS_ADOPTED = NO (until reviewed and merged)
TWELVE_DECISIONS_BACKFILLED = NO (until final canonical md/JSON/issue adoption)
F_W2_1_DUAL_PROFILE_REFREEZE = NOT_DONE
M_FINAL_DELTA_INDEPENDENT_REVIEW = PENDING
PHASE_M_CLOSE_ALLOWED = NO
F2_F3_F4_F5_PRODUCTION_RETIREMENT = NOT_AUTHORIZED
OPTIONAL_SUPPORTED = NO
RELEASE_READY = NO
```

When higher-authority amendments, final decision packet review and versioned two-profile re-freeze are complete, Phase M can close **without waiting for F2–F5 physical retirement**. Never flip a gate just to satisfy issue closure. Preserve all prior historical evidence and scoped test/formal obligations; remove only genuinely unnecessary v1 semantic/package burdens in their F slices.
