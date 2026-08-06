# Reachability — which guests can actually reach these five defects

> Line numbers are on **klinux `@a35f0a8c899e`** (working tree clean at the time of writing). They drift; the issue documents cite `#4` and differ by a few lines. Read with `git show a35f0a8c899e:<path>`.

All five filed issues live in **one** code path: an **ordinary (non-protected) guest running on a host booted `kvm-arm.mode=protected`**. None of them is reachable by a protected VM. This is worth stating in one place because it is easy to read the issue list as "pKVM is full of holes" when the accurate reading is narrower and, in one respect, worse.

## The two gates

A protected VM is turned away twice, in code, before it can reach any of this.

**Gate 1 — the stage-2 unmap returns immediately** (`arch/arm64/kvm/mmu.c:456`):

```c
static void __unmap_stage2_range(struct kvm_s2_mmu *mmu, phys_addr_t start, u64 size,
				 bool may_block)
{
	struct kvm *kvm = kvm_s2_mmu_to_kvm(mmu);
	phys_addr_t end = start + size;

	if (is_protected_kvm_enabled() && kvm->arch.pkvm.enabled)
		return;
	...
```

Everything below that line — `stage2_apply_range()`, `___unmap_stage2_range()`, `pkvm_unmap_range()`, `pkvm_unmap_guest()` — is unreachable for a protected VM.

**Gate 2 — dirty logging is rejected outright** (`arch/arm64/kvm/mmu.c:2610`, in `kvm_arch_prepare_memory_region()`):

```c
	if (kvm_vm_is_protected(kvm)) {
		...
		if (new && kvm->arch.pkvm.enabled &&
		    new->flags & (KVM_MEM_LOG_DIRTY_PAGES | KVM_MEM_READONLY)) {
			return -EPERM;
		}
	}
```

`KVM_SET_USER_MEMORY_REGION` with `KVM_MEM_LOG_DIRTY_PAGES` on a protected VM fails with `-EPERM`. The write-protect and relax-perms hypercalls behind it carry their own `WARN_ON(kvm_vm_is_protected(...))` as a second line of defence.

Neither gate is an oversight. The whole point of a protected VM is that the host cannot read its memory; dirty logging exists so the host can copy guest memory. A protected VM's pages are **donated** (`PKVM_PAGE_OWNED`, `guest_complete_donation()`), not **shared** (`PKVM_PAGE_SHARED_BORROWED`, `guest_complete_share()`), so the state check every one of these paths performs would reject them even without the explicit gates.

## What each issue needs

| issue | protected VM? | needs dirty logging? | needs anything else |
| --- | --- | --- | --- |
| **001** self-deadlock | **no** — gate 1 | no | a second `KVM_ARM_VCPU_INIT` on a vCPU that has run; CPU without `ARM64_HAS_STAGE2_FWB` |
| **002** TLB range flush | **no** — gate 2 | yes | — |
| **003** `-E2BIG` on huge pages | **no** — gate 2 | yes | THP-backed guest memory (the default) |
| **004** donation accounting | **no** — gate 2 | yes | — |
| **005** page-state wipe | **no** — gates 1 and 2 | yes | the dirty-log call must *succeed* |

## The part that inverts the obvious conclusion

"We mainly run protected VMs, so these matter less" is half right, and the half that is wrong matters more.

**`kvm-arm.mode=protected` is a host boot parameter, not a per-VM property.** Once a machine boots that way, *every* VM on it goes through the pKVM MMU — including ordinary ones. You do not get to switch the vulnerable path off; you only get to decide whether anything uses it. And **any local process that can open `/dev/kvm` can create an ordinary VM**.

That splits the five sharply:

- **001 is workload-independent.** It needs an ordinary VM and nothing else — no migration, no snapshots, no confidential workload. On a pKVM host without the fix, any process with `/dev/kvm` access wedges the machine, and the wedge spreads: the stuck task holds `mmap_lock`, rwsem fairness queues every later reader behind it, and anything that walks `/proc` freezes in turn. This is a local denial of service, and "we intend to run only protected VMs" does not mitigate it. It only takes someone *able* to run an ordinary one.
- **002 / 003 / 004 / 005 are workload-dependent.** They need a VMM that actually enables dirty logging on an ordinary guest — that is, live migration or snapshotting. A box that never migrates an ordinary VM never reaches them. On such a box they are genuinely dormant.

Two hardware conditions narrow 001 further, and are worth knowing because they explain why it survived so long unnoticed: the host must lack `ARM64_HAS_STAGE2_FWB` (FWB-capable silicon takes the `icache_inval_all_pou()` branch and never reaches the deadlock), and the guest must be non-protected. Phytium D3000 and Great Wall N90 both lack FWB. Most upstream test hardware does not.

## Upstream builds for this path deliberately

The np-guest-under-pKVM configuration is not an artifact of how we fuzz. Mainline `fce886a60207` ("KVM: arm64: Plumb the pKVM MMU in KVM") exists precisely to route ordinary guests' stage-2 through pKVM, and the hypercalls that survive there — `__pkvm_host_relax_perms_guest()`, `__pkvm_host_wrprotect_guest()` — open with `WARN_ON(kvm_vm_is_protected(...))`, i.e. they serve **only** ordinary guests. Nobody maintains that for a configuration that does not ship.

## The gap this leaves

Every one of the five was found on the ordinary-guest path. The protected path is **comparatively unexplored**, not demonstrated clean.

The 2026-08-06 campaign ([2026-08-06-n90-full-surface](runs/2026-08-06-n90-full-surface.md)) is the first to enable the protected-VM surface in full — 16 descriptors covering creation, the constrained vCPU path, the three memslot-reject composites and the `KVM_CAP_ARM_PROTECTED_VM` trio. At the time of writing it has produced no findings there. That is a starting point, not a result: it is one campaign, on a machine whose exposed surface for protected VMs is much smaller than for ordinary ones, and the reject composites deliberately execute no guest memory.

## What would change this document

The priority ordering above depends on one fact this project does not currently have: **what the product's real workload mix is.** A machine that only ever runs protected VMs, with nothing else touching `/dev/kvm`, has four dormant issues and one live one. A machine that mixes ordinary and protected guests, or that migrates ordinary guests, has five live ones. Worth establishing before this is used to set fix priorities.
