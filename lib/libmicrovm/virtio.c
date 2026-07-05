/*
 * virtio-mmio transport + split-virtqueue engine. See virtio.h.
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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "virtio.h"
#include "ioapic.h"

/* virtio-mmio v2 register offsets. */
#define R_MAGIC		0x000
#define R_VERSION	0x004
#define R_DEVICEID	0x008
#define R_VENDORID	0x00c
#define R_DEVFEAT	0x010
#define R_DEVFEATSEL	0x014
#define R_DRVFEAT	0x020
#define R_DRVFEATSEL	0x024
#define R_GUESTPAGESZ	0x028	/* legacy (v1) only */
#define R_QUEUESEL	0x030
#define R_QUEUENUMMAX	0x034
#define R_QUEUENUM	0x038
#define R_QUEUEALIGN	0x03c	/* legacy (v1) only */
#define R_QUEUEPFN	0x040	/* legacy (v1) only */
#define R_QUEUEREADY	0x044
#define R_QUEUENOTIFY	0x050
#define R_INTSTATUS	0x060
#define R_INTACK	0x064
#define R_STATUS	0x070
#define R_QDESCLO	0x080
#define R_QDESCHI	0x084
#define R_QDRVLO	0x090
#define R_QDRVHI	0x094
#define R_QDEVLO	0x0a0
#define R_QDEVHI	0x0a4
#define R_CFGGEN	0x0fc
#define R_CONFIG	0x100

#define MMIO_WINDOW	0x200

#define MAX_DEVS	8
static struct virtio_dev *devs[MAX_DEVS];
static int ndevs;

void *
virtio_gpa(struct virtio_dev *d, uint64_t gpa, uint32_t len)
{
	return gpa_to_hva(d->vm, gpa, len);
}

void
virtio_notify(struct virtio_dev *d)
{
	d->isr |= 1;
	ioapic_raise(d->irq);
}

struct virtio_dev *
virtio_mmio_create(struct microvm *vm, uint64_t base, int irq, uint32_t device_id,
    int nvq, const struct virtio_ops *ops, void *priv)
{
	struct virtio_dev *d;

	if (ndevs == MAX_DEVS)
		return NULL;			/* too many devices */
	d = calloc(1, sizeof(*d));
	if (d == NULL)
		return NULL;			/* out of memory */
	d->vm = vm;
	d->mmio_base = base;
	d->irq = irq;
	d->version = vm->cfg.virtio_legacy ? 1 : 2;
	d->device_id = device_id;
	d->nvq = nvq;
	d->ops = ops;
	d->priv = priv;
	/* Modern offers VIRTIO_F_VERSION_1; legacy must not (it predates it). */
	d->host_features = (d->version == 2) ? (1ULL << VIRTIO_F_VERSION_1) : 0;
	devs[ndevs++] = d;
	return d;
}

/*
 * Legacy (v1) queue setup: the driver gives a single page-frame number plus a
 * page size and used-ring alignment; the three rings are laid out contiguously
 * from PFN*page_size (desc, then avail, then used aligned up). Modern (v2) by
 * contrast supplies each ring's address directly. Once located we set the same
 * desc/avail/used GPAs the shared engine uses, so nothing downstream changes.
 */
static void
legacy_setup_queue(struct virtio_dev *d, struct virtqueue *vq)
{
	uint64_t base, avail;
	uint32_t align;

	if (vq->pfn == 0 || vq->num == 0) {	/* teardown / not ready */
		vq->ready = 0;
		return;
	}
	align = vq->align ? vq->align : 4096;
	base = (uint64_t)vq->pfn * d->guest_page_size;
	avail = base + 16ULL * vq->num;			/* after desc table */
	/* avail ring = flags+idx+ring[num]+used_event = 6 + 2*num bytes */
	vq->desc_gpa = base;
	vq->avail_gpa = avail;
	vq->used_gpa = (avail + 6 + 2ULL * vq->num + align - 1) & ~((uint64_t)align - 1);
	vq->ready = 1;
}

/* Walk the descriptor chain at head into an sg list; returns nsg. */
static int
build_sg(struct virtio_dev *d, struct virtqueue *vq, uint16_t head,
    struct vq_sg *sg, int maxsg)
{
	struct vring_desc *desc =
	    virtio_gpa(d, vq->desc_gpa, vq->num * sizeof(struct vring_desc));
	uint16_t i = head;
	int n = 0;

