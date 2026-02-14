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

#define IO_SIZE	128

static char iobuf[IO_SIZE];
static size_t iobuf_off = 0;

/* -------------------------------------------------------------------------- */

static void
io_callback(struct nvmm_io *io)
{
	if (io->port != 123) {
		err(-1, "wrong port: %u", io->port);
	}

	printf("-> port = %u, size = %zu (%s)\n", io->port, io->size,
	    io->in ? "in" : "out");

	if (io->in) {
		memcpy(io->data, iobuf + iobuf_off, io->size);
	} else {
		memcpy(iobuf + iobuf_off, io->data, io->size);
	}
	iobuf_off += io->size;

}

static void
handle_io(struct test_machine *tmach, void *arg __unused)
{
	if (nvmm_assist_io(&tmach->mach, &tmach->vcpu) == -1) {
		err(errno, "nvmm_assist_io");
	}
}

/* -------------------------------------------------------------------------- */

struct test {
	const char *name;
	uint8_t *code_begin;
	uint8_t *code_end;
	const char *wanted;
	bool in;
};

static void
run_test(struct test_machine *tmach, const struct test *test)
{
	size_t size;
	char *res;

	size = (size_t)test->code_end - (size_t)test->code_begin;

	iobuf_off = 0;
	memset(iobuf, 0, sizeof(iobuf));
	memset(tmach->databuf, 0, PAGE_SIZE);
	memcpy(tmach->instbuf, test->code_begin, size);

	if (test->in) {
		strcpy(iobuf, test->wanted);
	} else {
		strcpy((char *)tmach->databuf, test->wanted);
	}

	run_machine(tmach, NULL);

	if (test->in) {
		res = (char *)tmach->databuf;
	} else {
		res = iobuf;
	}

	if (!strcmp(res, test->wanted)) {
		printf("Test '%s' passed\n", test->name);
	} else {
		printf("Test '%s' failed, wanted '%s', got '%s'\n",
		    test->name, test->wanted, res);
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

static const struct test tests[] = {
	{ "test1 - INB", &test1_begin, &test1_end, "12", true },
	{ "test2 - INW", &test2_begin, &test2_end, "1234", true },
	{ "test3 - INL", &test3_begin, &test3_end, "12345678", true },
	{ "test4 - INSB+REP", &test4_begin, &test4_end, "12345", true },
	{ "test5 - INSW+REP", &test5_begin, &test5_end,
	  "Comment est votre blanquette", true },
	{ "test6 - INSL+REP", &test6_begin, &test6_end,
	  "123456789abcdefghijklmnopqrs", true },
	{ "test7 - OUTB", &test7_begin, &test7_end, "12", false },
	{ "test8 - OUTW", &test8_begin, &test8_end, "1234", false },
	{ "test9 - OUTL", &test9_begin, &test9_end, "12345678", false },
	{ "test10 - OUTSB+REP", &test10_begin, &test10_end, "12345", false },
	{ "test11 - OUTSW+REP", &test11_begin, &test11_end,
	  "Ah, Herr Bramard", false },
	{ "test12 - OUTSL+REP", &test12_begin, &test12_end,
	  "123456789abcdefghijklmnopqrs", false },
	{ NULL, NULL, NULL, NULL, false }
};

int main(int argc __unused, char *argv[] __unused)
{
	struct test_machine tmach;
	size_t i;

	if (nvmm_init() == -1)
		err(errno, "nvmm_init");

	memset(&tmach, 0, sizeof(tmach));
	tmach.callbacks.io = io_callback;
	tmach.handle_io = handle_io;
	tmach.map_databuf = true;

	create_machine(&tmach);

	for (i = 0; tests[i].name != NULL; i++) {
		reset_machine(&tmach);
		run_test(&tmach, &tests[i]);
	}

	destroy_machine(&tmach);

	return 0;
}
