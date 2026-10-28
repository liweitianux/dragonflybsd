/*
 * Copyright (c) 2026 The DragonFly Project.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of The DragonFly Project nor the names of its
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific, prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE
 * COPYRIGHT HOLDERS OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * Retpoline thunks for -mindirect-branch=thunk-extern.
 *
 * An indirect branch through a register is compiled to
 *
 *	call	__x86_indirect_thunk_<reg>
 *
 * with the branch target in <reg>.  The thunk performs the branch with a
 * "retpoline": a "ret" whose return address is overwritten with the target.
 * The processor predicts that "ret" to return to the value pushed by the
 * "call" below, which is the address of the "pause", so branch speculation
 * steered by a mistrained indirect branch is trapped in an infinite loop
 * instead of reaching an attacker controlled target.
 *
 * There is no thunk for %rsp: a branch target cannot live in the stack
 * pointer, so leaving it out turns a hypothetical code generation bug into a
 * link time error.
 *
 * Reference: "Retpoline: A Branch Target Injection Mitigation", Intel, 2018.
 */

.macro RETPOLINE_THUNK reg
	.p2align 5
	.globl	__x86_indirect_thunk_\reg
	.type	__x86_indirect_thunk_\reg,@function
__x86_indirect_thunk_\reg:
	call	2f
1:	pause
	lfence
	jmp	1b
2:	movq	%\reg, (%rsp)
	ret
	.size	__x86_indirect_thunk_\reg, . - __x86_indirect_thunk_\reg
.endm

	RETPOLINE_THUNK rax
	RETPOLINE_THUNK rbx
	RETPOLINE_THUNK rcx
	RETPOLINE_THUNK rdx
	RETPOLINE_THUNK rsi
	RETPOLINE_THUNK rdi
	RETPOLINE_THUNK rbp
	RETPOLINE_THUNK r8
	RETPOLINE_THUNK r9
	RETPOLINE_THUNK r10
	RETPOLINE_THUNK r11
	RETPOLINE_THUNK r12
	RETPOLINE_THUNK r13
	RETPOLINE_THUNK r14
	RETPOLINE_THUNK r15

/*
 * Used when the compiler cannot spare a register: it pushes the target and
 * jumps here, so at entry the target is on top of the stack and the return
 * address of the branch follows it.  Drop the address pushed by the "call"
 * below with "lea" and "ret" to the target; this leaves the stack exactly
 * as a direct call would, clobbers no register and preserves the flags.
 */
	.p2align 5
	.globl	__x86_indirect_thunk
	.type	__x86_indirect_thunk,@function
__x86_indirect_thunk:
	call	2f
1:	pause
	lfence
	jmp	1b
2:	leaq	8(%rsp), %rsp
	ret
	.size	__x86_indirect_thunk, . - __x86_indirect_thunk
