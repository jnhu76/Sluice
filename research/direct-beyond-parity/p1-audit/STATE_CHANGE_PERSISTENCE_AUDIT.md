# P1 Static Cost Audit — State-Change & Persistence Group (resize / sync_data / sync_all)

- Group: state-change-persistence (resize, sync_data, sync_all on the Direct path).
- Audit date: 2026-10-27. Auditor: P1 subagent (this session).
- Audited baseline: **B1** — worktree `/home/jnhu/Source/Sluice-600-wt`, HEAD
  `77e5399a45998dea396699fa2e063c94a665945a`, archive
  `build/linux/x86_64/release/libsluice_core.a` sha256
  `d1e351a54ded3dfa1df6b1cd2e47622eefbd5d7027439cb31a314bed3a3007f0`
  (re-verified this session with `sha256sum` — matches the P0 §1.3 frozen
  anchor; worktree used read-only, no reconfiguration or rebuild performed
  inside it).
- Comparator: **N1-pattern matched-native control**, rebuilt this session at
  `/tmp/sluice-p1-audit/n1_matched.cpp` following the #600 A1-arm discipline
  (probe snapshot `research/direct-beyond-parity/probe-snapshot-600/ladder.cpp:35-63`):
  hand-written minimal body, same check order, same error codes, EINTR retry,
  same `Result<void>` ABI, separate TU, public Sluice headers only.
- Contract authority: `docs/explicit-io-contract.md` (v2-r1) at B1; requirement
  IDs cited as `file:line` anchors.
- Method: static disassembly of the shipped archive objects (`objdump -d`,
  relocation resolution via `readelf -r`), instruction/branch/call/load-store
  counting on the warm success path, cold-path shape description, and a
  same-flags compile of the matched-native control. **No timing was measured in
  this audit** (static level only); all #600 timing figures cited below are
  `[TRANSCRIBED]` from `DIRECT_MACHINE_COST_LEDGER.md`.

Reproduction anchors (all commands run this session from `/tmp`):

```sh
sha256sum /home/jnhu/Source/Sluice-600-wt/build/linux/x86_64/release/libsluice_core.a
# d1e351a54ded3dfa1df6b1cd2e47622eefbd5d7027439cb31a314bed3a3007f0

objdump -d --no-show-raw-insn \
  /home/jnhu/Source/Sluice-600-wt/build/linux/x86_64/release/libsluice_core.a
ar x /home/jnhu/Source/Sluice-600-wt/build/linux/x86_64/release/libsluice_core.a
readelf -r blocking_file.cpp.o            # call-target resolution

CCACHE_DISABLE=1 /usr/lib64/ccache/gcc -m64 -fvisibility=hidden -Wall -Werror \
  -O3 -std=c++20 -DNDEBUG -I/home/jnhu/Source/Sluice-600-wt/include \
  -c n1_matched.cpp -o n1_matched.o       # flags = P0 §1.2/§1.5 recorded build flags
objdump -d --no-show-raw-insn n1_matched.o
```

Symbols audited in `blocking_file.cpp.o` (second archive member):

| Op | Symbol | Offset | Size | Success-path call (reloc) |
|---|---|---|---|---|
| sync_data | `_ZN6sluice8blocking9sync_dataERKNS_4FileE` | 0x820 | 0x110 | `call` @0x82e → reloc 0x82f = **fdatasync** |
| sync_all | `_ZN6sluice8blocking8sync_allERKNS_4FileE` | 0xe20 | 0x110 | `call` @0xe2e → reloc 0xe2f = **fsync** |
| resize | `_ZN6sluice8blocking6resizeERKNS_4FileEm` | 0xcb0 | 0x163 | `call` @0xce6 → reloc 0xce7 = **ftruncate** |

All three error paths call `__errno_location` (relocs 0x83c / 0xe3c / 0xcf0).

---

## 1. `sync_data`

### 1.1 Path chain (Public API → semantic validation → internal representation → native → Result)

1. **Public API**: `Result<void> sync_data(const File&)` —
   `include/sluice/blocking/file.hpp:23`; implementation
   `src/blocking_file.cpp:156-169`.