	if (desc == NULL)
		return 0;
	for (;;) {
		if (n >= maxsg || i >= vq->num)
			break;
		sg[n].buf = virtio_gpa(d, desc[i].addr, desc[i].len);
		sg[n].len = desc[i].len;
		sg[n].write = (desc[i].flags & VRING_DESC_F_WRITE) != 0;
		if (sg[n].buf == NULL) {
			if (microvm_debug)
				fprintf(stderr, "[sg] unmapped desc gpa=0x%llx "
				    "len=%u flags=0x%x at n=%d (chain cut)\n",
				    (unsigned long long)desc[i].addr,
				    desc[i].len, desc[i].flags, n);
			break;
		}
		n++;
		if (!(desc[i].flags & VRING_DESC_F_NEXT))
			break;
		i = desc[i].next;
	}
	return n;
}

static void
process_vq(struct virtio_dev *d, int qidx)
{
	struct virtqueue *vq = &d->vq[qidx];
	struct vring_avail *avail;
	struct vring_used *used;
	struct vq_sg sg[VIRTQ_MAX];	/* a chain can be up to the queue size */
	bool did = false;

	if (!vq->ready || vq->num == 0 || qidx >= d->nvq)
		return;
	if (d->passive_qmask & (1u << qidx))
		return;			/* RX queue: filled by host I/O, not here */
	avail = virtio_gpa(d, vq->avail_gpa, 0x1000);
	used = virtio_gpa(d, vq->used_gpa, 0x1000);
	if (avail == NULL || used == NULL)
		return;

	while (vq->last_avail != avail->idx) {
		uint16_t head = avail->ring[vq->last_avail % vq->num];
		int nsg = build_sg(d, vq, head, sg, VIRTQ_MAX);
		uint32_t ulen = 0;

		if (nsg > 0 && d->ops->request != NULL)
			ulen = d->ops->request(d, qidx, sg, nsg);

		used->ring[used->idx % vq->num].id = head;
		used->ring[used->idx % vq->num].len = ulen;
		__sync_synchronize();
		used->idx++;
		vq->last_avail++;
		did = true;
	}
	if (did)
		virtio_notify(d);
}

uint32_t
virtio_rx_push(struct virtio_dev *d, int qidx, const void *data, uint32_t len)
{
	struct virtqueue *vq = &d->vq[qidx];
	struct vring_avail *avail;
	struct vring_used *used;
	struct vq_sg sg[16];
	uint16_t head;
	uint32_t copied = 0;
	int nsg, j;

	if (qidx >= d->nvq || !vq->ready || vq->num == 0)
		return 0;
	avail = virtio_gpa(d, vq->avail_gpa, 0x1000);
	used = virtio_gpa(d, vq->used_gpa, 0x1000);
	if (avail == NULL || used == NULL || vq->last_avail == avail->idx)
		return 0;			/* no guest buffer available */

	head = avail->ring[vq->last_avail % vq->num];
	nsg = build_sg(d, vq, head, sg, 16);
	for (j = 0; j < nsg && copied < len; j++) {
		uint32_t n;
		if (!sg[j].write)
			continue;
		n = len - copied;
		if (n > sg[j].len)
			n = sg[j].len;
		memcpy(sg[j].buf, (const uint8_t *)data + copied, n);
		copied += n;
	}

	used->ring[used->idx % vq->num].id = head;
	used->ring[used->idx % vq->num].len = copied;
	__sync_synchronize();
	used->idx++;
	vq->last_avail++;
	virtio_notify(d);
	return copied;
}

void
virtio_poll_all(void)
{
	int i;

	for (i = 0; i < ndevs; i++)
		if (devs[i]->ops->poll != NULL)
			devs[i]->ops->poll(devs[i]);
}

int
virtio_collect_fds(int *fds, int max)
{
	int i, n = 0;

	for (i = 0; i < ndevs && n < max; i++)
		if (devs[i]->ops->collect_fds != NULL)
			n += devs[i]->ops->collect_fds(devs[i], fds + n, max - n);
	return n;
}

void
virtio_teardown_all(void)
{
	int i;

	for (i = 0; i < ndevs; i++) {
		if (devs[i]->ops->destroy != NULL)
			devs[i]->ops->destroy(devs[i]);
		free(devs[i]);
	}
	ndevs = 0;
}

/* -------------------------------------------------------------------------- */

