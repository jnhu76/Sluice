# P1 Static Cost Audit — Composition-Read Group (`read_exact_at`, `read_exact`)

- Group: 审计员·组合读 (composition-read). Date: 2026-10-27.
- Audited baseline: **B1** = frozen worktree `/home/jnhu/Source/Sluice-600-wt`, HEAD
  `77e5399a45998dea396699fa2e063c94a665945a`, archive
  `build/linux/x86_64/release/libsluice_core.a` sha256
  `d1e351a54ded3dfa1df6b1cd2e47622eefbd5d7027439cb31a314bed3a3007f0`
  (re-verified this session: `git -C /home/jnhu/Source/Sluice-600-wt rev-parse HEAD` →
  `77e5399a…`; `sha256sum …/libsluice_core.a` → `d1e351a5…`). The worktree was used
  read-only: no reconfigure, no rebuild; all disassembly is `objdump -dr` on the frozen
  archive, all comparator compilation happened in `/tmp/p1-audit-readat/`.
- Inputs read before auditing (per the ask): `P0_BASELINE_AND_OWNERSHIP.md` §1 (B0/B1/N0/N1
  identities, ownership rules — B1 anchor linked read-only), §3 (#600 conclusions cited,
  not re-derived), §5 (semantic/oracle floor); #600
  `DIRECT_MACHINE_COST_LEDGER.md` (`git show
  sluice-600-direct-research:research/600-direct-excess-cost/DIRECT_MACHINE_COST_LEDGER.md`);
  N1 A1 arm snapshot `research/direct-beyond-parity/probe-snapshot-600/ladder.cpp:42-63`;
  v2-r1 `docs/explicit-io-contract.md` (SEM-03 §, SEM-04, SEM-05, ERR-01, ERR-02, W-01,
  THREAD-01; read at `docs/explicit-io-contract.md:398-480,536-590,225-232` on the master
  working tree).
- Counting convention: "success path" = executed instructions for one **legal, nonempty,
  valid** call whose first primitive step returns a full count (the ladder AX shape),
  including every conditional branch on that path, the syscall `call`, and the `ret`.
  The same convention is applied to the comparator arms, compiled with the archive's own
  flags (`-m64 -fvisibility=hidden -O3 -std=c++20 -DNDEBUG`, GCC 16.2.1, `CCACHE_DISABLE=1`;
  flags per P0 §1.2/§1.5). Branch/load/store counts are per that path.

## 0. Comparator construction (machine-evidence provenance)

The #600 ladder has a matched-native arm (A1) only for `read_at`; **no matched-native
composition arm exists in the frozen #600 evidence** (P0 §1.4, §4.13). For a static parity
judgment of the two composition operations I therefore built N1-style comparators myself:

- `p1_a1_read_at` — verbatim copy of the A1 arm (`ladder.cpp:42-63`), to re-baseline the
  counting convention against #600's quoted numbers.
- `p1_n1_read_exact_at`, `p1_n1_read_exact` — hand-written minimal compositions with the
  same public contract on the legal domain (same check order and error codes, EINTR retry
  without count, EOF-before-full keeps prefix with `end=eof_before_full`/
  `remaining=accounted`, primitive error keeps prefix with `end=primitive_error`/
  `remaining=unknown` + mapped error, full → `complete`), same `Result<CompositionOutcome>`
  ABI, public Sluice headers only, independent TU boundary — the N1 rules of P0 §1.4.

Source: `/tmp/p1-audit-readat/n1_matched.cpp` (research probe; kept out of the repository
per AGENTS one-shot-probe rule). Build command (run this session):

```
CCACHE_DISABLE=1 /usr/lib64/ccache/gcc -c -m64 -fvisibility=hidden -O3 -std=c++20 -DNDEBUG \
  -I/home/jnhu/Source/Sluice-600-wt/include n1_matched.cpp -o n1_matched.o
```

These comparators are **P1-constructed, not #600-frozen**: they anchor static parity only.
The `pread`/`read`/`__errno_location` symbols are reached through `R_X86_64_PLT32`
relocations, exactly as in the archive (`blocking_file.cpp.o` relocs at 0x1021, 0x1034,
0x17c4, 0x17d7 etc.), so both sides pay the same PLT call shape.

