> **HISTORICAL / EVIDENCE — NOT CURRENT AUTHORITY.** Archived from `docs/architecture/` by S0-DOCS (#290, 2026-09-04). Point-in-time record; do not cite as authority for new decisions.

# Phase C1 Conformance Gate — Explicit-I/O Backend Conformance Framework

**Roadmap:** [`remediation-roadmap.md`](remediation-roadmap.md) — Phase C (status PARTIAL)
**Governing ADR:** [`ADR-explicit-io-request-contract`](../../adr/ADR-explicit-io-request-contract.md) (Accepted)
**DIV-13:** [`divergence-registry.md`](../../architecture/divergence-registry.md) — AsyncBackend public extension point
**PR:** https://github.com/jnhu76/Sluice/pull/66 ("test/phase-c-explicit-io-conformance-gate" git branch, OPEN)
**Scope:** Test/framework only. No `src/` or `include/sluice/` change.

This is the PR-level evidence ledger for Phase C1, the explicit-I/O backend conformance gate.
C1 establishes a reliable, extensible, auditable backend-conformance "ruler"; it does **not**
add semantic coverage (that is C2) and does **not** migrate Uring (that is Phase D). Phase C
remains **PARTIAL**.

---

## C1 corrective (first corrective: CI fix + attribution isolation)

The first C1 push (implementation head `32b1ff4`) landed two latent defects that the local
matrix did not catch but GitHub CI did:

1. **CI hardening-runner failure.** `scripts/tests/test_backend_conformance_manifest.py`
   called `sys.exit(0)` at **module top level**. The CI step `python3 -m unittest discover -v
   scripts/tests` imports every test module during collection; a `SystemExit` at import time
   becomes `unittest.loader._FailedTest` → `ERROR` → `FAILED (errors=1)` → job exit 1 (GitHub
   Actions run `30972306135`, job `92199116313`, step "Hardening runner unit tests"). Local
   matrix missed it because it ran the file as a *script*, not under `unittest discover`.

2. **Cross-backend result attribution.** The aggregate gate drove the shared conformance suite
   for all three backends in **one process**. The in-binary test harness
   ([`tests/harness.hpp`](../../../tests/harness.hpp)) **breaks on the first failing case**, so a
   single backend's shared-case failure made the driver exit non-zero and the gate then
   attributed that one `RUN_FAIL` to **all three** backends — including the unrelated Fake and
   the Uring(stub) case that never ran.

Both are fixed by the C1 corrective commits (implementation-fix head recorded below under
three-head discipline). The fixes are **scope-pure**: `scripts/` + `.github/workflows/` + docs
only — no `src/`, no `include/sluice/`, no test-binary change.

### Result-attribution isolation model (corrected)

The shared suite is now driven **once per registered backend in a separate subprocess**,
filtered to that backend's case via `SLUICE_TEST_FILTER=<driver_case>` (the harness case names
`conformance_fake` / `conformance_threadpool` / `conformance_uring` already exist in
[`tests/backend_conformance_driver_test.cpp`](../../../tests/backend_conformance_driver_test.cpp)).
Each subprocess owns:

- its own exit code (→ `PASS` / `RUN_FAIL` for that backend only);
- its own `[conformance-meta]` line (backend identity + mode); and
- its own `[conformance] FAIL <backend>` lines.

A backend's verdict therefore reads ONLY that backend's own subprocess result. One backend's
`RUN_FAIL` can no longer become another backend's state. The regression suite in
`scripts/tests/test_backend_conformance_manifest.py` proves this with fabricated per-backend
results (Fake-fails / ThreadPool-fails / Uring-fails) and asserts each unrelated backend keeps
its own verdict.

---

## C1 review-fix corrective (this update)

The PR review (Request changes) found two fail-closed gaps in the aggregate gate. Both are
closed at this head.

### P1: a per-backend shared run that ran NOTHING still counted as PASS (false green)

The gate drove each backend's shared-suite case via `SLUICE_TEST_FILTER=<driver_case>` and
classified the run purely from the subprocess exit code. The in-binary harness matched filter
tokens by SUBSTRING, and a filter that matched zero cases printed `ALL TESTS PASSED` and
exited 0 — so a typo'd or renamed `driver_case` made the subprocess "pass" without running
anything, and the backend was recorded ELIGIBLE while the gate exited 0. (The residual-risk
note in the original corrective claimed a zero-match filter "runs the whole suite"; that was
wrong — it runs ZERO cases and fabricates a green banner.)

