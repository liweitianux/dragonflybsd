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

#include <sys/types.h>
#include <sys/mman.h>
#include <err.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "h_os.h"
#include "h_common.h"

static void
reset_machine16(struct test_machine *tmach)
{
	struct nvmm_machine *mach = &tmach->mach;
	struct nvmm_vcpu *vcpu = &tmach->vcpu;
	struct nvmm_x64_state *state = vcpu->state;

	if (nvmm_vcpu_getstate(mach, vcpu, NVMM_X64_STATE_ALL) == -1)
		err(errno, "nvmm_vcpu_getstate");

	state->segs[NVMM_X64_SEG_CS].base = 0;
	state->segs[NVMM_X64_SEG_CS].limit = 0x2FFF;
	state->gprs[NVMM_X64_GPR_RIP] = 0x2000;

	if (nvmm_vcpu_setstate(mach, vcpu, NVMM_X64_STATE_ALL) == -1)
		err(errno, "nvmm_vcpu_setstate");
}

static void
map_pages16(struct test_machine *tmach)
{
	struct nvmm_machine *mach = &tmach->mach;
	uint8_t *instbuf;
	int ret;

	instbuf = mmap(NULL, PAGE_SIZE, PROT_READ|PROT_WRITE,
	    MAP_ANON|MAP_PRIVATE, -1, 0);
	if (instbuf == MAP_FAILED)
		err(errno, "mmap(instbuf)");

	if (nvmm_hva_map(mach, (uintptr_t)instbuf, PAGE_SIZE) == -1)
		err(errno, "nvmm_hva_map(instbuf)");
	ret = nvmm_gpa_map(mach, (uintptr_t)instbuf, 0x2000, PAGE_SIZE,
	    PROT_READ|PROT_EXEC);
	if (ret == -1)
		err(errno, "nvmm_gpa_map(instbuf)");

	tmach->instbuf = instbuf;
}

static void
init_seg(struct nvmm_x64_state_seg *seg, int type, int sel)
{
	seg->selector = sel;
	seg->attrib.type = type;
	seg->attrib.s = (type & 0b10000) != 0;
	seg->attrib.dpl = 0;
	seg->attrib.p = 1;
	seg->attrib.avl = 1;
	seg->attrib.l = 1;
	seg->attrib.def = 0;
	seg->attrib.g = 1;
	seg->limit = 0x0000FFFF;
	seg->base = 0x00000000;
}

void
reset_machine(struct test_machine *tmach)
{
	struct nvmm_machine *mach = &tmach->mach;
	struct nvmm_vcpu *vcpu = &tmach->vcpu;
	struct nvmm_x64_state *state = vcpu->state;

	if (tmach->is_16bit) {
		reset_machine16(tmach);
		return;
	}

	if (nvmm_vcpu_getstate(mach, vcpu, NVMM_X64_STATE_ALL) == -1)
		err(errno, "nvmm_vcpu_getstate");

	memset(state, 0, sizeof(*state));

	/* Default. */
	state->gprs[NVMM_X64_GPR_RFLAGS] = PSL_MBO;
	init_seg(&state->segs[NVMM_X64_SEG_CS], SDT_MEMERA,
	    GSEL(GCODE_SEL, SEL_KPL));
	init_seg(&state->segs[NVMM_X64_SEG_SS], SDT_MEMRWA,
	    GSEL(GDATA_SEL, SEL_KPL));
	init_seg(&state->segs[NVMM_X64_SEG_DS], SDT_MEMRWA,
	    GSEL(GDATA_SEL, SEL_KPL));
	init_seg(&state->segs[NVMM_X64_SEG_ES], SDT_MEMRWA,
	    GSEL(GDATA_SEL, SEL_KPL));
	init_seg(&state->segs[NVMM_X64_SEG_FS], SDT_MEMRWA,
	    GSEL(GDATA_SEL, SEL_KPL));
	init_seg(&state->segs[NVMM_X64_SEG_GS], SDT_MEMRWA,
	    GSEL(GDATA_SEL, SEL_KPL));

	/* Blank. */
	init_seg(&state->segs[NVMM_X64_SEG_GDT], 0, 0);
	init_seg(&state->segs[NVMM_X64_SEG_IDT], 0, 0);
	init_seg(&state->segs[NVMM_X64_SEG_LDT], SDT_SYSLDT, 0);
	init_seg(&state->segs[NVMM_X64_SEG_TR], SDT_SYS386BSY, 0);

	/* Protected mode enabled. */
	state->crs[NVMM_X64_CR_CR0] =
	    CR0_PG|CR0_PE|CR0_NE|CR0_TS|CR0_MP|CR0_WP|CR0_AM;

	/* 64bit mode enabled. */
	state->crs[NVMM_X64_CR_CR4] = CR4_PAE;
	state->msrs[NVMM_X64_MSR_EFER] = EFER_LME | EFER_SCE | EFER_LMA;

	state->msrs[NVMM_X64_MSR_PAT] = MSR_PAT_VALUE;

	/* Page tables. */
	state->crs[NVMM_X64_CR_CR3] = 0x3000;

	state->gprs[NVMM_X64_GPR_RIP] = 0x2000;

	if (nvmm_vcpu_setstate(mach, vcpu, NVMM_X64_STATE_ALL) == -1)
		err(errno, "nvmm_vcpu_setstate");
}