Convention cross-check: A1 verbatim compiles to **40** success-path instructions under my
exact-path convention (#600 quotes "≈42" for the same arm; both are consistent with B1
`read_at` = 39 — the small difference is counting convention, not code). **B1 `read_at` is
at static parity with the matched native (39 ≤ 40), corroborating #600's post-C1 "70 → 40"
and the D3 `NO_CHANGE_JUSTIFIED` verdict.** The composition numbers below use the same
convention on both sides.

## 1. `read_exact_at`

### 1.1 Path chain (Public API → semantic validation → internal representation → native → Result)

| Stage | Source | Machine site (frozen archive, `blocking_file.cpp.o`, symbol at 0x0f30) |
|---|---|---|
| Public API | `include/sluice/blocking/file.hpp:51-52` `read_exact_at(const File&, uint64_t, span<byte>) -> Result<CompositionOutcome>` | sret `%rdi` saved to `%r12` (0xf43); frame `sub $0x98` (0xf30) |
| Semantic validation (admission) | `src/blocking_file.cpp:286-296` → `admit_data_op` (`src/blocking_file.cpp:25-29`) → `precheck_data_op` (`include/sluice/detail/file_semantics.hpp:157-176`): closed → `reject_closed`(`invalid_state`), access → `reject_access`(`invalid_argument`), len 0 → `complete_empty`, range/transfer → `reject_range`; then explicit null-data check `src/blocking_file.cpp:294-296` | 0xf3c-0xfa0: fd load `(%rsi)`, `test/js` (closed), `cmpb $1,0x4(%rsi)/je` (write_only), `test %r8/je` (len 0), `movabs/lea/sub/cmp/jb` range, `or/js` transfer, `cmpq $0,0x8(%rsp)/je` null-data |
| Internal representation | `compose<Primitive>` (`src/blocking_file.cpp:269-283`) carrying `CompositionState` (`include/sluice/detail/file_semantics.hpp:232-240`) with `compose_progress` (`:243-263`) and `compose_error` (`:265-272`); the step primitive is the public `read_at` (`src/blocking_file.cpp:297-299`) inlined per step | step loop 0xfc0-0x102d (inlined `read_at` admission + syscall); progress merge 0x1200-0x1214; state materialization block 0x1250-0x127f; exit conversion `direct_outcome` (`src/blocking_file.cpp:241-265`) inlined at 0x10d3-0x1132 |
| Native operation | `native_pread` → `::pread` (`src/blocking_file.cpp:51-58`) via `retry_on_eintr` (`include/sluice/detail/posix_retry.hpp:7-15`) | `call pread@plt` (0x1020, reloc `R_X86_64_PLT32 pread-0x4`); EINTR retry 0x1033-0x1042 → 0x12e0 (reload fd + advanced buf, `jmp 0x1015`) |
| Result | `Result<CompositionOutcome>` built from `composition_effect_certainty` (`file_semantics.hpp:338-351`) + stop mapping (`src/blocking_file.cpp:245-263`); `Result` storage `include/sluice/result.hpp:19-90`; `CompositionOutcome` `include/sluice/blocking/file.hpp:42-49` | sret stores 0x1113/0x1121/0x1129/0x1132 (`has_value`, optional error, errno dead-store, `movups` 16-B value) |

Stop-condition chain (SEM-05): step error → `compose_error` → `end=primitive_error`,
`remaining=unknown`, mapped error (0x1033 errno switch → 0x10a0 block → epilogue with
`ecx=3`, `edi=1`); step count 0 with confirmed<total → EOF (`0x1211/0x1216/0x1232-0x1247`,
`ecx=1`, optional zeroed); `n > remaining` → `impossible_count` guard (0x1200-0x1203 →
0x12b0, `ecx=3`, `edx=6` `invalid_state`); confirmed==total → `complete` (0x1285-0x12a1,
`ecx=0`). Every branch of the mapping is present in the machine code.

### 1.2 Machine counts (success path, single fully-satisfiable step)

