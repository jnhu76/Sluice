
.LFB7111:
	.cfi_startproc
	endbr64
	pushq	%r13
	.cfi_def_cfa_offset 16
	.cfi_offset 13, -16
	pushq	%r12
	.cfi_def_cfa_offset 24
	.cfi_offset 12, -24
	movq	%rdx, %r12
	pushq	%rbp
	.cfi_def_cfa_offset 32
	.cfi_offset 6, -32
	movq	%rsi, %rbp
	pushq	%rbx
	.cfi_def_cfa_offset 40
	.cfi_offset 3, -40
	movq	%rdi, %rbx
	subq	$232, %rsp
	.cfi_def_cfa_offset 272
	movq	32(%rdi), %rdi
	movq	%fs:40, %r13
	movq	%r13, 216(%rsp)
	xorl	%r13d, %r13d
	call	_ZNK6sluice5async6detail11RequestCore7contextEv@PLT
	leaq	96(%rsp), %rdi
	movl	$15, %ecx
	movl	%ebp, 8(%rsp)
	movq	%rax, (%rsp)
	leaq	96(%rsp), %rsi
	subq	$32, %rsp
	.cfi_def_cfa_offset 304
	movq	%r12, 48(%rsp)
	movq	%r13, %rax
	rep stosq
	movb	$1, 208(%rsp)
	movq	32(%rbx), %rdi
	movdqa	32(%rsp), %xmm0
	movq	%r12, 16(%rsp)
	movups	%xmm0, (%rsp)
	call	_ZN6sluice5async6detail11RequestCore17begin_publicationENS1_10RequestKeyEPNS1_18PublicationPayloadE@PLT
	addq	$32, %rsp
	.cfi_def_cfa_offset 272
	testb	%al, %al
	jne	.L508
	subq	$32, %rsp
	.cfi_def_cfa_offset 304
	movq	32(%rbx), %rdi
	movl	%ebp, %ebp
	movdqa	32(%rsp), %xmm0
	movq	48(%rsp), %rax
	salq	$5, %rbp
	addq	72(%rbx), %rbp
	movzbl	0(%rbp), %ebp
	movq	%rax, 16(%rsp)
	movups	%xmm0, (%rsp)
	call	_ZN6sluice5async6detail11RequestCore20complete_publicationENS1_10RequestKeyE@PLT
	addq	$32, %rsp
	.cfi_def_cfa_offset 272
	testb	%al, %al
	jne	.L508
	movdqa	96(%rsp), %xmm0
	movq	112(%rsp), %rax
	subq	$32, %rsp
	.cfi_def_cfa_offset 304
	movq	32(%rbx), %rdi
	movaps	%xmm0, 64(%rsp)
	movq	%rax, 80(%rsp)
	movq	%rax, 16(%rsp)
	movups	%xmm0, (%rsp)
	call	_ZN6sluice5async6detail11RequestCore23claim_observer_deliveryENS1_10RequestKeyE@PLT
	movq	16(%rbx), %rdi
	addq	$32, %rsp
	.cfi_def_cfa_offset 272
	leaq	96(%rbx), %rax
	movq	32(%rsp), %r8
	movq	40(%rsp), %r9
	movb	%bpl, 88(%rsp)
	leaq	_ZN6sluice5async6detail18ReferenceReadySink8on_readyENS1_10ReadyEventE(%rip), %rcx
	testq	%rdi, %rdi
	movl	$0, 89(%rsp)
	cmove	%rax, %rdi
	movl	$0, 92(%rsp)
	movq	(%rdi), %rax
	movq	16(%rax), %rdx
	movq	48(%rsp), %rax
	movq	%rax, 80(%rsp)
	cmpq	%rcx, %rdx
	jne	.L513
.L510:
	movq	32(%rbx), %rdi
	subq	$32, %rsp
	.cfi_def_cfa_offset 304
	movq	%r8, (%rsp)
	movq	%r9, 8(%rsp)
	movq	%rax, 16(%rsp)
	call	_ZN6sluice5async6detail11RequestCore24retire_observer_deliveryENS1_10RequestKeyE@PLT
	addq	$32, %rsp
	.cfi_def_cfa_offset 272
	movq	216(%rsp), %rax
	subq	%fs:40, %rax
	jne	.L514
	addq	$232, %rsp
	.cfi_remember_state
	.cfi_def_cfa_offset 40
	popq	%rbx
	.cfi_def_cfa_offset 32
	popq	%rbp
	.cfi_def_cfa_offset 24
	popq	%r12
	.cfi_def_cfa_offset 16
	popq	%r13
	.cfi_def_cfa_offset 8
	ret
	.p2align 4,,10
	.p2align 3
.L513:
	.cfi_restore_state
	subq	$32, %rsp
	.cfi_def_cfa_offset 304
	movdqa	112(%rsp), %xmm0
	movq	%r8, (%rsp)
	movq	%r9, 8(%rsp)
	movups	%xmm0, 16(%rsp)
	call	*%rdx
	addq	$32, %rsp
	.cfi_def_cfa_offset 272
	movq	32(%rsp), %r8
	movq	40(%rsp), %r9
	movq	48(%rsp), %rax
	jmp	.L510
.L508:
	call	_ZN6sluice5async12_GLOBAL__N_1L42threadpool_publication_invariant_fail_fastEv
.L514:
	call	__stack_chk_fail@PLT
	.cfi_endproc
.LFE7111: