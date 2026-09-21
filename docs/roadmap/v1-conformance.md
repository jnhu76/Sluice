# Sluice v1 Conformance Ledger

**Target authority:** [v1 Architecture and Contract Reference](../explicit-io-v1-final-decision.md), revision v1-r3.
**Starting implementation baseline:** `c64f005e6e59e791f26a7ab4a594c33954f096dd`.
**Purpose:** track implementation and evidence, not add or relax contracts.

This ledger starts a new assessment. Previous architecture CLOSED/CONFORMANT
labels do not transfer. PR #389 changes documentation only; it does not implement
RequestCore, new Request APIs, progress integration, or shutdown behavior.

## Status vocabulary

| Status | Meaning |
|---|---|
| NOT_ASSESSED | No complete assessment against these target requirements recorded |
| GAP | A specific missing/nonconforming behavior has been identified with source evidence |
| IMPLEMENTED_UNVERIFIED | A candidate implementation exists; required evidence is incomplete |
| VERIFIED | Named requirements passed the recorded evidence for specified commits/configurations |
| DEFERRED_OUT_OF_SCOPE | Not a required v1 capability; no supported-product claim |

VERIFIED never means universal correctness. A required v1 item may not use
DEFERRED_OUT_OF_SCOPE. Split a row if only part has evidence. Keep safety,
conditional liveness, memory visibility and kernel evidence separately visible.

## Implementation assessment

| Slice | Root requirements | Status | Required evidence before closure |
|---|---|---|---|
| Shared File semantic oracle | SEM-02, SEM-03, SEM-04, ERR-01 | IMPLEMENTED_UNVERIFIED | See the A1 review record: direct execution and the canonical error table carry evidence; request-side ordering and effect reporting do not |
| Direct composition helpers (read_exact / write_all) | SEM-05 | NOT_ASSESSED | Direct completion helpers that report accumulated confirmed bytes separately from the reason; owner #393 |
| Minimal metadata and same-file identity | SEM-07 | NOT_ASSESSED | Kind, size and explicitly available/unavailable identity; no registry; owner #393 |
| Explicit close and RAII error boundary | SEM-02, LIFE | NOT_ASSESSED | Close consumes ownership on first attempt including error; destructor has no reporting obligation; owner #393 |
| Direct invocation | INV, ARCH, W-01 | NOT_ASSESSED | No request/runtime dependency; File lifetime and consumer tests |
| Admission and slot lifecycle | REQ, BOUND | NOT_ASSESSED | V04–V05, V08–V09, V24, V26; executable model and failure injection |
| Public Request and result lifetime | HANDLE, LIFE | NOT_ASSESSED | Move/consume/discard, retained results, release-build violation behavior |
| Observer attachment and retirement | OBS | NOT_ASSESSED | V06–V08; attach/publication and cancel/delivery interleavings |
| Progress and external integration | PROG, W-03 | NOT_ASSESSED | V10–V12, V23; both backends; no-busy-poll/no-lost-wake evidence |
| Public threading and memory handoff | THREAD, REQ-04 | NOT_ASSESSED | API concurrency matrix, publication review, deterministic handoff and race instrumentation |
| Cancel and effect reporting | CANCEL, ERR-02 | GAP | V13–V15, V19; unsupported/retryable/coalesced control behavior. A1 added reference rules but no request path represents an unaccounted remainder or preserves a count across a cancel |
| ThreadPool profile | BACKEND, PROD-02 | GAP | Full required operation matrix; bounded workers; shared conformance; shutdown. A1 recorded zero-length dispatch, precedence ordering and effect reporting as open |
| io_uring profile | BACKEND, PROD-02 | GAP | Shared conformance; partial-submit/control-CQE faults; real-kernel configuration. A1 recorded the same request-side gaps plus an undisclosed transfer limit and ad-hoc errno classification |
| Bounded pipeline scope | HOST-02, W-02 | NOT_ASSESSED | V18–V19; early return/exception cleanup and scope capacity |
| Shutdown and destruction | SHUT | NOT_ASSESSED | V20–V23; poison/retirement; retained-result and host-detachment checks |
| Core/public build boundary | ARCH, MIG | NOT_ASSESSED | V25; installed headers and standalone core-only consumers |
| Optional stackful host | HOST-01, HOST-03, W-04 | NOT_ASSESSED | If shipped as supported: task failure/stop/unwind, single-owner driver, bounded resources |
| Legacy and compatibility retirement | MIG-02 | NOT_ASSESSED | Consumer audit; replacements usable; old Completion/public surfaces explicitly retired |
| Windows/macOS, multi-worker host, coroutine adapter | PROD-02 | DEFERRED_OUT_OF_SCOPE | Root amendment before supported-v1 claims |
| Registered/vectored/direct-I/O/zero-copy extensions | PROD-03, LIFE-03 | DEFERRED_OUT_OF_SCOPE | Workload and root amendment; no inheritance from an existing header |

