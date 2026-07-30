# CONFIRMED root cause — union type confusion between `mmu_page_cache` and `stage2_mc`

Written 2026-07-30 as a **correction and completion** of `ROOT-CAUSE-ANALYSIS.md`. That document's PROVEN 1–4 stand. Its central **HYPOTHESIS ("stale stage-2 TLB entries let the guest write host memory") is now positively REFUTED**, and the real defect is proven from the register dumps alone, quantitatively, on both crashes. Nothing here rests on reasoning-from-code-shape: every number in the two Oops register dumps is derived exactly.

`ROOT-CAUSE-ANALYSIS.md` is kept unmodified for the record; read this file for the conclusion.

---

## The defect in one sentence

`vcpu->arch.mmu_page_cache` and `vcpu->arch.stage2_mc` are **the same 32 bytes** (a union), and on a `kvm-arm.mode=protected` host a **non-protected VM with a `KVM_MEM_LOG_DIRTY_PAGES` memslot** drives *both* views of that storage: pKVM's `pkvm_mem_abort()` fills it as a `kvm_hyp_memcache`, then the legacy `user_mem_abort()` permission-fault path reads it back as a `kvm_mmu_memory_cache` — so **`nr_pages` is used as `mc->kmem_cache`** and handed to `kmem_cache_alloc()` as a pointer.

```c
/* arch/arm64/include/asm/kvm_host.h:678 */
union {
        /* Cache some mmu pages needed inside spinlock regions */
        struct kvm_mmu_memory_cache mmu_page_cache;
        /* Pages to be donated to pkvm/EL2 if it runs out */
        struct kvm_hyp_memcache stage2_mc;
};
```

| byte range | as `mmu_page_cache` | as `stage2_mc` |
|---|---|---|
| 0–3   | `gfp_zero`   | `head[31:0]`  |
| 4–7   | `gfp_custom` | `head[63:32]` |
| **8–15**  | **`kmem_cache`** | **`nr_pages`** |
| 16–19 | `capacity`   | `flags[31:0]` |
| 20–23 | `nobjs`      | `flags[63:32]` |
| 24–31 | `objects`    | *(past end of the 24-byte hyp memcache)* |

Layout verified with `pahole` against the archived `vmlinux` (`sha256 7df1b262…`): `kvm_mmu_memory_cache` = 32 B, `kvm_hyp_memcache` = 24 B (`head`, `nr_pages`, `flags`), union at `kvm_vcpu_arch` +8952, i.e. `struct kvm_vcpu` +9208. The aliased field pair sits at `kvm_vcpu` +9216.

## PROVEN — every register decodes exactly, in both crashes

`kmem_cache_alloc+0x50` disassembled from the archived `vmlinux`:

```
ffff8000805e3e30:  aa0003f3  mov x19, x0        <- x19 = arg1 = struct kmem_cache *s
ffff8000805e3e34:  d5384100  mrs x0, sp_el0     <- x0  = current  (NOT the vcpu)
ffff8000805e3e3c:  2a0103f4  mov w20, w1        <- w20 = arg2 = gfp_flags
ffff8000805e3e68:  b9401e76  ldr w22, [x19,#28] <- faults: s->object_size
```

The reader is `__kvm_mmu_topup_memory_cache()` (`virt/kvm/kvm_main.c:414-444`):

```c
gfp_t gfp = mc->gfp_custom ? mc->gfp_custom : GFP_KERNEL_ACCOUNT;   /* :416 */
...
gfp_flags |= mc->gfp_zero;                                          /* :405 */
if (mc->kmem_cache)
        return kmem_cache_alloc(mc->kmem_cache, gfp_flags);         /* :408 */
```

