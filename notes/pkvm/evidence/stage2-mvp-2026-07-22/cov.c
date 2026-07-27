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
#include <asm/barrier.h>
#include <asm/percpu.h>
#include <asm/kvm_pkvm_cov.h>
#include <nvhe/memory.h>
#include <nvhe/mem_protect.h>

static DEFINE_PER_CPU(struct pkvm_cov_ring *, pkvm_cov_ring);
static DEFINE_PER_CPU(bool, pkvm_cov_in_cb);

/* SanCov trace-pc callback: record the caller's PC (the instrumented block). */
void notrace __sanitizer_cov_trace_pc(void)
{
	struct pkvm_cov_ring *r = __this_cpu_read(pkvm_cov_ring);
	u32 count;

	/* Acquire: pair with the host's release when it sets ENABLED. */
	if (!r || !(smp_load_acquire(&r->flags) & PKVM_COV_FLAG_ENABLED))
		return;
	/*
	 * Drop nested callbacks on this CPU (e.g. an EL2 exception re-entering
	 * instrumented code) rather than racing the ring — better to lose a PC
	 * than to corrupt count/pcs. Interrupts are masked in the hyp, so the
	 * CPU is stable across this window.
	 */
	if (__this_cpu_read(pkvm_cov_in_cb))
		return;
	__this_cpu_write(pkvm_cov_in_cb, true);

	count = READ_ONCE(r->count);
	if (count < PKVM_COV_RING_PCS) {
		r->pcs[count] = (u64)__builtin_return_address(0);
		/* Release: the PC store is visible before the count the host acquires. */
		smp_store_release(&r->count, count + 1);
	} else {
		WRITE_ONCE(r->flags, READ_ONCE(r->flags) | PKVM_COV_FLAG_OVERFLOW);
	}

	__this_cpu_write(pkvm_cov_in_cb, false);
}

/*
 * __pkvm_cov_setup(pfn) hypercall body.  The host has already shared the page
 * via __pkvm_host_share_hyp(pfn); here we pin it and register it as this CPU's
 * ring (pfn != 0), or unpin + clear it (pfn == 0).  Lifecycle mirrors the hyp
 * trace ring (trace.c): share_hyp (host) -> hyp_pin_shared_mem (hyp), and the
 * reverse on teardown.  Returns 0 or a negative errno.
 */
int pkvm_cov_setup(u64 pfn)
{
	struct pkvm_cov_ring *old = __this_cpu_read(pkvm_cov_ring);
	void *va;
	int ret;

	/* Teardown: unpin the previously-registered ring on this CPU. */
	if (!pfn) {
		if (old) {
			hyp_unpin_shared_mem((void *)old, (void *)old + PAGE_SIZE);
			__this_cpu_write(pkvm_cov_ring, NULL);
		}
		return 0;
	}
	if (old)		/* already set up on this CPU */
		return -EBUSY;

	va = hyp_phys_to_virt((phys_addr_t)pfn << PAGE_SHIFT);
	ret = hyp_pin_shared_mem(va, va + PAGE_SIZE);
	if (ret)
		return ret;
	__this_cpu_write(pkvm_cov_ring, (struct pkvm_cov_ring *)va);
	return 0;
}
