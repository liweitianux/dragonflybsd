/*
 * virtio-vsock device model: host<->guest sockets without networking.
 *
 * Phase 1: guest-initiated streams, bridged to host AF_UNIX sockets
 * (Firecracker model). When the guest connects to (cid=VMADDR_CID_HOST,
 * port P), the VMM connect()s the host unix socket "<uds_path>_<P>", so a
 * host program just listens on that path. Host-initiated connections (a host
 * listener + "CONNECT <port>" mux) are a documented follow-up.
 *
 * Three virtqueues: RX (host->guest, passive, filled via virtio_rx_push),
 * TX (guest->host, active, processed in .request), EVENT (unused for now).
 * The guest CID is exposed in config space. Credit-based flow control per the
 * virtio-vsock spec: every packet carries our buf_alloc/fwd_cnt, and we honour
 * the peer's so we never overrun the guest's receive buffer.
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

#include <sys/socket.h>
#include <sys/un.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "virtio.h"

#define VSOCK_RXQ	0
#define VSOCK_TXQ	1
#define VSOCK_EVENTQ	2

#define VMADDR_CID_HOST		2

#define VIRTIO_VSOCK_TYPE_STREAM	1
#define VIRTIO_VSOCK_OP_INVALID		0
#define VIRTIO_VSOCK_OP_REQUEST		1
#define VIRTIO_VSOCK_OP_RESPONSE	2
#define VIRTIO_VSOCK_OP_RST		3
#define VIRTIO_VSOCK_OP_SHUTDOWN	4
#define VIRTIO_VSOCK_OP_RW		5
#define VIRTIO_VSOCK_OP_CREDIT_UPDATE	6
#define VIRTIO_VSOCK_OP_CREDIT_REQUEST	7

#define VIRTIO_VSOCK_SHUTDOWN_F_RECEIVE	1
#define VIRTIO_VSOCK_SHUTDOWN_F_SEND	2

struct virtio_vsock_hdr {
	uint64_t src_cid;
	uint64_t dst_cid;
	uint32_t src_port;
	uint32_t dst_port;
	uint32_t len;
	uint16_t type;
	uint16_t op;
	uint32_t flags;
	uint32_t buf_alloc;
	uint32_t fwd_cnt;
} __attribute__((packed));

#define VSOCK_BUF_ALLOC		(256 * 1024)	/* our advertised RX buffer */
#define VSOCK_PKT_MAX		4000		/* per-RW payload (fits a 4K RX buf) */
#define VSOCK_MAXCONN		64

/* Connection lifecycle. OPEN = data flows. PENDING/CONNECTING are the
 * host-initiated (host->guest) handshake: PENDING = host socket accepted,
 * awaiting the "CONNECT <port>" line; CONNECTING = OP_REQUEST sent to the
 * guest, awaiting its OP_RESPONSE. (guest->host connections jump to OPEN.) */
enum vstate { VS_OPEN = 1, VS_PENDING, VS_CONNECTING };

struct vconn {
	int		used;
	int		fd;		/* host AF_UNIX socket, -1 if none */
	int		state;		/* enum vstate */
	uint32_t	guest_port;	/* remote (guest) port */
	uint32_t	host_port;	/* local (host cid 2) port */
	uint32_t	rx_cnt;		/* bytes consumed from guest (our fwd_cnt) */
	uint32_t	tx_cnt;		/* bytes sent to guest */
	uint32_t	peer_buf_alloc;	/* guest's RX buffer */
	uint32_t	peer_fwd_cnt;	/* guest's reported fwd_cnt */
	char		linebuf[64];	/* VS_PENDING: partial "CONNECT <port>" */
	int		linelen;
};

struct vsock {
	struct virtio_dev *dev;
	uint64_t	guest_cid;
	const char	*uds_path;
	int		listen_fd;	/* host->guest inbound listener, -1 if none */
	uint32_t	next_host_port;	/* ephemeral host-side ports (>= 50000) */
	struct vconn	conn[VSOCK_MAXCONN];
	uint8_t		cfg[8];		/* guest_cid, little-endian */
};