Closed at two layers:

- **Harness ([`tests/harness.hpp`](../../../tests/harness.hpp))**: `SLUICE_TEST_FILTER` tokens
  must now be EXACT registered case names (no substring matching), and a set filter that
  matches zero cases prints `SLUICE_TEST_FILTER matched zero cases` and exits non-zero — a
  zero-case run can never print `ALL TESTS PASSED`. This is fail-closed for every harness
  user, not just the gate. The documented substring workflows are updated in the same change:
  `AGENTS.md` §7, `scripts/run_test_repeated.sh`, and the stability-script headers
  (`verify-runnable-steal-stability.sh`, `verify-select-rollback-stability.sh`) now say
  "exact case name". All committed callers already pass full case names; no CI step passes a
  prefix.
- **Gate ([`scripts/verify-backend-conformance.py`](../../../scripts/verify-backend-conformance.py)
  `_classify_shared_run`)**: a per-backend shared run is PASS only when ALL of the following
  hold:
  1. subprocess returncode == 0 (else `RUN_FAIL`);
  2. the harness executed exactly `[driver_case]` — parsed from the stable `[run] <case>`
     lines the harness prints per executed case (zero or extra cases → `INCOMPLETE`);
  3. exactly one `[conformance-meta]` line (missing/duplicate/foreign → `INCOMPLETE`);
  4. its backend, canonicalized (`Uring(stub)` → `Uring`), equals the registered backend;
  5. its profile equals the manifest-declared profile; and
  6. its mode is in the profile's closed `PROFILE_MODES` set
     (`ReferenceProfile→deterministic`, `BlockingIoProfile→real`,
     `KernelIoProfile→real|stub`).

  Any violation is `INCOMPLETE` — never `PASS`.

### P2: one PASS per mandatory layer was treated as "layer satisfied"

The old verdict logic satisfied a mandatory layer when ANY evidence in it passed
(`layer_states & SATISFACTORY`), never consulted `ev.mandatory`, and scanned only for
`RUN_FAIL` — so a layer with one PASS + one MISSING_TARGET could still yield ELIGIBLE, and
"no evidence at all" was misclassified as NOT CONFORMING (a violation) instead of INCOMPLETE
(insufficient evidence). The verdict vocabulary claimed `INCOMPLETE` in the gate docstring
but the code never produced it.

The verdict is now an explicit priority over the backend's applicable MANDATORY evidence:

```text
any RUN_FAIL                                   -> NOT CONFORMING  (proven violation)
else any MISSING_TARGET / BUILD_FAIL /
     NOT_RUN / INCOMPLETE                      -> INCOMPLETE      (insufficient evidence)
else all PASS / legal NOT_APPLICABLE and
     every mandatory layer actually covered    -> ELIGIBLE
```

Non-mandatory evidence is diagnostic only: it can neither satisfy a mandatory layer nor block
ELIGIBLE. A layer with one PASS and one MISSING_TARGET is INCOMPLETE; an INCOMPLETE verdict
for a non-kernel backend fails the gate (exit 1). KernelIo remains NOT CONFORMING until
Phase D regardless (unchanged).

### Regression coverage

`scripts/tests/test_backend_conformance_manifest.py` adds:

- `SharedRunFailClosedTest` — driver_case typo, zero selected cases, missing/duplicate/foreign
  `[conformance-meta]`, wrong backend/profile/mode, filter matching multiple cases, valid run,
  non-zero rc, `Uring(stub)` canonicalization;
- `MandatoryVerdictPriorityTest` — one PASS + one MISSING_TARGET in a layer → INCOMPLETE,
  RUN_FAIL beats INCOMPLETE, uncovered mandatory layer → INCOMPLETE, non-mandatory evidence
  is diagnostic-only;
- `ProfileModesTest` — `PROFILE_MODES` is the closed profile→mode vocabulary and every
  registered backend has a non-empty `driver_case`.

All new cases were RED on the pre-fix code (the old state machine returned
ELIGIBLE/NOT CONFORMING where INCOMPLETE is required) and are GREEN at this head.

---