| Metric | B1 `read_exact_at` | N1-matched `p1_n1_read_exact_at` | Excess |
|---|---|---|---|
| Instructions (prologue / step / exit) | **100** (29 / 27 / 44) | **63** (29 / 17 / 17) | **+37** |
| Conditional branches | 14 (+1 `jmp`) | 9 (+2 `jmp`) | +5 |
| Calls | 1 (`pread@plt`) | 1 (`pread@plt`) | 0 |
| Memory loads | 13 | 12 | +1 |
| Memory stores | **27, of which 8 dead** (see §1.3) | 14 | +13 |
| Stack frame | 152 B (`sub $0x98`) | 120 B | +32 B |
| Syscalls per step | 1 `pread64` | 1 `pread64` | 0 |

Loop-back cost per additional short-progress step: **B1 44 vs N1 19 → +25 instructions per
step** (dominated by per-step re-admission ≈13, dead state block 10, impossible-count
guard 2, per-call `total` spill/reload 3). Prologue (outer admission) is **at parity:
29 vs 29** — all of the excess lives in the loop machinery and the exit materialization.

### 1.3 Dead-work certification (reader/writer enumeration, not speculation)

Greps over the full function listing (`objdump -dr` output in `/tmp/p1-audit-readat/rea.txt`):

- `0x20(%rsp)` (the 16-byte `movaps %xmm0,0x20(%rsp)` at 0x1277) is **written once and
  never read anywhere in the function** — dead store every step.
- The per-step state block 0x1250-0x127f writes `0x40/0x48/0x4c/0x50`
  (`confirmed/stopped-stop/error fields`) and loads it back with `movdqa` (0x1261); on
  **every** exit path the shared epilogue 0x10d3-0x1109 re-stores those exact slots from
  registers before the only subsequent read (`movdqa 0x40(%rsp)` at 0x10f7). The block is
  executed once per step and is dead on all of them: on loop-continuation nothing reads
  the slots (the loop carries `confirmed` in `%rbx`), and on exit the values are
  overwritten first.
- Exit-path dead zero-stores: 0x1285 (`movq $0,0x4a`) and 0x1294 (`movq $0,0x50`) are
  followed by 0x1109/0x10fd writes of the same bytes.

This is the same "CompositionState stack materialization" class #600 identified pre-C1
(`DIRECT_MACHINE_COST_LEDGER.md`, "NOT elided (dominant)"), now localized to the compose
loop: C1 removed the `Precheck` round-trip but the `CompositionState` round-trip survives
in B1.

### 1.4 Error path and EINTR shape

- Error: `pread` < 0 → `call __errno_location@plt` (0x1033), errno load, compare chain
  0x103f-0x108a (EINTR 4 / EACCES 0xd / EPERM 1 / ENOENT 2 / ENOSPC 0x14 / EDQUOT 0x1c /
  EAGAIN 0x7a / EWOULDBLOCK 0xb, default `backend_error`=7 via `setne/lea` arithmetic),
  then error-outcome materialization 0x10a0 block + shared epilogue (`end=3`,
  `remaining=unknown`, `error={code,os_errno}` with optional engaged). Shape is
  value-equal to the N1 arm's error block (0x340-0x3e2 there); the delta is again the
  intermediate state materialization, not the mapping. Mapping is ERR-01-required.
- EINTR: errno==4 → `je 0x12e0` → reload fd `(%rbp)` + advanced buffer `0x18(%rsp)` →
  `jmp 0x1015` re-issue with the same offset/remaining. Retry happens only without a
  completed count (the count path never re-enters), satisfying SEM-05/ERR-01; the
  `primitive_retries_eintr_without_a_completed_count` fault oracle (P0 §5.1) pins it.

### 1.5 Classification (8 classes)

