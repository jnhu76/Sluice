# Sluice v1 Conformance Ledger

**Target authority:** [v1 Architecture and Contract Reference](../explicit-io-v1-final-decision.md), revision v1-r3.
**Starting implementation baseline:** `c64f005e6e59e791f26a7ab4a594c33954f096dd`.
**Purpose:** track implementation and evidence, not add or relax contracts.

This ledger starts a new assessment. Previous architecture CLOSED/CONFORMANT
labels do not transfer. PR #389 changes documentation only; it does not implement
RequestCore, new Request APIs, progress integration, or shutdown behavior.

Formal evidence is governed by the
[v1 formal evidence map](v1-formal-evidence-map.md) (issue #391): existing
Lean/TLA+ assets are HISTORICAL / ADAPT / RETIRE_FROM_V1_EVIDENCE and none of
them is v1 evidence; future protocol and liveness closures must record their
evidence in the map's closure format. No row below changes status because an
old proof exists.

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
| Direct composition helpers (read_exact / write_all) | SEM-05, ERR-02 | VERIFIED | A2 review record: direct loops consume the shared composition rule; injected short/zero/error counts, confirmed prefix, structured stop reasons. The outcome publishes the failed attempt's effect certainty (`accounted` / `unknown`) from the shared rule, so ERR-02's distinction is observable on this path; the request-side representation stays owned by #400 |
| Minimal metadata and same-file identity | SEM-07 | VERIFIED | A2 review record: kind, size and explicitly available/unavailable identity on the direct path, no registry. Request-side file_info/size stays unassessed and owned by #400 |
| Explicit close and RAII error boundary | SEM-02, LIFE | VERIFIED | A2 review record: close consumes ownership on the first attempt including error; destructor and move-assignment fault injection |
| Direct invocation | INV, ARCH, W-01 | VERIFIED | A2 review record: W-01 explicit-close and RAII traces as a core-only consumer. Request-side invocation is unchanged and unassessed here |
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
| Core-only consumer build boundary | ARCH, MIG | VERIFIED | A2 review record: a standalone core-only consumer compiles, links and runs; no async definition reachable, no liburing linked |
| Installed-header / package surface | ARCH, MIG | NOT_ASSESSED | No install or package target exists in this commit, so no installed surface can be audited; the audit belongs to the legacy/retirement slice (#402) |
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
| Status | Shared File semantic oracle: IMPLEMENTED_UNVERIFIED. Validation rules and the error mapping are called by every execution path; the composition, effect and durability rules are a reference model with no production consumer yet, and their tests prove properties of that model rather than of a path. Direct path: CONFORMING for the tested rules and scenarios below, except the zero-op no-OS-call half of SEM-03 step 3, which stays IMPLEMENTED_UNVERIFIED (inspection-only evidence) until #393; the direct scope as a whole is therefore not CONFORMING. Request paths: GAP, see the map. **[A2 update, later commit]** the composition rules now have a production consumer and the zero-op no-OS-call half is CONFORMING; see the A2 review record below. The effect and durability rules remain a reference model |

### A1 baseline gap map

Rule/scenario | Current path | Observed behavior | v1 expected | Status | Owner
---|---|---|---|---|---
Open combination legality, defaults, embedded NUL | `File::open` | `invalid_argument` before the native open; NUL no longer truncates the name | SEM-02 | CONFORMING | —
Access matrix and precedence steps 1–4 | `sluice::blocking::*` | All 21 drivable scenarios match the oracle | SEM-03, SEM-04 | CONFORMING | —
Offset/length range, last-byte overflow | canonical File, direct syscalls | Unrepresentable offset and an overflowing last byte are `invalid_argument`; above-native transfer is a pure rule with no drivable buffer | SEM-04 | CONFORMING | —
Native error classification | `from_errno_value` | Single table; `ENOENT`/`ENOTDIR`→`not_found`, `EACCES`/`EPERM`→`permission_denied`, ECANCELED deliberately unmapped→`backend_error`, native detail preserved | ERR-01 | CONFORMING | —
Primitive EOF / short transfer / zero-length | direct syscalls | Matches `classify_primitive` on real files; a scripted short write is returned as a short success and no further native call is made for the remaining bytes | SEM-05 | CONFORMING (A2) | —
Exact/all composition | direct | Direct `read_exact`/`write_all` (shared cursor and positional) loop over the shared composition rule and report the confirmed prefix next to a structured stop reason | SEM-05 | CONFORMING (A2) | —
`file_info` / size / same-file identity | canonical File | `file_info` reports kind, observed size and an explicitly available/unavailable identity; `size` is the projection of its own observation rather than a second metadata rule, with no snapshot across separate calls; comparison is value-level with an explicit unknown | SEM-07 | CONFORMING (direct, A2) | —
Explicit close, destructor, move-assignment error boundary | canonical File | First native close attempt consumes ownership including on error, no retry after EINTR, destructor is a noexcept single attempt, move assignment best-effort closes the old resource | SEM-02 | CONFORMING (A2) | —
Zero-length request | ThreadPool, io_uring | Accepted, but not published at acceptance (measured): the terminal is not `ready()` until a progress call, so the zero-length data call is dispatched rather than completed at acceptance. The dispatch itself is inferred from the code path, not observed | SEM-03: never dispatches a data syscall | GAP | #400
Zero-length direct call | `sluice::blocking::*` | Returns 0 with zero intercepted native transfer calls under the test-only seam, on the primitives and on both composition surfaces | SEM-03: no OS call | CONFORMING (A2) | —
Precedence step 4 (range) vs step 7 (capacity) | both request backends | `submit_transaction` reserves a slot before the backend validates, so a full table reports `would_block` for a request with an invalid range | SEM-03 | GAP | #394
Precedence step 4 (range) vs steps 5–6 (health/support) | both request backends | `stage0_precheck` runs before validation, so admission-closed or a missing ring outranks an invalid range | SEM-03 | GAP | #394
Partial effect on failure | both request backends | `TerminalResult::err()` writes `bytes = 0` and the terminal has no effect-certainty channel, so an error plus confirmed bytes or an unaccounted remainder is not representable | ERR-02, V15 | GAP | #400
Cancel racing a completed count | both request backends | `record_canceled` stores `err(canceled)` with no count, so a raced success is erasable | ERR-02, CANCEL-01 | GAP | #400
Zero-progress write code | `op_helpers`, `await_op_helpers` | Three different codes across composition helpers (`invalid_state`, `backend_error`) and no confirmed-byte reporting | SEM-05 | GAP | #400
Durability coverage of a completed resize | direct resize + `sync_data`/`sync_all` | Sequence succeeds and the size change is observable; no request-path evidence exists | SEM-06, V27 | IMPLEMENTED_UNVERIFIED | #400
Submitted-then-sync ordering | request paths | No API reports coverage, so V16 cannot be exercised end to end | SEM-06, V16 | IMPLEMENTED_UNVERIFIED | #400
EINTR retry | direct syscalls | A scripted `EINTR` attempt with no completed count is retried and the following count is returned, in both directions; a successful short count ends the primitive, so on these direct surfaces further calls for the remainder come only from the exact/all composition; `close` is never retried, and its `EINTR` consumes ownership | SEM-05 | CONFORMING (A2) | —
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
  **[A2 update, later commit]** that reuse is a projection of the canonical stop
  state rather than the composition authority; the public direct outcome does not
  adopt it. See the A2 review record.
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

## A2 review record — Issue #393, direct File closure and W-01

This record uses the assessment table's status vocabulary for the rows it
updates, and the A1 gap-map vocabulary for the A1 rows it closes.

| Field | Content |
|---|---|
| Requirement scope | SEM-01 direct column (open/close, positional and shared-cursor I/O, sync, file_info/size, resize, read_exact/write_all); SEM-02 close/RAII/ownership; SEM-04 cursor and offset rules; SEM-05 composition; SEM-06 durability as a direct integration sequence; SEM-07 metadata and identity; ERR-02 confirmed progress and effect certainty (`CompositionOutcome::remaining`, decided by the shared rule); INV-01 direct selection; ARCH-02 File ownership; W-01; VERIFY-01 build/public boundary (V25); VERIFY-04 V01–V03 and V27 direct halves. Excludes every request-side responsibility: request `file_info`/`size`/`resize`/composition forms, request effect-certainty representation, backends, RequestCore, observer, progress, cancellation, shutdown |
| Implementation | `include/sluice/file_resource.hpp` (`FileKind`, `FileIdentity`, `FileInfo`, `IdentityMatch`, `identity_match`, close/RAII contract comments); `include/sluice/blocking/file.hpp` (`file_info`, `CompositionEnd`, `EffectCertainty`, `CompositionOutcome`, `read_exact(_at)`, `write_all(_at)`); `src/blocking_file.cpp` (one `fstat` metadata observation with `size` as the projection of its own observation; the composition loop consuming `detail::compose_progress`/`compose_error` and publishing `detail::composition_effect_certainty`); `src/file_resource.cpp` (`close` routed through the test-only native-call seam); `src/file_test_seams.hpp` (bounded allocation-free native-call script, family- and descriptor-filtered, compiled only under `SLUICE_FILE_INTERNAL_TESTING`); `xmake/tests.lua` (four core-only targets and two seam targets that compile their own copies of the direct TUs and link no other core object); `src/file.cpp` only follows the seam's new signature at its legacy `FileReader`/`FileWriter` call site, and no target compiles that TU with the macro, so those legacy close paths remain un-fault-injected |
| Change | Direct path gains minimal metadata/identity and the exact/all composition surfaces. The composition outcome also gains the ERR-02 effect certainty (`CompositionOutcome::remaining`), which the direct path previously left unrepresented, so a failed attempt's possibly-effective remainder is reportable instead of being claimed away. `File::close` already consumed ownership on the first attempt before its native call; the change routes that call through the seam so the rule is fault-injectable and unchanged in behavior. No public behavior was removed; the new `size` is the projection of `file_info`, so the previous `size` behavior is preserved. Legacy `Reader`/`Writer`/`FileReader`/`FileWriter` composition codes are untouched and stay with #402 |
| Semantic/regression evidence | `file_close_semantics_test` (8 cases: unarmed release, success, EINTR, other error, destructor, move assignment, descriptor-reuse, closed no-op), `direct_composition_fault_test` (14 cases: short reads/writes, EOF-before-full prefix, zero-progress stop after one attempt, error-after-prefix with `no_space`/ENOSPC, a scripted short primitive write returned as a short success with one intercepted call carrying the original buffer, count and offset, primitive EINTR retry in both directions — an interrupted attempt with no completed count is repeated and the following count is returned, and the first successful short count ends the primitive, so the remaining bytes are not retried by it — zero-length with zero intercepted calls, impossible count, buffer/offset advance per confirmed bytes recorded at the native-call boundary, and the five canonical stop reasons pinned side by side with the reference `IoError` projection (`detail::composition_error`), which also pins effect certainty at every stop, a primitive error reporting an unaccounted remainder in both directions, plus the frozen 21-scenario table driven through both composition surfaces: positional 16 compared / 0 divergences, shared cursor 11 compared / 0 divergences, 5 state-operation scenarios not drivable and 5 scenarios whose verdict needs a caller-supplied offset not expressible on the cursor surface), `file_info_identity_test` (7 cases: real fstat kind/size/identity, per-observation size projection, same/different file, unavailable-at-value-level unknown, closed invalid_state, other kind, cursor non-interference), `direct_cursor_position_test` (5 cases: positional vs cursor write, native duplication shares the cursor, independent opens, resize does not move the cursor, zero-length no-op), `direct_composition_test` (7 cases: real-file full read/write, real EOF-before-full prefix, positional placement, cursor advancement, empty request, rejection precedence), `direct_w01_consumer_probe` (3 cases: explicit-close W-01 trace, RAII trace, same-file comparison). The composition tests were checked against mutation: dropping the buffer advance (`subspan(confirmed)` → `subspan(0)`), dropping the offset advance (`offset + confirmed` → `offset`), making the direct stop adopt the reference rule's `eof` reason, dropping the `impossible_count` reason, and rotating the reference projection's `write_no_progress` category each fail `direct_composition_fault_test`, as do the two certainty mutations (the adapter claiming `accounted` for every stop, and claiming `unknown` for every stop). A primitive-retries-the-remainder mutation of `write_at` (looping until the requested length is satisfied) fails both primitive write cases in `direct_composition_fault_test` (checked case by case; the shared runner stops at the first failing case) while the ordinary real-file `blocking_file_write_test` still passes under it, which is why the primitive boundary is pinned at the native-call seam rather than on a real file |
| Protocol/model evidence | None claimed. This slice has no protocol; the request lifecycle model remains Phase B |
| Publication/thread evidence | None claimed. THREAD-01's File row (caller serialization, no outstanding borrow) is unchanged and not exercised here; no concurrency claim is added. A caller that closes or mutates the File while a direct call is in flight violates that row's serialization, so it is outside the contract rather than a defined outcome a call can report. LIFE-01's borrow rule is the reason the direct path adds no reference counting or deferred close: a direct call completes before it returns, so there is no outstanding borrow to track, and enforcement over a borrowed resource belongs to the request path (#394/#395). LIFE-02's `native_handle()` borrow is unchanged |
| Backend/kernel evidence | Direct Linux syscalls only (`pread`/`pwrite`, `read`/`write`, `fstat`, `ftruncate`, `fdatasync`/`fsync`). Close and transfer fault injection use the test-only seam at the native-call boundary, so the counts and reasons are deterministic while the production build contains no seam state. Configuration matrix: debug with liburing disabled (34/34), debug with `--liburing=y` (37/37, real-kernel smoke included), release with liburing disabled (34/34), targeted ASan+UBSan over the six new binaries (all pass). The project-wide `asanubsan` mode does not build in this environment: `src/async/fiber_ctx.cpp` fails on a sanitizer attribute directive under `-Werror`, a pre-existing condition in an untouched TU |
| Build boundary | `direct_w01_consumer_probe` includes only the two direct headers, declares (without including) `sluice::async::{AsyncIoContext, AsyncBackend, Scheduler, Fiber, ApplicationRuntime}` and `detail::RequestArena` and asserts each is incomplete, and links as `g++ … -lsluice_core` with no async or liburing library; `ldd` shows libc/libstdc++/libgcc/libm only. `rg -n 'async|scheduler|fiber|completion|request_arena|request_handle|runtime_task|ApplicationRuntime' include/sluice/file_resource.hpp include/sluice/blocking` returns no match |
| Status | Direct composition helpers: VERIFIED, ERR-02 included — the outcome publishes the failed attempt's effect certainty (`accounted` / `unknown`) from the shared rule, so the direct path represents the unaccounted remainder rather than arguing it away; the request-side representation stays owned by #400. Minimal metadata and same-file identity: VERIFIED on the direct path; the request-side forms remain unassessed and owned by #400. Explicit close and RAII error boundary: VERIFIED. Direct invocation / W-01: VERIFIED. Core-only consumer build boundary (V25): VERIFIED. Installed-header surface: NOT_ASSESSED (no install target exists). A1 rows closed by this slice: exact/all composition, `file_info`/size/identity, close/destructor/move-assignment boundary, zero-length no-OS-call, EINTR retry |

### A2 limitations and recorded boundaries

- **No-progress representation and the composition authority.** The authority
  for a stop is the shared state itself — `CompositionStop`, the accumulated
  `confirmed_bytes` and the primitive's own error — decided once by
  `detail::compose_progress`/`compose_error` and published by each path as its
  own representation. `detail::composition_error` is one such representation: a
  reference `IoError` projection with test consumers and no production consumer,
  not a second authority. The direct outcome carries a structured
  `write_no_progress` stop instead of adopting that projection's `invalid_state`,
  so the A1-recorded SEM-05 ambiguity is not canonized in the public contract.
  The projection keeps its reference-model choice, and a root decision on the
  category would move the projection without moving the canonical stop state.
  Because the two representations differ over two stop reasons, they are driven
  side by side from the same injected primitive sequence by
  `every_stop_reason_is_pinned_against_the_oracle_error_rule`, which pins the
  canonical stop state on both sides and records the split: `complete`,
  `primitive_error` (reason and native detail identical) and `impossible_count`
  agree, while `eof_before_full` and `write_no_progress` differ only in
  representation, the direct side reporting no `IoError` where the reference
  projection names one.
- **Effect certainty (ERR-02; SEM-05's OS-error row).** `CompositionOutcome`
  carries `remaining`, and `detail::composition_effect_certainty` decides it from
  the shared state: a completed, EOF-before-full or no-progress stop accounts for
  its remainder, while a primitive error and an impossible count report
  `unknown`, in both directions. Direction is deliberately not the
  discriminator, for the reason `failed_dispatched_attempt` already records — a
  failing read may have filled part of the caller's borrowed buffer (LIFE-01),
  and a failing write may already have reached the file. The write side has a
  concrete Linux counterexample: `write(2)` documents an error reported for a
  writeback failure, and the NFS write path (`nfs_file_write`, `fs/nfs/file.c`)
  returns a negative result after `generic_perform_write()` accepted bytes —
  either through `generic_write_sync` or through the writeback-error check that
  overwrites a positive result. The earlier claim recorded here, that a `-1`
  return from a direct regular-file primitive proves the attempt transferred
  nothing, is therefore withdrawn as too broad: v1 does not exclude network
  filesystems from the regular-file domain. The accounted side is limited to
  stops whose primitive reported a successful count. A direction-sensitive
  refinement (a failing read accounting for itself) is deliberately not claimed,
  because it would need a per-platform primitive contract with evidence rather
  than another assumption. With the field present the direct outcome is no longer
  a prefix-only terminal in the sense of `detail::prefix_only_terminal_can_carry`;
  the narrower shape that predicate names stays the request-side terminal. The
  field is pinned at every stop, and in both directions, by
  `direct_composition_fault_test`.
- **Cursor-surface range scenarios.** A shared-cursor call supplies no offset, so
  the offset-range step has no operand there: five scenarios whose verdict
  depends on the caller's offset (two range cases, two closed/access cases and
  one accepted-past-EOF case) are reported as not drivable on that surface. The
  length half of the range rule is covered on both surfaces.
- **Durability.** The direct integration sequence (completed write, resize grow
  and resize shrink, each followed by `sync_data`/`sync_all`) is evidence of the
  observable half only. It is not a power-loss durability proof, and the
  request-side half of the V27 row keeps its #400 owner.
- **Durability coverage of a completed resize (A1 row).** Its direct half is now
  CONFORMING; the row keeps its `IMPLEMENTED_UNVERIFIED` status for the
  request-path evidence owned by #400.
- **Open A1 items untouched.** The `bad_alloc` category (#399), the request-side
  effect/cancel representation, and the undisclosed io_uring transfer limit
  remain as recorded; this slice does not change root, error categories or
  request paths.
