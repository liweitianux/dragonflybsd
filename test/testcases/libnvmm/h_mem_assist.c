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

static uint8_t mmiobuf[PAGE_SIZE]; /* GPA 0x1000, unmapped */

/* -------------------------------------------------------------------------- */

static void
mem_callback(struct nvmm_mem *mem)
{
	size_t off;

	if (mem->gpa < 0x1000 || mem->gpa + mem->size > 0x1000 + PAGE_SIZE) {
		err(-1, "out of page: gpa = %p, size = %zu",
		    (void *)mem->gpa, mem->size);
	}

	off = mem->gpa - 0x1000;

	printf("-> gpa = %p, size = %zu (%s)\n", (void *)mem->gpa, mem->size,
	    mem->write ? "write" : "read");

	if (mem->write) {
		memcpy(mmiobuf + off, mem->data, mem->size);
	} else {
		memcpy(mem->data, mmiobuf + off, mem->size);
	}
}

static void
handle_memory(struct test_machine *tmach, void *arg __unused)
{
	if (nvmm_assist_mem(&tmach->mach, &tmach->vcpu) == -1) {
		err(errno, "nvmm_assist_mem");
	}
}

/* -------------------------------------------------------------------------- */

struct test {
	const char *name;
	uint8_t *code_begin;
	uint8_t *code_end;
	uint64_t wanted;
	uint64_t off;
};

static void
run_test(struct test_machine *tmach, const struct test *test)
{
	uint64_t *res;
	size_t size;

	size = (size_t)test->code_end - (size_t)test->code_begin;

	memset(mmiobuf, 0, sizeof(mmiobuf));
	memcpy(tmach->instbuf, test->code_begin, size);

	run_machine(tmach, NULL);

	res = (uint64_t *)(mmiobuf + test->off);
	if (*res == test->wanted) {
		printf("Test '%s' passed\n", test->name);
	} else {
		printf("Test '%s' failed, wanted 0x%lx, got 0x%lx\n",
		    test->name, test->wanted, *res);
		errx(-1, "run_test failed");
	}
}

/* -------------------------------------------------------------------------- */

extern uint8_t test1_begin, test1_end;
extern uint8_t test2_begin, test2_end;
extern uint8_t test3_begin, test3_end;
extern uint8_t test4_begin, test4_end;
extern uint8_t test5_begin, test5_end;
extern uint8_t test6_begin, test6_end;
extern uint8_t test7_begin, test7_end;
extern uint8_t test8_begin, test8_end;
extern uint8_t test9_begin, test9_end;
extern uint8_t test10_begin, test10_end;
extern uint8_t test11_begin, test11_end;
extern uint8_t test12_begin, test12_end;
extern uint8_t test13_begin, test13_end;
extern uint8_t test14_begin, test14_end;
extern uint8_t test_64bit_15_begin, test_64bit_15_end;
extern uint8_t test_64bit_16_begin, test_64bit_16_end;
extern uint8_t test_64bit_17_begin, test_64bit_17_end;
extern uint8_t test_64bit_18_begin, test_64bit_18_end;

