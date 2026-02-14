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

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <string.h>
#include <err.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/mman.h>

#include <nvmm.h>

#include "h_os.h"

static uint8_t *instbuf;

/* -------------------------------------------------------------------------- */

static int
handle_insn(struct nvmm_machine *mach, struct nvmm_vcpu *vcpu)
{
	int ret;

	ret = nvmm_assist_insn(mach, vcpu);
	if (ret == -1) {
		err(errno, "nvmm_assist_insn");
	}

	return 0;
}

static void
run_machine(struct nvmm_machine *mach, struct nvmm_vcpu *vcpu)
{
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

		case NVMM_VCPU_EXIT_INSN:
			handle_insn(mach, vcpu);
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

/* -------------------------------------------------------------------------- */

struct cr_test {
	const char *name;
	uint8_t *code_begin;
	uint8_t *code_end;
	uint64_t initial_cr0;
	uint64_t initial_cr4;
	uint64_t wanted_cr0;
	uint64_t wanted_cr4;
};

static void
run_cr_test(struct nvmm_machine *mach, struct nvmm_vcpu *vcpu,
    const struct cr_test *test)
{
	struct nvmm_x64_state *state = vcpu->state;
	uint64_t final_cr0, final_cr4;
	size_t size;

	size = (size_t)test->code_end - (size_t)test->code_begin;

	if (nvmm_vcpu_getstate(mach, vcpu, NVMM_X64_STATE_CRS) == -1)
		err(errno, "nvmm_vcpu_getstate");

	state->crs[NVMM_X64_CR_CR0] = test->initial_cr0;
	state->crs[NVMM_X64_CR_CR4] = test->initial_cr4;

	if (nvmm_vcpu_setstate(mach, vcpu, NVMM_X64_STATE_CRS) == -1)
		err(errno, "nvmm_vcpu_setstate");

	memcpy(instbuf, test->code_begin, size);

	run_machine(mach, vcpu);

	if (nvmm_vcpu_getstate(mach, vcpu, NVMM_X64_STATE_CRS) == -1)
		err(errno, "nvmm_vcpu_getstate");

	final_cr0 = state->crs[NVMM_X64_CR_CR0];
	final_cr4 = state->crs[NVMM_X64_CR_CR4];

	if (final_cr0 == test->wanted_cr0 && final_cr4 == test->wanted_cr4) {
		printf("Test '%s' passed\n", test->name);
	} else {
		printf("Test '%s' failed:\n", test->name);
		if (final_cr0 != test->wanted_cr0) {
			printf("  CR0: wanted 0x%lx, got 0x%lx\n",
			    test->wanted_cr0, final_cr0);
		}
		if (final_cr4 != test->wanted_cr4) {
			printf("  CR4: wanted 0x%lx, got 0x%lx\n",
			    test->wanted_cr4, final_cr4);
		}
		errx(-1, "run_cr_test failed");
	}
}

/* -------------------------------------------------------------------------- */

extern uint8_t test_lmsw_reg1_begin, test_lmsw_reg1_end;
extern uint8_t test_lmsw_reg2_begin, test_lmsw_reg2_end;
extern uint8_t test_lmsw_pe_begin, test_lmsw_pe_end;
extern uint8_t test_lmsw_mem_begin, test_lmsw_mem_end;
extern uint8_t test_lmsw_disp_begin, test_lmsw_disp_end;
extern uint8_t test_lmsw_sib_begin, test_lmsw_sib_end;
extern uint8_t test_lmsw_indirect_begin, test_lmsw_indirect_end;
extern uint8_t test_lmsw_riprel_begin, test_lmsw_riprel_end;
extern uint8_t test_mov_cr0_basic_begin, test_mov_cr0_basic_end;
extern uint8_t test_mov_cr0_rex_begin, test_mov_cr0_rex_end;
extern uint8_t test_mov_cr0_multi_begin, test_mov_cr0_multi_end;
extern uint8_t test_mov_cr4_basic_begin, test_mov_cr4_basic_end;
extern uint8_t test_mov_cr4_pge_begin, test_mov_cr4_pge_end;
extern uint8_t test_mov_cr_sequence_begin, test_mov_cr_sequence_end;

static const struct cr_test cr_tests[] = {
	{
		.name = "LMSW - register AX",
		.code_begin = &test_lmsw_reg1_begin,
		.code_end = &test_lmsw_reg1_end,
		.initial_cr0 = CR0_PG|CR0_NE|CR0_ET|CR0_MP|CR0_PE,
		.initial_cr4 = CR4_PAE,
		.wanted_cr0 = CR0_PG|CR0_NE|CR0_ET|CR0_MP|CR0_PE | CR0_TS,
		.wanted_cr4 = CR4_PAE,
	},
	{
		.name = "LMSW - register CX",
		.code_begin = &test_lmsw_reg2_begin,
		.code_end = &test_lmsw_reg2_end,
		.initial_cr0 = CR0_PG|CR0_NE|CR0_ET|CR0_MP|CR0_PE,
		.initial_cr4 = CR4_PAE,
		.wanted_cr0 = CR0_PG|CR0_NE|CR0_ET|CR0_MP|CR0_PE | CR0_TS,
		.wanted_cr4 = CR4_PAE,
	},
	{
		.name = "LMSW - cannot clear PE",
		.code_begin = &test_lmsw_pe_begin,
		.code_end = &test_lmsw_pe_end,
		.initial_cr0 = CR0_PG|CR0_NE|CR0_ET|CR0_MP|CR0_PE,
		.initial_cr4 = CR4_PAE,
		.wanted_cr0 = CR0_PG|CR0_NE|CR0_ET|CR0_PE,
		.wanted_cr4 = CR4_PAE,
	},
	{
		.name = "LMSW - memory operand",
		.code_begin = &test_lmsw_mem_begin,
		.code_end = &test_lmsw_mem_end,
		.initial_cr0 = CR0_PG|CR0_NE|CR0_ET|CR0_MP|CR0_PE,
		.initial_cr4 = CR4_PAE,
		.wanted_cr0 = CR0_PG|CR0_NE|CR0_ET|CR0_MP|CR0_PE | CR0_TS,
		.wanted_cr4 = CR4_PAE,
	},
	{
		.name = "LMSW - memory displacement",
		.code_begin = &test_lmsw_disp_begin,
		.code_end = &test_lmsw_disp_end,
		.initial_cr0 = CR0_PG|CR0_NE|CR0_ET|CR0_MP|CR0_PE,
		.initial_cr4 = CR4_PAE,
		.wanted_cr0 = CR0_PG|CR0_NE|CR0_ET|CR0_MP|CR0_PE | CR0_TS,
		.wanted_cr4 = CR4_PAE,
	},
	{
		.name = "LMSW - memory SIB",
		.code_begin = &test_lmsw_sib_begin,
		.code_end = &test_lmsw_sib_end,
		.initial_cr0 = CR0_PG|CR0_NE|CR0_ET|CR0_MP|CR0_PE,
		.initial_cr4 = CR4_PAE,
		.wanted_cr0 = CR0_PG|CR0_NE|CR0_ET|CR0_MP|CR0_PE | CR0_TS,
		.wanted_cr4 = CR4_PAE,
	},
	{
		.name = "LMSW - indirect memory",
		.code_begin = &test_lmsw_indirect_begin,
		.code_end = &test_lmsw_indirect_end,
		.initial_cr0 = CR0_PG|CR0_NE|CR0_ET|CR0_PE,
		.initial_cr4 = CR4_PAE,
		.wanted_cr0 = CR0_PG|CR0_NE|CR0_ET|CR0_PE | CR0_TS|CR0_MP,
		.wanted_cr4 = CR4_PAE,
	},
	{
		.name = "LMSW - RIP-relative",
		.code_begin = &test_lmsw_riprel_begin,
		.code_end = &test_lmsw_riprel_end,
		.initial_cr0 = CR0_PG|CR0_NE|CR0_ET|CR0_PE,
		.initial_cr4 = CR4_PAE,
		.wanted_cr0 = CR0_PG|CR0_NE|CR0_ET|CR0_PE | CR0_TS|CR0_MP,
		.wanted_cr4 = CR4_PAE,
	},

	{
		.name = "MOV CR0 - RAX (basic)",
		.code_begin = &test_mov_cr0_basic_begin,
		.code_end = &test_mov_cr0_basic_end,
		.initial_cr0 = CR0_PG|CR0_NE|CR0_ET|CR0_PE,
		.initial_cr4 = CR4_PAE,
		.wanted_cr0 = CR0_PG|CR0_NE|CR0_ET|CR0_PE | CR0_WP|CR0_AM,
		.wanted_cr4 = CR4_PAE,
	},
	{
		.name = "MOV CR0 - R15 (with REX prefix)",
		.code_begin = &test_mov_cr0_rex_begin,
		.code_end = &test_mov_cr0_rex_end,
		.initial_cr0 = CR0_PG|CR0_NE|CR0_ET|CR0_PE,
		.initial_cr4 = CR4_PAE,
		.wanted_cr0 = CR0_PG|CR0_NE|CR0_ET|CR0_PE | CR0_WP|CR0_AM,
		.wanted_cr4 = CR4_PAE,
	},
	{
		.name = "MOV CR0 - multiple movs",
		.code_begin = &test_mov_cr0_multi_begin,
		.code_end = &test_mov_cr0_multi_end,
		.initial_cr0 = CR0_PG|CR0_NE|CR0_ET|CR0_PE,
		.initial_cr4 = CR4_PAE,
		.wanted_cr0 = CR0_PG|CR0_NE|CR0_ET|CR0_PE | CR0_AM,
		.wanted_cr4 = CR4_PAE,
	},

	{
		.name = "MOV CR4 - RAX (basic)",
		.code_begin = &test_mov_cr4_basic_begin,
		.code_end = &test_mov_cr4_basic_end,
		.initial_cr0 = CR0_PG|CR0_NE|CR0_ET|CR0_PE,
		.initial_cr4 = CR4_PAE,
		.wanted_cr0 = CR0_PG|CR0_NE|CR0_ET|CR0_PE,
		.wanted_cr4 = CR4_PAE | CR4_PSE|CR4_OSFXSR,
	},
	{
		.name = "MOV CR4 - RBX (PGE bit)",
		.code_begin = &test_mov_cr4_pge_begin,
		.code_end = &test_mov_cr4_pge_end,
		.initial_cr0 = CR0_PG|CR0_NE|CR0_ET|CR0_PE,
		.initial_cr4 = CR4_PAE,
		.wanted_cr0 = CR0_PG|CR0_NE|CR0_ET|CR0_PE,
		.wanted_cr4 = CR4_PAE | CR4_PGE,
	},

	{
		.name = "MOV CR - alternating operations",
		.code_begin = &test_mov_cr_sequence_begin,
		.code_end = &test_mov_cr_sequence_end,
		.initial_cr0 = CR0_PG|CR0_NE|CR0_ET|CR0_PE | CR0_AM,
		.initial_cr4 = CR4_PAE | CR4_PSE|CR4_OSFXSR,
		.wanted_cr0 = CR0_PG|CR0_NE|CR0_ET|CR0_PE | CR0_WP,
		.wanted_cr4 = CR4_PAE | CR4_PGE,
	},

	{ NULL, NULL, NULL, 0, 0, 0, 0 },
};

/* -------------------------------------------------------------------------- */

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

static void
reset_machine64(struct nvmm_machine *mach, struct nvmm_vcpu *vcpu)
{
	struct nvmm_x64_state *state = vcpu->state;

	if (nvmm_vcpu_getstate(mach, vcpu, NVMM_X64_STATE_ALL) == -1)
		err(errno, "nvmm_vcpu_getstate");

	memset(state, 0, sizeof(*state));

	/* Default. */
	state->gprs[NVMM_X64_GPR_RFLAGS] = PSL_MBO;
	init_seg(&state->segs[NVMM_X64_SEG_CS], SDT_MEMERA, GSEL(GCODE_SEL, SEL_KPL));
	init_seg(&state->segs[NVMM_X64_SEG_SS], SDT_MEMRWA, GSEL(GDATA_SEL, SEL_KPL));
	init_seg(&state->segs[NVMM_X64_SEG_DS], SDT_MEMRWA, GSEL(GDATA_SEL, SEL_KPL));
	init_seg(&state->segs[NVMM_X64_SEG_ES], SDT_MEMRWA, GSEL(GDATA_SEL, SEL_KPL));
	init_seg(&state->segs[NVMM_X64_SEG_FS], SDT_MEMRWA, GSEL(GDATA_SEL, SEL_KPL));
	init_seg(&state->segs[NVMM_X64_SEG_GS], SDT_MEMRWA, GSEL(GDATA_SEL, SEL_KPL));

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
 * 0x1000: Data, mapped
 * 0x2000: Instructions, mapped
 * 0x3000: L4
 * 0x4000: L3
 * 0x5000: L2
 * 0x6000: L1
 */
static void
map_pages64(struct nvmm_machine *mach)
{
	pt_entry_t *L4, *L3, *L2, *L1;
	uint8_t *databuf;
	int ret;

	/* Map data page for memory operand tests */
	databuf = mmap(NULL, PAGE_SIZE, PROT_READ|PROT_WRITE, MAP_ANON|MAP_PRIVATE,
	    -1, 0);
	if (databuf == MAP_FAILED)
		err(errno, "mmap");

	if (nvmm_hva_map(mach, (uintptr_t)databuf, PAGE_SIZE) == -1)
		err(errno, "nvmm_hva_map");
	ret = nvmm_gpa_map(mach, (uintptr_t)databuf, 0x1000, PAGE_SIZE,
	    PROT_READ|PROT_WRITE);
	if (ret == -1)
		err(errno, "nvmm_gpa_map");

	/* Map instruction page */
	instbuf = mmap(NULL, PAGE_SIZE, PROT_READ|PROT_WRITE, MAP_ANON|MAP_PRIVATE,
	    -1, 0);
	if (instbuf == MAP_FAILED)
		err(errno, "mmap");

	if (nvmm_hva_map(mach, (uintptr_t)instbuf, PAGE_SIZE) == -1)
		err(errno, "nvmm_hva_map");
	ret = nvmm_gpa_map(mach, (uintptr_t)instbuf, 0x2000, PAGE_SIZE,
	    PROT_READ|PROT_EXEC);
	if (ret == -1)
		err(errno, "nvmm_gpa_map");

	/* Map page tables */
	L4 = mmap(NULL, PAGE_SIZE, PROT_READ|PROT_WRITE, MAP_ANON|MAP_PRIVATE,
	    -1, 0);
	if (L4 == MAP_FAILED)
		err(errno, "mmap");
	L3 = mmap(NULL, PAGE_SIZE, PROT_READ|PROT_WRITE, MAP_ANON|MAP_PRIVATE,
	    -1, 0);
	if (L3 == MAP_FAILED)
		err(errno, "mmap");
	L2 = mmap(NULL, PAGE_SIZE, PROT_READ|PROT_WRITE, MAP_ANON|MAP_PRIVATE,
	    -1, 0);
	if (L2 == MAP_FAILED)
		err(errno, "mmap");
	L1 = mmap(NULL, PAGE_SIZE, PROT_READ|PROT_WRITE, MAP_ANON|MAP_PRIVATE,
	    -1, 0);
	if (L1 == MAP_FAILED)
		err(errno, "mmap");

	if (nvmm_hva_map(mach, (uintptr_t)L4, PAGE_SIZE) == -1)
		err(errno, "nvmm_hva_map");
	if (nvmm_hva_map(mach, (uintptr_t)L3, PAGE_SIZE) == -1)
		err(errno, "nvmm_hva_map");
	if (nvmm_hva_map(mach, (uintptr_t)L2, PAGE_SIZE) == -1)
		err(errno, "nvmm_hva_map");
	if (nvmm_hva_map(mach, (uintptr_t)L1, PAGE_SIZE) == -1)
		err(errno, "nvmm_hva_map");

	ret = nvmm_gpa_map(mach, (uintptr_t)L4, 0x3000, PAGE_SIZE,
	    PROT_READ|PROT_WRITE);
	if (ret == -1)
		err(errno, "nvmm_gpa_map");
	ret = nvmm_gpa_map(mach, (uintptr_t)L3, 0x4000, PAGE_SIZE,
	    PROT_READ|PROT_WRITE);
	if (ret == -1)
		err(errno, "nvmm_gpa_map");
	ret = nvmm_gpa_map(mach, (uintptr_t)L2, 0x5000, PAGE_SIZE,
	    PROT_READ|PROT_WRITE);
	if (ret == -1)
		err(errno, "nvmm_gpa_map");
	ret = nvmm_gpa_map(mach, (uintptr_t)L1, 0x6000, PAGE_SIZE,
	    PROT_READ|PROT_WRITE);
	if (ret == -1)
		err(errno, "nvmm_gpa_map");

	memset(L4, 0, PAGE_SIZE);
	memset(L3, 0, PAGE_SIZE);
	memset(L2, 0, PAGE_SIZE);
	memset(L1, 0, PAGE_SIZE);

	L4[0] = PTE_P | PTE_W | 0x4000;
	L3[0] = PTE_P | PTE_W | 0x5000;
	L2[0] = PTE_P | PTE_W | 0x6000;
	L1[0x2000 / PAGE_SIZE] = PTE_P | PTE_W | 0x2000;
	L1[0x1000 / PAGE_SIZE] = PTE_P | PTE_W | 0x1000;  /* Data */
}

static void
test_cr_instructions(void)
{
	struct nvmm_machine mach;
	struct nvmm_vcpu vcpu;
	size_t i;

	if (nvmm_machine_create(&mach) == -1)
		err(errno, "nvmm_machine_create");
	if (nvmm_vcpu_create(&mach, 0, &vcpu) == -1)
		err(errno, "nvmm_vcpu_create");

	map_pages64(&mach);

	for (i = 0; cr_tests[i].name != NULL; i++) {
		reset_machine64(&mach, &vcpu);
		run_cr_test(&mach, &vcpu, &cr_tests[i]);
	}

	if (nvmm_vcpu_destroy(&mach, &vcpu) == -1)
		err(errno, "nvmm_vcpu_destroy");
	if (nvmm_machine_destroy(&mach) == -1)
		err(errno, "nvmm_machine_destroy");
}

/* -------------------------------------------------------------------------- */

int main(int argc, char *argv[])
{
	if (nvmm_init() == -1)
		err(errno, "nvmm_init");

	test_cr_instructions();

	return 0;
}