| # | Class | Where | Machine evidence | Semantic basis (v2-r1) |
|---|---|---|---|---|
| 1 | SEMANTICALLY_REQUIRED | outer admission `src/blocking_file.cpp:288-296` → `file_semantics.hpp:157-176` | prologue 0xf30-0xfbb: 29 instrs, 6 branches, 3 loads; N1 control has the identical 29-instr prologue | SEM-03 steps 1-4 precedence (closed → access → zero-no-op → range), SEM-04 range/transfer validation, V01 closed-first |
| 2 | KERNEL_FLOOR | `native_pread` `src/blocking_file.cpp:51-58`; call site 0x1020 | 1 `pread` per step; syscall parity 1.000/op with N1/A0 (#600 strace, ledger) | SEM-05 primitive performs the transfer; no composition may reduce syscall count for a full request |
| 3 | SEMANTICALLY_REQUIRED | EINTR retry `posix_retry.hpp:7-15`; machine 0x1033-0x1042 → 0x12e0 | errno==4 branch → fd/buf reload → single re-issue `jmp 0x1015`; no replay of counted bytes | SEM-05 "retry EINTR only without a completed count"; ERR-01 EINTR→interrupted |
| 4 | SEMANTICALLY_REQUIRED | stop-condition evaluation `compose_progress`/`compose_error` `file_semantics.hpp:243-272`, machine 0x1200-0x1214 + errno switch | EOF/error/short/full classification branches; observables identical to N1 arm | SEM-05 stop set (full/eof/error), ERR-02 confirmed-prefix + effect certainty |
| 5 | ABI_OR_COMPILER_COST | sret Result stores 0x1113-0x1132; pread-arg spills 0x100d/0x101b/0x1025; 6 callee-save spills 0xf3e-0xfb6; errno dead-store 0x1129 | same store/sret shape class emitted for A1 and both N1 arms (this session's builds); #600 D3 `NO_CHANGE_JUSTIFIED` | Result/CompositionOutcome are the public payload (W-01 direct form); not Sluice-specific excess |
| 6 | DUPLICATE_WORK | per-step re-admission: inlined `read_at` checks 0xfc0-0xfff (`src/blocking_file.cpp:78-97` inside the compose lambda `:297-299`) | ≈13 instrs/step (fd reload, closed test+branch, access load+cmp+branch, or/js transfer re-test, movabs/lea/sub/cmp/jb range recompute) absent from the N1 arm; +2 memory loads/step | SEM-03 precedence is established once per public call; under THREAD-01 the caller serializes move/close/destroy, so file state cannot legally change mid-call; the arithmetic invariant is documented at `src/blocking_file.cpp:267-268` |
| 7 | REPRESENTATION_OVERHEAD | `CompositionState` materialization block 0x1250-0x127f (`compose_progress` by-value round-trip, `file_semantics.hpp:232-263`) | 8 instrs / 7 memory ops per step (incl. `movdqa`+`movaps` SSE pair), certified dead (§1.3); +1 dead SSE copy slot `0x20(%rsp)`; N1 arm has none | SEM-05 requires the observable outcome fields, not the intermediate state object; `CompositionState` is internal representation |
| 8 | TRANSFORMABLE_WORK | exit double materialization 0x10d3-0x1132 (`direct_outcome` `src/blocking_file.cpp:241-265`) + dead zero-stores 0x1285/0x1294 | required 33-B sret payload written via an intermediate stack object: ~12 memory ops vs N1's 5 direct sret ops (0x303-0x320); +2 dead zero-stores | The outcome itself is contract-required (ERR-02/SEM-05 reporting); only the two-step construction form is removable |
| 9 | UNATTRIBUTED | impossible-count guard 0x1200-0x1203 (`compose_progress` `file_semantics.hpp:249-252`) | 2 instrs/step (`cmp/jb` → `CompositionStop::impossible_count` → `invalid_state` 0x12b0) absent from the N1 arm | no v2-r1 requirement IDs it; behavior is oracle-pinned by `impossible_count_stops_immediately` (`tests/direct_composition_fault_test.cpp:340`, registered `:666`), so removal is an oracle-visible change, keep-or-remove is an owner decision under the mechanism-justification gate |

### 1.6 Parity verdict

**Gap (initial judgment).** +37 static instructions on the single fully-satisfiable step
(100 vs 63), +25 per additional short step; outer admission itself is at parity. The
excess is the surviving carrier candidate for #600's unexplained ≈+9.7–9.9 ns AX−A2
post-C1 residual (P0 §3.2 arithmetic, §3.3, §4.7). See gaps G-REA-01/02/03.

## 2. `read_exact`

### 2.1 Path chain

| Stage | Source | Machine site (symbol at 0x1730) |
|---|---|---|
| Public API | `include/sluice/blocking/file.hpp:57` | sret `%rdi`→`%rbp` (0x173c); frame `sub $0x88` (0x1730) |
| Semantic validation | `src/blocking_file.cpp:318-326` → same `admit_data_op`/`precheck_data_op` chain with offset ≡ 0 | 0x173f-0x1775: closed, write_only, len-0, then GCC's elided range/transfer check — the whole offset-0 range test collapses to a sign-flag reuse `test %rcx,%rcx; je (zero); jle 0x1910` (0x175b-0x1769, flags from the zero test), then null-data `test %rdx/je` (0x1772) |
| Internal representation | `compose` + `CompositionState` as above; step primitive = public `read` (`src/blocking_file.cpp:327-328`) inlined | step loop 0x1790-0x17d0; progress 0x1980-0x1994; dead state block 0x19d0-0x19ff; `direct_outcome` at 0x1873-0x18c7 |
| Native operation | `native_read` → `::read` (`src/blocking_file.cpp:43-49`) via `retry_on_eintr` | `call read@plt` (0x17c3, reloc `read-0x4`); EINTR 0x17d6-0x17e5 → 0x1a60 (fd reload, `jmp 0x17b8` re-issue) |
| Result | same `Result<CompositionOutcome>` construction | sret stores 0x18ad-0x18c7 (`movups` + optional + errno dead-store + `has_value`) |

### 2.2 Machine counts

| Metric | B1 `read_exact` | N1-matched `p1_n1_read_exact` | Excess |
|---|---|---|---|
| Instructions (prologue / step / exit) | **82** (21 / 17 / 44) | **51** (21 / 13 / 17) | **+31** |
| Conditional branches | 12 (+1 `jmp`) | 8 (+2 `jmp`) | +4 |
| Calls | 1 (`read@plt`) | 1 (`read@plt`) | 0 |
| Memory loads | 11 | 10 | +1 |
| Memory stores | **25, of which 8 dead** | 11 | +14 |
| Stack frame | 136 B (`sub $0x88`) | 104 B | +32 B |

Loop-back per additional short step: **B1 34 vs N1 15 → +19/step**. Prologue parity again
(21 vs 21); the offset-0 admission is cheaper on both sides (compiler elision), so the
composition machinery is the whole delta. Dead-work certification identical to §1.3
(`0x10(%rsp)` copy at 0x19f7 never read; state slots re-stored at 0x1873-0x18a3 before
the only reader `movdqa 0x30` at 0x1894; dead zero-stores 0x1a05/0x1a14). EINTR and error
shapes as in §1.4 with addresses 0x17d6/0x1a60 and the errno chain 0x17e2-0x182d.

### 2.3 Classification

Same nine-item structure as §1.5, with this operation's sites: outer admission
SEMANTICALLY_REQUIRED (0x1730-0x178d, incl. the compiler-elided sign-flag range test);
`read@plt` KERNEL_FLOOR; EINTR/stop machinery SEMANTICALLY_REQUIRED; sret/spill/errno-dead-store
ABI_OR_COMPILER_COST; per-step re-admission DUPLICATE_WORK (0x1790-0x17ad: fd reload +
closed + access + remaining-sign ≈ 9 instrs/step); dead state block
REPRESENTATION_OVERHEAD (0x19d0-0x19ff, 8 instrs/7 memory ops); double exit
materialization + dead zero-stores TRANSFORMABLE_WORK (0x1873-0x18c7 vs N1 0x4f8-0x52a);
impossible-count guard UNATTRIBUTED (0x1980-0x1983, same oracle pinning).

### 2.4 Parity verdict

**Gap (initial judgment).** +31 static instructions single-step (82 vs 51), +19/step;
no #600 ladder arm ever measured `read_exact` (AX covered `read_exact_at` only — P0
§4.13), so its dynamic residual is [UNCERTAIN] by construction; statically it carries the
same mechanisms. See gaps G-REX-01/02/03.

## 3. Cross-checks against #600 and C2

- B1 `read_at` (inlined inside both audited ops) counts **39** vs A1's 40 under my
  convention — `read_at` PARITY_REACHED, consistent with #600 "70 → 40" and D3.
- The #600 C2 prototype (`~/sluice-600-direct-research/evidence/c1c2_full_worktree.patch`,
  read this session, hunk at patch lines 200-290) introduced `prevalidated_pread_step`/
  `prevalidated_read_step` (skipping per-step re-validation) **but kept `compose()`,
  `compose_progress`/`compose_error`, `CompositionState` and `direct_outcome` unchanged**.
  Therefore C2's ≈0 measured gain (AX−A2 +9.9 → +9.7 ns, P0 §3.3) is evidence that the
  per-step re-admission item (G-REA-01/G-REX-01, ≈13/≈9 instrs ≈ 2-3 ns predicted) is
  individually below the 2.5-3 ns minimum detectable effect — it does **not** falsify the
  state-materialization items, which C2 never removed. Caveat: C2's steps still returned
  `Result<std::size_t>` through the compose lambda, possibly adding its own cost, so the
  null bounds rather than cleanly measures.
