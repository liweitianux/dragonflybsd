/*
 * Copyright (c) 2026 The DragonFly Project.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of The DragonFly Project nor the names of its
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific, prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE
 * COPYRIGHT HOLDERS OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * Runtime choice between the retpoline thunks and plain indirect branches.
 *
 * The kernel is built with -mindirect-branch=thunk-extern, so every indirect
 * branch in the kernel and in every module goes through one of the 16 thunks
 * in retpoline.s.  On a CPU where branch target injection is not a problem,
 * the thunks are pure overhead, so replace them at boot with the branch they
 * implement.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/sysctl.h>

#include <machine/cpufunc.h>
#include <machine/md_var.h>
#include <machine/specialreg.h>

#ifdef _KERNEL_VIRTUAL
#include <sys/mman.h>
#else
#include <vm/vm.h>
#include <vm/vm_param.h>
#include <vm/pmap.h>
#endif

/*
 * Bytes per thunk slot.  retpoline.s pads every body to this size with int3,
 * so a body can be replaced by a copy that covers exactly one slot.
 */
#define RETPOLINE_SLOT		32

/*
 * Choices for machdep.retpoline tunable.
 */
#define RETPOLINE_AUTO		(-1)	/* decide from the CPU */
#define RETPOLINE_OFF		0	/* use plain indirect branches */
#define RETPOLINE_ON		1	/* use the thunks */

extern uint8_t __x86_indirect_thunk_rax[];
extern uint8_t __x86_indirect_thunk_rbx[];
extern uint8_t __x86_indirect_thunk_rcx[];
extern uint8_t __x86_indirect_thunk_rdx[];
extern uint8_t __x86_indirect_thunk_rsi[];
extern uint8_t __x86_indirect_thunk_rdi[];
extern uint8_t __x86_indirect_thunk_rbp[];
extern uint8_t __x86_indirect_thunk_r8[];
extern uint8_t __x86_indirect_thunk_r9[];
extern uint8_t __x86_indirect_thunk_r10[];
extern uint8_t __x86_indirect_thunk_r11[];
extern uint8_t __x86_indirect_thunk_r12[];
extern uint8_t __x86_indirect_thunk_r13[];
extern uint8_t __x86_indirect_thunk_r14[];
extern uint8_t __x86_indirect_thunk_r15[];
extern uint8_t __x86_indirect_thunk[];

/*
 * Replacement bodies.  "jmp *%reg" is "ff /4" (0xe0 + register number), with
 * a REX.B prefix for r8-r15.  The stack variant gets "ret".
 */
static const struct retpoline_patch {
	void		*thunk;
	uint8_t		code[3];
	uint8_t		len;
} retpoline_patches[] = {
	{ __x86_indirect_thunk_rax, { 0xff, 0xe0 }, 2 },	/* jmp *%rax */
	{ __x86_indirect_thunk_rbx, { 0xff, 0xe3 }, 2 },
	{ __x86_indirect_thunk_rcx, { 0xff, 0xe1 }, 2 },
	{ __x86_indirect_thunk_rdx, { 0xff, 0xe2 }, 2 },
	{ __x86_indirect_thunk_rsi, { 0xff, 0xe6 }, 2 },
	{ __x86_indirect_thunk_rdi, { 0xff, 0xe7 }, 2 },
	{ __x86_indirect_thunk_rbp, { 0xff, 0xe5 }, 2 },
	{ __x86_indirect_thunk_r8,  { 0x41, 0xff, 0xe0 }, 3 },
	{ __x86_indirect_thunk_r9,  { 0x41, 0xff, 0xe1 }, 3 },
	{ __x86_indirect_thunk_r10, { 0x41, 0xff, 0xe2 }, 3 },
	{ __x86_indirect_thunk_r11, { 0x41, 0xff, 0xe3 }, 3 },
	{ __x86_indirect_thunk_r12, { 0x41, 0xff, 0xe4 }, 3 },
	{ __x86_indirect_thunk_r13, { 0x41, 0xff, 0xe5 }, 3 },
	{ __x86_indirect_thunk_r14, { 0x41, 0xff, 0xe6 }, 3 },
	{ __x86_indirect_thunk_r15, { 0x41, 0xff, 0xe7 }, 3 },
	{ __x86_indirect_thunk,     { 0xc3 }, 1 },		/* ret */
};

static int retpoline_policy = RETPOLINE_AUTO;
static char retpoline_state[96] = "not initialized";

SYSCTL_INT(_machdep, OID_AUTO, retpoline, CTLFLAG_RD,
	&retpoline_policy, 0, "Retpoline policy: -1=auto, 0=disable, 1=enable");
SYSCTL_STRING(_machdep, OID_AUTO, retpoline_state, CTLFLAG_RD,
	retpoline_state, 0, "Result of the retpoline decision");

/*
 * Returns NULL if the CPU needs the thunks, or a description of why it does
 * not.
 */
static const char *
retpoline_hw_reason(void)
{
#ifdef _KERNEL_VIRTUAL
	/*
	 * Vkernel cannot read IA32_ARCH_CAPABILITIES, so it cannot tell.
	 * Keep the thunks unless machdep.retpoline says otherwise.
	 */
	return (NULL);
#else
	uint32_t p[4];

	switch (cpu_vendor_id) {
	case CPU_VENDOR_INTEL:
		/* IBRS_ALL means enhanced IBRS. */
		if (cpu_ia32_arch_caps & IA32_ARCH_CAP_IBRS_ALL)
			return ("enhanced IBRS");
		return (NULL);
	case CPU_VENDOR_AMD:
		bzero(p, sizeof(p));
		do_cpuid(0x80000008U, p);
		if (p[1] & CPUID_CAPEX_IBRS_ALWAYSON)
			return ("IBRS always on");
		if (p[1] & CPUID_CAPEX_PREFER_IBRS)
			return ("CPU prefers IBRS");
		return (NULL);
	default:
		return (NULL);
	}
#endif
}

static int
retpoline_replace(const struct retpoline_patch *p)
{
	uint8_t body[RETPOLINE_SLOT];
	u_int regs[4];
#ifdef _KERNEL_VIRTUAL
	vm_offset_t start, end;
	int prot;
#else
	void *alias;
	vm_paddr_t pa;
#endif

	memset(body, 0xCC, sizeof(body));
	memcpy(body, p->code, p->len);

#ifdef _KERNEL_VIRTUAL
	/*
	 * The host maps the vkernel text read-only, unprotect it.
	 */
	prot = PROT_READ | PROT_EXEC;
	start = trunc_page(p->thunk);
	end = round_page((vm_offset_t)p->thunk + RETPOLINE_SLOT);
	if (mprotect((void *)start, end - start, prot | PROT_WRITE) != 0) {
		kprintf("Retpoline: cannot unprotect %p\n", p->thunk);
		return (-1);
	}
	bcopy(body, p->thunk, sizeof(body));
	if (mprotect((void *)start, end - start, prot) != 0)
		kprintf("Retpoline: cannot reprotect %p\n", p->thunk);
#else
	/*
	 * Write through the direct map: it aliases all of physical memory and
	 * is always writable.
	 */
	pa = pmap_kextract((vm_offset_t)p->thunk);
	if (pa == 0) {
		kprintf("Retpoline: cannot translate %p\n", p->thunk);
		return (-1);
	}
	alias = (void *)PHYS_TO_DMAP(pa);
	/*
	 * Ensure the DMAP alias points to the correct memory.
	 */
	if (memcmp(alias, p->thunk, sizeof(body)) != 0) {
		kprintf("Retpoline: %p and its DMAP alias differ\n", p->thunk);
		return (-1);
	}
	bcopy(body, alias, sizeof(body));
#endif

	/*
	 * Self-modifying code: do a full serialization using CPUID.
	 */
	do_cpuid(0, regs);

	/*
	 * Read the bytes back to verify the result.
	 */
	if (memcmp(p->thunk, body, sizeof(body)) != 0) {
		kprintf("Retpoline: %p does not read back\n", p->thunk);
		return (-1);
	}

	return (0);
}

static void
retpoline_init(void *dummy __unused)
{
	const char *why;
	bool use_thunks;
	size_t i, npatched;

	TUNABLE_INT_FETCH("machdep.retpoline", &retpoline_policy);
	if (retpoline_policy < RETPOLINE_AUTO ||
	    retpoline_policy > RETPOLINE_ON)
	{
		kprintf("Retpoline: ignoring invalid machdep.retpoline=%d\n",
		    retpoline_policy);
		retpoline_policy = RETPOLINE_AUTO;
	}

	use_thunks = true;
	why = "CPU requires the mitigation";
	switch (retpoline_policy) {
	case RETPOLINE_AUTO:
		why = retpoline_hw_reason();
		if (why == NULL)
			why = "CPU requires the mitigation";
		else
			use_thunks = false;
		break;
	case RETPOLINE_ON:
		why = "forced by machdep.retpoline";
		break;
	case RETPOLINE_OFF:
		use_thunks = false;
		why = "forced by machdep.retpoline";
		break;
	}

	if (!use_thunks) {
		/*
		 * Validate the thunk slots to catch obvious out-of-sync.
		 */
		uintptr_t a = (uintptr_t)__x86_indirect_thunk_rax;
		uintptr_t b = (uintptr_t)__x86_indirect_thunk_rbx;

		if (b - a != RETPOLINE_SLOT) {
			kprintf("Retpoline: thunk slots are not %d "
			    "bytes apart, keeping the thunks\n",
			    RETPOLINE_SLOT);
			use_thunks = true;
			why = "cannot patch the thunks";
		}
	}

	if (use_thunks) {
		ksnprintf(retpoline_state, sizeof(retpoline_state),
		    "enabled: %s", why);
	} else {
		npatched = 0;
		for (i = 0; i < nitems(retpoline_patches); i++) {
			if (retpoline_replace(&retpoline_patches[i]) == 0)
				npatched++;
		}
		ksnprintf(retpoline_state, sizeof(retpoline_state),
		    "disabled: %s, %zu of %zu thunks replaced",
		    why, npatched, nitems(retpoline_patches));
	}
	kprintf("Retpoline: %s\n", retpoline_state);
}

/*
 * Run at SI_BOOT2_START_CPU, i.e. after the CPU has been identified and before
 * the APs are started, so no other CPU can be executing the bytes being
 * replaced and no rendezvous or IPI is needed.
 */
SYSINIT(retpoline, SI_BOOT2_START_CPU, SI_ORDER_MIDDLE, retpoline_init, NULL);