/*
 * 0x1000: Data (mapped if map_databuf=true)
 * 0x2000: Instructions, mapped
 * 0x3000: L4
 * 0x4000: L3
 * 0x5000: L2
 * 0x6000: L1
 */
static void
map_pages(struct test_machine *tmach)
{
	struct nvmm_machine *mach = &tmach->mach;
	pt_entry_t *L4, *L3, *L2, *L1;
	uint8_t *instbuf, *databuf;
	int ret;

	if (tmach->is_16bit) {
		map_pages16(tmach);
		return;
	}

	instbuf = mmap(NULL, PAGE_SIZE, PROT_READ|PROT_WRITE,
	    MAP_ANON|MAP_PRIVATE, -1, 0);
	if (instbuf == MAP_FAILED)
		err(errno, "mmap(instbuf)");
	if (nvmm_hva_map(mach, (uintptr_t)instbuf, PAGE_SIZE) == -1)
		err(errno, "nvmm_hva_map(instbuf)");
	ret = nvmm_gpa_map(mach, (uintptr_t)instbuf, 0x2000, PAGE_SIZE,
	    PROT_READ|PROT_EXEC);
	if (ret == -1)
		err(errno, "nvmm_gpa_map(instbuf)");

	if (tmach->map_databuf) {
		databuf = mmap(NULL, PAGE_SIZE, PROT_READ|PROT_WRITE,
		    MAP_ANON|MAP_PRIVATE, -1, 0);
		if (databuf == MAP_FAILED)
			err(errno, "mmap(databuf)");
		if (nvmm_hva_map(mach, (uintptr_t)databuf, PAGE_SIZE) == -1)
			err(errno, "nvmm_hva_map(databuf)");
		ret = nvmm_gpa_map(mach, (uintptr_t)databuf, 0x1000, PAGE_SIZE,
		    PROT_READ|PROT_WRITE);
		if (ret == -1)
			err(errno, "nvmm_gpa_map(databuf)");
	} else {
		databuf = NULL;
	}

	L4 = mmap(NULL, PAGE_SIZE, PROT_READ|PROT_WRITE, MAP_ANON|MAP_PRIVATE,
	    -1, 0);
	if (L4 == MAP_FAILED)
		err(errno, "mmap(L4)");
	L3 = mmap(NULL, PAGE_SIZE, PROT_READ|PROT_WRITE, MAP_ANON|MAP_PRIVATE,
	    -1, 0);
	if (L3 == MAP_FAILED)
		err(errno, "mmap(L3)");
	L2 = mmap(NULL, PAGE_SIZE, PROT_READ|PROT_WRITE, MAP_ANON|MAP_PRIVATE,
	    -1, 0);
	if (L2 == MAP_FAILED)
		err(errno, "mmap(L2)");
	L1 = mmap(NULL, PAGE_SIZE, PROT_READ|PROT_WRITE, MAP_ANON|MAP_PRIVATE,
	    -1, 0);
	if (L1 == MAP_FAILED)
		err(errno, "mmap(L1)");

	if (nvmm_hva_map(mach, (uintptr_t)L4, PAGE_SIZE) == -1)
		err(errno, "nvmm_hva_map(L4)");
	if (nvmm_hva_map(mach, (uintptr_t)L3, PAGE_SIZE) == -1)
		err(errno, "nvmm_hva_map(L3)");
	if (nvmm_hva_map(mach, (uintptr_t)L2, PAGE_SIZE) == -1)
		err(errno, "nvmm_hva_map(L2)");
	if (nvmm_hva_map(mach, (uintptr_t)L1, PAGE_SIZE) == -1)
		err(errno, "nvmm_hva_map(L1)");

	ret = nvmm_gpa_map(mach, (uintptr_t)L4, 0x3000, PAGE_SIZE,
	    PROT_READ|PROT_WRITE);
	if (ret == -1)
		err(errno, "nvmm_gpa_map(L4)");
	ret = nvmm_gpa_map(mach, (uintptr_t)L3, 0x4000, PAGE_SIZE,
	    PROT_READ|PROT_WRITE);
	if (ret == -1)
		err(errno, "nvmm_gpa_map(L3)");
	ret = nvmm_gpa_map(mach, (uintptr_t)L2, 0x5000, PAGE_SIZE,
	    PROT_READ|PROT_WRITE);
	if (ret == -1)
		err(errno, "nvmm_gpa_map(L2)");
	ret = nvmm_gpa_map(mach, (uintptr_t)L1, 0x6000, PAGE_SIZE,
	    PROT_READ|PROT_WRITE);
	if (ret == -1)
		err(errno, "nvmm_gpa_map(L1)");

	memset(L4, 0, PAGE_SIZE);
	memset(L3, 0, PAGE_SIZE);
	memset(L2, 0, PAGE_SIZE);
	memset(L1, 0, PAGE_SIZE);

	L4[0] = PTE_P | PTE_W | 0x4000;
	L3[0] = PTE_P | PTE_W | 0x5000;
	L2[0] = PTE_P | PTE_W | 0x6000;
	L1[0x2000 / PAGE_SIZE] = PTE_P | PTE_W | 0x2000; /* instructions */
	L1[0x1000 / PAGE_SIZE] = PTE_P | PTE_W | 0x1000; /* data */

	tmach->instbuf = instbuf;
	tmach->databuf = databuf;
}

