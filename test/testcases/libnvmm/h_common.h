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

#ifndef NVMM_H_COMMON_H_
#define NVMM_H_COMMON_H_

#include <stdbool.h>
#include <stdint.h>
#include <nvmm.h>

struct test_machine {
	bool is_16bit;
	struct nvmm_machine mach;
	struct nvmm_vcpu vcpu;
	struct nvmm_mach_conf_cr cr;
	struct nvmm_assist_callbacks callbacks;
	void (*handle_io)(struct test_machine *, void *);
	void (*handle_memory)(struct test_machine *, void *);
	void (*handle_insn)(struct test_machine *, void *);
	uint8_t *instbuf; /* instructions; mapped at GPA 0x2000 */
	uint8_t *databuf; /* data; mapped at GPA 0x1000 */
	bool map_databuf; /* whether to map databuf? */
};

void create_machine(struct test_machine *);
void destroy_machine(struct test_machine *);
void reset_machine(struct test_machine *);
void run_machine(struct test_machine *, void *);

#endif /* NVMM_H_COMMON_H_ */
