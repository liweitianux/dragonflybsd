/*
 * libmicrovm core: VM lifecycle, vCPU run loop, interrupt delivery, and the
 * public API (see microvm.h). Device models live in the sibling modules
 * (loader, uart, lapic, ioapic, mptable, virtio*).
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

#include <sys/types.h>
#include <sys/mman.h>
#include <sys/sysctl.h>

#include <machine/psl.h>	/* PSL_I */

#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "vmm_internal.h"
#include "lapic.h"
#include "ioapic.h"
#include "mptable.h"
#include "virtio.h"

/* virtio-mmio device windows (above guest RAM, below the APICs). Each device
 * gets a 0x200 window and an IOAPIC IRQ line (routed by the MP table). */
#define VIRTIO_MMIO_BASE	0xd0000000
#define VIRTIO_BLK_MMIO_BASE	(VIRTIO_MMIO_BASE + 0x000)
#define VIRTIO_BLK_IRQ		5
#define VIRTIO_CON_MMIO_BASE	(VIRTIO_MMIO_BASE + 0x200)
#define VIRTIO_CON_IRQ		6
#define VIRTIO_NET_MMIO_BASE	(VIRTIO_MMIO_BASE + 0x400)
#define VIRTIO_NET_IRQ		7
#define VIRTIO_VSOCK_MMIO_BASE	(VIRTIO_MMIO_BASE + 0x600)
#define VIRTIO_VSOCK_IRQ	8

/*
 * CR0_PE, CR0_ET, CPUID_0_01_ECX_* come from <dev/virtual/nvmm/x86/nvmm_x86.h>
 * (pulled via <nvmm.h>); PSL_I (RFLAGS.IF) from <machine/psl.h>.
 */

/*
 * Deadline-armed timer. Instead of kicking every vCPU at a fixed 1 kHz, the
 * timer thread sleeps to the NEAREST armed LAPIC deadline and kicks only the
 * vCPU(s) whose deadline elapsed. This removes the idle-VM wakeup storm (a
 * tickless-idle guest re-HLTs ~10/s instead of ~1000/s). Host input is handled
 * event-driven by the input reader thread (below), not by the timer; BACKSTOP_NS
 * caps the sleep purely as a safety net (e.g. a device IRQ raised onto a halted
 * vCPU that missed its wake-on-raise).
 */
#define BACKSTOP_NS	(100ULL * 1000000)	/* timer sleep cap / safety net */
#define OVERDUE_KICK_NS	(1ULL * 1000000)	/* backoff when a deadline is overdue
						 * but unserviced (a contended vCPU
						 * has not run to clear it) - bounds
						 * re-kicks instead of busy-spinning */

/*
 * Default guest command line. TSC is our only clocksource and it is the
 * pass-through host TSC (a single coherent timebase), so tsc=reliable keeps the
 * clocksource watchdog from needlessly flagging it; the frequency is supplied
 * via the CPUID.15H handler, so no tsc_early_khz= is needed.
 */
#define DEFAULT_CMDLINE	"console=ttyS0 earlycon=uart8250,io,0x3f8 tsc=reliable"

/* Single-VM-per-process runtime state (device models are process-global too). */
static volatile bool running;

/*
 * With per-vCPU threads, the device models, IOAPIC, and cross-vCPU LAPIC IRR
 * writes are shared. vm_lock serializes all of that: a vCPU thread holds it for
 * the exit-handling section of its loop (never across nvmm_vcpu_run, where the
 * vCPU is in-guest). ap_cond parks APs until they are released (BSP, or SIPI).
 */
static pthread_mutex_t vm_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t ap_cond = PTHREAD_COND_INITIALIZER;
static struct microvm *the_vm;	/* for vmm_ipi() to reach the vCPU array */
static pthread_t ticker_thread;	/* deadline-armed timer; joined first at shutdown */

/* Deadline-armed timer wakeup: the timer thread waits on timer_cond until the
 * nearest deadline; a vCPU arming its LAPIC timer sets timer_rearm + signals. */
static pthread_mutex_t timer_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t timer_cond;		/* CLOCK_MONOTONIC, init in start */
static volatile bool timer_rearm;
static volatile bool timer_active;		/* cond usable (thread live) */
static uint64_t timer_poll_ns;			/* timer thread sleep cap (backstop) */

/* Input reader: a thread that poll()s host input fds (tap/console/vsock) and
 * drains them event-driven, so a halted guest sees input without a poll floor. */
static pthread_t input_thread;
static bool input_started;
#define INPUT_FDS_MAX	72		/* vsock conns (64) + tap + stdin + slack */
#define INPUT_RESCAN_MS	100		/* re-collect fds / backstop for unwatched */

/* ---- debug instrumentation (enabled via microvm_config.debug) ---- */
int microvm_debug;
static volatile unsigned long dbg_cnt[16];
enum { D_NONE, D_IO, D_MEM, D_RDMSR, D_WRMSR, D_CPUID, D_INTRDY, D_HALT,
       D_INJECT, D_OTHER };

/* Stop the VM and wake the timer thread so shutdown is not delayed by the poll
 * floor (the timer thread's wait, otherwise up to the backstop). */
static void
stop_vm(void)
{
	running = false;
	vmm_timer_rearm();
}

/* -------------------------------------------------------------------------- */

int
microvm_seterr(struct microvm *m, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(m->errbuf, sizeof(m->errbuf), fmt, ap);
	va_end(ap);
	return -1;
}

const char *
microvm_error(const struct microvm *m)
{
	return m->errbuf[0] ? m->errbuf : "no error";
}

void *
gpa_to_hva(struct microvm *m, uint64_t gpa, uint64_t size)
{
	if (gpa + size < gpa)			/* overflow */
		return NULL;
	if (gpa + size > m->mem.size)
		return NULL;
	return m->mem.hva + gpa;
}

uint64_t
microvm_tsc_freq(void)
{
	static uint64_t freq;
	size_t len = sizeof(freq);

	if (freq == 0) {
		if (sysctlbyname("hw.tsc_frequency", &freq, &len, NULL, 0) == -1
		    || freq == 0)
			freq = 1000000000ULL;	/* fallback 1 GHz */
	}
	return freq;
}

