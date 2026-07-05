/*
 * Minimal PVH test guest for the microvm monitor.
 *
 * An ELF64 image whose XEN_ELFNOTE_PHYS32_ENTRY note points at 32-bit code.
 * The monitor enters here in 32-bit protected mode, paging off, with %ebx
 * holding the physical address of the hvm_start_info block. We validate the
 * magic, echo the kernel command line to COM1 (0x3f8), and halt.
 *
 * Copyright (c) 2026 The DragonFly Project.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE
 * COPYRIGHT HOLDERS OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
 * OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#define XEN_HVM_START_MAGIC	0x336ec578
#define SI_CMDLINE_OFF		24	/* offsetof(hvm_start_info, cmdline_paddr) */
#define COM1			0x3f8

	.code32
	.section .text
	.globl _start
_start:
	movl	$0x90000, %esp		/* our own stack in low RAM */
	movl	%ebx, %ebp		/* save &hvm_start_info */

	movl	$banner, %esi
	call	puts

	movl	0(%ebp), %eax		/* hvm_start_info.magic */
	cmpl	$XEN_HVM_START_MAGIC, %eax
	jne	1f
	movl	$okmsg, %esi
	call	puts
	jmp	2f
1:
	movl	$badmsg, %esi
	call	puts
2:
	movl	$cmdmsg, %esi
	call	puts
	movl	SI_CMDLINE_OFF(%ebp), %esi	/* cmdline_paddr (low 32) */
	call	puts
	movl	$nl, %esi
	call	puts

3:
	hlt
	jmp	3b

/* puts: write the NUL-terminated string at %esi to COM1. */
puts:
	movw	$COM1, %dx
.Lputs_loop:
	movb	(%esi), %al
	testb	%al, %al
	jz	.Lputs_done
	outb	%al, %dx
	incl	%esi
	jmp	.Lputs_loop
.Lputs_done:
	ret

	.section .rodata
banner:	.asciz	"microvm: PVH guest alive\n"
okmsg:	.asciz	"microvm: start_info magic OK\n"
badmsg:	.asciz	"microvm: start_info magic BAD\n"
cmdmsg:	.asciz	"microvm: cmdline="
nl:	.asciz	"\n"

/* PVH entry ELF note: name "Xen", type 18, desc = 32-bit entry point. */
	.section .note.xen, "a", @note
	.align	4
	.long	2f - 1f			/* n_namesz */
	.long	4f - 3f			/* n_descsz */
	.long	18			/* XEN_ELFNOTE_PHYS32_ENTRY */
1:	.asciz	"Xen"
2:	.align	4
3:	.long	_start
4:	.align	4
