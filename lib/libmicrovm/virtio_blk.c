/*
 * virtio-blk backend: serves a file-backed block device over one virtqueue.
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
#include <sys/stat.h>

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "virtio.h"

/*
 * virtio-blk request types and status codes, mirroring
 * <dev/virtual/virtio/block/virtio_blk.h> (kernel-only, not installed).
 */
#define VIRTIO_BLK_T_IN		0	/* read */
#define VIRTIO_BLK_T_OUT	1	/* write */

#define VIRTIO_BLK_S_OK		0
#define VIRTIO_BLK_S_IOERR	1

struct virtio_blk_outhdr {
	uint32_t type;
	uint32_t reserved;
	uint64_t sector;
} __attribute__((packed));

struct blk {
	int fd;
	uint8_t config[8];	/* capacity in 512-byte sectors (LE) */
};

/*
 * pread/pwrite that completes the whole transfer. A bare pread/pwrite may
 * return a short count or be cut short by a signal (EINTR) - harmless on a
 * cache hit, but on bare metal the backing image is not resident, so the I/O
 * actually blocks on disk and is far more likely to be interrupted or split.
 * Treating that as failure surfaces in the guest as a spurious I/O error, so
 * loop until the request is fully satisfied (or a real error / EOF).
 */
static ssize_t
blk_rw(int fd, void *buf, size_t len, off_t off, int write)
{
	size_t done = 0;

	while (done < len) {
		ssize_t r = write ?
		    pwrite(fd, (char *)buf + done, len - done, off + done) :
		    pread(fd, (char *)buf + done, len - done, off + done);
		if (r < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		if (r == 0)			/* EOF (read past end of image) */
			break;
		done += (size_t)r;
	}
	return (ssize_t)done;
}

static uint32_t
blk_request(struct virtio_dev *d, int qidx, struct vq_sg *sg, int nsg)
{
	struct blk *b = d->priv;
	struct virtio_blk_outhdr *hdr;
	uint8_t *status;
	uint64_t off;
	uint32_t total = 0;
	uint32_t type;
	int j;
	bool ok = true;

	(void)qidx;
	if (nsg < 2 || sg[0].len < sizeof(*hdr) || sg[nsg - 1].len < 1)
		return 0;

	hdr = sg[0].buf;
	type = hdr->type;
	off = hdr->sector * 512;
	status = sg[nsg - 1].buf;	/* 1-byte status, device-writable */

	for (j = 1; j < nsg - 1; j++) {
		ssize_t r;
		if (type == VIRTIO_BLK_T_IN)
			r = blk_rw(b->fd, sg[j].buf, sg[j].len, off, 0);
		else if (type == VIRTIO_BLK_T_OUT)
			r = blk_rw(b->fd, sg[j].buf, sg[j].len, off, 1);
		else
			r = 0;			/* unsupported: ignore data */
		if (r != (ssize_t)sg[j].len)
			ok = false;
		/*
		 * Diagnostic: log read errors, and any high-offset read with
		 * whether the bytes returned are all zero (a sparse-file hole),
		 * to tell "VMM returned a hole" from "VMM returned real data the
		 * guest fs rejected". Only with -D.
		 */
		if (microvm_debug && type == VIRTIO_BLK_T_IN &&
		    (r != (ssize_t)sg[j].len || off >= 0x100000000ULL)) {
			const uint8_t *p = sg[j].buf;
			uint32_t k;
			int zero = 1;

			for (k = 0; r > 0 && k < (uint32_t)r; k++)
				if (p[k] != 0) { zero = 0; break; }
			fprintf(stderr, "[blk] read off=0x%llx len=%u r=%ld "
			    "errno=%d allzero=%d\n", (unsigned long long)off,
			    sg[j].len, (long)r, errno, zero);
		}
		off += sg[j].len;
		if (type == VIRTIO_BLK_T_IN)
			total += sg[j].len;
	}

	status[0] = ok ? VIRTIO_BLK_S_OK : VIRTIO_BLK_S_IOERR;
	/*
	 * Per-request trace (so a request that completes "fine" here but the
	 * guest still rejects can be correlated): starting offset, descriptor
	 * count, total bytes transferred, status. Only with -D.
	 */
	if (microvm_debug)
		fprintf(stderr, "[blk] req type=%u off=0x%llx nsg=%d total=%u "
		    "ok=%d\n", type, (unsigned long long)(hdr->sector * 512),
		    nsg, total, ok);
	return total + 1;		/* data written (reads) + status byte */
}

static void
blk_destroy(struct virtio_dev *d)
{
	struct blk *b = d->priv;

	if (b == NULL)
		return;
	if (b->fd != -1)
		close(b->fd);
	free(b);
}

static const struct virtio_ops blk_ops = {
	.request = blk_request,
	.destroy = blk_destroy,
};

int
virtio_blk_init(struct microvm *vm, uint64_t base, int irq, const char *path)
{
	struct virtio_dev *d;
	struct blk *b;
	struct stat st;
	uint64_t sectors;

	b = calloc(1, sizeof(*b));
	if (b == NULL)
		return microvm_seterr(vm, "virtio-blk: out of memory");
	b->fd = open(path, O_RDWR);
	if (b->fd == -1) {
		free(b);
		return microvm_seterr(vm, "virtio-blk: open %s: %s", path,
		    strerror(errno));
	}
	if (fstat(b->fd, &st) == -1) {
		close(b->fd);
		free(b);
		return microvm_seterr(vm, "virtio-blk: fstat: %s",
		    strerror(errno));
	}

	sectors = (uint64_t)st.st_size / 512;
	memcpy(b->config, &sectors, sizeof(sectors));

	d = virtio_mmio_create(vm, base, irq, VIRTIO_ID_BLOCK, 1, &blk_ops, b);
	if (d == NULL) {
		close(b->fd);
		free(b);
		return microvm_seterr(vm, "virtio-blk: device registration failed");
	}
	d->config = b->config;
	d->config_len = sizeof(b->config);

	if (microvm_debug)
		fprintf(stderr, "[microvm] virtio-blk @0x%llx irq %d: %s "
		    "(%llu sectors)\n", (unsigned long long)base, irq, path,
		    (unsigned long long)sectors);
	return 0;
}