- The ≈+9.7-9.9 ns residual maps onto the ≈27 kept-excess instructions (items 7-9 + exit
  delta) at ≈2.7-3.7 instr/ns on this E5 path class — plausible carrier; per-item ns
  splits are [UNCERTAIN] (static decomposition, no per-item dynamic isolation exists).

## 4. Gap candidates

| ID | Source | Trigger | Necessary work | Excess work | Magnitude | Mechanism direction | Falsifier | Confidence |
|---|---|---|---|---|---|---|---|---|
| G-REA-01 | `src/blocking_file.cpp:297-299` (compose lambda calls public `read_at`); machine 0xfc0-0xfff | any multi- or single-step `read_exact_at` on an admitted request | one SEM-03 admission per public call + one pread/step | per-step re-admission ≈13 instrs (fd reload, closed, access, range/transfer recompute) duplicating the outer verdict | ≈13 instrs/step ≈ 2-3 ns/step [UNCERTAIN per-item]; C2 measured ≈0 alone (below MDE 2.5-3 ns) | private prevalidated step primitive (C2 shape) or loop-hoisted invariant | rebuild C2-style candidate in a fresh worktree from `77e5399a`; extend ladder with an A1X matched-native composition arm; 16 ABBA pairs 4 KiB: if AX−A1X < MDE the static excess is dynamically inert at this shape | high (static), low (standalone dynamic gain) |
| G-REA-02 | `compose_progress` by-value state round-trip `file_semantics.hpp:243-263`; machine 0x1250-0x127f | every step of every successful composition | none (block is dead on all paths; certified §1.3) | 8 instrs + 7 memory ops/step incl. SSE load+store to never-read `0x20(%rsp)` | ≈8 instrs/step ≈ 2-3 ns/step single-step; scales with steps | register-carry the loop state (confirmed/end/stop already live in registers at exit) or write outcome directly | candidate with block removed: objdump shows no `0x20(%rsp)`/`movaps`; suites 58/58 + fault 14/14 + admission mutants killed; dynamic paired delta as above | high (static dead-ness), medium (dynamic share) |
| G-REA-03 | `direct_outcome` + epilogue `src/blocking_file.cpp:241-265`; machine 0x10d3-0x1132 + dead 0x1285/0x1294 | every completed/failed composition exit | construct 33-B `Result<CompositionOutcome>` sret (≈5 memory ops) | intermediate stack materialization then copy: ~12 memory ops + 2 dead zero-stores vs N1's 5 | ≈7-9 instrs ≈ 2-3 ns/exit | build the outcome directly in sret slots (branch-tail stores per end-flavor) | same candidate class; static: single materialization in objdump; dynamic paired delta | high (static), medium (dynamic share) |
| G-REX-01 | `src/blocking_file.cpp:327-328`; machine 0x1790-0x17ad | any `read_exact` on an admitted request | one admission + one read/step | per-step re-admission ≈9 instrs (fd reload, closed, access, remaining-sign) | ≈9 instrs/step ≈ 2-3 ns [UNCERTAIN] | same as G-REA-01 | same protocol with a `read`-cursor ladder arm | high (static), low (standalone dynamic) |
| G-REX-02 | same `compose_progress` mechanism; machine 0x19d0-0x19ff | every step | none | 8 instrs + 7 memory ops/step (dead copy to `0x10(%rsp)`) | ≈2-3 ns/step | same as G-REA-02 | same as G-REA-02 | high (static), medium (dynamic) |
| G-REX-03 | same `direct_outcome`; machine 0x1873-0x18c7 + 0x1a05/0x1a14 | every exit | 33-B sret payload | ~12 vs 5 memory ops + 2 dead zero-stores | ≈2-3 ns/exit | same as G-REA-03 | same as G-REA-03 | high (static), medium (dynamic) |

