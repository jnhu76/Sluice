# AGENTS.md — Sluice Repository Operating Contract

This file is the repository-wide operating contract for coding agents working on Sluice.

It applies to the entire repository unless a more specific nested `AGENTS.md` exists for a
subdirectory. A nested file may add stricter local rules, but it MUST NOT silently weaken an
Accepted ADR, the architecture constitution, public API contract, or the explicit-I/O lifecycle
defined here.

Normative words such as **MUST**, **MUST NOT**, **SHOULD**, and **MAY** are intentional.

---

## 1. Project identity and architectural boundaries

Sluice is an experimental C++20 I/O control-flow library built around:

- explicit capability objects;
- backend-neutral public I/O descriptors;
- stable request identity;
- bounded resource ownership;
- pluggable execution backends;
- synchronous `Reader` / `Writer` semantics in the core; and
- an opt-in asynchronous runtime in `namespace sluice::async`.

The main build boundaries are:

- `sluice_core`
  - synchronous core;
  - owns `Result<T>`, `IoError`, `Reader`, `Writer`, buffering, copy helpers, WAL,
    positional I/O, durability, file I/O, and the production `BlockingIoPool`;
- `sluice_async`
  - opt-in asynchronous production library;
  - owns `AsyncIoContext`, `Completion<T>`, async backends, Runtime/Scheduler integration,
    and the explicit request lifecycle;
- `sluice_async_internal_testing`
  - test-only build of the authoritative async production sources plus guarded deterministic
    controls;
  - production code MUST NOT depend on this target;
  - no executable may link both async variants;
- `sluice_experimental_uring`
  - optional io_uring support;
  - off by default;
  - io_uring is one backend, not the architecture;
- `zig/`
  - source-derived design reference only;
  - not built, linked, vendored into production output, or copied mechanically.

Do not collapse these boundaries.

In particular:

- `BlockingIoContext` remains the default blocking path.
- `BlockingIoPool` is a bounded synchronous-core helper, not the async Runtime.
- `ThreadPoolBackend` is a blocking-I/O offload mechanism, not a task-execution strategy.
- Scheduler workers, blocking-I/O workers, kernel queue depth, request capacity, and application
  pipeline depth are distinct resources.
- Public synchronous `Reader` / `Writer` semantics remain synchronous.
- Async implementation convenience is not permission to alter the synchronous public contract.
- Test seams remain compile-time guarded and non-authoritative.

---

## 2. Current explicit-I/O architecture status

The Accepted explicit-I/O request contract defines the current target architecture.

The core logical request identity is:

```text
RequestKey = (ContextIdentity, SlotIndex, Generation)
```

The canonical lifecycle is:

```text
free
  -> reserved
  -> prepared
  -> pending
  -> enqueued
  -> running / kernel-owned
  -> backend-ready
  -> completion-ready
  -> free with generation increment
```

The five-stage submission transaction is:

```text
reserve
-> prepare
-> commit / accept
-> enqueue
-> dispatch
```

Current migration status:

- `FakeAsyncBackend` and `SyncBackend` use the bounded `RequestArena` / `RequestSlot` model.
- `ThreadPoolBackend` has completed the bounded persistent-worker migration (Phase E) and
  conforms with generation-safe explicit identity, bounded accepted-terminal storage, and the
  unified RequestArena lifecycle.
- `UringAsyncBackend` may temporarily retain legacy paths only where the
  active roadmap, ADR, or divergence registry explicitly records that staging.
- A backend still on a legacy path MUST NOT claim conformance with generation-safe explicit
  identity, bounded accepted-terminal storage, or the unified RequestArena lifecycle.
- `RequestKey` is currently an internal identity. The public API may remain Completion-based until
  a later approved API ADR introduces a public request handle.
- `Completion<T>` is a caller-owned publication and result object. It is not the logical identity
  of an accepted request.

Do not describe the repository as fully end-to-end migrated while a production backend or upper
layer still reconstructs identity from pointers, scans, or side-band containers.

---

## 3. Authority and conflict resolution

Before changing a subsystem, identify its authority chain.

Use the following order:

1. The explicit current task, approved issue scope, accepted review finding, or approved plan.
2. Accepted ADRs and active subsystem design/closeout documents under `docs/`.
3. `docs/architecture/architecture-constitution.md`.
4. This `AGENTS.md`.
5. Public headers under `include/sluice/` and `docs/reference/api.md`.
6. Production implementation under `src/`.
7. Contract, regression, death, negative-compile, causal, and formal tests.
8. `xmake.lua` for target membership, dependencies, feature gates, and test names.
9. `.github/workflows/*.yml` for repository merge gates.
10. `README.md` for orientation and common commands.

A scanner report, code-review summary, comment, commit message, or stale planning note is evidence,
not automatic authority.

If sources disagree:

- MUST NOT silently choose whichever is easiest;
- characterize the as-built behavior with a focused test or precise static argument;
- identify which higher authority is stale or violated;
- make the smallest change that restores an explicitly approved contract;
- update interface comments, implementation, tests, and governing docs together;
- record intentional divergence instead of hiding it;
- add a superseding ADR or closeout note when semantics deliberately change.

An Accepted ADR is not rewritten to pretend a historical decision never existed.

---

## 4. Explicit authority separation

### 4.1 RequestArena / RequestSlot authority

The `RequestArena` / `RequestSlot` lifecycle domain owns accepted-I/O request semantics:

- stable `RequestKey`;
- request provenance and generation;
- `RequestState`;
- slot admission and release;
- enqueue-in-flight pin;
- operation kind and bounded backend scratch;
- fd/buffer borrow metadata;
- accepted-outstanding and slot-in-use accounting;
- terminal-winner arbitration;
- terminal result storage;
- backend-ready linkage;
- waiter registration state stored in the request slot;
- reap transition to completion-ready; and
- generation increment before slot reuse.

The slot-lifecycle domain is the authority for the request, not a parallel map, closure, ready
deque, Completion pointer, Scheduler record, or worker-local object.

### 4.2 Backend progress authority

A concrete backend owns its bounded progress mechanism, such as:

- a fixed worker pool and bounded dispatch queue;
- io_uring SQ/CQ ownership;
- backend-specific cancellation attempts;
- backend progress notification; and
- the mapping from a valid `RequestKey` to execution ownership.

A backend worker or CQE handler may publish only `backend-ready` through the request lifecycle.
It MUST NOT make a `Completion<T>` ready directly.

### 4.3 Completion authority

`Completion<T>` is caller-owned and address-stable for the documented request lifetime.

Backend-only capabilities own:

```text
idle -> binding -> outstanding
outstanding -> publishing -> ready
binding -> idle rollback before acceptance
```

Caller lifecycle owns:

```text
ready -> resetting -> idle
idle -> idle no-op
```

Reset or destruction in `binding`, `outstanding`, `publishing`, or `resetting` is a checked contract
violation where the Completion contract requires fail-fast.

Only designated reap code publishes Completion-ready through the slot-bound publication capability.

### 4.4 Scheduler authority

The Scheduler owns:

- Scheduler-integrated waiter/routing records;
- routing leases;
- wait-epoch accounting;
- canonical runnable publication;
- Fiber/runnable routing; and
- Scheduler shutdown/drain rules.

The Scheduler does **not**:

- choose an I/O request's terminal result;
- overwrite a RequestSlot terminal result;
- own accepted-I/O generation;
- publish Completion-ready independently;
- treat `Completion*` as logical request identity; or
- bypass RequestArena terminal-winner authority.

The request slot may temporarily hold a stable Scheduler token and move-only routing lease, but the
Scheduler remains the sole routing authority.

---

## 5. Protect the working tree

Before any edit:

```sh
git status --short
git diff --stat
git diff
```

Agents MUST:

- preserve unrelated tracked, untracked, and ignored files;
- avoid `git clean`, `git reset --hard`, destructive checkout, blanket restore, or implicit stash;
- avoid rebasing, force-pushing, merging, or changing branches unless explicitly asked;
- never delete or rewrite `.c-review-results/` merely to make a finding disappear;
- avoid whole-file or repository-wide formatting for a local repair;
- inspect the final diff and prove that only intended files changed.

Normal xmake commands may refresh ignored state such as `.xmake/`, `build/`, and
`compile_commands.json`. Check `git status --short` afterward before claiming no source changes.

---

## 6. Required baseline before production changes

The minimum repository CI baseline is Linux Clang Debug.

Unless the current task defines a stronger baseline, run before editing production code:

```sh
xmake f -m debug --toolchain=clang -y
xmake build sluice_core
xmake build sluice_async
xmake build -g test
xmake test -v
```

The explicit production-library build is required even when tests mainly link
`sluice_async_internal_testing`.

If the baseline fails:

1. do not begin broad repair work;
2. isolate the failing target or case;
3. determine whether it predates the requested change;
4. record exact command and output;
5. add or use a focused reproducer;
6. do not weaken or skip the failing gate.

Never hide a baseline failure by:

- weakening assertions;
- removing a target;
- changing its group;
- adding retries;
- increasing sleeps or yield counts;
- marking it non-blocking; or
- excluding it without an approved, documented gate decision.

---

## 6.1 Local pre-push gate (developer tooling)

A repository-managed local pre-push gate catches deterministic mechanical
failures — documentation link validation, architecture-doc structure, the
backend-conformance manifest self-test, mechanical facts (identifier
near-miss, doc LOC/count/cross-reference claims), and whitespace damage —
BEFORE a push consumes a GitHub CI round trip. It is developer tooling only and does NOT
modify async/I/O production behavior or weaken GitHub CI.

Architecture (one authority):