So the two arguments are `mc->kmem_cache` and `mc->gfp_custom | mc->gfp_zero`, i.e. under the union: **`nr_pages`** and **`head[63:32] | head[31:0]`**. `push_hyp_memcache()` (`kvm_host.h:92-101`) stores `head = (pa & PAGE_MASK) | FIELD_PREP(~PAGE_MASK, order)`, so for order-0 donations `head[31:0]` is page-aligned and `head[63:32]` is a small PA-high word.

| | report0 | report1 |
|---|---|---|
| fault address | `0x1f` (= 3 + 28) | `0x1e` (= 2 + 28) |
| x19 = `kmem_cache` = **`nr_pages`** | **3** | **2** |
| x22 = `gfp_custom` = **`head[63:32]`** | **0x22** | **0x21** |
| x20 = gfp = `gfp_custom \| gfp_zero` | **0x2cc28022** | **0xf350d021** |
| ⇒ reconstructed `head` | `0x22_2cc28000` | `0x21_f350d000` |
| page-aligned? | yes | yes |

Check: `0x22 | 0x2cc28000 = 0x2cc28022` ✔ and `0x21 | 0xf350d000 = 0xf350d021` ✔. Both reconstructed `head` values are page-aligned physical addresses at ~136 GiB, the same region as the `pgdp=0x2265e88000` printed by the same Oops. A random garbage value cannot have that structure twice.

> **Independently verified 2026-07-30**, with one refinement to how the strength of this argument should be stated. Taken literally the `|` check is weak: `gfp_zero` is *defined* here as `x20 & ~x22`, so `x22 | (x20 & ~x22) == x20` holds for any `x22` whose bits are a subset of `x20`'s. The non-trivial content is the **page-alignment**, and what that requires: for `head[31:0]` to have zero low 12 bits, `x22` must equal `x20 & 0xfff` exactly. It does, in both crashes — `report0` `0x022`/`0x022`, `report1` `0x021`/`0x021`. Two independent 12-bit agreements is ~1-in-16-million against chance, and it is exactly what the union predicts, since `gfp_zero` is page-aligned and therefore contributes nothing to the low 12 bits of `gfp_flags`. Register values re-read from the reports: `report0 x19=3 x20=0x2cc28022 x22=0x22`; `report1 x19=2 x20=0xf350d021 x22=0x21`. All confirmed.

`nr_pages` ∈ {2,3} is likewise exactly what `topup_hyp_memcache(mc, kvm_mmu_cache_min_pages(kvm), 0)` leaves behind — `kvm_mmu_cache_min_pages(kvm) = kvm_stage2_levels(kvm) - 1` (`asm/stage2_pgtable.h:31`), i.e. 2 or 3.

One more corroboration: `flags = HYP_MEMCACHE_ACCOUNT_KMEMCG | HYP_MEMCACHE_ACCOUNT_STAGE2` = `BIT(1)|BIT(2)` = 6 (`kvm_host.h:152-153,163-167`), whose high half is 0, so `mc->nobjs == 0` and the `if (mc->nobjs >= min) return 0;` early-out at `kvm_main.c:419` does **not** fire — the top-up proceeds straight into the bad `kmem_cache_alloc()`.

**There is no memory corruption.** Both writer and reader are legitimate, correctly-typed accesses to a live `struct kvm_vcpu`. That is why KASAN (`CONFIG_KASAN=y`, shadow base `0xdfff800000000000` visible in x4) stayed silent — it has nothing to report. The elaborate "EL2/guest writes are invisible to KASAN" argument in `ROOT-CAUSE-ANALYSIS.md` is unnecessary.

## The full causal chain (all links verified in code)