2. **Semantic validation**: `detail::precheck_state_op(!file.is_open(),
   file.access(), FileOperation::sync_data)` at `src/blocking_file.cpp:157-159`
   → `precheck_state_op` `include/sluice/detail/file_semantics.hpp:178-185`
   (closed → `reject_closed`; then `access_allows` `file_semantics.hpp:54-68`,
   whose `sync_data`/`sync_all`/`file_info` arm at `file_semantics.hpp:63-65`
   returns `true` for every `FileAccess` value — the SEM-03 legality-table row
   "sync_data/sync_all: Legal ×3" compiles to **zero instructions**).
   Rejection mapping `rejection_of`/`rejection_error` (`file_semantics.hpp:131-135`,
   `:116-129`) — folded into the reject block, no success-path cost.
3. **Internal representation**: `DataOpVerdict` (`file_semantics.hpp:103-109`),
   a scalar `uint8_t` enum held in a register. Post-C1 there is **no struct
   materialization or memory round-trip** of any precheck value on this path
   (the B0-era `Precheck{optional<IoError>, bool}` stack round-trip that #600
   attributed ≈27 instructions on `read_at` does not exist here).
4. **Native operation**: `::fdatasync(file.native_handle())` wrapped in
   `detail::retry_on_eintr` (`src/blocking_file.cpp:164`; loop shape
   `include/sluice/detail/posix_retry.hpp:7-15`). `File::native_handle()` /
   `is_open()` / `access()` are `file_resource.hpp:86/84/88`.
5. **Result**: `Result<void>` (`include/sluice/result.hpp:145-158`; storage
   `:92-105` — `{IoError error_; bool has_value_}`, 12 bytes, trivially
   copyable) returned in `%rax:%rdx` (no sret). Success → `{}`; native failure
   → `from_errno_value(errno)` (`src/blocking_file.cpp:165-167`; mapping table
   `include/sluice/error.hpp:66-94`).

### 1.2 Machine ledger (success path, executed instructions)

Shape at `0x820`:

```
820: push %rbx; 821: mov %rdi,%rbx; 824: sub $0x30,%rsp   ; prologue, 48B frame
828: mov (%rdi),%edi; 82a: test %edi,%edi; 82c: js 8a8    ; closed check (fd sign)
82e: call fdatasync                                       ; the syscall
833: test %eax,%eax; 835: jns 8d0                         ; rc test
8d0: movb $1,0x20(%rsp); 8d5: mov 0x20(%rsp),%eax;        ; Result success block
8d9: movq $0x0,0x24(%rsp); 8e2: mov %eax,0x2c(%rsp)
8e6: jmp 8ba
8ba: mov 0x2c(%rsp),%ecx; 8be: mov 0x24(%rsp),%rax        ; merge tail reload
8c3: add $0x30,%rsp; 8c7: pop %rbx; 8c8: mov %rcx,%rdx; 8cb: ret
```

| Metric (success path) | Sluice B1 | N1 control | Δ |
|---|---|---|---|
| Executed instructions | **20** | **19** | +1 (layout `jmp`) |
| Conditional branches executed | 2 (`js` nt, `jns` t) | 2 | 0 |
| Unconditional jumps | 1 (`jmp 8ba`, layout) | 0 | +1 |
| Calls | 1 (`fdatasync`) | 1 | 0 |
| Loads | 4 (`(%rdi)`, `0x20`, `0x2c`, `0x24`) | 4 | 0 |
| Stores | 3 (`0x20`b, `0x24`q, `0x2c`b) | 3 | 0 |
| Stack frame | 0x30 + saved `%rbx` | 0x30 + saved `%rbx` | 0 |
| Static instructions (whole function) | 69 | 68 | +1 |

The single Δ is the always-predicted `jmp` at `0x8e6`, a basic-block layout
artifact (GCC placed the merge tail after the cold error block); the N1
control's own layout contains the same class of jump on its *resize* success
path (§3.2). N1's error block ordering differs, nothing else does.

**Error path / EINTR shape** (cold; identical in N1):
`call __errno_location; mov (%rax),%eax` (`0x83b-0x840`), then an unrolled
9-way compare chain — EINTR (`cmp $0x4` @`0x842`) checked **first**, then
errno 0, 13(EACCES), 1(EPERM), 2(ENOENT), 20(ENOTDIR), 28(ENOSPC),
122(EDQUOT), 11(EAGAIN) — each mapping arm is
`mov $imm,%edx` (relocated into `kNativeErrorMappings` entries,
`error.hpp:66-78`) + `movzbl 0x4(%rdx),%edx` + 3 stores into the failure
`Result` (`0x88e-0x8a0`). On EINTR: reload fd from saved `&file`
(`0x8f0: mov (%rbx),%edi`) and jump back to the call — exactly one re-call
per interruption, never replaying a confirmed count.

