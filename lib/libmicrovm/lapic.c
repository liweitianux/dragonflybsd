/*
 * Userspace local APIC (xAPIC) with a TSC-deadline / one-shot / periodic timer.
 * One instance per vCPU. See lapic.h.
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

#include <machine/cpufunc.h>	/* rdtsc() */

#include <string.h>

#include "vmm_internal.h"
#include "lapic.h"

/*
 * LAPIC MMIO register offsets and LVT bit fields, mirroring the xAPIC layout in
 * <machine_base/apic/apicreg.h>.  That header is kernel-only (not installed to
 * /usr/include), so this device model keeps its own copy.
 */
#define LAPIC_ID	0x020
#define LAPIC_VER	0x030
#define LAPIC_TPR	0x080
#define LAPIC_APR	0x090
#define LAPIC_PPR	0x0a0
#define LAPIC_EOI	0x0b0
#define LAPIC_RRD	0x0c0
#define LAPIC_LDR	0x0d0
#define LAPIC_DFR	0x0e0
#define LAPIC_SVR	0x0f0
#define LAPIC_ISR0	0x100	/* 0x100..0x170 */
#define LAPIC_TMR0	0x180	/* 0x180..0x1f0 */
#define LAPIC_IRR0	0x200	/* 0x200..0x270 */
#define LAPIC_ESR	0x280
#define LAPIC_ICRLO	0x300
#define LAPIC_ICRHI	0x310
#define LAPIC_LVT_TIMER	0x320
#define LAPIC_LVT_THER	0x330
#define LAPIC_LVT_PERF	0x340
#define LAPIC_LVT_LINT0	0x350
#define LAPIC_LVT_LINT1	0x360
#define LAPIC_LVT_ERR	0x370
#define LAPIC_TMICT	0x380	/* timer initial count */
#define LAPIC_TMCCT	0x390	/* timer current count */
#define LAPIC_TDCR	0x3e0	/* timer divide config */

#define SVR_ENABLE	0x100

#define LVT_MASKED	0x00010000
#define LVT_TIMER_MODE	0x00060000	/* bits 18:17 */
#define LVT_TM_ONESHOT	0x00000000
#define LVT_TM_PERIODIC	0x00020000
#define LVT_TM_DEADLINE	0x00040000
#define LVT_VECTOR	0x000000ff

struct lapic {
	uint32_t id;			/* APIC ID (== vCPU id) */
	uint32_t svr;
	uint32_t tpr;
	uint32_t ldr, dfr;
	uint32_t lvt_timer, lvt_ther, lvt_perf, lvt_lint0, lvt_lint1, lvt_err;
	uint32_t esr;
	uint32_t icr_hi;		/* latched ICR high dword (dest) */
	uint32_t tdcr;
	uint32_t tmict;

	uint32_t isr[8];
	uint32_t irr[8];
	uint32_t tmr[8];

	/* Timer bookkeeping (guest-TSC units). */
	uint64_t deadline;	/* 0 = disarmed */
	uint64_t period;	/* periodic reload */
};

static struct lapic lapics[MICROVM_MAXCPU];
static int lapic_ncpus = 1;
static uint64_t g_tsc_freq;
static uint64_t g_tsc_offset;	/* guest_tsc = host_rdtsc + offset */

uint64_t
lapic_guest_tsc(void)
{
	return rdtsc() + g_tsc_offset;
}

void
lapic_init(int ncpus, uint64_t tsc_freq_hz, uint64_t tsc_offset)
{
	int i;

	memset(lapics, 0, sizeof(lapics));
	lapic_ncpus = ncpus;
	g_tsc_freq = tsc_freq_hz;
	g_tsc_offset = tsc_offset;
	for (i = 0; i < ncpus; i++) {
		struct lapic *la = &lapics[i];
		la->id = (uint32_t)i;
		la->svr = 0xff;		/* spurious vector, soft-disabled */
		la->lvt_timer = LVT_MASKED;
		la->lvt_ther = LVT_MASKED;
		la->lvt_perf = LVT_MASKED;
		la->lvt_lint0 = LVT_MASKED;
		la->lvt_lint1 = LVT_MASKED;
		la->lvt_err = LVT_MASKED;
	}
}

/* ---- bit helpers over the 256-bit IRR/ISR/TMR vectors ---- */

static inline void
vec_set(uint32_t *v, int bit)
{
	v[bit >> 5] |= (1u << (bit & 31));
}

static inline void
vec_clear(uint32_t *v, int bit)
{
	v[bit >> 5] &= ~(1u << (bit & 31));
}