```text
git pre-push
    -> Lefthook (lefthook.yml, dispatcher only)
    -> scripts/gates/pre-push.sh  (the quality-gate authority)
    -> existing repository validators (same scripts CI runs)
```

The reusable shell script is the quality-gate authority; Lefthook is only the
Git-hook dispatcher. The exact local pre-push gate is reproducible by hand
without Git/Lefthook:

```sh
bash scripts/gates/pre-push.sh
```

Dependency: `lefthook` >= 1.10.0 (language-neutral, no Node/npm). The 1.10.0
floor is required because the configuration uses the `jobs:` key and the
`use_stdin: true` option on the pre-push job; older releases only support the
legacy `commands:` key and do not forward the pre-push stdin ref-pairs to the
script (which would silently degrade the whitespace gate to a working-tree-only
check). Check the installed version with `lefthook --version`.

Git hooks are NOT installed automatically just because `lefthook.yml` exists.
Each checkout must install them once:

```sh
lefthook install
```

Scope: fast + deterministic only. The local gate does NOT run the build, full
test suite, sanitizers, real-liburing, formal models, or fuzz loops — those
remain CI or explicit developer gates. Conceptual split:

```text
pre-push : docs, manifests, mechanical facts, whitespace (pushed ranges)
CI       : build, full tests, sanitizers, real liburing, negative compile,
           conformance, formal verification
```

Fail-closed: the gate exits 0 only when every check passes and exits non-zero
on the first failure, naming the failing gate and the exact reproduction
command. No `|| true`, no warning-only required gates.

Environment isolation: the gate unsets `SLUICE_TEST_FILTER` (and only that, for
now) so an ambient filter cannot narrow a manifest/attribution check or match
zero cases and misreport success. Do not blindly sanitize the whole
environment; unset only variables known to weaken an invoked check, and justify
each entry.

GitHub CI remains authoritative. Local hooks reduce turnaround; they do not
replace CI, because hooks can be bypassed (`git push --no-verify`), commits can
be produced without Lefthook, and bots/automation may not install local hooks.

`git push --no-verify` is an emergency/manual bypass, not normal workflow.

---

## 7. Focused build and test workflow

Use exact target names from `xmake.lua`.

```sh
xmake build <target>
xmake run <target>
```

The test harness supports case filtering:

```sh
SLUICE_TEST_FILTER=<case-name> xmake run <test-target>
```

Each filter token must be an **exact** registered case name (not a substring).
A filter that matches zero cases is an error: the binary prints
`SLUICE_TEST_FILTER matched zero cases` and exits non-zero instead of printing
"ALL TESTS PASSED" — a zero-case run must never masquerade as green evidence
(the Phase C1 backend-conformance gate depends on this fail-closed behavior
for its per-backend isolation runs).

A focused test is for diagnosis and iteration. It does not replace the full gate.

For bug, race, or security repairs, the normal order is:

1. reproduce the defect or establish a precise invariant violation;
2. add a regression test that fails for the intended reason when feasible;
3. implement the smallest production repair consistent with the accepted architecture;
4. run the focused test;
5. run the complete Clang Debug gate;
6. run all applicable change-class gates;
7. update architecture evidence with actual results.

A test that cannot fail on the pre-fix code is not proof of the repair.

For death tests and negative-compile probes, verify that the test reaches the intended invariant,
not an earlier unrelated failure.

---

## 8. Architecture compliance gate

Any change affecting one or more of the following MUST complete an architecture compliance gate
before production implementation begins:

- async I/O ownership;
- RequestKey / RequestSlot lifecycle;
- Completion binding or publication;
- backend submission, dispatch, or reap;
- cancellation;
- queue capacity or worker count;
- Scheduler wake/progress;
- Runtime ownership;
- public async API;
- shutdown/drain;
- a synchronization primitive;
- io_uring ownership;
- a thread pool or executor.

Required steps:

1. Read `docs/architecture/architecture-constitution.md`.
2. Identify all applicable AC-N rules.
3. Read the governing Accepted ADRs.
4. Complete either:
   - `docs/architecture/design-compliance-gate.md`; or
   - a phase-specific compliance gate that explicitly covers every Gate 0–4 field and links to the
     generic gate.
5. Classify Zig conformance/divergence.
6. Provide a state machine.
7. Provide a lock/atomic authority table.
8. Provide a resource-capacity and allocation model.
9. Provide a wake/progress model.
10. Provide shutdown semantics.
11. List evidence as `PENDING` before implementation.
12. Fill `PASS` only after commands actually run.
13. Record intentional divergence in `docs/architecture/divergence-registry.md`.
14. Update or add the design/ADR before production code.

Unknown or `TBD` authority, failure, wake, or capacity fields block implementation.

Passing tests is necessary but not sufficient for architecture compliance.

---

## 9. Core C++ and I/O contracts

The repository uses C++20 and treats compiler warnings as errors.

Preserve these rules unless an approved contract changes them:

- Use `Result<T>` / `IoError` for ordinary I/O error propagation.
- Do not introduce exception-based public I/O control flow.
- Preserve raw OS error information where required.
- Retry blocking syscalls on `EINTR` through repository retry authority.
- Do not duplicate inconsistent retry loops.
- `read_some` / `write_some` may be short.
- Exact/all helpers must loop correctly.
- Zero progress on a non-empty write is an invalid backend state, not an infinite retry.
- Positional I/O must not mutate the shared file offset.
- `flush()` does not imply durability.
- `sync_data()` and `sync_all()` retain distinct contracts.
- Destructors must not invent unreportable I/O success.
- Do not add hidden destructor flush, drain, or cancel behavior.
- Borrowed buffers and caller-owned Completions remain alive and address-stable for the documented
  lifetime.
- Check attacker-controlled sizes, integer conversions, arithmetic overflow, and allocation bounds
  before allocation or I/O.
- Check every syscall/backend return value whose failure affects correctness, liveness, or data
  integrity.
- Do not add networking, timers, coroutine layers, P2300, actor semantics, or new cancellation
  models as incidental changes.

### 9.1 Descriptor validation

A real syscall backend MUST validate representationally malformed descriptors before commit.

Examples include:

- negative fd parameter form;
- non-zero length with null buffer;
- impossible offset conversion;
- impossible native length conversion; and
- unsupported operation capability.

Malformed descriptors return synchronous `invalid_argument`, leave Completion idle, leave no
accepted slot or borrow, and start no background execution.

Do not use a syscall preflight such as `fcntl(F_GETFD)` as a substitute for operation execution;
that introduces TOCTOU. A non-negative but closed fd may be accepted and later complete with the
real syscall error.

Reference/synthetic backends may retain an explicitly registered descriptor-validation divergence.
Do not silently extend that exemption to real syscall backends.

---

## 10. Explicit request lifecycle invariants

Every migrated backend MUST preserve the following invariants.

### 10.1 Stable identity

- Every accepted request has exactly one current `RequestKey`.
- Slot reuse increments generation before the new occupant becomes visible.
- A stale key cannot cancel, dispatch, complete, attach a waiter to, or mutate a later occupant.
- `Completion*`, queue index, worker index, or closure identity cannot replace generation.

### 10.2 Transactional submission

A successful `submit_*` means:

- required bounded userspace resources were reserved;
- the Completion binding is complete;
- the slot is accepted;
- borrow lifetime has begun;
- accepted-outstanding accounting is committed;
- a reliable enqueue/dispatch/terminal path exists.

A failed `submit_*` means:

- Completion remains idle;
- no accepted request exists;
- no borrow exists;
- accepted-outstanding is unchanged;
- no queue entry or kernel/worker execution exists.

The commit/accept path uses:

```text
reserve
-> prepare
-> begin binding
-> commit slot
-> install release capability
-> publish Completion outstanding
-> allocation-free/noexcept enqueue
```

After the Completion is published outstanding:

- submit MUST NOT return a rejection;
- enqueue MUST NOT allocate;
- enqueue MUST NOT throw;
- an accepted request MUST NOT be dropped;
- dynamic expansion is not a recovery strategy.

### 10.3 Enqueue/cancel arbitration

`pending -> enqueued` and `pending -> backend-ready(canceled)` compete under one request-state
authority.

If cancel wins:

- it stores one canceled terminal;
- it establishes one backend-ready linkage;
- the enqueue pin remains live;
- later enqueue observes `backend-ready`;
- enqueue performs a successful terminal no-op;
- enqueue acknowledges the pin as its final slot access;
- no dispatch linkage is added.

If enqueue wins:

- the request becomes `enqueued`;
- later cancellation follows enqueued/running backend semantics.

A request can never have both a live dispatch linkage and an independent terminal-ready linkage.

### 10.4 Dispatch ownership

A dispatch path may start execution only after a current-generation
`enqueued -> running/kernel-owned` transition.

For a blocking backend:

- dequeue and establishment of `running` MUST form one coordinated ownership transfer;
- cancel MUST NOT be able to terminalize and reap a request in a visible pop-before-running gap;
- execution occurs without holding the queue mutex or RequestArena mutex;
- stale dispatch identity is an invariant violation, not normal backoff.

For io_uring:

- SQE preparation/submission ownership must preserve RequestKey;
- partial or zero-progress submission does not retroactively reject accepted work;
- a request cannot be released while a prepared SQE, kernel request, or future CQE may refer to it.

### 10.5 Terminal winner

The first valid transition to `backend-ready` wins.

Winner candidates may include:

- ordinary success;
- ordinary syscall error;
- confirmed effective cancellation;
- pending/enqueued cancellation where permitted;
- ownership-safe post-commit dispatch failure; and
- an explicitly approved shutdown terminal event.

Losers do not:

- overwrite terminal storage;
- publish Completion-ready;
- unlink independently;
- decrement accounting twice;
- mutate generation; or
- fabricate a second result.

