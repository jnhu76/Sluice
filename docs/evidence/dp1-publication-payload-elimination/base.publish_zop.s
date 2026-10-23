
.LFB7313:
	.cfi_startproc
	.cfi_personality 0x9b,DW.ref.__gxx_personality_v0
	.cfi_lsda 0x1b,.LLSDA7313
	endbr64
	pushq	%r12
	.cfi_def_cfa_offset 16
	.cfi_offset 12, -16
	pushq	%rbp
	.cfi_def_cfa_offset 24
	.cfi_offset 6, -24
	movq	%rdi, %rbp
	pushq	%rbx
	.cfi_def_cfa_offset 32
	.cfi_offset 3, -32
	movl	%esi, %ebx
	subq	$176, %rsp
	.cfi_def_cfa_offset 208
	movq	32(%rdi), %rdi
	movq	%fs:40, %r12
	movq	%r12, 168(%rsp)
	xorl	%r12d, %r12d
	movq	224(%rsp), %rax
	movdqu	208(%rsp), %xmm0
	movq	%rax, 16(%rsp)
	movups	%xmm0, (%rsp)
	call	_ZN6sluice5async6detail11RequestCore15acquire_controlENS1_10RequestKeyE@PLT
	addq	$32, %rsp
	.cfi_def_cfa_offset 176
	testb	%al, %al
	je	.L426
	leaq	16(%rsp), %rdi
	movq	%r12, %rax
	movl	$15, %ecx
	rep stosq
	leaq	16(%rsp), %rsi
	movb	$1, 96(%rsp)
	subq	$32, %rsp
	.cfi_def_cfa_offset 208
	movq	32(%rbp), %rdi
	movq	224(%rsp), %rax
	movdqu	208(%rsp), %xmm0
	movq	%rax, 16(%rsp)
	movups	%xmm0, (%rsp)
	call	_ZN6sluice5async6detail11RequestCore17begin_publicationENS1_10RequestKeyEPNS1_18PublicationPayloadE@PLT
	addq	$32, %rsp
	.cfi_def_cfa_offset 176
	testb	%al, %al
	jne	.L426
	subq	$32, %rsp
	.cfi_def_cfa_offset 208
	movq	32(%rbp), %rdi
	movdqu	208(%rsp), %xmm0
	movq	224(%rsp), %rax
	movups	%xmm0, (%rsp)
	movq	%rax, 16(%rsp)
	call	_ZN6sluice5async6detail11RequestCore20complete_publicationENS1_10RequestKeyE@PLT
	addq	$32, %rsp
	.cfi_def_cfa_offset 176
	testb	%al, %al
	jne	.L426
	leaq	232(%rbp), %rdi
	call	pthread_mutex_lock@PLT
	testl	%eax, %eax
	jne	.L453
	movdqu	176(%rsp), %xmm0
	movl	%ebx, %eax
	movq	192(%rsp), %rdx
	leaq	232(%rbp), %rdi
	salq	$5, %rax
	addq	72(%rbp), %rax
	movb	$1, 1(%rax)
	movq	%rdx, 24(%rax)
	movups	%xmm0, 8(%rax)
	addq	$1, 96(%rbp)
	call	pthread_mutex_unlock@PLT
	movq	24(%rbp), %rbx
	testq	%rbx, %rbx
	je	.L424
	movq	%rbx, %rdi
	call	pthread_mutex_lock@PLT
	testl	%eax, %eax
	jne	.L454
	movq	40(%rbx), %rax
	cmpq	$-1, %rax
	je	.L432
	addq	$1, %rax
	movq	%rax, 40(%rbx)
.L433:
	movl	108(%rbx), %eax
	testl	%eax, %eax
	js	.L434
	cmpb	$0, 112(%rbx)
	jne	.L434
	movq	$1, 8(%rsp)
	leaq	8(%rsp), %r12
	call	__errno_location@PLT
	movq	%rax, %rbp
.L437:
	movl	$0, 0(%rbp)
	movl	108(%rbx), %edi
	movl	$8, %edx
	movq	%r12, %rsi
	call	write@PLT
	cmpq	$8, %rax
	je	.L435
	movl	0(%rbp), %r8d
	testq	%rax, %rax
	jns	.L436
	cmpl	$4, %r8d
	je	.L437
	cmpl	$11, %r8d
	jne	.L436
.L435:
	movb	$1, 112(%rbx)
.L434:
	movq	%rbx, %rdi
	call	pthread_mutex_unlock@PLT
.L424:
	movq	136(%rsp), %rax
	subq	%fs:40, %rax
	jne	.L457
	addq	$144, %rsp
	.cfi_remember_state
	.cfi_def_cfa_offset 32
	popq	%rbx
	.cfi_def_cfa_offset 24
	popq	%rbp
	.cfi_def_cfa_offset 16
	popq	%r12
	.cfi_def_cfa_offset 8
	ret
	.p2align 4,,10
	.p2align 3
.L432:
	.cfi_restore_state
	movq	56(%rbx), %rax
	cmpq	$-1, %rax
	je	.L433
	addq	$1, %rax
	movq	%rax, 56(%rbx)
	jmp	.L433
.L426:
	call	_ZN6sluice5async6detail28uring_core_handoff_fail_fastEv@PLT
.L457:
	call	__stack_chk_fail@PLT
.L436:
	movq	stderr(%rip), %rdi
	leaq	.LC17(%rip), %rcx
	leaq	.LC18(%rip), %rdx
	xorl	%eax, %eax
	movl	$2, %esi
	call	__fprintf_chk@PLT
	movq	stderr(%rip), %rdi
	call	fflush@PLT
	call	_ZSt9terminatev@PLT
	.section	.gcc_except_table
.LLSDA7313:
	.byte	0xff
	.byte	0xff
	.byte	0x1
	.uleb128 .LLSDACSE7313-.LLSDACSB7313
.LLSDACSB7313:
.LLSDACSE7313:
	.text
	.cfi_endproc
	.section	.text.unlikely
	.cfi_startproc
	.cfi_personality 0x9b,DW.ref.__gxx_personality_v0
	.cfi_lsda 0x1b,.LLSDAC7313
	.type	_ZN6sluice5async17UringAsyncBackend22publish_zero_op_inlineENS0_6detail10RequestKeyENS2_18RequestReservationE.cold, @function
_ZN6sluice5async17UringAsyncBackend22publish_zero_op_inlineENS0_6detail10RequestKeyENS2_18RequestReservationE.cold:
.LFSB7313:
.L453:
	.cfi_def_cfa_offset 176
	.cfi_offset 3, -32
	.cfi_offset 6, -24
	.cfi_offset 12, -16
	movq	136(%rsp), %rdx
	subq	%fs:40, %rdx
	jne	.L456
	movl	%eax, %edi
	call	_ZSt20__throw_system_errori@PLT
.L454:
	movq	136(%rsp), %rdx
	subq	%fs:40, %rdx
	jne	.L456
	movl	%eax, %edi
	call	_ZSt20__throw_system_errori@PLT
.L456:
	call	__stack_chk_fail@PLT
	.cfi_endproc
.LFE7313:
	.section	.gcc_except_table
.LLSDAC7313:
	.byte	0xff
	.byte	0xff
	.byte	0x1
	.uleb128 .LLSDACSEC7313-.LLSDACSBC7313
.LLSDACSBC7313:
.LLSDACSEC7313:
	.section	.text.unlikely
	.text