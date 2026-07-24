# AGENTS.md — Sluice Repository Instructions

This file is the repository-wide operating contract for coding agents working on Sluice.
It applies to the entire repository unless a more specific nested `AGENTS.md` is added later.

Normative words such as **MUST**, **MUST NOT**, **SHOULD**, and **MAY** are intentional.

## 1. Project identity and architectural boundaries

Sluice is an experimental C++20 I/O control-flow library built around explicit capabilities,
pluggable backends, and backend-neutral `Reader` / `Writer` semantics.

The main build boundaries are:

- `sluice_core`: the synchronous core. It owns `Result<T>`, `IoError`, `Reader`, `Writer`,
  buffering, copy helpers, WAL, file I/O, positional I/O, durability, and the production
  `BlockingIoPool`.
- `sluice_async`: the opt-in asynchronous runtime in `namespace sluice::async`. It is a
  separate production static library and MUST NOT alter the synchronous public contract merely
  to make async implementation easier.
- `sluice_async_internal_testing`: a test-only build of the same authoritative async production
  sources plus guarded deterministic test controls. Production code MUST NOT depend on it, and
  no executable may link both async variants.
- `sluice_experimental_uring`: optional experimental io_uring code. It is off by default and is
  not the default backend.
- `zig/`: design reference only. It is not built, linked, vendored into production output, or
  treated as source code to copy mechanically.

Do not collapse these boundaries. In particular:

- `BlockingIoContext` remains the default blocking path.
- `BlockingIoPool` is a bounded OS-thread helper, not the async runtime.
- io_uring is one optional backend, not the architecture.
- public synchronous `Reader` / `Writer` semantics remain synchronous.
- test seams remain non-installed and compile-time guarded.

## 2. Authority and conflict resolution

Before changing a subsystem, identify its authority chain.

Use the following order:

1. The explicit current task, issue, approved plan, or review finding scope.
2. Accepted ADRs and subsystem closeout/design documents under `docs/`.
3. Public headers under `include/sluice/` and the deliberate public API description in
   `docs/api-reference.md`.
4. Production implementation under `src/`.
5. Contract, regression, death, negative-compile, and causal tests under `tests/`, plus the
   subsystem verification scripts under `scripts/`.
6. `xmake.lua` for target membership, dependencies, feature gates, and executable test names.
7. `.github/workflows/ci.yml` for the minimum repository merge gate.
8. `README.md` for orientation and common commands.

A scanner report, code-review summary, comment, commit message, or stale planning note is evidence,
not automatic authority.

If these sources disagree, MUST NOT silently choose whichever is easiest. Characterize the current
behavior with a focused test, report the contradiction, and make the smallest change that restores
an explicitly approved contract. Do not rewrite an accepted ADR to pretend a historical decision
never existed; add a superseding ADR or closeout note when semantics deliberately change.

## 3. Protect the working tree

Before any edit:

```sh
git status --short
git diff --stat
git diff
```

Agents MUST:

- preserve all unrelated tracked, untracked, and ignored files;
- avoid `git clean`, `git reset --hard`, destructive checkout, blanket restore, or implicit stashing;
- avoid rebasing, force-pushing, merging, or changing branches unless explicitly asked;
- never delete or rewrite `.c-review-results/` merely to make a finding disappear;
- avoid whole-file or repository-wide formatting when only a local repair is required;
- inspect the final diff and prove that only intended files changed.

Normal xmake commands may refresh ignored state such as `.xmake/`, `build/`, and
`compile_commands.json`. Do not claim that a command touched only ignored output without checking
`git status --short` afterward.

## 4. Required baseline before production changes

The repository CI gate is Linux Clang Debug. Unless the current task explicitly defines a stronger
baseline, run the same path before editing production code:

```sh
xmake f -m debug --toolchain=clang -y
xmake build sluice_core
xmake build sluice_async
xmake build -g test
xmake test -v
```

This explicit production-library build is required even when tests depend mostly on
`sluice_async_internal_testing`; the test variant is not proof that the production async surface
still compiles warning-clean.

If the baseline fails:

1. do not start broad repair work;
2. isolate the failing target or test case;
3. determine whether the failure predates the requested change;
4. record the exact command and output;
5. add or use a focused reproducer before modifying production behavior.

Never hide a baseline failure by weakening assertions, removing a test target, changing the test
group, adding retries, increasing sleeps, or marking failures non-blocking.

## 5. Focused build and test workflow

Use the exact target names defined in `xmake.lua`.

```sh
xmake build <target>
xmake run <target>
```

The dependency-free test harness supports case-name filtering:

```sh
SLUICE_TEST_FILTER=<case-name-substring> xmake run <test-target>
```