1. Boot: `kvm-arm.mode=protected` (verified in the `n90-dmesg-history.log.gz` cmdline) ⇒ `is_protected_kvm_enabled()` = true, `finalize_pkvm` ran.
2. `syz_kvm_setup_cpu$arm64` registers gpa `0xdddd1000`, 2 pages, with **`KVM_MEM_LOG_DIRTY_PAGES`** — unconditionally, inside the pseudo-syscall (`syzkaller/executor/common_kvm_arm64.h:104,117`). So `memslot_is_logging()` is true for that slot in *every* syzkaller KVM program, and the 5-call reproducer needs no explicit dirty-log ioctl. The reproducer's `@memwrite …0xdddd1000` writes straight into it.
3. First touch = **translation** fault → `kvm_handle_guest_abort()` takes the pKVM branch (`mmu.c:2322`, predicate `is_protected_kvm_enabled() && fault_status != ESR_ELx_FSC_PERM`) → `pkvm_mem_abort()` → `topup_hyp_memcache(&vcpu->arch.stage2_mc, …)` (`mmu.c:1766`). The union now holds a live hyp memcache: `head` = donated page PA, `nr_pages` = 2–3.
4. Registering that slot write-protects it: `KVM_SET_USER_MEMORY_REGION` → `kvm_arch_commit_memory_region` → `kvm_mmu_wp_memory_region` (`mmu.c:1404-1420`) → `stage2_wp_range` → `pkvm_wp_range` (`mmu.c:1354`) → `__pkvm_wrprotect` at EL2, **which does no TLBI**; then `kvm_flush_remote_tlbs_memslot` → `kvm_arch_flush_remote_tlbs_range()` (`mmu.c:188`) — **never made pKVM-aware** — issues HVC id 11 `__kvm_tlb_flush_vmid`. The EL2 dispatcher rejects it because post-finalization `hcall_min` is raised to id 20 (`hyp/nvhe/rust/src/hyp_main.rs:1627-1663`), returning `SMCCC_RET_NOT_SUPPORTED` (-1) → `WARN_ON` at `pgtable.c:639`. **This is finding (3) / "BUG2", and the TLB is genuinely never invalidated.**
5. Guest writes again → **permission** fault → `mmu.c:2322`'s predicate is now false → the `else` at `mmu.c:2325` → **`user_mem_abort()`**, whose `memcache` is `&vcpu->arch.mmu_page_cache` (`mmu.c:1952`).
6. `mmu.c:1979-1981`: `fault_status == PERM`, so the top-up runs only because `logging_active && write_fault` — both true here → `kvm_mmu_topup_memory_cache(memcache, …)`.
7. `mc->kmem_cache` = `nr_pages` = 3 ⇒ `if (mc->kmem_cache)` passes ⇒ `kmem_cache_alloc((struct kmem_cache *)3, 0x2cc28022)` ⇒ `ldr w22,[x19,#28]` ⇒ **Oops at 0x1f**.

The second Oops (`fpsimd.c:54 BUG_ON(!current->mm)`) is exactly as `ROOT-CAUSE-ANALYSIS.md` PROVEN 3 describes: a consequence, not a separate bug.

## The TLB WARN is a SIBLING symptom, not the cause

`ROOT-CAUSE-ANALYSIS.md` treats the 39 `pgtable.c:639` WARNs as "the head of the chain". They are not. **Both symptoms are reached only through dirty logging**, which is why they co-occur: step 4 (the WARN) is on the write-protect-on-enable path taken by `KVM_SET_USER_MEMORY_REGION`, and step 6 (the Oops) needs `logging_active && write_fault`.

### `res.a0 == -1` is visible in every WARN dump — the EL2 rejection is now empirical

`x21` holds `res.a0`. Verified by disassembling the archived `vmlinux` at `kvm_tlb_flush_vmid_range`:

```
+0x0ac:  bl      pkvm_cov_begin
+0x0b4:  mov     x0, #0xb                  <- hypercall id 11 = __kvm_tlb_flush_vmid
+0x0bc:  movk    x0, #0xc600, lsl #16      <- KVM_HOST_SMCCC_FUNC -> 0xc600000b
+0x0c4:  hvc     #0x0
+0x0c8:  mov     x21, x0                   <- x21 = res.a0
+0x0d4:  cbnz    x21, +0x20c               <- WARN_ON(res.a0 != SMCCC_RET_SUCCESS)
+0x210:  brk     #0x800                    <- the reported pc
```