/* -------------------------------------------------------------------------- */

static int
mem_setup(struct microvm *m, uint64_t size)
{
	void *buf;

	buf = mmap(NULL, size, PROT_READ | PROT_WRITE,
	    MAP_ANON | MAP_PRIVATE, -1, 0);
	if (buf == MAP_FAILED)
		return microvm_seterr(m, "mmap %llu bytes: %s",
		    (unsigned long long)size, strerror(errno));

	if (nvmm_hva_map(&m->mach, (uintptr_t)buf, size) == -1) {
		munmap(buf, size);
		return microvm_seterr(m, "nvmm_hva_map: %s", strerror(errno));
	}
	if (nvmm_gpa_map(&m->mach, (uintptr_t)buf, 0, size,
	    PROT_READ | PROT_WRITE | PROT_EXEC) == -1) {
		munmap(buf, size);
		return microvm_seterr(m, "nvmm_gpa_map: %s", strerror(errno));
	}

	m->mem.hva = buf;
	m->mem.size = size;
	return 0;
}

/* -------------------------------------------------------------------------- */

static void
io_callback(struct nvmm_io *io)
{
	if (io->port >= UART_IOBASE && io->port < UART_IOBASE + UART_IOSIZE) {
		uart_io(io->port, io->in, io->data, io->size);
		return;
	}
	/*
	 * Reset control ports. With no ACPI, the guest's reboot path falls
	 * back to the 0xcf9 PCI reset register and the 8042 keyboard-controller
	 * pulse-reset command (0xfe -> port 0x64); treat either as a clean stop.
	 */
	if (!io->in &&
	    (io->port == 0xcf9 ||
	    (io->port == 0x64 && io->size >= 1 && io->data[0] == 0xfe))) {
		stop_vm();
		return;
	}
	if (io->in)
		memset(io->data, 0, io->size);
}

static void
mem_callback(struct nvmm_mem *mem)
{
	int cpu = (int)mem->vcpu->cpuid;	/* LAPIC accessed is the caller's */

	if (lapic_mmio(cpu, mem->gpa, mem->write, mem->data, mem->size))
		return;
	if (ioapic_mmio(mem->gpa, mem->write, mem->data, mem->size))
		return;
	if (virtio_mmio(mem->gpa, mem->write, mem->data, mem->size))
		return;
	/* Unmodeled MMIO: reads return zero, writes are dropped. */
	if (!mem->write)
		memset(mem->data, 0, mem->size);
}

static struct nvmm_assist_callbacks callbacks = {
	.io = io_callback,
	.mem = mem_callback,
};

static int
vcpu_setup(struct vcpu *vc)
{
	struct nvmm_machine *mach = &vc->vm->mach;
	struct nvmm_vcpu_conf_cpuid cpuid;

	if (nvmm_vcpu_configure(mach, &vc->nvmm, NVMM_VCPU_CONF_CALLBACKS,
	    &callbacks) == -1)
		return microvm_seterr(vc->vm, "configure callbacks: %s",
		    strerror(errno));

	/* Leaf 1: hide x2APIC (force MMIO xAPIC) + advertise TSC-deadline. */
	memset(&cpuid, 0, sizeof(cpuid));
	cpuid.mask = 1;
	cpuid.leaf = 0x00000001;
	cpuid.u.mask.del.ecx = CPUID_0_01_ECX_X2APIC;
	cpuid.u.mask.set.ecx = CPUID_0_01_ECX_TSC_DEADLINE;
	if (nvmm_vcpu_configure(mach, &vc->nvmm, NVMM_VCPU_CONF_CPUID,
	    &cpuid) == -1)
		return microvm_seterr(vc->vm, "configure cpuid leaf1: %s",
		    strerror(errno));

	/* Leaf 0x15: exit so we can report the TSC frequency to the guest. */
	memset(&cpuid, 0, sizeof(cpuid));
	cpuid.exit = 1;
	cpuid.leaf = 0x00000015;
	if (nvmm_vcpu_configure(mach, &vc->nvmm, NVMM_VCPU_CONF_CPUID,
	    &cpuid) == -1)
		return microvm_seterr(vc->vm, "configure cpuid leaf15: %s",
		    strerror(errno));
	return 0;
}

static void
init_seg(struct nvmm_x64_state_seg *seg, int type, int sel, uint32_t limit)
{
	seg->selector = sel;
	seg->attrib.type = type & 0xf;
	seg->attrib.s = (type & 0x10) != 0;
	seg->attrib.dpl = 0;
	seg->attrib.p = 1;
	seg->attrib.avl = 1;
	seg->attrib.l = 0;
	seg->attrib.def = 1;		/* 32-bit */
	seg->attrib.g = 1;
	seg->limit = limit;
	seg->base = 0;
}

/* PVH entry state: 32-bit protected mode, paging off, flat segments,
 * %ebx -> hvm_start_info, %eip -> PVH entry. */
