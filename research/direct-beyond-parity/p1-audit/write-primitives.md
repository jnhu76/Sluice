# P1 Static Cost Audit — Write Primitives (`write_at`, `write`) at B1

- Group: write primitives (positional write `sluice::blocking::write_at`, shared-cursor
  write `sluice::blocking::write`).
- Audited baseline: **B1** = commit `77e5399a45998dea396699fa2e063c94a665945a`, frozen
  worktree `/home/jnhu/Source/Sluice-600-wt`, archive
  `build/linux/x86_64/release/libsluice_core.a`, sha256
  `d1e351a54ded3dfa1df6b1cd2e47622eefbd5d7027439cb31a314bed3a3007f0` — identity
  re-verified this session (`git rev-parse HEAD`, `sha256sum`), matching P0 §1.3.
  The worktree was used read-only; all builds/disassemblies happened in `/tmp`.
- Inputs read before auditing: `research/direct-beyond-parity/P0_BASELINE_AND_OWNERSHIP.md`
  §1 (baseline/ownership), §3 (#600 conclusions, not re-derived), §5 (semantics/oracle
  floor); #600 `DIRECT_MACHINE_COST_LEDGER.md`
  (`git show sluice-600-direct-research:research/600-direct-excess-cost/DIRECT_MACHINE_COST_LEDGER.md`);
  comparator pattern from `research/direct-beyond-parity/probe-snapshot-600/ladder.cpp`
  A1 arm (`a1_read_at`, ladder.cpp:35-63 of the snapshot).
- Method: static audit exactly as commissioned — `objdump -d/-dr` instruction counts on
  the frozen B1 archive object, plus a single-TU compile of A1-pattern matched-native
  write comparators with the B1 flags (`g++ -std=c++20 -O3 -DNDEBUG
  -fvisibility=hidden -Wall -Werror -I<worktree>/include`, GCC 16.2.1 per P0 §1.5).
  **No tests and no timing ladder were run in this audit**; every "not run" item below is
  stated as such. Per P0 §4.13, no E5 ladder arm exists for any write path — that
  measurement gap remains open and is recorded in Notes, not silently filled.

Commands run (all in `/tmp/p1-audit-write`):

```sh
cp <wt>/build/linux/x86_64/release/libsluice_core.a sluice_copy.a   # archive copy; worktree untouched
ar x sluice_copy.a blocking_file.cpp.o
objdump -d --demangle blocking_file.cpp.o        # function map + bodies
objdump -dr blocking_file.cpp.o                  # relocations (call targets, errno table)
g++ -std=c++20 -O3 -DNDEBUG -fvisibility=hidden -Wall -Werror \
    -I<wt>/include -c matched.cpp -o matched.o   # A1-pattern comparators, B1 flags
objdump -d --demangle matched.o ; objdump -r matched.o
python3 count_path.py <func.txt> <addr-ranges>   # success-path counter (script kept in /tmp)
```

Object symbol map (B1 `blocking_file.cpp.o`): `read_at` 0x0-0x220, `write_at`
0x220-0x440, `read` 0x440-0x630, `write` 0x630-0x820. `write_at` and `read_at` are
shape-identical (544 B each); the only differences are the access-compare immediate
(`cmpb $0x1` for read vs `cmpb $0x0` for write, both at +0x10) and the call relocation
(`pread` vs `pwrite`).

---

## 1. `write_at` — Public API → semantics → representation → native → Result

