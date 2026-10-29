# P1 Static Cost Audit — Resource & Metadata Operations (File::open, File::close, file_info, size)

- Group: Direct resource & metadata read path (open / close / file_info / size).
- Auditor scope: static cost attribution against the N1 matched-native discipline.
- Baseline audited: **B1** — worktree `/home/jnhu/Source/Sluice-600-wt`, HEAD
  `77e5399a45998dea396699fa2e063c94a665945a`, archive
  `build/linux/x86_64/release/libsluice_core.a`, sha256
  `d1e351a54ded3dfa1df6b1cd2e47622eefbd5d7027439cb31a314bed3a3007f0`
  (re-verified this session with `sha256sum`; equals the frozen B1 identity in
  P0 §1.3). The worktree was used read-only; no reconfiguration or rebuild
  happened inside it.
- Date: 2026-10-27. Inputs read before auditing: P0
  `research/direct-beyond-parity/P0_BASELINE_AND_OWNERSHIP.md` §1, §3, §5; #600
  ledger `DIRECT_MACHINE_COST_LEDGER.md` (via `git show
  sluice-600-direct-research:research/600-direct-excess-cost/DIRECT_MACHINE_COST_LEDGER.md`);
  comparator snapshot `research/direct-beyond-parity/probe-snapshot-600/ladder.cpp`
  (A1 arm at `ladder.cpp:42-63`).
- Root authority: `docs/explicit-io-contract.md` (v2-r1) at the B1 tree.

## 0. Method

1. Source chain built from the B1 tree (`src/file_resource.cpp`,
   `src/blocking_file.cpp`, `include/sluice/detail/file_semantics.hpp`,
   `include/sluice/detail/posix_retry.hpp`, `include/sluice/result.hpp`,
   `include/sluice/error.hpp`).
2. Machine evidence: `ar x` of the frozen archive (members `file_resource.cpp.o`,
   `blocking_file.cpp.o`), `objdump -d` / `objdump -dr -C` per symbol. All
   instruction offsets below are object-section offsets from those dumps.
