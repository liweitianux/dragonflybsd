/*
 * PVH loader: map a PVH-capable ELF kernel into guest RAM, build the
 * hvm_start_info boot block (command line, e820-style memory map, optional
 * initramfs module), and report the 32-bit entry point.
 *
 * Segments load at their physical (p_paddr) addresses into one contiguous RAM
 * region, and the entry point comes from the XEN_ELFNOTE_PHYS32_ENTRY note
 * rather than e_entry.
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
#include <sys/elf64.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "vmm_internal.h"
#include "pvh.h"

/* Read a whole file into a malloc'd buffer. Returns NULL on error (sets err). */
static void *
slurp(struct microvm *m, const char *path, size_t *sizep)
{
	struct stat st;
	void *buf;
	ssize_t rd;
	int fd;

	fd = open(path, O_RDONLY);
	if (fd == -1) {
		microvm_seterr(m, "open %s: %s", path, strerror(errno));
		return NULL;
	}
	if (fstat(fd, &st) == -1) {
		microvm_seterr(m, "fstat %s: %s", path, strerror(errno));
		close(fd);
		return NULL;
	}
	buf = malloc(st.st_size);
	if (buf == NULL) {
		microvm_seterr(m, "malloc %lld", (long long)st.st_size);
		close(fd);
		return NULL;
	}
	rd = pread(fd, buf, st.st_size, 0);
	close(fd);
	if (rd != st.st_size) {
		microvm_seterr(m, "short read on %s", path);
		free(buf);
		return NULL;
	}
	*sizep = st.st_size;
	return buf;
}

/* Scan PT_NOTE segments for the Xen PHYS32_ENTRY note. 0 + *entry, or -1. */
static int
find_pvh_entry(const uint8_t *elf, size_t elfsize, const Elf64_Phdr *phdr,
    size_t phnum, uint64_t *entry)
{
	size_t i;

	for (i = 0; i < phnum; i++) {
		const uint8_t *p, *end;

		if (phdr[i].p_type != PT_NOTE)
			continue;
		if (phdr[i].p_offset > elfsize ||
		    phdr[i].p_filesz > elfsize - phdr[i].p_offset)
			continue;
		p = elf + phdr[i].p_offset;
		end = p + phdr[i].p_filesz;

		while (p + sizeof(Elf64_Nhdr) <= end) {
			const Elf64_Nhdr *nh = (const Elf64_Nhdr *)p;
			const char *name = (const char *)(nh + 1);
			const uint8_t *desc = (const uint8_t *)name +
			    roundup(nh->n_namesz, 4);

			if (desc + roundup(nh->n_descsz, 4) > end)
				break;
			if (nh->n_type == XEN_ELFNOTE_PHYS32_ENTRY &&
			    nh->n_namesz >= 4 &&
			    memcmp(name, PVH_NOTE_NAME, sizeof(PVH_NOTE_NAME)) == 0) {
				if (nh->n_descsz == 4)
					*entry = *(const uint32_t *)desc;
				else if (nh->n_descsz == 8)
					*entry = *(const uint64_t *)desc;
				else
					return -1;
				return 0;
			}
			p = desc + roundup(nh->n_descsz, 4);
		}
	}
	return -1;
}

/* Load PT_LOAD segments at their physical addresses; report page-rounded end. */
static int
load_segments(struct microvm *m, const uint8_t *elf, size_t elfsize,
    const Elf64_Phdr *phdr, size_t phnum, uint64_t *image_end)
{
	uint64_t end = 0, off = 0, minpa = UINT64_MAX;
	size_t i;

	/*
	 * A PVH kernel normally carries true physical load addresses in p_paddr
	 * (e.g. Linux). The DragonFly kernel is instead linked with p_paddr ==
	 * p_vaddr in the KERNBASE high half; relocate those down to physical the
	 * same way its native loader does (off = -(base & 0xffffffffff000000)).
	 * Kernels already using low physical addresses are left untouched.
	 */
	for (i = 0; i < phnum; i++) {
		const Elf64_Phdr *ph = &phdr[i];
		if (ph->p_type == PT_LOAD && ph->p_memsz != 0 &&
		    ph->p_paddr < minpa)
			minpa = ph->p_paddr;
	}
	if (minpa >= 0xffffffff80000000ULL)
		off = 0 - (minpa & 0xffffffffff000000ULL);

	for (i = 0; i < phnum; i++) {
		const Elf64_Phdr *ph = &phdr[i];
		uint64_t pa;
		void *dst;

		if (ph->p_type != PT_LOAD || ph->p_memsz == 0)
			continue;
		if (ph->p_offset > elfsize ||
		    ph->p_filesz > elfsize - ph->p_offset)
			return microvm_seterr(m, "segment %zu file range "
			    "(offset 0x%llx, filesz 0x%llx) out of bounds", i,
			    (unsigned long long)ph->p_offset,
			    (unsigned long long)ph->p_filesz);

		pa = ph->p_paddr + off;
		dst = gpa_to_hva(m, pa, ph->p_memsz);
		if (dst == NULL)
			return microvm_seterr(m, "segment %zu (paddr 0x%llx, "
			    "memsz 0x%llx) does not fit in guest RAM", i,
			    (unsigned long long)pa,
			    (unsigned long long)ph->p_memsz);

		memcpy(dst, elf + ph->p_offset, ph->p_filesz);
		if (ph->p_memsz > ph->p_filesz)
			memset((uint8_t *)dst + ph->p_filesz, 0,
			    ph->p_memsz - ph->p_filesz);

		if (pa + ph->p_memsz > end)
			end = pa + ph->p_memsz;
	}

	*image_end = roundup(end, PAGE_SIZE);
	return 0;
}

