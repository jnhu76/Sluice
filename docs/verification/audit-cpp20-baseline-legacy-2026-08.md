# C++20 Baseline / Compiler Floor / Legacy Hygiene Audit — 2026-08

Correctness-oriented audit of the repository's language-standard contract. Not a
style pass: the goal is to answer whether Sluice is genuinely a C++20 codebase
end-to-end, whether any target/script/CI path silently drops the standard,
which legacy-looking constructs are intentional concurrency design (and must
never be mechanically modernized), and what compiler floor the project can
defend with evidence.

## 1. Baseline

| Item | Value |
|------|-------|
| master SHA | `5e5ec3663d69f09b5b571ea01c9f4200c75aec98` (merge of #93) |
| Parent PR | #102 — OPEN, head `fea5d26c4878cba801a239780e4d54e68549cd8d` (not merged; audit branched from its head) |
| Audit branch | `audit/cpp20-legacy-hygiene` |
| Audit head | `fea5d26` + 2 one-line fixes (see §5 Fixed) |
| Compilers (local) | Clang 22.1.8 (Fedora 44), GCC 16.1.1 |
| CI compiler | ubuntu-latest (single required job) |
| xmake | 3.0.9+20260621 |
| liburing | xrepo `liburing 2.14` (real path verified; system pkg-config 2.13 unused) |

## 2. Language-standard truth

**Sluice minimum language standard = C++20, and it is enforced consistently
across the whole build graph.** The audit found no path that compiles under an
older standard.

Evidence:

- `xmake.lua` root: `set_languages("c++20")` + `set_warnings("all", "error")`.
  All 13 xmake config files under `xmake/` contain **zero** `set_languages`
  overrides, zero `-std=` overrides, zero `set_toolchains` overrides. Every
  target (production, test, example, bench, fuzz, app) inherits C++20.
- Every script that bypasses xmake and invokes a compiler directly passes
  `-std=c++20` explicitly:
  - `scripts/verify-async-api-negative-compile.sh`,
    `verify-async-identity-negative-compile.sh`,
    `verify-completion-authority-negative-compile.sh`,
    `verify-request-arena-negative-compile.sh`,
    `verify-external-backend-authority-negative-compile.sh`
    (`-std=c++20 -Wall -Werror -fsyntax-only`);
  - `scripts/hardening/preflight.py` (`-std=c++20`);
  - `scripts/verify-wal-copy-curated-seeds.sh` (`-std=c++20 -O2`);
  - `scripts/formal/*.sh` (8 scripts, `-std=c++20 -fsyntax-only`).
- As-built ground truth: `compile_commands.json` — 286/286 entries carry
  `-std=c++20 -Wall -Werror` (plus `-Wthread-safety` on the Clang async paths).
- The public API has real, non-removable C++20 dependencies: `concept`
  (`SelectCaseType` in `include/sluice/async/select_fwd.hpp`),
  `requires`-clauses (select, Scheduler friend template), `std::remove_cvref_t`,
  `std::span` (10 public headers), designated initializers
  (`include/sluice/fault.hpp`, `src/wal.cpp`), `std::atomic<T>::wait/notify`
  (internal-testing seams only). C++20 is a source contract, not aspirational.
- Fuzz targets are Clang-gated (`xmake/fuzz.lua`) but inherit C++20; the
  instrumented `sluice_core_fuzz` compiles the same `core_sources()` manifest.
- CI runs no compiler without the standard: the negative-compile steps pin
  `CXX=clang++` (3 of 5) or fall back to `c++` (g++ on ubuntu-latest); all use
  `-std=c++20` regardless.

**Verdict: Sluice is a real C++20 codebase. No target, script, example, test,
fuzz, or benchmark silently lowers the language standard.**

## 3. Build graph audit

| Target / path | Effective standard | Compiler path | Exceptions |
|---------------|--------------------|---------------|------------|
| `sluice_core` | C++20 | clang/gcc/clang-cl | — |
| `sluice_async` | C++20 | clang/gcc/clang-cl | `-Wthread-safety` scoped to clang frontends only (`{tools={"clang","clang_cl"}}`) |
| `sluice_async_internal_testing` | C++20 | clang/gcc/clang-cl | TSA flags same scope; `SLUICE_ASYNC_INTERNAL_TESTING` public define |
| `sluice_bench_common` | C++20 | all | — |
| `sluice_experimental_uring` | C++20 | all | stub or real liburing per `--with-liburing` |
| `sluice_core_fuzz` + 3 fuzz binaries | C++20 | clang-only (file gate + per-target guard) | `-fsanitize=fuzzer(-no-link),address,undefined` |
| 157 test targets | C++20 | all | — |
| examples / bench / apps (`sluice-copy`) | C++20 | all | — |
| negative-compile scripts | `-std=c++20` | `${CXX}` (CI pins clang++ for 3/5) | `-fsyntax-only` |
| formal probe scripts (8) | `-std=c++20` | `$CXX_BIN` | `-fsyntax-only` |
| `preflight.py` | `-std=c++20` | `$CXX` | — |
| CI | ubuntu-latest, Linux Clang Debug | xmake `--toolchain=clang` | single required job; no GCC/sanitizer/tidy in merge gate |

## 4. Findings

| ID | Severity | Location | Finding | Evidence | Action |
|----|----------|----------|---------|----------|--------|
| F1 | P2 | `.clang-tidy`, `docs/verification/README.md` | clang-tidy config exists (rich check set, `WarningsAsErrors: ''`, `HeaderFilterRegex: include/sluice/.*`) but is executed **nowhere**: zero mentions in workflows, scripts, lefthook, or pre-push. The docs' five-layer quality model lists "Code Quality Analysis — clang-tidy" as layer 4 — a claimed gate that never runs. | `grep -rn -i clang-tidy .github/ scripts/ xmake/ lefthook.yml` → 0 hits | Wire a no-new-warning baseline (CI or nightly); do not flip all-warnings-as-error |
| F2 | P2 | README, docs/, CI | Compiler floor is unspecified. CI proves only "current ubuntu-latest Clang builds". No GCC merge job, no floor job. Local reviews happened to use Clang 18-21 / GCC 15-16. | `.github/workflows/ci.yml` (single job); no floor text in README/docs | Adopt Clang ≥ 14 / GCC ≥ 11 (§6) and add a nightly floor job |
| F3 | P2 | `xmake.lua:22`, `src/async/scheduler.cpp:4428`, `fuzz/copy_all_fault_fuzz.cpp:160` | `-Wextra` not enabled (`set_warnings("all")` maps to `-Wall` only, as-built). Full-tree `-Wextra` probe: exactly 3 real instances — `scheduler.cpp` unused `wh` (FIXED), fuzz harness unused `result` (FIXED), plus 2 authority-probe TUs with unused params that only matter if gates adopt `-Wextra`. | `-Wextra` scan of every TU under src/, tests/, examples/, bench/, fuzz/, apps/ | Enable `-Wextra` after the 2 probe `(void)` fixes; currently the tree is already clean |
| F4 | P3 | `.github/workflows/ci.yml` | Negative-compile CXX pinning is inconsistent: completion-authority, request-arena, external-backend steps pin `CXX=clang++`; async-api and identity steps rely on `${CXX:-c++}` (g++ on ubuntu). Behaviorally harmless (both are C++20), but the identity gate's structural authority claim is verified against a different compiler than its siblings. | ci.yml steps | Pin `CXX=clang++` on the two remaining steps |
| F5 | P3 | docs review history | Some review docs cite `-Wall -Wextra -pedantic` manual compile evidence while the build only emits `-Wall -Werror`. Cosmetic mismatch between recorded evidence flags and as-built flags. | `docs/history/reviews/E12-E-QUEUE-CORRECTIVE-2-...` | No code action; noted for future evidence templates |
| F6 | P3 | `xmake/experimental.lua`, build cache | Reconfiguring stub→real liburing can leave stale stub objects in `build/.objs` (xmake object cache); binaries can then silently be stub-built while `compile_commands.json` shows mixed entries. Caught here by fail-closed tests; the c2e `[evidence-meta] mode=` line misreported stub for a real-built TU until objects were wiped. | observed during this audit | Prefer `xmake f -c` when toggling `--with-liburing`; consider a config-hash-in-object-path |
| F7 | P3 | `include/sluice/async/group.hpp:278` | `std::unique_ptr<std::byte[]>(new std::byte[kStackBytes])` — documented stable-address + 16-byte alignment. `std::make_unique<std::byte[]>` would value-initialize (zero) 64 KiB — a behavior change. Keep. | comment at site | Keep intentionally |
| F8 | P3 | `src/experimental/uring_write_batch.cpp:26` | Manual `new`/`delete` of `io_uring` on queue-init failure path. Contained, experimental, correct as written. | site | Keep (or `unique_ptr` in a future experimental cleanup) |
| F9 | P3 | `include/sluice/blocking_io_pool.hpp:31` | Header says "Pure C++17/20" — wording only; code is C++20. | site | Wording cleanup optional |
| F10 | P2 | `src/async/uring_backend.cpp:1553-1601` (test seams) | Uring pause gates still use `while (!gate->resume.load(acquire)) std::this_thread::yield();` (4 sites, `SLUICE_ASYNC_INTERNAL_TESTING`-guarded). ThreadPool migrated away from exactly this spin (issue #92: yield-spin starves the resume publisher under parallel-Debug oversubscription) to blocking `atomic::wait` + mandatory store+notify pairing. Test-infrastructure-only, zero production impact — but must migrate the whole gate group at once. | issue #92 record; `threadpool_backend.hpp:734-752` | Safe modernization candidate (deferred) |

## 5. Legacy constructs

### Fixed (this audit)

- `src/async/scheduler.cpp:4450` — `(void)wh;` for the documented
  contract-assertion parameter of `attach_ready_wake` (internal method; header
  declares the contract, ADR §9.4.10). Enables `-Wextra`-clean src/.
- `fuzz/copy_all_fault_fuzz.cpp:162` — `(void)result;` reserved-parameter note
  in `check_universal`. Enables `-Wextra`-clean fuzz/.

Both are comment-carrying `(void)` casts of deliberately unused parameters;
zero semantic effect, verified by full test gate.

### Keep intentionally (design, do not mechanically modernize)

The deep concurrency read (every async production file) found **no safely
mechanizable concurrency construct**. The following are load-bearing design
(ADR + AGENTS.md + TLA+ backed) — each entry names the modern replacement that
would silently change the protocol and why:

| Construct | Why it must not be "modernized" |
|-----------|----------------------------------|
| `sluice::async::LockGuard` (custom, `lock_guard.hpp`) | Carries `SLUICE_ACQUIRE`/`SLUICE_RELEASE` TSA annotations; `std::scoped_lock`/`std::lock_guard` cannot express the thread-safety contract that Clang TSA enforces on the async path |
| ThreadPoolBackend persistent workers: `std::thread` + `stopping_` + `work_cv_` + explicit dtor/ctor-failure join (`threadpool_backend.cpp:129,144-164`) | `jthread`/`stop_token` would introduce a stop source unrelated to the `work_cv_` predicate and move join past the quiescence fail-fast check — shutdown ownership is AGENTS §14 |
| Scheduler per-invocation worker threads; join must precede `active_worker_count_` unpublication (`scheduler.cpp:575-601`) | `jthread` auto-join fires after `run_impl` returns, inverting the topology-publication order that keeps `spawn()`/routing from targeting a winding-down worker |
| Scheduler wake domain: `wake_cv_` + `wake_epoch_`/`observed_epoch` + bounded `wait_until` (2 ms backstop is load-bearing, E9-LIFE-8) + multi-domain predicate (`scheduler.cpp:349-402`) | `atomic::wait` has no timeout and cannot express the epoch × terminate × inbox conjunction; the deadline is dynamically computed |
| ReadyWaitSource dual epoch + per-waiter observed token + arm/consume baseline (`ready_wait_source.hpp:55-127`) | `atomic::wait` cannot express per-waiter snapshot predicates, wake-reason reporting, or the D4-RM14 one-shot arm window |
| `work_cv_` predicate = `stopping_ || !dispatch_.empty()` (two domains); ApplicationRuntime `runtime_cv_` + `control_epoch_` re-entry protocol | `atomic::wait` is single-atomic; these predicates are conjunctive over lock-protected state and re-arm per run |
| `Future` mtx_+cv_+atomic ready_ triple (`future.hpp:66-132` + `wait_policy.hpp:46-48`) | The WaitPolicy seam receives all three by reference; the Evented policy ignores mtx/cv and only waits the flag. `atomic::wait` cannot serve both policies |
| `Completion<T>` `std::atomic<State>` CAS + binding/publishing/resetting transients (`completion.hpp:289-421`) | Already the modern form (atomic enum + CAS, single-winner publication, fail-fast authority); "simplification" would violate ADR-explicit-io-completion-authority |
| `CancelToken` single atomic bit-pack (pending bit + epoch in one word, `cancel.hpp:111`, `cancel.cpp:28,95-114`) | `check_cancel` must linearize on one snapshot; splitting the word reopens the rearm/clear tear window (ADR-cancel-request-epoch) |
| `WaitNode`/`WaitQueue` intrusive raw `next_`/`prev_`/`home_`/`head_` + winner-CAS-unlink-in-same-critical-section (`wait_node.hpp:197-251`, `wait_queue.hpp:199-211`) | Caller-owned address-as-identity (mirrors Completion L7); UNLINK LAW is the mechanical guarantee of "no second wake"; shared_ptr/containers would change identity and add post-accept allocation (AGENTS §12) |
| `RequestArena` single leaf mutex + `SlotHandle`/`Generation` value identity + type-erased `CompletionBinding` + intrusive ready-ring (`request_arena.hpp:1044-1134`) | Leaf-domain rule (AGENTS §13.1); slot-as-identity with zero allocation is AC-2/AC-14 |
| `SchedulerWakeHandle::Control::mtx` as mutex-serialized callback lease (`scheduler.cpp:61-96,201-237`) | Explicitly NOT shared ownership/refcount; has a TLA+ proof (`docs/spec/e9_wake_handle_lifetime/`) |
| `QueuePort::CallGuard` destructor re-acquiring G+S to decrement (`queue_port.cpp:100-128`) | #86-A protocol brace: decrement must live in the same sync domain as increment and `begin_teardown`; not an ordinary scope guard |
| `QueueItemLease`/`Node<T>` typed new/delete + non-empty-destructor fail-fast (`queue_port.hpp:232,315`, `queue_item.hpp:123-173`) | Linear capability; smart pointers would silently swallow ownership errors |
| `Group` threaded-mode per-task `std::thread` + explicit join before `Future::await` (`group.hpp:207-215`, `group.cpp:94-144`) | Exception-safety ordering is load-bearing; jthread would conflate task cancellation with thread stop (AGENTS §11 layering) |
| `ApplicationRuntime` driver thread explicit join + `close_resources()` order + full manual flag set (`application_runtime.cpp:141-155,477-482,730-760`) | Close-owner election (P1-05) and ADR §8 state machine are the sole close authority; jthread auto-join decouples the publish order |
| Scheduler `global_terminate_`/`idle_workers_`/`admission_` re-armed per coordinated run (`scheduler.cpp:549-551`) | stop_token is one-shot and cannot be re-armed per invocation (MW-S1/S2/S3) |
| `__asm__ volatile` (`fiber_ctx.cpp:169`) | The fiber context-switch primitive itself; not volatile-as-synchronization |
| `condition_variable` + predicate waits elsewhere (wait_policy, uring_wait_source, future, blocking pools) | Each has a documented predicate/epoch protocol; `atomic::wait` would need a provably equivalent wake/lock protocol (AGENTS §13.2) — none were found equivalent |
| Manual `unique_lock` unlock-before-notify (`application_runtime.cpp`) | Lock-order/wake protocol requirement, not a hygiene slip |
| Raw `Completion*` / `RequestSlot*` / `Fiber*` identity pointers | Non-owning stable identity is the explicit-I/O contract (AGENTS §4); `shared_ptr` would invert ownership |
| Custom `Result<T>` / `IoError` | `std::expected` is C++23 — out of the C++20 floor; and the error vocabulary is contractual |
| `std::atomic<bool>::wait/notify` confined to `SLUICE_ASYNC_INTERNAL_TESTING` seams | Correct placement: test seams use the C++20 primitive; production stays on the documented CV/epoch protocols |
| Explicit enum request state machine (`RequestState`) | The canonical lifecycle; not an "old-style" state machine |

### Deferred / false positives

- F7, F8, F9, F10 (P3, documented above; F10 is the only real modernization
  candidate — Uring test-gate yield-spin → `atomic::wait`, must migrate as a
  group with the ThreadPool #92 discipline).
- F22/F35 (agent findings): `new QueueItemLease[capacity_]()` and
  `new ApplicationRuntime(...)` are access-control workarounds (private
  constructors); semantically safe but modernization value ≈ 0 (friend/factory
  churn). Keep.
- `NULL`, `register`, `pthread_`, `<=>` matches: all in comments or doc text —
  not code.
- `async_mutex_authority_probe.cpp` / `async_condition_authority_probe.cpp`
  "errors" under `-Wextra`: these are sealed-name authority probes whose
  intended diagnostic is a compile failure; their gates compile them with
  `-fsyntax-only` (no `-Wall`). Only relevant if gates adopt `-Wextra`.

## 6. Compiler-floor recommendation

Feature surface actually used (inventory from `grep` over include/ + src/):

| Feature | Minimum full support |
|---------|----------------------|
| concepts / requires / `std::same_as` | Clang 10, GCC 10 |
| designated initializers | Clang 10 (full), GCC 8 |
| `std::span` | libc++ 7, libstdc++ 10 |
| `std::remove_cvref_t` | libc++ 6, libstdc++ 9 |
| `std::atomic::wait/notify` (internal-testing seams) | libc++ 11, **libstdc++ 11** |
| C++17 staples (`std::byte`, `std::optional`, `string_view`, fold expressions, `[[nodiscard]]`) | well below |

The binding constraint is the `atomic::wait` test seam on the library side:
**GCC/libstdc++ ≥ 11, Clang/libc++ ≥ 11** (concepts alone would only demand
GCC 10 / Clang 10). Pairing with distro reality:

- Ubuntu 22.04 LTS: GCC 11, clang-14 (default libstdc++ 11) — works.
- Ubuntu 24.04 LTS: GCC 13, clang-18 — works.
- Fedora 44 local: GCC 16, Clang 22 — verified this audit.

**Recommendation: officially state `GCC >= 11` and `Clang >= 14` as the
supported compiler floor** (Clang 14 chosen over 11 because it is the first
widely-shipped LLVM release with complete C++20 library support and is the
Ubuntu 22.04 default pairing). This is *defensible, not yet frozen*: freeze it
only after a nightly floor job builds the full test group with gcc-11 and
clang-14 (see §7).

Verified this audit: full Clang 22 gate PASS (157/157), full GCC 16 compile
gate PASS (production + all test targets), real-liburing path PASS.

## 7. CI recommendation

Keep the PR merge gate fast; move heavy evidence to nightly.

Merge gate (PR):
- Linux Clang Debug (existing): configure, build core + async, build `-g test`,
  `xmake test -v`, negative-compile × 5, acceptance consumers, docs, backend
  conformance aggregate, manifest self-test.
- **Add: Linux GCC Debug compile-only job** — `xmake f -m debug
  --toolchain=gcc` + `xmake build sluice_core sluice_async` + `xmake build -g
  test` (no test run). Locally ~40 s. Catches GCC-only regressions; the
  toolchain-dependent code paths (TSA flag scoping, fuzz clang gate) already
  exist and deserve a merge-gate check.
- (Optional, cheap) pin `CXX=clang++` on the two remaining negative-compile
  steps (F4).

Nightly / optional (not PR):
- TSan and ASan/UBSan full runs (change-class gates already documented in
  `docs/verification/README.md`).
- Real-liburing full test run (`--with-liburing=true`), including the 8 uring
  test binaries in real mode.
- clang-tidy **no-new-warning baseline** (F1): run the existing check set,
  diff against a committed baseline, fail on new findings. Never
  all-warnings-as-error.
- Compiler-floor job: gcc-11 and clang-14 toolchains (container or
  `llvm.sh`/apt) compiling the full test group.
- Stress loops and formal models stay where they are (formal.yml exists).

Do not turn PR CI into a multi-hour matrix; the repo's evidence split
(AGENTS §6.1: pre-push = fast mechanical; CI = build+test; nightly = heavy) is
the right shape.

## 8. Tests (executed, real results)

| Gate | Command | Result |
|------|---------|--------|
| Clang Debug full | `xmake f -c -m debug --toolchain=clang -y` + build core/async + `build -g test` + `xmake test -v` | 157/157 PASS |
| GCC compile gate | `xmake f -c -m debug --toolchain=gcc -y` + build core/async + `build -g test` | PASS (real GCC objects verified) |
| Fuzz group build | `xmake build -g fuzz` (clang) | PASS |
| Real liburing | `--with-liburing=true`; uring_d2, c2b, c2c, c2e, submit_failure, write_batch, io_context, stats tests | PASS, `[evidence-meta] mode=real` (c2e `backend.available()` true) |
| pre-push | `bash scripts/gates/pre-push.sh` | ALL CHECKS PASSED (incl. 208 unit self-tests) |
| Negative compile ×5 | all `verify-*-negative-compile.sh` with `CXX=clang++` | PASS |
| Docs | `check-doc-links.py --self-test`, full scan, `verify-architecture-docs.py` | PASS |
| `-Wextra` probe | every TU under src/, tests/, examples/, bench/, fuzz/, apps/ | 2 real instances, both FIXED; 2 probe TUs (P3) |

## 9. Residual risks

- Compiler floor is recommended but not yet CI-enforced; a floor regression
  (e.g. a `std::expected` slip, or `atomic::wait` leaking into production
  headers) is not yet caught automatically.
- clang-tidy's 100+ enabled checks are unexecuted; the configuration may
  contain checks that would fire today (no baseline exists to know).
- Stub↔real liburing reconfig cache staleness (F6) can mislead local
  evidence; CI is unaffected (fresh checkout).
- The `-Wextra` strengthening is one `xmake.lua` line + 2 probe `(void)` casts
  away, but it is not yet a gate.

## 10. Recommendation before Phase F

**READY FOR F1** with respect to language-standard and legacy hygiene. The
audit found no P0/P1 issues: no UB, no memory-safety, race, or lifetime bug in
scope, no wrong language mode anywhere, no production semantics bug. The
recommended follow-ups (compiler floor, clang-tidy baseline, GCC merge job,
`-Wextra`) are hygiene hardening, not blockers, and are tracked as an umbrella
issue (see §11).

## 11. Tracking

- Umbrella issue: "C++20 baseline & compiler-floor hardening" (floor + tidy
  baseline + GCC merge job + `-Wextra`).
- This audit's fixes: PR "chore(cpp): audit C++20 baseline and legacy
  compatibility debt".
