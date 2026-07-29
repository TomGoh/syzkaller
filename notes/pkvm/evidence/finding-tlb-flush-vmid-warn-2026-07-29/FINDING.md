# Finding: kvm_tlb_flush_vmid_range() calls a hypercall pKVM forbids — the stage-2 TLB is never invalidated (2026-07-29)

**`WARNING: CPU: 0 PID: … at arch/arm64/kvm/hyp/pgtable.c:639 kvm_tlb_flush_vmid_range`**, N90 kernel 6.6.30+ #47. Fires continuously under the broadened KVM syscall set — 15,528 occurrences recorded in a single pre-hang window. Console capture in `kvm_tlb_flush_vmid-warn-dmesg.txt`.

**This supersedes the original write-up**, which recorded only the symptom ("EL2 returns non-success for `__kvm_tlb_flush_vmid`") and left open "whether it's a real EL2 bug or EL2 being strict — the kernel owner's call". It is neither. The root cause is **host-side**, EL2 is behaving exactly as designed, and the consequence is worse than a spurious warning: **the TLB flush silently does not happen.**

## Root cause: a missing `is_protected_kvm_enabled()` branch (VERIFIED — full read of both functions)

Two sibling functions request the same operation. One is pKVM-aware; the other is not.

```c
/* arch/arm64/kvm/mmu.c:179 — CORRECT: has the protected branch */
int kvm_arch_flush_remote_tlbs(struct kvm *kvm)
{
	if (is_protected_kvm_enabled())
		kvm_call_hyp_nvhe(__pkvm_tlb_flush_vmid, kvm->arch.pkvm.handle);  /* id #28 */
	else
		kvm_call_hyp(__kvm_tlb_flush_vmid, &kvm->arch.mmu);               /* id #11 */
	return 0;
}
```

```c
/* arch/arm64/kvm/hyp/pgtable.c:635 — BUG: no protected branch at all */
void kvm_tlb_flush_vmid_range(struct kvm_s2_mmu *mmu, phys_addr_t addr, size_t size)
{
	unsigned long pages, inval_pages;

	if (!system_supports_tlb_range()) {
		kvm_call_hyp(__kvm_tlb_flush_vmid, mmu);            /* :639  id #11 */
		return;
	}

	pages = size >> PAGE_SHIFT;
	while (pages > 0) {
		inval_pages = min(pages, MAX_TLBI_RANGE_PAGES);
		kvm_call_hyp(__kvm_tlb_flush_vmid_range, mmu, addr, inval_pages);  /* :646  id #12 */
		addr += inval_pages << PAGE_SHIFT;
		pages -= inval_pages;
	}
}
```

Ids #11 and #12 are both in the block pKVM **closes** after finalisation (next section), so under pKVM every call from this function is rejected with `SMCCC_RET_NOT_SUPPORTED`. `kvm_call_hyp_nvhe()` then trips `WARN_ON(res.a0 != SMCCC_RET_SUCCESS)` (`asm/kvm_host.h:1124`) — that is the warning we see.

This board reports `:639` every time because it lacks `system_supports_tlb_range()`. On hardware with TLB-range support the `:646` branch would fail identically.

## Why pKVM rejects every hypercall with id < 20

Deliberate, and documented in the source. `asm/kvm_asm.h` splits the hypercall enum into two explicitly-commented blocks:

```c
enum __kvm_host_smccc_func {
	/* Hypercalls available only prior to pKVM finalisation */
	__KVM_HOST_SMCCC_FUNC___kvm_get_mdcr_el2 = __KVM_HOST_SMCCC_FUNC___kvm_hyp_init + 1,  /* 1  */
	__KVM_HOST_SMCCC_FUNC___pkvm_init,                                                    /* 2  */
	__KVM_HOST_SMCCC_FUNC___pkvm_create_private_mapping,                                  /* 3  */
	__KVM_HOST_SMCCC_FUNC___pkvm_cpu_set_vector,                                          /* 4  */
	...
	__KVM_HOST_SMCCC_FUNC___kvm_tlb_flush_vmid,                                           /* 11 */
	__KVM_HOST_SMCCC_FUNC___kvm_tlb_flush_vmid_range,                                     /* 12 */
	...
	__KVM_HOST_SMCCC_FUNC___pkvm_prot_finalize,                                           /* 20 */

	/* Hypercalls available after pKVM finalisation */
	__KVM_HOST_SMCCC_FUNC___pkvm_host_share_hyp,                                          /* 21 */
	...
```

EL2 enforces the split in `hyp_main.rs:1624-1663`:

```rust
let mut hcall_min = 0u64;
if kvm_protected_mode_initialized {
    hcall_min = __KVM_HOST_SMCCC_FUNC___pkvm_prot_finalize;   /* 20 */
}
...
if unlikely(id < hcall_min || (id as usize) >= HOST_HCALL.len()) {
    self.regs.regs[0] = SMCCC_RET_NOT_SUPPORTED;
    return;                                                   /* handler never runs */
}
```

The key flips once and permanently at `arch/arm64/kvm/pkvm.c:487` — `static_branch_enable(&kvm_protected_mode_initialized)` — with the comment *"Flip the static key upfront as that may no longer be possible once the host stage 2 is installed."*

**The rationale is the pKVM threat model.** Before finalisation the host is still *constructing* the hypervisor and is necessarily trusted. After finalisation the host is explicitly untrusted — that is the entire point of protected KVM. Every hypercall that lets the host reach into EL2-managed state must therefore be closed. What is in the closed block makes this obvious:

| id | hypercall | what it would hand an untrusted host |
|---|---|---|
| 2 | `__pkvm_init` | re-initialise the hypervisor |
| 3 | `__pkvm_create_private_mapping` | create arbitrary EL2 mappings |
| 4 | `__pkvm_cpu_set_vector` | replace the EL2 exception vector |
| 14-18 | `__pkvm_*_module*`, `__pkvm_register_hcall` | load code into EL2 |
| 19 | `__pkvm_iommu_init` | reconfigure DMA protection |
| **8-13** | the raw TLB/cache ops | **make EL2 dereference a host-supplied pointer** |

The last row is the one that matters here, and the signatures make it concrete.

```c
/* CLOSED block — takes a RAW HOST POINTER */
extern void __kvm_tlb_flush_vmid(struct kvm_s2_mmu *mmu);              /* kvm_asm.h:310 */
extern void __kvm_tlb_flush_vmid_range(struct kvm_s2_mmu *mmu, ...);   /* kvm_asm.h:308 */
```

```c
/* OPEN block — takes an OPAQUE HANDLE that EL2 validates against its own table */
static void handle___pkvm_tlb_flush_vmid(struct kvm_cpu_context *host_ctxt)  /* hyp-main.c:1192 */
{
	DECLARE_REG(pkvm_handle_t, handle, host_ctxt, 1);
	struct pkvm_hyp_vm *vm;

	if (!is_protected_kvm_enabled())
		return;
	vm = pkvm_get_hyp_vm(handle);      /* EL2 resolves the handle ITSELF */
	if (!vm)
		return;
```

A post-finalisation `__kvm_tlb_flush_vmid(attacker_pointer)` would have EL2 treat host-controlled memory as a `struct kvm_s2_mmu` — a direct hypervisor-compromise primitive. Closing ids 1-19 and replacing them with handle-based equivalents is exactly right.

**The gate is not the bug. Calling through it is.**

## Consequence: dirty-page tracking silently loses writes

`kvm_tlb_flush_vmid_range()` is reached from `kvm_mmu_wp_memory_region()` (`mmu.c:1355-1366`), immediately after the write-protect pass — the **dirty-logging** path:

```c
	write_lock(&kvm->mmu_lock);
	stage2_wp_range(&kvm->arch.mmu, start, end);
	write_unlock(&kvm->mmu_lock);
	kvm_flush_remote_tlbs_memslot(kvm, memslot);   /* -> kvm_tlb_flush_vmid_range() */
```

So under pKVM, enabling `KVM_MEM_LOG_DIRTY_PAGES` on a memslot write-protects the stage-2 tables **but never invalidates the stale TLB entries**. A vCPU holding a cached writable stage-2 translation keeps writing through it; those writes are not trapped and never appear in the dirty bitmap.

**Impact:** silent loss of dirty-page tracking — live migration, snapshotting, or any dirty-log consumer can miss writes and produce a corrupt result. The WARN is the only outward sign, and it is easy to dismiss as noise. This project initially did exactly that and suppressed it.

## Suggested fix

Give `kvm_tlb_flush_vmid_range()` the branch its sibling already has:

```c
	if (is_protected_kvm_enabled()) {
		kvm_call_hyp_nvhe(__pkvm_tlb_flush_vmid, <handle>);
		return;
	}
```

Two complications the owner should know about:

1. `kvm_tlb_flush_vmid_range()` receives a `struct kvm_s2_mmu *`, not a `struct kvm *`, so the pKVM handle is not directly to hand. Either resolve it from the mmu or route range flushes through a handle-aware caller.
2. There is **no range-precise handle-based primitive** in the open block — only the coarse whole-VMID `__pkvm_tlb_flush_vmid(handle)` (#28). The fix is therefore correct but over-invalidates, unless a `__pkvm_tlb_flush_vmid_range(handle, addr, pages)` is added.

## Status and provenance

- **Symptom:** verified on hardware; 15,528 occurrences in one window.
- **Root cause:** verified by full reads of `pgtable.c:635-651`, `mmu.c:179-185`, `mmu.c:1355-1366`, `kvm_asm.h:300-320`, `hyp_main.rs:1619-1670`, `hyp-main.c:1192-1203`, `pkvm.c:480-490`.
- **Dirty-tracking loss:** a **DERIVED** consequence of the verified reject path. Not separately demonstrated — no test was run showing a missed dirty page. Confirming it would need a dirty-log consumer and a guest writing through a stale mapping.
- **Not new to this effort:** an earlier occurrence of the same WARN title exists from **2026-07-21** in `/home/jose/syzkaller/workdir/crashes/`. First *banked* 2026-07-29.
- Not checked against upstream syzkaller dashboards or the ACK tree's own history.

## How it was found (unchanged, and still the point)

This was the first hardware evidence that enabling the ordinary (non-protected) KVM target — which the campaign had left disabled — reaches EL2 paths the protected composite never drove. Stage-A run: `drains=11987`, `el2_hits_max=10113`, `LOST_IN_RING=0`. It confirmed the earlier "composite lever exhausted" conclusion was wrong.

## Corrects the HVC census

Handlers with id 1-19 are **structurally unreachable at runtime** — EL2 rejects them before dispatch, so coverage for them can never exist. That is init/finalize (#1-5), vgic (#6-7), tlb/vmid/cache (#8-13), module-load (#14-18) and `__pkvm_iommu_init` (#19), plus `__pkvm_prot_finalize` (#20) which runs once at boot before the ring is armed.

**20 of the 46 never-fired handlers are closed by design.** The genuinely addressable gap is roughly **26**, not 46, and no amount of input expansion will change that. Future census output should classify these separately rather than presenting them as a coverage gap.

It also settles a separate open question: the TLB handlers being cold is **not** evidence that the `LOST_IN_KCOV_AREA` truncation is eating unique coverage. They are cold because they never execute.