static struct vconn *
vconn_find(struct vsock *vs, uint32_t guest_port, uint32_t host_port)
{
	int i;

	for (i = 0; i < VSOCK_MAXCONN; i++)
		if (vs->conn[i].used && vs->conn[i].guest_port == guest_port &&
		    vs->conn[i].host_port == host_port)
			return &vs->conn[i];
	return NULL;
}

static struct vconn *
vconn_alloc(struct vsock *vs)
{
	int i;

	for (i = 0; i < VSOCK_MAXCONN; i++)
		if (!vs->conn[i].used) {
			memset(&vs->conn[i], 0, sizeof(vs->conn[i]));
			vs->conn[i].used = 1;
			vs->conn[i].fd = -1;
			return &vs->conn[i];
		}
	return NULL;
}

static void
vconn_free(struct vconn *c)
{
	if (c->fd >= 0)
		close(c->fd);
	c->used = 0;
	c->fd = -1;
}

/* How many bytes the guest can still receive on this connection. */
static uint32_t
vconn_peer_credit(struct vconn *c)
{
	uint32_t inflight = c->tx_cnt - c->peer_fwd_cnt;

	return (c->peer_buf_alloc > inflight) ? c->peer_buf_alloc - inflight : 0;
}

/* Push a packet (header + optional payload) to the guest on the RX queue. */
static void
vsock_tx_guest(struct vsock *vs, uint32_t host_port, uint32_t guest_port,
    uint16_t op, uint32_t flags, uint32_t fwd_cnt, const void *pay, uint32_t plen)
{
	uint8_t pkt[sizeof(struct virtio_vsock_hdr) + VSOCK_PKT_MAX];
	struct virtio_vsock_hdr *h = (struct virtio_vsock_hdr *)pkt;

	if (plen > VSOCK_PKT_MAX)
		plen = VSOCK_PKT_MAX;
	memset(h, 0, sizeof(*h));
	h->src_cid = VMADDR_CID_HOST;
	h->dst_cid = vs->guest_cid;
	h->src_port = host_port;
	h->dst_port = guest_port;
	h->type = VIRTIO_VSOCK_TYPE_STREAM;
	h->op = op;
	h->flags = flags;
	h->len = plen;
	h->buf_alloc = VSOCK_BUF_ALLOC;
	h->fwd_cnt = fwd_cnt;
	if (plen != 0)
		memcpy(pkt + sizeof(*h), pay, plen);
	virtio_rx_push(vs->dev, VSOCK_RXQ, pkt, sizeof(*h) + plen);
}

static void
vsock_send(struct vsock *vs, struct vconn *c, uint16_t op, uint32_t flags,
    const void *pay, uint32_t plen)
{
	vsock_tx_guest(vs, c->host_port, c->guest_port, op, flags, c->rx_cnt,
	    pay, plen);
	if (op == VIRTIO_VSOCK_OP_RW)
		c->tx_cnt += plen;
}

/* Connect a host AF_UNIX stream to "<uds_path>_<port>". */
static int
host_connect(struct vsock *vs, uint32_t port)
{
	struct sockaddr_un sun;
	int fd, fl;

	if (vs->uds_path == NULL)
		return -1;
	fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0)
		return -1;
	memset(&sun, 0, sizeof(sun));
	sun.sun_family = AF_UNIX;
	snprintf(sun.sun_path, sizeof(sun.sun_path), "%s_%u", vs->uds_path, port);
	if (connect(fd, (struct sockaddr *)&sun, sizeof(sun)) != 0) {
		close(fd);
		return -1;
	}
	if ((fl = fcntl(fd, F_GETFL, 0)) != -1)
		(void)fcntl(fd, F_SETFL, fl | O_NONBLOCK);
	return fd;
}

