# Root-cause analysis — the `__kvm_mmu_topup_memory_cache` Oops and the OOM

> **SUPERSEDED — read `ROOT-CAUSE-CONFIRMED.md` for the conclusion.**
>
> This file is the working analysis. It reached the union type-confusion answer,
> but `ROOT-CAUSE-CONFIRMED.md` proves it quantitatively from the register dumps
> and goes further on several points. Where the two differ, that file wins.
>
> Note it says this document was "kept unmodified for the record"; that is no
> longer accurate — it was revised independently at roughly the same time, which
> is why both now carry the union finding. The convergence was arrived at
> separately and is corroboration, not duplication.
>
> Its most important correction to what is written below: the TLB-flush WARNs are
> a **sibling symptom, not the head of the chain**. Both the WARN storm and the
> Oops are reached only through dirty logging, which is why they co-occur.

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

## PROVEN 2 — it is NOT corruption. It is union type confusion.

> **Correction.** An earlier revision of this document concluded that
> `struct kvm_vcpu` was "being overwritten by something else" and proposed that
> the guest was writing host memory through stale stage-2 TLB entries. **That was
> wrong.** It came from grepping `arch/arm64/kvm/*.c` for writers and finding
> none, without reading the declaration of the field itself. Reading it settles
> the question immediately, and the real answer needs no corruption at all.

`mmu_page_cache` is declared inside an **anonymous union**
(`arch/arm64/include/asm/kvm_host.h:679`):

```c
union {
        /* Cache some mmu pages needed inside spinlock regions */
        struct kvm_mmu_memory_cache mmu_page_cache;
        /* Pages to be donated to pkvm/EL2 if it runs out */
        struct kvm_hyp_memcache stage2_mc;
};
```

The two members alias exactly at the faulting offset:

| offset | `struct kvm_mmu_memory_cache` | `struct kvm_hyp_memcache` |
|---|---|---|
| +0 | `gfp_t gfp_zero` | `phys_addr_t head` |
| +4 | `gfp_t gfp_custom` | ″ |
| **+8** | **`struct kmem_cache *kmem_cache`** | **`unsigned long nr_pages`** |
| +16 | `int capacity` | `unsigned long flags` |
| +20 | `int nobjs` | ″ |
| +24 | `void **objects` | — |

So `mc->kmem_cache == 3` **is** `stage2_mc.nr_pages == 3` — a count of pages
queued for donation to EL2. `report1`'s `2` is likewise two pages.

This explains every observation without invoking corruption: the values are small
integers because they are page counts; they differ between runs because the count
differs; and **KASAN is silent because nothing illegal happened** — both writes
are legitimate accesses to their own union member.

## PROVEN 3 — one vCPU reaches both union members on a pKVM host

`kvm_handle_guest_abort()` (`mmu.c:2322`) dispatches on fault *type*, not on VM
type:

```c
if (is_protected_kvm_enabled() && fault_status != ESR_ELx_FSC_PERM)
        ret = pkvm_mem_abort(vcpu, &fault_ipa, memslot, hva, NULL);
else
        ret = user_mem_abort(vcpu, fault_ipa, memslot, hva, fault_status);
```

On a pKVM host, for the *same* vCPU:

- a **non-permission** fault takes `pkvm_mem_abort()`. EL2 answers with a memory
  request, and `handle_hyp_req_mem()` (`handle_exit.c:345`) runs
  `topup_hyp_memcache(&vcpu->arch.stage2_mc, ...)`, writing `nr_pages` at +8.
- a **permission** fault falls through to `user_mem_abort()`, which calls
  `kvm_mmu_topup_memory_cache(&vcpu->arch.mmu_page_cache, ...)` and reads +8 as
  `kmem_cache`.

`mmu_memory_cache_alloc_obj()` then tests `if (mc->kmem_cache)`, sees a non-zero
page count, and hands it to `kmem_cache_alloc()` as a `struct kmem_cache *`.

The union is only sound if a vCPU uses exactly one member for its whole life.
That invariant holds for a *protected* VM (always `pkvm_mem_abort`) and for a
*non-pKVM host* (always `user_mem_abort`). It is violated by an **ordinary VM on
a pKVM host**, which is precisely what the reproducer creates —
`ioctl$KVM_CREATE_VM(r0, 0xae01, 0x0)`, type 0, no bit 31.

Note `user_mem_abort()` only reaches the topup when
`fault_status != ESR_ELx_FSC_PERM || (logging_active && write_fault)`. Since the
`else` branch is taken only for permission faults, the surviving trigger is
`logging_active && write_fault` — dirty logging on the memslot plus a write
permission fault. Confirming that the reproducer satisfies that (via
`syz_kvm_setup_cpu$arm64`'s memslot setup) is the one step not yet done.

## PROVEN 4 — the second Oops is a consequence of the first, not a separate bug

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

## PROVEN 5 — 39 TLB-flush failures also occurred in the same window

`log0` contains exactly two distinct WARN sites before the Oops:

```
39 x  arch/arm64/kvm/hyp/pgtable.c:639  kvm_tlb_flush_vmid_range
 1 x  kernel/context_tracking.c:128     ct_kernel_exit
```

`pgtable.c:639` is `kvm_call_hyp(__kvm_tlb_flush_vmid, mmu);` (read directly).
The WARN is `WARN_ON(res.a0 != SMCCC_RET_SUCCESS)` in `kvm_call_hyp_nvhe`
(`kvm_host.h:1131`) — i.e. **EL2 reported that the TLB invalidation did not
succeed**, 39 times, in the same window as the Oops.

---

## RETRACTED — "stale stage-2 TLB entries let the guest write host memory"

An earlier revision proposed this as the leading hypothesis, resting largely on
KASAN's silence. **It is withdrawn.** PROVEN 2/3 explain the Oops completely with
no corruption, and they explain KASAN's silence better: KASAN said nothing
because nothing illegal happened.

The TLB-flush WARNs (PROVEN 5) are real and remain an open defect in their own
right — finding (3) — but their proximity to this Oops was **coincidence**. The
fuzzer produces them continuously: N90 logged 1,328 over 14 hours with no
corruption at all. Treating "39 of them immediately preceded the crash" as
evidence of causation was a post-hoc error, and N90 was already the counter-example
sitting in the same dataset.

Keeping this retraction visible because the discarded theory was the more
alarming one — it would have implied a guest→host memory-corruption primitive.
It does not exist. Nothing here is evidence of a guest escaping stage-2.

## The OOM, re-examined

`FINDING.md` §2 records the measurements. Two things sharpen here.

The zone accounting at first OOM is internally contradictory in a telling way:

```
mapped : 8.6 GB      anon : 125 MB      file : 1 MB
```

`Mapped` counts pages present in userspace page tables. With anon and file both
negligible, 8.6 GB of *mapped* pages means page-table mappings persist for pages
that are no longer on any LRU — mappings outliving the pages' accounted lifetime.

N90 under identical workload shows the normal profile
(`Mapped 753 MB`, `AnonPages 10.5 GB`) — the inversion is specific to the failure.

**The OOM is a separate, still-unexplained defect.** An earlier revision
suggested it and the Oops might be two symptoms of one root cause; with PROVEN
2/3 that link is gone — the Oops is union type confusion and has nothing to do
with page lifetime. Do not carry the merged theory forward.

The most promising thread is `handle_hyp_req_mem()` (`handle_exit.c:345`), which
is now known to run for ordinary VMs on a pKVM host. It donates pages to EL2 and
accounts them into `kvm->stat.protected_hyp_mem`, and `arm.c:510` subtracts
`stage2_mc.nr_pages` and calls `free_hyp_memcache()` only on vCPU destroy. Pages
donated to EL2 leave host accounting exactly as observed, so the questions worth
asking are whether every donated page is reclaimed when an *ordinary* VM on a
pKVM host tears down, and whether that path is reached at all when the owning
task is killed rather than closing its fds. Unverified — no evidence gathered yet.

Both boards now carry `mem-watch.sh` sampling, whose `unaccounted_mb` column is
the direct measure; a recurrence gives the growth curve instead of an end state.