### 1.3 Classification

| # | Class | Where | Machine evidence | Semantic basis (v2-r1) |
|---|---|---|---|---|
| 1 | SEMANTICALLY_REQUIRED | `src/blocking_file.cpp:157-159` → `file_semantics.hpp:178-181` | 3 instr @0x828-0x82c (fd load + sign test + branch); reject block @0x8a8 stores `invalid_state` (6) | SEM-03 step 1 "Closed File: `invalid_state`" (`docs/explicit-io-contract.md:412`); oracle `sync_data_after_close_reports_invalid_state` (`tests/blocking_file_sync_data_test.cpp:87-107`) |
| 2 | KERNEL_FLOOR | `src/blocking_file.cpp:164` | 1 `call fdatasync` @0x82e (reloc 0x82f) | SEM-06: sync_data coverage of completed writes + completed resize is defined by the completed native sync (`explicit-io-contract.md:485-497`); no userspace substitute exists. One syscall/op — matches #600 `EXCESS_SYSCALLS: 0` caliber |
| 3 | SEMANTICALLY_REQUIRED | `src/blocking_file.cpp:164` + `posix_retry.hpp:7-15` | 2 instr on success (`test %eax,%eax; jns` @0x833-0x835); retry arm `cmp $0x4` @0x842 + fd reload @0x8f0 | SEM-05 "may retry EINTR only when the underlying attempt reports interruption without a successful byte count" (`explicit-io-contract.md:469-471`) |
| 4 | ABI_OR_COMPILER_COST | `result.hpp:92-105,145-158`; merge at `src/blocking_file.cpp:168` | 14 of 20 success instr are prologue/epilogue + `Result<void>` store-reload shape; includes a 3-instr bool round-trip (`movb $1,0x20; mov 0x20,%eax; mov %eax,0x2c` @0x8d0-0x8e2) where 1 would do — **identical in the N1 control** (`n1.dis` @0x27-0x39): GCC SRA does not scalarize the merged `Result<void>` construction in either arm. #600 D3 precedent: necessary public-interface cost, `NO_CHANGE_JUSTIFIED` | ERR-02 "For scalar/void operations, the result distinguishes success from error without inventing a byte count" (`explicit-io-contract.md:577-579`); `Result<void>` is the public ABI (`blocking/file.hpp:23`) |
| 5 | SEMANTICALLY_REQUIRED (cold) | `src/blocking_file.cpp:165-167` → `error.hpp:66-94` | Unrolled errno chain + `__errno_location` call + 7 table-load arms @0x83b-0x929; identical in N1 | ERR-01 canonical mapping + native errno preservation (`explicit-io-contract.md:556-560`); oracle `sync_data_on_device_reports_backend_error` (`tests/blocking_file_sync_data_test.cpp:150-165`, EINVAL → `backend_error` + `os_errno`) |
| 6 | ABI_OR_COMPILER_COST | layout of `src/blocking_file.cpp:156-169` | +1 executed `jmp 8ba` @0x8e6 vs N1 (Δstatic +1); block-placement artifact only | None — not semantic work; recorded for completeness, not as excess |

DUPLICATE_WORK / REPRESENTATION_OVERHEAD / COMPOSITION_OVERHEAD /
TRANSFORMABLE_WORK: **none found** on this op.

### 1.4 Parity judgment vs N1 pattern

**PARITY_REACHED.** Executed-instruction Δ = +1 (an always-predicted layout
jump; sub-ns class, far below #600's ≈2.5–3 ns minimum detectable effect,
`DIRECT_MACHINE_COST_LEDGER.md` A/A calibration), identical loads/stores/
branches/calls/frame shape, identical EINTR and errno-mapping cold paths. The
N1 control carries the same `Result<void>` round-trip artifact, so by the #600
A1 discipline this is interface/compiler cost, not Sluice excess. No B0-class
structure (Precheck materialization, redundant sign test) survives on this
path after C1.

---

## 2. `sync_all`

### 2.1 Path chain

1. **Public API**: `Result<void> sync_all(const File&)` —
   `include/sluice/blocking/file.hpp:25`; implementation
   `src/blocking_file.cpp:216-229`.
