// SPDX-License-Identifier: GPL-2.0
/*
 * Stage-2 EL2 coverage — host (EL1) side.
 *
 * The EL2 SanCov callback (hyp/nvhe/cov.c) records hyp *runtime* PCs into a
 * host-shared per-CPU ring. This file owns the host end of that ring:
 *
 *   - pkvm_cov_runtime_to_link(): convert a runtime hyp PC to its vmlinux
 *     __kvm_nvhe_ *link* address, so syzkaller/DWARF can symbolize it.
 *   - pkvm_cov_enable()/disable(): allocate + kvm_share_hyp() the ring page and
 *     register/unregister it with EL2 on the owner CPU (debugfs-triggered).
 *   - pkvm_cov_begin()/end(): the pKVM #23 (pkvm_host_map_guest) drain — arm the
 *     ring, let EL2 fill it during the map, then convert + hand the PCs to
 *     kcov_add_pcs() in the faulting task's KCOV area.
 *
 * MVP scope: ONE page, ONE CPU. Coverage is attributed by `current` (the task
 * whose KVM_RUN took the #23 fault), so the drain lands in exactly that task's
 * KCOV area. Because the ring is single-CPU, KVM_RUNs on any other CPU collect
 * nothing (owner-CPU guard below) — so this mode REQUIRES strict serialization
 * (procs=1, no Threaded, no Collide) and the executor pinned to the owner CPU.
 * Per-online-CPU rings (to allow controlled concurrency) are a later step.
 */
#include <linux/types.h>
#include <linux/kcov.h>
#include <linux/debugfs.h>
#include <linux/kvm_host.h>	/* kvm_debugfs_dir */
#include <linux/gfp.h>
#include <linux/mm.h>
#include <linux/smp.h>
#include <linux/mutex.h>
#include <linux/printk.h>
#include <linux/preempt.h>
#include <asm/barrier.h>
#include <asm/sections.h>	/* __hyp_text_start / __hyp_text_end */
#include <asm/memory.h>		/* lm_alias */
#include <asm/kvm_asm.h>	/* __KVM_HOST_SMCCC_FUNC___pkvm_cov_setup */
#include <asm/kvm_host.h>	/* kvm_call_hyp_nvhe */
#include <asm/kvm_mmu.h>	/* kern_hyp_va, kvm_share_hyp, kvm_unshare_hyp */
#include <asm/kvm_pkvm_cov.h>

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
 * Prerequisite: CONFIG_RANDOMIZE_BASE=n (enforced by Kconfig), so the returned
 * link address matches the offline vmlinux. Returns 0 for a PC outside the hyp
 * text (caller drops it).
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

/* ---- single-page / single-CPU host ring state (MVP) ---- */

static DEFINE_MUTEX(pkvm_cov_lock);	/* serializes enable/disable */
static struct pkvm_cov_ring *pkvm_cov_host_ring;	/* host linear-map vaddr, or NULL */
static int pkvm_cov_owner_cpu = -1;	/* CPU whose EL2 ring this is */

/* IPI body: tear down the EL2 ring on the owner CPU (pfn == 0). */
static void pkvm_cov_hyp_teardown_ipi(void *unused)
{
	kvm_call_hyp_nvhe(__pkvm_cov_setup, 0);
}

/*
 * Allocate the shared ring, kvm_share_hyp() it (public API — keeps host page
 * refcounts correct, unlike a raw __pkvm_host_share_hyp), and register it as this
 * CPU's EL2 ring. The caller must be pinned to the intended owner CPU; that CPU
 * is recorded and enforced at drain time.
 */
static int pkvm_cov_enable(void)
{
	struct pkvm_cov_ring *ring;
	unsigned long page;
	int cpu, ret;

	mutex_lock(&pkvm_cov_lock);
	if (pkvm_cov_host_ring) {
		ret = -EBUSY;
		goto out;
	}
	/* Normal contiguous low-end page (not vmalloc): shareable with EL2. */
	page = get_zeroed_page(GFP_KERNEL);
	if (!page) {
		ret = -ENOMEM;
		goto out;
	}
	ring = (struct pkvm_cov_ring *)page;

	ret = kvm_share_hyp(ring, (char *)ring + PAGE_SIZE);
	if (ret)
		goto free;

	cpu = get_cpu();	/* pin to this CPU for the setup hypercall */
	ret = kvm_call_hyp_nvhe(__pkvm_cov_setup, page_to_pfn(virt_to_page(ring)));
	put_cpu();
	if (ret)
		goto unshare;

	pkvm_cov_owner_cpu = cpu;
	pkvm_cov_host_ring = ring;
	pr_info("pkvm_cov: ring armed on CPU %d\n", cpu);
	ret = 0;
	goto out;

unshare:
	kvm_unshare_hyp(ring, (char *)ring + PAGE_SIZE);
free:
	free_page(page);
out:
	mutex_unlock(&pkvm_cov_lock);
	return ret;
}

