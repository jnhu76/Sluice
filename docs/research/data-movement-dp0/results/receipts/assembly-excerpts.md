# Assembly excerpts (receipts)

All excerpts are from `objdump -drwC` of the arm archives built at be28f275
with the frozen release config (see ../experiment-protocol.md). Full objdump
text is reproducible via `results/receipts/build_arm.sh` plus the dump commands
in `../compiler-inventory.md`; excerpts below are the load-bearing passages,
quoted verbatim from the session artifacts.

## A. begin_publication (base arm) — the DP-1 payload copy block

`RequestCore::begin_publication(RequestKey, PublicationPayload*)`, base:
the `out != nullptr` guarded block fills the out payload with the id from the
key argument (key bytes at 0x30..0x47 on the callee stack), then copies the
slot's descriptor op/offset/length (slot 0x28/0x30..0x3f), IoOutcome
(0x60..0x7f) and FileInfo (0x80..0xa7) into the 120-byte payload — 12 SIMD
moves per publication:

```
11ed: test %r14,%r14            ; r14 = out payload pointer
11f0: je   1250                 ; out == nullptr skips the whole block
11f2: movdqu 0x30(%rsp),%xmm0   ; key.context+slot (key argument area)
11f8: mov  0x40(%rsp),%rdx
11fd: movups %xmm0,(%r14)
1201: mov  %rdx,0x10(%r14)
1205: movdqu 0x30(%rax),%xmm0   ; slot op/offset/len
120a: movzbl 0x28(%rax),%edx
120e: movups %xmm0,0x20(%r14)
1213: movdqu 0x60(%rax),%xmm0   ; slot IoOutcome
1218: mov  %dl,0x18(%r14)
121c: movups %xmm0,0x30(%r14)
1221: movdqu 0x70(%rax),%xmm0
1226: movups %xmm0,0x40(%r14)
122b: movdqu 0x80(%rax),%xmm0   ; slot FileInfo
1233: movups %xmm0,0x50(%r14)
1238: movdqu 0x90(%rax),%xmm0
1240: mov  0xa0(%rax),%rax
1247: mov  %rax,0x70(%r14)
124b: movups %xmm0,0x60(%r14)
1250: xor  %ebp,%ebp            ; success
```

In the dp1/dp1dp2b arms every caller passes `nullptr`, so at runtime the block
is skipped by the `test/je` — and the callers' payload construction
(`mov $0xf,%ecx; rep stos %rax,(%rdi)` — 15 qwords = 120 bytes — in base
`UringAsyncBackend::publish_zero_op_inline` at 14d7..14e4) is gone entirely
from the dp1/dp1dp2b machine code. GCC cannot do this elimination itself at
-O3 without LTO: the probe in `../gimple-sra-dse.md` §2 shows cross-TU DSE
keeps all dead stores against an extern callee.

## B. reserve() (base vs dp3a) — the DP-3A double zeroing

Base `RequestCore::reserve()`, free-list path: ~14 stores re-zeroing ~132 B of
the slot that release_slot_ (below) already zeroed before the free-list push:

```
94a: mov -0x4(%rax),%edx       ; pop generation
951: pxor %xmm0,%xmm0
955: mov %rax,0x30(%rbp)
95b: movups %xmm0,(%rbx)       ; Reservation init (not slot)
95e: imul $0xa8,%rax,%rax
965: add 0x10(%rbp),%rax       ; slot address
969: mov %edx,0x8(%rbx)
96c: movb $0x1,(%rax)          ; slot.phase = reserved
...
```
(The base arm's corresponding body stores zero to phase/generation/bools/
observer_phase/refs/descriptor/borrow/outcome/metadata regions here; dp3a's
entire free-list path is:)
```
94a: mov -0x4(%rax),%edx
94d: sub  $0x4,%rax
951: pxor %xmm0,%xmm0
955: mov %rax,0x30(%rbp)
959: mov %edx,%eax
95b: movups %xmm0,(%rbx)       ; Reservation init only
95e: imul $0xa8,%rax,%rax
965: add 0x10(%rbp),%rax
969: mov %edx,0x8(%rbx)
96c: movb $0x1,(%rax)          ; slot.phase = reserved  <-- the ONLY slot store
96f: mov 0x8(%rax),%rax
973: mov %rax,0x10(%rbx)
```

## C. release_slot_ (base; unchanged in dp3a) — full 3-line zeroing, 16 stores

