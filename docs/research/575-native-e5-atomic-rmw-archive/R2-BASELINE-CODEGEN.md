# R2 — Compiler and Binary Authenticity (Baseline Release Codegen)

## Verdict

`NO_EARLY_STOP` for both candidates. Both target atomic RMWs **survive GCC
16.2.1 -O3 optimization** at every level examined: optimized GIMPLE IR,
relocatable objects, the `libsluice_async.a` archive members, and the final
stripped Release benchmark executables. R2's early-stop condition does not
apply; both arms proceed to R3.

The WSL2 GCC 15 preliminary step was executed here on E5 GCC 16.2.1 per the
owner override (`NOT_RUN_BY_OWNER_OVERRIDE` for the WSL2 environment itself).

## Compiler and flags (exact)

- `/usr/lib64/ccache/g++` → GCC 16.2.1 20260819 (Red Hat 16.2.1-2)
- Production TU flags (identical for `sluice_async` members, from
  `compile_commands.json`):
  `-c -m64 -fvisibility=hidden -fvisibility-inlines-hidden -Wall -Werror -O3 -std=c++20 -I <src/async> -I <include> -D SLUICE_LIBURING_PROFILE=on -D SLUICE_HAS_LIBURING -D NDEBUG`
- No LTO, no PGO, no sanitizer in this profile.

## Evidence chain (three levels, all preserved under `assembly/base/`)

### 1. Optimized GIMPLE IR (`-fdump-tree-optimized`)

Regeneration (byte-identical objects to the production build, SHA-verified —
see `sha256-baseline.txt`):

```sh
g++ -c -m64 -fvisibility=hidden -fvisibility-inlines-hidden -Wall -Werror \
    -O3 -std=c++20 -I src/async -I include \
    -D SLUICE_LIBURING_PROFILE=on -D SLUICE_HAS_LIBURING -D NDEBUG \
    -fdump-tree-optimized -o /tmp/r2.o src/async/threadpool_backend.cpp
```

- **TP** (`worker_loop`, immediately after `run_syscall` returns and before
  `pthread_mutex_lock`):
  ```gimple
  outcome = sluice::async::ThreadPoolBackend::run_syscall (&op, &metadata);
  _47 = &MEM[(struct __atomic_base *)this_40(D) + 400B]._M_i;
  __atomic_fetch_add_8 (_47, 1, 0);        // model 0 = relaxed
  _180 = pthread_mutex_lock (_1);
  ```
  Preserved: `assembly/base/gimple-threadpool_backend.worker_loop.optimized`.

- **UR** (`submit_transport_locked`, after the ledger-empty early-out and
  immediately before `io_uring_submit`):
  ```gimple
  if (_8 == 0) goto <bb 7>;                 // ledger empty → early return
  _10 = &MEM[(struct __atomic_base *)this_3(D) + 288B]._M_i;
  __atomic_fetch_add_8 (_10, 1, 0);
  rc_13 = io_uring_submit (_12);
  ```
  Preserved: `assembly/base/gimple-uring_transport.submit_transport_locked.optimized`.

Field offsets: TP `syscall_count_` at **+400 (0x190)**; UR `submit_flushes_`
at **+288 (0x120)** (its siblings `live_cookies_` at +296/0x128 and
`live_control_sqes_` at +304/0x130). The IR offsets and the assembly
displacement operands agree exactly.

### 2. Relocatable objects and archive members

Full disassembly preserved:
`assembly/base/objdump-threadpool_backend.release.txt`,
`assembly/base/objdump-uring_transport.release.txt` (demangled, `--no-show-raw-insn`).

All `lock`-prefixed instructions in the four async objects (exact pattern
`^\s+[0-9a-f]+:\s+lock\s`, so `block`/`clock` strings are not miscounted):

