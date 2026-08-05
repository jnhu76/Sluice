# Phase C1 Conformance Gate — Explicit-I/O Backend Conformance Framework

**Roadmap:** [`remediation-roadmap.md`](remediation-roadmap.md) — Phase C (status PARTIAL)
**Governing ADR:** [`ADR-explicit-io-request-contract`](../adr/ADR-explicit-io-request-contract.md) (Accepted)
**DIV-13:** [`divergence-registry.md`](divergence-registry.md) — AsyncBackend public extension point
**Branch:** "test/phase-c-explicit-io-conformance-gate" git branch (not yet merged)
**Scope:** Test/framework only. No `src/` or `include/sluice/` change.

This is the PR-level evidence ledger for Phase C1, the explicit-I/O backend conformance gate.
C1 establishes a reliable, extensible, auditable backend-conformance "ruler"; it does **not**
add semantic coverage (that is C2) and does **not** migrate Uring (that is Phase D). Phase C
remains **PARTIAL**.

---

## SHA provenance (three-head discipline)

Following the PR #64/#65 corrective, evidence SHAs are separated so the ledger does not recurse
(changing a recorded SHA must not change the SHA it records).

- **Validated implementation head:** `32b1ff4a2643834627ae2423ca894a18a52e8c8d`
  — three slices: manifest+profiles (`94dece3`), external admission boundary (`47e207f`),
  aggregate gate (`32b1ff4`). The full change-class matrix below ran at this head.
- **Evidence-recording head:** this documentation commit (records the implementation-head
  results + roadmap update; no production or test-binary change).
- **PR final head / merge commit:** TBD at merge time.

No evidence row below is pre-marked PASS; every PASS row corresponds to a command that actually
ran at the validated implementation head `32b1ff4`.

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
`AsyncIoContext`. It is **not** run through the shared observable-semantics suite and is reported
as `conformance NOT ASSESSED`. The companion negative-compile gate closes the narrower gap that
`AsyncBackend` protected publication helpers (`try_claim` / `publish`) are inaccessible to
non-derived ordinary code.

---

## Validation matrix (ran at implementation head `32b1ff4`)

Evidence is split by class: C++ test, Python infra, negative-compile, repository.

### C++ test evidence

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
| `python3 scripts/tests/test_backend_conformance_manifest.py` | **PASS** | 3 backends, 22 evidence records, 3 profiles; pure-data invariants hold (no xmake grep). |
| `python3 scripts/verify-backend-conformance.py` | **PASS** (exit 0) | Aggregate gate: Fake=ELIGIBLE, ThreadPool=ELIGIBLE, Uring(stub)=NOT CONFORMING, external=admission PASS / conformance NOT ASSESSED. |

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
  line, not from display-name or skip-text parsing; a driver that forgets to emit the meta line
  would surface as `mode=unknown`, which is treated as NOT CONFORMING for KernelIo (fail-closed).
- DIV-14 (descriptor-validation deferral) remains open for Fake/Sync (reference, no real syscall)
  and Uring (Phase D); it is recorded, not a C1 blocker.

---

## Files changed (implementation head `32b1ff4`)

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
docs/architecture/remediation-roadmap.md         (mod)  Phase C: C1 implemented, C2 pending
docs/architecture/phase-c1-conformance-gate.md   (new)  this ledger
```

No `src/` or `include/sluice/` file was modified (strict scope).
