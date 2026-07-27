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

1. **Acceptance Testing** — `xmake test -v` on every PR.
2. **Unit / Component Testing** — per-slice test binaries (see `tests/`).
3. **Mutation Testing** — manual targeted mutation evidence was completed for
   E15; automated repository-wide mutation tooling is planned.
4. **Code Quality Analysis** — `clang-tidy`, `.clang-format`, `.clang-tidy`.
5. **Agent Workflow Discipline** — per `AGENTS.md`.

## Formal models

TLA+ models supplement implementation tests; they do not prove C++ code is
bug-free.

- TLA+ specs: `docs/spec/` (per-subsystem), `spec/tla/` (BlockingIoPool)
- TLA+ is abstract protocol evidence only
- A modeled transition change requires model review
- A C++ regression must connect the model property to implementation behavior

**Verification scripts:**

| Script | Status |
|--------|--------|
| `scripts/verify-e11-formal.sh` | Available now |
| `scripts/verify-e12-queue-formal.sh` | Available now |
| `scripts/verify-e12-semaphore-formal.sh` | Available now |
| `scripts/verify-e12-async-mutex-formal.sh` | Available now |
| `scripts/verify-e12-async-condition-formal.sh` | Available now |
| `scripts/verify-e12-event-formal.sh` | Available now |
| `scripts/verify-e12-rwlock-formal.sh` | Available now |
| `scripts/verify-e12-api-contract-negative-compile.sh` | Available now |
| `scripts/verify-external-wake-stability.sh` | Available now |
| `scripts/verify-runnable-steal-stability.sh` | Available now |
| `scripts/verify-select-rollback-stability.sh` | Available now |

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
| Automated mutation testing | **Planned** — no repository-wide tooling yet |

## Navigation

| Topic | Document |
|-------|----------|
| Testing strategy | `docs/verification/testing-strategy.md` *(not yet created)* |
| Fuzzing strategy | `docs/verification/fuzzing-strategy.md` *(not yet created)* |
| Sanitizer matrix | `docs/verification/sanitizer-matrix.md` *(not yet created)* |
| Mutation testing | `docs/verification/mutation-testing.md` *(not yet created)* |
| Formal models | `docs/spec/` |
| io_uring validation | `docs/history/archive/liburing-validation-2026-07-03.md` |