## C1 review round 2 (this update)

Second review pass — findings against the review-fix head, applied at this head:

- **Empty `driver_case` can no longer run the whole suite unfiltered.** The gate's
  `_run_shared_suite` rejects a registered backend with an empty `driver_case`
  (`INCOMPLETE`, never a subprocess) — an empty filter would otherwise run every backend's
  cases in one process and fabricate attribution. The manifest self-test already requires a
  non-empty `driver_case` per backend.
- **Canonical meta keys.** `[conformance-meta]` lines are normalized through
  `canonical_backend_key` ONCE when the report table is built, so any variant
  (`Uring(stub)`, `ThreadPool(stub)`, …) resolves to the registered backend name; the
  hardcoded `_meta_name_for` "(stub)" special case is gone.
- **`_drive` args guard.** `G.Gate._drive` guards `self.args` exactly like
  `_run_shared_suite` (`args=None` no longer raises when checking `--no-build`).
- **Tautological manifest tests replaced with behavioral checks.**
  `test_not_implemented_never_counts_as_pass` now drives the gate with a fabricated
  `not_implemented` evidence record and asserts the result is `INCOMPLETE` (never in
  `SATISFACTORY`); `test_no_unregistered_backend_can_be_eligible` calls `_backend_verdict`
  with an unregistered `BackendEntry` and asserts it is not ELIGIBLE. The registry-content
  assertion remains in `BackendRegistryTest`.
- **External-admission probe scope (per review).** `external_backend_admission_test` now
  proves ONLY subclass compilation + `AsyncIoContext` ownership (one case); the public
  claim/publish wrappers and the direct Completion lifecycle assertions are removed. Derived
  access to the protected helpers is proven at COMPILE level by the authority probe's
  positive control, which now exercises `try_claim` / `publish` through private using
  declarations in `LegitimateBackend` (so a protected→private regression in `AsyncBackend`
  fails the positive control); claim/publish lifecycle semantics remain covered by the
  arena-backed Completion tests.
- **CI budget.** The `linux-clang-debug` job budget is raised to 60 minutes with a comment
  tying it to the aggregate gate's per-run subprocess timeouts (the gate re-runs every
  evidence target in its own subprocess; a hung target fails the gate — fail-closed — and
  the larger budget lets it finish attributing failures instead of being killed mid-report).
- **Ledger hygiene.** Full SHA recorded for the implementation-fix head; the duplicate
  "C++ test evidence" heading removed.

---

## SHA provenance (three-head discipline)

Following the PR #64/#65 corrective, evidence SHAs are separated so the ledger does not recurse
(changing a recorded SHA must not change the SHA it records). The C1 corrective introduces a
fresh implementation-fix head; the original implementation head is preserved for history.

- **Baseline master:** `07cfcad401971c7333bf3b0f2d959759d508a3bd`
- **Original reviewed PR head:** `75d58b6f0ef29fbd8859cb6f0bfd2a34cb7de833`
  (implementation head `32b1ff4a2643834627ae2423ca894a18a52e8c8d`; GitHub CI RED here).
- **Implementation-fix head:** `10b0dba96221cf0ecaf1ccf4541e15d4146b4261` — the C1
  corrective commit (gate/result-attribution fix + unittest-discover fix + CI wiring). The
  validation matrix below re-ran at this head.
- **Review-fix head:** `bbd91984c03f0f4c5e9aeb8fab549ac87c380188` — the review corrective
  fix commit (P1: harness exact-filter + zero-match error + gate `_classify_shared_run`
  meta/selection validation; P2: verdict priority with a real `INCOMPLETE` verdict),
  with the regression tests and the review-fix documentation as separate slices of the
  same PR. The validation matrix re-ran at this head.
- **Evidence/docs head:** this documentation commit (records the fix-head results + this
  corrective section + roadmap update; no production or test-binary change).
- **PR final head:** TBD at merge time (will be the implementation-fix head unless further
  review changes land).
- **GitHub Actions-validated head:** TBD — must be a green run on the final PR head before
  READY.

No evidence row below is pre-marked PASS; every PASS row corresponds to a command that actually
ran. The C1 corrective rows are bound to the implementation-fix head, not the stale
`32b1ff4` matrix (kept below under "Original implementation-head matrix" for history).

---

## What C1 produces

