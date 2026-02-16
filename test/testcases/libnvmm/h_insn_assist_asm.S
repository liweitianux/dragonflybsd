/*
 * Copyright (c) 2018-2021 Maxime Villard, m00nbsd.net
 * Copyright (c) 2026 The DragonFly Project
 * All rights reserved.
 *
 * This code is part of the NVMM hypervisor.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

	.text
	.code64

/* Test end macro - trigger RDMSR to exit */
#define TEST_END	\
	movq	$0xFFFFFFFFFFFFFFFF,%rcx;	\
	rdmsr	;

/* Memory area for data */
#define DATA_PAGE	0x1000

/* CR0 bit definitions */
#define CR0_PE		0x00000001	/* Protection Enable */
#define CR0_MP		0x00000002	/* Monitor Coprocessor */
#define CR0_EM		0x00000004	/* Emulation */
#define CR0_TS		0x00000008	/* Task Switched */
#define CR0_ET		0x00000010	/* Extension Type */
#define CR0_NE		0x00000020	/* Numeric Error */
#define CR0_WP		0x00010000	/* Write Protect */
#define CR0_AM		0x00040000	/* Alignment Mask */
#define CR0_NW		0x20000000	/* Not Write-through */
#define CR0_CD		0x40000000	/* Cache Disable */
#define CR0_PG		0x80000000	/* Paging */

/* CR4 bit definitions */
#define CR4_PSE		0x00000010	/* Page Size Extensions */
#define CR4_PAE		0x00000020	/* Physical Address Extension */
#define CR4_PGE		0x00000080	/* Page Global Enable */
#define CR4_OSFXSR	0x00000200	/* OS FXSAVE/FXRSTOR Support */

/* -------------------------------------------------------------------------- */

	.globl	test_lmsw_reg1_begin, test_lmsw_reg1_end
	.globl	test_lmsw_reg2_begin, test_lmsw_reg2_end
	.globl	test_lmsw_pe_begin, test_lmsw_pe_end
	.globl	test_lmsw_mem_begin, test_lmsw_mem_end
	.globl	test_lmsw_disp_begin, test_lmsw_disp_end
	.globl	test_lmsw_sib_begin, test_lmsw_sib_end
	.globl	test_lmsw_indirect_begin, test_lmsw_indirect_end
	.globl	test_lmsw_riprel_begin, test_lmsw_riprel_end

	.align	64
test_lmsw_reg1_begin:
	/*
	 * Test: LMSW with register AX
	 */
	movw	$(CR0_EM|CR0_PE),%ax
	lmsw	%ax

	TEST_END
test_lmsw_reg1_end:

	.align	64
test_lmsw_reg2_begin:
	/*
	 * Test: LMSW with register BX
	 */
	movw	$(CR0_EM|CR0_MP|CR0_PE),%bx
	lmsw	%bx

	TEST_END
test_lmsw_reg2_end:

	.align	64
test_lmsw_pe_begin:
	/*
	 * Test: LMSW cannot clear PE bit
	 */
	movw	$0x0000,%ax
	lmsw	%ax

	TEST_END
test_lmsw_pe_end:

	.align	64
test_lmsw_mem_begin:
	/*
	 * Test: LMSW with memory operand
	 */
	movq	$DATA_PAGE,%rbx
	movw	$(CR0_EM|CR0_MP|CR0_PE),(%rbx)
	lmsw	(%rbx)

	TEST_END
test_lmsw_mem_end:

	.align	64
test_lmsw_disp_begin:
	/*
	 * Test: LMSW with displacement
	 */
	movq	$DATA_PAGE,%rax
	movw	$(CR0_EM|CR0_MP|CR0_PE),16(%rax)
	lmsw	16(%rax)

	TEST_END
test_lmsw_disp_end:

	.align	64
test_lmsw_sib_begin:
	/*
	 * Test: LMSW with SIB addressing
	 */
	movq	$DATA_PAGE,%rbp
	movq	$4,%rsi
	movw	$(CR0_EM|CR0_MP|CR0_PE),(%rbp,%rsi,2)
	lmsw	(%rbp,%rsi,2)

	TEST_END