/* TX queue: one guest->device packet (header in sg[0], payload follows). */
static uint32_t
vsock_request(struct virtio_dev *d, int qidx, struct vq_sg *sg, int nsg)
{
	struct vsock *vs = d->priv;
	struct virtio_vsock_hdr *h;
	struct vconn *c;
	int j;

	if (qidx != VSOCK_TXQ)			/* RX/EVENT are passive */
		return 0;
	if (nsg < 1 || sg[0].len < sizeof(*h))
		return 0;
	h = sg[0].buf;

	c = vconn_find(vs, h->src_port, h->dst_port);
	if (c != NULL) {			/* refresh peer credit */
		c->peer_buf_alloc = h->buf_alloc;
		c->peer_fwd_cnt = h->fwd_cnt;
	}

	switch (h->op) {
	case VIRTIO_VSOCK_OP_REQUEST:
		if (c != NULL)			/* duplicate: ignore */
			break;
		c = vconn_alloc(vs);
		if (c == NULL) {
			vsock_tx_guest(vs, h->dst_port, h->src_port,
			    VIRTIO_VSOCK_OP_RST, 0, 0, NULL, 0);
			break;
		}
		c->guest_port = h->src_port;
		c->host_port = h->dst_port;
		c->peer_buf_alloc = h->buf_alloc;
		c->peer_fwd_cnt = h->fwd_cnt;
		c->fd = host_connect(vs, h->dst_port);
		if (c->fd < 0) {
			vsock_tx_guest(vs, c->host_port, c->guest_port,
			    VIRTIO_VSOCK_OP_RST, 0, 0, NULL, 0);
			vconn_free(c);
			break;
		}
		c->state = VS_OPEN;
		vsock_send(vs, c, VIRTIO_VSOCK_OP_RESPONSE, 0, NULL, 0);
		break;

	case VIRTIO_VSOCK_OP_RESPONSE:		/* guest accepted a host->guest */
		if (c != NULL && c->state == VS_CONNECTING) {
			char ok[32];
			int l;

			c->state = VS_OPEN;	/* peer credit refreshed above */
			l = snprintf(ok, sizeof(ok), "OK %u\n", c->host_port);
			(void)write(c->fd, ok, (size_t)l);
		}
		break;

	case VIRTIO_VSOCK_OP_RW:
		if (c == NULL || c->fd < 0) {
			vsock_tx_guest(vs, h->dst_port, h->src_port,
			    VIRTIO_VSOCK_OP_RST, 0, 0, NULL, 0);
			break;
		}
		/* Write the payload (after the hdr in sg[0], then sg[1..]) to
		 * the host socket; best-effort non-blocking. */
		{
			uint32_t want = h->len;
			for (j = 0; j < nsg && want > 0; j++) {
				uint8_t *p = sg[j].buf;
				uint32_t off = (j == 0) ? sizeof(*h) : 0;
				uint32_t avail = sg[j].len > off ?
				    sg[j].len - off : 0;
				uint32_t n = want < avail ? want : avail;
				if (n != 0)
					(void)write(c->fd, p + off, n);
				want -= n;
			}
			c->rx_cnt += h->len;
		}
		/* Tell the guest we consumed it, so its tx credit recovers. */
		vsock_send(vs, c, VIRTIO_VSOCK_OP_CREDIT_UPDATE, 0, NULL, 0);
		break;

	case VIRTIO_VSOCK_OP_CREDIT_REQUEST:
		if (c != NULL)
			vsock_send(vs, c, VIRTIO_VSOCK_OP_CREDIT_UPDATE, 0,
			    NULL, 0);
		break;

	case VIRTIO_VSOCK_OP_CREDIT_UPDATE:
		break;				/* peer credit already refreshed */

	case VIRTIO_VSOCK_OP_SHUTDOWN:
	case VIRTIO_VSOCK_OP_RST:
		if (c != NULL) {
			if (h->op == VIRTIO_VSOCK_OP_SHUTDOWN)
				vsock_tx_guest(vs, c->host_port, c->guest_port,
				    VIRTIO_VSOCK_OP_RST, 0, 0, NULL, 0);
			vconn_free(c);
		}
		break;

	default:
		break;
	}
	return 0;				/* TX buffers are device-readable */
}