A focused test is for diagnosis and iteration. It does not replace the full repository gate.

For bug and security repairs, the normal order is:

1. reproduce the defect or establish a precise invariant violation;
2. add a regression test that fails for the intended reason when feasible;
3. implement the smallest production fix;
4. run the focused test;
5. run the complete Clang Debug gate;
6. run the change-class-specific gates below.

A test that cannot fail on the pre-fix code is not proof of the repair. For death tests and
negative-compile probes, verify the test reaches the intended invariant rather than merely failing
for an earlier unrelated precondition.

## 6. Change-class-specific gates

### 6.1 Public headers, templates, `noexcept`, assertions, or API contracts

Run Clang Release in addition to Debug because `NDEBUG`, unused assertion-only locals, template
instantiation, visibility, and function-type changes may differ:

```sh
xmake f -m release --toolchain=clang -y
xmake build sluice_core
xmake build sluice_async
xmake build -g test
xmake test -v
```

After a non-Debug mode, restore the CI configuration before the final status check if the working
session continues:

```sh
xmake f -m debug --toolchain=clang -y
```

Public API changes require deliberate approval and corresponding updates to public headers,
contract tests, `docs/api-reference.md`, and affected examples or README text. Do not silently
remove or re-semanticize a public API.

### 6.2 Memory ownership, parsing, allocation, buffer lifetime, or filesystem changes

Run ASan and UBSan, preferably together when supported by the current environment:

```sh
xmake f -m asanubsan --toolchain=clang -y
xmake build sluice_core
xmake build sluice_async
xmake build -g test
xmake run -g test
```

Use Valgrind for focused ownership/leak questions when available. Report unavailable tools as
skipped; do not claim a gate passed when it was not executed.

### 6.3 Scheduler, synchronization, cancellation, queues, wakeups, or multi-worker changes

Run TSan in addition to deterministic causal tests:

```sh
xmake f -m tsan --toolchain=clang -y
xmake build sluice_core
xmake build sluice_async
xmake build -g test
xmake run -g test
```

Correctness tests for concurrency SHOULD use deterministic phase seams, barriers, controlled
clocks, or explicit state observation. `sleep_for` MAY be used during diagnosis but MUST NOT be the
proof of ordering, liveness, absence of a lost wake, or exactly-once publication. Report tests that
were not built or run because of architecture or platform gates; a skip is not execution evidence.

### 6.4 Build-system or CI changes

Prove that:

- `sluice_core` builds independently;
- production `sluice_async` builds independently;
- the complete `test` group builds;
- test failures still propagate as a non-zero command result;
- optional feature gates remain off by default;
- no test-only define or source leaks into a production target.

If `xmake.lua` or `.github/workflows/ci.yml` changes the canonical commands, update this file in the
same change.

### 6.5 io_uring changes

Always validate the default stub/off path. When liburing is available and the task concerns the
real backend, also configure with `--with-liburing=true` and report that real-path evidence
separately. Never present a stub-only build as validation of the real io_uring path.

## 7. Core C++ and I/O contracts

The repository uses C++20 and treats compiler warnings as errors.

Preserve these rules unless an approved contract explicitly changes them:

- Use `Result<T>` / `IoError` for ordinary I/O error propagation.
- Do not introduce exception-based public I/O control flow.
- Preserve raw OS error information where the existing error model requires it.
- Retry blocking syscalls on `EINTR` through the repository retry authority; do not duplicate
  inconsistent retry loops.
- `read_some` / `write_some` may be short. Derived exact/all helpers must loop correctly.
- Zero progress on a non-empty write is an invalid backend state, not an infinite retry.
- Positional I/O must not mutate the shared file offset.
- `flush()` drains software buffering; it does not imply durability.
- `sync_data()` and `sync_all()` retain their distinct OS/filesystem contracts.
- Destructors must not invent unreportable I/O success. Do not add hidden destructor flushes.
- Borrowed buffers and caller-owned completions must remain alive and address-stable for the
  documented operation lifetime.
- Check attacker-controlled sizes, integer conversions, addition/multiplication overflow, and
  allocation bounds before allocation or I/O.
- Check every syscall/backend return value whose failure affects correctness, liveness, or data
  integrity.
- Do not add dependencies, public abstractions, networking, timers, coroutine layers, P2300,
  actor semantics, or new cancellation models as incidental parts of an unrelated fix.

## 8. Async and concurrency invariants

Async repairs MUST preserve the subsystem's documented authority model. At minimum:

- The Scheduler owns scheduler-integrated registration, terminal resolution accounting, and
  canonical runnable routing.
