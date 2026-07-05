/*
 * Userspace local APIC (xAPIC, MMIO) model with a TSC-deadline timer.
 *
 * NVMM has no in-kernel irqchip, so the monitor emulates the LAPIC and drives
 * nvmm_vcpu_inject() from it. This is the minimal model needed to get Linux a
 * clockevent/clocksource and reach userspace: enable/SVR, TPR/PPR, IRR/ISR,
 * EOI, the LVT timer (one-shot/periodic/TSC-deadline), plus interrupt priority
 * resolution. IOAPIC/PIT (legacy device IRQs) come later.
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

#ifndef _MICROVM_LAPIC_H_
#define _MICROVM_LAPIC_H_

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#define LAPIC_MMIO_BASE		0xfee00000
#define LAPIC_MMIO_SIZE		0x1000

/*
 * Canonical in <machine/specialreg.h>, but that header cannot be included in
 * this library: its control-register (CR0/CR4) defines collide with the
 * __BIT() forms in <dev/virtual/nvmm/x86/nvmm_x86.h>, which every TU already
 * pulls via <nvmm.h>.  Kept local; values match specialreg.h (MSR_APICBASE
 * 0x1b, MSR_TSC_DEADLINE 0x6e0).
 */
#define MSR_APICBASE		0x0000001b
#define MSR_TSC_DEADLINE	0x000006e0

/* Initialize all per-vCPU LAPICs (ncpus of them) and the shared TSC params. */
void lapic_init(int ncpus, uint64_t tsc_freq_hz, uint64_t tsc_offset);

/* Per-vCPU access (cpu = accessing vCPU's id). MMIO returns true if it targeted
 * the LAPIC window; rdmsr/wrmsr return true if the MSR was handled. */
bool lapic_mmio(int cpu, uint64_t gpa, bool write, uint8_t *data, size_t size);
bool lapic_rdmsr(int cpu, uint32_t msr, uint64_t *val);
bool lapic_wrmsr(int cpu, uint32_t msr, uint64_t val);

/* Fire the timer vector into IRR if this vCPU's deadline has elapsed. */
void lapic_poll_timer(int cpu);

/* Raise an interrupt vector (set IRR) on a target vCPU - used by IOAPIC/IPIs. */
void lapic_raise(int cpu, uint8_t vector);

/* Highest-priority deliverable IRR vector (> PPR) for this vCPU, or -1. */
int lapic_deliverable(int cpu);

/* Move a vector from IRR to ISR after it has been injected. */
void lapic_accept(int cpu, int vector);

/* Guest TSC value of this vCPU's next armed timer deadline, 0 if none. */
uint64_t lapic_next_deadline(int cpu);

/* Current guest TSC (host rdtsc + offset); shared by all vCPUs. */
uint64_t lapic_guest_tsc(void);

#endif /* _MICROVM_LAPIC_H_ */