/* A pending host connection: read its "CONNECT <port>\n" line and, once we
 * have it, dial the guest by sending OP_REQUEST (host->guest). */
static void
vsock_connect_line(struct vsock *vs, struct vconn *c)
{
	unsigned port;
	ssize_t n;

	n = read(c->fd, c->linebuf + c->linelen,
	    sizeof(c->linebuf) - 1 - (size_t)c->linelen);
	if (n == 0) {
		vconn_free(c);
		return;
	}
	if (n < 0) {
		if (errno != EAGAIN && errno != EWOULDBLOCK)
			vconn_free(c);
		return;
	}
	c->linelen += (int)n;
	c->linebuf[c->linelen] = '\0';
	if (strchr(c->linebuf, '\n') == NULL) {
		if ((size_t)c->linelen >= sizeof(c->linebuf) - 1)
			vconn_free(c);		/* runaway line */
		return;
	}
	if (sscanf(c->linebuf, "CONNECT %u", &port) != 1) {
		vconn_free(c);
		return;
	}
	c->guest_port = port;			/* the guest service port */
	c->host_port = vs->next_host_port++;	/* our ephemeral host port */
	c->state = VS_CONNECTING;
	c->peer_buf_alloc = 0;
	c->peer_fwd_cnt = 0;
	if (microvm_debug)
		fprintf(stderr, "[vsock] CONNECT %u -> OP_REQUEST (host_port %u)\n",
		    port, c->host_port);
	vsock_send(vs, c, VIRTIO_VSOCK_OP_REQUEST, 0, NULL, 0);
}

/* Accept inbound host connections, advance pending handshakes, and pump
 * host->guest data for open connections (honouring the guest's credit). */
static void
vsock_poll(struct virtio_dev *d)
{
	struct vsock *vs = d->priv;
	uint8_t buf[VSOCK_PKT_MAX];
	int i;

	/* Accept host->guest connections on the base UDS. */
	while (vs->listen_fd >= 0) {
		struct vconn *c;
		int afd, fl;

		afd = accept(vs->listen_fd, NULL, NULL);
		if (afd < 0)
			break;
		c = vconn_alloc(vs);
		if (c == NULL) {
			close(afd);
			continue;
		}
		if ((fl = fcntl(afd, F_GETFL, 0)) != -1)
			(void)fcntl(afd, F_SETFL, fl | O_NONBLOCK);
		c->fd = afd;
		c->state = VS_PENDING;
		if (microvm_debug)
			fprintf(stderr, "[vsock] accepted host conn fd=%d\n", afd);
	}

	for (i = 0; i < VSOCK_MAXCONN; i++) {
		struct vconn *c = &vs->conn[i];
		uint32_t cr, want;
		ssize_t n;

		if (!c->used || c->fd < 0)
			continue;
		if (c->state == VS_PENDING) {		/* awaiting "CONNECT" */
			vsock_connect_line(vs, c);
			continue;
		}
		if (c->state != VS_OPEN)		/* CONNECTING: await RESP */
			continue;

		cr = vconn_peer_credit(c);
		if (cr == 0)			/* guest full: leave data queued */
			continue;
		want = cr < sizeof(buf) ? cr : sizeof(buf);
		n = read(c->fd, buf, want);
		if (n > 0) {
			vsock_send(vs, c, VIRTIO_VSOCK_OP_RW, 0, buf,
			    (uint32_t)n);
		} else if (n == 0) {		/* host closed */
			vsock_tx_guest(vs, c->host_port, c->guest_port,
			    VIRTIO_VSOCK_OP_SHUTDOWN,
			    VIRTIO_VSOCK_SHUTDOWN_F_RECEIVE |
			    VIRTIO_VSOCK_SHUTDOWN_F_SEND, c->rx_cnt, NULL, 0);
			vconn_free(c);
		} else if (errno != EAGAIN && errno != EWOULDBLOCK) {
			vconn_free(c);
		}
	}
}