static int
vcpu_set_pvh_state(struct vcpu *vc, uint64_t entry, uint64_t start_info_gpa)
{
	struct nvmm_machine *mach = &vc->vm->mach;
	struct nvmm_vcpu_state *st = vc->nvmm.state;

	if (nvmm_vcpu_getstate(mach, &vc->nvmm, NVMM_X64_STATE_ALL) == -1)
		return microvm_seterr(vc->vm, "getstate: %s", strerror(errno));

	init_seg(&st->segs[NVMM_X64_SEG_CS], 27, 0, 0xFFFFFFFF);
	init_seg(&st->segs[NVMM_X64_SEG_DS], 19, 0, 0xFFFFFFFF);
	init_seg(&st->segs[NVMM_X64_SEG_ES], 19, 0, 0xFFFFFFFF);
	init_seg(&st->segs[NVMM_X64_SEG_SS], 19, 0, 0xFFFFFFFF);
	init_seg(&st->segs[NVMM_X64_SEG_FS], 19, 0, 0xFFFFFFFF);
	init_seg(&st->segs[NVMM_X64_SEG_GS], 19, 0, 0xFFFFFFFF);
	init_seg(&st->segs[NVMM_X64_SEG_GDT], 0, 0, 0xFFFF);
	init_seg(&st->segs[NVMM_X64_SEG_IDT], 0, 0, 0xFFFF);
	init_seg(&st->segs[NVMM_X64_SEG_LDT], 2, 0, 0xFFFFFFFF);
	init_seg(&st->segs[NVMM_X64_SEG_TR], 11, 0, 0xFFFFFFFF);

	st->crs[NVMM_X64_CR_CR0] = CR0_PE | CR0_ET;
	st->crs[NVMM_X64_CR_CR3] = 0;
	st->crs[NVMM_X64_CR_CR4] = 0;

	st->gprs[NVMM_X64_GPR_RIP] = entry;
	st->gprs[NVMM_X64_GPR_RBX] = start_info_gpa;	/* %ebx */
	st->gprs[NVMM_X64_GPR_RFLAGS] = 0x2;
	st->gprs[NVMM_X64_GPR_RSP] = 0;

	if (nvmm_vcpu_setstate(mach, &vc->nvmm, NVMM_X64_STATE_ALL) == -1)
		return microvm_seterr(vc->vm, "setstate: %s", strerror(errno));
	return 0;
}

/* Real-mode segment (16-bit, base = selector<<4 by convention). */
static void
rm_seg(struct nvmm_x64_state_seg *seg, int type, uint16_t sel, uint64_t base)
{
	seg->selector = sel;
	seg->attrib.type = type & 0xf;
	seg->attrib.s = (type & 0x10) != 0;
	seg->attrib.dpl = 0;
	seg->attrib.p = 1;
	seg->attrib.avl = 0;
	seg->attrib.l = 0;
	seg->attrib.def = 0;		/* 16-bit */
	seg->attrib.g = 0;
	seg->limit = 0xffff;
	seg->base = base;
}

/* Park an AP at the real-mode SIPI vector: CS.base = vector<<12, IP = 0. */
static int
vcpu_set_realmode(struct vcpu *vc, uint8_t vector)
{
	struct nvmm_machine *mach = &vc->vm->mach;
	struct nvmm_vcpu_state *st = vc->nvmm.state;

	if (nvmm_vcpu_getstate(mach, &vc->nvmm, NVMM_X64_STATE_ALL) == -1)
		return microvm_seterr(vc->vm, "ap getstate: %s", strerror(errno));

	rm_seg(&st->segs[NVMM_X64_SEG_CS], 27, (uint16_t)(vector << 8),
	    (uint64_t)vector << 12);
	rm_seg(&st->segs[NVMM_X64_SEG_DS], 19, 0, 0);
	rm_seg(&st->segs[NVMM_X64_SEG_ES], 19, 0, 0);
	rm_seg(&st->segs[NVMM_X64_SEG_SS], 19, 0, 0);
	rm_seg(&st->segs[NVMM_X64_SEG_FS], 19, 0, 0);
	rm_seg(&st->segs[NVMM_X64_SEG_GS], 19, 0, 0);
	rm_seg(&st->segs[NVMM_X64_SEG_GDT], 0, 0, 0);
	rm_seg(&st->segs[NVMM_X64_SEG_IDT], 0, 0, 0);
	rm_seg(&st->segs[NVMM_X64_SEG_LDT], 2, 0, 0);
	rm_seg(&st->segs[NVMM_X64_SEG_TR], 11, 0, 0);

	st->crs[NVMM_X64_CR_CR0] = CR0_ET;	/* real mode: PE=0, PG=0 */
	st->crs[NVMM_X64_CR_CR3] = 0;
	st->crs[NVMM_X64_CR_CR4] = 0;
	st->gprs[NVMM_X64_GPR_RIP] = 0;
	st->gprs[NVMM_X64_GPR_RFLAGS] = 0x2;
	st->gprs[NVMM_X64_GPR_RSP] = 0;

	if (nvmm_vcpu_setstate(mach, &vc->nvmm, NVMM_X64_STATE_ALL) == -1)
		return microvm_seterr(vc->vm, "ap setstate: %s", strerror(errno));
	return 0;
}

/*
 * Deliver an IPI (called from a vCPU's ICR write, under vm_lock). Handles the
 * delivery modes Linux needs for SMP: fixed (set the target IRR + wake it),
 * INIT (AP is already parked), and SIPI (start a parked AP in real mode).
 */
/*
 * Wake a vCPU whose LAPIC IRR just gained a vector (device IRQ via the IOAPIC).
 * Kicks it out of nvmm_vcpu_run so deliver_pending() injects without waiting for
 * the timer thread's poll floor. Skips the caller (which will inject on its own
 * loop) and any non-running vCPU.
 */
void
vmm_wake_cpu(int cpu)
{
	struct microvm *m = the_vm;

	if (m == NULL || cpu < 0 || cpu >= m->ncpus)
		return;
	if (!m->vcpus[cpu].started)
		return;
	if (pthread_equal(pthread_self(), m->vcpus[cpu].thread))
		return;			/* self: delivers on this same loop */
	pthread_kill(m->vcpus[cpu].thread, SIGUSR1);
}

/*
 * LAPIC ICR field extraction.  Delivery mode (bits 10:8): 0 fixed, 5 INIT,
 * 6 SIPI.  Destination shorthand (bits 19:18): 0 dest field, 1 self, 2 all,
 * 3 all-excl-self.  Encodings mirror <machine_base/apic/apicreg.h>
 * (APIC_DELMODE_*), which is kernel-only and not installed to /usr/include.
 */
#define ICR_DELMODE(lo)		(((lo) >> 8) & 0x7)
#define ICR_SHORTHAND(lo)	(((lo) >> 18) & 0x3)
#define ICR_VECTOR(lo)		((lo) & 0xff)
#define ICR_DEST(hi)		(((hi) >> 24) & 0xff)