| Object | Instruction | Function (from symbols) | Field |
|---|---|---|---|
| threadpool_backend.cpp.o | `lock addq $0x1,0x190(%r15)` @0x2e1a | `worker_loop()` | **syscall_count_** |
| uring_transport.cpp.o | `lock addq $0x1,0x120(%rdi)` @0x12e7 | `submit_transport_locked()` (out-of-line) | **submit_flushes_** |
| uring_transport.cpp.o | `lock addq $0x1,0x120(%rbx)` @0x1850 | `dispatch_one_locked()` (inlined submit) | **submit_flushes_** |
| uring_transport.cpp.o | `lock addq $0x1,0x120(%r9)` @0x1c73 | `issue_running_cancel_locked_()` (inlined) | **submit_flushes_** |
| uring_transport.cpp.o | `lock addq $0x1,0x120(%rbx)` @0x226b | `dispatch_after_accept()` (inlined submit) | **submit_flushes_** |
| uring_transport.cpp.o | `lock addq $0x1,0x128(%rbx)` @0x14d5 | `dispatch_one_locked()` | live_cookies_ (protocol) |
| uring_transport.cpp.o | `lock subq $0x1,0x128(%rdx)` @0x790 | `retire_router_entry_()` | live_cookies_ (protocol) |
| uring_transport.cpp.o | `lock addq $0x1,0x130(%rdi)` @0x3dc | `mark_consumed_control_locked_()` | live_control_sqes_ (protocol) |
| uring_transport.cpp.o | `lock subq $0x1,0x130(%r15)` @0xf20 | `poison_and_recover_locked()` | live_control_sqes_ (protocol) |
| uring_completion.cpp.o | `lock subq $0x1,0x130(%rbx)` @0x572 | CQ reap path | live_control_sqes_ (protocol) |
| uring_backend.cpp.o | (none) | — | — |

GCC emits `lock addq $0x1` (the exact x86-64 lowering of
`fetch_add(1, relaxed)` on `std::atomic<uint64_t>`). The 3 inlined
`submit_flushes_` increments plus the 1 out-of-line body map one-to-one onto
the four `submit_transport_locked()` call sites found in R1
(uring_transport.cpp:87, 143, 647 and the cross-TU poll call at
uring_backend.cpp:631, which uses the exported out-of-line definition).

### 3. Linked Release executables (stripped)

`bench_w02_pipeline` contains exactly 14 `lock`-prefixed instructions; the
target sites are present (function attribution via byte-identical instruction
neighborhoods against the symbolized archive members, since the binary is
stripped):

```
41c41a:  lock addq $0x1,0x190(%r15)   ← syscall_count_ (worker_loop)
         (preceded by the run_syscall metadata byte sequence, followed by
          mov %rbx,%rdi; call pthread_mutex_lock — identical to the object)
425c67:  lock addq $0x1,0x120(%rdi)   ← submit_flushes_ (out-of-line submit_transport_locked)
4261d0:  lock addq $0x1,0x120(%rbx)   ← submit_flushes_ (inlined, dispatch_one_locked)
4265f3:  lock addq $0x1,0x120(%r9)    ← submit_flushes_ (inlined, issue_running_cancel)
426beb:  lock addq $0x1,0x120(%rbx)   ← submit_flushes_ (inlined, dispatch_after_accept)
```

Example neighborhood match (object @0x2e1a vs linked @0x41c41a):

```
movzbl 0x80(%rsp),%eax / mov %al,0xf(%rsp) / lock addq $0x1,0x190(%r15)
/ mov %rbx,%rdi / call pthread_mutex_lock@plt
```

Preserved: `assembly/base/linked-bench_w02-lockadd-sites.txt`.
`bench_w01_direct` links only `sluice_core` (no async objects) and contains
no sluice lock RMWs — consistent with its role as the negative-control
sentinel.

## Authenticity checksums

See `assembly/base/sha256-baseline.txt` for per-object, archive, and
executable SHA-256 values. Baseline binary SHAs:

```
222b8c2c…e0397  threadpool_backend.cpp.o
3e107900…7d96  uring_transport.cpp.o
f1419abe…c7a  libsluice_async.a
1b962213…edb6  libsluice_core.a
7ea9df98…8b12  bench_w02_pipeline
052befc2…16e0  bench_w03_external_loop
f578f047…1740a  bench_w01_direct
```

The `-fdump-tree-optimized` rebuilds are byte-identical to the production
objects (SHA match), so the IR evidence reflects the production codegen
exactly, not a parallel build.

## Interpretation

- Neither operation is compiler-eliminated: the compiler cannot remove them
  because the field writes are externally visible side effects on an object
  that escapes (backend instance owned by `AsyncIoContext`).
- The generated instruction is a full locked RMW (`lock addq`), not a
  plain `add`: on x86 it drains the store buffer / claims the cache line for
  write regardless of the `relaxed` C++ ordering.
- Cost positioning: TP's RMW executes once per physical syscall (which
  itself costs ≥ hundreds of cycles in the kernel), UR's once per
  `io_uring_submit` flush attempt (a syscall or at least a userspace→kernel
  transition). The locked instruction is therefore several orders of
  magnitude cheaper than the surrounding operation on these paths — this
  frames the R8 materiality analysis, but the end-to-end measurement decides.