2. **Semantic validation**: same `precheck_state_op` chain with
   `FileOperation::sync_all` (`src/blocking_file.cpp:217-219`;
   `file_semantics.hpp:178-185`, access arm folds to constant-true) — zero
   success-path cost beyond the closed check.
3. **Internal representation**: scalar `DataOpVerdict`; no materialization.
4. **Native**: `::fsync(file.native_handle())` in `retry_on_eintr`
   (`src/blocking_file.cpp:224`).
5. **Result**: `Result<void>` success `{}` / `from_errno_value(errno)`
   (`src/blocking_file.cpp:225-228`).

### 2.2 Machine ledger

Byte-for-byte the same code shape as `sync_data` (symbol at `0xe20`, size
0x110; `call fsync` @0xe2e, reloc 0xe2f; `__errno_location` @0xe3b, reloc
0xe3c):

| Metric (success path) | Sluice B1 | N1 control (`n1_sync_all`) | Δ |
|---|---|---|---|
| Executed instructions | **20** | **19** | +1 (layout `jmp` @0xee6) |
| Cond branches / calls | 2 / 1 (`fsync`) | 2 / 1 | 0 |
| Loads / stores | 4 / 3 | 4 / 3 | 0 |
| Frame | 0x30 + `%rbx` | 0x30 + `%rbx` | 0 |
| Static instructions | 69 | 68 | +1 |

Error/EINTR shape: identical unrolled errno chain and fd-reload EINTR arm
(`0xef0`), identical to `sync_data` and to N1.

### 2.3 Classification

Same five entries as `sync_data` (§1.3) with these op-specific anchors:

| # | Class | Where | Machine evidence | Semantic basis |
|---|---|---|---|---|
| 1 | SEMANTICALLY_REQUIRED | `src/blocking_file.cpp:217-219` | 3 instr @0xe28-0xe2c | SEM-03 step 1 (`explicit-io-contract.md:412`); oracle `sync_all_after_close_reports_invalid_state` (`tests/blocking_file_sync_all_test.cpp:176`, same 5-test layout as the sync_data suite, `:175-179`) |
| 2 | KERNEL_FLOOR | `src/blocking_file.cpp:224` | 1 `call fsync` @0xe2e | SEM-06 "sync_all includes all sync_data coverage and adds the file's broader metadata" (`explicit-io-contract.md:499-503`) — the metadata increment is carried by the syscall choice, zero extra userspace |
| 3 | SEMANTICALLY_REQUIRED | `src/blocking_file.cpp:224` | `test/jns` @0xe33-0xe35 + EINTR arm @0xe42/0xef0 | SEM-05 (`explicit-io-contract.md:469-471`) |
| 4 | ABI_OR_COMPILER_COST | `result.hpp:92-105` | Same 14-instr ABI/prologue/epilogue + bool round-trip @0xed0-0xee2; identical in N1 | ERR-02 void-result rule (`explicit-io-contract.md:577-579`) |
| 5 | SEMANTICALLY_REQUIRED (cold) | `src/blocking_file.cpp:225-228` | errno chain @0xe3b-0xf29 | ERR-01 (`explicit-io-contract.md:556-560`) |
| 6 | ABI_OR_COMPILER_COST | layout | +1 layout `jmp` @0xee6 | — |

No DUPLICATE_WORK / REPRESENTATION_OVERHEAD / COMPOSITION_OVERHEAD /
TRANSFORMABLE_WORK found.

### 2.4 Parity judgment

**PARITY_REACHED** — same evidence class as §1.4 (Δ = +1 layout jump,
everything else identical to the N1 control).

---

## 3. `resize`

### 3.1 Path chain

1. **Public API**: `Result<void> resize(const File&, std::uint64_t)` —
   `include/sluice/blocking/file.hpp:31`; implementation
   `src/blocking_file.cpp:201-214`.
2. **Semantic validation**: `detail::precheck_resize(!file.is_open(),
   file.access(), new_size)` at `src/blocking_file.cpp:203` →
   `file_semantics.hpp:187-197`: (a) `precheck_state_op` (`:178-185`) gives
   closed → `invalid_state`, then (b) `access_allows(..., resize)`
   (`:60-61`): read_only → `invalid_argument`, then (c)
   `size_is_representable(new_size)` (`:41-43`): `new_size > off_t max` →
   `reject_range` → `invalid_argument`. `is_rejection`/`rejection_error`
   (`:111-129`) fold into the three reject blocks.
