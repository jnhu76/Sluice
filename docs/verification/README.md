# Sluice Verification

## Required PR gate

```bash
xmake f -m debug --toolchain=clang -y
xmake build sluice_core
xmake build sluice_async
xmake build -g test
xmake test -v
```

This is the minimum CI gate (Linux Clang Debug). All production libraries must
build warning-clean.

## Compiler floor

Sluice's minimum language standard is **C++20** and every build path enforces
it (root `set_languages("c++20")`; every script that compiles C++ passes
`-std=c++20` explicitly; as-built `compile_commands.json` is 100% `-std=c++20`).

Supported compiler floor (recommendation, 2026-08 audit —
`docs/verification/audit-cpp20-baseline-legacy-2026-08.md`):

- **GCC >= 11** (libstdc++ 11: concepts, `std::span`, `std::atomic::wait`);
- **Clang >= 14** with a C++20-capable standard library — libstdc++ >= 11 or
  libc++ >= 11 (the `atomic::wait`-using internal-testing seams require it).
  The Clang >= 14 claim is validated on the Ubuntu 22.04 pairing
  (clang-14 + libstdc++ 11); other pairings need the same stdlib floor.

The floor is enforced by CI only for the current ubuntu-latest Clang; a
nightly gcc-11 / clang-14 floor job is the pending follow-up before the floor
is frozen.

## Change-class-specific gates

### Public headers, templates, noexcept, assertions, API contracts

```bash
xmake f -m release --toolchain=clang -y
xmake build sluice_core
xmake build sluice_async
xmake build -g test
xmake test -v
```

### Memory ownership, parsing, allocation, buffer lifetime, filesystem

```bash
xmake f -m asanubsan --toolchain=clang -y
xmake build sluice_core
xmake build sluice_async
xmake build -g test
xmake run -g test
```

### Scheduler, synchronization, cancellation, queues, wakeups, multi-worker

```bash
xmake f -m tsan --toolchain=clang -y
xmake build sluice_core
xmake build sluice_async
xmake build -g test
xmake run -g test
```

### Build system or CI changes

Prove: `sluice_core` builds independently, `sluice_async` builds independently,
complete test group builds, test failures propagate, optional feature gates
remain off by default.

### io_uring changes

Validate default stub/off path. When liburing available, also configure with
`--with-liburing=true` and report real-path evidence separately.

## Sanitizer matrix

| Mode | Flag | What it catches |
|------|------|----------------|
| asan | `-fsanitize=address` | Out-of-bounds, use-after-free, double-free |
| tsan | `-fsanitize=thread` | Data races, deadlocks |
| ubsan | `-fsanitize=undefined` | Signed overflow, null dereference, alignment |
| asanubsan | ASan + UBSan combined | Both |
| valgrind | debug + `-O3` | Memory leaks, invalid reads/writes |

## Five-layer quality model

1. **Acceptance Testing** — public-only compile+run probes that prove the
   installed headers are usable end-to-end:
   - `xmake build public_api_acceptance && xmake run public_api_acceptance`
   - `xmake build async_foundation_quickstart && xmake run async_foundation_quickstart`
   - E16 application acceptance consumer:
     `xmake build runtime_acceptance && xmake run runtime_acceptance`
   - M1-A reference application (public-only consumer):
     `xmake build sluice-copy` (and run as a production binary)
2. **Unit / Component Testing** — `xmake build -g test && xmake test -v`
   (per-slice test binaries in `tests/`). M1-A added:
   `runtime_wait_test` (Runtime cooperative Completion wait),
   `sluice_copy_integration_test` (real-file copy),
   `sluice_copy_fault_test` (FakeAsyncBackend fault injection).
3. **Mutation Testing** — manual targeted mutation evidence was completed for
   E15; automated repository-wide mutation tooling is planned.
4. **Code Quality Analysis** — `clang-tidy`, `.clang-format`, `.clang-tidy`.
5. **Agent Workflow Discipline** — per `AGENTS.md`.

## Formal models

TLA+ models supplement implementation tests; they do not prove C++ code is
bug-free.

- TLA+ specs: `spec/tla/` (per-suite directory)
- TLA+ is abstract protocol evidence only
- A modeled transition change requires model review
- A C++ regression must connect the model property to implementation behavior
- Unified orchestrator: `python3 scripts/formal/verify.py`

**Verification scripts:**

| Script | Status |
|--------|--------|
| `scripts/formal/verify-timer-wait.sh` | Available now |
| `scripts/formal/verify-async-queue.sh` | Available now |
| `scripts/formal/verify-async-semaphore.sh` | Available now |
| `scripts/formal/verify-async-mutex.sh` | Available now |
| `scripts/formal/verify-async-condition.sh` | Available now |
| `scripts/formal/verify-event.sh` | Available now |
| `scripts/formal/verify-async-rwlock.sh` | Available now |
| `scripts/formal/verify-e13-select-core.sh` | Available now |
| `scripts/formal/verify-e13-select-safety.sh` | Available now |
| `scripts/formal/verify-blocking-io-pool.sh` | Available now |
| `scripts/formal/verify-e7-publication.sh` | Available now |
| `scripts/formal/verify-e8-ownership-transfer.sh` | Available now |
| `scripts/formal/verify-e9-park-wake.sh` | Available now |
| `scripts/formal/verify-e10-waitnode.sh` | Available now |

## Weak-memory model checking

Bounded kernels carrying the exact production atomic ordering, checked
exhaustively under GenMC (RC11 and RA+RLX), with mandatory broken-order
negative controls — a **separately-run evidence layer** (not wired into CI or
pre-push; #197 non-goal). Current kernel:
[Completion publication/reset](weak-memory/completion-publication-kernel.md)
(`scripts/weakmem/verify-completion-weak-memory.sh`). Claim vocabulary:
`MEMORY-MODEL-CHECKED (BOUNDED KERNEL)` — never a whole-program claim.

## Evidence status

| Evidence | Status |
|----------|--------|
| Clang Debug test gate | **CI required gate** — runs on every PR |
| Clang Release gate | **Available now** — local extended gate |
| ASan / UBSan | **Available now** — local extended gate |
| TSan | **Available now** — local extended gate |
| Valgrind | **Environment-dependent** — requires valgrind installed |
| Real liburing validation | **Environment-dependent** — requires liburing-equipped host |
| TLA+ TLC model checker | **Environment-dependent** — requires `tla2tools.jar` |
| GenMC weak-memory kernels | **Environment-dependent** — requires GenMC (user-local build documented in the kernel evidence doc) |
| Automated mutation testing | **Planned** — no repository-wide tooling yet |

## Stable evidence identifiers (issue #167 Step 5, 2026-08-25)

The `phase-c2b..c2e` / `phase-d2..d4` mutation-evidence filenames under this
directory are **stable identifiers, not phase-era sediment**: each is
machine-pinned as `evidence[].ref` rows in `failure-envelope.json` (validated
by `scripts/gates/failure-envelope.py` in pre-push) and referenced by the
compliance gates, the divergence registry, and `guarantee-cost.md`.
**Decision: KEEP — do not rename.** Step 5 completion does not require
renaming stable evidence identifiers.

## Navigation

| Topic | Document |
|-------|----------|
| Formal models | `spec/tla/` (inventory: `spec/tla/manifest.json`) |
| Formal model documentation | [`formal-models.md`](formal-models.md), [`formal/`](formal/) |
| Weak-memory kernel evidence (#197) | [`weak-memory/completion-publication-kernel.md`](weak-memory/completion-publication-kernel.md) |
| Failure envelope matrix (#198) | [`failure-envelope.md`](failure-envelope.md) (artifact: [`failure-envelope.json`](failure-envelope.json); gate: `scripts/gates/failure-envelope.py`) |
| Guarantee-cost vectors + sustained overload (#199) | [`guarantee-cost.md`](guarantee-cost.md) (artifact: [`v6-overload-backpressure.json`](../results/performance-attribution/v6-overload-backpressure.json); runner `scripts/bench/perf-attribution.py overload`) |
| io_uring / liburing validation runbook | [`io-uring-liburing-validation.md`](io-uring-liburing-validation.md) |
| Local hardening gate | [`hardening.md`](hardening.md) |
| Sync benchmark methodology (W1–W4) | [`sync-bench-methodology.md`](sync-bench-methodology.md) |
| Sync benchmark matrix | [`sync-bench-matrix.md`](sync-bench-matrix.md) |
| Performance engineering methodology (governing) | [`performance-engineering.md`](performance-engineering.md) |
| Performance attribution framework (APP/CORE ladder + grep round 1) | [`performance-attribution.md`](performance-attribution.md) |