Aggregate falsifier (honest given MDE): a combined candidate removing items 01+02+03 for
`read_exact_at` predicts ≈ −7…−10 ns on the AX single-step 4 KiB cell (>MDE). If the
combined paired delta stays < 3 ns, the +9.9 ns residual is not carried by these static
items and must be re-attributed (e.g. front-end/branch effects invisible to static
counts) — that outcome would refute this decomposition.

## 5. Must-preserve floor for any candidate (from P0 §5, verified anchors)

Precedence/oracle tests pinning these two ops at B1 (existence re-verified by P0 §5.3;
not re-run in this audit): zero-length no-op with no syscall
(`direct_composition_fault_test` `zero_length_requests_make_no_native_call`), EOF prefix
(`read_exact_reports_eof_before_full_with_the_prefix`), EINTR-without-count
(`primitive_retries_eintr_without_a_completed_count`), stop-reason pinning
(`every_stop_reason_is_pinned_against_the_oracle_error_rule`), precedence
(`composition_rejections_follow_the_shared_precedence`), plus the shared semantic tables
and the three admission mutants with their named killers. Matrix commands are not pinned
anywhere yet (P0 §3.6) — a P2 candidate must record them before claiming the 58/58.

## 6. Mutation-seam inventory for this group (status at B1)