C1's product is an evidence *system*, not "more tests". It can honestly answer:

- which evidence applies to which backend (closed profiles);
- which shared semantics / lifecycle / authority / backend-specific layers PASS;
- whether a backend is ELIGIBLE for "conforming" status;
- which cases are genuinely not-applicable (with a reason) vs not-yet-implemented (never PASS);
- why Uring is NOT CONFORMING today (Phase D migration not implemented).

### Closed profiles (registered backends only)

```text
ReferenceProfile   → Fake         (the only reference backend the shared driver instantiates)
BlockingIoProfile  → ThreadPool
KernelIoProfile    → Uring
```

The C1 backend registry covers Fake, ThreadPool, and Uring. Any synchronous/reference
implementation not instantiated by the shared driver (e.g. SyncBackend) remains out of the C1
backend registry and is **not** inferred conforming. A profile never names a backend with no
evidence object.

### Three-layer evidence model

- **A. Shared observable semantics** — the 8-case `backend_conformance_test` suite
  (submit→reap exactly-once, positional independence, EOF, short-completion retry, terminal
  exactly-once, cancel-defined-terminal, stats, clean shutdown).
- **B. Lifecycle protocol evidence** — aggregated EXISTING targets (RequestSlot/identity/reap/
  terminal contracts): `request_arena_*`, `reference_backend_*`, `completion_*`,
  `request_lifecycle_scheme_b_*`, the ThreadPool scheme-B/death targets.
- **C. Backend-specific mechanism** — `fake_backend_test`, `threadpool_backend_*`,
  `uring_backend_test`. Kept OUT of layer A by construction.

### External backend admission (NOT conformance)

`external_backend_admission_test` proves a legitimate backend subclass can be built from the
PUBLIC extension surface (no `<sluice/async/detail/*>` include) and admitted into an
`AsyncIoContext`, destroying cleanly with zero outstanding work. It is **not** run through the
shared observable-semantics suite and is reported as `conformance NOT ASSESSED`. The companion
negative-compile gate closes the narrower gap that `AsyncBackend` protected publication helpers
(`try_claim` / `publish`) are inaccessible to non-derived ordinary code; its positive control
proves at compile level that a DERIVED backend can still reach both helpers (via private using
declarations in `LegitimateBackend`), and the claim/publish lifecycle semantics live in the
arena-backed Completion tests.

---

## Validation matrix — C1 corrective (ran at implementation-fix head)

Evidence is split by class: C++ test, Python infra, negative-compile, repository. The C++
target set is unchanged from the original implementation head (`32b1ff4`), so the C++/sanitizer
rows below remain valid; the Python-infra and CI rows are re-run at the implementation-fix head
because that is where the gate and self-test changed.

### C++ test evidence (unchanged target set; re-verified at fix head)

| Command | Profile / target | Result | Notes |
|---|---|---|---|
| `xmake f -m debug --toolchain=clang -y` → `xmake build sluice_core` → `xmake build sluice_async` → `xmake build -g test` → `xmake test -v` | Clang Debug, all 139 targets | **PASS** (139/139, 0 failed) | Includes new `external_backend_admission_test` (3 cases) and modified `backend_conformance_test` (now emits `[conformance-meta]`). |
| `xmake f -m release --toolchain=clang -y` → build `sluice_core`/`sluice_async`/`-g test` → `xmake test -v` | Clang Release, all 139 targets | **PASS** (139/139, 0 failed) | Change-class gate (§16.1): new test target + public-extension-surface probe. |
| `xmake f -m asanubsan --toolchain=clang -y` → build → `xmake test -v` | ASan+UBSan, all 139 targets | **PASS** (139/139, 0 failed) | Change-class gate (§16.2): new tests touch Completion/buffer lifetime. No sanitizer errors. |
| `xmake f -m tsan --toolchain=clang -y` → build → `xmake test -v` | TSan, all 139 targets | **PASS** (139/139, 0 failed) | Change-class gate (§16.3). No data-race reports. |
| `xmake run external_backend_admission_test` | focused, Debug | **PASS** (3 cases) | `external_backend_can_claim_and_publish`, `external_backend_owned_by_context_destroys_clean`, `external_backend_double_claim_loses`. |
| `xmake run backend_conformance_test` | focused, Debug | **PASS** (+ `[conformance-meta]` lines) | Fake/ThreadPool/Uring(stub) each emit meta; 4 fd-backed cases skip on non-real backends. |