static int
vec_highest(const uint32_t *v)
{
	int i, b;

	for (i = 7; i >= 0; i--) {
		if (v[i] == 0)
			continue;
		for (b = 31; b >= 0; b--) {
			if (v[i] & (1u << b))
				return (i << 5) | b;
		}
	}
	return -1;
}

static uint32_t
lapic_ppr(struct lapic *la)
{
	int isrv = vec_highest(la->isr);
	uint32_t tpr = la->tpr;
	uint32_t isrclass = (isrv < 0) ? 0 : ((uint32_t)isrv & 0xf0);

	if ((tpr & 0xf0) >= isrclass)
		return tpr & 0xf0;
	return isrclass;
}

/* ---- timer ---- */

static uint32_t
tdcr_divider(struct lapic *la)
{
	static const uint32_t tbl[8] = { 2, 4, 8, 16, 32, 64, 128, 1 };
	/* TDCR divide value: bits [1:0] and [3] form a 3-bit table index. */
	uint32_t v = ((la->tdcr & 0x8) >> 1) | (la->tdcr & 0x3);
	return tbl[v];
}

static void
timer_arm_from_tmict(struct lapic *la)
{
	uint32_t mode = la->lvt_timer & LVT_TIMER_MODE;
	uint64_t ticks;

	if (la->tmict == 0 || mode == LVT_TM_DEADLINE) {
		if (mode != LVT_TM_DEADLINE)
			la->deadline = 0;
		return;
	}
	ticks = (uint64_t)la->tmict * tdcr_divider(la);
	la->period = (mode == LVT_TM_PERIODIC) ? ticks : 0;
	la->deadline = lapic_guest_tsc() + ticks;
	vmm_timer_rearm();		/* nearest deadline may have moved earlier */
}

static uint32_t
timer_current_count(struct lapic *la)
{
	uint64_t now, remain;
	uint32_t div;

	if (la->deadline == 0 ||
	    (la->lvt_timer & LVT_TIMER_MODE) == LVT_TM_DEADLINE)
		return 0;
	now = lapic_guest_tsc();
	if (now >= la->deadline)
		return 0;
	div = tdcr_divider(la);
	remain = (la->deadline - now) / (div ? div : 1);
	return (uint32_t)remain;
}

void
lapic_poll_timer(int cpu)
{
	struct lapic *la = &lapics[cpu];
	uint32_t vec;

	if (la->deadline == 0)
		return;
	if (lapic_guest_tsc() < la->deadline)
		return;

	/* Deadline elapsed: raise the timer vector (if enabled & unmasked). */
	if ((la->svr & SVR_ENABLE) && !(la->lvt_timer & LVT_MASKED)) {
		vec = la->lvt_timer & LVT_VECTOR;
		if (vec >= 16)
			vec_set(la->irr, (int)vec);
	}

	/* Re-arm for periodic, otherwise disarm. */
	if ((la->lvt_timer & LVT_TIMER_MODE) == LVT_TM_PERIODIC &&
	    la->period != 0)
		la->deadline += la->period;
	else
		la->deadline = 0;		/* one-shot / deadline */
}

uint64_t
lapic_next_deadline(int cpu)
{
	return lapics[cpu].deadline;
}

/* ---- interrupt delivery ---- */

void
lapic_raise(int cpu, uint8_t vector)
{
	if (cpu >= 0 && cpu < lapic_ncpus && vector >= 16)
		vec_set(lapics[cpu].irr, vector);
}

int
lapic_deliverable(int cpu)
{
	struct lapic *la = &lapics[cpu];
	int vec;

	if (!(la->svr & SVR_ENABLE))
		return -1;
	vec = vec_highest(la->irr);
	if (vec < 0)
		return -1;
	if (((uint32_t)vec & 0xf0) <= (lapic_ppr(la) & 0xf0))
		return -1;		/* masked by priority */
	return vec;
}

void
lapic_accept(int cpu, int vector)
{
	vec_clear(lapics[cpu].irr, vector);
	vec_set(lapics[cpu].isr, vector);
}

static void
lapic_eoi(struct lapic *la)
{
	int vec = vec_highest(la->isr);
	if (vec >= 0)
		vec_clear(la->isr, vec);
}

/* ---- MMIO ---- */