Every captured WARN carries **`x21 = 0xffffffffffffffff` = -1 = `SMCCC_RET_NOT_SUPPORTED`**. That value is *uniquely* the dispatcher's `id < hcall_min` rejection: `handle_host_hcall` sets `regs[0] = SMCCC_RET_SUCCESS` **before** invoking a handler, and `handle___kvm_tlb_flush_vmid` has no error return at all, so a handler that ran — successfully or not — could never leave -1 in `a0`. The TLBI therefore did not merely fail; **it was never attempted.**

The rest of the dump is constant across every instance, confirming a fixed, data-independent trigger rather than a race: `x22 = &kvm->arch.mmu`, `x23 = 0xdddd1000` (memslot base), `x20 = 0x2000` (8 KiB = 2 pages), `x26 = 0xdddd3000`, `x28 = 0xdddd2fff`.

### Why it fires from the first program and never stops

`syz_kvm_setup_cpu$arm64` registers the `KVM_MEM_LOG_DIRTY_PAGES` slot in **every** syzkaller program (`executor/common_kvm_arm64.h:117` — hardcoded in `setup_vm()`, not a fuzzer choice), the WARN sits on the write-protect-on-enable path, and it is `WARN_ON`, not `WARN_ON_ONCE`. So the rate is **exactly one WARN per fuzzing program**, deterministic, from program #1 — visible in the consecutive comms `syz.1.6552, 6553, 6555, 6556, 6557, 6558, 6559` at ~10/s. Nothing ever complains louder because `kvm_arch_flush_remote_tlbs_range()` returns 0 unconditionally, and the reported pc points at generic-looking TLB code (`pgtable.c:639`) rather than at the ID gate that actually rejected the call — the failure happens inside the `kvm_call_hyp_nvhe` macro, so it is attributed to the call site.

Beyond the correctness hole this is also a *measurement* problem: each WARN emits the full module list plus a ~30-line backtrace (~8-10 KiB), which at 115200 baud saturates the serial console, throttles the fuzzer, and can truncate or interleave genuine reports. Do **not** paper over it with `WARN_ON_ONCE`; fix 2 below removes both the flood and the correctness hole.

The correlation the note flagged as "suggestive, not conclusive" — TLB WARN registers `x23=0xdddd1000, x26=0xdddd3000, x28=0xdddd2fff` matching the reproducer's write target — is now fully explained: that is precisely the 2-page `ARM64_ADDR_DIRTY_PAGES` memslot `[0xdddd1000, 0xdddd3000)` being flushed. It is a shared *trigger*, not a causal link.

This also settles the D3000-vs-N90 puzzle the note could not resolve: N90's TLB WARNs did not leak or corrupt anything because the TLB failure is not the corrupting mechanism.

Consequence of the TLB bug in its own right (still real, still worth fixing): `kvm_arch_flush_remote_tlbs_range()` returns 0 unconditionally, so generic KVM skips its `kvm_flush_remote_tlbs()` fallback — which *would* have worked, since `kvm_arch_flush_remote_tlbs()` (`mmu.c:179`) does use the pKVM-aware `__pkvm_tlb_flush_vmid` (id 28). Stale writable stage-2 TLB entries therefore survive a write-protect, so **writes can be missed by dirty tracking**.

---

## The leak — sharpened, still NOT root-caused

`FINDING.md` §2 stays open. **Do not close it on the strength of the union bug**: a negative control (below) shows neither the Oops nor the TLB WARN drives it.

### Re-measured from the serial log — worse than recorded

At the first `oom-kill:constraint=CONSTRAINT_NONE` (`[37491]`), pages × 4 KiB:

