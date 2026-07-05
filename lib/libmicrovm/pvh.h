/*
 * PVH direct-boot ABI definitions.
 *
 * The structures and constants below are part of the Xen public ABI and are
 * reproduced here from Xen's MIT-licensed public headers so this VMM can build
 * the boot information block a PVH guest expects. They are NOT part of the GPL
 * Xen hypervisor proper; the public interface headers are MIT-licensed.
 *
 *   xen/include/public/arch-x86/hvm/start_info.h  (SPDX: MIT)
 *   xen/include/public/elfnote.h                  (SPDX: MIT)
 *     Copyright (c) 2016, Citrix Systems, Inc.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef _MICROVM_PVH_H_
#define _MICROVM_PVH_H_

#include <stdint.h>

/*
 * ELF note used to locate the 32-bit PVH entry point. The note lives in a
 * PT_NOTE segment, has the name "Xen", type XEN_ELFNOTE_PHYS32_ENTRY, and a
 * descriptor holding the physical entry address. Xen (and this VMM) launch the
 * guest there in 32-bit protected mode with paging disabled.
 */
#define XEN_ELFNOTE_PHYS32_ENTRY	18
#define PVH_NOTE_NAME			"Xen"

/* Magic in hvm_start_info.magic: "xEn3" with the 0x80 bit of 'E' set. */
#define XEN_HVM_START_MAGIC_VALUE	0x336ec578

/* hvm_memmap_table_entry.type values (an E820-like memory map). */
#define XEN_HVM_MEMMAP_TYPE_RAM		1
#define XEN_HVM_MEMMAP_TYPE_RESERVED	2
#define XEN_HVM_MEMMAP_TYPE_ACPI	3
#define XEN_HVM_MEMMAP_TYPE_NVS		4
#define XEN_HVM_MEMMAP_TYPE_UNUSABLE	5
#define XEN_HVM_MEMMAP_TYPE_DISABLED	6
#define XEN_HVM_MEMMAP_TYPE_PMEM	7

struct hvm_start_info {
	uint32_t magic;		/* XEN_HVM_START_MAGIC_VALUE */
	uint32_t version;	/* Version of this structure. */
	uint32_t flags;		/* SIF_xxx flags. */
	uint32_t nr_modules;	/* Number of modules passed to the kernel. */
	uint64_t modlist_paddr;	/* Physical address of hvm_modlist_entry array. */
	uint64_t cmdline_paddr;	/* Physical address of the command line. */
	uint64_t rsdp_paddr;	/* Physical address of the RSDP ACPI structure. */
	uint64_t memmap_paddr;	/* Physical address of hvm_memmap_table_entry[]. */
	uint32_t memmap_entries;/* Number of entries in the memmap table. */
	uint32_t reserved;	/* Must be zero. */
};

struct hvm_modlist_entry {
	uint64_t paddr;		/* Physical address of the module. */
	uint64_t size;		/* Size of the module in bytes. */
	uint64_t cmdline_paddr;	/* Physical address of the command line. */
	uint64_t reserved;
};

struct hvm_memmap_table_entry {
	uint64_t addr;		/* Base address of the memory region. */
	uint64_t size;		/* Size of the memory region in bytes. */
	uint32_t type;		/* XEN_HVM_MEMMAP_TYPE_xxx. */
	uint32_t reserved;	/* Must be zero for Version 1. */
};

#endif /* _MICROVM_PVH_H_ */
