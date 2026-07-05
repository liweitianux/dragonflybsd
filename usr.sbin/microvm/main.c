/*
 * microvm - command-line front-end to libmicrovm: boot a PVH kernel as a
 * lightweight NVMM microVM.
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

#include <err.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <microvm.h>

static void
usage(const char *prog)
{
	fprintf(stderr, "usage: %s -k kernel [-i initramfs] [-c cmdline] "
	    "[-m ram_MiB] [-p ncpus] [-d disk.img] [-V] [-n /dev/tapN] "
	    "[-s [cid@]uds_path] [-D] [-L]\n",
	    prog);
	exit(EXIT_FAILURE);
}

int
main(int argc, char *argv[])
{
	struct microvm_config cfg;
	struct microvm *vm;
	const char *disk = NULL, *tap = NULL, *vsock = NULL;
	bool console = false;
	int ch;

	memset(&cfg, 0, sizeof(cfg));
	while ((ch = getopt(argc, argv, "k:i:c:m:p:d:n:s:VDL")) != -1) {
		switch (ch) {
		case 'k': cfg.kernel = optarg; break;
		case 'i': cfg.initramfs = optarg; break;
		case 'c': cfg.cmdline = optarg; break;
		case 'm': cfg.mem_bytes = strtoull(optarg, NULL, 0) << 20; break;
		case 'p': cfg.ncpus = (int)strtol(optarg, NULL, 0); break;
		case 'd': disk = optarg; break;
		case 'n': tap = optarg; break;
		case 's': vsock = optarg; break;
		case 'V': console = true; break;
		case 'D': cfg.debug = 1; break;
		case 'L': cfg.virtio_legacy = 1; break;
		default: usage(argv[0]);
		}
	}
	if (cfg.kernel == NULL)
		usage(argv[0]);

	vm = microvm_create(&cfg);
	if (vm == NULL)
		errx(EXIT_FAILURE, "microvm_create: out of memory");

	if (disk != NULL)
		microvm_add_blk(vm, disk);
	if (console)
		microvm_add_console(vm);
	if (tap != NULL)
		microvm_add_net(vm, tap);
	if (vsock != NULL) {
		uint64_t cid = 3;		/* default first guest CID */
		const char *path = vsock, *at = strchr(vsock, '@');
		if (at != NULL) {
			cid = strtoull(vsock, NULL, 0);
			path = at + 1;
		}
		if (microvm_add_vsock(vm, cid, path) != 0)
			errx(EXIT_FAILURE, "%s", microvm_error(vm));
	}

	if (microvm_load(vm) != 0)
		errx(EXIT_FAILURE, "%s", microvm_error(vm));
	if (microvm_run(vm) != 0)
		errx(EXIT_FAILURE, "%s", microvm_error(vm));

	microvm_destroy(vm);
	return 0;
}