static const struct test tests64[] = {
	{ "64bit test1 - MOV", &test1_begin, &test1_end, 0x3004, 0 },
	{ "64bit test2 - OR",  &test2_begin, &test2_end, 0x16FF, 0 },
	{ "64bit test3 - AND", &test3_begin, &test3_end, 0x1FC0, 0 },
	{ "64bit test4 - XOR", &test4_begin, &test4_end, 0x10CF, 0 },
	{ "64bit test5 - Address Sizes", &test5_begin, &test5_end, 0x1F00, 0 },
	{ "64bit test6 - DMO", &test6_begin, &test6_end, 0xFFAB, 0 },
	{ "64bit test7 - STOS", &test7_begin, &test7_end, 0x00123456, 0 },
	{ "64bit test8 - LODS", &test8_begin, &test8_end, 0x12345678, 0 },
	{ "64bit test9 - MOVS", &test9_begin, &test9_end, 0x12345678, 0 },
	{ "64bit test10 - MOVZXB", &test10_begin, &test10_end, 0x00000078, 0 },
	{ "64bit test11 - MOVZXW", &test11_begin, &test11_end, 0x00005678, 0 },
	{ "64bit test12 - CMP", &test12_begin, &test12_end, 0x00000001, 0 },
	{ "64bit test13 - SUB", &test13_begin, &test13_end,
	  0x0000000F0000A0FF, 0 },
	{ "64bit test14 - TEST", &test14_begin, &test14_end, 0x00000001, 0 },
	{ "64bit test15 - XCHG", &test_64bit_15_begin, &test_64bit_15_end,
	  0x123456, 0 },
	{ "64bit test16 - XCHG", &test_64bit_16_begin, &test_64bit_16_end,
	  0x123456, 0 },
	{ "64bit test17 - RIP-relative MOV",
	  &test_64bit_17_begin, &test_64bit_17_end, 0xAB1234, 0 },
	{ "64bit test18 - RIP-relative OR",
	  &test_64bit_18_begin, &test_64bit_18_end, 0xFFAB, 0 },
	{ NULL, NULL, NULL, -1, 0 }
};

static void
test_vm64(void)
{
	struct test_machine tmach;
	size_t i;

	memset(&tmach, 0, sizeof(tmach));
	tmach.callbacks.mem = mem_callback;
	tmach.handle_memory = handle_memory;

	create_machine(&tmach);

	for (i = 0; tests64[i].name != NULL; i++) {
		reset_machine(&tmach);
		run_test(&tmach, &tests64[i]);
	}

	destroy_machine(&tmach);
}

/* -------------------------------------------------------------------------- */

extern uint8_t test_16bit_1_begin, test_16bit_1_end;
extern uint8_t test_16bit_2_begin, test_16bit_2_end;
extern uint8_t test_16bit_3_begin, test_16bit_3_end;
extern uint8_t test_16bit_4_begin, test_16bit_4_end;
extern uint8_t test_16bit_5_begin, test_16bit_5_end;
extern uint8_t test_16bit_6_begin, test_16bit_6_end;

static const struct test tests16[] = {
	{ "16bit test1 - MOV single", &test_16bit_1_begin, &test_16bit_1_end,
	  0x023, 0x10f1 - 0x1000 },
	{ "16bit test2 - MOV dual", &test_16bit_2_begin, &test_16bit_2_end,
	  0x123, 0x10f3 - 0x1000 },
	{ "16bit test3 - MOV dual+disp", &test_16bit_3_begin, &test_16bit_3_end,
	  0x678, 0x10f1 - 0x1000 },
	{ "16bit test4 - Mixed", &test_16bit_4_begin, &test_16bit_4_end,
	  0x1011, 0x10f6 - 0x1000 },
	{ "16bit test5 - disp16-only", &test_16bit_5_begin, &test_16bit_5_end,
	  0x12, 0x1234 - 0x1000 },
	{ "16bit test6 - XCHG", &test_16bit_6_begin, &test_16bit_6_end,
	  0x1234, 0x1234 - 0x1000 },
	{ NULL, NULL, NULL, -1, -1 }
};

static void
test_vm16(void)
{
	struct test_machine tmach;
	size_t i;

	memset(&tmach, 0, sizeof(tmach));
	tmach.is_16bit = true;
	tmach.callbacks.mem = mem_callback;
	tmach.handle_memory = handle_memory;

	create_machine(&tmach);

	for (i = 0; tests16[i].name != NULL; i++) {
		reset_machine(&tmach);
		run_test(&tmach, &tests16[i]);
	}

	destroy_machine(&tmach);
}

/* -------------------------------------------------------------------------- */

int main(int argc __unused, char *argv[] __unused)
{
	if (nvmm_init() == -1)
		err(errno, "nvmm_init");
	test_vm64();
	test_vm16();
	return 0;
}