void
vmm_ipi(int src, uint32_t lo, uint32_t hi)
{
	struct microvm *m = the_vm;
	int delivery = ICR_DELMODE(lo);
	int shorthand = ICR_SHORTHAND(lo);
	uint8_t vector = ICR_VECTOR(lo);
	int dest = ICR_DEST(hi);
	int c;

	if (m == NULL)
		return;
	for (c = 0; c < m->ncpus; c++) {
		bool target;

		switch (shorthand) {
		case 1: target = (c == src); break;	/* self */
		case 2: target = true; break;		/* all incl self */
		case 3: target = (c != src); break;	/* all excl self */
		default: target = (c == dest); break;	/* physical dest id */
		}
		if (!target)
			continue;

		switch (delivery) {
		case 0:				/* fixed */
			lapic_raise(c, vector);
			if (c != src)
				pthread_kill(m->vcpus[c].thread, SIGUSR1);
			break;
		case 5:				/* INIT (AP already parked) */
			break;
		case 6:				/* SIPI (startup) */
			if (c != 0 && !m->vcpus[c].started) {
				if (vcpu_set_realmode(&m->vcpus[c], vector) == 0) {
					m->vcpus[c].started = true;
					pthread_cond_broadcast(&ap_cond);
				}
			}
			break;
		default:			/* NMI/SMI: not modeled */
			break;
		}
	}
}

/* -------------------------------------------------------------------------- */

static int
handle_cpuid(struct vcpu *vc)
{
	struct nvmm_machine *mach = &vc->vm->mach;
	struct nvmm_vcpu_state *st = vc->nvmm.state;
	struct nvmm_vcpu_exit *exit = vc->nvmm.exit;

	/*
	 * The only leaf we configure to exit is CPUID.15H. NVMM has already
	 * run the host CPUID into the GPRs by the time we get here (so RAX no
	 * longer holds the requested leaf - do not test it). Report the TSC
	 * frequency as the core-crystal clock with a 1:1 ratio, so the guest
	 * derives the exact TSC kHz directly and needs no PIT/HPET calibration.
	 */
	if (nvmm_vcpu_getstate(mach, &vc->nvmm, NVMM_X64_STATE_GPRS) == -1)
		return microvm_seterr(vc->vm, "cpuid getstate: %s",
		    strerror(errno));
	st->gprs[NVMM_X64_GPR_RAX] = 1;			/* EAX: denominator */
	st->gprs[NVMM_X64_GPR_RBX] = 1;			/* EBX: numerator   */
	st->gprs[NVMM_X64_GPR_RCX] = vc->vm->tsc_freq;	/* ECX: crystal Hz  */
	st->gprs[NVMM_X64_GPR_RDX] = 0;
	st->gprs[NVMM_X64_GPR_RIP] = exit->u.insn.npc;
	if (nvmm_vcpu_setstate(mach, &vc->nvmm, NVMM_X64_STATE_GPRS) == -1)
		return microvm_seterr(vc->vm, "cpuid setstate: %s",
		    strerror(errno));
	return 0;
}

static int
handle_rdmsr(struct vcpu *vc)
{
	struct nvmm_machine *mach = &vc->vm->mach;
	struct nvmm_vcpu_state *st = vc->nvmm.state;
	struct nvmm_vcpu_exit *exit = vc->nvmm.exit;
	uint64_t val = 0;

	if (!lapic_rdmsr(vc->id, exit->u.rdmsr.msr, &val))
		val = 0;		/* permissive: unknown MSRs read 0 */

	if (nvmm_vcpu_getstate(mach, &vc->nvmm, NVMM_X64_STATE_GPRS) == -1)
		return microvm_seterr(vc->vm, "rdmsr getstate: %s",
		    strerror(errno));
	st->gprs[NVMM_X64_GPR_RAX] = val & 0xffffffff;
	st->gprs[NVMM_X64_GPR_RDX] = val >> 32;
	st->gprs[NVMM_X64_GPR_RIP] = exit->u.rdmsr.npc;
	if (nvmm_vcpu_setstate(mach, &vc->nvmm, NVMM_X64_STATE_GPRS) == -1)
		return microvm_seterr(vc->vm, "rdmsr setstate: %s",
		    strerror(errno));
	return 0;
}

static int
handle_wrmsr(struct vcpu *vc)
{
	struct nvmm_machine *mach = &vc->vm->mach;
	struct nvmm_vcpu_state *st = vc->nvmm.state;
	struct nvmm_vcpu_exit *exit = vc->nvmm.exit;

	(void)lapic_wrmsr(vc->id, exit->u.wrmsr.msr, exit->u.wrmsr.val);

	if (nvmm_vcpu_getstate(mach, &vc->nvmm, NVMM_X64_STATE_GPRS) == -1)
		return microvm_seterr(vc->vm, "wrmsr getstate: %s",
		    strerror(errno));
	st->gprs[NVMM_X64_GPR_RIP] = exit->u.wrmsr.npc;
	if (nvmm_vcpu_setstate(mach, &vc->nvmm, NVMM_X64_STATE_GPRS) == -1)
		return microvm_seterr(vc->vm, "wrmsr setstate: %s",
		    strerror(errno));
	return 0;
}

/* -------------------------------------------------------------------------- */

static int
set_int_window(struct vcpu *vc, bool on)
{
	struct nvmm_machine *mach = &vc->vm->mach;

	if (vc->int_window_requested == on)
		return 0;
	if (nvmm_vcpu_getstate(mach, &vc->nvmm, NVMM_X64_STATE_INTR) == -1)
		return microvm_seterr(vc->vm, "intr getstate: %s",
		    strerror(errno));
	vc->nvmm.state->intr.int_window_exiting = on ? 1 : 0;
	if (nvmm_vcpu_setstate(mach, &vc->nvmm, NVMM_X64_STATE_INTR) == -1)
		return microvm_seterr(vc->vm, "intr setstate: %s",
		    strerror(errno));
	vc->int_window_requested = on;
	return 0;
}

