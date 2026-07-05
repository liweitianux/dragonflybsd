/*
 * Minimal I/O APIC model (intel 82093AA) - MMIO at 0xfec00000.
 *
 * Enough for Linux to discover/init the IOAPIC (so it enters full APIC mode and
 * sets up the LAPIC timer) and for device IRQ lines to be routed to the LAPIC.
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

#ifndef _MICROVM_IOAPIC_H_
#define _MICROVM_IOAPIC_H_

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#define IOAPIC_BASE	0xfec00000
#define IOAPIC_SIZE	0x1000
#define IOAPIC_ID	0		/* APIC ID we advertise */
#define IOAPIC_NPINS	24

void ioapic_init(void);
bool ioapic_mmio(uint64_t gpa, bool write, uint8_t *data, size_t size);

/* Raise an edge-triggered IRQ line (GSI); routes through the RTE to the LAPIC. */
void ioapic_raise(int gsi);

#endif /* _MICROVM_IOAPIC_H_ */
