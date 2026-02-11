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

#include <machine/segments.h>
#include <machine/psl.h>

#ifdef __DragonFly__
#define	SDT_SYS386BSY	SDT_SYSBSY	/* 11: system 64-bit TSS busy */
#define	PSL_MBO		PSL_RESERVED_DEFAULT	/* 0x00000002: reserved flags */
#endif

#define	PAGE_SIZE	4096

typedef	uint64_t	pt_entry_t;
#define	PTE_P		0x001	/* P: Valid */
#define	PTE_W		0x002	/* R/W: Read/Write */

/* Stolen from x86/pmap.c */
#define	PATENTRY(n, type)	(type << ((n) * 8))
#define	PAT_UC		0x0ULL
#define	PAT_WC		0x1ULL
#define	PAT_WT		0x4ULL
#define	PAT_WP		0x5ULL
#define	PAT_WB		0x6ULL
#define	PAT_UCMINUS	0x7ULL
#define	MSR_PAT_VALUE	\
	(PATENTRY(0, PAT_WB) | \
	 PATENTRY(1, PAT_WT) | \
	 PATENTRY(2, PAT_UCMINUS) | \
	 PATENTRY(3, PAT_UC) | \
	 PATENTRY(4, PAT_WB) | \
	 PATENTRY(5, PAT_WT) | \
	 PATENTRY(6, PAT_UCMINUS) | \
	 PATENTRY(7, PAT_UC))