### Python infrastructure evidence

| Command | Result | Notes |
|---|---|---|
| `python3 -m unittest discover -v scripts/tests` | **PASS** | The exact CI invocation. 208 tests, 0 errors/failures. Includes the manifest self-test (36 cases) AND the new per-backend attribution/isolation regression suite. This is the command that was RED at `32b1ff4` (top-level `sys.exit` → `_FailedTest`) and is now GREEN. |
| `python3 scripts/tests/test_backend_conformance_manifest.py` | **PASS** (exit 0) | Standalone invocation: 36 cases (manifest invariants + result-attribution isolation). Proves ThreadPool-fails leaves Fake=ELIGIBLE, Fake-fails leaves ThreadPool=ELIGIBLE, Uring-fails leaves both ELIGIBLE, and the closed profile mapping (ReferenceProfile→Fake, BlockingIoProfile→ThreadPool, KernelIoProfile→Uring) admits no nameless backend. |
| `python3 scripts/verify-backend-conformance.py [--no-build]` | **PASS** (exit 0) | Aggregate gate now drives the shared suite **once per registered backend in a separate subprocess** (`SLUICE_TEST_FILTER=conformance_<backend>`). Fake=ELIGIBLE, ThreadPool=ELIGIBLE, Uring(stub)=NOT CONFORMING (Phase D), external=admission PASS / conformance NOT ASSESSED. Per-backend results are isolated by construction. |

### Negative-compile evidence

| Command | Result | Notes |
|---|---|---|
| `scripts/verify-completion-authority-negative-compile.sh` | **PASS** (12 cases) | Existing: Completion publication authority. |
| `scripts/verify-request-arena-negative-compile.sh` | **PASS** (6 cases) | Existing: RequestSlot private-field authority. |
| `scripts/verify-async-identity-negative-compile.sh` | **PASS** (3 cases) | Existing: async identity private-setter authority. |
| `scripts/verify-external-backend-authority-negative-compile.sh` | **PASS** (2 cases) | **NEW (C1):** AsyncBackend protected helpers inaccessible to non-derived ordinary code (`NEG_TRY_CLAIM_AS_NON_BACKEND`, `NEG_PUBLISH_AS_NON_BACKEND`). |

### Repository evidence

| Command | Result | Notes |
|---|---|---|
| `python3 scripts/check-doc-links.py` | **PASS** | No broken links / stale paths. |
| `python3 scripts/verify-architecture-docs.py` | **PASS** | Constitution/divergence/PR-template/AGENTS checks. |
| `git diff --check` | **PASS** | No whitespace errors. |
| `.github/workflows/ci.yml` Phase C1 steps | **WIRED** | Three explicit, named steps added after the hardening runner: (1) `Phase C1 backend-conformance aggregate gate` (`verify-backend-conformance.py --no-build`); (2) `Phase C1 manifest + attribution self-test` (`test_backend_conformance_manifest.py`); (3) `External-backend authority negative-compile (Phase C1)`. Previously these ran only as implications of other steps; a result-attribution regression could pass as "runner unit tests pass". Each step's failure now makes the job non-zero directly. |
| GitHub Actions (Linux Clang Debug) | **PENDING** | Must be a GREEN run on the final PR head before READY. Was RED at `32b1ff4` (run `30972306135`, hardening step) due to defect 1; the corrective's new run is the gate of record. |

### Review-fix head re-run (P1/P2 corrective)

All commands ran at the review-fix head (Clang Debug; regression tests RED on the pre-fix
code, GREEN at this head).

| Command | Result | Notes |
|---|---|---|
| `xmake f -m debug --toolchain=clang -y` → `xmake build sluice_core` → `xmake build sluice_async` → `xmake build -g test` → `xmake test -v` | **PASS** (139/139, 0 failed) | Full baseline after the harness change (`tests/harness.hpp` exact-match + zero-match error). |
| `python3 -m unittest discover -v scripts/tests` | **PASS** (232 tests) | The exact CI invocation; includes hardening-runner tests + the 60-case manifest/attribution suite. |
| `python3 scripts/tests/test_backend_conformance_manifest.py` | **PASS** (60 tests, exit 0) | Standalone: 36 original cases + 24 new (ProfileModes 6, SharedRunFailClosed 12, MandatoryVerdictPriority 6). All 24 were RED pre-fix (old state machine returned ELIGIBLE/NOT CONFORMING where INCOMPLETE is required; new symbols absent). |
| `python3 scripts/verify-backend-conformance.py --no-build` | **PASS** (exit 0) | Fake=ELIGIBLE, ThreadPool=ELIGIBLE, Uring(stub)=NOT CONFORMING (Phase D), external=admission PASS / conformance NOT ASSESSED. Per-backend shared runs now validated by `_classify_shared_run`. |
| `SLUICE_TEST_FILTER=no_such_case xmake run backend_conformance_test` | **FAILS** (exit ≠ 0) | Probe: harness prints `SLUICE_TEST_FILTER matched zero cases` and exits non-zero — no more "ALL TESTS PASSED" on a zero-case run. |
| `SLUICE_TEST_FILTER=conformance_fak xmake run backend_conformance_test` | **FAILS** (exit ≠ 0) | Probe: substring typos now select zero cases (exact match) and fail instead of silently running `conformance_fake`. |
| `SLUICE_TEST_FILTER=conformance_fake xmake run backend_conformance_test` | **PASS** (exit 0) | Probe: exactly one `[run] conformance_fake` + one `[conformance-meta]` line. |
| `python3 scripts/check-doc-links.py --self-test` / `check-doc-links.py` / `verify-architecture-docs.py` | **PASS** | Doc links + architecture-doc structure after the corrective edits. |
| `git diff --check` | **PASS** | No whitespace errors. |

### Review round 2 re-run (this head)

All commands ran at the round-2 head (Clang Debug).

| Command | Result | Notes |
|---|---|---|
| `xmake f -m debug --toolchain=clang -y` → `xmake build sluice_core` → `xmake build sluice_async` → `xmake build -g test` → `xmake test -v` | **PASS** (139/139, 0 failed) | Full baseline after the admission-probe scope change (`external_backend_admission_test` now 1 case) and the positive-control update in the authority probe. |
| `python3 -m unittest discover -v scripts/tests` | **PASS** | Includes the rewritten behavioral tests (not_implemented → INCOMPLETE; unregistered backend → not ELIGIBLE) and the 3-backend-meta fail-closed case. |
| `python3 scripts/tests/test_backend_conformance_manifest.py` | **PASS** (60 tests, exit 0) | Two tautological assertions replaced with behavioral checks; foreign-meta case extended to all three backends. |
| `python3 scripts/verify-backend-conformance.py --no-build` | **PASS** (exit 0) | Fake=ELIGIBLE, ThreadPool=ELIGIBLE, Uring(stub)=NOT CONFORMING (Phase D); meta now canonical-keyed; empty `driver_case` rejected before any subprocess. |
| `bash scripts/verify-external-backend-authority-negative-compile.sh` | **PASS** | Positive control compiles with the private using declarations + claim/publish calls; both NEG_ cases still rejected with the expected diagnostic. |
| `xmake run external_backend_admission_test` | **PASS** (1 case) | `external_backend_owned_by_context_destroys_clean`; probe scoped to compile + ownership. |
| `python3 scripts/check-doc-links.py` / `verify-architecture-docs.py` / `git diff --check` | **PASS** | Doc links + architecture-doc structure + whitespace after round-2 edits. |

---

## Aggregate gate output (default stub build, no liburing)

```text
Backend: Fake (ReferenceProfile)
  mode (from meta): deterministic  profile (from meta): ReferenceProfile
  shared                 PASS
  lifecycle              PASS
  backend specific       PASS
  overall               ELIGIBLE

Backend: ThreadPool (BlockingIoProfile)
  mode (from meta): real  profile (from meta): BlockingIoProfile
  shared                 PASS
  lifecycle              PASS
  backend specific       PASS
  overall               ELIGIBLE

Backend: Uring (KernelIoProfile)
  mode (from meta): stub  profile (from meta): KernelIoProfile
  shared                 PASS (stub subset)
  lifecycle              INCOMPLETE
  backend specific       INCOMPLETE
  overall               NOT CONFORMING
    reason: kernel profile built as stub (Phase D not implemented)

External backend probe (admission, NOT conformance)
  extension admission    PASS
  authority boundary     PASS
  conformance            NOT ASSESSED

RESULT: PASS (all mandatory gates satisfied; KernelIo NOT CONFORMING is expected before Phase D).
```