static int
deliver_pending(struct vcpu *vc)
{
	struct nvmm_vcpu_exit *exit = vc->nvmm.exit;
	int vec;
	bool can;

	lapic_poll_timer(vc->id);

	vec = lapic_deliverable(vc->id);
	if (vec < 0)
		return set_int_window(vc, false);

	can = (exit->exitstate.rflags & PSL_I) &&
	    !exit->exitstate.int_shadow &&
	    !exit->exitstate.evt_pending;
	if (!can)
		return set_int_window(vc, true);

	vc->nvmm.event->type = NVMM_VCPU_EVENT_INTR;
	vc->nvmm.event->vector = vec;
	if (nvmm_vcpu_inject(&vc->vm->mach, &vc->nvmm) == -1)
		return microvm_seterr(vc->vm, "inject: %s", strerror(errno));
	lapic_accept(vc->id, vec);
	dbg_cnt[D_INJECT]++;
	return set_int_window(vc, false);
}

void
vmm_timer_rearm(void)
{
	if (!timer_active)		/* timer thread not running yet */
		return;
	pthread_mutex_lock(&timer_lock);
	timer_rearm = true;
	pthread_cond_signal(&timer_cond);
	pthread_mutex_unlock(&timer_lock);
}

/* Earliest armed LAPIC deadline (guest TSC) across started vCPUs, 0 if none.
 * Reads lapic_next_deadline() locklessly - same pattern (and benign race) as
 * wait_for_timer, which the vCPU also calls with vm_lock dropped. */
static uint64_t
earliest_deadline(struct microvm *m)
{
	uint64_t best = 0;
	int i;

	for (i = 0; i < m->ncpus; i++) {
		uint64_t d;

		if (!m->vcpus[i].started)
			continue;
		d = lapic_next_deadline(i);
		if (d != 0 && (best == 0 || d < best))
			best = d;
	}
	return best;
}

static void
wait_for_timer(struct vcpu *vc)
{
	uint64_t dl, now, ns;
	struct timespec ts;

	dl = lapic_next_deadline(vc->id);
	now = lapic_guest_tsc();

	if (dl == 0) {
		ns = timer_poll_ns;	/* no timer armed: input / backstop poll */
	} else if (dl <= now) {
		return;
	} else {
		ns = (dl - now) * 1000000000ULL / microvm_tsc_freq();
		if (ns > timer_poll_ns)
			ns = timer_poll_ns;
	}
	ts.tv_sec = ns / 1000000000ULL;
	ts.tv_nsec = ns % 1000000000ULL;
	nanosleep(&ts, NULL);
}

/* -------------------------------------------------------------------------- */

static void
sigusr1(int sig __unused)
{
	/* Empty: only used to kick a vCPU thread out of nvmm_vcpu_run. */
}

static void
sig_stop(int sig __unused)
{
	/*
	 * SIGINT (when stdin is NOT raw) / SIGTERM: stop the VM cleanly. The
	 * ticker's SIGUSR1 then kicks the vCPUs out of nvmm_vcpu_run and they
	 * observe !running and exit. (In raw console mode Ctrl-C is delivered to
	 * the guest as a byte, not as SIGINT - use the ^A x console escape.)
	 */
	running = false;
}

/*
 * Deadline-armed timer thread. Sleeps until the nearest armed LAPIC deadline
 * across started vCPUs (re-armed early via timer_cond when a vCPU programs its
 * timer), capped by BACKSTOP_NS, then kicks the vCPU(s) whose deadline has
 * elapsed - preempting an in-guest spinner so it injects the timer IRQ. Host
 * input is handled by the input reader thread, not here. Replaces the old fixed
 * 1 kHz kicker.
 */
static void *
ticker(void *arg)
{
	struct microvm *m = arg;
	struct timespec last;
	int i;

	clock_gettime(CLOCK_MONOTONIC, &last);
	while (running) {
		uint64_t now = lapic_guest_tsc();
		uint64_t d = earliest_deadline(m);
		uint64_t ns;

		if (d == 0)
			ns = timer_poll_ns;	/* nothing armed: poll floor */
		else if (d <= now)
			ns = OVERDUE_KICK_NS;	/* overdue+unserviced: bounded re-kick */
		else {
			ns = (d - now) * 1000000000ULL / microvm_tsc_freq();
			if (ns > timer_poll_ns)
				ns = timer_poll_ns;
		}

		pthread_mutex_lock(&timer_lock);
		if (!timer_rearm && ns > 0) {
			struct timespec ts;

			clock_gettime(CLOCK_MONOTONIC, &ts);
			ts.tv_sec += ns / 1000000000ULL;
			ts.tv_nsec += ns % 1000000000ULL;
			if (ts.tv_nsec >= 1000000000L) {
				ts.tv_sec++;
				ts.tv_nsec -= 1000000000L;
			}
			pthread_cond_timedwait(&timer_cond, &timer_lock, &ts);
		}
		timer_rearm = false;
		pthread_mutex_unlock(&timer_lock);

		fflush(stdout);		/* flush buffered console output */

		now = lapic_guest_tsc();
		for (i = 0; i < m->ncpus; i++) {
			uint64_t di;

			if (!m->vcpus[i].started)
				continue;
			di = lapic_next_deadline(i);
			if (di != 0 && di <= now)
				pthread_kill(m->vcpus[i].thread, SIGUSR1);
		}

		if (microvm_debug) {
			struct timespec t;

			clock_gettime(CLOCK_MONOTONIC, &t);
			if (t.tv_sec != last.tv_sec) {		/* ~1/s */
				last = t;
				fprintf(stderr, "[microvm] none=%lu io=%lu mem=%lu "
				    "rd=%lu wr=%lu cpuid=%lu intrdy=%lu halt=%lu "
				    "inject=%lu\n",
				    dbg_cnt[D_NONE], dbg_cnt[D_IO], dbg_cnt[D_MEM],
				    dbg_cnt[D_RDMSR], dbg_cnt[D_WRMSR],
				    dbg_cnt[D_CPUID], dbg_cnt[D_INTRDY],
				    dbg_cnt[D_HALT], dbg_cnt[D_INJECT]);
			}
		}
	}
	return NULL;
}

