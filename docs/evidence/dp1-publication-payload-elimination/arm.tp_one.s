
.LFB7108:
	.cfi_startproc
	endbr64
	pushq	%r12
	.cfi_def_cfa_offset 16
	.cfi_offset 12, -16
	movq	%rdx, %r12
	pushq	%rbp
	.cfi_def_cfa_offset 24
	.cfi_offset 6, -24
	movq	%rsi, %rbp
	pushq	%rbx
	.cfi_def_cfa_offset 32
	.cfi_offset 3, -32
	movq	%rdi, %rbx
	subq	$96, %rsp
	.cfi_def_cfa_offset 128
	movq	32(%rdi), %rdi
	call	_ZNK6sluice5async6detail11RequestCore7contextEv@PLT
	movq	%r12, 16(%rsp)
	subq	$32, %rsp
	.cfi_def_cfa_offset 160
	xorl	%esi, %esi
	movq	%rax, 32(%rsp)
	movq	32(%rbx), %rdi
	movl	%ebp, 40(%rsp)
	movdqa	32(%rsp), %xmm0
	movq	%r12, 16(%rsp)
	movups	%xmm0, (%rsp)
	call	_ZN6sluice5async6detail11RequestCore17begin_publicationENS1_10RequestKeyEPNS1_18PublicationPayloadE@PLT
	addq	$32, %rsp
	.cfi_def_cfa_offset 128
	testb	%al, %al
	jne	.L506
	subq	$32, %rsp
	.cfi_def_cfa_offset 160
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
	.cfi_def_cfa_offset 128
	testb	%al, %al
	jne	.L506
	movdqa	(%rsp), %xmm0
	movq	16(%rsp), %rax
	subq	$32, %rsp
	.cfi_def_cfa_offset 160
	movq	32(%rbx), %rdi
	movaps	%xmm0, 64(%rsp)
	movq	%rax, 80(%rsp)
	movq	%rax, 16(%rsp)
	movups	%xmm0, (%rsp)
	call	_ZN6sluice5async6detail11RequestCore23claim_observer_deliveryENS1_10RequestKeyE@PLT
	movq	16(%rbx), %rdi
	addq	$32, %rsp
	.cfi_def_cfa_offset 128
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
	jne	.L510
.L508:
	movq	32(%rbx), %rdi
	subq	$32, %rsp
	.cfi_def_cfa_offset 160
	movq	%r8, (%rsp)
	movq	%r9, 8(%rsp)
	movq	%rax, 16(%rsp)
	call	_ZN6sluice5async6detail11RequestCore24retire_observer_deliveryENS1_10RequestKeyE@PLT
	subq	$-128, %rsp
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
.L510:
	.cfi_def_cfa_offset 128
	subq	$32, %rsp
	.cfi_def_cfa_offset 160
	movdqa	112(%rsp), %xmm0
	movq	%r8, (%rsp)
	movq	%r9, 8(%rsp)
	movups	%xmm0, 16(%rsp)
	call	*%rdx
	addq	$32, %rsp
	.cfi_def_cfa_offset 128
	movq	32(%rsp), %r8
	movq	40(%rsp), %r9
	movq	48(%rsp), %rax
	jmp	.L508
.L506:
	call	_ZN6sluice5async12_GLOBAL__N_1L42threadpool_publication_invariant_fail_fastEv
	.cfi_endproc
.LFE7108: