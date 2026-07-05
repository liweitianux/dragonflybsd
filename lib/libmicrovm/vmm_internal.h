/*
 * libmicrovm internal definitions (not installed).
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

#ifndef _MICROVM_INTERNAL_H_
#define _MICROVM_INTERNAL_H_

#include <sys/param.h>		/* PAGE_SIZE, rounddown(), roundup() */

#include <nvmm.h>

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "microvm.h"

#define MICROVM_MAXCPU	32

/*
 * Guest physical memory layout: low conventional RAM holds the boot blocks; the
 * kernel loads at its ELF p_paddr (typically 16 MiB); the initramfs goes just
 * past the kernel image.
 */
#define GUEST_RAM_DEFAULT	(256ULL * 1024 * 1024)
#define LOWMEM_TOP		0x9fc00		/* top of conventional RAM */
#define BOOTINFO_CMDLINE_GPA	0x00010000
#define BOOTINFO_START_GPA	0x00011000	/* hvm_start_info */
#define BOOTINFO_MEMMAP_GPA	0x00012000	/* hvm_memmap_table_entry[] */
#define BOOTINFO_MODLIST_GPA	0x00013000	/* hvm_modlist_entry[] */

/* 8250 UART (COM1). */
#define UART_IOBASE		0x3f8
#define UART_IOSIZE		8

/* One contiguous host allocation mapped at GPA 0. */
struct guest_mem {
	uint8_t *hva;
	uint64_t size;
};

/* Per-vCPU state. */
struct vcpu {
	struct microvm *vm;		/* back-pointer */
	struct nvmm_vcpu nvmm;		/* NVMM vCPU (state/event/exit) */
	int id;				/* cpuid 0..ncpus-1 */
	pthread_t thread;		/* per-vCPU run thread */
	volatile bool started;		/* released from parked state (BSP/SIPI);
					 * read locklessly by the ticker */
	bool int_window_requested;
	int rc;				/* vcpu_run() return value */
};

struct microvm {
	struct nvmm_machine mach;
	bool mach_created;
	int ncpus;
	struct vcpu vcpus[MICROVM_MAXCPU];
	struct guest_mem mem;

	struct microvm_config cfg;

	/* Requested devices (instantiated in microvm_load). */
	const char *blk_path;
	const char *net_tap;
	bool want_console;
	uint64_t vsock_cid;	/* 0 = no vsock device */
	const char *vsock_uds;	/* host AF_UNIX path base, or NULL */

	char cmdline[1024];	/* base cmdline + appended virtio_mmio.device= */
	uint64_t tsc_freq;
	char errbuf[256];
};

/* vmm.c */
extern int microvm_debug;	/* set from microvm_config.debug in microvm_load */
void *gpa_to_hva(struct microvm *, uint64_t gpa, uint64_t size);
int microvm_seterr(struct microvm *, const char *fmt, ...);
uint64_t microvm_tsc_freq(void);
/* Deliver an IPI from src_cpu, decoded from the LAPIC ICR low/high dwords. */
void vmm_ipi(int src_cpu, uint32_t icr_lo, uint32_t icr_hi);
/* Wake the deadline-armed timer thread to recompute after a LAPIC timer is
 * (re)armed, so it does not oversleep past a newly-armed earlier deadline. */
void vmm_timer_rearm(void);
/* Wake a vCPU that just had an interrupt raised into its LAPIC IRR (event-driven
 * delivery: kick a halted target out of nvmm_vcpu_run so it injects promptly).
 * No-op if cpu is the caller or is not running. */
void vmm_wake_cpu(int cpu);

/* loader.c */
int pvh_load(struct microvm *, uint64_t *entry, uint64_t *start_info_gpa);

/* uart.c */
void uart_init(void);
void uart_io(uint16_t port, bool in, uint8_t *data, size_t size);
void uart_rx_poll(void);

/* Console escape filter (^A x = stop the VM, ^A ^A = literal ^A); returns the
 * number of bytes to forward to the guest after stripping any escape. */
size_t microvm_console_input(uint8_t *buf, size_t n);

#endif /* _MICROVM_INTERNAL_H_ */