| | value |
|---|---|
| managed (DMA + Normal) | 27,657 MB |
| accounted (free+anon+file+slab+pagetables+sec_pagetables+cma) | 1,718 MB |
| **unaccounted** | **25,939 MB = 25.3 GB (94 %)** |
| `mapped` | **8,840 MB** |
| `active_file + inactive_file` | 26 MB |
| `shmem` | **0** |
| Free / Total swap | 37,015,860 / 39,390,272 kB (untouched) |
| `all_unreclaimable?` | **no** |

`FINDING.md` records 23.9 GB; the correct figure is **25.3 GB**. And `mapped` = 8.6 GB with 26 MB of file pages and `shmem: 0` is an **internally impossible** state: millions of pages are counted in userspace page tables while every LRU counter says they do not exist ⇒ mapped-but-isolated-from-every-LRU. That sub-signature is worth ~8.6 GB on its own and is not explained by "donated to EL2".

### NEGATIVE CONTROL — the leak is not caused by the Oops (or the TLB WARN)

Build `6.6.30-covfix #51`, `n90-dmesg-history.log.gz`: **N90 hit the identical crash twice** — `Internal error: Oops: 0000000096000006` → `kernel BUG at fpsimd.c:54` → `Fixing recursive fault but reboot is needed!` at `[33369]` and `[34182]` — and 30,134 × `pgtable.c:639`. Its live snapshot at `[53401]`, i.e. **5.3 h after the second recursive fault**, is healthy: accounting gap **0.5 GB** of 24.0 GB, `Mapped` 726 MB, `MemAvailable` 11.0 GB, swap untouched.

So: same kernel, same workload, same Oops, same TLB storm, **longer** uptime → no leak. Whatever ran away on D3000 needs a further ingredient that N90 never entered. (An earlier draft of this file argued the aborted `exit_mmap` was the leak; the N90 data refutes that as sufficient, and it is retracted.)

**Evidence-hygiene warning:** `n90-dmesg-history.log.gz` mixes two kernels — 27,766 lines of `6.6.30-covfix #51` and 2,482 of `6.6.30+ #47`. **All 656 `TAINT_BAD_PAGE` ('B') lines belong to #47, not to the build under investigation** (#51 has zero). Do not cite them here.

### Leak candidate A — best code-level explanation, NOT confirmed in any log

`pkvm_mem_abort()`, `arch/arm64/kvm/mmu.c:1860-1868`:

```c
	ppage->page = page;
	ppage->ipa = *fault_ipa;
	ppage->order = get_order(page_size);
	ppage->pins = 1 << ppage->order;
	WARN_ON(insert_ppage(kvm, ppage));     /* :1864 */

	write_unlock(&kvm->mmu_lock);

	return 0;                              /* ← success, even if insert failed */
```

`insert_ppage()` → `mtree_insert_range()` returns `-EEXIST` on **any** overlapping range and `-ENOMEM` under pressure (and it is called with `GFP_KERNEL` while holding `write_lock(&kvm->mmu_lock)`). On failure the page stays **pinned (`FOLL_LONGTERM`) and mapped into the guest at EL2**, but its only tracking record — the `kvm_pinned_page` in `kvm->arch.pkvm.pinned_pages` — is dropped, and the abort **reports success**, so the guest re-faults and leaks again. The overlap pre-check at `mmu.c:1840-1841` only runs for `page_size > PAGE_SIZE`, so the 4 KB fall-back path re-inserts into a range still covered by a surviving order-9 entry. Nothing else can recover the pin: `kvm_unmap_gfn_range()` is a **no-op under pKVM** (`mmu.c:2339-2341`), so munmap / mmu-notifier invalidation never drops pins, and teardown walks only the maple tree.