/*
 * Teardown in the safe order: stop the producer (clear ENABLED) and make begin()
 * skip (NULL the host pointer) BEFORE EL2 drops its pointer and we unshare/free —
 * never unshare or free a page EL2 might still reach.
 */
static void pkvm_cov_disable(void)
{
	struct pkvm_cov_ring *ring;
	int owner;

	mutex_lock(&pkvm_cov_lock);
	ring = pkvm_cov_host_ring;
	if (!ring)
		goto out;
	owner = pkvm_cov_owner_cpu;

	smp_store_release(&ring->flags, 0);	/* disarm the producer */
	pkvm_cov_host_ring = NULL;		/* begin() now skips */

	/* EL2 clears its per-CPU pointer on the owner CPU (cov.c reverses the pin). */
	if (owner >= 0)
		smp_call_function_single(owner, pkvm_cov_hyp_teardown_ipi, NULL, 1);

	kvm_unshare_hyp(ring, (char *)ring + PAGE_SIZE);
	free_page((unsigned long)ring);
	pkvm_cov_owner_cpu = -1;
	pr_info("pkvm_cov: ring disarmed\n");
out:
	mutex_unlock(&pkvm_cov_lock);
}

/*
 * #23 drain, part 1 — called just before pkvm_host_map_guest() with kvm->mmu_lock
 * held (preemption off, so smp_processor_id() is stable). Arm the ring only when
 * this is the owner CPU AND the faulting task is collecting KCOV trace-pc; else
 * skip (coverage is optional, never wrong). Cheap: no alloc, no sleep.
 */
bool pkvm_cov_begin(void)
{
	struct pkvm_cov_ring *ring = READ_ONCE(pkvm_cov_host_ring);

	if (!ring)
		return false;
	if (smp_processor_id() != pkvm_cov_owner_cpu) {
		pr_warn_ratelimited("pkvm_cov: skip drain, CPU %d != owner %d\n",
				    smp_processor_id(), pkvm_cov_owner_cpu);
		return false;
	}
	if (!kcov_current_trace_pc())
		return false;

	WRITE_ONCE(ring->count, 0);
	smp_store_release(&ring->flags, PKVM_COV_FLAG_ENABLED);
	return true;
}

/*
 * #23 drain, part 2 — called right after pkvm_host_map_guest() returns (on BOTH
 * the success and error paths), still under mmu_lock. Disarm, then convert the
 * EL2 runtime PCs to link addresses and append them to the faulting task's KCOV
 * area. No alloc / no sleep; a bounded loop over at most PKVM_COV_RING_PCS.
 */
void pkvm_cov_end(void)
{
	struct pkvm_cov_ring *ring = READ_ONCE(pkvm_cov_host_ring);
	u64 batch[64];
	u32 i, count, flags, nb = 0;

	if (!ring)
		return;

	count = READ_ONCE(ring->count);
	flags = READ_ONCE(ring->flags);
	smp_store_release(&ring->flags, 0);	/* disarm before we walk the ring */
	if (count > PKVM_COV_RING_PCS)
		count = PKVM_COV_RING_PCS;

	for (i = 0; i < count; i++) {
		unsigned long link = pkvm_cov_runtime_to_link((unsigned long)ring->pcs[i]);

		if (!link)
			continue;
		batch[nb++] = link;
		if (nb == ARRAY_SIZE(batch)) {
			kcov_add_pcs(batch, nb);
			nb = 0;
		}
	}
	if (nb)
		kcov_add_pcs(batch, nb);

	if (flags & PKVM_COV_FLAG_OVERFLOW)
		pr_warn_ratelimited("pkvm_cov: ring overflow (%u PCs) — coverage truncated\n",
				    PKVM_COV_RING_PCS);
}

/* ---- debugfs control: kvm/pkvm_cov/{enable,owner_cpu} ---- */

static int pkvm_cov_enable_set(void *data, u64 val)
{
	if (val)
		return pkvm_cov_enable();
	pkvm_cov_disable();
	return 0;
}
DEFINE_DEBUGFS_ATTRIBUTE(pkvm_cov_enable_fops, NULL, pkvm_cov_enable_set, "%llu\n");

static int pkvm_cov_owner_get(void *data, u64 *val)
{
	*val = (u64)(s64)READ_ONCE(pkvm_cov_owner_cpu);
	return 0;
}
DEFINE_DEBUGFS_ATTRIBUTE(pkvm_cov_owner_fops, pkvm_cov_owner_get, NULL, "%lld\n");

static int __init pkvm_cov_debugfs_init(void)
{
	struct dentry *dir;

	if (!kvm_debugfs_dir || IS_ERR(kvm_debugfs_dir))
		return 0;
	dir = debugfs_create_dir("pkvm_cov", kvm_debugfs_dir);
	debugfs_create_file("enable", 0200, dir, NULL, &pkvm_cov_enable_fops);
	debugfs_create_file("owner_cpu", 0400, dir, NULL, &pkvm_cov_owner_fops);
	return 0;
}
late_initcall(pkvm_cov_debugfs_init);