static void
start_ticker(struct microvm *m)
{
	struct sigaction sa;
	pthread_condattr_t ca;

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = sigusr1;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = 0;			/* no SA_RESTART: interrupt run */
	sigaction(SIGUSR1, &sa, NULL);

	sa.sa_handler = sig_stop;		/* graceful stop on ^C (non-raw)/TERM */
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);

	timer_poll_ns = BACKSTOP_NS;	/* input is event-driven; this is the net */

	pthread_condattr_init(&ca);
	pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
	pthread_cond_init(&timer_cond, &ca);
	pthread_condattr_destroy(&ca);
	timer_active = true;

	pthread_create(&ticker_thread, NULL, ticker, m);
}

/*
 * Input reader thread. Blocks in poll() on the host input fds (virtio tap /
 * console / vsock sockets, plus stdin when it is a tty) and, on readiness,
 * drains them under vm_lock - which pushes RX into the guest rings and raises
 * the device IRQ (waking the target vCPU via ioapic_raise -> vmm_wake_cpu). This
 * makes host->guest input event-driven: an idle guest costs nothing (the reader
 * is blocked in poll) yet sees input immediately, with no timer poll floor. The
 * fd set is re-collected each pass so vsock connections that come and go are
 * picked up; INPUT_RESCAN_MS also backstops any input source not in the set.
 */
static void *
input_reader(void *arg)
{
	struct microvm *m = arg;
	bool watch_stdin = isatty(STDIN_FILENO) != 0;

	while (running) {
		struct pollfd pfds[INPUT_FDS_MAX];
		int fds[INPUT_FDS_MAX];
		int nfds = 0, kn, i, r;

		if (watch_stdin) {
			pfds[nfds].fd = STDIN_FILENO;
			pfds[nfds].events = POLLIN;
			pfds[nfds].revents = 0;
			nfds++;
		}
		pthread_mutex_lock(&vm_lock);
		kn = virtio_collect_fds(fds, INPUT_FDS_MAX - nfds);
		pthread_mutex_unlock(&vm_lock);
		for (i = 0; i < kn; i++) {
			pfds[nfds].fd = fds[i];
			pfds[nfds].events = POLLIN;
			pfds[nfds].revents = 0;
			nfds++;
		}

		r = poll(pfds, (nfds_t)nfds, INPUT_RESCAN_MS);
		if (!running)
			break;
		if (r < 0 && errno != EINTR)
			break;			/* unexpected poll error: stop reader */

		/* Drain under the lock: device polls read the fds, push RX, and
		 * raise the IRQ (which wakes the owning vCPU). */
		pthread_mutex_lock(&vm_lock);
		virtio_poll_all();
		if (!m->want_console)
			uart_rx_poll();
		pthread_mutex_unlock(&vm_lock);

		/* A readable fd we could not fully drain (e.g. guest RX ring full)
		 * stays readable; back off briefly so we do not busy-spin. */
		if (r > 0) {
			struct timespec ts = { 0, 1000000 };	/* 1 ms */
			nanosleep(&ts, NULL);
		}
	}
	return NULL;
}

static void
start_input_reader(struct microvm *m)
{
	/* Only if there is an async host-input source worth watching. */
	if (m->net_tap == NULL && m->vsock_cid == 0 &&
	    isatty(STDIN_FILENO) == 0)
		return;
	if (pthread_create(&input_thread, NULL, input_reader, m) == 0)
		input_started = true;
}

/* -------------------------------------------------------------------------- */
/* Host terminal raw mode, for an interactive serial/virtio console. */

static struct termios saved_termios;
static bool termios_saved;

static void
term_restore(void)
{
	if (termios_saved)
		(void)tcsetattr(STDIN_FILENO, TCSANOW, &saved_termios);
}

static void
term_raw(void)
{
	struct termios raw;

	if (!isatty(STDIN_FILENO))
		return;			/* piped/redirected input: leave as-is */
	if (tcgetattr(STDIN_FILENO, &saved_termios) == -1)
		return;
	termios_saved = true;
	atexit(term_restore);		/* restore on any exit() path */
	raw = saved_termios;
	cfmakeraw(&raw);
	(void)tcsetattr(STDIN_FILENO, TCSANOW, &raw);
}

/*
 * Console escape handling.  Because the host tty is raw (no ISIG), Ctrl-C is
 * delivered to the guest as a byte rather than signalling us - so a panicked or
 * spinning guest can't be stopped from the keyboard.  Provide a QEMU-style
 * escape on the console input: Ctrl-A is a prefix; "Ctrl-A x" stops the VM and
 * "Ctrl-A Ctrl-A" sends a literal Ctrl-A to the guest.  Filters buf in place and
 * returns the number of bytes to forward to the guest.  Called from the console
 * RX readers (uart_rx_poll / virtio console poll), under vm_lock.
 */
#define	CONSOLE_ESC	0x01			/* Ctrl-A */
size_t
microvm_console_input(uint8_t *buf, size_t n)
{
	static int saw_esc;
	size_t i, o = 0;

	for (i = 0; i < n; i++) {
		uint8_t c = buf[i];

		if (saw_esc) {
			saw_esc = 0;
			switch (c) {
			case 'x': case 'X':	/* stop the VM */
				fprintf(stderr, "\r\n[microvm] stopped (^A x)\r\n");
				stop_vm();
				return o;
			case CONSOLE_ESC:	/* ^A ^A -> literal ^A */
				buf[o++] = CONSOLE_ESC;
				break;
			default:		/* unknown: pass the char through */
				buf[o++] = c;
				break;
			}
			continue;
		}
		if (c == CONSOLE_ESC) {		/* swallow prefix, await command */
			saw_esc = 1;
			continue;
		}
		buf[o++] = c;
	}
	return o;
}

/* -------------------------------------------------------------------------- */
/* Public API */