static uint32_t
mmio_read(struct virtio_dev *d, uint32_t off)
{
	struct virtqueue *vq = &d->vq[d->queue_sel];

	switch (off) {
	case R_MAGIC:		return VIRTIO_MMIO_MAGIC;
	case R_VERSION:		return d->version;
	case R_DEVICEID:	return d->device_id;
	case R_VENDORID:	return 0x44464c59;	/* "DFLY" */
	case R_DEVFEAT:
		return (uint32_t)(d->host_features >> (d->features_sel * 32));
	case R_QUEUENUMMAX:	return VIRTQ_MAX;
	case R_QUEUEPFN:	return vq->pfn;		/* legacy (v1) */
	case R_QUEUEREADY:
		return (d->queue_sel < VIRTIO_MAX_VQ) ? vq->ready : 0;
	case R_INTSTATUS:	return d->isr;
	case R_STATUS:		return d->status;
	case R_CFGGEN:		return d->config_generation;
	default:
		if (off >= R_CONFIG && d->config != NULL) {
			uint32_t i = off - R_CONFIG, v = 0;
			if (i + 4 <= d->config_len)
				memcpy(&v, d->config + i, 4);
			return v;
		}
		return 0;
	}
}

static void
mmio_write(struct virtio_dev *d, uint32_t off, uint32_t val)
{
	struct virtqueue *vq = &d->vq[d->queue_sel];

	switch (off) {
	case R_DEVFEATSEL:	d->features_sel = val; break;
	case R_DRVFEATSEL:	d->guest_features_sel = val; break;
	case R_DRVFEAT:
		d->guest_features |= (uint64_t)val << (d->guest_features_sel * 32);
		break;
	case R_QUEUESEL:	if (val < VIRTIO_MAX_VQ) d->queue_sel = val; break;
	case R_QUEUENUM:	vq->num = (val > VIRTQ_MAX) ? 0 : val; break;
	case R_GUESTPAGESZ:	d->guest_page_size = val; break;	/* legacy */
	case R_QUEUEALIGN:	vq->align = val; break;		/* legacy */
	case R_QUEUEPFN:	vq->pfn = val; legacy_setup_queue(d, vq); break;
	case R_QUEUEREADY:	vq->ready = val; break;		/* modern */
	case R_QUEUENOTIFY:	process_vq(d, val); break;
	case R_INTACK:		d->isr &= ~val; break;
	case R_STATUS:
		if (microvm_debug)
			fprintf(stderr, "[virtio] dev@0x%llx id=%u status 0x%x->0x%x\n",
			    (unsigned long long)d->mmio_base, d->device_id,
			    d->status, val);
		d->status = val;
		if (val == 0) {		/* reset */
			int i;
			for (i = 0; i < VIRTIO_MAX_VQ; i++)
				memset(&d->vq[i], 0, sizeof(d->vq[i]));
			d->isr = 0;
		}
		break;
	case R_QDESCLO:	vq->desc_gpa = (vq->desc_gpa & ~0xffffffffULL) | val; break;
	case R_QDESCHI:	vq->desc_gpa = (vq->desc_gpa & 0xffffffffULL) | ((uint64_t)val << 32); break;
	case R_QDRVLO:	vq->avail_gpa = (vq->avail_gpa & ~0xffffffffULL) | val; break;
	case R_QDRVHI:	vq->avail_gpa = (vq->avail_gpa & 0xffffffffULL) | ((uint64_t)val << 32); break;
	case R_QDEVLO:	vq->used_gpa = (vq->used_gpa & ~0xffffffffULL) | val; break;
	case R_QDEVHI:	vq->used_gpa = (vq->used_gpa & 0xffffffffULL) | ((uint64_t)val << 32); break;
	default:
		if (off >= R_CONFIG && d->config != NULL) {
			uint32_t i = off - R_CONFIG;
			if (i + 4 <= d->config_len)
				memcpy(d->config + i, &val, 4);
		}
		break;
	}
}

bool
virtio_mmio(uint64_t gpa, bool write, uint8_t *data, size_t size)
{
	int i;

	for (i = 0; i < ndevs; i++) {
		struct virtio_dev *d = devs[i];
		uint32_t off, val;

		if (gpa < d->mmio_base || gpa >= d->mmio_base + MMIO_WINDOW)
			continue;
		off = (uint32_t)(gpa - d->mmio_base);

		if (write) {
			val = 0;
			memcpy(&val, data, size > 4 ? 4 : size);
			mmio_write(d, off & ~0x3u, val);
		} else {
			val = mmio_read(d, off & ~0x3u);
			/* honour sub-word config reads */
			if (off & 0x3)
				val >>= (off & 0x3) * 8;
			memset(data, 0, size);
			memcpy(data, &val, size > 4 ? 4 : size);
		}
		return true;
	}
	return false;
}