void
create_machine(struct test_machine *tmach)
{
	struct nvmm_machine *mach = &tmach->mach;
	struct nvmm_vcpu *vcpu = &tmach->vcpu;
	int ret;

	if (nvmm_machine_create(mach) == -1)
		err(errno, "nvmm_machine_create");
	ret = nvmm_machine_configure(mach, NVMM_MACH_CONF_CR, &tmach->cr);
	if (ret == -1)
		err(errno, "nvmm_machine_configure");

	if (nvmm_vcpu_create(mach, 0, vcpu) == -1)
		err(errno, "nvmm_vcpu_create");
	ret = nvmm_vcpu_configure(mach, vcpu, NVMM_VCPU_CONF_CALLBACKS,
	    &tmach->callbacks);
	if (ret == -1)
		err(errno, "nvmm_vcpu_configure");

	map_pages(tmach);
}

void
destroy_machine(struct test_machine *tmach)
{
	struct nvmm_machine *mach = &tmach->mach;
	struct nvmm_vcpu *vcpu = &tmach->vcpu;

	if (nvmm_vcpu_destroy(mach, vcpu) == -1)
		err(errno, "nvmm_vcpu_destroy");
	if (nvmm_machine_destroy(mach) == -1)
		err(errno, "nvmm_machine_destroy");
}

void
run_machine(struct test_machine *tmach, void *arg)
{
	struct nvmm_machine *mach = &tmach->mach;
	struct nvmm_vcpu *vcpu = &tmach->vcpu;
	struct nvmm_vcpu_exit *exit = vcpu->exit;

	while (1) {
		if (nvmm_vcpu_run(mach, vcpu) == -1)
			err(errno, "nvmm_vcpu_run");

		switch (exit->reason) {
		case NVMM_VCPU_EXIT_NONE:
			break;

		case NVMM_VCPU_EXIT_RDMSR:
			/* Stop here. */
			return;

		case NVMM_VCPU_EXIT_IO:
			if (tmach->handle_io != NULL) {
				(*tmach->handle_io)(tmach, arg);
			} else {
				errx(-1, "unexpected I/O exit");
			}
			break;

		case NVMM_VCPU_EXIT_MEMORY:
			if (tmach->handle_memory != NULL) {
				(*tmach->handle_memory)(tmach, arg);
			} else {
				errx(-1, "unexpected memory exit");
			}
			break;

		case NVMM_VCPU_EXIT_INSN:
			if (tmach->handle_insn != NULL) {
				(*tmach->handle_insn)(tmach, arg);
			} else {
				errx(-1, "unexpected instruction exit");
			}
			break;

		case NVMM_VCPU_EXIT_SHUTDOWN:
			printf("Shutting down!\n");
			return;

		default:
			printf("Invalid VMEXIT: 0x%lx\n", exit->reason);
			return;
		}
	}
}
