# DIRECT_COST_DAG — Sluice #600 Phase I (E0)

BASE_SHA: bdfd0d8d4595fd82d1cd4b099af8ed37851e6d57 (master, frozen 2026-10-26)
Platform: E5 native Linux — Intel Xeon E5-2666 v3 (10C/20T, 2.9GHz base / 3.5GHz turbo),
Fedora 44, kernel 7.2.11-200.fc44.x86_64, GCC 16.2.1, glibc ld 2.46.1, btrfs+zstd root,
tmpfs fixture at /tmp/sluice600. Release build: xmake -m release --liburing=n,
`-O3 -DNDEBUG -fvisibility=hidden`, static lib `libsluice_core.a`
sha256 489104b5819cdaca251471d5e0b10c20051071bca88dcb7bfc198f1336c99c5a.
Measurement env: governor=performance @3.5GHz, nmi_watchdog=0, perf_event_paranoid=1.

## Source-level call DAG (Direct, `blocking::read_at`)

```
Application
  └─ sluice::blocking::read_at(const File&, offset, span<byte>)
       ├─ precheck()                       [anonymous-namespace, blocking_file.cpp:30]
       │    └─ detail::precheck_data_op()  [constexpr, file_semantics.hpp:163]
       │         ├─ closed?            fd_ < 0                       → reject_closed  (invalid_state)
       │         ├─ access_allows()    write_only for read           → reject_access  (invalid_argument)
       │         ├─ length == 0                                       → complete_empty (Result{0}, no syscall)
       │         ├─ range_is_valid()   offset/last-byte fit off_t    → reject_range  (invalid_argument)
       │         └─ length > kMaxNativeTransfer (ssize_t max)        → reject_range  (invalid_argument)
       │    └─ buffer == nullptr (execute verdict only)             → invalid_argument
       ├─ detail::checked_posix_offset(offset)  [Result<off_t>]      → invalid_argument (unreachable after range pass)
       ├─ detail::retry_on_eintr(lambda)
       │    └─ native_pread → ::pread(fd, buf, len, off)             [the only syscall]
       │         └─ n < 0 && errno == EINTR → retry
       └─ n < 0 ? from_errno_value(errno) : Result<size_t>{n}
```

Not on this path: RequestCore, io_uring, ProgressSource, Worker, EffectReport publication
(composition publishes EffectCertainty via `direct_outcome` only). Verified by reading
`src/blocking_file.cpp` at BASE_SHA and by symbol table of the shipped object (only
`precheck[.isra]`, `pread`, `__errno_location` externals in read_at's body).

Composition (`read_exact_at`): outer `precheck` once, then `compose()` loop calling
`read_at(file, offset+confirmed, dst.subspan(confirmed))` per step; at -O3 the step body is
INLINED into the loop (direct `pread` call at 0x1497), so every step re-executes the full
validation sequence in addition to the loop's own state machine.

## What each DAG node costs at -O3 (see DIRECT_MACHINE_COST_LEDGER.md)

- precheck_data_op checks: necessary v2 semantics; ~18 instructions fused well by GCC
  (three source range conditions compile to two compares + one or/sign test).
- Precheck struct materialization: implementation artifact, ~25 instructions of stack
  traffic per call on the hot path (two 24-byte copies + reloads of two branch bytes).
- checked_posix_offset: 2 surviving redundant instructions (`test/js` at 0x220) plus the
  Result<off_t> scaffolding folded away — GCC did NOT elide the duplicate compare.
- retry_on_eintr: 2 instructions on success (`test rax; jns`) — necessary.
- Result<size_t> sret: 3 stores + sret register pressure — identical shape emitted for a
  hand-written matched native wrapper, i.e. the ABI cost of the public interface, not
  Sluice-specific excess.