**Uring is never marked conforming.** The KernelIoProfile lifecycle/backend_specific layers
report `INCOMPLETE` (not PASS) because the RequestSlot contract evidence is real but Uring has
not been migrated onto RequestArena (Phase D pending) — the contract exists, Uring does not yet
implement it. The shared suite covers only the stub subset in a stub build.

---

## Skipped / out-of-scope evidence

- **Real liburing path:** not built/available in this environment (default `--with-liburing`
  off). The gate's Uring classification is therefore bound to the **stub** build; a real-liburing
  run would still classify the KernelIo profile as NOT CONFORMING until Phase D migration, but
  would additionally exercise the real-path shared cases. Reported as **SKIPPED** (toolchain
  unavailable), not PASS.
- **Phase C2 semantic matrix:** full capacity/high-water/rejection, full stale
  cancel/wait/reap matrix, allocator/startup/dispatch failure injection across all backends,
  full waiter/borrow/delivery-lease interleave, all shutdown/reset/non-quiescent destruction
  cases — explicitly out of C1 scope (C2).
- **Phase D Uring migration:** RequestArena/RequestKey identity, kernel transport token,
  bounded dispatch/staged SQE queue, removal of Uring legacy maps/deques, partial-submit
  production migration — explicitly out of C1 scope (Phase D).

---

## Residual risk / review hotspots

- The `external_backend_admission_test` proves the public extension surface admits a subclass;
  it does NOT prove the subclass *conforms* (it implements no real semantics). The report says so
  explicitly (`conformance NOT ASSESSED`).
- The aggregate gate's Uring mode classification comes from the stable `[conformance-meta]`
  line, not from display-name or skip-text parsing. A driver that forgets, duplicates, or
  misdeclares the meta line now makes that backend's shared run `INCOMPLETE` (and the Fake/
  ThreadPool verdict `INCOMPLETE` fails the gate); for KernelIo a missing meta additionally
  stays NOT CONFORMING via `mode=unknown`.
- DIV-14 (descriptor-validation deferral) remains open for Fake/Sync (reference, no real syscall)
  and Uring (Phase D); it is recorded, not a C1 blocker.
- **C1 corrective residual — CLOSED at the review-fix head:** the original corrective's
  residual risk (a rename of the harness case names or the manifest `BackendEntry.driver_case`
  without the other) is now fail-closed: the harness exits non-zero when a set filter matches
  zero cases, and the gate's `_classify_shared_run` requires exactly the driver case to have
  executed plus exactly one valid `[conformance-meta]` line. A one-sided rename now fails the
  gate with a precise `INCOMPLETE`/`RUN_FAIL` reason instead of a green run of nothing. Covered
  by `SharedRunFailClosedTest` (typo / zero-selected / missing-meta / duplicate-meta cases).

---

## Phase status

- **Phase C overall: PARTIAL.** C1 infrastructure is implementable and now CI-green (pending the
  final PR-head run); C2 semantic coverage is still PENDING. Do not describe Phase C as complete.
- **READY gate:** READY requires a GREEN GitHub Actions run on the final PR head in addition to
  the local matrix. Until that run is green, the verdict is HOLD.

---

## Files changed

### Original implementation (`32b1ff4`)

```text
scripts/backend_conformance_manifest.py          (new)  closed profiles + evidence table
scripts/tests/test_backend_conformance_manifest.py (new)  pure-data self-test
scripts/verify-backend-conformance.py            (new)  aggregate gate
scripts/verify-external-backend-authority-negative-compile.sh (new)  neg-compile gate
tests/external_backend_admission_test.cpp        (new)  external admission probe
tests/external_backend_authority_negative_probe.cpp (new)  neg-compile probe
tests/backend_conformance.hpp                    (mod)  + profile/mode fields, emit_meta
tests/backend_conformance_driver_test.cpp        (mod)  + [conformance-meta] lines
xmake/tests/async.lua                            (mod)  + external_backend_admission_test
docs/history/closeout/remediation-roadmap.md         (mod)  Phase C: C1 implemented, C2 pending
docs/history/closeout/phase-c1-conformance-gate.md   (new)  this ledger
```