### 10.6 Reap authority

Only designated reap code makes Completion-ready.

Workers, CQE handlers, cancel paths, timers, and Scheduler routing code MUST NOT directly publish a
Completion.

Reap:

- validates key and publication binding;
- observes an acknowledged enqueue pin;
- closes waiter registration;
- extracts waiter delivery exactly once;
- installs the terminal result;
- ends the borrow;
- changes slot state to completion-ready;
- decrements accepted-outstanding;
- release-publishes Completion-ready;
- leaves the slot-lifecycle domain; and
- synchronously invokes any identity-bearing ReadySink without slot or Completion pointers.

### 10.7 Slot release

`Completion::reset()` or destruction of a ready Completion may release the slot only after:

- reap left the slot-lifecycle domain;
- enqueue pin is acknowledged;
- registration is closed;
- no waiter delivery remains stored;
- no worker/kernel/backend ownership remains.

Release:

- clears the Completion binding;
- increments generation;
- decrements slot-in-use;
- publishes the slot reusable;
- allocates nothing;
- waits for no asynchronous progress; and
- calls no Scheduler, user code, sink, or backend progress function.

---

## 11. Cancellation layers and semantics

The following are distinct and MUST NOT be conflated:

- task cancellation;
- wait cancellation;
- I/O operation cancellation;
- backend syscall interruption;
- admission closure;
- graceful drain;
- abort shutdown.

Every cancel entry point documents:

- what object it targets;
- how identity is resolved;
- winner authority;
- possible disposition;
- possible terminal results;
- whether it interrupts a syscall;
- exactly-once behavior; and
- whether it is best-effort.

Required I/O cancellation behavior:

### Pending

Cancellation may win:

```text
pending -> backend-ready(canceled)
```

It stores the canceled terminal and backend-ready linkage. It does not publish Completion-ready.

### Enqueued

A backend may remove or neutralize the bounded dispatch linkage and then win the canceled terminal.
If cancellation reports a terminal win, the request MUST NOT later execute.

### Running blocking syscall

Cancellation records intent only unless the backend confirms an effective interruption.

An ordinary syscall success or error remains eligible to win **verbatim**.

A cancel intent MUST NOT rewrite ordinary success or ordinary error into `canceled`.

### Kernel-owned io_uring

Cancel SQE result is evidence about the cancellation attempt, not independent authority to overwrite
the original request. Original CQE, effective cancel, and cancel-not-found races resolve through the
same RequestSlot terminal winner and generation validation.

### Terminal/stale

- `backend-ready` or `completion-ready` returns/acts as already terminal.
- Released or stale identity is not found.
- Unknown identity never acts on a reused slot.

Cancellation never publishes Completion-ready directly.

---

## 12. Resource bounds

Every significant resource MUST be:

- explicitly bounded; or
- caller-owned with the caller responsible for bounding.

No long-lived container may grow with cumulative historical submissions without reclamation.

Required resource distinctions:

```text
request_capacity
blocking-I/O worker count
scheduler worker count
io_uring queue depth
application pipeline depth
caller-owned Completion count
```

For each resource, document:

- capacity;
- allocation time;
- hot-path allocation;
- full behavior;
- reclamation;
- high-water metric;
- shutdown ownership.

Configured capacity pressure returns a synchronous, reportable result such as `would_block` before
acceptance.

Do not use:

- unbounded per-op maps;
- per-op detached threads;
- per-op `std::function` as the core accepted request record;
- vectors that grow by total operations submitted;
- unbounded ready deques required for terminal progress; or
- dynamic allocation as a post-accept liveness dependency.

### 12.1 ThreadPoolBackend migration constraints

When modifying `ThreadPoolBackend`, the target is a bounded persistent blocking-I/O offload backend:

- fixed persistent worker count;
- construction-time bounded dispatch storage;
- RequestArena / RequestSlot identity;
- fixed operation kind/payload, not arbitrary callable;
- no `Completion*` dispatch/ready queue;
- worker only records backend-ready;
- reap publishes Completion-ready;
- no per-operation thread creation;
- no worker storage growth by historical submissions;
- enqueued cancel and worker dequeue have a coordinated ownership protocol;
- running cancel remains best-effort intent unless interruption is confirmed.

A patch that merely joins threads earlier, increases limits, reduces test scale, or renames the old
thread-per-op model does not close the resource violation.

### 12.2 Uring migration constraints

When modifying the real io_uring path:

- validate the default stub/off build;
- validate the real liburing path when available;
- encode or indirectly preserve RequestKey in `user_data`;
- do not use Completion pointer as sole kernel identity;
- preserve unsubmitted suffixes after partial submit;
- do not terminalize while SQE/kernel/CQE ownership may remain;
- validate stale CQEs by generation;
- keep request capacity distinct from ring depth.

Never present a stub-only build as real-path evidence.

---

## 13. Locks, wakeups, and concurrency

### 13.1 Lock-order discipline