Why it fits the profile: these are THP-backed shmem/memfd pages (note the `PageSwapBacked` guard at `mmu.c:1799` is gated on `kvm->arch.pkvm.enabled`, so it is **skipped for non-protected VMs**). Once the executor dies and the inode leaves the page cache, an orphaned pin keeps `refcount > 0` forever: not in `NR_FILE_PAGES`, not in `NR_ANON_MAPPED`, on no LRU ⇒ unreclaimable, unswappable, invisible to every counter. Rate is ~1 page per fault with no VM churn required, so 25 GB is reachable in minutes — which also explains why this is a D3000-only runaway rather than a slow drip both boxes would share.

**Not confirmed.** The `WARN_ON` at `mmu.c:1864` would have had to fire millions of times, and it appears in **no** captured log: N90's 30,134 WARNs are 30,134 × `pgtable.c:639` plus 19 × `kvm_emulate.h:563`, 12 × `arch_timer.c:461`, 1 × `context_tracking.c:128`, 1 × `vmid.c:69` — and nothing from `mmu.c`. D3000's serial capture is only a tail excerpt, so its silence proves little. **The discriminator is therefore: recover D3000's full serial log, or re-run with a counter on `mmu.c:1864`.** If that WARN is silent there too, candidate A is dead and the next candidates are the unaccounted `__pkvm_topup_hyp_alloc_mgt()` donation channel (`init_hyp_memcache()` ⇒ `flags = 0` ⇒ no `kvm_account_pgtable_pages`, no `protected_hyp_mem` stat — the one truly untracked host→EL2 path) and failed VM teardown leaving `destroy_hyp_vm_pgt` / `drain_hyp_pool` unrun.

### Leak candidate B — proven, but far too small

`kvm_arch_vcpu_destroy()` (`arm.c:509-515`) frees by the **host-wide** predicate:

```c
if (is_protected_kvm_enabled()) {
        atomic64_sub(vcpu->arch.stage2_mc.nr_pages << PAGE_SHIFT, …);
        free_hyp_memcache(&vcpu->arch.stage2_mc);
} else {
        kvm_mmu_free_memory_cache(&vcpu->arch.mmu_page_cache);
}
```

On this host the first branch always wins, so **`kvm_mmu_free_memory_cache()` is never called**. Now take step 6 in the *non-fatal* case, i.e. when the hyp memcache happens to be empty (`nr_pages == 0` ⇒ `kmem_cache == NULL` ⇒ no Oops): `__kvm_mmu_topup_memory_cache()` succeeds and `kvmalloc_array()`s `mc->objects` into bytes 24–31 (outside the hyp memcache), sets `mc->capacity`/`mc->nobjs` over `stage2_mc.flags` (destroying `HYP_MEMCACHE_ACCOUNT_KMEMCG|HYP_MEMCACHE_ACCOUNT_STAGE2`), and allocates **`KVM_ARCH_NR_OBJS_PER_MEMORY_CACHE` = 40 pages** via `__get_free_page()` (`asm/kvm_types.h:5`).

Those 40 pages (160 KiB) plus the `objects` array are then **leaked for the lifetime of the machine**: `free_hyp_memcache()` walks `while (mc->nr_pages)`, which is 0, and frees nothing. Because they come from `__get_free_page()` they appear in **no** `/proc/meminfo` counter, sit on **no LRU**, and are unreclaimable and unswappable — the signature recorded in `FINDING.md` §2. But the magnitude is wrong by three orders of magnitude: **160 KiB per affected vCPU** needs ~160,000 vCPUs for 25 GB, against `exec total=126188` for the whole two-machine campaign. So candidate B is a **real, proven leak that is not the OOM**. Note also that `init_hyp_memcache()` memsets the union, wiping the `gfp_zero = __GFP_ZERO` set at `arm.c:474`, so these pages come back **not zeroed** — a latent hazard if they are ever used as stage-2 tables.

**Verdict on §1 vs §2:** they share a *trigger* (dirty logging on a pKVM host) and they share a *root theme* (legacy non-pKVM paths left unconverted), but on the present evidence they are **not proven to be one defect**. §1 is closed; §2 is open with candidate A as the leading hypothesis and a named discriminator.