static uint32_t
mmio_read(struct lapic *la, uint32_t off)
{
	switch (off) {
	case LAPIC_ID:		return la->id << 24;
	case LAPIC_VER:		return 0x00060014;	/* ver 0x14, 6 LVTs */
	case LAPIC_TPR:		return la->tpr;
	case LAPIC_PPR:		return lapic_ppr(la);
	case LAPIC_LDR:		return la->ldr;
	case LAPIC_DFR:		return la->dfr;
	case LAPIC_SVR:		return la->svr;
	case LAPIC_ESR:		return la->esr;
	case LAPIC_ICRLO:	return 0;
	case LAPIC_ICRHI:	return la->icr_hi;
	case LAPIC_LVT_TIMER:	return la->lvt_timer;
	case LAPIC_LVT_THER:	return la->lvt_ther;
	case LAPIC_LVT_PERF:	return la->lvt_perf;
	case LAPIC_LVT_LINT0:	return la->lvt_lint0;
	case LAPIC_LVT_LINT1:	return la->lvt_lint1;
	case LAPIC_LVT_ERR:	return la->lvt_err;
	case LAPIC_TMICT:	return la->tmict;
	case LAPIC_TMCCT:	return timer_current_count(la);
	case LAPIC_TDCR:	return la->tdcr;
	default:
		if (off >= LAPIC_ISR0 && off < LAPIC_ISR0 + 0x80)
			return la->isr[(off - LAPIC_ISR0) >> 4];
		if (off >= LAPIC_TMR0 && off < LAPIC_TMR0 + 0x80)
			return la->tmr[(off - LAPIC_TMR0) >> 4];
		if (off >= LAPIC_IRR0 && off < LAPIC_IRR0 + 0x80)
			return la->irr[(off - LAPIC_IRR0) >> 4];
		return 0;
	}
}

static void
mmio_write(struct lapic *la, int cpu, uint32_t off, uint32_t val)
{
	switch (off) {
	case LAPIC_ID:		la->id = val >> 24; break;
	case LAPIC_TPR:		la->tpr = val & 0xff; break;
	case LAPIC_EOI:		lapic_eoi(la); break;
	case LAPIC_LDR:		la->ldr = val; break;
	case LAPIC_DFR:		la->dfr = val; break;
	case LAPIC_SVR:		la->svr = val; break;
	case LAPIC_ESR:		la->esr = 0; break;
	case LAPIC_ICRHI:	la->icr_hi = val; break;
	case LAPIC_ICRLO:	vmm_ipi(cpu, val, la->icr_hi); break;
	case LAPIC_LVT_TIMER:	la->lvt_timer = val; break;
	case LAPIC_LVT_THER:	la->lvt_ther = val; break;
	case LAPIC_LVT_PERF:	la->lvt_perf = val; break;
	case LAPIC_LVT_LINT0:	la->lvt_lint0 = val; break;
	case LAPIC_LVT_LINT1:	la->lvt_lint1 = val; break;
	case LAPIC_LVT_ERR:	la->lvt_err = val; break;
	case LAPIC_TMICT:	la->tmict = val; timer_arm_from_tmict(la); break;
	case LAPIC_TDCR:	la->tdcr = val; break;
	default:		break;
	}
}

bool
lapic_mmio(int cpu, uint64_t gpa, bool write, uint8_t *data, size_t size)
{
	struct lapic *la = &lapics[cpu];
	uint32_t off, val;

	if (gpa < LAPIC_MMIO_BASE || gpa >= LAPIC_MMIO_BASE + LAPIC_MMIO_SIZE)
		return false;

	off = (uint32_t)(gpa - LAPIC_MMIO_BASE) & ~0x3u;
	if (write) {
		val = 0;
		memcpy(&val, data, size > 4 ? 4 : size);
		mmio_write(la, cpu, off, val);
	} else {
		val = mmio_read(la, off);
		memset(data, 0, size);
		memcpy(data, &val, size > 4 ? 4 : size);
	}
	return true;
}

/* ---- MSRs ---- */

bool
lapic_rdmsr(int cpu, uint32_t msr, uint64_t *val)
{
	struct lapic *la = &lapics[cpu];

	switch (msr) {
	case MSR_APICBASE:
		/* xAPIC enabled, default physical base; BSP flag for cpu 0. */
		*val = 0xfee00000ULL | 0x800 | (cpu == 0 ? 0x100 : 0);
		return true;
	case MSR_TSC_DEADLINE:
		*val = (la->deadline != 0 &&
		    (la->lvt_timer & LVT_TIMER_MODE) == LVT_TM_DEADLINE) ?
		    la->deadline : 0;
		return true;
	default:
		return false;
	}
}

bool
lapic_wrmsr(int cpu, uint32_t msr, uint64_t val)
{
	struct lapic *la = &lapics[cpu];

	switch (msr) {
	case MSR_APICBASE:
		return true;		/* relocation not supported; ignore */
	case MSR_TSC_DEADLINE:
		if ((la->lvt_timer & LVT_TIMER_MODE) == LVT_TM_DEADLINE)
			la->deadline = val;	/* absolute guest TSC, 0 disarms */
		else
			la->deadline = 0;
		vmm_timer_rearm();		/* nearest deadline may have moved */
		return true;
	default:
		return false;
	}
}