test_lmsw_sib_end:

	.align	64
test_lmsw_indirect_begin:
	/*
	 * Test: LMSW with indirect memory access
	 */
	movq	$DATA_PAGE,%rdi
	movw	$(CR0_EM|CR0_MP),(%rdi)
	lmsw	(%rdi)

	TEST_END
test_lmsw_indirect_end:

	.align	64
test_lmsw_riprel_begin:
	/*
	 * Test: LMSW with RIP-relative addressing
	 */
	lmsw	lmsw_data(%rip)
	jmp	.Lmsw_end

	.align	2
lmsw_data:
	.word	(CR0_EM|CR0_MP)

.Lmsw_end:
	TEST_END
test_lmsw_riprel_end:

/* -------------------------------------------------------------------------- */

	.globl	test_mov_cr0_basic_begin, test_mov_cr0_basic_end
	.globl	test_mov_cr0_rex_begin, test_mov_cr0_rex_end
	.globl	test_mov_cr0_multi_begin, test_mov_cr0_multi_end

	.align	64
test_mov_cr0_basic_begin:
	/*
	 * Test: MOV CR0, RAX (basic)
	 */
	movq	%cr0,%rax
	orq	$(CR0_WP|CR0_AM),%rax
	movq	%rax,%cr0

	TEST_END
test_mov_cr0_basic_end:

	.align	64
test_mov_cr0_rex_begin:
	/*
	 * Test: MOV CR0, R15 (with REX prefix)
	 */
	movq	%cr0,%r15
	orq	$(CR0_WP|CR0_AM),%r15
	movq	%r15,%cr0

	TEST_END
test_mov_cr0_rex_end:

	.align	64
test_mov_cr0_multi_begin:
	/*
	 * Test: Multiple REX-prefixed MOV CR0 instructions
	 */
	movq	%cr0,%r8
	movq	%cr0,%r9

	orq	$CR0_WP,%r8
	movq	%r8,%cr0

	orq	$CR0_AM,%r9
	movq	%r9,%cr0

	TEST_END
test_mov_cr0_multi_end:

/* -------------------------------------------------------------------------- */

	.globl	test_mov_cr4_basic_begin, test_mov_cr4_basic_end
	.globl	test_mov_cr4_pge_begin, test_mov_cr4_pge_end

	.align	64
test_mov_cr4_basic_begin:
	/*
	 * Test: MOV CR4, RAX (basic)
	 */
	movq	%cr4,%rax
	orq	$(CR4_PSE|CR4_OSFXSR),%rax
	movq	%rax,%cr4

	TEST_END
test_mov_cr4_basic_end:

	.align	64
test_mov_cr4_pge_begin:
	/*
	 * Test: MOV CR4, RBX (PGE bit)
	 */
	movq	%cr4,%rbx
	orq	$CR4_PGE,%rbx
	movq	%rbx,%cr4

	TEST_END
test_mov_cr4_pge_end:

/* -------------------------------------------------------------------------- */

	.globl	test_mov_cr_sequence_begin, test_mov_cr_sequence_end

	.align	64
test_mov_cr_sequence_begin:
	/*
	 * Test: Alternating CR0 and CR4 operations
	 */
	/* Toggle CR0.AM */
	movq	%cr0,%rax
	xorq	$CR0_AM,%rax
	movq	%rax,%cr0

	/* Clear CR4.PSE and CR4.OSFXSR */
	movq	%cr4,%rbx
	andq	$~(CR4_PSE|CR4_OSFXSR),%rbx
	movq	%rbx,%cr4

	/* Set CR0.WP */
	movq	%cr0,%rcx
	orq	$CR0_WP,%rcx
	movq	%rcx,%cr0

	/* Set CR4.PGE */
	movq	%cr4,%rdx
	orq	$CR4_PGE,%rdx
	movq	%rdx,%cr4

	TEST_END
test_mov_cr_sequence_end:
