/*
 * virtio-console backend: guest console TX -> host stdout, host stdin -> guest
 * console RX. A single port, no multiport control queue.
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

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "virtio.h"

/* Queue indices. */
#define VTCON_RXQ	0	/* host -> guest (passive) */
#define VTCON_TXQ	1	/* guest -> host (active)  */

struct console {
	uint8_t config[16];	/* cols, rows, max_nr_ports, emerg_wr */
};

static uint32_t
console_request(struct virtio_dev *d, int qidx, struct vq_sg *sg, int nsg)
{
	int j;

	(void)d;
	if (qidx != VTCON_TXQ)
		return 0;
	for (j = 0; j < nsg; j++) {
		if (sg[j].write)
			continue;		/* TX buffers are guest-readable */
		(void)!write(STDOUT_FILENO, sg[j].buf, sg[j].len);
	}
	fflush(stdout);
	return 0;
}

static void
console_poll(struct virtio_dev *d)
{
	uint8_t buf[256];
	ssize_t n;

	n = read(STDIN_FILENO, buf, sizeof(buf));	/* non-blocking */
	if (n > 0)
		n = (ssize_t)microvm_console_input(buf, (size_t)n);	/* ^A x escape */
	if (n > 0)
		virtio_rx_push(d, VTCON_RXQ, buf, (uint32_t)n);
}

static void
console_destroy(struct virtio_dev *d)
{
	free(d->priv);		/* stdin/stdout are not owned by us */
}

static const struct virtio_ops console_ops = {
	.request = console_request,
	.poll = console_poll,
	.destroy = console_destroy,
};

int
virtio_console_init(struct microvm *vm, uint64_t base, int irq)
{
	struct virtio_dev *d;
	struct console *c;
	int fl;

	c = calloc(1, sizeof(*c));
	if (c == NULL)
		return microvm_seterr(vm, "virtio-console: out of memory");

	/* stdin non-blocking so console_poll never stalls the vCPU loop. */
	fl = fcntl(STDIN_FILENO, F_GETFL, 0);
	if (fl != -1)
		fcntl(STDIN_FILENO, F_SETFL, fl | O_NONBLOCK);

	d = virtio_mmio_create(vm, base, irq, VIRTIO_ID_CONSOLE, 2,
	    &console_ops, c);
	if (d == NULL) {
		free(c);
		return microvm_seterr(vm, "virtio-console: device registration "
		    "failed");
	}
	d->passive_qmask = (1u << VTCON_RXQ);
	d->config = c->config;
	d->config_len = sizeof(c->config);

	if (microvm_debug)
		fprintf(stderr, "[microvm] virtio-console @0x%llx irq %d\n",
		    (unsigned long long)base, irq);
	return 0;
}