NOT_ASSESSED does not imply the baseline has no useful implementation or evidence.
It prevents partial or older evidence from silently becoming certification of
the new target. The implementing PR records the precise source/evidence.

A row moves off NOT_ASSESSED only through a recorded review below. A GAP row
means a specific nonconforming behavior was identified with source evidence; it
does not mean the whole slice is unimplemented.

## Evidence record template

For each reviewed slice, record:

| Field | Required content |
|---|---|
| Requirement scope | Exact IDs and accepted workload; explicit exclusions |
| Implementation | Commit SHA, code paths and public API/configuration |
| Change | Current behavior, target behavior and compatibility impact |
| Semantic/regression evidence | Test names/commands, result and output artifact or durable CI link |
| Protocol/model evidence | Model-to-C++ mapping, explored bounds, safety/liveness properties and assumptions |
| Publication/thread evidence | Synchronization argument and relevant instrumentation/configuration |
| Backend/kernel evidence | Backend, compiler, kernel/liburing, filesystem, injected faults and limitations |
| Status | New status and remaining gaps; never close a required slice with skipped tests |

## Phase gates

Use MIG-01 for the normative sequence and VERIFY-04 for named counterexamples.
Implement evidence with the changed protocol. Phase B already includes shutdown
and observer-reference obligations even though full integration closes later.

1. A: semantic oracle and direct path.
2. B: core, handles, publication/reclaim and teardown skeleton.
3. C: host-neutral observation and persistent progress integration.
4. D: bounded scope and optional host cleanup.
5. E: required operations, cancellation, both backend profiles and shutdown closure.
6. F: compatibility/public-surface retirement after consumer migration.
7. G: measured optimization preserving verified obligations.

Release evidence must enumerate actual supported configurations and pass W-01,
W-02, W-03, and W-04 for any supported optional host. This file is intentionally
not a claim that those gates have already passed.

## A1 review record — Issue #392, shared File semantic oracle

This record uses two axes, and they must not be conflated:

- The **assessment table** above uses the ledger status vocabulary.
- The **gap map** below uses the A1 slice's own coverage vocabulary:
  `CONFORMING` (this slice verified that rule on that path with the cited
  evidence), `GAP` (a specific divergence, with an owner), `IMPLEMENTED_UNVERIFIED`
  (a candidate or reference exists, evidence incomplete), `OUT_OF_SCOPE`.

`OUT_OF_SCOPE` in the gap map means *this slice did not own that requirement*, not
that v1 does not require it. A required capability is never recorded as
DEFERRED_OUT_OF_SCOPE anywhere in this file; where a required capability is simply
not assessed yet, the gap map points at its assessment-table row instead.

