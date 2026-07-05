/*
 * Intel MP table generator. See mptable.h.
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

#include <stdint.h>
#include <string.h>

#include "mptable.h"
#include "lapic.h"
#include "ioapic.h"

#define LAPIC_ADDR	0xfee00000

struct mpfps {
	uint8_t  signature[4];	/* "_MP_" */
	uint32_t pap;		/* phys addr of MP config table */
	uint8_t  length;	/* in 16-byte units (1) */
	uint8_t  spec_rev;	/* 4 => 1.4 */
	uint8_t  checksum;
	uint8_t  feature[5];
} __attribute__((packed));

struct mpch {			/* MP config table header */
	uint8_t  signature[4];	/* "PCMP" */
	uint16_t length;
	uint8_t  spec_rev;
	uint8_t  checksum;
	uint8_t  oem_id[8];
	uint8_t  product_id[12];
	uint32_t oem_table_pap;
	uint16_t oem_table_size;
	uint16_t entry_count;
	uint32_t lapic_addr;
	uint16_t ext_length;
	uint8_t  ext_checksum;
	uint8_t  reserved;
} __attribute__((packed));

struct mpe_proc {
	uint8_t  type;		/* 0 */
	uint8_t  apic_id;
	uint8_t  apic_ver;
	uint8_t  cpu_flags;	/* bit0 EN, bit1 BSP */
	uint32_t cpu_signature;
	uint32_t feature_flags;
	uint32_t reserved[2];
} __attribute__((packed));

struct mpe_bus {
	uint8_t  type;		/* 1 */
	uint8_t  bus_id;
	uint8_t  bus_type[6];
} __attribute__((packed));

struct mpe_ioapic {
	uint8_t  type;		/* 2 */
	uint8_t  apic_id;
	uint8_t  apic_ver;
	uint8_t  flags;		/* bit0 EN */
	uint32_t addr;
} __attribute__((packed));

struct mpe_int {
	uint8_t  type;		/* 3 = I/O int, 4 = local int */
	uint8_t  int_type;	/* 0 INT, 1 NMI, 2 SMI, 3 ExtINT */
	uint16_t flags;
	uint8_t  src_bus_id;
	uint8_t  src_bus_irq;
	uint8_t  dst_apic_id;
	uint8_t  dst_apic_intin;
} __attribute__((packed));

#define MPE_PROC	0
#define MPE_BUS		1
#define MPE_IOAPIC	2
#define MPE_IOINT	3
#define MPE_LOCALINT	4

#define INT_INT		0
#define INT_NMI		1
#define INT_EXTINT	3

#define BUS_ISA		0

static uint8_t
checksum(const uint8_t *p, size_t len)
{
	uint8_t sum = 0;
	size_t i;
	for (i = 0; i < len; i++)
		sum += p[i];
	return (uint8_t)(0x100 - sum);
}

int
mptable_build(struct microvm *v)
{
	uint8_t buf[1024];
	uint8_t *base = buf + sizeof(struct mpfps);	/* config after FP */
	uint8_t *p = base + sizeof(struct mpch);
	struct mpfps *fps = (struct mpfps *)buf;
	struct mpch *ch = (struct mpch *)base;
	uint16_t nent = 0;
	uint32_t cfg_gpa = MPTABLE_GPA + sizeof(struct mpfps);
	void *dst;
	int i;

	memset(buf, 0, sizeof(buf));

	/* Processors: cpu0 is the BSP, the rest enabled APs. */
	for (i = 0; i < v->ncpus; i++) {
		struct mpe_proc *e = (struct mpe_proc *)p;
		e->type = MPE_PROC;
		e->apic_id = (uint8_t)i;
		e->apic_ver = 0x14;
		e->cpu_flags = (i == 0) ? 0x3 : 0x1;	/* (BSP|EN) : EN */
		e->cpu_signature = 0x600;	/* family 6 */
		e->feature_flags = 0x201;	/* FPU | APIC */
		p += sizeof(*e); nent++;
	}

	/* ISA bus. */
	{
		struct mpe_bus *e = (struct mpe_bus *)p;
		e->type = MPE_BUS;
		e->bus_id = BUS_ISA;
		memcpy(e->bus_type, "ISA   ", 6);
		p += sizeof(*e); nent++;
	}

	/* IOAPIC. */
	{
		struct mpe_ioapic *e = (struct mpe_ioapic *)p;
		e->type = MPE_IOAPIC;
		e->apic_id = IOAPIC_ID;
		e->apic_ver = 0x11;
		e->flags = 0x1;			/* EN */
		e->addr = IOAPIC_BASE;
		p += sizeof(*e); nent++;
	}

	/* ISA IRQ -> IOAPIC routing. INTIN0 carries the 8259 ExtINT; the
	 * timer (IRQ0) is wired to INTIN2, the rest map identically. */
	{
		struct mpe_int *e = (struct mpe_int *)p;
		e->type = MPE_IOINT;
		e->int_type = INT_EXTINT;
		e->src_bus_id = BUS_ISA;
		e->src_bus_irq = 0;
		e->dst_apic_id = IOAPIC_ID;
		e->dst_apic_intin = 0;
		p += sizeof(*e); nent++;
	}
	for (i = 0; i < 16; i++) {
		struct mpe_int *e = (struct mpe_int *)p;
		e->type = MPE_IOINT;
		e->int_type = INT_INT;
		e->src_bus_id = BUS_ISA;
		e->src_bus_irq = i;
		e->dst_apic_id = IOAPIC_ID;
		e->dst_apic_intin = (i == 0) ? 2 : i;
		p += sizeof(*e); nent++;
	}

	/* Local interrupts: ExtINT -> LINT0, NMI -> LINT1. */
	{
		struct mpe_int *e = (struct mpe_int *)p;
		e->type = MPE_LOCALINT;
		e->int_type = INT_EXTINT;
		e->dst_apic_id = 0xff;
		e->dst_apic_intin = 0;
		p += sizeof(*e); nent++;
		e = (struct mpe_int *)p;
		e->type = MPE_LOCALINT;
		e->int_type = INT_NMI;
		e->dst_apic_id = 0xff;
		e->dst_apic_intin = 1;
		p += sizeof(*e); nent++;
	}

	/* Config table header. */
	memcpy(ch->signature, "PCMP", 4);
	ch->length = (uint16_t)(p - base);
	ch->spec_rev = 4;
	memcpy(ch->oem_id, "DRAGONFL", 8);
	memcpy(ch->product_id, "microvm     ", 12);
	ch->entry_count = nent;
	ch->lapic_addr = LAPIC_ADDR;
	ch->checksum = checksum(base, ch->length);

	/* Floating pointer. */
	memcpy(fps->signature, "_MP_", 4);
	fps->pap = cfg_gpa;
	fps->length = 1;
	fps->spec_rev = 4;
	fps->checksum = checksum((uint8_t *)fps, sizeof(*fps));

	/* Copy into guest RAM. */
	dst = gpa_to_hva(v, MPTABLE_GPA, (size_t)(p - buf));
	if (dst == NULL)
		return microvm_seterr(v, "mptable does not fit in guest RAM");
	memcpy(dst, buf, (size_t)(p - buf));
	return 0;
}
