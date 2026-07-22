/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Stage-2 EL2 coverage — shared ring layout between the pKVM hyp (producer) and
 * the host (consumer). One page; the hyp's __sanitizer_cov_trace_pc appends EL2
 * PCs, the host drains them into the current task's KCOV area.
 */
#ifndef __ASM_KVM_PKVM_COV_H
#define __ASM_KVM_PKVM_COV_H

#include <linux/types.h>

/* Fits one 4K page: 8-byte header + 511 * 8-byte PCs = 4096. */
#define PKVM_COV_RING_PCS 511

struct pkvm_cov_ring {
	__u32 count;			/* number of valid entries in pcs[] */
	__u32 overflow;			/* set by the hyp if it hit capacity */
	__u64 pcs[PKVM_COV_RING_PCS];	/* EL2 hyp-VA program counters */
};

#endif /* __ASM_KVM_PKVM_COV_H */
