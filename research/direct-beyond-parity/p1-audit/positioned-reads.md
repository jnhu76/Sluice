# P1 Audit — Positioned Reads (`read_at`, `read`) — Group: positioned-reads

- Audit target: B1 frozen worktree `/home/jnhu/Source/Sluice-600-wt`, HEAD
  `77e5399a45998dea396699fa2e063c94a665945a`, linked archive
  `build/linux/x86_64/release/libsluice_core.a` sha256
  `d1e351a54ded3dfa1df6b1cd2e47622eefbd5d7027439cb31a314bed3a3007f0`
  (identity re-verified this session: `git -C /home/jnhu/Source/Sluice-600-wt
  rev-parse HEAD` and `sha256sum` of the archive — both match P0 §1.3). The
  worktree was used read-only; no build or configuration was performed in it.
- Scope: the two positioned/shared-cursor read primitives
  `sluice::blocking::read_at` and `sluice::blocking::read`. Composition ops
  (`read_exact_at`/`read_exact`) and write/state ops belong to other groups.
- Inputs consumed: P0 record §1/§3/§5
  (`research/direct-beyond-parity/P0_BASELINE_AND_OWNERSHIP.md`), the #600
  machine cost ledger (`git show
  sluice-600-direct-research:research/600-direct-excess-cost/DIRECT_MACHINE_COST_LEDGER.md`),
  and the frozen N1 probe snapshot
  (`research/direct-beyond-parity/probe-snapshot-600/ladder.cpp`, A1 arm
  `a1_read_at` at `ladder.cpp:42-63`). #600 conclusions are cited, not
  re-derived.
- Audit date: 2026-10-27. All commands below were executed this session on the
  E5 host unless tagged otherwise.

## 0. Method

1. Disassembly of the shipped archive member (no rebuild):
   `ar x libsluice_core.a blocking_file.cpp.o` → `objdump -d --demangle` /
   `objdump -dr` for relocations. The archive contains exactly two members:
   `file_resource.cpp.o`, `blocking_file.cpp.o`.
2. Comparator: `/tmp/p1-audit/n1_probe.cpp` compiled with the B1 build flags
   (`-m64 -fvisibility=hidden -Wall -Werror -O3 -std=c++20 -DNDEBUG`,
   `CCACHE_DISABLE=1 /usr/lib64/ccache/gcc`, include path
   `-I/home/jnhu/Source/Sluice-600-wt/include`). It contains
   (a) `a1_read_at` — a byte-verbatim copy of the frozen N1 arm, and
   (b) `a1_read` — a **newly constructed** matched-native comparator for the
   shared-cursor `read` primitive (the #600 ladder never covered `read`; P0
   §4.13). Both are `noinline` so their standalone bodies are emitted.
3. Success-path counts were enumerated by address and machine-checked
   (`/tmp/p1-audit/count_paths.py`, output reproduced below); every count in
   this document is that script's output, not a hand tally.

Counts produced (success path: open read-capable File, legal nonzero-length
request, non-null buffer, non-interrupted `pread`/`read`):

```
read_at B1:    instructions=39 branches=7     calls=1 reg->mem stores=6 mem-reads=3
read B1:       instructions=29 branches=6     calls=1 reg->mem stores=5 mem-reads=3
a1_read_at N1: instructions=40 branches=8*    calls=1 reg->mem stores=6 mem-reads=3
a1_read N1new: instructions=35 branches=7*    calls=1 reg->mem stores=5 mem-reads=6
```

(\* includes one unconditional `jmp` loop-entry; conditional-branch counts are
7 / 6 / 7 / 6 respectively.) Reg→mem stores exclude immediate stores; total
memory writes on the success paths are 8 for `read_at` and B1-`read`
(5/4 spill stores + 3 Result stores; `read` has 4 spills), and 7 for
`a1_read` (4 callee-saved saves + 3 Result stores).

## 1. `read_at` — chain, machine ledger, classification

### 1.1 Public API → semantic validation → internal representation → native → Result

