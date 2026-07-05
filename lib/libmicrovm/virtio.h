/*
 * virtio-mmio transport + split-virtqueue engine. Presents either the modern
 * (version 2) or legacy (version 1) mmio interface per microvm_config; the
 * split-virtqueue engine is shared (only how the queue is located differs).
 *
 * Devices are discovered by the guest via the kernel command line
 * "virtio_mmio.device=<size>@<base>:<irq>". Each device gets an MMIO window and
 * an IOAPIC IRQ line; the virtqueue engine walks the split rings in guest RAM
 * and the device backend processes each descriptor chain.
 *
 * The virtqueue/device-model structure is implemented from the VIRTIO 1.x
 * specification. The split-ring layout (struct vring_desc/avail/used) is the
 * canonical virtio ABI from virtio_ring.h (Copyright Rusty Russell, IBM Corp.;
 * BSD-licensed).
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

#ifndef _MICROVM_VIRTIO_H_
#define _MICROVM_VIRTIO_H_

#include "vmm_internal.h"

/*
 * virtio ABI constants: device IDs, status bits, and vring descriptor flags.
 * These mirror the guest-driver headers <dev/virtual/virtio/virtio/virtio.h>
 * and <.../virtio_ring.h>, which are kernel-only and not installed.
 */
#define VIRTIO_MMIO_MAGIC	0x74726976	/* "virt" */

/* Device IDs. */
#define VIRTIO_ID_NET		1
#define VIRTIO_ID_BLOCK		2
#define VIRTIO_ID_CONSOLE	3
#define VIRTIO_ID_VSOCK		19

/* Status bits. */
#define VIRTIO_STAT_ACK		1
#define VIRTIO_STAT_DRIVER	2
#define VIRTIO_STAT_DRIVER_OK	4
#define VIRTIO_STAT_FEATURES_OK	8
#define VIRTIO_STAT_FAILED	0x80

/* Always-offered feature: virtio 1.0 (modern). Bit 32. */
#define VIRTIO_F_VERSION_1	32

/* Split-ring descriptor flags. */
#define VRING_DESC_F_NEXT	1
#define VRING_DESC_F_WRITE	2	/* device writes (read for guest) */
#define VRING_DESC_F_INDIRECT	4

#define VIRTQ_MAX		256

struct vring_desc {
	uint64_t addr;
	uint32_t len;
	uint16_t flags;
	uint16_t next;
} __attribute__((packed));

struct vring_avail {
	uint16_t flags;
	uint16_t idx;
	uint16_t ring[];
} __attribute__((packed));

struct vring_used_elem {
	uint32_t id;
	uint32_t len;
} __attribute__((packed));

struct vring_used {
	uint16_t flags;
	uint16_t idx;
	struct vring_used_elem ring[];
} __attribute__((packed));

struct virtqueue {
	uint32_t num;			/* negotiated size */
	uint32_t ready;
	uint64_t desc_gpa, avail_gpa, used_gpa;
	uint16_t last_avail;		/* next avail index we will consume */
	uint32_t pfn;			/* legacy (v1) QueuePFN, for read-back */
	uint32_t align;			/* legacy (v1) QueueAlign for used ring */
};

/* One scatter/gather element of a descriptor chain (host pointer view). */
struct vq_sg {
	void *buf;
	uint32_t len;
	bool write;			/* device-writable buffer */
};

struct virtio_dev;

struct virtio_ops {
	/* Process one descriptor chain on a notified (active) queue; return
	 * bytes written into device-writable buffers (the used "len"). */
	uint32_t (*request)(struct virtio_dev *, int qidx,
	    struct vq_sg *sg, int nsg);
	/* Pump host->guest input into RX (passive) queues. Called from the
	 * main loop; should do non-blocking I/O and virtio_rx_push(). */
	void (*poll)(struct virtio_dev *);
	/* Release backend priv (fds, memory). Called once at VM teardown. */
	void (*destroy)(struct virtio_dev *);
	/* Optional: append this backend's readable host fds (tap/socket) to fds[]
	 * for the input reader thread to poll; returns the count appended. */
	int (*collect_fds)(struct virtio_dev *, int *fds, int max);
};

#define VIRTIO_MAX_VQ	3	/* vsock uses 3 (rx, tx, event) */

struct virtio_dev {
	struct microvm *vm;
	uint64_t mmio_base;
	int irq;
	int version;			/* mmio transport: 1 (legacy) or 2 (modern) */
	uint32_t guest_page_size;	/* legacy (v1) GuestPageSize */
	uint32_t device_id;
	uint64_t host_features;		/* offered */
	uint64_t guest_features;	/* negotiated */
	uint32_t features_sel, guest_features_sel;
	uint32_t status;
	uint32_t queue_sel;
	uint32_t isr;			/* InterruptStatus */
	uint32_t config_generation;
	struct virtqueue vq[VIRTIO_MAX_VQ];
	int nvq;
	uint32_t passive_qmask;		/* queues filled by host I/O (RX) */
	const struct virtio_ops *ops;
	uint8_t *config;		/* device config space */
	uint32_t config_len;
	void *priv;			/* backend state */
};

/* Registration / dispatch. Returns NULL on OOM or too many devices. */
struct virtio_dev *virtio_mmio_create(struct microvm *, uint64_t base, int irq,
    uint32_t device_id, int nvq, const struct virtio_ops *ops, void *priv);
bool virtio_mmio(uint64_t gpa, bool write, uint8_t *data, size_t size);

/* Backend helpers. */
void *virtio_gpa(struct virtio_dev *, uint64_t gpa, uint32_t len);
void virtio_notify(struct virtio_dev *);	/* raise used-buffer interrupt */

/* Deliver a host->guest buffer into a passive (RX) queue. Copies up to len
 * bytes into the next available descriptor chain; returns bytes delivered, or
 * 0 if no guest buffer is available (caller should drop/retry). */
uint32_t virtio_rx_push(struct virtio_dev *, int qidx, const void *data,
    uint32_t len);

/* Pump host input for all devices (call from the main loop). */
void virtio_poll_all(void);

/* Collect every backend's readable host fds into fds[] (call under vm_lock;
 * returns the count). Used by the input reader thread to poll for host input. */
int virtio_collect_fds(int *fds, int max);

/* Release every registered device and its backend (VM teardown). */
void virtio_teardown_all(void);

/* Backend init. */
int virtio_blk_init(struct microvm *, uint64_t base, int irq, const char *path);
int virtio_console_init(struct microvm *, uint64_t base, int irq);
int virtio_net_init(struct microvm *, uint64_t base, int irq, const char *tap);
int virtio_vsock_init(struct microvm *, uint64_t base, int irq,
    uint64_t guest_cid, const char *uds_path);

#endif /* _MICROVM_VIRTIO_H_ */
