
.LFB7106:
	.cfi_startproc
	.cfi_personality 0x9b,DW.ref.__gxx_personality_v0
	.cfi_lsda 0x1b,.LLSDA7106
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
	subq	$48, %rsp
	.cfi_def_cfa_offset 80
	movq	32(%rdi), %rdi
	movq	%fs:40, %rbx
	movq	%rbx, 40(%rsp)
	movl	%esi, %ebx
	movdqu	80(%rsp), %xmm0
	movq	96(%rsp), %rax
	movups	%xmm0, (%rsp)
	movq	%rax, 16(%rsp)
	call	_ZN6sluice5async6detail11RequestCore15acquire_controlENS1_10RequestKeyE@PLT
	addq	$32, %rsp
	.cfi_def_cfa_offset 48
	testb	%al, %al
	je	.L324
	subq	$32, %rsp
	.cfi_def_cfa_offset 80
	movq	32(%rbp), %rdi
	xorl	%esi, %esi
	movdqu	80(%rsp), %xmm0
	movq	96(%rsp), %rax
	movups	%xmm0, (%rsp)
	movq	%rax, 16(%rsp)
	call	_ZN6sluice5async6detail11RequestCore17begin_publicationENS1_10RequestKeyEPNS1_18PublicationPayloadE@PLT
	addq	$32, %rsp
	.cfi_def_cfa_offset 48
	testb	%al, %al
	jne	.L324
	subq	$32, %rsp
	.cfi_def_cfa_offset 80
	movq	32(%rbp), %rdi
	movdqu	80(%rsp), %xmm0
	movq	96(%rsp), %rax
	movups	%xmm0, (%rsp)
	movq	%rax, 16(%rsp)
	call	_ZN6sluice5async6detail11RequestCore20complete_publicationENS1_10RequestKeyE@PLT
	addq	$32, %rsp
	.cfi_def_cfa_offset 48
	testb	%al, %al
	jne	.L324
	leaq	104(%rbp), %rdi
	call	pthread_mutex_lock@PLT
	testl	%eax, %eax
	jne	.L349
	movq	64(%rsp), %rdx
	movl	%ebx, %eax
	movdqu	48(%rsp), %xmm0
	leaq	104(%rbp), %rdi
	salq	$5, %rax
	addq	72(%rbp), %rax
	movb	$1, 1(%rax)
	movq	%rdx, 24(%rax)
	movups	%xmm0, 8(%rax)
	call	pthread_mutex_unlock@PLT
	movq	24(%rbp), %rbx
	testq	%rbx, %rbx
	je	.L322
	movq	%rbx, %rdi
	call	pthread_mutex_lock@PLT
	testl	%eax, %eax
	jne	.L350
	movq	40(%rbx), %rax
	cmpq	$-1, %rax
	je	.L330
	addq	$1, %rax
	movq	%rax, 40(%rbx)
.L331:
	movl	108(%rbx), %eax
	testl	%eax, %eax
	js	.L332
	cmpb	$0, 112(%rbx)
	jne	.L332
	movq	$1, (%rsp)
	movq	%rsp, %r12
	call	__errno_location@PLT
	movq	%rax, %rbp
.L335:
	movl	$0, 0(%rbp)
	movl	108(%rbx), %edi
	movl	$8, %edx
	movq	%r12, %rsi
	call	write@PLT
	cmpq	$8, %rax
	je	.L333
	movl	0(%rbp), %r8d
	testq	%rax, %rax
	jns	.L334
	cmpl	$4, %r8d
	je	.L335
	cmpl	$11, %r8d
	jne	.L334
.L333:
	movb	$1, 112(%rbx)
.L332:
	movq	8(%rsp), %rax
	subq	%fs:40, %rax
	jne	.L353
	addq	$16, %rsp
	.cfi_remember_state
	.cfi_def_cfa_offset 32
	movq	%rbx, %rdi
	popq	%rbx
	.cfi_def_cfa_offset 24
	popq	%rbp
	.cfi_def_cfa_offset 16
	popq	%r12
	.cfi_def_cfa_offset 8
.LEHB28:
	jmp	pthread_mutex_unlock@PLT
.LEHE28:
	.p2align 4,,10
	.p2align 3
.L322:
	.cfi_restore_state
	movq	8(%rsp), %rax
	subq	%fs:40, %rax
	jne	.L353
	addq	$16, %rsp
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
.L330:
	.cfi_restore_state
	movq	56(%rbx), %rax
	cmpq	$-1, %rax
	je	.L331
	addq	$1, %rax
	movq	%rax, 56(%rbx)
	jmp	.L331
.L324:
	call	_ZN6sluice5async6detail33threadpool_core_handoff_fail_fastEv@PLT
.L353:
	call	__stack_chk_fail@PLT
.L334:
	movq	stderr(%rip), %rdi
	leaq	.LC13(%rip), %rcx
	leaq	.LC14(%rip), %rdx
	xorl	%eax, %eax
	movl	$2, %esi
	call	__fprintf_chk@PLT
	movq	stderr(%rip), %rdi
	call	fflush@PLT
	call	_ZSt9terminatev@PLT
	.section	.gcc_except_table
.LLSDA7106:
	.byte	0xff
	.byte	0xff
	.byte	0x1
	.uleb128 .LLSDACSE7106-.LLSDACSB7106
.LLSDACSB7106:
	.uleb128 .LEHB28-.LFB7106
	.uleb128 .LEHE28-.LEHB28
	.uleb128 0
	.uleb128 0
.LLSDACSE7106:
	.text
	.cfi_endproc
	.section	.text.unlikely
	.cfi_startproc
	.cfi_personality 0x9b,DW.ref.__gxx_personality_v0
	.cfi_lsda 0x1b,.LLSDAC7106
	.type	_ZN6sluice5async17ThreadPoolBackend22publish_zero_op_inlineENS0_6detail10RequestKeyENS2_18RequestReservationE.cold, @function
_ZN6sluice5async17ThreadPoolBackend22publish_zero_op_inlineENS0_6detail10RequestKeyENS2_18RequestReservationE.cold:
.LFSB7106:
.L350:
	.cfi_def_cfa_offset 48
	.cfi_offset 3, -32
	.cfi_offset 6, -24
	.cfi_offset 12, -16
	movq	8(%rsp), %rdx
	subq	%fs:40, %rdx
	jne	.L352
	movl	%eax, %edi
	call	_ZSt20__throw_system_errori@PLT
.L349:
	movq	8(%rsp), %rdx
	subq	%fs:40, %rdx
	jne	.L352
	movl	%eax, %edi
	call	_ZSt20__throw_system_errori@PLT
.L352:
	call	__stack_chk_fail@PLT
	.cfi_endproc
.LFE7106:
	.section	.gcc_except_table
.LLSDAC7106:
	.byte	0xff
	.byte	0xff
	.byte	0x1
	.uleb128 .LLSDACSEC7106-.LLSDACSBC7106
.LLSDACSBC7106:
.LLSDACSEC7106:
	.section	.text.unlikely
	.text