3. **Internal representation**: scalar `DataOpVerdict`; the representability
   verdict is carried in flags/registers — no materialization.
4. **Native**: `::ftruncate(file.native_handle(), static_cast<off_t>(new_size))`
   in `retry_on_eintr` (`src/blocking_file.cpp:208-209`); the narrowing cast is
   justified by the execute verdict (comment at `:207`).
5. **Result**: `Result<void>` (`src/blocking_file.cpp:210-213`).

### 3.2 Machine ledger

Shape at `0xcb0`:

```
cb0: sub $0x48,%rsp                                        ; 72B frame
cb4: mov (%rdi),%eax; cb6: test; cb8: js dc0               ; closed check
cbe: cmpb $0x0,0x4(%rdi); cc2: je da2                      ; access == read_only?
cc8: mov %rbx,0x38(%rsp); ccd: mov %rsi,%rbx               ; spill old rbx; new_size→rbx
cd0: mov %r14,0x40(%rsp); cd5: mov %rdi,%r14               ; spill old r14; &file→r14
cd8: test %rsi,%rsi; cdb: js d98                           ; representability (sign test)
ce1: mov %rbx,%rsi; ce4: mov %eax,%edi
ce6: call ftruncate                                        ; the syscall
ceb: test %eax,%eax; ced: jns d60
d60: mov 0x38(%rsp),%rbx; d65: mov 0x40(%rsp),%r14         ; restore callee-saved
d6a: movb $1,0x20(%rsp); d6f: movq $0,0x24(%rsp)           ; Result success block
d78: mov 0x20(%rsp),%eax; d7c: mov %eax,0x2c(%rsp)         ; bool round-trip
d80: mov 0x2c(%rsp),%ecx; d84: mov 0x24(%rsp),%rax         ; merge tail
d89: add $0x48,%rsp; d8d: mov %rcx,%rdx; d90: ret
```

| Metric (success path) | Sluice B1 | N1 control (`n1_resize`) | Δ |
|---|---|---|---|
| Executed instructions | **28** | **28** | **0** |
| Conditional branches executed | 4 (`js` nt, `je` nt, `js` nt, `jns` t) | 4 (`js` nt, `jne` t, `jns` t, `js` nt) | 0 |
| Unconditional jumps | 0 | 1 (`jmp 1ea`, layout — mirror image of the sync-op case) | ∓1 (net 0 executed total) |
| Calls | 1 (`ftruncate`) | 1 | 0 |
| Loads | 7 (`(%rdi)`, `0x4(%rdi)`, `0x38`, `0x40`, `0x20`, `0x2c`, `0x24`) | 7 (same fields) | 0 |
| Stores | 5 (`0x38`, `0x40`, `0x20`b, `0x24`q, `0x2c`b) | 5 | 0 |
| Stack frame | 0x48 | 0x48 | 0 |
| Static instructions | 87 | 85 | +2 (reject/EINTR block layout) |

GCC canonicalized `new_size <= kMaxNativeOffset` (unsigned compare) into the
sign test `test %rsi,%rsi; js` — **identically** in N1 (`n1.dis` @0x208-0x20b,
where the source wrote `new_size > kMaxOff`). This is the #600 D1 site class,
but here the sign test **is** the representability obligation itself, not a
duplicate of an already-proven bound: no D1-class redundancy exists.

The two callee-saved registers (`rbx` = new_size, `r14` = `&file`) exist
because both values must stay live across the call for the EINTR retry arm
(`0xdd0: mov (%r14),%eax; jmp ce1` reloads fd and re-calls). N1 makes the
identical choice with `rbx`/`rbp` (`n1.dis` @0x1c4-0x205, restore @0x244,
reload @0x224). The `mov`-spill idiom (instead of `push`) costs the same 2
stores + 2 loads as N1's.

**Error path**: errno chain at `0xcef-0xe0c` (same unrolled 9-way chain +
`__errno_location` + table loads as the sync ops); reject blocks:
closed → `invalid_state` (6) at `0xdc0-0xdc5`; access/size →
`invalid_argument` (8) at `0xda2-0xdb8`. EINTR: `cmp $0x4` @`0xcf6-0xcf9`,
reload + re-call @`0xdd0-0xdd3`.

### 3.3 Classification