Every design touching concurrency MUST publish a lock-order table.

The RequestArena slot-lifecycle domain is a leaf domain.

Code holding it MUST NOT:

- call Scheduler global state;
- call ReadySink;
- call user code;
- wait for worker/kernel progress;
- join a thread;
- execute a syscall;
- acquire a backend progress lock if another path acquires backend progress lock then arena lock.

Backend designs must avoid bidirectional lock order such as:

```text
work mutex -> arena mutex
arena mutex -> work mutex
```

unless a proven, documented protocol eliminates the cycle.

Joining threads under a queue/progress mutex is forbidden.

### 13.2 Wake obligation

Every progress-enabling state change documents:

- persistent state written;
- signal producer;
- sleeping consumer;
- predicate;
- commit-to-sleep race closure;
- worst-case latency;
- shutdown behavior.

Correct designs use an explicit predicate, epoch, sequence, or equivalent condition protocol.

Do not use:

- bounded yield loops;
- sleeps as ordering proof;
- periodic poll as undocumented sole progress;
- notification without persistent state;
- a predicate read from a different unsynchronized domain.

A pinned backend-ready request may be temporarily reap-ineligible. Pin acknowledgement must preserve
or re-arm readiness so no wake is lost.

### 13.3 Deterministic concurrency tests

Concurrency correctness SHOULD use:

- deterministic phase seams;
- barriers;
- condition variables;
- controlled clocks;
- explicit state observations;
- bounded ownership probes.

`sleep_for` MAY be used for diagnosis but MUST NOT prove:

- ordering;
- no lost wake;
- exactly-once;
- cancellation precedence;
- shutdown convergence;
- stale-generation safety; or
- liveness.

TSan supplements deterministic tests; it does not replace them.

---

## 14. Shutdown and destruction

Async backend/context destruction is quiescent.

Unless an approved ADR defines otherwise:

- destructors do not implicitly close admission;
- destructors do not cancel accepted work;
- destructors do not drain;
- destructors do not wait for asynchronous I/O completion;
- destructors do not publish terminal results;
- destruction with accepted or bound requests is a contract violation;
- fail-fast behavior required by contract remains active in Release.

The explicit lifecycle is:

```text
close admission
-> continue progress
-> reap accepted requests
-> callers reset or destroy ready Completions
-> accepted_outstanding == 0
-> slot_in_use == 0
-> backend progress ownership == 0
-> destroy
```

A persistent worker backend may notify and join already-idle workers during quiescent destruction.
That join is worker-pool teardown, not implicit I/O drain.

Any abort/cancel-on-shutdown mode requires separate approved semantics.

---

## 15. Test-only controls

Test-only controls MUST:

- be guarded by `SLUICE_ASYNC_INTERNAL_TESTING`;
- live outside installed production headers where practical;
- not change public API;
- not alter exported production behavior;
- not become production correctness dependencies;
- not silently change object layout unless the design explicitly accepts and documents the cost.

An unguarded public method named `*_for_test` is a contract smell and requires explicit review.

Prefer:

- method-only introspection seams;
- internal helper headers;
- deterministic pause gates compiled only into the internal-testing target;
- stable counters proving boundedness.

