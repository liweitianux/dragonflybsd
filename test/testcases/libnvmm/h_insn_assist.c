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
#include <string.h>
#include <err.h>
#include <errno.h>

#include <nvmm.h>

#include "h_os.h"
#include "h_common.h"

/* -------------------------------------------------------------------------- */

static void
handle_insn(struct test_machine *tmach, void *arg)
{
	int *hits = arg;

	(*hits)++;
	if (nvmm_assist_insn(&tmach->mach, &tmach->vcpu) == -1) {
		err(errno, "nvmm_assist_insn");
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
	int hits;
};

static void
run_cr_test(struct test_machine *tmach, struct cr_test *test)
{
	struct nvmm_machine *mach = &tmach->mach;
	struct nvmm_vcpu *vcpu = &tmach->vcpu;
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

	memcpy(tmach->instbuf, test->code_begin, size);

	run_machine(tmach, (void *)&test->hits);

	if (nvmm_vcpu_getstate(mach, vcpu, NVMM_X64_STATE_CRS) == -1)
		err(errno, "nvmm_vcpu_getstate");

	final_cr0 = state->crs[NVMM_X64_CR_CR0];
	final_cr4 = state->crs[NVMM_X64_CR_CR4];

	if (test->hits == 0) {
		printf("Test '%s' not hit\n", test->name);
		errx(-1, "%s failed", __func__);
	} else if (final_cr0 == test->wanted_cr0 &&
		   final_cr4 == test->wanted_cr4) {
		printf("Test '%s' passed (hits=%d)\n", test->name, test->hits);
	} else {
		printf("Test '%s' failed:\n", test->name);
		printf("  Hits: %d\n", test->hits);
		if (final_cr0 != test->wanted_cr0) {
			printf("  CR0: wanted 0x%lx, got 0x%lx\n",
			    test->wanted_cr0, final_cr0);
		}
		if (final_cr4 != test->wanted_cr4) {
			printf("  CR4: wanted 0x%lx, got 0x%lx\n",
			    test->wanted_cr4, final_cr4);
		}
		errx(-1, "%s failed", __func__);
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

static struct cr_test cr_tests[] = {
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

	{ NULL, NULL, NULL, 0, 0, 0, 0, 0 },
};

static void
test_cr_instructions(void)
{
	struct test_machine tmach;
	size_t i;

	memset(&tmach, 0, sizeof(tmach));
	tmach.handle_insn = handle_insn;
	tmach.cr.cr0_user = 1;
	tmach.cr.cr4_user = 1;
	tmach.map_databuf = true;

	create_machine(&tmach);

	for (i = 0; cr_tests[i].name != NULL; i++) {
		reset_machine(&tmach);
		run_cr_test(&tmach, &cr_tests[i]);
	}

	destroy_machine(&tmach);
}

/* -------------------------------------------------------------------------- */

int main(int argc __unused, char *argv[] __unused)
{
	if (nvmm_init() == -1)
		err(errno, "nvmm_init");

	test_cr_instructions();

	return 0;
}