| # | Class | Where | Machine evidence | Semantic basis |
|---|---|---|---|---|
| 1 | SEMANTICALLY_REQUIRED | `src/blocking_file.cpp:203` → `file_semantics.hpp:189-191` | 3 instr @0xcb4-0xcb8 | SEM-03 step 1 (`explicit-io-contract.md:412`); oracle `resize_after_close_reports_invalid_state` (`tests/blocking_file_state_test.cpp:298`) |
| 2 | SEMANTICALLY_REQUIRED | `file_semantics.hpp:60-61` (via `:189`) | 2 instr @0xcbe-0xcc2 (access byte load + branch) | SEM-03 legality table: resize read_only → `invalid_argument` (`explicit-io-contract.md:405`); oracle `resize_on_read_only_reports_invalid_argument` (`tests/blocking_file_state_test.cpp:274`) |
| 3 | SEMANTICALLY_REQUIRED | `file_semantics.hpp:41-43,193-195` | 2 instr @0xcd8-0xcdb (sign test); reject → `invalid_argument` @0xd98→0xda2 | SEM-04 "resize sizes … native representation and checked arithmetic must not overflow" + range rejection `invalid_argument` (`explicit-io-contract.md:440-446`); oracle `resize_overflow_reports_invalid_argument` (`tests/blocking_file_state_test.cpp:320`), registered mutant `SLUICE_SEM_MUTANT_ADMIT_UNREPRESENTABLE_SIZE` (`file_semantics.hpp:192`) |
| 4 | KERNEL_FLOOR | `src/blocking_file.cpp:208-209` | 1 `call ftruncate` @0xce6 (reloc 0xce7) | SEM-07 grow-zero-fill/shrink-discard are the filesystem contract (`explicit-io-contract.md:523-525`); oracles `resize_grow_zero_fills`/`resize_shrink_truncates` (`tests/blocking_file_state_test.cpp:176,205`) |
| 5 | SEMANTICALLY_REQUIRED | `src/blocking_file.cpp:208` + `posix_retry.hpp:7-15` | Cross-call liveness: 2 spills + 2 restores (@0xcc8-0xcd5, 0xd60-0xd65) + rc test (@0xceb-0xced) + EINTR reload/re-call (@0xdd0) — every one of these instructions also present in N1 (n1.dis @0x1c4-0x205, 0x22e-0x230, 0x224) | SEM-05 EINTR retry (`explicit-io-contract.md:469-471`) — new_size and &file must survive the call to re-dispatch |
| 6 | ABI_OR_COMPILER_COST | `result.hpp:92-105`; frame layout | Result success block + bool round-trip (3 instr @0xd6a-0xd7c where 1 would do — identical in N1 @0x232-0x249); frame 0x48; prologue/epilogue `sub/add` | ERR-02 void-result rule (`explicit-io-contract.md:577-579`); public ABI `blocking/file.hpp:31` |
| 7 | SEMANTICALLY_REQUIRED (cold) | `src/blocking_file.cpp:210-212` | errno chain @0xcef-0xe0c | ERR-01 (`explicit-io-contract.md:556-560`); oracle `resize_on_device_reports_backend_error` (`tests/blocking_file_state_test.cpp:340`) |

No DUPLICATE_WORK / REPRESENTATION_OVERHEAD / COMPOSITION_OVERHEAD /
TRANSFORMABLE_WORK found. The +2 static-instruction delta vs N1 sits entirely
in reject/EINTR block layout (jmp-vs-fallthrough), zero on the warm path.

### 3.4 Parity judgment

**PARITY_REACHED.** Executed Δ = 0 (28 vs 28), identical loads/stores/
branches/calls/frame. The only static difference is cold-block layout.

---

## 4. Cross-group observations

1. **C1 generalizes to this group.** All three ops dispatch their admission
   through scalar-enum verdicts (`DataOpVerdict`) with zero struct
   materialization, zero memory round-trips, and no redundant sign tests. The
   #600 B0-class excess (≈27-instr Precheck round-trip + D1 duplicate test on
   `read_at`) has no analogue on any state-change/persistence path at B1.