The established mechanism for "outside installed production headers" (C4, issue #135)
is the non-installed seam header under `src/async/` (for example
`src/async/scheduler_test_access.hpp`, `src/async/threadpool_test_seams.hpp`): the
installed header keeps only guarded declarations plus layout-bearing test members,
and includes the seam header at its bottom under the same
`SLUICE_ASYNC_INTERNAL_TESTING` guard. The seam include path (`src/async`) is public
ONLY on the `sluice_async_internal_testing` target; production TUs never compile the
include, carry no seam symbol, and their preprocessed output contains no seam code
(verified by `nm` on the production archive and `clang -E` shape checks). New test
control plane for an async class goes into a `src/async/*_test_seams.hpp` (or
`*_test_access.hpp`) header, not inline into the installed header.

Do not expose private queue mutation or RequestSlot mutation merely to simplify a test.

---

## 16. Change-class-specific gates

### 16.1 Public headers, templates, `noexcept`, fail-fast, or API contracts

Run Clang Release in addition to Debug:

```sh
xmake f -m release --toolchain=clang -y
xmake build sluice_core
xmake build sluice_async
xmake build -g test
xmake test -v
```

Restore Debug configuration afterward if work continues:

```sh
xmake f -m debug --toolchain=clang -y
```

Public API changes require explicit approval and updates to:

- public headers;
- contract tests;
- `docs/reference/api.md`;
- examples;
- README text where affected;
- negative-compile tests where authority changes.

Do not silently remove or re-semanticize public API.

### 16.2 Ownership, allocation, buffer lifetime, parsing, or filesystem

Run ASan + UBSan:

```sh
xmake f -m asanubsan --toolchain=clang -y
xmake build sluice_core
xmake build sluice_async
xmake build -g test
xmake run -g test
```

Use Valgrind for focused ownership/leak questions when available.

Report unavailable tools as skipped. Never report an unexecuted gate as passed.

### 16.3 Scheduler, synchronization, cancellation, queues, wakeups, multi-worker, or backend migration

Run TSan:

```sh
xmake f -m tsan --toolchain=clang -y
xmake build sluice_core
xmake build sluice_async
xmake build -g test
xmake run -g test
```

TSan evidence must include the modified race classes, not merely unrelated tests.

For a blocking worker backend, include:

- submit vs dequeue;
- enqueued cancel vs dequeue;
- running cancel vs terminal recording;
- backend-ready vs reap;
- wake signal vs wait;
- reset/reuse after reap;
- shutdown worker wake.

### 16.4 Build-system or CI changes

Prove:

- `sluice_core` builds independently;
- production `sluice_async` builds independently;
- complete test group builds;
- test failures propagate non-zero;
- optional feature gates remain off by default;
- test-only defines/sources do not leak to production.

If canonical commands change, update this file in the same change.

### 16.5 io_uring changes

Always validate stub/off.

When liburing is available and the real backend is affected:

```sh
xmake f --with-liburing=true ...
```

Report real-path evidence separately.

### 16.6 Negative compile, docs, and diff hygiene

Run applicable authority probes and documentation checks:

```sh
scripts/verify-completion-authority-negative-compile.sh
scripts/verify-request-arena-negative-compile.sh
python3 scripts/check-doc-links.py --self-test
python3 scripts/check-doc-links.py
python3 scripts/verify-architecture-docs.py
python3 scripts/gates/mechanical-facts.py --self-test
python3 scripts/gates/mechanical-facts.py
git diff --check
```

Use repository-authoritative script names from the current tree.

### 16.7 Performance changes

A **performance change** is any change whose stated purpose includes making something
faster (Core, backend, Scheduler, app, or benchmark). Such changes are governed by
`docs/verification/performance-engineering.md` (methodology) and enforced by
`scripts/bench/perf-evidence-validate.py` (structure). Rules:

- A Core performance change MUST NOT be authorized by an end-to-end application
  comparison alone.
- Every performance claim MUST identify: workload, competent baseline,
  normalization/ownership domain (APP / Boundary / Core / environment /
  benchmark artifact), before/after evidence from the same session, and the
  applicable regression matrix.
- PMU counters are diagnostic evidence, not optimization authorization.
- A Core performance candidate requires either evidence of a Common Tax or
  evidence of a material Cliff Weakness.
- Specialized optimizations MUST evaluate optional placement (compile-time
  policy / runtime backend / optional mechanism) before entering the default
  Core.
- Correctness, lifetime, ownership, request identity, cancellation,
  wait/wake ordering, and shutdown semantics MUST NOT become optional plugins.
- A concurrency-semantic optimization MUST additionally pass the architecture
  compliance gate (§8) and the formal-model requirements (§17).
- A microbenchmark gain is not sufficient evidence of success. Where
  applicable, the gain must survive back up the funnel: microbenchmark →
  normalized application → real application.
- Performance evidence is Release-only, machine-readable (runner JSON under
  `docs/results/performance-attribution/`), never hand-created, and
  structurally validated by the pre-push/CI gate. "No performance claim
  without benchmark + workload" (§21) remains in force.
- Aggregate ladder increments (e.g. L4 − L3) prove a Core-owned cost exists;
  they do NOT decompose it. Do not claim internal attribution (handoff, wake,
  reap, ...) without a decomposition experiment.

---

## 17. Formal models and protocol evidence

Formal models under `spec/tla/` supplement implementation tests. They do not prove the C++ code
bug-free.

Canonical locations:

- models: `spec/tla/`;
- inventory: `spec/tla/manifest.json`;
- scripts: `scripts/formal/`;
- orchestrator: `python3 scripts/formal/verify.py`;
- workflow: `.github/workflows/formal.yml`.

Rules:

- Never run TLC directly in a source suite directory.
- Use repository verifier scripts that copy files into isolated temporary workspaces.
- Do not describe abstract model checking as C++ implementation verification.
- When code changes a modeled:
  - state transition;
  - admission rule;
  - queue bound;
  - terminal winner;
  - wake rule;
  - lifecycle;
  - generation rule; or
  - shutdown behavior,
  update the matching model or explicitly document why no existing model applies.
- Preserve negative/broken-model checks where the suite uses them.
- Retain a C++ regression test connecting the modeled property to implementation behavior.
- If no model covers a new high-risk protocol, the compliance gate must either:
  - add a focused model; or
  - record a justified formal-coverage gap and follow-up trigger.

Do not invent a model merely for ceremonial coverage. Model the smallest protocol that captures the
load-bearing race.

---

## 18. Backend conformance and testing philosophy

Correctness tests prove semantics, not implementation preference.

Do not use thread count, container internals, timing, or syscall names as the sole proof of:

- no lost wake;
- exactly-once;
- accepted terminality;
- cancellation correctness;
- shutdown convergence;
- backend conformance.

Implementation-resource tests are still required for AC-7 and may assert:

- configured worker count;
- total workers created;
- queue capacity;
- high-water marks;
- lack of historical growth;
- absence of per-op thread creation.

Pair them with semantic tests:

- full capacity returns the correct synchronous result;
- accepted request reaches exactly one terminal;
- canceled enqueued request does not execute;
- running cancel preserves real result;
- wait does not lose a wake;
- stale generation cannot dispatch;
- quiescent destruction succeeds;
- non-quiescent destruction fails as specified.

Common backend conformance should run against every backend for which the property is meaningful.
Backend-specific mechanism tests do not by themselves establish repository-wide conformance.

---

## 19. Security and review findings

Treat automated findings as hypotheses.

For each finding:

1. inspect exact code and callers;
2. determine trust boundary;
3. identify reachable failure mode;
4. reproduce with a test, focused probe, or precise static argument;
5. classify true positive, false positive, accepted risk, or duplicate;
6. repair root cause;
7. preserve behavior outside scope;
8. add regression evidence;
9. document residual platform limitations.

Do not:

- batch unrelated findings into a broad refactor;
- change public semantics merely to silence a warning;
- mark a finding fixed because a scanner stopped reporting it;
- hide a violation by weakening fail-fast or changing an error vocabulary;
- describe a stale identity, double terminal, or lost wake as harmless timing noise.

---

## 20. Formatting and static analysis

Use repository `.clang-format`:

- 4-space indentation;
- no tabs;
- attached braces;
- 100-column limit;
- pointer/reference alignment with the type;
- preserved include blocks and include order.

Format only intentionally changed files or ranges.

Do not run whole-repository formatting for a focused change.

Use `.clang-tidy` when part of the task. `clang-tidy --fix` is a code change and requires normal
review and gates.

Before completion:

```sh
git diff --check
git status --short
git diff --stat
git diff
```

---

## 21. Documentation discipline

Update documentation when a change affects:

- authority;
- lifecycle;
- ownership;
- failure behavior;
- resource bounds;
- lock order;
- wake behavior;
- cancellation;
- shutdown;
- public API;
- target names;
- feature gates;
- build commands;
- formal-model bindings;
- Zig conformance/divergence.

Comments should explain:

- invariant;
- authority;
- ownership;
- lock order;
- allocation boundary;
- failure behavior;
- why a tempting alternative is incorrect.

Avoid comments that merely narrate code or preserve obsolete phase history in production headers.

Interface comment, implementation, Accepted ADR, compliance gate, and tests MUST agree for critical
transitions.

Do not claim:

- performance improvement without a benchmark and workload;
- sanitizer coverage not executed;
- real liburing coverage from stub builds;
- formal implementation verification from abstract models;
- complete explicit-I/O migration while legacy production paths remain.

---

## 22. Commit and PR discipline

Keep commits reviewable and semantically coherent.

Prefer slices such as:

```text
design / compliance gate
tests proving the old violation
bounded storage and invariants
production migration
race/shutdown repair
documentation and evidence
```

Do not combine unrelated cleanup with a protocol migration unless the cleanup is required to make
the governing contract internally consistent.

A PR affecting explicit I/O should state:

- baseline SHA;
- affected capability/layer;
- governing ADR;
- AC-N rules;
- state-machine changes;
- resource equations;
- lock order;
- wake protocol;
- cancellation semantics;
- shutdown semantics;
- migration status;
- tests and actual results;
- remaining divergences.

Do not pre-fill `PASS` before execution.

Do not claim a phase is complete if its stated production backend, wake bridge, Scheduler routing,
or public API migration remains out of scope.

---

## 23. Completion report

Every non-trivial coding task ends with a command-backed report:

- **Scope**
  - issue, finding, contract, or phase addressed;
- **Baseline**
  - branch, SHA, exact commands, results;
- **Authority**
  - governing ADR/design and applicable AC-N rules;
- **Root cause**
  - why the prior implementation violated the contract;
- **Changes**
  - files and semantic effect;
- **State/race proof**
  - winner, lock/atomic domains, lost-wake closure, generation/reuse safety;
- **Resources**
  - capacities, allocation boundaries, high-water evidence;
- **Tests**
  - exact focused and full commands with results;
- **Additional gates**
  - Release, ASan/UBSan, TSan, Valgrind, real liburing, negative compile, death tests,
    docs, formal model;
- **Skipped evidence**
  - unavailable or out-of-scope gates with reasons;
- **Residual risk**
  - only real unresolved risk;
- **Commits**
  - SHA and subject;
- **Working tree**
  - unrelated files preserved;
- **Pushed branch**
  - when publishing was requested.

“Looks correct”, “should pass”, “CI will catch it”, and unexecuted claimed gates are not completion
criteria.
