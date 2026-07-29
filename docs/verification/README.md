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
| Formal models | `spec/tla/` |
| io_uring validation | `docs/history/archive/liburing-validation-2026-07-03.md` |