struct microvm *
microvm_create(const struct microvm_config *cfg)
{
	struct microvm *m;

	m = calloc(1, sizeof(*m));
	if (m == NULL)
		return NULL;
	m->cfg = *cfg;
	if (m->cfg.mem_bytes == 0)
		m->cfg.mem_bytes = GUEST_RAM_DEFAULT;
	m->ncpus = m->cfg.ncpus > 0 ? m->cfg.ncpus : 1;
	if (m->ncpus > MICROVM_MAXCPU)
		m->ncpus = MICROVM_MAXCPU;
	return m;
}

int
microvm_add_blk(struct microvm *m, const char *path)
{
	m->blk_path = path;
	return 0;
}

int
microvm_add_console(struct microvm *m)
{
	m->want_console = true;
	return 0;
}

int
microvm_add_net(struct microvm *m, const char *tap)
{
	m->net_tap = tap;
	return 0;
}

int
microvm_add_vsock(struct microvm *m, uint64_t guest_cid, const char *uds_path)
{
	if (guest_cid < 3)		/* 0=hyperv, 1=any, 2=host: reserved */
		return microvm_seterr(m, "vsock guest cid must be >= 3");
	m->vsock_cid = guest_cid;
	m->vsock_uds = uds_path;
	return 0;
}

int
microvm_load(struct microvm *m)
{
	uint64_t entry, start_info_gpa;
	size_t cl;
	int i;

	microvm_debug = m->cfg.debug;
	the_vm = m;

	if (m->cfg.kernel == NULL)
		return microvm_seterr(m, "no kernel specified");

	if (nvmm_init() == -1)
		return microvm_seterr(m,
		    "nvmm_init: %s (is the nvmm module loaded?)",
		    strerror(errno));
	if (nvmm_machine_create(&m->mach) == -1)
		return microvm_seterr(m, "nvmm_machine_create: %s",
		    strerror(errno));
	m->mach_created = true;

	for (i = 0; i < m->ncpus; i++) {
		struct vcpu *vc = &m->vcpus[i];
		vc->vm = m;
		vc->id = i;
		if (nvmm_vcpu_create(&m->mach, i, &vc->nvmm) == -1)
			return microvm_seterr(m, "nvmm_vcpu_create %d: %s", i,
			    strerror(errno));
		if (vcpu_setup(vc) != 0)
			return -1;
	}

	if (mem_setup(m, m->cfg.mem_bytes) != 0)
		return -1;
	uart_init();

	/* Assemble the command line: base + a virtio_mmio.device per device. */
	cl = (size_t)snprintf(m->cmdline, sizeof(m->cmdline), "%s",
	    m->cfg.cmdline ? m->cfg.cmdline : DEFAULT_CMDLINE);
	if (m->blk_path != NULL)
		cl += snprintf(m->cmdline + cl, sizeof(m->cmdline) - cl,
		    " virtio_mmio.device=0x200@0x%x:%d",
		    VIRTIO_BLK_MMIO_BASE, VIRTIO_BLK_IRQ);
	if (m->want_console)
		cl += snprintf(m->cmdline + cl, sizeof(m->cmdline) - cl,
		    " virtio_mmio.device=0x200@0x%x:%d",
		    VIRTIO_CON_MMIO_BASE, VIRTIO_CON_IRQ);
	if (m->net_tap != NULL)
		cl += snprintf(m->cmdline + cl, sizeof(m->cmdline) - cl,
		    " virtio_mmio.device=0x200@0x%x:%d",
		    VIRTIO_NET_MMIO_BASE, VIRTIO_NET_IRQ);
	if (m->vsock_cid != 0)
		cl += snprintf(m->cmdline + cl, sizeof(m->cmdline) - cl,
		    " virtio_mmio.device=0x200@0x%x:%d",
		    VIRTIO_VSOCK_MMIO_BASE, VIRTIO_VSOCK_IRQ);

	/* Instantiate devices. */
	if (m->blk_path != NULL &&
	    virtio_blk_init(m, VIRTIO_BLK_MMIO_BASE, VIRTIO_BLK_IRQ,
	    m->blk_path) != 0)
		return -1;
	if (m->want_console &&
	    virtio_console_init(m, VIRTIO_CON_MMIO_BASE, VIRTIO_CON_IRQ) != 0)
		return -1;
	if (m->net_tap != NULL &&
	    virtio_net_init(m, VIRTIO_NET_MMIO_BASE, VIRTIO_NET_IRQ,
	    m->net_tap) != 0)
		return -1;
	if (m->vsock_cid != 0 &&
	    virtio_vsock_init(m, VIRTIO_VSOCK_MMIO_BASE, VIRTIO_VSOCK_IRQ,
	    m->vsock_cid, m->vsock_uds) != 0)
		return -1;

	m->tsc_freq = microvm_tsc_freq();

	if (pvh_load(m, &entry, &start_info_gpa) != 0)
		return -1;
	/* Place the BSP at the PVH entry; APs start parked (started via SIPI). */
	if (vcpu_set_pvh_state(&m->vcpus[0], entry, start_info_gpa) != 0)
		return -1;

	/* NVMM passes the TSC through (offset 0): guest TSC == host rdtsc(). */
	lapic_init(m->ncpus, m->tsc_freq, 0);
	ioapic_init();
	if (mptable_build(m) != 0)
		return -1;

	return 0;
}

