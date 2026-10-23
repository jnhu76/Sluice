_ZN6sluice5async6detail11RequestCore7reserveEv:
.LFB6970:
	.cfi_startproc
	endbr64
	pushq	%r12
	.cfi_def_cfa_offset 16
	.cfi_offset 12, -16
	pushq	%rbp
	.cfi_def_cfa_offset 24
	.cfi_offset 6, -24
	movq	%rsi, %rbp
	pushq	%rbx
	.cfi_def_cfa_offset 32
	.cfi_offset 3, -32
	movq	%rdi, %rbx
	leaq	88(%rsi), %rdi
	call	pthread_mutex_lock@PLT
	testl	%eax, %eax
	jne	.L157
	cmpb	$0, 80(%rbp)
	jne	.L153
	pxor	%xmm0, %xmm0
	movq	$0, 16(%rbx)
	movups	%xmm0, (%rbx)
	movb	$1, (%rbx)
.L154:
	leaq	88(%rbp), %rdi
	call	pthread_mutex_unlock@PLT
	movq	%rbx, %rax
	popq	%rbx
	.cfi_remember_state
	.cfi_def_cfa_offset 24
	popq	%rbp
	.cfi_def_cfa_offset 16
	popq	%r12
	.cfi_def_cfa_offset 8
	ret
	.p2align 4,,10
	.p2align 3
.L153:
	.cfi_restore_state
	movq	48(%rbp), %rax
	cmpq	%rax, 40(%rbp)
	je	.L159
	movl	-4(%rax), %edx
	subq	$4, %rax
	pxor	%xmm0, %xmm0
	movq	%rax, 48(%rbp)
	movl	%edx, %eax
	imulq	$168, %rax, %rax
	addq	16(%rbp), %rax
	movups	%xmm0, 40(%rax)
	movups	%xmm0, 72(%rax)
	movups	%xmm0, 128(%rax)
	movups	%xmm0, 49(%rax)
	movups	%xmm0, 96(%rax)
	movups	%xmm0, 112(%rax)
	movups	%xmm0, 144(%rax)
	pxor	%xmm0, %xmm0
	movb	$1, (%rax)
	movq	$0, 16(%rax)
	movb	$0, 24(%rax)
	movq	$0, 28(%rax)
	movq	$0, 88(%rax)
	movl	$-1, 72(%rax)
	movq	$0, 160(%rax)
	movb	$1, 128(%rax)
	movq	8(%rax), %rax
	movups	%xmm0, (%rbx)
	movq	%rax, 16(%rbx)
	movl	%edx, 8(%rbx)
	jmp	.L154
	.p2align 4,,10
	.p2align 3
.L159:
	movq	8(%rbp), %rax
	pxor	%xmm0, %xmm0
	cmpq	%rax, 64(%rbp)
	movq	$0, 16(%rbx)
	movups	%xmm0, (%rbx)
	je	.L160
	movb	$2, (%rbx)
	jmp	.L154
	.p2align 4,,10
	.p2align 3
.L160:
	movb	$3, (%rbx)
	jmp	.L154
	.cfi_endproc
	.section	.text.unlikely
	.cfi_startproc
	.type	_ZN6sluice5async6detail11RequestCore7reserveEv.cold, @function
_ZN6sluice5async6detail11RequestCore7reserveEv.cold:
.LFSB6970:
.L157:
	.cfi_def_cfa_offset 32
	.cfi_offset 3, -32
	.cfi_offset 6, -24
	.cfi_offset 12, -16
	movl	%eax, %edi
	call	_ZSt20__throw_system_errori@PLT
	.cfi_endproc
.LFE6970:
	.text
	.size	_ZN6sluice5async6detail11RequestCore7reserveEv, .-_ZN6sluice5async6detail11RequestCore7reserveEv
