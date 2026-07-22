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
#include <linux/rcupdate.h>
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
/*
 * RCU-protected so the lockless #23 readers (pkvm_cov_begin/end) can never race a
 * concurrent disable() into a use-after-free. begin()/end() run under
 * kvm->mmu_lock (preemption off on a non-RT kernel = an RCU read-side section);
 * disable() publishes NULL then synchronize_rcu() before it may free the page.
 */
static struct pkvm_cov_ring __rcu *pkvm_cov_host_ring;	/* host linear-map vaddr, or NULL */
static int pkvm_cov_owner_cpu = -1;	/* CPU whose EL2 ring this is */

/* IPI body: tear down the EL2 ring on the owner CPU (pfn == 0), capturing the result. */
struct pkvm_cov_teardown { int ret; };
static void pkvm_cov_hyp_teardown_ipi(void *arg)
{
	struct pkvm_cov_teardown *t = arg;

	t->ret = kvm_call_hyp_nvhe(__pkvm_cov_setup, 0);
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
	rcu_assign_pointer(pkvm_cov_host_ring, ring);	/* release: publishes the initialized ring */
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
 * Teardown as a full active-drain lifecycle, not just a NULL write:
 *   1. disarm + publish NULL      -> new begin() bails
 *   2. synchronize_rcu()          -> wait for in-flight begin()/end() to finish
 *   3. __pkvm_cov_setup(0) on the owner CPU, CHECKED -> EL2 drops its pointer
 *   4. only then unshare + free
 * If step 3 cannot be confirmed (owner CPU offline, IPI failure, hyp error), EL2
 * may still reference the page: keep it disabled and LEAK the page rather than
 * free something the hypervisor can still touch.
 */
static void pkvm_cov_disable(void)
{
	struct pkvm_cov_ring *ring;
	struct pkvm_cov_teardown td = { .ret = -EIO };
	int owner, ipi;

	mutex_lock(&pkvm_cov_lock);
	ring = rcu_dereference_protected(pkvm_cov_host_ring,
					 lockdep_is_held(&pkvm_cov_lock));
	if (!ring)
		goto out;
	owner = pkvm_cov_owner_cpu;

	/* 1. reject new batches. */
	smp_store_release(&ring->flags, 0);		/* disarm the producer */
	rcu_assign_pointer(pkvm_cov_host_ring, NULL);	/* new begin() sees NULL */
	/* 2. wait for readers that already loaded the old pointer (preempt-off). */
	synchronize_rcu();

	/* 3. unregister EL2's per-CPU pointer on the owner CPU; must be confirmed. */
	ipi = (owner < 0) ? -ENXIO
			  : smp_call_function_single(owner, pkvm_cov_hyp_teardown_ipi, &td, 1);
	if (ipi || td.ret) {
		pr_warn("pkvm_cov: teardown unconfirmed (ipi=%d hyp=%d) — leaking ring page (EL2 may still map it)\n",
			ipi, td.ret);
		pkvm_cov_owner_cpu = -1;		/* stays disabled; page intentionally leaked */
		goto out;
	}

	/*
	 * 4. Undo the host->hyp share, CHECKED: kvm_unshare_hyp() is void (only
	 * WARN_ONs), so use the checked variant — if the unshare is not confirmed the
	 * page may still be hyp-mapped, so leak it rather than free.
	 */
	if (kvm_unshare_hyp_checked(ring, (char *)ring + PAGE_SIZE)) {
		pr_warn("pkvm_cov: unshare unconfirmed — leaking ring page (EL2 may still map it)\n");
		pkvm_cov_owner_cpu = -1;
		goto out;
	}
	free_page((unsigned long)ring);
	pkvm_cov_owner_cpu = -1;
	pr_info("pkvm_cov: ring disarmed\n");
out:
	mutex_unlock(&pkvm_cov_lock);
}

/*
 * #23 drain, part 1 — called just before pkvm_host_map_guest() with kvm->mmu_lock
 * held (preemption off, so smp_processor_id() is stable and this is an RCU
 * read-side section). Arm the ring only when this is the owner CPU AND the
 * faulting task is collecting KCOV trace-pc; else skip (coverage is optional,
 * never wrong). Returns the armed ring for pkvm_cov_end(), or NULL. Cheap: no
 * alloc, no sleep. The returned pointer stays valid until end() because
 * disable()'s free waits on synchronize_rcu() for this preempt-off section.
 */
struct pkvm_cov_ring *pkvm_cov_begin(void)
{
	/* preempt-off under mmu_lock is the RCU read-side; suppresses the lockdep check. */
	struct pkvm_cov_ring *ring =
		rcu_dereference_check(pkvm_cov_host_ring, !preemptible());

	if (!ring)
		return NULL;
	if (smp_processor_id() != READ_ONCE(pkvm_cov_owner_cpu)) {
		pr_warn_ratelimited("pkvm_cov: skip drain, CPU %d != owner %d\n",
				    smp_processor_id(), READ_ONCE(pkvm_cov_owner_cpu));
		return NULL;
	}
	if (!kcov_current_trace_pc())
		return NULL;

	WRITE_ONCE(ring->count, 0);
	smp_store_release(&ring->flags, PKVM_COV_FLAG_ENABLED);
	return ring;
}

/*
 * #23 drain, part 2 — called with the ring pkvm_cov_begin() returned, right after
 * pkvm_host_map_guest() returns (on BOTH the success and error paths), still under
 * mmu_lock. Disarm, then convert the EL2 runtime PCs to link addresses and append
 * them to the faulting task's KCOV area. No alloc / no sleep; a bounded loop over
 * at most PKVM_COV_RING_PCS.
 */
void pkvm_cov_end(struct pkvm_cov_ring *ring)
{
	u64 batch[64];
	u32 i, count, flags, nb = 0;

	flags = READ_ONCE(ring->flags);		/* snapshot OVERFLOW before disarming */
	smp_store_release(&ring->flags, 0);	/* disarm the producer */
	/* Acquire: pair with the EL2 callback's smp_store_release(&count) so every
	 * pcs[] entry published before `count` is visible here. */
	count = smp_load_acquire(&ring->count);
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