/* The per-vCPU execution loop (one of these runs per vCPU thread). */
static int
vcpu_run(struct vcpu *vc)
{
	struct nvmm_machine *mach = &vc->vm->mach;
	struct nvmm_vcpu_exit *exit = vc->nvmm.exit;
	int rc = 0;

	while (running) {
		int r = nvmm_vcpu_run(mach, &vc->nvmm);

		/*
		 * Hold vm_lock for the whole exit-handling section (device
		 * models, IOAPIC, cross-vCPU LAPIC IRR) - never across
		 * nvmm_vcpu_run, where the vCPU is in-guest.
		 */
		pthread_mutex_lock(&vm_lock);
		if (r == -1) {
			if (errno != EINTR) {
				rc = microvm_seterr(vc->vm, "nvmm_vcpu_run: %s",
				    strerror(errno));
				running = false;
			}
			pthread_mutex_unlock(&vm_lock);
			continue;
		}

		switch (exit->reason) {
		case NVMM_VCPU_EXIT_NONE:
			dbg_cnt[D_NONE]++;
			break;
		case NVMM_VCPU_EXIT_IO:
			dbg_cnt[D_IO]++;
			if (nvmm_assist_io(mach, &vc->nvmm) == -1) {
				rc = microvm_seterr(vc->vm, "assist_io: %s",
				    strerror(errno));
				running = false;
			}
			break;
		case NVMM_VCPU_EXIT_MEMORY:
			dbg_cnt[D_MEM]++;
			if (nvmm_assist_mem(mach, &vc->nvmm) == -1) {
				rc = microvm_seterr(vc->vm, "assist_mem: %s",
				    strerror(errno));
				running = false;
			}
			break;
		case NVMM_VCPU_EXIT_RDMSR:
			dbg_cnt[D_RDMSR]++;
			if (handle_rdmsr(vc) != 0) {
				rc = -1;
				running = false;
			}
			break;
		case NVMM_VCPU_EXIT_WRMSR:
			dbg_cnt[D_WRMSR]++;
			if (handle_wrmsr(vc) != 0) {
				rc = -1;
				running = false;
			}
			break;
		case NVMM_VCPU_EXIT_CPUID:
			dbg_cnt[D_CPUID]++;
			if (handle_cpuid(vc) != 0) {
				rc = -1;
				running = false;
			}
			break;
		case NVMM_VCPU_EXIT_INT_READY:
			dbg_cnt[D_INTRDY]++;
			vc->int_window_requested = false;  /* NVMM cleared it */
			break;
		case NVMM_VCPU_EXIT_HALTED:
			dbg_cnt[D_HALT]++;
			/*
			 * HLT with interrupts enabled is normal idle: wait for
			 * the next timer. HLT with interrupts disabled means the
			 * guest stopped itself (poweroff/halt/panic) and nothing
			 * can wake it - exit cleanly.
			 */
			if (exit->exitstate.rflags & PSL_I) {
				/* Sleep without vm_lock so other vCPUs run. */
				pthread_mutex_unlock(&vm_lock);
				wait_for_timer(vc);
				pthread_mutex_lock(&vm_lock);
			} else
				stop_vm();
			break;
		case NVMM_VCPU_EXIT_SHUTDOWN:
			stop_vm();
			break;
		case NVMM_VCPU_EXIT_INVALID:
		default:
			if (nvmm_vcpu_getstate(mach, &vc->nvmm,
			    NVMM_X64_STATE_GPRS) == -1)
				rc = microvm_seterr(vc->vm, "cpu%d unhandled "
				    "exit 0x%llx (getstate failed: %s)", vc->id,
				    (unsigned long long)exit->reason,
				    strerror(errno));
			else
				rc = microvm_seterr(vc->vm, "cpu%d unhandled "
				    "exit 0x%llx hwcode=0x%llx at rip=0x%llx",
				    vc->id, (unsigned long long)exit->reason,
				    (unsigned long long)exit->u.inv.hwcode,
				    (unsigned long long)
				    vc->nvmm.state->gprs[NVMM_X64_GPR_RIP]);
			running = false;
			break;
		}

		if (running && deliver_pending(vc) != 0) {
			rc = -1;
			running = false;
		}
		if (running) {
			virtio_poll_all();	/* pump host->guest device input */
			/*
			 * Feed the serial console from stdin, unless a
			 * virtio-console owns it (avoid double-consuming input).
			 */
			if (!vc->vm->want_console)
				uart_rx_poll();
		}
		pthread_mutex_unlock(&vm_lock);
	}
	return rc;
}

/* Per-vCPU thread: APs park until released (BSP runs immediately; APs via the
 * SIPI path), then run the vCPU loop. */
static void *
vcpu_thread(void *arg)
{
	struct vcpu *vc = arg;

	pthread_mutex_lock(&vm_lock);
	while (running && !vc->started)
		pthread_cond_wait(&ap_cond, &vm_lock);
	pthread_mutex_unlock(&vm_lock);

	if (running)
		vc->rc = vcpu_run(vc);
	return NULL;
}

int
microvm_run(struct microvm *m)
{
	int i;

	running = true;
	m->vcpus[0].started = true;		/* BSP runs immediately */
	term_raw();

	for (i = 0; i < m->ncpus; i++)
		pthread_create(&m->vcpus[i].thread, NULL, vcpu_thread,
		    &m->vcpus[i]);
	start_ticker(m);			/* after thread handles exist */
	start_input_reader(m);			/* host-input poll thread (if any) */

	/*
	 * A vCPU (or a signal) clears `running` on shutdown. Join the ticker and
	 * input reader first: both pthread_kill() the vCPU thread handles (the
	 * ticker per deadline, the reader on input via wake-on-raise), so they
	 * must stop before we join (and thus invalidate) those handles.
	 */
	pthread_join(ticker_thread, NULL);
	timer_active = false;		/* cond no longer serviced */
	if (input_started)
		pthread_join(input_thread, NULL);

	/* Release parked APs and kick everyone out of nvmm_vcpu_run, then join. */
	running = false;
	pthread_mutex_lock(&vm_lock);
	pthread_cond_broadcast(&ap_cond);
	pthread_mutex_unlock(&vm_lock);
	for (i = 0; i < m->ncpus; i++)
		pthread_kill(m->vcpus[i].thread, SIGUSR1);
	for (i = 0; i < m->ncpus; i++)
		pthread_join(m->vcpus[i].thread, NULL);

	term_restore();
	return m->vcpus[0].rc;
}

void
microvm_destroy(struct microvm *m)
{
	if (m == NULL)
		return;
	virtio_teardown_all();		/* close device fds, free backends */
	if (m->mach_created)
		nvmm_machine_destroy(&m->mach);	/* drops the gpa mappings */
	if (m->mem.hva != NULL)
		munmap(m->mem.hva, m->mem.size);
	free(m);
}