- Private `WaitQueue` structural operations must not be exposed as a public or forgeable bypass.
- A terminal winner transition and its unlink/removal obligation remain one coordinated authority;
  losers do not unlink, publish, or wake.
- A wait epoch has exactly one terminal outcome and at most one runnable publication.
- Reusing, moving, or destroying an outstanding registration object is forbidden unless its
  documented lifecycle explicitly permits it.
- Queue identity, Scheduler identity, owner identity, and stable-address requirements are not
  optional debug conveniences.
- Preserve documented lock ordering. Do not acquire a primitive/queue lock and then reach upward
  into Scheduler global state if the subsystem contract requires Scheduler-global lock first.
- Resource availability versus deadline/cancellation precedence must remain consistent with the
  primitive's contract and cross-primitive parity tests.
- Destruction with live waiters, registrations, or outstanding callbacks is a contract violation
  where the subsystem says so; do not silently leak or detach them to make teardown pass.
- Existing fail-fast boundaries must remain fail-fast in Release when the contract requires it.
  Do not replace invariant failures with recoverable results without an approved semantic change.
- Test-only controls belong behind `SLUICE_ASYNC_INTERNAL_TESTING`, outside installed production
  headers where possible, and must not alter production object layout or exported behavior unless
  the design explicitly accepts that cost.

When touching E10–E13 code, read the named subsystem document referenced by the relevant header or
test before editing. The comments and tests encode specific laws such as fresh-per-epoch nodes,
queue-identity-safe cancellation, owner-before-publication, rollback ordering, stale-registration
handling, and exactly-once publication. Do not reconstruct these laws from memory.

## 9. Formal models and protocol evidence

Formal models under `spec/` supplement implementation tests; they do not prove that the C++ code is
bug-free.

When a code change alters a modeled state transition, admission rule, winner rule, queue bound,
lifecycle, or shutdown behavior:

- update the matching model or explicitly explain why the model is unaffected;
- run the repository's existing verification script or documented checker command;
- preserve a negative/broken-model check when the subsystem uses one to demonstrate that the model
  can actually produce a counterexample;
- add or retain a C++ regression test connecting the modeled property to implementation behavior.

Never report "formally verified implementation" when only the abstract protocol model was checked.

## 10. Security and review findings

Treat `.c-review-results/` and other automated findings as hypotheses to verify.

For each finding:

1. inspect the exact code and all relevant callers;
2. determine the input/trust boundary and reachable failure mode;
3. reproduce with a test, a focused probe, or a precise static argument;
4. distinguish true positive, false positive, accepted risk, and duplicate;
5. repair the root cause, not merely the reported line;
6. retain behavior outside the finding's scope;
7. add evidence that would catch regression;
8. document any residual platform or environment limitation.

Do not batch unrelated findings into a broad refactor. Do not change public semantics merely because
it makes one warning easier to silence. Do not mark a finding fixed solely because the compiler or
scanner stopped reporting it.

## 11. Formatting and static analysis

Use the repository `.clang-format` configuration:

- 4-space indentation;
- no tabs;
- attached braces;
- 100-column limit;
- pointer/reference alignment with the type;
- preserved include blocks and include order.

Format only files or ranges intentionally changed. Do not run formatting across the entire
repository during a focused repair.

Use the repository `.clang-tidy` configuration when static analysis is part of the task. Treat
`clang-tidy --fix` as a code change requiring full review and normal tests; it is not an automatic
source of truth.

Before completion, run:

```sh
git diff --check
git status --short
git diff --stat
git diff
```

## 12. Documentation discipline

Update documentation when the change affects a documented contract, public API, build command,
target, feature gate, lifecycle, ownership rule, or failure behavior.

Comments should explain invariants, authority, ownership, lock order, and non-obvious failure
behavior. Avoid comments that merely narrate the next line or preserve obsolete phase history in
production code.

Do not claim performance improvements without an appropriate benchmark and a described workload.
Do not claim sanitizer, platform, real-liburing, or formal-model coverage that was not run.

## 13. Completion report

Every non-trivial coding task should end with a concise report containing:

- **Scope:** finding, issue, or contract addressed.
- **Baseline:** exact pre-change commands and result.
- **Root cause:** why the defect occurred.
- **Changes:** files and semantic effect.
- **Tests:** exact focused and full commands with results.
- **Additional gates:** Release, ASan/UBSan, TSan, Valgrind, real liburing, negative compile,
  death tests, or formal model, as applicable.
- **Skipped evidence:** unavailable or intentionally out-of-scope gates, with reason.
- **Residual risk:** platform limitations, untested paths, or follow-up work.
- **Working tree:** confirmation that unrelated files were preserved.

Evidence must be command-backed. "Looks correct", "should pass", and "CI will catch it" are not
completion criteria.