---

## Also found while confirming the above (independent, not needed for §1)

**The hypercall-ID gate is the real cause of the `pgtable.c:639` WARN storm.** `kvm_asm.h:53-75` puts all TLB-maintenance ids *below* `__pkvm_prot_finalize` (`__kvm_tlb_flush_vmid` = 11, `__pkvm_prot_finalize` = 20), and the Rust EL2 dispatcher raises `hcall_min` to 20 once `kvm_protected_mode_initialized` is set (`hyp/nvhe/rust/src/hyp_main.rs:1627-1663`), returning `SMCCC_RET_NOT_SUPPORTED` (-1) **without calling the handler**. So post-finalization **every** `__kvm_tlb_flush_vmid*` / `__kvm_flush_vm_context` from the host is silently a no-op. Reached via `kvm_arch_flush_remote_tlbs_range()` (`mmu.c:188`), which lacks the `is_protected_kvm_enabled()` branch its sibling `kvm_arch_flush_remote_tlbs()` (`mmu.c:179`) has, and which also passes `&kvm->arch.mmu` whose `pgt`/`vmid`/`pgd_phys` are never initialised under pKVM (`kvm_init_stage2_mmu()` returns early at `mmu.c:1104`). Two independent reviews reached this conclusion. Severity: **write-protect without invalidation ⇒ dirty tracking can miss guest writes**, i.e. a migration/snapshot correctness hole, not just log noise.

**Guest-driven pin underflow.** `hypercalls.c:371-372` routes `ARM_SMCCC_VENDOR_HYP_KVM_MEM_RELINQUISH_FUNC_ID` to `pkvm_host_reclaim_page(vcpu->kvm, smccc_get_arg1(vcpu))` **without** a `kvm_vm_is_protected()` gate (the adjacent `MEM_SHARE`/`MEM_UNSHARE` cases have one) and with a fully guest-controlled IPA. `pkvm_host_reclaim_page()` (`pkvm.c:536-563`) decrements `ppage->pins` per call and unpins at 0, so a guest can call it 512× on one 4 KB IPA of an order-9 ppage and free the host folio while EL2 still maps all 2 MB to that guest. Security-relevant.

**The new EL2-coverage code is NOT implicated in this crash** — confirmed by measurement, not inspection: `struct kvm_vcpu` / `kvm_vcpu_arch` / `kvm_mmu_memory_cache` DWARF is byte-identical across host C (`vmlinux`), EL2 C (`kvm_nvhe.o`) and the Rust bindgen view (`nvhe_rust.o`), so the layout-skew theory is dead; and `KVM_PKVM_COV_HVC` leaves the SMCCC arg list, register selection and `&res` store untouched, so the WARN is a genuine EL2 rejection, not an ABI artifact. It does carry its own bug, though: the RCU/preemption safety argument in `pkvm_cov.c:77-82, 383-395` cites a `kvm->mmu_lock` call site that **no longer exists** (the blanket HVC wrapper replaced it), so begin/end now run **preemptibly**; with `PREEMPT_VOLUNTARY` + `TREE_RCU` a preemptible context is a quiescent state, so `synchronize_rcu()` in `pkvm_cov_disable()` does not wait for an in-flight pair and `free_pages()` can land mid-drain (host-side ring use-after-free). Fix by wrapping begin→HVC→end in `rcu_read_lock()` / `preempt_disable()`. Also: bindgen runs with `--no-layout-tests`, removing the one automatic guard against the very skew that was ruled out here.

---

## What to fix (not yet implemented)

