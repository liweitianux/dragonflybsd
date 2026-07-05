/*
 * virtio-net backend: bridges the guest NIC to a host tap device.
 * One RX queue (host->guest, passive) and one TX queue (guest->host).
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

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "virtio.h"

#define VTNET_RXQ	0	/* host -> guest (passive) */
#define VTNET_TXQ	1	/* guest -> host (active)  */

/* virtio-net MAC feature bit, per <dev/virtual/virtio/net/virtio_net.h>. */
#define VIRTIO_NET_F_MAC	5

/* 12-byte virtio-net header (virtio 1.0 layout). */
struct virtio_net_hdr {
	uint8_t  flags;
	uint8_t  gso_type;
	uint16_t hdr_len;
	uint16_t gso_size;
	uint16_t csum_start;
	uint16_t csum_offset;
	uint16_t num_buffers;
} __attribute__((packed));

#define VTNET_HDRLEN	sizeof(struct virtio_net_hdr)
#define FRAME_MAX	2048

struct net {
	int tapfd;
	uint8_t config[12];		/* mac[6] + status + max_vq_pairs */
};

static uint32_t
net_request(struct virtio_dev *d, int qidx, struct vq_sg *sg, int nsg)
{
	struct net *n = d->priv;
	uint8_t pkt[FRAME_MAX + VTNET_HDRLEN];
	uint32_t len = 0;
	int j;

	if (qidx != VTNET_TXQ)
		return 0;

	/* Coalesce the guest's [hdr][frame] descriptors. */
	for (j = 0; j < nsg; j++) {
		if (sg[j].write)
			continue;
		if (len + sg[j].len > sizeof(pkt))
			break;
		memcpy(pkt + len, sg[j].buf, sg[j].len);
		len += sg[j].len;
	}

	/* Strip the virtio-net header and inject the frame into the tap. */
	if (len > VTNET_HDRLEN)
		(void)!write(n->tapfd, pkt + VTNET_HDRLEN, len - VTNET_HDRLEN);
	return 0;
}

static void
net_poll(struct virtio_dev *d)
{
	struct net *n = d->priv;
	uint8_t buf[FRAME_MAX + VTNET_HDRLEN];
	struct virtio_net_hdr *hdr = (struct virtio_net_hdr *)buf;
	ssize_t r;

	for (;;) {
		r = read(n->tapfd, buf + VTNET_HDRLEN, FRAME_MAX);
		if (r <= 0)
			break;
		memset(hdr, 0, VTNET_HDRLEN);
		hdr->num_buffers = 1;
		if (virtio_rx_push(d, VTNET_RXQ, buf,
		    (uint32_t)r + VTNET_HDRLEN) == 0)
			break;			/* no guest buffer; drop rest */
	}
}

static void
net_destroy(struct virtio_dev *d)
{
	struct net *n = d->priv;

	if (n == NULL)
		return;
	if (n->tapfd != -1)
		close(n->tapfd);
	free(n);
}

static int
net_collect_fds(struct virtio_dev *d, int *fds, int max)
{
	struct net *n = d->priv;

	if (max < 1 || n->tapfd < 0)
		return 0;
	fds[0] = n->tapfd;
	return 1;
}

static const struct virtio_ops net_ops = {
	.request = net_request,
	.poll = net_poll,
	.destroy = net_destroy,
	.collect_fds = net_collect_fds,
};

int
virtio_net_init(struct microvm *vm, uint64_t base, int irq, const char *tap)
{
	struct virtio_dev *d;
	struct net *n;
	static const uint8_t mac[6] = { 0x52, 0x54, 0x00, 0x12, 0x34, 0x56 };
	int fl;

	n = calloc(1, sizeof(*n));
	if (n == NULL)
		return microvm_seterr(vm, "virtio-net: out of memory");
	n->tapfd = open(tap, O_RDWR);
	if (n->tapfd == -1) {
		free(n);
		return microvm_seterr(vm, "virtio-net: open %s: %s", tap,
		    strerror(errno));
	}
	fl = fcntl(n->tapfd, F_GETFL, 0);
	if (fl != -1)
		fcntl(n->tapfd, F_SETFL, fl | O_NONBLOCK);

	memcpy(n->config, mac, 6);

	d = virtio_mmio_create(vm, base, irq, VIRTIO_ID_NET, 2, &net_ops, n);
	if (d == NULL) {
		close(n->tapfd);
		free(n);
		return microvm_seterr(vm, "virtio-net: device registration failed");
	}
	d->host_features |= (1ULL << VIRTIO_NET_F_MAC);
	d->passive_qmask = (1u << VTNET_RXQ);
	d->config = n->config;
	d->config_len = sizeof(n->config);

	if (microvm_debug)
		fprintf(stderr, "[microvm] virtio-net @0x%llx irq %d: %s "
		    "(mac 52:54:00:12:34:56)\n", (unsigned long long)base, irq,
		    tap);
	return 0;
}
