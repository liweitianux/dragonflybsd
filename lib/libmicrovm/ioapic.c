/*
 * Minimal I/O APIC (82093AA). See ioapic.h.
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

#include <string.h>

#include "vmm_internal.h"
#include "ioapic.h"
#include "lapic.h"

/*
 * IOAPIC window and redirection-table layout, mirroring
 * <machine_base/apic/apicreg.h> (kernel-only, not installed to /usr/include).
 */
#define IOREGSEL	0x00
#define IOWIN		0x10

#define IOAPIC_REG_ID	0x00
#define IOAPIC_REG_VER	0x01
#define IOAPIC_REG_ARB	0x02
#define IOAPIC_REG_RTE0	0x10	/* RTEs occupy 0x10..0x3f (2 regs each) */

/* Redirection table entry fields (low dword). */
#define RTE_VECTOR	0x000000ff
#define RTE_MASK	0x00010000
/* Destination is the high dword (bits 56..63): physical-mode APIC id. */
#define RTE_DEST(rte)	((int)(((rte) >> 56) & 0xff))

struct ioapic {
	uint32_t id;
	uint32_t regsel;
	uint64_t rte[IOAPIC_NPINS];
};

static struct ioapic io;

void
ioapic_init(void)
{
	int i;

	memset(&io, 0, sizeof(io));
	io.id = IOAPIC_ID << 24;
	for (i = 0; i < IOAPIC_NPINS; i++)
		io.rte[i] = RTE_MASK;		/* all masked at reset */
}

static uint32_t
reg_read(uint32_t reg)
{
	switch (reg) {
	case IOAPIC_REG_ID:
		return io.id;
	case IOAPIC_REG_VER:
		/* version 0x11, max redirection entry = NPINS-1 in bits 23:16 */
		return 0x00000011 | ((IOAPIC_NPINS - 1) << 16);
	case IOAPIC_REG_ARB:
		return io.id;
	default:
		if (reg >= IOAPIC_REG_RTE0 &&
		    reg < IOAPIC_REG_RTE0 + IOAPIC_NPINS * 2) {
			int idx = (reg - IOAPIC_REG_RTE0) >> 1;
			if (reg & 1)
				return (uint32_t)(io.rte[idx] >> 32);
			return (uint32_t)io.rte[idx];
		}
		return 0;
	}
}

static void
reg_write(uint32_t reg, uint32_t val)
{
	switch (reg) {
	case IOAPIC_REG_ID:
		io.id = val & 0x0f000000;
		break;
	default:
		if (reg >= IOAPIC_REG_RTE0 &&
		    reg < IOAPIC_REG_RTE0 + IOAPIC_NPINS * 2) {
			int idx = (reg - IOAPIC_REG_RTE0) >> 1;
			if (reg & 1)
				io.rte[idx] = (io.rte[idx] & 0xffffffffULL) |
				    ((uint64_t)val << 32);
			else
				io.rte[idx] = (io.rte[idx] & ~0xffffffffULL) |
				    val;
		}
		break;
	}
}

bool
ioapic_mmio(uint64_t gpa, bool write, uint8_t *data, size_t size)
{
	uint32_t off, val;

	if (gpa < IOAPIC_BASE || gpa >= IOAPIC_BASE + IOAPIC_SIZE)
		return false;

	off = (uint32_t)(gpa - IOAPIC_BASE);
	if (write) {
		val = 0;
		memcpy(&val, data, size > 4 ? 4 : size);
		if (off == IOREGSEL)
			io.regsel = val;
		else if (off == IOWIN)
			reg_write(io.regsel, val);
	} else {
		if (off == IOREGSEL)
			val = io.regsel;
		else if (off == IOWIN)
			val = reg_read(io.regsel);
		else
			val = 0;
		memset(data, 0, size);
		memcpy(data, &val, size > 4 ? 4 : size);
	}
	return true;
}

void
ioapic_raise(int gsi)
{
	uint64_t rte;

	if (gsi < 0 || gsi >= IOAPIC_NPINS)
		return;
	rte = io.rte[gsi];
	if (rte & RTE_MASK)
		return;
	/*
	 * Physical-destination delivery: the destination APIC id equals the
	 * vCPU index (lapic_init sets lapics[i].id = i).  Logical / lowest-
	 * priority delivery is not modelled; lapic_raise() drops an
	 * out-of-range target.
	 */
	lapic_raise(RTE_DEST(rte), (uint8_t)(rte & RTE_VECTOR));
	vmm_wake_cpu(RTE_DEST(rte));	/* event-driven: kick a halted target */
}