| Step | Source (B1) | Notes |
|---|---|---|
| Public API decl | `include/sluice/blocking/file.hpp:16-17` | `Result<std::size_t> write_at(const File&, std::uint64_t, std::span<const std::byte>)` |
| Public API def | `src/blocking_file.cpp:99-118` | body audited below |
| Admission dispatch | `src/blocking_file.cpp:25-29` (`admit_data_op`) → `detail::DataOpRequest{!file.is_open(), file.access(), operation, offset, length}` (`include/sluice/detail/file_semantics.hpp:149-155`) | fully inlined at -O3; no function call |
| Semantic validation | `precheck_data_op` `file_semantics.hpp:157-176`: closed → `reject_closed` (:162); `access_allows` write = `access != read_only` (:164, table :54-68); zero length → `complete_empty` (:166-167); `range_is_valid` (:171, def :32-39); `length > kMaxNativeTransfer` (:173-174) | check order = SEM-03 steps 1-4 |
| Verdict → outcome | `blocking_file.cpp:103-104` (complete_empty → `0`), `:105-106` (`is_rejection` → `rejection_error`, `file_semantics.hpp:111-129`: closed→`invalid_state`, access/range→`invalid_argument`), `:107-108` (null `src.data()` → `invalid_argument`) | |
| Internal representation | `DataOpVerdict` uint8 enum (`file_semantics.hpp:103-109`) | at B1 the verdict is **scalarized into a register branch structure — no Precheck struct materialization remains** (the C1 restructure; #600's dominant B0 excess of ≈27 instrs/Precheck round-trip is absent here) |
| Native operation | `retry_on_eintr` lambda `include/sluice/detail/posix_retry.hpp:7-15` wrapping `native_pwrite` `blocking_file.cpp:68-75` → `::pwrite`; test-seam `#ifdef SLUICE_FILE_INTERNAL_TESTING` compiles out in release | compiled to `call pwrite@plt` (reloc at 0x29c `R_X86_64_PLT32 pwrite-0x4`) |
| Result | `blocking_file.cpp:114-117`: `n<0` → `from_errno_value(errno)` (`include/sluice/error.hpp:92-94`, canonical mapping table :66-78); success → `static_cast<std::size_t>(n)` into `Result<std::size_t>` (sret, `include/sluice/result.hpp:114-143`) | |

### 1.1 Machine ledger — `write_at` success path (open, write-capable, len>0, valid range, non-null data, `pwrite` ≥ 0)

Path traced: entry 0x220 → 0x29b (validation fall-through + arg setup + call), 0x2a0-0x2a8
(post-call test; `jns` taken), 0x3b0-0x3c7 (success tail). Counter: `count_path.py`
(one script artifact: the `movabs` imm64 spans two objdump text lines; the second line is
not an instruction — counts below are corrected by hand).

| Region | Addresses | Instructions | Branches | Loads | Stores | Classification |
|---|---|---|---|---|---|---|
| entry + File state read | 0x220-0x22a | 4 (`mov rdi,r9`, load `fd_`, `mov rsi,r10`, `test`) | 1 (`js` closed) | 1 (`fd_` @0x223) | 0 | SEMANTICALLY_REQUIRED (SEM-03 step 1; V01) |
| access check | 0x230-0x234 | 1 | 1 (`je` read_only) | 1 (`access_` @0x230) | 0 | SEMANTICALLY_REQUIRED (SEM-03 step 2, legality table :402) |
| zero-length check + fast tail | 0x23a-0x23d (+0x340-0x357 taken on len==0) | 2 | 1 (`je`) | 0 | 0 (on success path; the taken tail does 3 stores, no call) | SEMANTICALLY_REQUIRED (SEM-03 step 3: no-op returns 0, "without an OS call" — tail 0x340-0x357 contains no `call`) |
| fused range/transfer block | 0x243-0x266 | 7 (`movabs kMax`, `mov r11,off`, `lea len-1`, `sub`, `cmp`, `mov`, `or`) | 2 (`jb` last-byte; `js` bit63 of `len|offset`) | 0 | 0 | SEMANTICALLY_REQUIRED (SEM-04: last-addressed-byte validation without overflow; native-invalid range → `invalid_argument`). Three source conditions fuse to two compares + one or/sign test — no REPRESENTATION_OVERHEAD remains (#600 COMPILER_ELIDED finding holds at B1 for write_at) |
| null-data guard | 0x26c-0x272 | 2 (`mov rcx,rsi`, `test`) | 1 (`je`) | 0 | 0 | TRANSFORMABLE_WORK (candidate, low confidence) — see §1.3 and gap G-WRITEAT-02; guards a span state (`data()==nullptr && size()>0`) that is not constructible without UB at the `std::span` precondition level, so no contract-observable input reaches it. Not excess vs the N1 pattern (the A1 comparator carries the same guard, ladder.cpp:54-55) |
| call setup + spills | 0x278-0x29b | 8 (`sub $0x38`, 2 arg `mov`s, 5 spill stores, `call`) | 0 | 0 | 5 | ABI_OR_COMPILER_COST (sret + live args must survive the PLT call; A1 comparator emits the identical 5 spills, matched.o 0xa2-0xb6) |
| native op | 0x29b | (counted above) `call pwrite@plt` | — | — | — | KERNEL_FLOOR (exactly 1 syscall/op expected; not strace-verified in this audit) |
| EINTR test | 0x2a0-0x2a8 | 2 (`mov` reload sret, `test rax`) | 1 (`jns` taken on success) | 1 (sret @0x2a0) | 0 | SEMANTICALLY_REQUIRED (SEM-05: retry EINTR "only when the underlying attempt reports interruption without a successful byte count") |
| Result stores + epilogue | 0x3b0-0x3c7 | 6 (`mov` value, `movl $0` errno field, `movb $1` has_value, `mov r9,rax`, `add $0x38`, `ret`) | 0 | 0 | 3 | ABI_OR_COMPILER_COST (#600 D3: `Result<size_t>` sret 24-byte, errno dead-store — identical shape in A1; NO_CHANGE_JUSTIFIED precedent) |
| **Total success path** | | **39** | **7 cond (all fall-through/taken-to-success), 0 uncond** | **3** (fd, access, sret) | **8** (5 spill + 3 result) | stack frame 0x38 (56 B), 1 call |

### 1.2 Error paths and EINTR shape — `write_at`

- closed → 0x380: `mov $0x6` (invalid_state) → shared reject tail 0x365 (3 stores + ret).
- access/range reject → 0x360: `mov $0x8` (invalid_argument) → same tail.
- zero-length → 0x340: success-shaped 0-result, no `call` (SEM-03 step 3 honored).
- null-data → 0x390: invalid_argument, same tail shape.
- `n < 0`: 0x2ae `call __errno_location` (reloc 0x2af), reload 5 regs (0x2b3-0x2c9), then
  `cmp $0x4` (EINTR) at 0x2ce:
  - **EINTR loop**: `je 0x3f8` → `mov (%r10),%edi` (reload fd) → `jmp 0x27c` — re-enters
    the arg-shuffle + 5 spill stores + `call pwrite`. The retry re-issues the identical
    request only when `n<0`, satisfying "never replay confirmed bytes" (SEM-05). The
    spill block sits inside the loop, so each retry repeats 5 stores + 8 setup
    instructions — a per-retry cost shared by the A1 comparator's loop (matched.o
    0x9c-0xbb), i.e. not Sluice-specific.
  - **errno mapping**: compare chain `4 / 0 / 0xd / 1 / 2 / 0x14 / 0x1c / 0x7a / 0xb`
    (EINTR / 0 / EACCES / EPERM / ENOENT / ENOTDIR / ENOSPC / EDQUOT / EAGAIN); matches
    jump to `mov $imm32,%ecx` whose immediate is relocated (`R_X86_64_32
    _ZN6sluice6detail20kNativeErrorMappingsE+off`, offsets 0x0-0x38) to the address of
    the matched `kNativeErrorMappings` entry, then `movzbl 0x4(%rcx),%ecx` loads the
    canonical `IoError::Code`; errno 0 and no-match fall to `mov $0x7` (backend_error);
    `os_errno` is preserved in the packed store (0x3d5-0x3e5, `shl $0x20` merge). This is
    `from_errno_value` fully inlined (SEM-05 "retain native detail"; ERR mapping).

### 1.3 Classification summary and residual attribution — `write_at`

Every success-path region maps to SEMANTICALLY_REQUIRED, ABI_OR_COMPILER_COST, or
KERNEL_FLOOR, except the 3-instruction null-data guard classified TRANSFORMABLE_WORK
(low confidence, see G-WRITEAT-02). **UNATTRIBUTED residual: none.** DUPLICATE_WORK:
none (no repeated validation; the B0-era duplicate `checked_posix_offset` sign test is
absent — the or/js at 0x260-0x266 is the *fused* single sign test covering offset-max
and transfer-max jointly). COMPOSITION_OVERHEAD: not applicable (primitive;
`write_all_at` is out of this group's scope). REPRESENTATION_OVERHEAD: none (no Precheck
stack round-trip survives at B1 — verified by absence of any 16/24-byte stack
copy/reload pair in the disassembly).

### 1.4 Comparison against the N1 pattern and verdict — `write_at`

Matched comparator `a1_write_at` (this session, `/tmp/p1-audit-write/matched.cpp`,
A1-arm pattern: same check order, same error codes, EINTR do-while retry, same
`Result<size_t>` ABI, public headers only, independent TU, B1 flags; call relocations
verified `pwrite` + `__errno_location` + the same `kNativeErrorMappings` refs):

- success path: **40 instructions** (including one in-loop unconditional `jmp` at
  0x66 introduced by GCC loop rotation of the do-while), 7 conditional + 1 unconditional
  branch, 1 call, 3 loads, 8 stores (same 5-spill + 3-result shape), frame 0x38.
- `Sluice write_at (39) − a1_write_at (40) = −1`: the shipping path is **not longer**
  than the hand-written semantics-matched native wrapper. There is no static excess left
  to eliminate on this operation.

**Initial verdict: PARITY_REACHED** at the static machine level. Caveat, stated not
filled: no E5 paired timing exists for any write arm (P0 §4.13); measured parity is
unverified, though a path that is statically ≤ the matched-native control cannot carry
meaningful positive excess at #600's effect scale (MDE ≈2.5-3 ns, P0 §3.2).

---

## 2. `write` (shared-cursor write) — chain and machine ledger

| Step | Source (B1) | Notes |
|---|---|---|
| Public API decl | `include/sluice/blocking/file.hpp:21` | |
| Public API def | `src/blocking_file.cpp:138-154` | |
| Admission | `admit_data_op(file, write, offset=0, src.size())` (`:139`) — the offset is the constant 0, so `range_is_valid(0, len)` degenerates to `len ≤ 2^63` and the transfer bound to `len < 2^63`; the compiler reduces both to the length sign bit | same `precheck_data_op` |
| Native operation | `retry_on_eintr` → `native_write` (`:60-66`) → `::write` (`:147-149`), reloc `R_X86_64_PLT32 write-0x4` at 0x680 | using `::write` (not seek+write) preserves the native open-file-description cursor (SEM-04: "Shared-cursor operations use the native open-file-description cursor"; "Hidden seek-plus-read/write is not a conforming lowering") |
| Result | `:150-153` | same `from_errno_value` mapping, same sret shape |

### 2.1 Success path (open, write-capable, len>0, len < 2^63, non-null data)

Path traced: 0x630-0x67f, 0x684-0x68b (`jns` taken), 0x790-0x7a7.

| Region | Addresses | Instructions | Branches | Loads | Stores | Classification |
|---|---|---|---|---|---|---|
| entry + fd read + closed test | 0x630-0x63a | 4 | 1 (`js`) | 1 (`fd_` @0x633) | 0 | SEMANTICALLY_REQUIRED (SEM-03 step 1) |
| access check | 0x640-0x644 | 1 | 1 (`je`) | 1 (`access_` @0x640) | 0 | SEMANTICALLY_REQUIRED (SEM-03 step 2 table: write on read_only → invalid_argument) |
| zero-length + degenerate range | 0x64a-0x653 | 1 (`test rcx,rcx`) | 2 (`je` zero; `jle` sign of len — **reuses the flags of the preceding `test`; costs zero extra arithmetic instructions**) | 0 | 0 | SEMANTICALLY_REQUIRED (SEM-03 step 3; SEM-04 length must not exceed native transfer representability — "must not wrap/truncate") |
| null-data guard | 0x659-0x65f | 2 | 1 (`je`) | 0 | 0 | TRANSFORMABLE_WORK (candidate, low confidence) — same as §1.3 |
| call setup + spills | 0x665-0x67f | 6 (`sub $0x28`, 1 arg `mov`, 4 spills, `call`) | 0 | 0 | 4 | ABI_OR_COMPILER_COST (fewer live values than write_at: no offset, 40-byte frame) |
| native op | 0x67f | `call write@plt` | — | — | — | KERNEL_FLOOR |
| EINTR test | 0x684-0x68b | 2 | 1 (`jns` taken) | 1 (sret) | 0 | SEMANTICALLY_REQUIRED (SEM-05) |
| Result + epilogue | 0x790-0x7a7 | 6 | 0 | 0 | 3 | ABI_OR_COMPILER_COST (D3 shape) |
| **Total success path** | | **29** | **6 cond, 0 uncond** | **3** | **7** | frame 0x28 (40 B), 1 call |

### 2.2 Error paths and EINTR — `write`

Identical topology to `write_at` at shifted addresses: closed 0x760 (code 6), access and
range reject 0x740 (code 8), zero-length tail 0x720 (no `call`), null 0x770. `n<0` →
`call __errno_location` (0x691), reloads, `cmp $0x4`; EINTR → 0x7d8 reload fd → `jmp
0x669` re-entry (spill block inside the loop, as in write_at); errno mapping chain
identical (0x6ab-0x710, same nine compares, same `R_X86_64_32 kNativeErrorMappings+off`
table loads, offsets 0x0-0x38).

### 2.3 Comparison against the N1 pattern and verdict — `write`

Matched comparator `a1_write` (same session/flags; `::write` + do-while EINTR retry):
success path **35 instructions** — GCC chose callee-saved `rbx/rbp/r12/r13` for values
live across the `__errno_location` retry call, paying 4 spill stores up front and 3
reloads plus an exit `jmp` (matched.o 0x1a0-0x1e4, 0x23b-0x258). The shipping `write`
(29) is **6 instructions shorter** than the hand-written matched wrapper, with the same
call count, fewer stores (7 vs 7 — equal), fewer branches (6 vs 7), smaller frame.

**Initial verdict: PARITY_REACHED** at the static machine level (same measured-parity
caveat as §1.4).

---

## 3. Group findings

1. **No excess vs N1 remains on either write primitive.** Static success-path deltas:
   `write_at` −1, `write` −6 instructions versus freshly compiled A1-pattern
   comparators (same compiler, flags, session). The #600 B0 excess mechanism (Precheck
   struct materialization ≈27 instr + duplicate sign test ≈2) is fully absent at B1 on
   the write side too — C1's elision generalized beyond the measured `read_at` path.
2. **Kernel floor unchanged**: exactly one `pwrite`/`write` PLT call per op on the
   success path; zero-length paths contain no `call` (SEM-03 step 3 "without an OS
   call" verified statically; the dynamic oracle `zero_length_requests_make_no_native_call`
   exists at B1 `tests/direct_composition_fault_test.cpp:304` but was **not run** in
   this audit).
3. **Everything interface-visible is preserved in the same instruction budget**: closed
   precedence, access table, zero-length no-op, overflow-free range validation, EINTR
   retry without byte replay, errno canonicalization with `os_errno` retention.
4. The only candidates not defensible as interface-necessary are the two 3-instruction
   null-data guards (`blocking_file.cpp:107-108`, `:144-145`), recorded as
   G-WRITEAT-02 below — transformable, sub-noise, and *shared by the N1 comparator*, so
   their removal is not a beyond-parity gain; it is a mechanism-justification question.
5. Ledger anchors: this audit's B1 `read_at` recount (39 success-path instructions,
   same method) is consistent with #600's recorded 70 → 40 (PHASE3 V4); the ±1 is
   counting convention (ledger counts the two-line `movabs`/immediate encodings and
   `ret` inclusivity differently), not a baseline discrepancy.

## 4. Gap candidates

- **G-WRITEAT-01** (evidence gap, not excess): no E5 paired ladder arm exists for any
  write primitive (P0 §4.13). Recorded in Notes because the gap schema's
  `excessWork` field has no honest value — static evidence shows none. A write-arm
  extension of the (committed-arm) ladder is the prerequisite before any measured-parity
  claim.
- **G-WRITEAT-02** (low confidence, TRANSFORMABLE_WORK): the null-data admission guard
  at `blocking_file.cpp:107-108` (write_at) and `:144-145` (write). See AuditResult
  gaps entry. Not a parity gain (comparator carries it); magnitude ~3 instr, far below
  the ≈2.5-3 ns MDE; removal must pass the AGENTS.md Mechanism Justification Gate and
  has **no oracle and no mutant seam today** (no test constructs `span{nullptr, n>0}`,
  grep of `tests/blocking_file_write_test.cpp`, `tests/blocking_file_read_test.cpp`,
  `tests/file_access_precedence_test.cpp`: 0 hits).

## 5. Mutation seams and their gaps (write group, at B1)

Registered admission mutants (both embedded in `precheck_data_op`,
`include/sluice/detail/file_semantics.hpp:158-161` EMPTY_BEFORE_CLOSED and
`:168-170` ADMIT_INVALID_RANGE) cover the write primitives through the shared admission
path; `:192-195` ADMIT_UNREPRESENTABLE_SIZE is resize-only (out of group). Killer
suites are registered in `xmake/tests.lua:81-97` via `direct_mutant_target()`
(`:91` admit_invalid_range → `tests/blocking_file_read_test.cpp`; `:94`
empty_before_closed → `tests/blocking_file_sequential_test.cpp`; `:97`
admit_unrepresentable_size → `tests/blocking_file_state_test.cpp`) — all three killers
are read/resize-side; the write-side equivalents exist as independent assertions
(`write_with_illegal_offset_reports_invalid_argument`
`tests/blocking_file_write_test.cpp:246`, registered `:284`;
`blocking_write_at_closed_with_empty_buffer_reports_invalid_state`
`tests/file_access_precedence_test.cpp:149`) but are not registered mutant killers.
Since the mutants break the shared `precheck_data_op`, the registered read-side kills
do kill them; the write-side assertions add redundant coverage. Native-behavior seams:
`SLUICE_FILE_INTERNAL_TESTING` native-script injection covers `pwrite`/`write` EINTR
and short counts (`primitive_write_retries_eintr_then_stops_at_the_first_count`
`tests/direct_composition_fault_test.cpp:277`; `write_primitive_result_does_not_exceed_request`
`tests/blocking_file_write_test.cpp:225`). **Seam gaps for this group**: (a) no mutant
flips the write access arm (`access_allows` write/read_only) — public oracle
`sequential_write_on_read_only_reports_invalid_argument`
(`tests/blocking_file_sequential_test.cpp:459`) exists, but nothing verifies it kills;
(b) the null-data guards have neither oracle nor mutant (above); (c) no mutant or seam
exercises the errno-mapping chain on the write path specifically (the chain is shared
code, so a read-side kill would cover a break, but no such mutant is registered at
all). None of the mutant builds/tests were executed in this audit — seam *existence*
was verified by reading sources only.
