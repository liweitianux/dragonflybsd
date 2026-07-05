/*
 * libmicrovm - a tiny NVMM-based PVH microVM monitor, as an embeddable library.
 *
 * Boots a PVH-capable kernel (Linux CONFIG_PVH=y, or anything exposing
 * XEN_ELFNOTE_PHYS32_ENTRY) as a lightweight virtual machine on top of libnvmm:
 * contiguous RAM from GPA 0, a userspace LAPIC/IOAPIC + timer, an 8250 serial
 * console, and virtio-mmio block/console/net devices. No firmware, no PCI, no
 * ACPI.
 *
 * Typical use:
 *
 *	struct microvm_config cfg = {
 *		.mem_bytes = 512ULL << 20,
 *		.kernel    = "vmlinux",
 *		.cmdline   = "console=ttyS0 root=/dev/vda rw",
 *	};
 *	struct microvm *vm = microvm_create(&cfg);
 *	microvm_add_blk(vm, "rootfs.img");
 *	if (microvm_load(vm) != 0)
 *		errx(1, "%s", microvm_error(vm));
 *	microvm_run(vm);
 *	microvm_destroy(vm);
 *
 * NOTE: device-model state is currently process-global, so one microVM may be
 * run per process (matches xhyve). x86-64 only (NVMM has no aarch64 backend).
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

#ifndef _MICROVM_H_
#define _MICROVM_H_

#include <stdint.h>
#include <stddef.h>

struct microvm;			/* opaque VM handle */

struct microvm_config {
	uint64_t	mem_bytes;	/* guest RAM size (0 => default 256 MiB) */
	int		ncpus;		/* number of vCPUs (0 => 1) */
	const char	*kernel;	/* path to a PVH vmlinux (required) */
	const char	*initramfs;	/* optional cpio(.gz) initramfs */
	const char	*cmdline;	/* kernel command line (may be NULL) */
	int		debug;		/* nonzero: emit diagnostics to stderr */
	int		virtio_legacy;	/* nonzero: present virtio-mmio legacy (v1)
					 * instead of modern (v2). Needed by guests
					 * whose mmio driver is v1-only (e.g. the
					 * DragonFly virtio-mmio transport); Linux
					 * speaks both. Default 0 (v2). */
};

/* Allocate a VM and copy the config. Returns NULL on allocation failure. */
struct microvm *microvm_create(const struct microvm_config *);

/* Attach devices (call before microvm_load). Return 0 on success, -1 on error. */
int microvm_add_blk(struct microvm *, const char *path);	/* -> /dev/vda */
int microvm_add_console(struct microvm *);			/* -> hvc0 */
int microvm_add_net(struct microvm *, const char *tap);		/* -> eth0 */
/* virtio-vsock: guest gets CID guest_cid (>=3); guest streams to (cid=2,port P)
 * are bridged to the host AF_UNIX socket "<uds_path>_<P>". */
int microvm_add_vsock(struct microvm *, uint64_t guest_cid, const char *uds_path);

/* Create the NVMM machine, load the kernel, wire devices and the interrupt
 * controller, and place the vCPU at the PVH entry. Returns 0, or -1 with the
 * reason available from microvm_error(). */
int microvm_load(struct microvm *);

/* Run the vCPU until the guest shuts down or an unhandled exit occurs.
 * Returns 0 on clean shutdown, -1 on error. */
int microvm_run(struct microvm *);

/* Tear down and free. */
void microvm_destroy(struct microvm *);

/* Human-readable description of the last failure on this handle. */
const char *microvm_error(const struct microvm *);

#endif /* _MICROVM_H_ */