1. **The union is only safe if exactly one view is ever live per vCPU.** Under `is_protected_kvm_enabled()` the `stage2_mc` view owns it, so `user_mem_abort()` must not be reached at all for such VMs — the permission fault at `mmu.c:2322-2325` needs a pKVM path (the tree already has `pkvm_relax_perms()`, reached later at `mmu.c:2142`, and it correctly uses `stage2_mc`). The minimal correct change is to stop `user_mem_abort()`'s prologue from touching `mmu_page_cache` when pKVM owns the union. Splitting the union would also work and is far easier to reason about; the 32 bytes saved per vCPU are not worth this class of bug.
2. `kvm_arch_flush_remote_tlbs_range()` must mirror `kvm_arch_flush_remote_tlbs()`: use `__pkvm_tlb_flush_vmid` with `kvm->arch.pkvm.handle` under pKVM, or return non-zero so the generic full-flush fallback engages. Alternatively move the TLB ids above `__pkvm_prot_finalize` in `kvm_asm.h`.
3. `mmu.c:1864` must not swallow `insert_ppage()` failure: unwind the EL2 mapping, `unpin_user_pages()`, `kfree(ppage)` and return an error — never 0. Move the overlap `mt_find()` out of the `page_size > PAGE_SIZE` condition and preallocate the maple node outside `write_lock(&kvm->mmu_lock)`.
4. `kvm_arch_vcpu_destroy()`'s predicate should match whichever view was actually used, and `fpsimd.c:54`'s `BUG_ON(!current->mm)` should be demoted — any task dying with a loaded vCPU hits it.
5. Gate `MEM_RELINQUISH` on `kvm_vm_is_protected()` and validate the relinquished IPA against `ppage->ipa`/order.

Fixes 1 and 2 are independent; either alone leaves a real bug.

---

## Independent verification log (2026-07-30)

Every load-bearing claim above was re-checked against the sources rather than
accepted. All hold.

| claim | check | result |
|---|---|---|
| union `mmu_page_cache` / `stage2_mc`, alias at +8 | read `kvm_host.h:678`, `kvm_types.h:93`, `kvm_host.h:86` | ✔ `kmem_cache` and `nr_pages` both at +8 |
| `x19` is `kmem_cache_alloc` arg1 | disassembled archived `vmlinux`: `+0x18 mov x19,x0`; fault at `+0x50` | ✔ matches `pc : kmem_cache_alloc+0x50` |
| register decode, both crashes | re-read reports; `x22 == x20 & 0xfff` twice; `head` page-aligned twice | ✔ (see refinement above) |
| dirty logging set unconditionally | `executor/common_kvm_arm64.h:117` — `KVM_MEM_LOG_DIRTY_PAGES` on `ARM64_ADDR_DIRTY_PAGES` | ✔ closes the gap `ROOT-CAUSE-ANALYSIS.md` left open |
| fault-type dispatch reaches both members | `mmu.c:2322-2325` | ✔ predicate is `is_protected_kvm_enabled() && fault_status != PERM` |
| N90 hit the identical crash twice | `zcat n90-dmesg-history.log.gz` | ✔ `[33369]` and `[34182]`, same Oops + `fpsimd.c:54` + recursive fault |
| all `B` taints belong to `#47`, none to `#51` | same log | ✔ 656 total, **0** on `6.6.30-covfix` |
| `flush_remote_tlbs_range` lacks the pKVM branch | `mmu.c:179` vs `mmu.c:188` | ✔ sibling has `is_protected_kvm_enabled()`, range version does not, and returns 0 |
| `MEM_RELINQUISH` ungated | `hypercalls.c:371` vs the adjacent `MEM_SHARE`/`MEM_UNSHARE` at `:375-377` | ✔ neighbours carry `if (!kvm_vm_is_protected(...)) break;`, relinquish does not |

Nothing was found that contradicts this document. The `ROOT-CAUSE-ANALYSIS.md`
hypothesis it refutes — guest writes through stale stage-2 TLB entries — is
correctly dead: the N90 negative control (identical crash, 30k TLB WARNs, no
leak, no corruption, 5.3 h later still healthy) is decisive on its own.