- Present and registered: `SLUICE_SEM_MUTANT_EMPTY_BEFORE_CLOSED`
  (`file_semantics.hpp:158`) and `SLUICE_SEM_MUTANT_ADMIT_INVALID_RANGE`
  (`file_semantics.hpp:168`) cover the outer admission of both audited ops;
  `SLUICE_SEM_MUTANT_ADMIT_UNREPRESENTABLE_SIZE` (`:192`) is resize-only. xmake targets
  via `direct_mutant_target()` (`xmake/tests.lua:81-97`, three targets). Named killers at
  B1: `read_with_illegal_offset_reports_invalid_argument`
  (`tests/blocking_file_read_test.cpp:122`), `sequential_read_on_write_only_file_rejected_upfront`
  (`tests/blocking_file_sequential_test.cpp:392`).
- Missing at B1 (gap): **no registered product mutation seam exists for the composition
  machinery** — none for `compose_progress` transitions (EOF-vs-complete boundary,
  confirmed-prefix accumulation), `compose_error`, `composition_effect_certainty`
  (accounted/unknown), `direct_outcome` end/certainty mapping, impossible-count, or the
  per-step re-validation. The oracles exist (7 composition tests, 14 fault-injection
  tests) but kill-ability of machinery mutants is unproven. C2's two step seams
  (`SLUICE_DIRECT_MUTANT_STEP_SKIPS_EINTR_RETRY`,
  `SLUICE_DIRECT_MUTANT_STEP_REPORTS_FULL_COUNT`, patch lines 212-248) existed only in
  the reverted C2 worktree; `grep SLUICE_DIRECT_MUTANT` over B1 `include/ src/ xmake/`
  exits 1 (run this session). Any P2 candidate touching `compose`/`direct_outcome` must
  add such seams with named killer assertions (BASE GREEN → MUTANT RED → RESTORED GREEN).

## 7. Uncertainties declared

- Per-item nanosecond splits are static-derived; only the aggregate ≈+9.7-9.9 ns residual
  is measured (#600), and C2's null bounds the re-admission item.
- The N1 composition comparators are P1-built (this document), not #600-frozen artifacts.
- `read_exact` has no measured ladder history at all (P0 §4.13); its dynamic gap is
  extrapolated from `read_exact_at`'s.
- I did not execute any test suite in this audit (static audit scope); oracle existence
  claims cite P0 §5.3's executed checks at B1.
