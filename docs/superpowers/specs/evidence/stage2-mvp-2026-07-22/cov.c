// SPDX-License-Identifier: GPL-2.0
/*
 * Stage-2 EL2 coverage producer (pKVM hyp side).
 *
 * The Rust hyp crate is built with SanCov trace-pc (build_rust.sh, gated by
 * CONFIG_PKVM_EL2_COV), so every basic block calls __sanitizer_cov_trace_pc().
 * This file provides that callback.  It is in the nvhe C build, which is NOT
 * SanCov-instrumented, and the callback is additionally marked `notrace`, so it
 * cannot recurse into itself.
 *
 * The ring is a host-shared page (struct pkvm_cov_ring) registered per-CPU by
 * __pkvm_cov_setup.  The host sets PKVM_COV_FLAG_ENABLED and reads count/pcs[]
 * directly; the hyp appends PCs with a release (pc, then count) so the host's
 * acquire load of `count` only exposes fully-written PCs.  MVP assumes the
 * fuzzer thread is CPU-pinned, so producer and consumer share one CPU.
 */
#include <asm/percpu.h>
#include <asm/kvm_pkvm_cov.h>

static DEFINE_PER_CPU(struct pkvm_cov_ring *, pkvm_cov_ring);

/* SanCov trace-pc callback: record the caller's PC (the instrumented block). */
void notrace __sanitizer_cov_trace_pc(void)
{
	struct pkvm_cov_ring *r = __this_cpu_read(pkvm_cov_ring);
	u32 count;

	if (!r || !(READ_ONCE(r->flags) & PKVM_COV_FLAG_ENABLED))
		return;
	count = READ_ONCE(r->count);
	if (count < PKVM_COV_RING_PCS) {
		r->pcs[count] = (u64)__builtin_return_address(0);
		barrier();			/* publish the PC before the count (same-CPU: compiler barrier) */
		WRITE_ONCE(r->count, count + 1);
	} else {
		WRITE_ONCE(r->flags, READ_ONCE(r->flags) | PKVM_COV_FLAG_OVERFLOW);
	}
}

/* Register the (host->hyp shared) ring page for this CPU.  Called from the
 * __pkvm_cov_setup hypercall handler with the ring's hyp VA (or NULL to clear). */
void pkvm_cov_set_ring(struct pkvm_cov_ring *ring_hyp_va)
{
	__this_cpu_write(pkvm_cov_ring, ring_hyp_va);
}