```
1b58: pxor %xmm0,%xmm0
1b5c: movb  $0x0,(%rsi)          ; line0: phase
1b5f: movups %xmm0,0x28(%rsi)    ; line0/1: descriptor 32B
1b63: movups %xmm0,0x48(%rsi)    ; line1: borrow facts
1b67: movups %xmm0,0x80(%rsi)    ; line2: metadata
1b6e: movq  $0x0,0x10(%rsi)      ; line0: generation
1b76: movb  $0x0,0x18(%rsi)      ; line0: observer_phase
1b7a: movq  $0x0,0x1c(%rsi)      ; line0: execution_refs
1b82: movq  $0x0,0x58(%rsi)      ; line1
1b8a: movl  $0xffffffff,0x48(%rsi); line1: borrow gen reset
1b91: movq  $0x0,0xa0(%rsi)      ; line2
1b9c: movb  $0x1,0x80(%rsi)      ; line2: metadata kind reset
1ba3: movups %xmm0,0x31(%rsi)    ; line0: descriptor tail
1ba7: movups %xmm0,0x60(%rsi)    ; line1: IoOutcome
1bab: movups %xmm0,0x70(%rsi)    ; line1
1baf: movups %xmm0,0x90(%rsi)    ; line2
1bb6: cmp $0xffffffffffffffff,%rax ; generation decide reclaim/defer
1bc4: mov %rax,0x8(%rsi)         ; new generation
```

## D. RequestBridge::observe 4-arg (dp2) and split 5-arg (dp2b) — consumer side

dp2's `observe(core, id, RequestResultData*, bool include_metadata)`, excerpt
(dp2_rc.objdump.txt 1fc7..209c): the bool flag selects the metadata copy; when
false, the 0x80/0x90/0xa0 slot loads and the 40 B store to `out->metadata` are
skipped, but the caller's 72 B `RequestResultData` still exists:

```
1fc7: test %cl,%cl                    ; include_metadata
1fe9: cmovne %rbp,%rax                ; metadata out ptr or null
...
2076: test %rdx,%rdx                  ; rdx = metadata ptr
2079: je   2130                       ; skip FileInfo copy when null
207f: movdqu 0x80(%rax),%xmm0         ; slot metadata
208a: movaps %xmm0,(%rdx)
...
```

dp2b's split `observe(core, id, bool*, IoError*, EffectReport*)` accesses only
IoOutcome (0x60/0x70) — zero accesses to 0x80/0x90/0xa0 anywhere in the
function (verified by grep over the full objdump; same for the split
`consume`), and the caller (Request<T> header template) no longer declares any
`RequestResultData` for non-metadata T, eliminating the 72 B local init
(`movaps %xmm0,0x30(%rsp); movq $0x0,0x50(%rsp); movaps %xmm0,0x40(%rsp)` in
the base/dp2 shapes) — visible as its absence in dp2b_rc.objdump.txt.

## E. w01ctl loop body — base 14 vs dp2b 15 instructions/iteration

base `main` w01ctl loop (back edge to 5c10):
```
5c10: mov %r14,%r13
5c13: mov %r13,%rdx
5c16: mov -0x340(%rbp),%rsi
5c1d: mov %r15,%rcx
5c20: mov %rbx,%r8
5c23: and $0xfff,%edx
5c29: mov -0x338(%rbp),%rdi
5c30: shl $0xc,%rdx
5c34: call blocking::read_at
5c39: cmpb $0x0,-0x220(%rbp)
5c40: je ...                       ; error branch (not taken)
5c46: add -0x230(%rbp),%r12
5c4d: lea 0x1(%r13),%r14
5c51: cmp -0x350(%rbp),%r14
5c58: je ...
5c5e: cmp %r13,-0x348(%rbp)
5c65: jne 5c10
```
dp2b same loop (back edge to 5c40): identical work but the register allocator
reloads loop-invariant operands from the stack and passes r8 via an extra
`mov %rdx,%r8` — 15 instructions per iteration:
```
5c40: mov %r14,%r13
5c43: mov -0x338(%rbp),%rdx
5c4a: mov -0x348(%rbp),%rsi
5c51: mov %r15,%rcx
5c54: mov -0x340(%rbp),%rdi
5c5b: mov %rdx,%r8          <-- the +1
5c5e: mov %r13,%rdx
5c61: and $0xfff,%edx
5c67: shl $0xc,%rdx
5c6b: call blocking::read_at
...
5c9c: jne 5c40
```
+1 instruction × 220 000 iterations (200 000 measured + 20 000 warmup)
= +220 000 — exactly the measured w01ctl instruction delta of the dp2b
microbench binary. This is a bench-TU register-allocation artifact of
recompiling the same source against a modified header, not a library change
(`blocking_file.o` is byte-identical across arms).