static void
vsock_destroy(struct virtio_dev *d)
{
	struct vsock *vs = d->priv;
	int i;

	if (vs == NULL)
		return;
	if (vs->listen_fd >= 0)
		close(vs->listen_fd);
	for (i = 0; i < VSOCK_MAXCONN; i++)
		if (vs->conn[i].used)
			vconn_free(&vs->conn[i]);
	free(vs);
}

/* Readable host fds: the inbound listener plus every open connection. */
static int
vsock_collect_fds(struct virtio_dev *d, int *fds, int max)
{
	struct vsock *vs = d->priv;
	int i, n = 0;

	if (vs->listen_fd >= 0 && n < max)
		fds[n++] = vs->listen_fd;
	for (i = 0; i < VSOCK_MAXCONN && n < max; i++)
		if (vs->conn[i].used && vs->conn[i].fd >= 0)
			fds[n++] = vs->conn[i].fd;
	return n;
}

static const struct virtio_ops vsock_ops = {
	.request = vsock_request,
	.poll = vsock_poll,
	.destroy = vsock_destroy,
	.collect_fds = vsock_collect_fds,
};

int
virtio_vsock_init(struct microvm *vm, uint64_t base, int irq,
    uint64_t guest_cid, const char *uds_path)
{
	struct vsock *vs;
	struct virtio_dev *d;
	int i;

	vs = calloc(1, sizeof(*vs));
	if (vs == NULL)
		return microvm_seterr(vm, "virtio-vsock: out of memory");
	vs->guest_cid = guest_cid;
	vs->uds_path = uds_path;
	vs->listen_fd = -1;
	vs->next_host_port = 50000;
	for (i = 0; i < VSOCK_MAXCONN; i++)
		vs->conn[i].fd = -1;
	for (i = 0; i < 8; i++)			/* guest_cid, little-endian */
		vs->cfg[i] = (uint8_t)(guest_cid >> (8 * i));

	/* Host->guest: listen on the base UDS; a host connects and sends
	 * "CONNECT <port>\n" to reach a guest service (Firecracker mux). */
	if (uds_path != NULL) {
		struct sockaddr_un sun;
		int lfd, fl;

		lfd = socket(AF_UNIX, SOCK_STREAM, 0);
		if (lfd >= 0) {
			memset(&sun, 0, sizeof(sun));
			sun.sun_family = AF_UNIX;
			snprintf(sun.sun_path, sizeof(sun.sun_path), "%s",
			    uds_path);
			(void)unlink(uds_path);
			if (bind(lfd, (struct sockaddr *)&sun, sizeof(sun)) == 0 &&
			    listen(lfd, 8) == 0) {
				if ((fl = fcntl(lfd, F_GETFL, 0)) != -1)
					(void)fcntl(lfd, F_SETFL, fl | O_NONBLOCK);
				vs->listen_fd = lfd;
			} else {
				close(lfd);
			}
		}
	}

	d = virtio_mmio_create(vm, base, irq, VIRTIO_ID_VSOCK, 3, &vsock_ops, vs);
	if (d == NULL) {
		if (vs->listen_fd >= 0)
			close(vs->listen_fd);
		free(vs);
		return microvm_seterr(vm, "virtio-vsock: device registration "
		    "failed");
	}
	vs->dev = d;
	d->config = vs->cfg;
	d->config_len = sizeof(vs->cfg);
	d->passive_qmask = (1u << VSOCK_RXQ) | (1u << VSOCK_EVENTQ);

	if (microvm_debug)
		fprintf(stderr, "[microvm] virtio-vsock @0x%llx irq %d cid %llu "
		    "uds %s\n", (unsigned long long)base, irq,
		    (unsigned long long)guest_cid,
		    uds_path ? uds_path : "(none)");
	return 0;
}