| Field | Content |
|---|---|
| Requirement scope | SEM-02 open rules; SEM-03 access matrix and validation precedence steps 1–4; SEM-04 offset/length/range; SEM-05 primitive and composition rules; SEM-06 durability rules; ERR-01 canonical mapping; ERR-02 effect representation. Excludes SEM-01 operation matrix closure, SEM-07 metadata/identity, direct composition helpers, and every request-side responsibility |
| Implementation | `include/sluice/detail/file_semantics.hpp` is the single semantic authority. `src/file_resource.cpp`, `src/blocking_file.cpp`, `src/async/file.cpp`, `src/async/async_io_context.cpp`, `src/async/threadpool_backend.cpp` and `src/async/uring_backend.cpp` call the validation rules and the error mapping; none of them re-derives File legality. The composition, effect and durability rules have no production consumer in this commit |
| Change | `ENOENT`/`ENOTDIR` now map to `not_found` (was `permission_denied`). Embedded NUL is rejected before the native open (was silently truncated). An unrepresentable offset is `invalid_argument` (was `invalid_state` for the legacy reader). The last addressed byte is validated without overflow and a transfer above the native count is an invalid range (io_uring had no such check). `detail/io_validation.hpp` became `detail/uring_submit.hpp` because its remaining content is io_uring mechanism, not File validation. Review corrections: ECANCELED falls back to `backend_error` per ERR-01, since the root assigns it no canonical category and the semantic `canceled` outcome comes only from CANCEL-01 dispositions; buffer presence is no longer claimed as an SEM-03 rule and each raw-pointer surface keeps its own null-buffer fail-fast; effect certainty is derived from dispatch evidence rather than read/write direction; durability supersession requires strict ordering plus a conflicting mutation |
| Semantic/regression evidence | `semantic_errno_mapping_test` (14 table cases, table consistency, real `ENOENT`/`ENOTDIR`/`EACCES` failures), `semantic_open_test` (18-combination legality table, oracle-versus-real-open cross-check, NUL rejected pre-open), `semantic_range_test` (13 range cases cross-checked against wide-arithmetic restatement), `semantic_short_io_reference_test`, `semantic_effect_outcome_test`, `semantic_durability_reference_test`, `semantic_validation_precedence_test` (21 scenarios), `semantic_reference_case_test` (V01–V03, V15–V17, V27) |
| Protocol/model evidence | None claimed. A1 is a table/property slice; the request lifecycle model belongs to Phase B |
| Publication/thread evidence | None claimed. This slice does not change publication or threading |
| Backend/kernel evidence | Direct, ThreadPool and real-kernel io_uring (`--liburing=y`, 8-case smoke passing) are compared against the same scenario table. io_uring unavailability is reported NOT RUN, never as a pass. `/dev/full` is a fault-injection device outside the PROD-02 regular-file domain and is used only to produce a deterministic write failure |
| Status | Shared File semantic oracle: IMPLEMENTED_UNVERIFIED. Validation rules and the error mapping are called by every execution path; the composition, effect and durability rules are a reference model with no production consumer yet, and their tests prove properties of that model rather than of a path. Direct path: CONFORMING for the tested rules and scenarios below, except the zero-op no-OS-call half of SEM-03 step 3, which stays IMPLEMENTED_UNVERIFIED (inspection-only evidence) until #393; the direct scope as a whole is therefore not CONFORMING. Request paths: GAP, see the map |

### A1 baseline gap map