### C1 corrective (implementation-fix head)

```text
scripts/tests/test_backend_conformance_manifest.py (mod)  remove top-level sys.exit; convert to
                                                          unittest.TestCase; add 36-case
                                                          manifest + result-attribution
                                                          isolation regression suite
scripts/verify-backend-conformance.py            (mod)  drive shared suite once PER backend in
                                                          a separate subprocess
                                                          (SLUICE_TEST_FILTER); per-backend
                                                          RunResult store; fail-closed
                                                          mandatory/missing-backend handling
.github/workflows/ci.yml                         (mod)  +3 explicit Phase C1 steps
docs/history/closeout/phase-c1-conformance-gate.md   (mod)  this corrective section + matrices
docs/history/closeout/remediation-roadmap.md         (mod)  C1 corrective note
```

### C1 review-fix (this head)

```text
scripts/tests/test_backend_conformance_manifest.py (mod)  +24 regression cases: SharedRunFailClosed
                                                          (P1: zero/extra selected, missing/
                                                          duplicate/foreign meta, wrong backend/
                                                          profile/mode), MandatoryVerdictPriority
                                                          (P2: one PASS + one MISSING_TARGET ->
                                                          INCOMPLETE, RUN_FAIL precedence,
                                                          uncovered layer, diagnostic-only
                                                          non-mandatory), ProfileModes
scripts/verify-backend-conformance.py            (mod)  INCOMPLETE verdict; _classify_shared_run
                                                          fail-closed per-backend run validation
                                                          (exact [run] selection, exactly-one meta,
                                                          canonical backend, profile, PROFILE_MODES
                                                          mode); verdict priority over mandatory
                                                          evidence; [run] + raw meta parsers
scripts/backend_conformance_manifest.py          (mod)  +PROFILE_MODES closed profile->mode set
tests/harness.hpp                                (mod)  exact case-name filter (no substring);
                                                          zero-match filter exits non-zero instead
                                                          of "ALL TESTS PASSED"
AGENTS.md                                        (mod)  §7: filter contract = exact case name;
                                                          zero-match is an error
scripts/run_test_repeated.sh                     (mod)  header: exact case-name filter; example
                                                          uses a full case name
scripts/verify-runnable-steal-stability.sh       (mod)  header: exact case-name filter
scripts/verify-select-rollback-stability.sh      (mod)  header: exact case-name filter
docs/history/closeout/phase-c1-conformance-gate.md   (mod)  this review-fix section + matrix + residual
                                                          closure
```

### C1 review round 2 (this head)

```text
scripts/verify-backend-conformance.py            (mod)  empty driver_case rejected before any
                                                          subprocess (INCOMPLETE); meta table keyed
                                                          by canonical registered name (removes
                                                          _meta_name_for "(stub)" special case);
                                                          _drive guards self.args like
                                                          _run_shared_suite
scripts/tests/test_backend_conformance_manifest.py (mod)  tautological assertions replaced with
                                                          behavioral checks (not_implemented ->
                                                          INCOMPLETE via gate.run(); unregistered
                                                          BackendEntry -> not ELIGIBLE via
                                                          _backend_verdict); foreign-meta case
                                                          extended to all three backends
scripts/backend_conformance_manifest.py          (mod)  evidence notes: admission probe scope;
                                                          authority positive control
tests/external_backend_admission_test.cpp        (mod)  probe scoped to compile + AsyncIoContext
                                                          ownership (1 case); public claim/publish
                                                          wrappers and direct Completion lifecycle
                                                          assertions removed
tests/external_backend_authority_negative_probe.cpp (mod) positive control exercises derived
                                                          try_claim/publish via private using
                                                          declarations (protected->private guard)
.github/workflows/ci.yml                         (mod)  linux-clang-debug job budget 30 -> 60 min
                                                          (aggregate-gate subprocess timeouts)
docs/history/closeout/phase-c1-conformance-gate.md   (mod)  this round-2 section + matrix + full SHA
                                                          for the implementation-fix head +
                                                          duplicate heading removed
```

No `src/` or `include/sluice/` file was modified in the original implementation, the
corrective, the review-fix, or the round-2 review-fix (strict scope).
