# Root-cause analysis — the `__kvm_mmu_topup_memory_cache` Oops and the OOM

Worked from `report0`, `report1`, `log0`, the serial capture, and the archived
`vmlinux`. Split into **PROVEN** (disassembly / full code read / captured output)
and **HYPOTHESIS** (mechanism inferred, not yet confirmed), per the project rule
that code reading yields a hypothesis and never a conclusion.

---

## PROVEN 1 — the faulting pointer is `mc->kmem_cache`, and its value is 3

The report gives `pc : kmem_cache_alloc+0x50` and
`Code: ... (b9401e76)` = `ldr w22, [x19, #28]`, with `x19 = 3` and fault address
`0x1f` (= 3 + 28). `report1` has `x19 = 2` and fault address `0x1e`.

Disassembling the archived `vmlinux` (`sha256 7df1b262…`) settles what `x19`
holds, rather than assuming:

```
ffff8000805e3e18 <kmem_cache_alloc>:
ffff8000805e3e30:   aa0003f3    mov  x19, x0        <-- x19 = arg1 = struct kmem_cache *s
ffff8000805e3e68:   b9401e76    ldr  w22, [x19,#28] <-- faults; this is +0x50
```

`kmem_cache_alloc + 0x50 = 0xffff8000805e3e68` exactly. So **`x19` is the
`struct kmem_cache *` argument**, and the caller
`mmu_memory_cache_alloc_obj()` (`kvm_main.c:409`) passes `mc->kmem_cache`:

```c
if (mc->kmem_cache)
        return kmem_cache_alloc(mc->kmem_cache, gfp_flags);
else
        return (void *)__get_free_page(gfp_flags);
```

Therefore **`vcpu->arch.mmu_page_cache.kmem_cache == 3`** (and `2` in the other
run). The `if (mc->kmem_cache)` test passes because 3 is non-zero, and the
garbage pointer goes straight to the allocator.

## PROVEN 2 — arm64 never writes that field, so this is memory corruption

Full grep + read of every `mmu_page_cache` reference in `arch/arm64/kvm/`:

```
mmu.c:1952   struct kvm_mmu_memory_cache *memcache = &vcpu->arch.mmu_page_cache;
arm.c:474    vcpu->arch.mmu_page_cache.gfp_zero = __GFP_ZERO;
arm.c:514    kvm_mmu_free_memory_cache(&vcpu->arch.mmu_page_cache);
```

arm64 sets **only** `gfp_zero`. `kmem_cache` is never assigned anywhere in the
arm64 tree, so it must remain `NULL` for the vCPU's entire lifetime and
`mmu_memory_cache_alloc_obj()` must always take the `__get_free_page()` branch.

A value of 2 or 3 is not a logic error in KVM. It is **`struct kvm_vcpu` being
overwritten by something else.**

## PROVEN 3 — the second Oops is a consequence of the first, not a separate bug

```
kernel BUG at arch/arm64/kvm/fpsimd.c:54!   ->  BUG_ON(!current->mm)
```

with the trace:

```
__mmput -> exit_mmap -> mmu_notifier_release -> kvm_mmu_notifier_release
  -> kvm_flush_shadow_all -> kvm_arch_flush_shadow_all (mmu.c:2660)
    -> unmap_stage2_range -> __unmap_stage2_range -> stage2_apply_range (mmu.c:86)
      -> cond_resched_rwlock_write -> __schedule -> finish_task_switch
        -> fire_sched_in_preempt_notifiers -> kvm_sched_in
          -> kvm_arch_vcpu_load -> kvm_arch_vcpu_load_fp -> BUG_ON(!current->mm)
```

The task Oopsed (PROVEN 1) *inside* `KVM_RUN`, so `vcpu_put()` never ran and the
KVM preempt notifier stayed registered. The task then died; during its `mm`
teardown, `stage2_apply_range()` voluntarily reschedules
(`cond_resched_rwlock_write`, `mmu.c:86`), the notifier fires on schedule-in, and
`kvm_arch_vcpu_load_fp()` asserts on a `current->mm` that `exit_mmap()` has
already torn down.

**Our deadlock fix is not implicated.** `pkvm_flush_unaccount(kvm)` sits *after*
`write_unlock()` in `kvm_arch_flush_shadow_all()`, while the BUG fires inside
`unmap_stage2_range()` above it — control never reaches our line.

Note this is nonetheless a genuine latent bug in its own right: **any** task that
dies with a loaded vCPU can hit that `BUG_ON`. It just needs a first crash to get
there, so it is a severity amplifier rather than an independent finding.

## PROVEN 4 — 39 TLB-flush failures immediately precede the corruption

`log0` contains exactly two distinct WARN sites before the Oops:

```
39 x  arch/arm64/kvm/hyp/pgtable.c:639  kvm_tlb_flush_vmid_range
 1 x  kernel/context_tracking.c:128     ct_kernel_exit
```

`pgtable.c:639` is `kvm_call_hyp(__kvm_tlb_flush_vmid, mmu);` (read directly).
The WARN is `WARN_ON(res.a0 != SMCCC_RET_SUCCESS)` in `kvm_call_hyp_nvhe`
(`kvm_host.h:1131`) — i.e. **EL2 reported that the TLB invalidation did not
succeed**, 39 times, in the seconds before host memory was found corrupted.

---

## HYPOTHESIS — stale stage-2 TLB entries let the guest write host memory

Not confirmed. Stated because it is the only mechanism found that explains every
observation at once, and because it is cheaply testable.

If `__kvm_tlb_flush_vmid` genuinely fails to invalidate, the guest keeps a valid
TLB entry for an IPA whose stage-2 mapping the host has since torn down and whose
backing page the host has reused for its own allocations. Guest writes then land
in whatever the host put there — including slab objects such as `struct kvm_vcpu`.

What it explains that other candidates do not:

| observation | fits? |
|---|---|
| corrupted field holds a *small integer* (2, 3) | yes — guest-written data, not a wild kernel pointer |
| value differs between runs (3 vs 2) | yes — non-deterministic, depends on what the guest wrote |
| **KASAN is enabled (`CONFIG_KASAN=y`) yet silent** | yes — KASAN instruments compiler-generated CPU accesses; it cannot see writes the *guest* performs through a stale stage-2 mapping. A software UAF would have been reported. |
| the read of `mc->kmem_cache` did not itself fault | yes — the vCPU allocation is live; only its *contents* are wrong |
| 39 TLB failures immediately prior | yes — direct precondition |
| D3000's 23.9 GB unaccounted (`FINDING.md` §2) | plausibly — pages never truly released from stage-2 are never returned to the host |
| N90 healthy with 1,328 TLB warnings | consistent — corruption depends on *which* reused page the guest writes, so it is probabilistic, not a function of warning count |

### Candidates considered and ruled out

- **Stale objects / struct layout mismatch after our `kvm_host.h` edit.** Ruled
  out: `kvm_host.h` mtime `07-29 16:03`; `mmu.o` `16:15`; `arm.o`, `fpsimd.o`,
  `kvm_main.o`, `vmlinux` all `18:12`. Every object postdates the header.
- **Stale Rust EL2 bindings desynced from the changed C header.** Ruled out:
  `bindings_generated.rs` is bindgen-generated at build time (`include!(concat!(
  env!("OUT_DIR"), "/bindings_generated.rs"))`) and was regenerated at `18:12`.
- **Our KCOV HVC wrapper corrupting state.** Weak. `KVM_PKVM_COV_HVC`
  (`kvm_host.h:1112`) passes `&__cov` and writes the HVC result straight into
  `_res`; `kcov_add_pcs()` (`kcov.c:238`) is bounds-checked against
  `t->kcov_size`. Not excluded, but nothing in the code read supports it.

### Experiments that would settle it

1. **Read the EL2 `__kvm_tlb_flush_vmid` handler** and determine why it returns
   non-success. That is the head of the chain and is unexamined so far.
2. **Correlate**: instrument the WARN to log VMID + IPA range, and check whether
   corrupted addresses fall in ranges that recently failed invalidation.
3. **Discriminate cheaply**: run the campaign with `KVM_ARM_VCPU_INIT` still
   enabled on a kernel where the TLB path is forced down the
   `system_supports_tlb_range() == false` branch. If corruption stops while the
   fuzzing surface is unchanged, the link is real.
4. If EL2 is genuinely failing to invalidate, this is a **security-relevant**
   guest→host memory-corruption primitive, not merely a stability bug, and
   should be treated accordingly.

---

## The OOM, re-examined

`FINDING.md` §2 records the measurements. Two things sharpen here.

The zone accounting at first OOM is internally contradictory in a telling way:

```
mapped : 8.6 GB      anon : 125 MB      file : 1 MB
```

`Mapped` counts pages present in userspace page tables. With anon and file both
negligible, 8.6 GB of *mapped* pages means page-table mappings persist for pages
that are no longer on any LRU — mappings outliving the pages' accounted
lifetime. That is the same shape as PROVEN 4 / the hypothesis above: mappings
that were supposed to be torn down and invalidated, but were not.

N90 under identical workload shows the normal profile
(`Mapped 753 MB`, `AnonPages 10.5 GB`) — the inversion is specific to the failure.

So the OOM and the Oops may be **two symptoms of one defect** (stage-2 unmap /
invalidation not completing) rather than two findings. That is currently a
hypothesis, and experiment 1 above is the shared discriminator for both.