Rule/scenario | Current path | Observed behavior | v1 expected | Status | Owner
---|---|---|---|---|---
Open combination legality, defaults, embedded NUL | `File::open` | `invalid_argument` before the native open; NUL no longer truncates the name | SEM-02 | CONFORMING | —
Access matrix and precedence steps 1–4 | `sluice::blocking::*` | All 21 drivable scenarios match the oracle | SEM-03, SEM-04 | CONFORMING | —
Offset/length range, last-byte overflow | canonical File, direct syscalls | Unrepresentable offset and an overflowing last byte are `invalid_argument`; above-native transfer is a pure rule with no drivable buffer | SEM-04 | CONFORMING | —
Native error classification | `from_errno_value` | Single table; `ENOENT`/`ENOTDIR`→`not_found`, `EACCES`/`EPERM`→`permission_denied`, ECANCELED deliberately unmapped→`backend_error`, native detail preserved | ERR-01 | CONFORMING | —
Primitive EOF / short transfer / zero-length | direct syscalls | Matches `classify_primitive` on real files | SEM-05 | CONFORMING | —
Exact/all composition | direct | No direct composition surface exists | SEM-05 | OUT_OF_SCOPE | #393
`file_info` / size / same-file identity | canonical File | Only `size` via `fstat`; no `FileInfo` | SEM-07 | OUT_OF_SCOPE | #393
Explicit close, destructor, move-assignment error boundary | canonical File | Not audited against SEM-02 close rules | SEM-02 | OUT_OF_SCOPE | #393
Zero-length request | ThreadPool, io_uring | Accepted, but not published at acceptance (measured): the terminal is not `ready()` until a progress call, so the zero-length data call is dispatched rather than completed at acceptance. The dispatch itself is inferred from the code path, not observed | SEM-03: never dispatches a data syscall | GAP | #400
Zero-length direct call | `sluice::blocking::*` | Returns 0 without consulting the range rule, proven by the zero-length-plus-unrepresentable-offset scenario. Whether it makes an OS call is not externally observable and is asserted by inspection only | SEM-03: no OS call | IMPLEMENTED_UNVERIFIED | #393
Precedence step 4 (range) vs step 7 (capacity) | both request backends | `submit_transaction` reserves a slot before the backend validates, so a full table reports `would_block` for a request with an invalid range | SEM-03 | GAP | #394
Precedence step 4 (range) vs steps 5–6 (health/support) | both request backends | `stage0_precheck` runs before validation, so admission-closed or a missing ring outranks an invalid range | SEM-03 | GAP | #394
Partial effect on failure | both request backends | `TerminalResult::err()` writes `bytes = 0` and the terminal has no effect-certainty channel, so an error plus confirmed bytes or an unaccounted remainder is not representable | ERR-02, V15 | GAP | #400
Cancel racing a completed count | both request backends | `record_canceled` stores `err(canceled)` with no count, so a raced success is erasable | ERR-02, CANCEL-01 | GAP | #400
Zero-progress write code | `op_helpers`, `await_op_helpers` | Three different codes across composition helpers (`invalid_state`, `backend_error`) and no confirmed-byte reporting | SEM-05 | GAP | #400
Durability coverage of a completed resize | direct resize + `sync_data`/`sync_all` | Sequence succeeds and the size change is observable; no request-path evidence exists | SEM-06, V27 | IMPLEMENTED_UNVERIFIED | #400
Submitted-then-sync ordering | request paths | No API reports coverage, so V16 cannot be exercised end to end | SEM-06, V16 | IMPLEMENTED_UNVERIFIED | #400
EINTR retry | direct syscalls | `retry_on_eintr` exists and shares one helper; no test drives it | SEM-05 | IMPLEMENTED_UNVERIFIED | #393
Undisclosed transfer limit | io_uring | A request longer than the SQE count is lowered to a short count without disclosing the limit | BACKEND-02 capability disclosure | GAP | #400
Ad-hoc errno classification outside the table | io_uring submit path, io_uring wait source | `EINTR`/`EAGAIN`/`EBUSY` compared locally next to shared classification | ERR-01 | GAP | #400
Closed-operation error | legacy `Reader`/`Writer`/`FileReader`/`FileWriter` | Fabricates `permission_denied` (no native detail) where the oracle requires `invalid_state` | SEM-03 step 1 | GAP | #402
Access legality, composition codes, confirmed bytes | legacy reader/writer, buffered layer, copy | No canonical access check; `read_exact`/`write_all` return no confirmed prefix and stop with locally chosen codes | SEM-03, SEM-05 | GAP | #402
Closed descriptor error code, copied open flags | `src/experimental/*` | Returns `permission_denied`; duplicates the writer flag lowering; in no build target | SEM-02, SEM-03 | OUT_OF_SCOPE | #402
Unused submit-classification helpers | `detail/uring_submit.hpp` | `classify_uring_submit`/`UringSubmitProgress` have no call site | — | OUT_OF_SCOPE | #402
Code-to-name table for CLI output | `apps/sluice-copy/cli_parse.cpp` | Second presentation table with a `default` fallback; not a semantic authority | — | OUT_OF_SCOPE | App surface
Task exception to terminal error | `include/sluice/async/task_result.hpp` | A `std::system_error` now routes through the shared mapping, so an `ENOENT` there is `not_found`. `std::bad_alloc` still becomes `no_space`, which ERR-01 does not assign; recorded as an open item below | ERR-01 | CONFORMING (mapping) / GAP (`bad_alloc` category) | #399
Fabricated not-found substitute | `include/sluice/memory_io_context.hpp` | A seeded-path miss returns `permission_denied` with no native detail | ERR-01 | GAP | #402

### A1 open items recorded rather than decided

- SEM-05 requires composition to stop with a "no-progress failure" without naming
  a canonical category. The oracle reuses `invalid_state` as the closest existing
  category and adds no new category. The root does not specify this mapping, so
  it is recorded here as an ambiguity rather than treated as settled.
- Buffer presence is not an SEM-03 rule: SEM-03 treats caller memory validity as
  not generally dynamically detectable. Each raw-pointer surface keeps a
  null-buffer-with-nonzero-length fail-fast as its own implementation
  precondition, outside the shared contract; a surface whose interface takes
  spans does not need one, and native-to-semantic refinement of such cases
  belongs to #400.
- An allocation failure (`std::bad_alloc`) reaching the host boundary has no
  canonical category. `no_space` is used as the closest existing one, but ERR-01
  describes `no_space` for the filesystem and no requirement assigns this case,
  so the mapping is recorded rather than treated as settled.
- The oracle rejects only what is not natively representable. A filesystem may
  still refuse an accepted offset near the native maximum; that refusal is an
  operation result with native detail, not a validation rejection.
- V16 and the request-side half of V15 and V27 need request-path capability
  (an unobserved write followed by a sync, and a terminal that can carry an
  unaccounted remainder). Their rules are asserted; their production evidence is
  owned by #400 and is not claimed here.