3. Matched-native controls (A1 discipline after `ladder.cpp:35-63`: hand-written
   minimal body, same public contract on the legal domain, same check order and
   error codes, EINTR retry where the Sluice op retries, `Result` ABI, separate
   TU, public Sluice headers only) built as
   `/tmp/p1-metadata-ladder/arms.cpp` with
   `g++ -std=c++20 -O3 -DNDEBUG -fvisibility=hidden -Wall -Werror -I/home/jnhu/Source/Sluice-600-wt/include -c arms.cpp -o arms.o`
   (exit 0), then disassembled the same way. Arms: `a1_open`, `a1_close`,
   `a1_file_info` (zero-init stat buffer, matching Sluice's source discipline),
   `a1_file_info_noinit` (uninitialized stat buffer — the minimal floor),
   `a1_size` (uninitialized buffer).
4. Success paths were traced by hand through the branch graph (default mode
   read-only/open-existing/preserve for open; open File for the rest), and the
   per-path instruction / call / conditional-branch / jmp / load / store tallies
   were computed by a script over the hand-traced address lists
   (`/tmp/p1-count2.py`; store = memory-destination operand, load =
   memory-source operand; `lea` excluded). Path address lists are recorded in
   that script; the tracing method and this limitation are part of the claim.
5. Classification rule from the ask: interface-required cost is never recorded
   as redundant, and "bare POSIX has fewer instructions" is not by itself
   eliminable work — the comparison baseline is the matched-native arm, not raw
   syscalls.

Adaptation disclosure (matched arms): `File`'s `(int, FileAccess)` constructor
is private (`include/sluice/file_resource.hpp:91`), so `a1_open`/`a1_close`
carry an 8-byte local `struct NF {int fd; FileAccess access;}` with the same
layout as `File`. `Result<NF>` emits the same 24-byte sret shape as
`Result<File>` (verified: identical store offsets 0/4/8/0xc/0x10 in both
disassemblies). The control arms live in `/tmp` only (P0 §4.8 rule: new P1
comparator arms need a committed, rebuildable identity — flagged in notes).

## 1. Success-path machine ledger

| Path | insn | call | cond-br | jmp | load | store | Frame |
|---|---:|---:|---:|---:|---:|---:|---|
| `sluice::File::open` (RO/OE/preserve, NUL-free) | 57 | 2 (memchr, open) | 9 | 1 | 9 | 9 | 0x28 |
| `a1_open` (same domain) | 44 | 2 | 6 | 1 | 7 | 7 | 0x10+push |
| `sluice::File::close` (open, close ok) | 18 | 1 (close) | 2 | 0 | 4 | 4 | 0x38 |
| `a1_close` | 18 | 1 | 2 | 0 | 4 | 4 | 0x38 |
| `File::close` on already-closed File | 13 | 0 | 1 | 0 | 4 | 3 | 0x38 |
| `~File` (open File) | 9 | 1 | 1 | 0 | 1 | 1 | 0x8 |
| `sluice::blocking::file_info` (open, fstat ok) | 49 | 1 (fstat) | 2 | 1 | 11 | 22 | 0xd8 |
| `a1_file_info` (zero-init buffer) | 49 | 1 | 2 | 1 | 11 | 22 | 0xd8 |
| `a1_file_info_noinit` (floor) | 39 | 1 | 2 | 1 | 11 | 13 | 0xd8 |
| `sluice::blocking::size` (open, fstat ok) | 31 | 1 (fstat) | 2 | 0 | 5 | 14 | 0xe8 |
| `a1_size` (floor, uninit buffer) | 22 | 1 | 2 | 1 | 4 | 5 | 0xa8 |

Deltas vs matched native: **open +13 instructions (+3 executed branches)**;
**close 0** (instruction-for-instruction identical); **file_info 0** vs the
zero-init matched arm, **+10 instructions / +9 stores** vs the minimal
(uninitialized-buffer) floor; **size +9 instructions / +9 stores** vs the
minimal floor (the same zero-init; `size` has no zero-init matched arm, its
matched arm is the floor form).

Reference scale (from #600, not re-measured here): ≈0.5–0.7 ns per userspace
instruction on E5 (28 insns ≈ 12.8–19.3 ns). fstat/open syscall floors are in
the #600 A0/A1 band (~530–970 ns warm at 4 KiB-class work), i.e. every op in
this group is syscall-dominated.

## 2. File::open

### 2.1 Chain (Public API → semantic validation → internal representation → native → Result)

| Step | Source location | Notes |
|---|---|---|
| Public API `Result<File> File::open(const std::string&, FileOpen)` | `include/sluice/file_resource.hpp:75`; impl `src/file_resource.cpp:68-80` | |
| Semantic validation `precheck_open` (embedded-NUL scan; read_only+truncate rejection) | `src/file_resource.cpp:69-73` → `include/sluice/detail/file_semantics.hpp:82-90` | NUL scan runs before any OS effect (`file_semantics.hpp:80-81` comment; SEM-02:384) |
| Rejection materialization `open_rejection_of` → `IoError{invalid_argument}` | `file_semantics.hpp:92-101` | |
| Internal representation: `open_flags_for` (O_CLOEXEC + access + existence + truncate), creation mode 0644 | `src/file_resource.cpp:31-57`, used at `:75` | 9 legal flag combinations |
| Native: `::open(path.c_str(), flags, 0644)` | `src/file_resource.cpp:75` | No EINTR retry (single attempt) |
| Error mapping `from_errno_value` → canonical code + preserved errno | `src/file_resource.cpp:77`; `include/sluice/error.hpp:66-94` | ERR-01 table |
| Result: 24-byte sret `Result<File>` {fd@0, access@4, code@8, errno@0xc, has_value@0x10} | `include/sluice/result.hpp:19-143` | |

### 2.2 Machine shape (objdump of `file_resource.cpp.o`, `File::open` at 0x0)

- Success path (57 insns): string size/data loads (+0x9/+0x15), `call memchr`
  (PLT32 reloc at +0x24) for the NUL scan, mode byte loads (+0x50/+0x55/+0x6a),
  legality test (+0x5b/+0x5f), the 6-constant flag table for the access row
  (+0x160..+0x17c: `mov $0x80200/%r14d`… six immediates, then `jmp` +0x182),
  existence/contents select via cmp/je/cmov (+0xa3..+0xbc), `mov $0x1a4,%edx`
  (0644) and `call open` (+0xc6), then the sret fill (+0x1c0..+0x1e1: fd,
  access, errno=0, has_value=1). Flag constants decode as
  O_CLOEXEC(|O_WRONLY|O_RDWR|O_CREAT|O_EXCL|O_TRUNC) — matches
  `open_flags_for` exactly.
- Error paths: precheck rejection returns `invalid_argument` with errno 0
  (+0x31..+0x4c); native failure calls `__errno_location` (+0xd7) then an
  8-compare unrolled dispatch (+0xe4..+0x126) loading the canonical byte from
  `kNativeErrorMappings+i*8+4` (relocs at +0x1f9…+0x256) and packing
  `(errno<<32)|code` into the sret (+0x131..+0x155). EINTR (=4) maps to
  `interrupted`; **no retry loop** — conforming shape (the SEM-05 retry rule
  governs data primitives; no root term requires an open retry).
- Stack: frame 0x28; 4 spill stores + 4 reloads on the path (mode, rbx, rdi,
  r14).

### 2.3 Classification

| Klass | Where | Machine evidence | Semantic basis (v2-r1) |
|---|---|---|---|
| SEMANTICALLY_REQUIRED | `src/file_resource.cpp:69-73`, `file_semantics.hpp:82-90` | +0x9/+0x15 loads, `call memchr` (+0x23), NULL test + branch (+0x2c/+0x2f) | SEM-02:384 embedded NUL rejected before OS effects |
| SEMANTICALLY_REQUIRED | `file_semantics.hpp:85-88` | +0x5b test/jne, +0x5f cmp, +0x63 je → invalid_argument return (+0x31) | SEM-02 legality table: read_only+truncate → invalid_argument |
| SEMANTICALLY_REQUIRED | `src/file_resource.cpp:31-57,75` | flag constants 0x80000–0x802c2; `mov $0x1a4,%edx` before `call open` | SEM-02:386 Linux baseline: close-on-exec, mode 0644 via umask |
| KERNEL_FLOOR | `src/file_resource.cpp:75` | `call open` (+0xc6) | the one native open; path resolution dominates |
| ABI_OR_COMPILER_COST | sret fill `+0x1c0..+0x1e1`; spills +0x4/+0xd/+0x1f/+0x65 | 24-byte `Result<File>` sret; 4 spill/reload pairs; identical sret offsets in `a1_open` | public Result ABI (result.hpp); #600 D3: Result scaffolding is interface cost |
| TRANSFORMABLE_WORK | `open_flags_for` `src/file_resource.cpp:31-58` (as compiled at +0x81..+0xbc, +0x160..+0x190) | 6 flag immediates loaded + jmp per call; 9 executed cond branches vs matched arm's 6; total 57 vs 44 insns | flag *values* are SEM-02-required; this instruction *layout* is not — the matched arm computes the same 9 combinations with cmp/cmov/or arithmetic in 13 fewer instructions, semantics identical |
| ABI_OR_COMPILER_COST | errno dispatch +0xd7..+0x126 | 8 cmp/je + table loads; same shape in matched arm (error path only) | ERR-01 errno mapping is required; the unroll shape is the compiler's |

### 2.4 Verdict: gap (small) — G-OPEN-01

## 3. File::close (and move/dtor context)

### 3.1 Chain

| Step | Source location |
|---|---|
| Public API `Result<void> File::close() noexcept` | `include/sluice/file_resource.hpp:82`; impl `src/file_resource.cpp:104-115` |
| Semantic validation: closed/moved-from → no-op success | `src/file_resource.cpp:105-107` (SEM-02:394) |
| Internal representation: ownership consumed *before* the native attempt (`std::exchange(fd_, -1)`) | `src/file_resource.cpp:110` |
| Native: single terminal `::close(fd)` via `close_native` (test seam under `SLUICE_FILE_INTERNAL_TESTING`) | `src/file_resource.cpp:21-29,111` |
| Error mapping (incl. EINTR→interrupted, **no retry**) | `src/file_resource.cpp:112`; SEM-02:391 |
| Result: 12-byte `Result<void>` returned in RDX:RAX (trivially copyable, 2 eightbytes) | `include/sluice/result.hpp:92-105,145-158` |

### 3.2 Machine shape (`File::close` at 0x310)

- Success path (18 insns): fd load/test/js (+0x314..+0x318), `movl $-1,(%rdi)`
  **before** `call close` (+0x348/+0x350) — ownership consumed on first attempt
  exactly as SEM-02:391 requires; rc test (+0x355), then the `Result<void>`
  construction staged through a stack slot (+0x31a..+0x340: store byte 1,
  reload, `movq $0`, store, reload, `mov %rcx,%rdx`, ret) — 4 loads/4 stores of
  register-return staging.
- Already-closed path: 13 insns, 0 calls (no syscall — oracle
  `close_on_closed_file_is_not_a_native_call`).
- Error path: `__errno_location` (+0x359), 8-cmp dispatch, table loads, error
  packed into the staging slot (+0x396..+0x39e). EINTR (cmp $0x4, +0x382)
  maps to `interrupted`; **no retry branch exists** — the disassembly has no
  backward edge to the call site, matching the single-attempt rule.
- `~File` (0x2e0): 9 insns, tail region calls `::close` directly, result
  ignored (best-effort, SEM-02:394-395). Move ctor (0x260): 6 insns, no
  syscall (LIFE-01 ownership transfer).

### 3.3 Classification

| Klass | Where | Machine evidence | Semantic basis |
|---|---|---|---|
| SEMANTICALLY_REQUIRED | `src/file_resource.cpp:105-107` | +0x314..+0x318 js → no-op success, 0 calls | SEM-02:394 close on closed File is a no-op success |
| SEMANTICALLY_REQUIRED | `src/file_resource.cpp:110-111` | `movl $-1,(%rdi)` precedes `call close`; single call site; no retry edge | SEM-02:391-393 first native close attempt consumes ownership, incl. EINTR; MUST NOT retry |
| KERNEL_FLOOR | `src/file_resource.cpp:111` | `call close` (+0x350) | the one native close |
| ABI_OR_COMPILER_COST | staging +0x31a..+0x340 | 4 loads/4 stores round-trip; **`a1_close` is instruction-for-instruction identical** (18=18 insns, same frame, same staging) | public `Result<void>` ABI; GCC emits this shape for the minimal matched body → not Sluice excess (D3 pattern) |
| SEMANTICALLY_REQUIRED | error mapping `src/file_resource.cpp:112` | EINTR→interrupted via `kNativeErrorMappings` load (+0x3d9 region) | ERR-01 canonical categories + preserved errno |

### 3.4 Verdict: PARITY_REACHED

0 instruction delta vs the matched arm on both the success and closed-no-op
paths; the ownership-consumed-before-syscall order and no-retry shape are
contract-mandated, not overhead.

## 4. file_info

### 4.1 Chain

| Step | Source location |
|---|---|
| Public API `Result<FileInfo> file_info(const File&)` | `include/sluice/blocking/file.hpp:27`; impl `src/blocking_file.cpp:171-191` |
| Semantic validation `precheck_state_op` (closed → invalid_state; access always legal for file_info) | `src/blocking_file.cpp:172-174` → `file_semantics.hpp:178-185`, `access_allows` `:54-68` (file_info row returns true — compile-time elided) |
| Internal representation: zero-initialized `struct ::stat st{}` on the stack (144 B) | `src/blocking_file.cpp:179` |
| Native: `fstat` with EINTR retry (`retry_on_eintr`) | `src/blocking_file.cpp:180` → `include/sluice/detail/posix_retry.hpp:7-15` |
| Field extraction: kind (S_ISREG), size (st_size), identity always engaged (dev/ino) | `src/blocking_file.cpp:185-190` |
| Error mapping on failure | `src/blocking_file.cpp:182` |
| Result: 56-byte sret `Result<FileInfo>` {kind@0, size@8, identity-engaged@0x10, dev@0x18, ino@0x20, code@0x28, errno@0x2c, has_value@0x30} | `include/sluice/file_resource.hpp:47-51`; `result.hpp` |

### 4.2 Machine shape (`blocking_file.cpp.o`, `file_info` at 0x930)

- Success path (49 insns): fd load/test/js (+0x942..+0x946), `pxor` + **9
  `movaps` zeroing 144 B at rsp+0x30..0xc0** (+0x94c..+0x98c), `lea` + `call
  fstat` (+0x994/+0x999), rc test (+0x9a2), then extraction and staging:
  st_mode (+0xa50), `and $0xf000` / `cmp $0x8000` / `setne` (kind),
  st_size/st_dev via `movq`/`movhps` into xmm (+0xa58/+0xa5e), st_ino
  (+0xa81), identity-engaged byte = 1 (+0xaa1), staged at rsp+0x0..0x28 then
  copied to the sret with `movups`/`movdqa`/`mov` (+0xa8e..+0xab8). 11 loads,
  22 stores total.
- Closed path: 4-insn error return (code=6 invalid_state) at +0xa28.
- Error path: `__errno_location` (+0x9aa), 8-cmp dispatch; **EINTR retry
  folded**: `cmp $0x4` (+0x9b1) je +0xac8 → reload fd from the File
  (`mov 0x0(%rbp),%edi`) → `jmp` back to re-issue fstat with the same buffer
  (+0x994). No count can have been completed (metadata op), so the retry is
  safe per SEM-05/ERR-01.
- Stack object shape: frame 0xd8 — 144-B stat at +0x30, 40-B FileInfo staging
  at +0x00..0x28, 2 saved regs.

### 4.3 Classification

| Klass | Where | Machine evidence | Semantic basis |
|---|---|---|---|
| SEMANTICALLY_REQUIRED | `src/blocking_file.cpp:172-177` | +0x942..0x946 js → +0xa28 invalid_state return | SEM-03:412 step 1 (closed → invalid_state); SEM-07 |
| KERNEL_FLOOR | `src/blocking_file.cpp:180` | `call fstat` (+0x999) — exactly one metadata syscall | SEM-07 observation-time metadata |
| SEMANTICALLY_REQUIRED | `src/blocking_file.cpp:185-190` | +0xa50 mode/`setne` kind; +0xa58 size; +0xa5e/+0xa81 dev/ino; engaged=1 | SEM-07:517-535 FileInfo contents; Linux dev/ino identity; identity availability explicit |
| SEMANTICALLY_REQUIRED | `src/blocking_file.cpp:180` / `posix_retry.hpp:7-15` | folded retry at +0x9b1→+0xac8→+0x994 | SEM-05/ERR-01 EINTR discipline |
| TRANSFORMABLE_WORK | `src/blocking_file.cpp:179` (`struct ::stat st {}`) | pxor + 9 movaps = 10 insns / 9 stores / 144 B per call; `a1_file_info_noinit` removes exactly them (49→39 insns, 22→13 stores); fstat success defines every consumed field (st_mode/st_size/st_dev/st_ino) | no v2-r1 term requires zeroing the buffer; SEM-07 requires the observed values only. Caveat: the zero-init *matched* arm carries it too (see 4.4), and hygiene arguments (reserved fields, MSan) are the counterweight — [UNCERTAIN] whether dropping `{}` is admissible |
| ABI_OR_COMPILER_COST | staging +0xa54..+0xab8 | ≈12 staging loads/stores before the sret fill; **`a1_file_info` (zero-init) emits the identical 49-insn / 22-store shape** | public Result/FileInfo ABI; #600 D3: emitted identically for matched natives |

### 4.4 Verdict: PARITY_REACHED vs the matched-native discipline (zero-init arm identical, 49=49); one transformable item vs the minimal floor — G-META-01

## 5. size

### 5.1 Chain

| Step | Source location |
|---|---|
| Public API `Result<std::uint64_t> size(const File&)` | `include/sluice/blocking/file.hpp:29`; impl `src/blocking_file.cpp:193-199` |
| Semantic validation: projection of `file_info` (closed → invalid_state) | `src/blocking_file.cpp:194-197` (GCC inlines the whole `file_info` body; the call at source level disappears) |
| Internal representation: inlined zero-init stat buffer (144 B) + dead-code-eliminated kind/identity | `src/blocking_file.cpp:193-198` as compiled |
| Native: one `fstat` with folded EINTR retry | compiled at `blocking_file.cpp.o` +0xb8c, retry +0xba0→+0xc70→+0xb87 |
| Result: 24-byte sret `Result<uint64_t>` {value@0, code@8, errno@0xc, has_value@0x10} | `result.hpp` |

### 5.2 Machine shape (`size` at 0xb20)

- Success path (31 insns): fd load/test/js (+0xb32..+0xb36), `pxor` + 9
  `movaps` zeroing 144 B (+0xb3c..+0xb7f), `lea` + `call fstat` (+0xb87/+0xb8c),
  rc test (+0xb91), single `mov 0x70(%rsp),%rax` (st_size) then the sret fill
  (+0xc1d..+0xc30: errno=0, has_value=1, value). **Kind/dev/ino computation is
  eliminated after inlining** — only st_size is consumed. 5 loads, 14 stores.
- Exactly **1 fstat per call** — SEM-01:361 "`size` may be a projection of
  `file_info`; no duplicate metadata operation is required". (The dududu
  adapter's file_info+size double-fstat, P0 §4.5 / #600 E4, is adapter-side
  and out of this audit's object.)
- Closed path: 4-insn invalid_state return (+0xc50..+0xc6c). Error path: same
  8-cmp errno dispatch; EINTR folded retry (+0xc70 reload fd → re-issue).

### 5.3 Classification

| Klass | Where | Machine evidence | Semantic basis |
|---|---|---|---|
| SEMANTICALLY_REQUIRED | `src/blocking_file.cpp:194-197` | +0xb32..0xb36 js → +0xc50 invalid_state | SEM-03 step 1; SEM-07 |
| KERNEL_FLOOR | inlined `file_info` native step | `call fstat` (+0xb8c), 1 syscall/op | SEM-01:361 projection permitted; observation-time size (SEM-07) |
| SEMANTICALLY_REQUIRED | EINTR retry | folded at +0xba0/+0xc70 | SEM-05/ERR-01 |
| TRANSFORMABLE_WORK | zero-init in the inlined body (`src/blocking_file.cpp:193-198`, originating `:179`) | pxor + 9 movaps (+0xb3c..+0xb7f); sluice 31 insns/14 stores vs `a1_size` floor 22/5 — delta is exactly the zeroing | same basis as §4.3; not interface-required |
| ABI_OR_COMPILER_COST | sret fill +0xc1d..+0xc30 | 24-byte `Result<uint64_t>` sret; same ABI shape in `a1_size` | #600 D3 |

### 5.4 Verdict: PARITY_REACHED vs matched-native discipline modulo the same zero-init item — G-META-01 (shared)

## 6. Gap candidates

### G-OPEN-01 — flag-table materialization and spill shape in `File::open`

- Source: `src/file_resource.cpp:31-58` (`open_flags_for`) as compiled into
  `File::open` (+0x81..+0xbc, +0x160..+0x190); spill pair at +0x65/+0x1c2.
- Trigger: every `File::open` on the legal domain (any mode).
- Necessary work: NUL scan, read_only+truncate rejection, correct flag values
  (O_CLOEXEC + access/existence/contents), one `::open` with 0644, errno
  mapping, `Result<File>` construction.
- Excess work: +13 static success-path instructions vs the matched arm
  (57 vs 44), incl. +3 executed conditional branches and a 6-immediate flag
  table where cmp/cmov/or arithmetic computes the same 9 combinations in fewer
  instructions.
- Magnitude: ≈13 insns ≈ 6–9 ns userspace at the #600 ≈0.5–0.7 ns/insn band
  [UNCERTAIN conversion], on an op whose kernel floor (path resolution) is
  ≳1 µs — expected relative effect ≤~1% and likely below the 2.5–3 ns MDE at
  the op's own noise level.
- Mechanism direction: reshape `open_flags_for` to arithmetic construction
  (access OR-composition instead of a full constant per combination), keep the
  9 outputs bit-identical (assert table in a unit test).
- Falsifier: (a) candidate rebuild → objdump success path ≤ matched-arm count;
  (b) 16-pair ABBA ladder over an open/close loop on tmpfs short paths — if
  the delta is not repeatedly measurable (likely), the candidate dies by the
  #600 admission rule and only the static elision remains.
- Confidence: medium (static delta certain; eliminability plausible; measured
  benefit unlikely).

### G-META-01 — 144-byte stat-buffer zero-init in `file_info` and `size`

- Source: `src/blocking_file.cpp:179` (`file_info`); the same init inlined
  into `size` (`src/blocking_file.cpp:193-198`).
- Trigger: every `file_info`/`size` call on an open File (metadata-heavy
  loops; the many-small-reads shape's per-open metadata step).
- Necessary work: closed check, one `fstat` with EINTR retry, field
  extraction/projection, errno mapping, Result construction.
- Excess work: pxor + 9 `movaps` zeroing 144 B that `fstat` success overwrites
  for every consumed field (file_info 49→39 insns, 22→13 stores; size 31→22,
  14→5).
- Magnitude: ≈10 insns ≈ 5–7 ns userspace per call against a ≈0.5–1 µs fstat
  floor — ≈0.5–1%; static elision is provable, wall gain likely below MDE.
- Mechanism direction: declare the stat buffer without `{}` in both sites (no
  buffer reuse/hoisting — a shared buffer would be a new runtime mechanism and
  a reentrancy hazard).
- Falsifier: (a) rebuild without `{}` → objdump shows the noinit shape at both
  symbols; (b) MSan configuration of the 58-test matrix stays clean (proves no
  uninitialized value is consumed); (c) paired ABBA ladder on a size/file_info
  loop — expect n.s. vs the fstat floor.
- Confidence: medium (machine delta certain; admissibility of dropping `{}` —
  reserved-field hygiene, sanitizer discipline — [UNCERTAIN], decided by the
  falsifier's MSan gate, not by this audit).

Priority context for both gaps: in this group the kernel floor dominates, so
the #600 admission rule ("only on repeatedly measurable gain") will likely
reject both as standalone tasks; their value is (i) static-parity closure of
the Direct surface and (ii) the P4 many-small-reads application shape, where
they ride along. The dududu double-fstat (P0 §4.5) is a whole syscall per open
and dwarfs both — fix that comparator defect before any application-shape
claim.

## 7. Mutation/fault-seam inventory for this group (B1 state)

- `File::close`: fault seam exists — `NativeCall::close` interception
  (`src/file_resource.cpp:21-29`, seam `src/file_test_seams.hpp:16-22`);
  EINTR script `{{-1,EINTR}}` driven by
  `tests/file_close_semantics_test.cpp:96-100`
  (`close_eintr_consumes_ownership_without_retry`) and `:210`. Killer oracle
  present and named in P0 §5.1.
- Move/lifetime: `SLUICE_LIFE_MUTANT_MOVE_CLOSES_NATIVE`
  (`src/file_resource.cpp:62-66,84-88`), armed by
  `tests/file_move_borrow_test.cpp:127,178`.
- `File::open`: **no fault seam** (`NativeCall` has no `open`) and **no mutant
  seam** for `precheck_open`; coverage is natural-oracle only
  (`tests/file_open_contract_test.cpp` 36 scenarios; `semantic_open_test`
  embedded-NUL cases). Open failure paths are exercised through real errno
  (ENOENT etc.), not injection; EINTR on open cannot be injected.
- `file_info`/`size`: **no fault seam** (`NativeCall` has no `fstat`) — the
  folded EINTR retry (`src/blocking_file.cpp:180`) and the fstat-failure errno
  mapping have **no deterministic injection oracle at B1**; the only tested
  error path is the closed-file precheck rejection
  (`tests/file_info_identity_test.cpp`, `tests/blocking_file_state_test.cpp`
  assert success shapes and the closed case). Any P1+ candidate touching the
  fstat error/retry path cannot be mutation-tested without adding a seam, and
  a new seam must pass the mechanism-justification gate.
- Data-op admission mutants (P0 §5.3) do not cover this group's prechecks:
  `precheck_open` and `precheck_state_op` have no registered mutant.

## 8. Session record

Commands run this session (all on this E5 host, read-only against the frozen
worktree): `sha256sum …/libsluice_core.a` (d1e351a5…, matches P0 §1.3);
`ar t` / `ar x` + `nm -C` + `objdump -d(-r) -C` over both member objects;
control-arm compile (command in §0, exit 0) + `objdump -d -C`; tallies via
`/tmp/p1-count2.py`; greps over the B1 tree for seams/mutants/tests cited
above. Not run: any runtime measurement (static audit only — P1 measurement
tasks will need their own preregistered ladder per P0 §4.11/§4.12); the
58/58 matrix; mutation kills.
