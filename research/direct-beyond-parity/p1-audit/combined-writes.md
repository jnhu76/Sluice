# P1 Static Cost Audit — Combined Writes (`write_all_at`, `write_all`)

- Auditor group: 审计员·组合写 (combined writes).
- Audit target: B1 frozen worktree `/home/jnhu/Source/Sluice-600-wt`, HEAD
  `77e5399a45998dea396699fa2e063c94a665945a`, archive
  `build/linux/x86_64/release/libsluice_core.a` sha256
  `d1e351a54ded3dfa1df6b1cd2e47622eefbd5d7027439cb31a314bed3a3007f0`
  (identity re-verified this session: `git -C /home/jnhu/Source/Sluice-600-wt
  rev-parse HEAD`; `sha256sum` of the archive). Read-only use: no
  reconfiguration, no rebuild inside the worktree.
- Inputs read: `research/direct-beyond-parity/P0_BASELINE_AND_OWNERSHIP.md`
  §1/§3/§5; #600 `DIRECT_MACHINE_COST_LEDGER.md` (`git show
  sluice-600-direct-research:research/600-direct-excess-cost/DIRECT_MACHINE_COST_LEDGER.md`);
  v2-r1 `docs/explicit-io-contract.md` SEM-01/03/04/05, ERR-01/02;
  `research/direct-beyond-parity/probe-snapshot-600/ladder.cpp` (A1 pattern).
- All disassembly: `objdump -d --demangle` on
  `blocking_file.cpp.o` extracted from the frozen archive with
  `ar x /home/jnhu/Source/Sluice-600-wt/build/linux/x86_64/release/libsluice_core.a`
  (session working copy `/tmp/p1-audit-combined-writes/`).
- Comparator: A1-pattern matched-native write compositions, single-TU compile
  this session (§3). Flags identical to the P0-recorded build recipe
  (`P0_BASELINE_AND_OWNERSHIP.md` §1.5):
  `CCACHE_DISABLE=1 /usr/lib64/ccache/gcc -m64 -fvisibility=hidden -Wall -Werror
  -O3 -std=c++20 -DNDEBUG -I/home/jnhu/Source/Sluice-600-wt/include -c
  a1_writes.cpp -o a1_writes.o` (exit 0). Source preserved at
  `/tmp/p1-audit-combined-writes/a1_writes.cpp`; sha256 of this session's
  working copies recorded in Appendix C.
- Scope: static machine audit only. No timing was run; every ns figure below is
  a scaled estimate flagged `[UNCERTAIN]` (P0 §4.13: no E5 ladder exists for
  write paths).

## 1. Operation chains (Public API → semantic validation → internal
representation → native operation → Result)

### 1.1 `write_all_at(file, offset, src)`

| Step | Source location | What happens |
|---|---|---|
| Public API declaration | `include/sluice/blocking/file.hpp:54-55` — `Result<CompositionOutcome> write_all_at(const File&, std::uint64_t, std::span<const std::byte>)` | Return payload `CompositionOutcome{confirmed_bytes, end, remaining, optional<IoError>}` declared at `include/sluice/blocking/file.hpp:42-49`; `CompositionEnd` at `:33-38`. |
| Semantic validation (outer admission) | `src/blocking_file.cpp:302-312` builds `DataOpRequest{!file.is_open(), file.access(), write, offset, src.size()}` via `admit_data_op` (`src/blocking_file.cpp:25-29`) and calls `detail::precheck_data_op` (`include/sluice/detail/file_semantics.hpp:157-176`): closed → `reject_closed` (`invalid_state`); access via `access_allows` (`:54-68`, read_only → `reject_access` = `invalid_argument`); `length == 0` → `complete_empty`; range via `range_is_valid` (`:32-39`, last addressed byte `offset + (length-1)` without overflow) and `length > kMaxNativeTransfer` (`:29-30`, `kMaxNativeTransfer` = `ssize_t` max) → `reject_range`. Mapping to `IoError` by `is_rejection`/`rejection_error` (`:111-129`). Null-data check after admission (`src/blocking_file.cpp:310-312`). |
| Zero-length short-circuit | `src/blocking_file.cpp:306-307` returns `CompositionOutcome{}` (complete, 0 confirmed, no OS call — SEM-03 step 3). |
| Internal representation (composition state) | `src/blocking_file.cpp:313-315` calls the `compose` template (`:269-283`) with `CompositionKind::write_all`, `total = src.size()`, and a step lambda returning `write_at(file, offset + confirmed, src.subspan(confirmed))`. Loop state is `CompositionState{confirmed_bytes, stopped, stop, error}` (`include/sluice/detail/file_semantics.hpp:232-240`); progress classified by `compose_progress` (`:243-263`): `transferred > remaining` → `impossible_count`; advance `confirmed_bytes`; completion at `total`; `transferred == 0` → `write_no_progress`. Errors via `compose_error` (`:265-272`). |
| Per-step primitive (nested public call) | `write_at` (`src/blocking_file.cpp:99-118`): full re-admission (same `precheck_data_op`), null check, then `retry_on_eintr` (`include/sluice/detail/posix_retry.hpp:5-12`) around `native_pwrite` (`src/blocking_file.cpp:68-75`) → `::pwrite(fd, src.data()+confirmed, remaining, off_t(offset+confirmed))`. |
| Native operation | `::pwrite` — relocation `R_X86_64_PLT32 pwrite` at object offset `0x1421` in `write_all_at` (inlined copy; the standalone `write_at` has its own at `0x29b`). Exactly 1 syscall per step; 0 syscalls on the zero-length path. |
| Result construction | `direct_outcome` (`src/blocking_file.cpp:241-265`) maps `CompositionStop` → public `CompositionEnd`, sets `remaining` via `publish_certainty` (`:236-239`) from `composition_effect_certainty` (`include/sluice/detail/file_semantics.hpp:338-351`): `primitive_error`/`impossible_count` → `unknown`; `write_no_progress` → `accounted`. Composition failures return a *successful* `Result` whose value carries `end`/`error`; only admission rejections return `make_unexpected` (`src/blocking_file.cpp:308-309`). Errno mapping inlined from `from_errno_value` (`include/sluice/error.hpp:92`, table `:66-79`). |

### 1.2 `write_all(file, src)` (shared-cursor composition)

Identical chain with: declaration `include/sluice/blocking/file.hpp:59`;
implementation `src/blocking_file.cpp:331-342`; admission with `offset = 0`
(`:332`), so the only range check is the transfer limit (`length >
kMaxNativeTransfer`, `include/sluice/detail/file_semantics.hpp:173-174`);
step lambda `write(file, src.subspan(confirmed))` (`:340-341`) →
`src/blocking_file.cpp:138-154` → `retry_on_eintr` around `native_write`
(`:60-66`) → `::write(fd, src.data()+confirmed, remaining)` — shared-cursor
form, no offset argument (SEM-04: the native open-file-description cursor is
the position authority; no hidden seek). Relocation `R_X86_64_PLT32 write` at
`0x1b44` (composition) and `0x2a0`-region for standalone `write`.

### 1.3 Zero-progress stop and confirmed-bytes advancement (explicitly audited)

- Zero-progress stop: `compose_progress` marks `write_no_progress` when a step
  returns 0 with confirmed < total (`include/sluice/detail/file_semantics.hpp:257-261`);
  the loop exits (`state.stopped`, `src/blocking_file.cpp:273`) after exactly
  one zero-returning attempt. Machine: success continuation `0x1611 test
  %rax,%rax; 0x1614 jne` — fall-through publishes `end = 2`
  (`write_no_progress`), `remaining = 0` (`accounted`), no error
  (`0x1616-0x1647`). Oracle: `zero_progress_write_stops_after_one_attempt`
  asserts `script.calls() == 1`
  (`tests/direct_composition_fault_test.cpp:132-150`).
- Confirmed-bytes advancement: `0x1609 add %rax,%rbx` (confirmed += n) after
  the impossible-count guard (`0x1600-0x1603`); on primitive error the already
  confirmed prefix survives (`compose_error` only marks stop/error,
  `include/sluice/detail/file_semantics.hpp:265-272`; publish path
  `0x14a0-0x1543` carries `confirmed_bytes` through). Oracles:
  `short_writes_reach_full_progress` (buffer/count/offset assertions,
  `tests/direct_composition_fault_test.cpp:110-130`),
  `error_after_confirmed_prefix_keeps_the_prefix` (256/1024 prefix, `unknown`
  remainder, `no_space` + `ENOSPC`, `:197-222`).

## 2. Machine evidence from the B1 archive

All counts below were produced by a script over the disassembly
(`/tmp/p1-audit-combined-writes/count_paths.py`, corrected run) that classifies
each instruction on an explicitly enumerated success-path address sequence.
"1-step" = the whole request completes in a single native call (the dominant
warm path for small writes); "/extra-step" = each additional loop iteration
after the first (multi-short-write shape).

| Path | Total insns | cond-branch | call | load (incl 16B) | store (incl 16B) |
|---|---|---|---|---|---|
| B1 `write_all_at` 1-step success | **101** | 14 | 1 | 16 | 31 |
| B1 `write_all_at` per extra step | **44** | 9 | 1 | 6 | 9 |
| B1 `write_all` 1-step success | **82** | 12 | 1 | 13 | 27 |
| B1 `write_all` per extra step | **34** | 8 | 1 | 4 | 8 |
| A1 `write_all_at` 1-step success | **72** | 9 | 1 | 17 | 20 |
| A1 `write_all_at` per extra step | **27** | 5 | 1 | 7 | 4 |
| A1 `write_all` 1-step success | **60** | 8 | 1 | 11 | 17 |
| A1 `write_all` per extra step | **23** | 5 | 1 | 3 | 2 |

Whole-function sizes (all paths, `objdump` per-function count):
`write_all_at` 241 instructions, `write_all` 208, standalone `write_at` 129,
`write` 118. The standalone `write_at` single-call success path is **40**
instructions (`0x220-0x2a8` admission+frame+call+branch, `0x3b0-0x3c7`
success tail) — identical to the #600 post-C1 `read_at` figure of 40
(`PHASE3_VALIDATION.md` V4), confirming the archive matches the C1-elided
shape.

Composition increment inside B1 (write-path analogue of #600's AX−A2):
`write_all_at` 1-step (101) − `write_at` 1-call (40) = **+61**, consistent
with the read-side 45–60 band recorded in `DIRECT_MACHINE_COST_LEDGER.md`.

### 2.1 `write_all_at` shape (object offsets `0x1330-0x1729`)

- Entry + outer admission `0x1330-0x13bd` (30 insns): frame `sub $0x98,%rsp`;
  6 callee-saved spills (r12/rbp/r13/r14/r15/rbx at `0x78/0x70/0x80/0x88/0x90/0x68(%rsp)`);
  fd load `mov (%rsi),%ecx` + closed test `0x1346-0x1348` (js → `0x15c0`
  publishes `invalid_state`, code 6); access `cmpb $0x0,0x4(%rsi)` +
  `0x1356-0x135a` (je → `invalid_argument`, code 8); zero-length `test
  %r8,%r8; je 0x1548` → all-zero `CompositionOutcome`, `has_value = 1`, **no
  syscall**; range: `lea -0x1(%r8)` + `movabs $0x7fffffffffffffff` + `sub` +
  `cmp/jb` (`0x1374-0x1388`) fused with the sign test `or %r15,%r8-eq; js`
  (`0x138e-0x1394`) — GCC fuses `offset > kMaxNativeOffset` and `length >
  kMaxNativeTransfer` into one `or`+sign test (COMPILER_ELIDED, same fusion
  #600 recorded for `read_at`); null check `cmpq $0x0,0x8(%rsp); je 0x15d0`
  (`0x139a-0x13a0`).
- Loop head `0x13c0-0x141f` (24 insns to the call): fd reload `mov
  0x0(%rbp),%edi` + closed re-test (`0x13c0-0x13c5`); remaining/advanced-offset
  derivation `mov %r8,%r14; lea (%rbx,%r15,1),%r13; sub %rbx,%r14`
  (`0x13cb-0x13d2`); access re-check `0x13d5-0x13d9`; step range re-validation
  — sign `or/js` (`0x13df-0x13e5`) + `movabs/lea/sub/cmp/jb`
  (`0x13eb-0x13ff`); buffer re-derivation `mov 0x8(%rsp),%rax; add %rbx,%rax;
  mov %rax,0x18(%rsp)` (`0x1405-0x140d`, the `0x18(%rsp)` store feeds the
  EINTR re-entry); arg setup + total spill `mov %r8,0x10(%rsp)`
  (`0x1412-0x141b`); `call pwrite` (`0x1420`); reload + `test/jns`
  (`0x1425-0x142d`).
- Success continuation `0x1600-0x1614`: impossible-count guard `cmp %rax,%r14;
  jb 0x16b0` (→ `end=primitive_error`, code 4 = `invalid_state`, `remaining=unknown`,
  confirmed NOT advanced); `add %rax,%rbx`; completion `cmp %rbx,%r8; je`;
  zero-progress `test %rax,%rax; jne`.
- Loop tail / state staging `0x1650-0x167f` (10 insns, executed **every**
  iteration): stores `confirmed_bytes`(`0x40`), `end+remaining`(`0x48`, 2B),
  error code(`0x4c`), errno(`0x30` zero), then `movdqa 0x40(%rsp),%xmm0`
  (16B load) and **`movaps %xmm0,0x20(%rsp)` at `0x1677` — a 16-byte store to
  a slot never read anywhere in the function** (single textual occurrence;
  verified by grep of the disassembly) — then loop condition `cmp/jb`.
- Complete exit `0x1685-0x16a1` (8 insns) → shared publish tail
  `0x14d3-0x1543` (21 insns): staging stores (`0x48/0x49/0x4c/0x50/0x54`),
  `movdqa` re-load, sret stores `movups %xmm0,(%r12)` (16B outcome head),
  `mov %rax,0x10(%r12)` (errno), `movl $0x0,0x1c(%r12)`, `movb $0x1,0x20(%r12)`
  (`has_value`), callee-saved restores, `add $0x98,%rsp`, `ret`.
- EINTR shape (error path): `call __errno_location` (`0x1433`), `mov
  (%rax),%eax`, `cmp $0x4,%eax; je 0x16e0` → `mov 0x0(%rbp),%edi; mov
  0x18(%rsp),%rsi; jmp 0x1415` — re-issues the same `pwrite` with identical
  args, **skipping re-admission and not advancing `confirmed`** (no replayed
  bytes; SEM-05). One extra stack store (`0x140d`) is paid on the success path
  to feed this retry.
- Errno mapping shape `0x1448-0x1494`: `cmp/je` chain over `0xd`(EACCES),
  `0x1`(EPERM), `0x2`(ENOENT), `0x14`(ENOTDIR), `0x1c`(ENOSPC), `0x7a`(EDQUOT),
  `setne/lea` for `0xb`(EAGAIN) → canonical codes, default `backend_error` —
  inlined `from_errno_value`; identical chain exists in the A1 arm (same public
  header).
- Error publish `0x14a0-0x1543`: `end = 3` (primitive_error), `remaining = 1`
  (unknown), error code + `os_errno` round-trip (`mov %eax,0x50(%rsp)` …
  `mov 0x50(%rsp),%rax` → `0x10(%r12)`) — the same errno store/reload shape
  #600's D3 classified as necessary `Result` ABI scaffolding (present in A1
  too, at `0x4cb/0x4df` of the comparator).
- Frame/stack object shape: 0x98 bytes — callee-save area `0x68-0x98`;
  `src.data` spill `0x8`; loop-carried `total` spill `0x10`; `buf+confirmed`
  EINTR spill `0x18`; dead outcome shadow `0x20` (16B); outcome staging
  `0x30-0x57` (`confirmed/end/remaining/code/errno/has_value`), re-loaded via
  `movdqa` and copied to the sret by `movups`. Result object at sret:
  16B outcome head + 8B errno + 4B zero pad + `has_value` byte at `+0x20`.

### 2.2 `write_all` shape (object offsets `0x1ab0-0x1e28`)

Same structure, smaller admission (no offset arithmetic): closed
(`0x1abf-0x1ac3`), access (`0x1ad1-0x1ad5`), zero-length (`0x1adb-0x1ade` →
`0x1c60`, no syscall), transfer-limit sign test (`test %rcx,%rcx` … `jle
0x1c90`, `0x1ae9`), null (`0x1af2-0x1af5`). Loop head `0x1b10-0x1b43` (14
insns to the call): fd reload + closed re-test (`0x1b10-0x1b14`), access
re-check (`0x1b1a-0x1b1e`), remaining derivation + re-sign-test
(`0x1b24-0x1b2d`), buffer derivation, total spill `mov %rcx,0x8(%rsp)`
(`0x1b3e`), `call write` (`0x1b43`). EINTR at `0x1b56-0x1b65` → `0x1de0`
re-issues with `jmp 0x1b38`. Tail staging `0x1d50-0x1d7f` includes the dead
16B store `movaps %xmm0,0x10(%rsp)` at `0x1d77` (single occurrence, never
read). Frame 0x88 bytes.

### 2.3 Syscall accounting

Strace-level: 1 native call per composition step, 0 on the zero-length path —
not re-measured here (static evidence: single `call pwrite`/`call write` per
loop iteration, and the zero-length path branches to the publish tail before
the loop); the #600 strace result (1.00 pread64/op, 0 syscalls on zero-length)
is the read-side analogue `[TRANSCRIBED]`.

## 3. A1-pattern matched-native comparator (write arms)

`ladder.cpp` covers reads only (P0 §4.13); per the audit instruction I
constructed the write composition comparator in the same A1 pattern as
`research/direct-beyond-parity/probe-snapshot-600/ladder.cpp:42-63`:
hand-written minimal bodies, public Sluice headers only (no `detail/`
headers), same check order (closed → access → zero-length → range/transfer →
null), same error codes, EINTR retried without a completed count, confirmed
prefix preserved on error with `unknown` remainder, impossible count →
`invalid_state`/`unknown`, zero-progress stop with `accounted` remainder,
failure carried as a *successful* `Result<CompositionOutcome>` — exactly the
B1-observable contract (§1). Separate TU, compiled with the P0-recorded
reconstruction flags (command in the header of this document; exit 0).
Behavioral equivalence is by construction (same source-level semantics);
**no runtime differential test was executed in this audit** — the paired
measurement is left to the P1 measurement phase, matching how #600 treated the
A1 arm before its ladder runs.

Result (from §2 table): single-native-call success 72 (`write_all_at`
pattern) / 60 (`write_all` pattern); per-extra-step 27 / 23.

**Beyond-parity excess (B1 − A1, static):**

| Operation | 1-step | per extra step |
|---|---|---|
| `write_all_at` | **+29** (101 − 72) | **+17** (44 − 27) |
| `write_all` | **+22** (82 − 60) | **+11** (34 − 23) |

Syscall parity holds (1 vs 1 per step; 0 vs 0 on zero-length); branch counts
+5/+3; store traffic +11/+8 on the 1-step path.

## 4. Cost classification (8 classes)

Interface-required cost is not recorded as redundant, and a bare-POSIX arm
having fewer instructions is not by itself evidence of removable Sluice work
(the A1 arm — not raw POSIX — is the semantic floor here; see
`DIRECT_MACHINE_COST_LEDGER.md` A1 definition and P0 §1.4).

### 4.1 SEMANTICALLY_REQUIRED

1. Outer admission (closed/access/zero-length/range/transfer/null), `write_all_at`
   `0x1330-0x13a0`, `write_all` `0x1ab0-0x1af5`; source
   `src/blocking_file.cpp:302-312`, `:331-339`. Machine: 24–25 check+branch
   instructions (fused to two compares + one `or`/sign test + one
   last-byte subtraction compare by GCC). Basis: SEM-03 steps 1–4 (precedence
   table; zero-length logical no-op skipping range checks), SEM-04 (validate
   starting offset and last addressed byte without overflow; transfer limit),
   V01 closed-file precedence. The A1 arm carries the same checks (≈22 insns).
2. Per-step progress law (impossible-count guard, confirmed advance,
   completion test, zero-progress stop): `0x1600-0x1614` / `0x1d00-0x1d14`
   (5–7 insns). Source `include/sluice/detail/file_semantics.hpp:243-263`.
   Basis: SEM-05 ("stops on full success, zero progress"; "check arithmetic";
   "advance buffer/offset by confirmed bytes"), ERR-02 (confirmed prefix
   distinct from reason; a zero count is trustworthy → `accounted`).
3. Confirmed-prefix survival and effect certainty on failure: publish tails
   (`0x14a0-0x1543`, `0x1bc0-0x1c57`); source
   `include/sluice/detail/file_semantics.hpp:265-272`, `:338-351`;
   `src/blocking_file.cpp:241-265`. Basis: ERR-02 ("report effects as unknown,
   not zero"), SEM-05 table row 5.

### 4.2 KERNEL_FLOOR

4. `call pwrite` (`0x1420`) / `call write` (`0x1b43`) — one syscall per step;
   `include/sluice/detail/posix_retry.hpp:5-12` wraps exactly this call. The
   A1 arm's differential against a raw `::pwrite` loop is the analogue of
   #600's A1−A0 (syscall-entry floor) and is not Sluice excess.

### 4.3 DUPLICATE_WORK

5. Per-step admission re-run — `write_all_at` loop `0x13c0-0x13ff`: fd reload +
   closed re-test (3), access re-check (2), step offset sign re-test (3),
   step last-byte range re-check (5 incl. `movabs` re-materialization):
   **12 instructions/step re-proving facts the outer admission already
   established**. `write_all` loop `0x1b10-0x1b2d`: closed (3) + access (2) +
   remaining sign re-test (2) = 7/step. Source: the step lambda calls the
   *public* `write_at`/`write` (`src/blocking_file.cpp:313-315`, `:340-341`),
   each re-running `precheck_data_op`. The compose comment itself states the
   invariant that makes the re-checks redundant (`src/blocking_file.cpp:267-268`:
   "confirmed never exceeds the validated request, so an advanced offset stays
   inside the range the precheck accepted"); the File cannot close or change
   access mid-call (caller serialization is a SEM-03 caller precondition;
   THREAD-01). Not contract work: the contract requires the composition to
   *behave* as safe repeated primitive use (SEM-01), not to re-execute the
   public validation per step. On the 1-step path this duplicate share is 12
   of the +29 excess; on multi-step it is 12/44 (write_all_at) and 7/34
   (write_all).

### 4.4 REPRESENTATION_OVERHEAD

6. Per-iteration `CompositionState` → outcome staging: `write_all_at`
   `0x1650-0x167f` / `write_all` `0x1d50-0x1d7f` — 10 instructions executed
   every loop iteration (6 scalar stores + 16B `movdqa` load + 16B store +
   loop condition) although only the final iteration's state is observable.
   The A1 arm's per-iteration analogue is 3 instructions (zero-template
   refresh) — differential +7/step. Source: `compose`'s state round-trip
   (`src/blocking_file.cpp:272-282`) plus `direct_outcome` reading
   `CompositionState` fields (`:241-265`); SRA does not scalarize because all
   stop edges merge at the shared publish tail — the same round-trip-through-
   memory pattern #600 recorded as the dominant non-elided cost for `read_at`
   (`DIRECT_MACHINE_COST_LEDGER.md`, "NOT elided (dominant)").
7. Loop-carried `total` in a caller-saved register: `write_all_at` keeps
   `total` in `%r8` and pays `mov %r8,0x10(%rsp)` (`0x141b`) + `mov
   0x10(%rsp),%r8` (`0x1425`) around **every** `pwrite`; `write_all` keeps it
   in `%rcx` with `0x1b3e`/`0x1b48`. +2 memory ops/step vs the A1 arm, which
   holds `total` in callee-saved `%r12`/`%r15` with no per-step traffic. A
   compiler consequence of the composition's live-value set (6 live values
   exhaust the callee-saved pool), so partially ABI_OR_COMPILER_COST — but the
   A1 arm demonstrates the same semantics fit without the traffic.
8. EINTR-feeder spill on the success path: `mov %rax,0x18(%rsp)` (`0x140d`)
   stores `buf+confirmed` for the retry path although the common case never
   retries. 1 store/step.

### 4.5 COMPOSITION_OVERHEAD

9. The composition boundary as a whole: stepping through the public primitive
   re-derives remaining/advanced-offset/buffer (`0x13cb-0x13d2`, `0x1b24-0x1b33`
   — necessary arithmetic, ≈3 insns) and merges two validation frames. Net
   composition machinery beyond the standalone primitive: `write_all_at`
   1-step 101 = `write_at` 40 + 61 (the write-path analogue of #600's AX−A2
   45–60/step and of the unexplained +9.7 ns C2 residual, P0 §3.3/§4.7 — for
   writes the residual is *structurally accounted for* by items 5–8 above).
   The +29/1-step and +17/step vs A1 is the beyond-parity share.

### 4.6 ABI_OR_COMPILER_COST

10. `Result<CompositionOutcome>` sret scaffolding: 16B `movups` head + errno
    qword + `has_value` byte stores (`0x1521-0x1537`, `0x1c28-0x1c47`); errno
    store→reload round trip on the error path; 6 callee-saved spill/restore
    pairs per call. The comparator emits the same shapes (its publish tails
    carry the identical `movups`/errno/`has_value` stores; D3 verdict
    `NO_CHANGE_JUSTIFIED` carries over — necessary public-interface cost, not
    Sluice-specific excess).
11. GCC fusions already elided: `offset > kMax` and `length > kMaxNativeTransfer`
    collapse into one `or`+`js`; the three source range conditions collapse to
    two compares + one sign test (both baselines) — COMPILER_ELIDED, no
    residual sign-test duplicate remains on the *outer* admission (unlike
    #600's D1 finding on B0, C1 removed it; the loop's re-test at `0x13df` is
    item 5, not D1).

### 4.7 TRANSFORMABLE_WORK

12. Items 5–8 as a package are transformable: a composed-step write primitive
    that admits once and then loops on raw `pwrite` (the write-side analogue
    of #600's reverted C2, whose read-side machine effect was "compose loop
    reduced to `pread`/`__errno_location` calls; the out-of-line precheck
    clone disappeared" — P0 §3.3) would remove the 12/step re-admission, the
    per-iteration staging (build outcome only at exit, A1 shape), and the
    caller-saved spill traffic: static −17/step upper bound (`write_all_at`),
    −11/step (`write_all`), plus −12…−17 of the single-step path's +29.
    Whether that transforms into measurable time is exactly what the C2 read
    experiment could not show (admission rule "only on repeatedly measurable
    gain") — the write path must re-earn it with its own ladder.

### 4.8 UNATTRIBUTED

13. Dead 16-byte store per iteration: `write_all_at` `movaps %xmm0,0x20(%rsp)`
    (`0x1677`) and `write_all` `movaps %xmm0,0x10(%rsp)` (`0x1d77`) — written
    every iteration, read nowhere in the function (single textual occurrence
    of each displacement; verified by grep over the disassembly). No semantic
    obligation, no ABI need; compiler artifact of the staging pattern.
    Magnitude 1 store16/iteration — almost certainly below the ≈2.5–3 ns MDE,
    but it is pure excess and likely disappears with item 6.

## 5. Verdict per operation

- **`write_all_at`: gap (initial).** Syscall parity reached (1 pwrite/step, 0
  on zero-length; EINTR shape correct); admission and progress law are at the
  matched-native floor; but the composition loop carries +29 static
  instructions on the single-pwrite success path and +17 per additional step
  vs the A1-pattern comparator, of which 12/step are provably duplicate
  re-validation and ~9/step representation round-trips. PARITY_REACHED would
  require the A1 differential to be zero-or-noise; it is not, statically.
- **`write_all`: gap (initial).** Same mechanisms, smaller admission: +22
  single-write success path, +11 per additional step (7 duplicate re-validation
  + ~4 representation).

Time-scale estimate `[UNCERTAIN]`: scaling by the #600 caliber (≈16.6 ns per
≈28 excess instructions ≈ 0.6 ns/insn on the syscall path), the single-step
excess maps to roughly 13–18 ns/op and the per-step excess to ≈7–11 ns/step
(`write_all_at`), ≈10–13 ns/op and ≈4–7 ns/step (`write_all`) — **estimates
only**: no write-path ladder measurement exists (P0 §4.13), the minimum
detectable effect at 16 ABBA pairs is ≈2.5–3 ns, and the C2 read precedent
shows static removal of this size can fail the measurability admission rule.

## 6. Gap candidates

### G-WALLAT-01 — per-step admission re-run in `write_all_at` compose loop
- Source: `src/blocking_file.cpp:313-315` (step lambda → public `write_at`),
  `src/blocking_file.cpp:99-106`; machine `0x13c0-0x13ff`.
- Trigger: any multi-step `write_all_at` (short writes — e.g. interrupted
  streams, very large transfers split by the kernel, or scripted short
  counts); on the 1-step path the same duplicate block runs once.
- Necessary work: one admission per public call (SEM-03), one `pwrite` per
  step, progress bookkeeping (SEM-05).
- Excess work: 12 instructions/step re-validating closed/access/range facts
  already proven by the outer admission and the compose invariant
  (`src/blocking_file.cpp:267-268`).
- Magnitude: 12/44 instructions per step (static, exact); ≈5–8 ns/step scaled
  `[UNCERTAIN]`; 12 of the +29 1-step excess.
- Mechanism: write-side composed-step primitive (C2 analogue): validate once,
  loop on raw `pwrite`; public `write_at` unchanged.
- Falsifier: paired ABBA write ladder (AW2 = sluice `write_all_at`, AW1 =
  matched-native arm, 16 pairs, E5, tmpfs, sizes 1 B/64 B/512 B/4 KiB/64 KiB,
  preregistered 15-pair rule per P0 §4.11): if AW2−AW1 ≤ MDE at every size,
  the gap downgrades to representation-level (unmeasurable). A prototype patch
  must show ≥12-insn static drop with GREEN→RED→GREEN on the §7 mutant set.
- Confidence: high (static count), medium (ns magnitude).

### G-WALLAT-02 — per-iteration state staging and loop-carried spill traffic
- Source: `src/blocking_file.cpp:272-282` (compose loop + `CompositionState`),
  `:241-265` (`direct_outcome`); machine `0x1650-0x167f`, `0x141b/0x1425`,
  `0x140d`.
- Trigger: every `write_all_at` call (staging runs per iteration; spills run
  per step); worst on multi-step shapes.
- Necessary work: publish one final `CompositionOutcome` with confirmed bytes,
  end, certainty, error (ERR-02).
- Excess work: rebuilding the outcome staging every iteration (+7 vs A1) and
  keeping `total` in a caller-saved register (+2 memory ops/step) plus the
  EINTR-feeder store (+1/step).
- Magnitude: ~10 instructions/step static; ≈4–7 ns/step scaled `[UNCERTAIN]`;
  remainder of the +29 1-step excess after G-WALLAT-01.
- Mechanism: hold confirmed/total in callee-saved registers; build the outcome
  only on exit (A1 arm demonstrates the shape).
- Falsifier: same ladder; additionally a source variant that stages only at
  exit must show the static drop and no semantic drift on the §7 oracles.
- Confidence: medium-high.

### G-WALLAT-03 — dead per-iteration 16-byte store
- Source: compiler artifact over `src/blocking_file.cpp:272-282`; machine
  `0x1677` (`write_all_at`), `0x1d77` (`write_all`).
- Trigger: every loop iteration of either composition.
- Necessary work: none (the slot is never read).
- Excess work: one 16-byte store per iteration.
- Magnitude: 1 store16/iteration — sub-MDE almost surely; tracked because it
  is pure excess and a canary for the staging pattern.
- Mechanism: none direct; expected to vanish with G-WALLAT-02's exit-only
  staging.
- Falsifier: disappears from the disassembly when staging leaves the loop;
  cannot be independently timed (below MDE).
- Confidence: high (existence), low (independent measurability).

### G-WALL-01 / G-WALL-02 / G-WALL-03 — `write_all` (shared cursor) analogues
- Source: `src/blocking_file.cpp:340-341` (step lambda → `write`),
  `:138-145`; machine `0x1b10-0x1b2d`, `0x1d50-0x1d7f`, `0x1b3e/0x1b48`,
  dead store `0x1d77`.
- Trigger / necessary / mechanism / falsifier: identical to G-WALLAT-01/02/03
  with the cursor form's smaller numbers: 7 duplicate re-validation
  instructions/step, +22 1-step and +11/step total excess.
- Confidence: high (static), medium (ns magnitude).

## 7. Mutation seams: current state for this operation group

- Compile-time mutant seams touching the audited path at B1 are exactly the
  three C1 admission mutants: `SLUICE_SEM_MUTANT_EMPTY_BEFORE_CLOSED` and
  `SLUICE_SEM_MUTANT_ADMIT_INVALID_RANGE` in `precheck_data_op`, and
  `SLUICE_SEM_MUTANT_ADMIT_UNREPRESENTABLE_SIZE` in `precheck_resize`
  (`include/sluice/detail/file_semantics.hpp:158,168,192`), with xmake targets
  via `direct_mutant_target()` (`xmake/tests.lua:93-99`). Verified by
  repository-wide grep this session.
- **No compile-time mutant seam exists for the composition machinery** —
  nothing covers `compose_progress` (zero-progress stop, confirmed advance,
  impossible count), `compose_error`, `composition_effect_certainty`
  (`accounted` vs `unknown`), or `direct_outcome`. The request side has
  `SLUICE_BACKEND_CONTRACT_MUTANT_WRITE_FAILURE_ACCOUNTED`
  (`src/async/threadpool_backend.cpp:505`) for write-failure effect
  certainty, but the direct path has no analogue.
- Behavioral oracles (script injection, `SLUICE_FILE_INTERNAL_TESTING`,
  `src/file_test_seams.hpp` — compiled out of the shipped archive: its
  disassembly contains no `file_testing` symbols, calls go straight to
  `pwrite`/`write` PLT): `short_writes_reach_full_progress`
  (`tests/direct_composition_fault_test.cpp:110-130`, asserts buffer
  pointer/`count`/`offset` of the advanced step),
  `zero_progress_write_stops_after_one_attempt` (`:132-150`, `calls()==1`),
  `error_after_confirmed_prefix_keeps_the_prefix` (`:197-222`, prefix 256,
  `unknown`, `no_space`+`ENOSPC`),
  `primitive_write_retries_eintr_then_stops_at_the_first_count`
  (`:277-297`), `zero_length_requests_make_no_native_call` (`:305-338`,
  `write_all_at`/`write_all` included), plus the read/write-composition
  equivalents in `tests/direct_composition_test.cpp` named in P0 §5.1.
- Consequence for any P2 candidate that restructures the compose loop (e.g. a
  write-side C2): it must add named mutants with killer assertions before the
  change — candidate seams and killers: (a) "compose advances by requested
  instead of confirmed" → killed by `short_writes_reach_full_progress` /
  `composition_advances_the_buffer_by_confirmed_bytes`; (b) "compose retries
  after zero progress" → killed by `zero_progress_write_stops_after_one_attempt`
  / `composition_never_spins_on_zero_progress`; (c) "failure reports
  `accounted`" → killed by `a_primitive_error_reports_an_unaccounted_remainder_in_both_directions`
  / `error_after_confirmed_prefix_keeps_the_prefix`; (d) "compose skips the
  impossible-count guard" → killed by `impossible_count_stops_immediately`
  (`tests/direct_composition_fault_test.cpp:340-363`, read-side; a write-side
  scripted-overcount variant does not exist yet and should be added with the
  mutant). `composition_advances_the_buffer_by_confirmed_bytes` lives at
  `tests/direct_composition_fault_test.cpp:155`;
  `write_all_zero_progress_stops_and_keeps_its_prefix` and
  `composition_never_spins_on_zero_progress` at
  `tests/semantic_short_io_reference_test.cpp:78,90`.

## 8. Limitations and [UNCERTAIN] items

1. No timing measurement was run (static audit only); every ns figure is a
   scaled estimate from the #600 caliber and must be replaced by a paired
   write-ladder before any admission decision.
2. The A1 write arms are new comparator code written this session (the
   preserved `ladder.cpp` covers reads only); semantic equivalence is by
   construction and code inspection, not by an executed differential. The
   compile provenance is fully recorded (§ header) but the arms are
   uncommitted working copies under `/tmp` — committing a rebuildable
   comparator (with the write ladder) is a P1 measurement-phase obligation
   (P0 §4.8: new comparator arms need a committed, rebuildable identity).
3. Per-step instruction attribution (which exact instructions of the merged
   loop correspond to which source construct) is by address-level reading of
   the disassembly against the source; the block boundaries are unambiguous
   (relocated call sites, unique displacements), but per-instruction source
   mapping inside fused check sequences is interpretive.
4. The `0x20(%rsp)`/`0x10(%rsp)` dead-store claim is verified within each
   function's own disassembly (single textual occurrence, no read); aliasing
   through a different base register within the same frame does not occur
   (frame accessed only via `%rsp` in these functions).
5. strace syscall/op for the write compositions was not re-run; the 1-call-
   per-step / 0-on-zero-length claim rests on the disassembly control flow
   and the existing fault-injection oracle (`script.calls() == 0`).

## Appendix A — B1 success-path address sequences used for counting

- `write_all_at` 1-step: `0x1330-0x13bd`, `0x13c0-0x142d`, `0x1600-0x160f`,
  `0x1650-0x167f`, `0x1685-0x16a1`, `0x14d3-0x1543` (= 101).
- `write_all_at` extra step: `0x13c0-0x142d`, `0x1600-0x1614`,
  `0x1650-0x167f` (= 44).
- `write_all` 1-step: `0x1ab0-0x1b0d`, `0x1b10-0x1b50`, `0x1d00-0x1d0f`,
  `0x1d50-0x1d7f`, `0x1d85-0x1da1`, `0x1bf3-0x1c57` (= 82).
- `write_all` extra step: `0x1b10-0x1b50`, `0x1d00-0x1d14`,
  `0x1d50-0x1d7f` (= 34).
- A1 arms: explicit address lists in `/tmp/p1-audit-combined-writes/`
  (counting script `count_paths.py`, corrected run; every address verified
  present in the disassembly).

## Appendix B — Zero-progress / advancement machine shapes (asked explicitly)

- Zero-progress stop (`write_all_at`): `0x1611 test %rax,%rax; 0x1614 jne
  0x1650` — fall-through (n == 0, confirmed < total) sets `eax = 2`
  (`0x1616`), stages `{confirmed, end=2, remaining=0/accounted, no error}`
  (`0x1620-0x1647`) and publishes via the shared tail; loop does not
  re-execute the step. `write_all`: `0x1d11/0x1d14`, `eax=2` at `0x1d16`,
  staging `0x1d20-0x1d47`.
- Confirmed-bytes advance: `0x1609 add %rax,%rbx` (`write_all_at`),
  `0x1d09 add %rax,%r12` (`write_all`), guarded by the impossible-count
  compare `0x1600-0x1603` / `0x1d00-0x1d03` (n > remaining stops *without*
  absorbing n — publish sets code 6 = `invalid_state`, `remaining=unknown`,
  confirmed unchanged; machine `0x16b0→0x16f0` path).
- EINTR (both ops): `call __errno_location` → `cmp $0x4` → reload fd (+buf for
  `write_all_at`) → jump back to the arg-setup immediately before the native
  call; no admission re-run, no confirmed advance, identical args
  (`0x1433-0x1442`+`0x16e0-0x16e8`; `0x1b56-0x1b65`+`0x1de0-0x1de2`).

## Appendix C — Session working-copy provenance

`/tmp/p1-audit-combined-writes/`: `a1_writes.cpp` (comparator source),
`a1_writes.o` + `a1_writes.asm` (compiled/disassembled this session),
`blocking_file.cpp.o` (extracted from the frozen B1 archive; archive member
list verified: exactly `file_resource.cpp.o`, `blocking_file.cpp.o`),
`write_all_at.asm`, `write_all.asm`, `count_paths.py` (counting script).
None of these are committed; the audit document you are reading is the only
repository artifact written by this audit, per the P0 ownership rules.