int
pvh_load(struct microvm *m, uint64_t *entry_out, uint64_t *start_info_gpa_out)
{
	uint8_t *kernel;
	size_t ksize;
	const Elf64_Ehdr *eh;
	const Elf64_Phdr *phdr;
	uint64_t entry, image_end = 0;
	uint64_t initrd_gpa = 0, initrd_size = 0;
	struct hvm_start_info *si;
	struct hvm_memmap_table_entry *mm;
	unsigned int nmemmap = 0;
	const char *cmd = m->cmdline;

	kernel = slurp(m, m->cfg.kernel, &ksize);
	if (kernel == NULL)
		return -1;

#define FAIL(...) do { free(kernel); return microvm_seterr(m, __VA_ARGS__); } while (0)
	if (ksize < sizeof(Elf64_Ehdr))
		FAIL("kernel too small to be ELF");
	eh = (const Elf64_Ehdr *)kernel;
	if (memcmp(eh->e_ident, ELFMAG, SELFMAG) != 0)
		FAIL("kernel is not an ELF file");
	if (eh->e_ident[EI_CLASS] != ELFCLASS64)
		FAIL("kernel is not ELF64");
	if (eh->e_phoff == 0 || eh->e_phnum == 0)
		FAIL("kernel has no program headers");
	if (eh->e_phentsize != sizeof(Elf64_Phdr))
		FAIL("unexpected program header entry size");
	if (eh->e_phoff > ksize ||
	    (uint64_t)eh->e_phnum * sizeof(Elf64_Phdr) > ksize - eh->e_phoff)
		FAIL("program headers out of bounds");
	phdr = (const Elf64_Phdr *)(kernel + eh->e_phoff);

	if (find_pvh_entry(kernel, ksize, phdr, eh->e_phnum, &entry) != 0)
		FAIL("kernel has no XEN_ELFNOTE_PHYS32_ENTRY note "
		    "(not PVH-capable?)");
	if (load_segments(m, kernel, ksize, phdr, eh->e_phnum, &image_end) != 0) {
		free(kernel);
		return -1;
	}
#undef FAIL
	free(kernel);

	/* initramfs (optional), page-aligned just past the kernel image. */
	if (m->cfg.initramfs != NULL) {
		uint8_t *initrd;
		size_t isz;
		void *dst;

		initrd = slurp(m, m->cfg.initramfs, &isz);
		if (initrd == NULL)
			return -1;
		initrd_gpa = roundup(image_end, PAGE_SIZE);
		dst = gpa_to_hva(m, initrd_gpa, isz);
		if (dst == NULL) {
			free(initrd);
			return microvm_seterr(m, "initramfs does not fit in RAM");
		}
		memcpy(dst, initrd, isz);
		initrd_size = isz;
		free(initrd);
	}

	/* Command line. */
	{
		char *dst = gpa_to_hva(m, BOOTINFO_CMDLINE_GPA, strlen(cmd) + 1);
		if (dst == NULL)
			return microvm_seterr(m, "cmdline does not fit");
		memcpy(dst, cmd, strlen(cmd) + 1);
	}

	/* e820-style memory map. */
	mm = gpa_to_hva(m, BOOTINFO_MEMMAP_GPA,
	    2 * sizeof(struct hvm_memmap_table_entry));
	if (mm == NULL)
		return microvm_seterr(m, "memmap does not fit");
	mm[nmemmap].addr = 0;
	mm[nmemmap].size = LOWMEM_TOP;
	mm[nmemmap].type = XEN_HVM_MEMMAP_TYPE_RAM;
	mm[nmemmap].reserved = 0;
	nmemmap++;
	mm[nmemmap].addr = 0x100000;
	mm[nmemmap].size = m->mem.size - 0x100000;
	mm[nmemmap].type = XEN_HVM_MEMMAP_TYPE_RAM;
	mm[nmemmap].reserved = 0;
	nmemmap++;

	/* Module list (initramfs). */
	if (initrd_size != 0) {
		struct hvm_modlist_entry *mod =
		    gpa_to_hva(m, BOOTINFO_MODLIST_GPA, sizeof(*mod));
		if (mod == NULL)
			return microvm_seterr(m, "modlist does not fit");
		mod->paddr = initrd_gpa;
		mod->size = initrd_size;
		mod->cmdline_paddr = 0;
		mod->reserved = 0;
	}

	/* hvm_start_info. */
	si = gpa_to_hva(m, BOOTINFO_START_GPA, sizeof(*si));
	if (si == NULL)
		return microvm_seterr(m, "start_info does not fit");
	memset(si, 0, sizeof(*si));
	si->magic = XEN_HVM_START_MAGIC_VALUE;
	si->version = 1;
	si->cmdline_paddr = BOOTINFO_CMDLINE_GPA;
	si->memmap_paddr = BOOTINFO_MEMMAP_GPA;
	si->memmap_entries = nmemmap;
	if (initrd_size != 0) {
		si->nr_modules = 1;
		si->modlist_paddr = BOOTINFO_MODLIST_GPA;
	}

	*entry_out = entry;
	*start_info_gpa_out = BOOTINFO_START_GPA;

	if (microvm_debug)
		fprintf(stderr, "[microvm] kernel entry=0x%llx image_end=0x%llx "
		    "initrd=0x%llx/%llu\n", (unsigned long long)entry,
		    (unsigned long long)image_end,
		    (unsigned long long)initrd_gpa,
		    (unsigned long long)initrd_size);

	return 0;
}