| Step | Source (B1) | Machine shape |
|---|---|---|
| Public API | `include/sluice/blocking/file.hpp:14` — `Result<std::size_t> read_at(const File&, std::uint64_t, std::span<std::byte>)` | sret: `Result<size_t>` = 24 B at `(%r9)` after prologue move `read_at+0x0` |
| Implementation entry | `src/blocking_file.cpp:78-81` | — |
| Admission call | `admit_data_op` `src/blocking_file.cpp:25-29` | fully inlined; no call, no struct store at `-O3` |
| Semantic validation | `precheck_data_op` `include/sluice/detail/file_semantics.hpp:157-176`; request struct `DataOpRequest` `:149-155`; verdicts `:103-114`; rejection mapping `rejection_error` `:116-129` | branch sequence at `read_at+0x8..+0x52` (below); **no verdict materialization** — C1's elision holds: the verdict lives only in branch order |
| Verdict dispatch | `complete_empty` → `src/blocking_file.cpp:82-83`; rejection → `:84-85`; null-data guard → `:86-87` | fast paths at `+0x120` (5 instr), `+0x140/+0x160` (~7 instr), `+0x170` (6 instr) |
| Native operation | `retry_on_eintr` `include/sluice/detail/posix_retry.hpp:7-15` wrapping `native_pread` `src/blocking_file.cpp:51-58` → `::pread` `:57` | `call pread` at `+0x7b` (reloc `R_X86_64_PLT32 pread`); EINTR back-edge at `+0x1d8` |
| Result | success `static_cast<std::size_t>(n)` `src/blocking_file.cpp:96`; error `from_errno_value(errno)` `:94`, mapping table `include/sluice/error.hpp:66-90` | success stores at `+0x190..+0x1a0`; errno scan `+0x8e..+0xfa`; IoError pack `+0x1b0..+0x1c9` |

Admission check order (SEM-03 precedence, machine-verified):
closed `+0x8..+0xa` (`mov (%rsi),%edi; test; js` — fd load via
`File::is_open()`/`native_handle()` `include/sluice/file_resource.hpp:84-86`,
`fd_` at `:93`); access `+0x10..+0x14` (`cmpb $0x1,0x4(%rsi); je` —
`FileAccess::write_only` via `access()` `:88`, field `:94`;
`access_allows` read row `file_semantics.hpp:56-57`); zero-length
`+0x1a..+0x1d` (`test %r8,%r8; je`); range last-byte `+0x23..+0x3a`
(`movabs $0x7fff…; mov %rdx,%r11; lea -1(%r8); sub; cmp; jb` —
`range_is_valid` `file_semantics.hpp:32-39`); fused offset-bound +
transfer-bound `+0x40..+0x46` (`mov %r8,%rax; or %r11,%rax; js` —
`file_semantics.hpp:171-174`); null-data guard `+0x4c..+0x52`
(`mov %rcx,%rsi; test %rcx,%rcx; je`).