2. **Syscall discipline**: exactly 1 syscall per op on the success path
   (`fdatasync`/`fsync`/`ftruncate`), re-invoked only on EINTR — consistent
   with #600's `EXCESS_SYSCALLS: 0` for the read path. Any real invocation of
   these ops is dominated by the KERNEL_FLOOR syscall (µs–ms scale), so the
   ≤1-instruction userspace delta is arithmetically invisible at application
   level (cf. #600 R1 arithmetic, `DIRECT_MACHINE_COST_LEDGER.md` E4).
3. **The sync-op access check costs nothing**: `access_allows` folds to
   constant `true` for sync ops because SEM-03 (`explicit-io-contract.md:406`)
   makes them legal in all three access modes — the legality-table row is
   enforced by absence, which is the correct encoding of "Legal ×3".
4. **`Result<void>` bool round-trip** (`movb→stack; reload; store`) is a GCC
   SRA artifact of the merged return construction, present **in the N1 control
   as well** — per #600's D3 verdict this is necessary public-interface /
   compiler cost, not Sluice-specific excess. A source-level transformation
   that avoids it is not known to exist within the same semantics, and would
   have to beat the N1 bar, not just the Sluice bar.

## 5. Gaps

**None.** All three operations are PARITY_REACHED against the N1-pattern
matched-native control at the static machine level:

| Op | Sluice success instr | N1 success instr | Δ executed | Verdict |
|---|---|---|---|---|
| sync_data | 20 | 19 | +1 layout jmp | PARITY_REACHED |
| sync_all | 20 | 19 | +1 layout jmp | PARITY_REACHED |
| resize | 28 | 28 | 0 | PARITY_REACHED |

No gap candidates are registered. Timing was not measured in this audit; given
Δ ≤ 1 always-predicted jump and identical memory/branch/call structure, any
residual timing difference is bounded far below #600's ≈2.5–3 ns minimum
detectable effect and is not attributable to any source-level structure.

## 6. Mutation-seam and oracle inventory for this group (current state at B1)

- Registered direct mutant seams (from `include/sluice/detail/file_semantics.hpp`
  + `xmake/tests.lua:91-99`): exactly three —
  `SLUICE_SEM_MUTANT_EMPTY_BEFORE_CLOSED` and `SLUICE_SEM_MUTANT_ADMIT_INVALID_RANGE`
  in `precheck_data_op` (`file_semantics.hpp:158,168`), and
  `SLUICE_SEM_MUTANT_ADMIT_UNREPRESENTABLE_SIZE` in `precheck_resize`
  (`file_semantics.hpp:192`) — the last is this group's only seam, killed by
  `resize_overflow_reports_invalid_argument` (`tests/blocking_file_state_test.cpp:320`,
  registered `:406`).
- **sync_data/sync_all have no registered mutant seam**: no mutant flips their
  closed-check, and no mutant covers "never fabricate successful
  synchronization" (SEM-06 `explicit-io-contract.md:515`).
- **Fault-injection does not reach these native calls**: the seam enum
  `NativeCall` (`src/file_test_seams.hpp:16-22`) lists only
  close/read/pread/write/pwrite — `fdatasync`/`fsync`/`ftruncate` are not
  interceptable. `tests/direct_composition_fault_test.cpp` (grep: 0 hits for
  sync/truncate/resize) therefore cannot exercise the EINTR arms or errno
  mapping of this group; those paths are currently covered only by real-device
  oracles (`/dev/null` EINVAL sync at `blocking_file_sync_data_test.cpp:150-165`
  and the sync_all twin; `resize_on_device_reports_backend_error` at
  `blocking_file_state_test.cpp:340`).
- Durability *coverage semantics* (SEM-06) are oracle'd by the model-side
  reference tests (`tests/semantic_durability_reference_test.cpp`: `v16`, `v27`
  ×2, `v17`, `failed_sync_covers_nothing_and_metadata_needs_sync_all`,
  `direct_resize_then_sync_sequence_succeeds`) against the constexpr coverage
  model (`file_semantics.hpp:365-412`), not against injected native failures.

## 7. Caveats

- All counts in §1–§3 are from this session's `objdump`/`readelf` output on the
  frozen archive and the freshly compiled N1 control; none are transcribed.
- The N1 control is a fresh same-flags compile (GCC 16.2.1, flags identical to
  the P0-recorded build flags), not the #600-measured binary — the #600 ladder
  covers `read_at` only (P0 §4.13), so a matched control for this group had to
  be built; its source follows the snapshot A1 discipline exactly.
- No `[UNCERTAIN]` items on the machine counts. Not measured here: wall-clock
  timing, EINTR-frequency behavior, cold-cache behavior.
