// SPDX-License-Identifier: GPL-2.0
/*
 * Stage-2 EL2 coverage — host (EL1) side.
 *
 * The EL2 SanCov callback (hyp/nvhe/cov.c) records hyp *runtime* PCs into a
 * host-shared ring. syzkaller / vmlinux / DWARF use the link-time __kvm_nvhe_*
 * addresses, so before handing PCs to kcov_add_pcs() we convert runtime -> link
 * here, on the drain side (syzkaller must not guess the boot-time hyp layout).
 */
#include <linux/types.h>
#include <asm/sections.h>	/* __hyp_text_start / __hyp_text_end */
#include <asm/memory.h>		/* lm_alias */
#include <asm/kvm_mmu.h>	/* kern_hyp_va */

/*
 * Convert an EL2 hyp runtime PC to its vmlinux __kvm_nvhe_* link address.
 *
 * The nVHE hyp-VA is a *tagged* __kern_hyp_va() transform (NOT hyp_physvirt_offset,
 * which is the hyp-VA<->phys relation). It is only a constant offset *within* the
 * contiguous hyp .text, so a single anchor reverses PCs in that range and a range
 * check is mandatory. The runtime base is kern_hyp_va(lm_alias(__hyp_text_start))
 * (matching hyp_events.c): lm_alias() is required because __hyp_text_start is a
 * kernel-image symbol while __kern_hyp_va() expects the linear-map alias of the
 * same physical page.
 *
 * Prerequisite: CONFIG_RANDOMIZE_BASE=n, so the returned link address matches the
 * offline vmlinux. Returns 0 for a PC outside the hyp text (caller drops it).
 */
unsigned long pkvm_cov_runtime_to_link(unsigned long pc)
{
	unsigned long link_start = (unsigned long)__hyp_text_start;
	unsigned long hyp_start  = (unsigned long)kern_hyp_va(lm_alias((unsigned long)__hyp_text_start));
	unsigned long text_size  = (unsigned long)__hyp_text_end - (unsigned long)__hyp_text_start;

	if (pc < hyp_start || pc - hyp_start >= text_size)
		return 0;
	return link_start + (pc - hyp_start);
}