Elisions verified at B1 (relative to #600's B0 findings):

- The B0-era D1 redundant `checked_posix_offset` sign test
  (`test %r11,%r11; js`) is **gone**. The `or/js` at `+0x40..+0x46` covers both
  `offset > kMaxNativeOffset` (the wrapped `kMax - offset` sets the sign bit)
  and `length > kMaxNativeTransfer` (length's own sign bit) — GCC's fused form
  of the two source conditions.
- The Precheck `struct → copy → reload` round-trip (B0 ≈27 instructions) does
  not exist: there is no out-of-line precheck call and no stack
  materialization of the verdict at `-O3` (object dump inspected directly).

### 1.2 Success-path machine ledger (39 instructions)

| Region | Addresses | Instr | Branch | Load | Store |
|---|---|---|---|---|---|
| entry + closed check | `+0x0..+0xa` | 5 | 1 (`js` nt) | 1 (fd) | 0 |
| access check | `+0x10..+0x14` | 2 | 1 (`je` nt) | 1 (access byte) | 0 |
| zero-length branch | `+0x1a..+0x1d` | 2 | 1 (`je` nt) | 0 | 0 |
| range last-byte | `+0x23..+0x3a` | 6 | 1 (`jb` nt) | 0 | 0 |
| fused offset/transfer sign | `+0x40..+0x46` | 3 | 1 (`js` nt) | 0 | 0 |
| null-data guard | `+0x4c..+0x52` | 3 | 1 (`je` nt) | 0 | 0 |
| call setup + spills | `+0x58..+0x76` | 8 | 0 | 0 | 5 (stack) |
| `call pread` | `+0x7b` | 1 | 0 | 0 | 0 |
| post-call classify | `+0x80..+0x88` | 3 | 1 (`jns` taken) | 1 (sret reload) | 0 |
| Result + epilogue | `+0x190..+0x1a7` | 6 | 0 | 0 | 3 (sret) |
| **Total** | | **39** | **7** | **3** | **8** |

Stack-object shapes: frame `sub $0x38,%rsp` (56 B) with spill slots
`0x8`=sret, `0x10`=buf, `0x18`=length, `0x20`=offset, `0x28`=File*.
`Result<size_t>` sret layout: `value`@+0 (8 B), `IoError.code`@+8 (1 B),
`IoError.os_errno`@+0xc (4 B), `has_value`@+0x10 (1 B).

### 1.3 Error paths and EINTR shape (recorded, not success-path cost)

- EINTR (`+0x1d8`): reached when `n<0` and `errno==EINTR`
  (`call __errno_location` at `+0x8e`; `mov (%rax),%eax`;
  `cmp $0x4,%eax; je +0x1d8`). The back-edge `mov (%r10),%edi` (fd reload from
  `File*`) then `jmp +0x5c` re-enters the call setup, **re-executing 7
  instructions including 4 redundant re-stores of already-spilled identical
  values** — a compiler artifact on the retry path only (blocking-fd EINTR is
  rare; no measured workload in the supported domain stresses it).
- Errno canonicalization (`+0x8e..+0xfa`): `errno==0` → `backend_error`;
  linear `cmp/je` chain over EACCES(13)/EPERM(1)/ENOENT(2)/ENOTDIR(20)/
  ENOSPC(28)/EDQUOT(122)/EAGAIN(11); six targets materialize the canonical
  code via a relocated load out of `kNativeErrorMappings`
  (`include/sluice/error.hpp:66-78`, relocs `R_X86_64_32 …+0x0..+0x38`);
  default → `backend_error` immediate.
- IoError packing (`+0x1b0..+0x1c9`): code zero-extended, errno `shl $0x20`,
  OR, one 8-byte store at sret+8, `has_value=0`, share of epilogue.
- Admission rejections: closed → code 6 (invalid_state) at `+0x160`;
  access/range → code 8 (invalid_argument) at `+0x140`; null-data → code 8 at
  `+0x170`; each ~6–7 instructions to `ret`.
- Zero-length success fast path `+0x120..+0x137`: 5 instructions, no syscall
  (matches the #600 strace result "size-0 Sluice path issues 0 pread64").

### 1.4 Classification (8 classes)

| # | Class | Where | Machine evidence | Semantic basis (v2-r1) |
|---|---|---|---|---|
| 1 | SEMANTICALLY_REQUIRED | closed/access admission, `src/blocking_file.cpp:80-81` → `file_semantics.hpp:162-165` | `+0x8..+0x14`: 7 instr, 2 branches, 2 loads | SEM-03 steps 1–2 (`docs/explicit-io-contract.md:408-412`, table `:400-407`); oracles `tests/file_access_precedence_test.cpp:116-200`, `tests/blocking_file_read_test.cpp:122,185` |
| 2 | SEMANTICALLY_REQUIRED | zero-length branch, `src/blocking_file.cpp:82-83` → `file_semantics.hpp:166-167` | `+0x1a..+0x1d` (2 instr, 1 br) + 5-instr no-syscall return `+0x120` | SEM-03 step 3 (`:412-413`, `:418-420` "without an OS call"); oracles `tests/direct_composition_fault_test.cpp:304,665` (no-native-call proof via EIO-armed script), `tests/blocking_file_read_test.cpp:80,180` |
| 3 | SEMANTICALLY_REQUIRED | range+transfer admission, `file_semantics.hpp:171-174` (`range_is_valid` `:32-39`) | `+0x23..+0x46`: 9 instr, 2 branches; D1 sign test verified absent | SEM-04 (`:437-452`): no wrap/truncate, last-addressed-byte validation, native-invalid → `invalid_argument`, transfer limit is a range rejection; oracles `tests/semantic_range_test.cpp:91-117,183-185`, `tests/blocking_file_read_test.cpp:122` |
| 4 | TRANSFORMABLE_WORK | null-data guard, `src/blocking_file.cpp:86-87` | `+0x4c..+0x52`: 3 instr, 1 branch | **No v2-r1 anchor**: the contract contains no "null" text; SEM-03 (`:408-410`) explicitly leaves caller memory validity "not generally dynamically detectable"; P0 §5.1's null-buffer oracle row is satisfied by size-0 spans (empty-span tests), which never reach this check. Gap candidate G-READAT-01 |
| 5 | ABI_OR_COMPILER_COST | EINTR loop live ranges, `posix_retry.hpp:7-15`; spills at `read_at+0x62..+0x76` | 5 spill stores + 1 sret reload; frame 56 B; identical slots/frame in N1 (`a1_read_at` `+0x92..+0xa6`) | SEM-05 (`:466-472`) requires the retry-without-replay loop; the *spill count* is GCC register allocation across a call in a loop and is matched instruction-for-instruction by the matched native arm — #600 D3 `NO_CHANGE_JUSTIFIED` (cited, not re-derived) |
| 6 | KERNEL_FLOOR | `call pread`, `src/blocking_file.cpp:89-92` | 1 call, reloc `pread` at `+0x7b` | SEM-05 primitive table; #600 strace: 1.000 pread64/op on A2 (cited) |
| 7 | SEMANTICALLY_REQUIRED (+ABI_OR_COMPILER_COST for the dead errno store) | success classification + Result build, `src/blocking_file.cpp:93-96` | `test/jns` + 3 sret stores + epilogue (6 instr); `movl $0,0xc(%r9)` is an `os_errno` zero-store dead under the public API (error() read only when `!has_value`) but emitted identically in N1 (`a1_read_at+0xbd`) — #600 D3 | SEM-05 (`:459-466`): 0<n≤requested is success; primitive does not loop; ERR-02 effect certainty via `Result` |
| 8 | SEMANTICALLY_REQUIRED (error path; zero success-path cost) | errno mapping, `src/blocking_file.cpp:93-95` → `error.hpp:66-93` | `__errno_location` call + ≤10-way scan + IoError pack (`+0x8e..+0x1c9`); 6 of 8 mappings resolved via table load | ERR-01 error domains / canonical mapping; oracle `tests/semantic_errno_mapping_test.cpp` (14 mappings, incl. `EINTR_is_interrupted`) |

REPRESENTATION_OVERHEAD: none on these paths — `DataOpVerdict`/`DataOpRequest`
are fully scalarized at `-O3` (no store, no reload; verified by inspection of
the complete dump). DUPLICATE_WORK: none — each File field is loaded exactly
once per call. COMPOSITION_OVERHEAD: not applicable to a primitive.
UNATTRIBUTED: none.

## 2. `read` — chain, machine ledger, classification

### 2.1 Public API → semantic validation → internal representation → native → Result

| Step | Source (B1) | Machine shape |
|---|---|---|
| Public API | `include/sluice/blocking/file.hpp:19` | 24-B sret in `%r8` after `read+0x440` |
| Implementation entry + admission (offset = 0) | `src/blocking_file.cpp:120-121` → `file_semantics.hpp:157-176` | inlined; see below |
| Verdict dispatch | `src/blocking_file.cpp:122-127` (empty → 0; rejection; null-data guard) | `+0x530` (5 instr), `+0x550/+0x570`, `+0x580` |
| Native operation | `retry_on_eintr` (`posix_retry.hpp:7-15`) wrapping `native_read` `src/blocking_file.cpp:43-49` → `::read` `:48` | `call read` at `+0x48f`; EINTR back-edge `+0x5e8` |
| Result | success `src/blocking_file.cpp:135`; error `:133` | success stores `+0x5a0..+0x5b7`; errno scan `+0x4a1..` |

Machine admission for `read` differs from `read_at` in exactly one way:
because the admission offset is the constant 0, `range_is_valid(0, length)`
degrades to the transfer bound alone, and GCC **fuses it into a single `jle`
at `+0x463` sharing flags with the zero-length `test %rcx,%rcx` at `+0x45a`**
(taken iff bit 63 of length is set, i.e. `length ≥ 2^63`, exactly the
`length > kMaxNativeTransfer` reject set). The full check order
closed → access → zero-length → range/transfer → null is otherwise identical
to `read_at`.

### 2.2 Success-path machine ledger (29 instructions)

| Region | Addresses | Instr | Branch | Load | Store |
|---|---|---|---|---|---|
| entry + closed | `+0x440..+0x44a` | 5 | 1 (`js` nt) | 1 | 0 |
| access | `+0x450..+0x454` | 2 | 1 (`je` nt) | 1 | 0 |
| zero-length + fused sign check | `+0x45a..+0x463` | 3 | 2 (`je`,`jle` nt) | 0 | 0 |
| null-data guard | `+0x469..+0x46f` | 3 | 1 (`je` nt) | 0 | 0 |
| call setup + spills | `+0x475..+0x48a` | 6 | 0 | 0 | 4 (stack) |
| `call read` | `+0x48f` | 1 | 0 | 0 | 0 |
| post-call classify | `+0x494..+0x49b` | 3 | 1 (`jns` taken) | 1 | 0 |
| Result + epilogue | `+0x5a0..+0x5b7` | 6 | 0 | 0 | 3 |
| **Total** | | **29** | **6** | **3** | **7** |

Frame `sub $0x28,%rsp` (40 B): slots `0x0`=sret, `0x8`=buf, `0x10`=length,
`0x18`=File*. Result layout as in §1.2.

### 2.3 Error paths and EINTR shape

Same family as §1.3 with two concrete differences: the EINTR back-edge
(`+0x5e8`: fd reload `mov (%r9),%edi`; `jmp +0x479`) re-enters the setup at
`+0x479` and re-executes 6 instructions (4 redundant re-stores); the errno
scan (`+0x4a1..+0x507`) is the same `__errno_location` + linear chain +
relocated table-load shape.

### 2.4 Classification (8 classes)

| # | Class | Where | Machine evidence | Semantic basis |
|---|---|---|---|---|
| 1 | SEMANTICALLY_REQUIRED | closed/access admission, `src/blocking_file.cpp:121` → `file_semantics.hpp:162-165` | `+0x440..+0x454`: 7 instr, 2 br, 2 loads | SEM-03 steps 1–2; oracles `file_access_precedence_test.cpp:116-176`, `blocking_file_read_test.cpp:185` |
| 2 | SEMANTICALLY_REQUIRED | zero-length + transfer bound, `src/blocking_file.cpp:122-123` → `file_semantics.hpp:166-174` | `+0x45a..+0x463` (3 instr, 2 br, one flag-sharing fusion) + 5-instr no-syscall return `+0x530` | SEM-03 step 3; SEM-04 transfer limit (`semantic_range_test.cpp:104` `native_transfer_limit_is_a_range_rejection`, registered `:183`) |
| 3 | TRANSFORMABLE_WORK | null-data guard, `src/blocking_file.cpp:126-127` | `+0x469..+0x46f`: 3 instr, 1 branch | Same as §1.4 #4 — no contract anchor, no oracle (grep over `tests/` finds no `span(nullptr, n)` construction). Gap candidate G-READ-01 |
| 4 | ABI_OR_COMPILER_COST | EINTR loop live ranges, `posix_retry.hpp:7-15`; spills `+0x47c..+0x48a` | 4 spills + 1 sret reload, 40-B frame | SEM-05 EINTR rule; the new matched-native `a1_read` chose callee-saved saves instead (4 saves + 4 restores = 8 memory ops vs B1's 4+1) — B1's shape is the cheaper of the two legal allocations; compiler, not contract, decides |
| 5 | KERNEL_FLOOR | `call read`, `src/blocking_file.cpp:129-131` | 1 call at `+0x48f` | SEM-04 shared-cursor semantics (`:452-458`): the native open-file-description cursor itself is the mechanism; no hidden seek; oracles `tests/direct_cursor_position_test.cpp` (`positional_operations_do_not_move_shared_position`, `shared_cursor_is_shared_through_native_duplication`) |
| 6 | SEMANTICALLY_REQUIRED (+ABI_OR_COMPILER_COST dead errno store) | success classification + Result build, `src/blocking_file.cpp:132-135` | `test/jns` + 3 sret stores + epilogue; dead `os_errno` store at `+0x5a3` also present in `a1_read+0x230` | SEM-05 partial-count success; ERR-02; D3 parity (cited) |
| 7 | SEMANTICALLY_REQUIRED (error path only) | errno mapping `src/blocking_file.cpp:132-134` → `error.hpp:66-93` | `+0x4a1..+0x5d9` same shape as §1.3 | ERR-01; `semantic_errno_mapping_test` |

REPRESENTATION_OVERHEAD / DUPLICATE_WORK / COMPOSITION_OVERHEAD /
UNATTRIBUTED: none, as in §1.4.

## 3. Parity judgment vs N1

### 3.1 `read_at` — PARITY_REACHED (static initial judgment)

| Metric | B1 `read_at` | N1 `a1_read_at` (frozen arm, this compile) |
|---|---|---|
| success-path instructions | **39** | **40** |
| conditional branches | 7 | 7 (+1 unconditional `jmp` loop-entry) |
| calls (success path) | 1 (`pread`) | 1 (`pread`) |
| memory reads / writes | 3 / 8 | 3 / 8 |
| stack frame | 56 B, 5 spill slots | 56 B, same 5 slot offsets |

The admission sequence of B1 `read_at` (`+0x8..+0x46`) is instruction-for-
instruction the same check sequence GCC emits for the hand-written A1 body —
including the `movabs/lea/sub/cmp/jb` range form and the `mov/or/js` fused
sign test — because the C1 admission is now the same source-level arithmetic
the hand arm performs. B1 is one instruction shorter only because N1's
loop-entry uses an extra `jmp`. Both carry the same 5 spills and the same 3
Result stores.

Dynamic caliber (cited, not re-measured): at B0, #600 measured
A2−A1 = +12.8 ns (0 B, no-kernel cell) and +15.8…+19.3 ns (syscall cells);
C1 removed −13.3…−16.6 ns (PHASE3 V2), which arithmetically returns the
candidate to the A1 band (0 B: 23.4 − 13.3 ≈ 10.1 vs A1's 10.5, inside the
2.5–3 ns minimum detectable effect). **No direct B1×N1 paired ladder cell
exists in the frozen evidence**; the static counts here are the parity
evidence, and a paired B1×N1 run (16 ABBA pairs, 0 B / 1 B / 4 KiB) is the
natural confirmation experiment if a dynamic statement is ever needed.

### 3.2 `read` — PARITY_REACHED (static initial judgment; comparator newly constructed)

B1 `read` = 29 instructions vs `a1_read` = 35. B1 is *below* the comparator;
the 6-instruction difference is register-allocation variance (comparator paid
4 callee-saved saves/restores, B1 pays 4 spills + 1 reload and no restores),
not semantic content: both have the same 5 admission decisions in the same
order, 1 call, the same EINTR loop shape, and the same 3 Result stores.
Caveat stated plainly: this comparator arm is **new** (constructed this
session in N1's style, same flags); the frozen #600 N1 never covered `read`
(P0 §4.13), so unlike §3.1 this judgment has no frozen-predecessor cross-check.

Neither op carries measurable excess work relative to its semantics-matched
native mode. Bare POSIX (N0) is fewer instructions still, but N0 carries none
of the SEM-03/04/05 obligations; instruction-count difference vs N0 is not
eliminable Sluice work.

## 4. Gap candidates

One mechanism, instantiated in both ops:

### G-READAT-01 / G-READ-01 — contract-unanchored null-data runtime guard

- Source: `src/blocking_file.cpp:86-87` (read_at) and `:126-127` (read).
- Trigger payload: any legal nonzero-length read call on an open
  read-capable File (the guard executes on **every** success-path call).
- Necessary work: none of these 3 instructions is required by v2-r1 — the
  contract text contains no null-buffer obligation; SEM-03 classifies caller
  memory validity as a caller precondition ("not generally dynamically
  detectable", `docs/explicit-io-contract.md:408-410`); every named oracle
  that mentions empty buffers passes size-0 spans
  (`tests/file_access_precedence_test.cpp:116-200`), which the zero-length
  branch handles before this guard runs.
- Excess work: 3 instructions + 1 not-taken branch per nonzero-length call
  (`mov`/`test`/`je`), guarding only the ill-formed input class
  `span(nullptr, n>0)` (std::span constructor precondition violation — UB at
  construction), whose rejection today is `invalid_argument` and without the
  guard would become pread's `EFAULT` → `backend_error`.
- Magnitude: ≈3 instructions ≈ sub-ns; below the #600 minimum detectable
  effect (2.5–3 ns) at every size cell, and invisible at syscall-dominated
  cells. This is a machine-count hygiene candidate, **not** a performance
  candidate. Evidence: success-path tables §1.2/§2.2; noise floor cited from
  the #600 ledger (A/A sd 5.0 ns).
- Mechanism direction: remove the runtime guard (or, if the owner wants the
  defensive rejection as product behavior, anchor it in the root per the
  AGENTS.md Mechanism Justification Gate — "do not add runtime mechanism
  merely as defensive insurance for an unreachable state"). Note the frozen
  N1 comparator carries the identical check (`ladder.cpp:54-55`), so this is
  **not** an N1-relative excess; eliminating it would take B1 *beyond* N1
  parity and would require the N1 successor arms to drop it too for a clean
  comparison.
- Falsifier: (a) candidate branch with the guard deleted, rebuilt with the
  identical flags → expect −3 instructions/−1 branch in both functions'
  success paths; (b) run the full semantic matrix (4-configuration `xmake
  test` 58/58, precedence, range, errno suites) — prediction: all green,
  because no oracle constructs the guarded input (grep evidence: zero matches
  for `span(nullptr` / nullptr-buffer constructions in `tests/`); (c) 16-ABBA
  paired benchmark at 1 B and 4 KiB — prediction: within noise. Any red suite
  or measurable delta falsifies the classification.
- Confidence: high for the machine facts and the oracle absence (disassembly
  and greps run this session); medium for the disposition (removal vs root
  amendment is an owner decision).

No other gap candidates: admission, EINTR shape, Result shape, and the errno
path are each either SEMANTICALLY_REQUIRED or at parity with the matched
native arm (§1.4, §2.4, §3).

## 5. Mutation-seam inventory for this group (B1)

Registered compile-time direct-path mutant seams that reach `read_at`/`read`:

| Seam | Source | xmake target | Killer assertion (verified present) |
|---|---|---|---|
| `SLUICE_SEM_MUTANT_EMPTY_BEFORE_CLOSED` | `include/sluice/detail/file_semantics.hpp:158-161` | `blocking_file_mut_empty_before_closed` (`xmake/tests.lua:94-96`) | empty-before-closed ordering; killer suite `tests/blocking_file_sequential_test.cpp` (`sequential_read_on_write_only_file_rejected_upfront` body at `:392`); closed+empty invalid_state row in `tests/file_access_precedence_test.cpp:116-153` |
| `SLUICE_SEM_MUTANT_ADMIT_INVALID_RANGE` | `file_semantics.hpp:168-170` | `blocking_file_mut_admit_invalid_range` (`xmake/tests.lua:91-93`) | `read_with_illegal_offset_reports_invalid_argument` (`tests/blocking_file_read_test.cpp:122`, registered `:183`) |
| `SLUICE_SEM_MUTANT_ADMIT_UNREPRESENTABLE_SIZE` | `file_semantics.hpp:192` (resize only) | `blocking_file_mut_admit_unrepresentable_size` (`xmake/tests.lua:97-99`) | not this group |

The three targets are declared via `direct_mutant_target()`
(`xmake/tests.lua:81-88`) and are documented as executed/recorded manually,
never as regular tests (P0 §5.3; comment carried in the target block).

Seam **gaps** for this group (no compile-time mutant macro exists; oracles
listed are the only protection):

1. Access-legality admission (`access_allows`, `file_semantics.hpp:54-68`):
   no macro can admit an illegal access. Oracles:
   `read_on_write_only_file_rejected_upfront`
   (`tests/blocking_file_read_test.cpp:185` registration; body `:122` region),
   `blocking_read_on_write_only_with_empty_buffer_reports_invalid_argument`
   (`tests/file_access_precedence_test.cpp:160`).
2. EINTR retry semantics (`posix_retry.hpp:7-15`): no macro; protected only
   by runtime fault injection (`SLUICE_FILE_INTERNAL_TESTING` seams
   `src/blocking_file.cpp:31-58`, `NativeScript`):
   `primitive_retries_eintr_without_a_completed_count`
   (`tests/direct_composition_fault_test.cpp:224`, registered `:659`, script
   `{{-1,EINTR},{4,0}}`, asserts exactly 2 native calls).
3. Null-data guard (`src/blocking_file.cpp:86-87,126-127`): no macro **and no
   oracle at all** (§4) — the only code in this group whose deletion is
   predicted to keep the entire matrix green.
4. Errno canonicalization (`error.hpp:66-93`): no macro; oracle
   `tests/semantic_errno_mapping_test.cpp` (14 mappings).
5. Partial-read no-loop / EOF-zero-success (SEM-05 table): no macro; oracles
   `short_read_reports_only_available_bytes`, `read_past_end_returns_zero`
   (`tests/blocking_file_read_test.cpp:179`).

## 6. Caveats and [UNCERTAIN] items

1. The #600-era static count for N1 was "≈42 instructions"; my standalone
   `noinline` compile of the byte-identical body yields 40. The difference is
   compilation-context variance (the original arm was called from inside
   `run_arm`'s lambda and #600 counted that context; my compile is a
   standalone TU body). Both compilations used the same toolchain and flags
   this session; the parity conclusion is insensitive to ±2 instructions of
   context. [UNCERTAIN: the exact counting convention #600 used for "42" is
   not recoverable from the ledger text.]
2. Dynamic B1×N1 paired confirmation does not exist in the frozen evidence
   (§3.1); the parity judgments here are static. The #600 dynamic figures
   cited are [TRANSCRIBED] from the ledger, not re-measured.
3. The `a1_read` comparator is new (this session), not part of the frozen
   snapshot; per P0 §4.8's rule, any P1 comparator arm that is later used for
   decision-bearing measurements needs a committed, rebuildable identity —
   this audit's probe lives in `/tmp/p1-audit/` deliberately.
4. The EINTR re-entry redundancy (re-executed spills, §1.3/§2.3) was not
   filed as a gap: it is error-path-only work whose trigger rate in the
   supported blocking-fd domain is not represented by any admitted workload;
   no falsifiable magnitude exists at current floors.
5. All line numbers cite B1 (`/home/jnhu/Source/Sluice-600-wt`); B0 differs
   (P0 §5.4 note on the moved seam applies when comparing citations).
