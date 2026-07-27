/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Stage-2 EL2 coverage — shared ring layout between the pKVM hyp (producer) and
 * the host (consumer). Exactly one 4K page: 8-byte header + 511 * 8-byte PCs.
 * The hyp's __sanitizer_cov_trace_pc appends EL2 hyp-VA PCs; the host drains
 * them into the current task's KCOV area via kcov_add_pcs().
 */
#ifndef __ASM_KVM_PKVM_COV_H
#define __ASM_KVM_PKVM_COV_H

#include <linux/types.h>

/* header (2 * u32) + PKVM_COV_RING_PCS * u64 == PAGE_SIZE (4096). */
#define PKVM_COV_RING_PCS 511

/* flags: a single u32 (fix: a separate `enabled` field would push the struct to 4104). */
#define PKVM_COV_FLAG_ENABLED  (1u << 0)	/* host-set: collect on this CPU */
#define PKVM_COV_FLAG_OVERFLOW (1u << 1)	/* hyp-set: ring filled, PCs dropped */

struct pkvm_cov_ring {
	__u32 count;			/* number of valid entries in pcs[] (hyp-written) */
	__u32 flags;			/* PKVM_COV_FLAG_* */
	__u64 pcs[PKVM_COV_RING_PCS];	/* EL2 hyp-VA program counters */
};

#endif /* __ASM_KVM_PKVM_COV_H */
