// SPDX-License-Identifier: GPL-2.0
/*
 * Stage-2 EL2 coverage producer (pKVM hyp side).
 *
 * The Rust hyp crate is built with SanCov trace-pc (build_rust.sh), so every
 * basic block calls __sanitizer_cov_trace_pc().  This file provides that
 * callback.  It lives in the nvhe C build, which is NOT SanCov-instrumented, so
 * the callback cannot recurse into itself.
 *
 * MVP (Increment 1): a static per-CPU ring in hyp memory, gated by an enable
 * flag.  Increment 2 will point the ring at a host-shared page and add the
 * host-side drain; for now this proves the producer (instrumented Rust -> our
 * callback -> ring) builds and links.
 */
#include <asm/percpu.h>
#include <asm/kvm_pkvm_cov.h>

static DEFINE_PER_CPU(struct pkvm_cov_ring, pkvm_cov_ring);
static DEFINE_PER_CPU(bool, pkvm_cov_enabled);

/* SanCov trace-pc callback: record the caller's PC (the instrumented block). */
void __sanitizer_cov_trace_pc(void)
{
	struct pkvm_cov_ring *r;
	u32 c;

	if (!__this_cpu_read(pkvm_cov_enabled))
		return;
	r = this_cpu_ptr(&pkvm_cov_ring);
	c = r->count;
	if (c < PKVM_COV_RING_PCS) {
		r->pcs[c] = (u64)__builtin_return_address(0);
		r->count = c + 1;
	} else {
		r->overflow = 1;
	}
}

/* Enable/disable collection on this CPU and reset the ring (called around a boundary). */
void pkvm_cov_set_enabled(bool on)
{
	struct pkvm_cov_ring *r = this_cpu_ptr(&pkvm_cov_ring);

	r->count = 0;
	r->overflow = 0;
	__this_cpu_write(pkvm_cov_enabled, on);
}

/* Snapshot this CPU's ring into a caller buffer; returns the number of PCs. */
u32 pkvm_cov_snapshot(u64 *dst, u32 max)
{
	struct pkvm_cov_ring *r = this_cpu_ptr(&pkvm_cov_ring);
	u32 n = r->count < max ? r->count : max;
	u32 i;

	for (i = 0; i < n; i++)
		dst[i] = r->pcs[i];
	return n;
}
