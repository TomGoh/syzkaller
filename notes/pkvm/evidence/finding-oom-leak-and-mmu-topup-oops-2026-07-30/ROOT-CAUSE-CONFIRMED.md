# CONFIRMED root cause — union type confusion between `mmu_page_cache` and `stage2_mc`

Written 2026-07-30 as a **correction and completion** of `ROOT-CAUSE-ANALYSIS.md`. That document's PROVEN 1–4 stand. Its central **HYPOTHESIS ("stale stage-2 TLB entries let the guest write host memory") is now positively REFUTED**, and the real defect is proven from the register dumps alone, quantitatively, on both crashes. Nothing here rests on reasoning-from-code-shape: every number in the two Oops register dumps is derived exactly.

`ROOT-CAUSE-ANALYSIS.md` is kept unmodified for the record; read this file for the conclusion.

> ### Status — do NOT read this document as uniformly confirmed
>
> The title applies to **§1 only** (the `__kvm_mmu_topup_memory_cache` Oops). Confidence varies by section, and three review rounds on 2026-07-30 found several claims of mine that were wrong. Read the per-item labels.
>
> | Section | Status |
> |---|---|
> | Union type confusion → the Oops | **CONFIRMED** — register-exact on both crashes, mechanism verified in code, upstream independently fixed it the same way |
> | `pgtable.c:639` WARN → EL2 rejects hypercall id 11 | **CONFIRMED** — `x21 == -1 == SMCCC_RET_NOT_SUPPORTED` in 27,674 of 27,674 `#51` dumps, single backtrace signature |
> | WARN *count, rate and granularity* | **RE-MEASURED (round 3)** — the 30,134 total mixes two kernels (`#51` = 27,674, `#47` = 2,460). `#51` alone: 0.524 WARN/s over its own 52,816 s span; peak 162 reports in a 1 s *kernel-timestamp* window; **3,297 B** per report (measured, not 9 KiB) ⇒ ~1.7 KB/s average. The old "2,787/s" peak is **not reproducible** and the "console saturated / fuzzer throttled" conclusion is **withdrawn entirely** |
> | WARN granularity | **CORRECTED (round 3)** — not "one per VM per program": one per *dirty-logging memslot commit*, i.e. one per VM, and one program can build several VMs (one PID produced **6** WARNs with **6 different `x22`**) |
> | OOM / ~25 GB residual | **OPEN, root cause unconfirmed.** Four inferences withdrawn: "swap untouched" (2.264 GiB *was* used), `mapped` = 8.6 GB read as occupancy/LRU proof, D3000-vs-N90 residuals computed with different formulas, and the residual formula **double-counting free CMA** (zone `free`/`MemFree` already include it). Corrected residuals: **D3000 25,977 MiB (93.9 %)**, **N90 1,024 MiB (4.3 %)** |
> | Leak candidate A (`insert_ppage`) | **HYPOTHESIS, mechanism corrected (round 3)** — a dropped maple-tree record does *not* by itself make the same IPA re-fault (the EL2 mapping succeeded), so the "re-faults and leaks again" rate estimate is withdrawn; repeated leaking needs new IPAs or an intervening relinquish/unmap. Upstream `9cfc94644ba4` removes **both** the `-ENOMEM` **and** the `-EEXIST` precondition |
> | Leak candidate B (40 pages/vCPU) | **NOT EXCLUDED** — my "three orders of magnitude too small" dismissal was an arithmetic error; the real ratio is 1.32. The N90 "bound" rests on an **unsourced 50/50 execution split** — the manager log carries no per-machine counters |
> | `MEM_RELINQUISH` 512×-same-IPA | **RETRACTED** — refuted by EL2's `kvm_pte_valid()` pre-check |
> | `MEM_RELINQUISH` `kvm_vm_is_protected()` gate | **WITHDRAWN AS WRONG (round 3)** — EL2 deliberately supports this call for **non-protected** guests; adding the gate would break a working protocol. Replaced by three real defects (concurrent `ppage->pins` UAF, order>0 un-accounting, both already fixed upstream) |
> | `pkvm_cov` begin/end locking | **DOWNGRADED (round 3)** — the documented `kvm->mmu_lock`/preempt-off contract is **void** (fact), but a use-after-free in *this* build is **unproven**: the config is `PREEMPT_VOLUNTARY` + `TREE_RCU` with no scheduling point in begin→HVC→end. Real hazard on a `CONFIG_PREEMPT=y` build |
> | `arch_timer.c:461` WARN (12×) | **SEPARATE, UNANALYSED (new in round 3)** — `kvm_vgic_inject_irq()` error inside `kvm_timer_update_irq()` on the vCPU-reset path; no evidence of any link to the TLB HVC rejection |
> | Fix recommendations | **three were wrong and are struck**: "`user_mem_abort()` must not be reached under pKVM"; "move the TLB hypercall ids above `__pkvm_prot_finalize`" (would re-expose a host-pointer ABI to an untrusted host); and "add a `kvm_vm_is_protected()` gate to `MEM_RELINQUISH`" (would break non-protected guests) |

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

**These two Oopses are fully explained without needing to assume any prior memory corruption.** Both writer and reader are legitimate, correctly-typed accesses to a live `struct kvm_vcpu`, so nothing in either report *requires* a corrupting agent to have run first. That is also why KASAN (`CONFIG_KASAN=y`, shadow base `0xdfff800000000000` visible in x4) stayed silent on this path — there is nothing here for it to report — and the elaborate "EL2/guest writes are invisible to KASAN" argument in `ROOT-CAUSE-ANALYSIS.md` is unnecessary. *(Scope, added 2026-07-30 round 3: an earlier version of this paragraph asserted flatly "there is no memory corruption". That over-reached. This evidence shows these two crashes need no corruption to explain them; it cannot show that no corruption exists anywhere on this machine, and in particular it says nothing about the still-open §2 leak.)*

## The full causal chain (all links verified in code)

1. Boot: `kvm-arm.mode=protected` (verified in the `n90-dmesg-history.log.gz` cmdline) ⇒ `is_protected_kvm_enabled()` = true, `finalize_pkvm` ran.
2. `setup_vm()` registers gpa `0xdddd1000`, 2 pages, with **`KVM_MEM_LOG_DIRTY_PAGES`** — unconditionally, inside the pseudo-syscall (`syzkaller/executor/common_kvm_arm64.h:104,117`), reached from both `syz_kvm_setup_cpu$arm64` (`:224`) and `syz_kvm_setup_syzos_vm$arm64` (`:251`). So `memslot_is_logging()` is true for that slot in *every* syzkaller KVM program, and the 5-call reproducer needs no explicit dirty-log ioctl. The reproducer's `@memwrite …0xdddd1000` writes straight into it.
3. **That registration happens first, before any `KVM_RUN`, and it already fires the TLB WARN.** `KVM_SET_USER_MEMORY_REGION` → `kvm_arch_commit_memory_region` (`mmu.c:2532-2563`) → `kvm_mmu_wp_memory_region` (`mmu.c:1404-1420`) → `stage2_wp_range` → `pkvm_wp_range` (`mmu.c:1354`) → `__pkvm_wrprotect` at EL2, **which does no TLBI**; then `kvm_flush_remote_tlbs_memslot` → `kvm_arch_flush_remote_tlbs_range()` (`mmu.c:188`) — **never made pKVM-aware** — issues HVC id 11 `__kvm_tlb_flush_vmid`. The EL2 dispatcher rejects it because post-finalization `hcall_min` is raised to id 20 (`hyp/nvhe/rust/src/hyp_main.rs:1627-1663`), returning `SMCCC_RET_NOT_SUPPORTED` (-1) → `WARN_ON` at `pgtable.c:639`. At this instant the guest has never run, so **the stage-2 may still be completely empty and `pkvm_wp_range()`'s maple-tree walk has nothing to iterate** — the WARN is about the *flush*, not about any mapping. **This is finding (3) / "BUG2", and the TLB is genuinely never invalidated.**
4. Only later, on the first guest access to that slot, does the abort path run: first touch = **translation** fault → `kvm_handle_guest_abort()` takes the pKVM branch (`mmu.c:2322`, predicate `is_protected_kvm_enabled() && fault_status != ESR_ELx_FSC_PERM`) → `pkvm_mem_abort()` → `topup_hyp_memcache(&vcpu->arch.stage2_mc, …)` (`mmu.c:1776`). The union now holds a live hyp memcache: `head` = donated page PA, `nr_pages` = 2–3.
5. `pkvm_mem_abort()` installs the mapping **read-only** — `pkvm_host_map_guest(pfn, …, KVM_PGTABLE_PROT_R)` (`mmu.c:1851-1852`) — and returns to the guest.
6. **The same store instruction retries** (AArch64 data aborts are precise; the faulting instruction is restarted, this is not a new, second write) and now takes a **permission** fault → `mmu.c:2322`'s predicate is false → the `else` at `mmu.c:2325` → **`user_mem_abort()`**, whose `memcache` is `&vcpu->arch.mmu_page_cache` (`mmu.c:1952`).
7. `mmu.c:1979-1981`: `fault_status == PERM`, so the top-up runs only because `logging_active && write_fault` — both true here → `kvm_mmu_topup_memory_cache(memcache, …)`.
8. `mc->kmem_cache` = `nr_pages` = 3 ⇒ `if (mc->kmem_cache)` passes ⇒ `kmem_cache_alloc((struct kmem_cache *)3, 0x2cc28022)` ⇒ `ldr w22,[x19,#28]` ⇒ **Oops at 0x1f**.

*Ordering corrected 2026-07-30 (round 3).* An earlier version of this list had steps 3 and 4 the other way round — "first touch → translation fault … **then** registering that slot write-protects it" — and described step 6 as "guest **writes again**". **Both were wrong.** `setup_vm()` creates the dirty-logging memslot before the vCPU is ever run (`common_kvm_arm64.h:97-140`; `syz_kvm_add_vcpu` and `ioctl$KVM_RUN` are separate, later calls), so the write-protect-plus-flush — and therefore the WARN — comes **first**, with the stage-2 possibly still empty. And the abort at step 6 is the *same* instruction being restarted after being handed a read-only mapping, not a second, independent write.

Two consequences worth stating explicitly, because they are easy to get backwards:

- The WARN and the Oops **share the dirty-logging trigger**: `KVM_MEM_LOG_DIRTY_PAGES` on this memslot is what puts step 3 on the write-protect path *and* what makes `logging_active` true at step 7. That is why they co-occur in every affected program.
- But the step-3 WARN is **not a necessary precondition of the Oops**. The Oops needs only the union filled as `stage2_mc` by step 4 plus `logging_active && write_fault` at step 7. Nothing downstream consumes the outcome of the range flush, and `kvm_arch_flush_remote_tlbs_range()` returns 0 unconditionally, so the failed flush does not even alter control flow. A machine *with* FEAT_TLBIRANGE, or one on which id 11 were accepted, would still take the Oops.

The second Oops (`fpsimd.c:54 BUG_ON(!current->mm)`) is exactly as `ROOT-CAUSE-ANALYSIS.md` PROVEN 3 describes: a consequence, not a separate bug.

## The TLB WARN is a SIBLING symptom, not the cause

`ROOT-CAUSE-ANALYSIS.md` treats the 39 `pgtable.c:639` WARNs as "the head of the chain". They are not — even though, as the corrected ordering above shows, the WARN does come **first in time**. Being first is not being causal: **both symptoms are independently reached through dirty logging**, which is why they co-occur. Step 3 (the WARN) is on the write-protect-on-enable path taken by `KVM_SET_USER_MEMORY_REGION`; step 7 (the Oops) needs `logging_active && write_fault`. Neither consumes the other's result, and the Oops does not require the flush to have failed.

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

Every captured WARN carries **`x21 = 0xffffffffffffffff` = -1 = `SMCCC_RET_NOT_SUPPORTED`** — measured: **27,674 of 27,674** `6.6.30-covfix #51` instances, one distinct value. That value is *uniquely* the dispatcher's `id < hcall_min` rejection: `handle_host_hcall` sets `regs[0] = SMCCC_RET_SUCCESS` **before** invoking a handler, and `handle___kvm_tlb_flush_vmid` has no error return at all, so a handler that ran — successfully or not — could never leave -1 in `a0`. The TLBI therefore did not merely fail; **it was never attempted.**

What is genuinely invariant is the *call site*, not the register file: all 27,674 `#51` instances share a **single backtrace signature**, `kvm_tlb_flush_vmid_range < kvm_arch_flush_remote_tlbs_range < kvm_flush_remote_tlbs_memslot < kvm_arch_commit_memory_region < kvm_set_memslot < __kvm_set_memory_region < kvm_vm_ioctl < __arm64_sys_ioctl`. Together with the constant `x21 = -1` that is what establishes a fixed, data-independent trigger rather than a race.

*Corrected 2026-07-30 (round 3).* An earlier version claimed "the rest of the dump is constant across every instance". **That was wrong**, and it mattered, because it was the basis for the (also wrong) "one WARN per program" granularity claim. Counted over the 27,674 `#51` dumps:

| register | meaning | distinct values observed |
|---|---|---|
| `x21` | `res.a0` | **1** — `0xffffffffffffffff` (27,674) |
| `x22` | `&kvm->arch.mmu`, i.e. the `struct kvm_s2_mmu` | **21,016** — one per VM (some addresses reused after teardown) |
| `x23` | flush range base | 3 — `0xdddd1000` (27,663), `0x08080000` (8), `0x00001000` (3) |
| `x20` | flush size | 2 — `0x2000` (27,666), `0x1000` (8) |
| `x26` | range end | 3 — `0xdddd3000`, `0x08081000`, `0x00003000` |
| `x28` | last byte of range | 3 — `0xdddd2fff`, `0x08080fff`, `0x00002fff` |

So `x22` varies essentially per VM, and the flush range is *dominantly* but not exclusively the 2-page `0xdddd1000` dirty-logging slot — a handful of dumps show a 1-page flush at `0x08080000` (= `ARM64_ADDR_GITS_BASE`, `executor/kvm.h:521`, the GICv3 ITS window) and three at `0x00001000`. The `0xdddd1000` slot accounts for 27,663/27,674 = 99.96 %, so the earlier "constant" reading was a reasonable *approximation* of the range registers and simply wrong about `x22`.

### Why it fires from the first program and never stops — mechanism, granularity, rate

**Mechanism, stated exactly.** `kvm_tlb_flush_vmid_range()` (`hyp/pgtable.c:633-651`) begins:

```c
	if (!system_supports_tlb_range()) {
		kvm_call_hyp(__kvm_tlb_flush_vmid, mmu);   /* :639 */
		return;
	}
```

On this machine, **which lacks FEAT_TLBIRANGE**, that early-out is taken — the presence of `pgtable.c:639` in the pc is itself the proof — so **every relevant dirty-logging memslot commit attempts the legacy HVC id 11, EL2 returns -1, and one `WARN_ON` fires**. Exactly one: line 639 issues a single HVC and returns, and the `WARN_ON(res.a0 != SMCCC_RET_SUCCESS)` lives inside the `kvm_call_hyp_nvhe` macro (`kvm_host.h:1127-1134`), which is why the report is attributed to the call site rather than to the ID gate that actually rejected it. ("Relevant" excludes VMs that enabled `KVM_CAP_MANUAL_DIRTY_LOG_PROTECT2`: `kvm_arch_commit_memory_region()` then returns before `kvm_mmu_wp_memory_region()`, and the flush arrives later from `KVM_CLEAR_DIRTY_LOG` instead. Empirically none of the 27,674 `#51` dumps took that route — all came through `kvm_arch_commit_memory_region`.) On a FEAT_TLBIRANGE machine the fall-through loop would instead issue id 12 one or more times, so both the count *and* the failing id are machine-dependent.

`setup_vm()` registers the `KVM_MEM_LOG_DIRTY_PAGES` slot on **every** VM it builds (`executor/common_kvm_arm64.h:117` — hardcoded, not a fuzzer choice), the WARN sits on the write-protect-on-enable path, and it is `WARN_ON`, not `WARN_ON_ONCE`. So the trigger is deterministic and fires from the first KVM program onward. Nothing ever complains louder because `kvm_arch_flush_remote_tlbs_range()` returns 0 unconditionally.

**Granularity — corrected 2026-07-30 (round 3).** An earlier version said "**one WARN per `syz_kvm_setup_cpu` call**, i.e. per VM, not per program", citing a gap in the `syz.1.65xx` comm sequence as evidence. That is **not** what the log shows. The unit is one WARN per dirty-logging memslot commit, i.e. **one per VM** — but **one program can create several VMs**, so the per-*PID* count is not 1. Measured over the 27,674 `#51` WARNs, which come from **20,391 distinct PIDs**:

| WARNs from one PID | number of PIDs |
|---|---|
| 1 | 14,609 |
| 2 | 4,361 |
| 3 | 1,365 |
| 4 | 33 |
| 5 | 22 |
| **6** | **1** |

The extreme case is PID 175465 (`syz.0.3`), which produced **six** WARNs in 26 ms (`[33415.104]` … `[33415.130]`) with **six different `x22` values** — six distinct `struct kvm_s2_mmu`, i.e. six VMs inside one program. Across the whole `#51` set the distinct-`x22`-per-PID histogram matches the WARN-per-PID histogram almost exactly, which is the direct confirmation that the unit is the VM and the multiplicity is VMs-per-program.

**Rate — recounted 2026-07-30 (round 3).** An earlier version claimed "exactly one WARN per fuzzing program … at ~10/s" and concluded the serial console was saturated; a second version corrected the rate to 0.689/s but **still mixed two kernel builds into one span**. `n90-dmesg-history.log.gz` is a concatenation of two boots whose uptimes overlap, so the earlier span `[9478.4] → [53242.1]` starts on a `6.6.30+ #47` WARN and ends on a `6.6.30-covfix #51` one. Split by build:

| | all | `6.6.30-covfix #51` | `6.6.30+ #47` |
|---|---|---|---|
| `pgtable.c:639` WARN count | 30,134 | **27,674** | **2,460** |
| first → last WARN | — | `[426.2]` → `[53242.1]` | `[320.4]` → `[10467.9]` |
| span | — | **52,815.9 s (14.67 h)** | 10,147.5 s (2.82 h) |
| **average rate, own span** | — | **0.524 WARN/s** | 0.242 WARN/s |
| peak, worst 1 s sliding window | — | **162 reports** | 37 reports |

Only the `#51` row belongs to the build under investigation. **Average rate = 27,674 / 52,815.9 s = 0.524 WARN/s.**

**Bytes per WARN — measured, 2026-07-30 (round 3).** The earlier "~9 KiB per WARN" was never measured and is **too large by ~2.7×**. Extracting all 30,134 `cut here`→`end trace` blocks containing `pgtable.c:639` from the same log gives **median 3,297 bytes / 36 lines** per report (mean 3,298; min 3,289; max 3,334 — the spread is just the varying PID and comm). Redoing the byte-rate arithmetic with the `#51` rate and the measured size: 0.524 × 3,297 B = **~1.7 KB/s average**, against ~11.25 KB/s for a 115200 8N1 UART (`console=ttyAMA0,115200` is in the cmdline). So on average the console carries roughly 15 % of its line rate from this WARN.

**About the burst figure.** The earlier "2,787 WARN/s peak" is **not reproducible from this log by any aggregation I can construct**, and it is withdrawn. The largest 1-second figures I can actually measure are: **162 WARN reports** in the worst 1-second sliding window of `#51` kernel timestamps (worst whole second: 159, at `t ≈ 42397`), and **5,832 log lines** in the worst 1-second window over all lines — which is just 162 × 36, the same event counted per line. Both are **aggregations over `printk` timestamps**, i.e. over the times at which records entered the kernel ring buffer. **They are not measurements of UART throughput, and they must not be presented as one.** In fact the arithmetic points the other way: 162 × 3,297 B ≈ 534 KB would need ~47 s to leave a 115200 UART, yet it appears inside a 1-second timestamp window. Since this kernel flushes the console from the `printk` caller's own context, a genuinely 115200-baud-limited console could not have transmitted that; so the burst figure is evidence about ring-buffer arrival, and what actually reached the wire during that second is **not determined by this log**. The old inference chain — burst demand ≈ 25 MB/s ⇒ console oversubscribed by three orders of magnitude ⇒ throttling and truncation ⇒ fuzzer throttled — is therefore **deleted, not merely downgraded**; none of its links is supported by the data in hand. Measuring it would need the raw UART capture with host-side timestamps, which we do not have for `#51` (`d3000-serial-oom-tlbwarn.log` contains exactly one `pgtable.c:639` report). Independently of any of this, do **not** paper the WARN over with `WARN_ON_ONCE`; fix 2 below removes both the noise and the correctness hole.

The correlation the note flagged as "suggestive, not conclusive" — TLB WARN registers `x23=0xdddd1000, x26=0xdddd3000, x28=0xdddd2fff` matching the reproducer's write target — is now fully explained: that is precisely the 2-page `ARM64_ADDR_DIRTY_PAGES` memslot `[0xdddd1000, 0xdddd3000)` being flushed. It is a shared *trigger*, not a causal link.

This also removes the TLB failure as the *corrupting* mechanism in the D3000-vs-N90 puzzle the note could not resolve. *(Scoped 2026-07-30 round 3: an earlier version said "N90's TLB WARNs did not leak or corrupt anything". That over-claims — see "What this control does and does not establish" below. What the N90 data supports is that the TLB rejection is not sufficient to produce the runaway, not that it cost nothing.)*

Consequence of the TLB bug in its own right (still real, still worth fixing): `kvm_arch_flush_remote_tlbs_range()` returns 0 unconditionally, so generic KVM skips its `kvm_flush_remote_tlbs()` fallback — which *would* have worked, since `kvm_arch_flush_remote_tlbs()` (`mmu.c:179`) does use the pKVM-aware `__pkvm_tlb_flush_vmid` (id 28). Stale writable stage-2 TLB entries therefore survive a write-protect, so **writes can be missed by dirty tracking**.

---

## The leak — sharpened, still NOT root-caused

`FINDING.md` §2 stays open. **Do not close it on the strength of the union bug**: a negative control (below) shows the Oops and the TLB WARN are not sufficient to produce it.

### Re-measured from the serial log — worse than recorded

At the first `oom-kill:constraint=CONSTRAINT_NONE` (`[37491]`), from the `Mem-Info` block in `serial/d3000-serial-oom-tlbwarn.log:57-85`. Page-unit counters are converted at 4 KiB; "MiB" below means 1,024 kB throughout, which the earlier mixed-unit version of this table did not hold to:

| | value |
|---|---|
| managed (DMA 1,807,036 + Normal 26,513,448 kB) | 28,320,484 kB = **27,656.7 MiB** |
| accounted (free+anon+file+unevictable+slab+pagetables+sec_pagetables) | 1,719,856 kB = **1,679.6 MiB** |
| **unaccounted** | **26,600,628 kB = 25,977 MiB = 25.4 GiB (93.9 %)** |
| `mapped` | 9,052,192 kB = **8,840 MiB** (2,263,048 pages) |
| `active_file + inactive_file` | 26,144 kB = 26 MiB (6,536 pages) |
| `shmem` | **0** |
| Free / Total swap | 37,015,860 / 39,390,272 kB ⇒ **2.264 GiB in use (6.0 %)** |
| `all_unreclaimable?` | **no** |

**Five corrections to how these numbers must be read (2026-07-30, rounds 2 and 3).** Earlier versions of this section over-claimed; the raw figures stand, the inferences did not.

1. **The residual formula was double-counting CMA — corrected in round 3.** Earlier versions summed `… + cma_free` alongside `free`. **That is wrong: zone `free` (and `MemFree`) already include free CMA pages.** The OOM report makes this visible directly — the global line reads `free:35917 … free_cma:9754`, and `9754 × 4 KiB = 39,016 kB` reappears verbatim as `free_cma:39016kB` *inside* `Node 0 DMA free:122620kB`. Adding it again inflated "accounted" by 39,016 kB and shrank the residual by the same amount. Note the direction: removing the double count makes the residual **larger**, not smaller — D3000 goes from 25,939 MiB (93.8 %) to **25,977 MiB (93.9 %)**, and N90 from 984 MiB (4.1 %) to **1,024 MiB (4.3 %)**. Every dependent number in this document has been recomputed; the table above and the D3000-vs-N90 table below are the corrected versions.
2. **Swap was NOT untouched.** `39,390,272 − 37,015,860 = 2,374,412 kB = 2.264 GiB` had been swapped out (6.0 % of swap). `FINDING.md`'s "37 GB (never touched)" is a subtraction error that this document repeated without checking. "Swap untouched" is not usable as evidence.
3. **"Swap ran out of candidates, so the missing memory is not anon" was too strong — corrected in round 3.** The observation is real: 2.26 GiB had gone to swap against only 128.7 MiB resident anon (`active_anon` 79,340 + `inactive_anon` 52,496 kB) with `all_unreclaimable? no`, so the swap path was working and was not being starved. But what that excludes is only **ordinary reclaimable, swappable anon**. It does *not* exclude anon (or shmem) pages that reclaim cannot touch — in particular pages that have been **unmapped but are still held by a long-term GUP pin**, which is exactly the shape of leak candidate A. `__remove_mapping()` / `add_to_swap()` fail on an elevated, pinned refcount, and such a folio is simply kept, so it is invisible to a "swap ran out of candidates" argument. The honest statement is therefore: *swap functioned and had exhausted the ordinary anon it could reach*; the residual is not ordinary anon, and this evidence says nothing either way about pinned anon.
4. **`mapped` = 8.6 GiB cannot be treated as 8.6 GiB of occupancy, nor as proof of LRU detachment.** `NR_FILE_MAPPED` is a *classification* counter (file-backed folios present in ≥1 page table), maintained by `folio_add_file_rmap_*` / `page_remove_rmap`. An inflated value is equally consistent with a **pure counter leak** — rmap increments without matching decrements, involving *zero* physical memory — as with pages that are mapped but isolated from every LRU. Calling the state "internally impossible" was too strong. It is **internally inconsistent, cause undetermined**: `mapped` (2,263,048 pages) vastly exceeds the file LRUs (6,536 pages) with `shmem: 0`, which proves *some* accounting or LRU defect exists but neither quantifies memory nor identifies which.
5. **25.4 GiB is a coarse residual, not a measured leak.** It is `managed −` (the counters the OOM report happens to print), so it silently absorbs vmalloc, per-cpu, driver allocations — `amdgpu` is loaded on this box — and anything else the report does not classify. (`kernel_stack:38528kB` *is* printed and is small enough to ignore at this scale; it is excluded from the sum above for consistency with the N90 counter set.) It is an upper bound on "unexplained".

`FINDING.md` records 23.9 GB; the residual computed here is **25.4 GiB**. The OOM root cause remains **unconfirmed**.

### NEGATIVE CONTROL — the leak is not caused by the Oops (or the TLB WARN)

Build `6.6.30-covfix #51`, `n90-dmesg-history.log.gz`: **N90 hit the identical crash twice** — `Internal error: Oops: 0000000096000006` → `kernel BUG at fpsimd.c:54` → `Fixing recursive fault but reboot is needed!` at `[33369]` and `[34182]` — and **27,674** × `pgtable.c:639` on this build (the log's 30,134 total includes 2,460 from the earlier `#47` boot). Its live snapshot at `[53401]`, i.e. **5.3 h after the second recursive fault**, is healthy: `Mapped` 726 MB, `MemAvailable` 11.0 GB, `SwapFree == SwapTotal` (genuinely untouched here, unlike D3000).

*Methodology fix (2026-07-30, rounds 2 and 3).* An earlier version quoted "0.5 GB" for N90 against "25.3 GB" for D3000 — but those were computed with **different counter sets** (N90 from `/proc/meminfo`, including `VmallocUsed`/`Percpu`/`Buffers`; D3000 from the OOM `Mem-Info`, which prints none of those). Round 2 recomputed with the same counter set but **added `cma_free` on top of `free` on both sides**, which double-counts (see correction 1 above). Both machines are shown below with the same counter set and **without** the CMA double count — `free + anon + file + unevictable + slab + pagetables + sec_pagetables`:

| | total | accounted | residual | (round-2 figure, CMA double-counted) |
|---|---|---|---|---|
| D3000 at OOM `[37491]` | 28,320,484 kB managed = 27,656.7 MiB | 1,719,856 kB = 1,679.6 MiB | **25,977 MiB (93.9 %)** | 25,939 MiB (93.8 %) |
| N90 live `[53401]` | 24,614,392 kB = 24,037.5 MiB | 23,565,920 kB = 23,013.6 MiB | **1,024 MiB (4.3 %)** | 984 MiB (4.1 %) |

The contrast survives the correction — ~1 GiB versus ~25 GiB — but both figures are residuals, not measurements.

So: same kernel, same workload, same Oops, same TLB storm, **longer** uptime → no runaway. (An earlier draft of this file argued the aborted `exit_mmap` was the leak; the N90 data refutes that as sufficient, and it is retracted.)

**What this control does and does not establish — sharpened 2026-07-30 (round 3).** Stated plainly: it shows at most that **the Oops and the TLB WARN are NOT SUFFICIENT to cause the OOM**. N90 took both, twice and 27,674 times respectively, over a *longer* uptime, and did not run away. That is a real and useful exclusion — it kills "the Oops *is* the leak" as a complete explanation, and it is what refutes `ROOT-CAUSE-ANALYSIS.md`'s stale-TLB hypothesis. **It cannot show that they contribute nothing.** A necessary-but-not-sufficient contribution, a rate that is merely lower on N90, or a per-event leak too small to matter over 14.7 h there but decisive under whatever additional state D3000 entered, are all fully consistent with this data. Whatever ran away on D3000 needs at least one further ingredient that N90 never entered; the control identifies neither that ingredient nor the size of any contribution from the two known symptoms.

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

`insert_ppage()` → `mtree_insert_range()` returns `-EEXIST` on **any** overlapping range and `-ENOMEM` under pressure (and it is called with `GFP_KERNEL` while holding `write_lock(&kvm->mmu_lock)`). On failure the page stays **pinned (`FOLL_LONGTERM`) and mapped into the guest at EL2**, but its only tracking record — the `kvm_pinned_page` in `kvm->arch.pkvm.pinned_pages` — is dropped, and the abort **reports success**. The overlap pre-check at `mmu.c:1840-1841` only runs for `page_size > PAGE_SIZE`, so the 4 KB fall-back path re-inserts into a range still covered by a surviving order-9 entry. Nothing else can recover the pin: `kvm_unmap_gfn_range()` is a **no-op under pKVM** (`mmu.c:2339-2341`), so munmap / mmu-notifier invalidation never drops pins, and teardown walks only the maple tree — an untracked pin is therefore permanent for the life of the machine.

**Mechanism corrected 2026-07-30 (round 3) — the "re-faults and leaks again" step was wrong.** An earlier version of this paragraph said the abort's false success means "the guest re-faults and leaks again", and built a rate estimate of ~1 page per fault on that. **That does not follow.** By the time `insert_ppage()` runs, `pkvm_host_map_guest()` has already **succeeded** (`mmu.c:1851-1858`; a failure there jumps to `dec_account` and never reaches the insert), so the EL2 stage-2 mapping for that IPA is live and valid. A subsequent guest access to the same IPA therefore **does not fault at all** — merely lacking the maple-tree record changes nothing about translation. What the missing record loses is the *host's* ability to find, unpin and account that page later; it does not re-arm the fault. For the leak to *repeat*, the guest needs either a **new IPA** (each of which leaks at most its own page, once) or an intervening event that removes the EL2 mapping and lets the IPA fault again — a successful `MEM_RELINQUISH` on it, `pkvm_unmap_range()`, or unmap/remap of the memslot. Note the second-order effect, which is what makes the `-EEXIST` variant self-limiting rather than self-sustaining: once the record is gone, a *later* order-9 THP attempt over that range no longer sees an overlap in `mt_find()`, so it stops being forced down the 4 KB path.

Consequently the **rate estimate is withdrawn.** The earlier "~1 page per fault with no VM churn required, so 25 GiB is reachable in minutes" is not supported: the correct upper bound is roughly one leaked page per *distinct IPA that hits the failing insert*, plus one per relinquish/unmap/remap cycle on such an IPA. Reaching 25 GiB (≈ 6.6 M pages) that way needs on the order of millions of distinct qualifying IPAs or cycles, which is a far stronger requirement than the earlier phrasing implied and is **not** demonstrated by anything in the logs. Candidate A remains the best code-level *shape* for the leak's signature; it is no longer accompanied by a plausible magnitude.

Why it still fits the signature: these are THP-backed shmem/memfd pages (note the `PageSwapBacked` guard at `mmu.c:1799` is gated on `kvm->arch.pkvm.enabled`, so it is **skipped for non-protected VMs**). Once the executor dies and the inode leaves the page cache, an orphaned pin keeps `refcount > 0` forever: not in `NR_FILE_PAGES`, not in `NR_ANON_MAPPED`, unreclaimable and unswappable. That matches the D3000 residual's character. It does **not** by itself explain why the runaway is D3000-only.

**Not confirmed.** The `WARN_ON` at `mmu.c:1864` would have had to fire millions of times, and it appears in **no** captured log: N90's 30,167 WARN reports are 30,134 × `pgtable.c:639` plus 19 × `kvm_emulate.h:563`, 12 × `arch_timer.c:461`, 1 × `context_tracking.c:128`, 1 × `vmid.c:69` — and nothing from `mmu.c`. D3000's serial capture is only a tail excerpt, so its silence proves little. **The discriminator is therefore: recover D3000's full serial log, or re-run with a counter on `mmu.c:1864`.** If that WARN is silent there too, candidate A is dead and the next candidates are the unaccounted `__pkvm_topup_hyp_alloc_mgt()` donation channel (`init_hyp_memcache()` ⇒ `flags = 0` ⇒ no `kvm_account_pgtable_pages`, no `protected_hyp_mem` stat — the one truly untracked host→EL2 path) and failed VM teardown leaving `destroy_hyp_vm_pgt` / `drain_hyp_pool` unrun.

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

Those 40 pages (160 KiB) plus the `objects` array are then **leaked for the lifetime of the machine**: `free_hyp_memcache()` walks `while (mc->nr_pages)`, which is 0, and frees nothing. They sit on **no LRU** and are unreclaimable and unswappable — the signature recorded in `FINDING.md` §2. *(Wording corrected 2026-07-30 round 3: an earlier version said these pages "appear in **no** `/proc/meminfo` counter". That is wrong. `__get_free_page()` takes pages **out of the buddy allocator**, so they are plainly visible as a reduction in `MemFree` / zone `free` — they consume real free pages like any other allocation. What is true, and is the only property the argument needs, is that they are **not separately classified**: they belong to no slab cache, no LRU, no `NR_*` category that the OOM report or `/proc/meminfo` breaks out, so they land squarely in the unattributed residual rather than in any named line item.)*

**Magnitude — corrected 2026-07-30 (rounds 2 and 3).** An earlier version dismissed candidate B as "wrong by three orders of magnitude". **That was an arithmetic blunder.** With the round-3 residual, 25,977 MiB ÷ 160 KiB = **166,254** affected vCPUs, against `exec total=126188` for the whole fleet — a ratio of **1.32**, i.e. the *same* order of magnitude (the CMA correction moves this by 0.2 %). Since one program can create several VMs (19 × `KVM_CREATE_VM` in a single `log0` excerpt) with up to `KVM_MAX_VCPU = 4` vCPUs each, 1.32 vCPUs per execution is entirely reachable. **Candidate B cannot be excluded**; its runtime precondition (reaching the top-up while `nr_pages == 0`) is simply unverified.

**The N90 "bound" rests on an assumption I cannot source — flagged 2026-07-30 (round 3).** Candidate B leaks at most once per vCPU (after the top-up, `nobjs == 40`, so a later top-up returns early at `kvm_main.c:419`), so a per-machine execution count would turn the N90 residual into a real constraint. **`campaign/manager-overnight1.log` does not contain one.** I read all 5,524 lines: the only recurring statistics line is fleet-aggregate — `candidates=… corpus=… coverage=… exec total=… (…/min)`, ending at `exec total=126188` — and the only per-instance lines at all are 7 × `VM N: running for …, restarting`, the two Oops crash reports (`VM 1` at 03:40:20, `VM 0` at 03:41:50), one `VM 1: crash: lost connection to test machine` at 04:59:06, and scp failures against `10.42.27.18`. There are **no per-machine exec, VM or vCPU counters**, and `fleet.cfg` shows only that the two targets are `10.42.27.17` and `10.42.27.18` with `procs: 1` each.

So the earlier calculation must be labelled for what it is: **ASSUMPTION (unverified) — that roughly half the campaign's 126,188 executions ran on N90 (~63,000).** *Conditional on that assumption*, if every one had leaked 160 KiB, N90's residual would be ~9.6 GiB; it is **1,024 MiB**. That is suggestive — it would mean candidate B is not firing on every vCPU, so it is either rare or specific to whatever state D3000 entered — but it is **not a quantitative bound**, because its input is a guess. Sourcing it needs a per-instance exec counter (syzkaller's per-VM stats or the web dashboard), which was not captured.

One weak, independent hint from the N90 dmesg itself, offered with its caveat: executor comms on the `#51` boot are of the form `syz.<k>.<n>`, and the maximum `<n>` observed per `<k>` is 17,311 / 30,900 / 42,617 / 18 (sum 90,846). *If* `<n>` counted program executions, N90 alone would account for ~72 % of the fleet total — i.e. **more** than half, which would only strengthen the direction of the argument above. But I have not verified what `<n>` actually counts in this syzkaller build (executor process spawns need not be 1:1 with the manager's `exec total`), and the sum-of-maxima exceeds what a 50/50 split would predict, so this is a hint about ordering only and is **not** used as a number anywhere in this document.

Note also that `init_hyp_memcache()` memsets the union, wiping the `gfp_zero = __GFP_ZERO` set at `arm.c:474`, so these pages come back **not zeroed** — a latent hazard if they are ever used as stage-2 tables.

**Verdict on §1 vs §2:** they share a *trigger* (dirty logging on a pKVM host) and they share a *root theme* (legacy non-pKVM paths left unconverted), but on the present evidence they are **not proven to be one defect**. §1 is closed; §2 is open with candidate A as the leading hypothesis and a named discriminator.

---

## Also found while confirming the above (independent, not needed for §1)

**The hypercall-ID gate is the real cause of the `pgtable.c:639` WARN storm.** `kvm_asm.h:53-75` puts all TLB-maintenance ids *below* `__pkvm_prot_finalize` (`__kvm_tlb_flush_vmid` = 11, `__pkvm_prot_finalize` = 20), and the Rust EL2 dispatcher raises `hcall_min` to 20 once `kvm_protected_mode_initialized` is set (`hyp/nvhe/rust/src/hyp_main.rs:1627-1663`), returning `SMCCC_RET_NOT_SUPPORTED` (-1) **without calling the handler**. So post-finalization **every** `__kvm_tlb_flush_vmid*` / `__kvm_flush_vm_context` from the host is silently a no-op. Reached via `kvm_arch_flush_remote_tlbs_range()` (`mmu.c:188`), which lacks the `is_protected_kvm_enabled()` branch its sibling `kvm_arch_flush_remote_tlbs()` (`mmu.c:179`) has, and which also passes `&kvm->arch.mmu` whose `pgt`/`vmid`/`pgd_phys` are never initialised under pKVM (`kvm_init_stage2_mmu()` returns early at `mmu.c:1104`). Two independent reviews reached this conclusion. Severity: **write-protect without invalidation ⇒ dirty tracking can miss guest writes**, i.e. a migration/snapshot correctness hole, not just log noise.

**~~Guest-driven pin underflow~~ — RETRACTED 2026-07-30.** An earlier version claimed a guest could call `MEM_RELINQUISH` 512× on **one** 4 KiB IPA of an order-9 ppage and drive `ppage->pins` to 0, freeing the host folio while EL2 still mapped all 2 MiB. **EL2's up-front check refutes this.** The host function never runs twice for the same IPA, because the first success zaps the guest PTE and the second call is absorbed at EL2:

```c
/* relinquish_walker(), mem_protect.c:385 */
	if (!kvm_pte_valid(pte))
		return 0;                       /* leaves data.pa == 0 */
...
/* __pkvm_guest_relinquish_to_host(), mem_protect.c:414 — on the FIRST success: */
	ret = kvm_pgtable_stage2_annotate(&vm->pgt, ipa, PAGE_SIZE,
					  &vcpu->vcpu.arch.stage2_mc, 0);   /* zaps the PTE */
	WARN_ON(host_stage2_set_owner_locked(data.pa, PAGE_SIZE, PKVM_ID_HOST));
...
/* pkvm_memrelinquish_call(), pkvm.c:1623 */
	if (pa != 0)
		return false;                   /* exit to host -> pkvm_host_reclaim_page() */
	/* This was a NOP as no page was actually mapped at the IPA. */
	smccc_set_retval(vcpu, 0, 0, 0, 0);
	return true;                            /* handled at EL2, host never sees it */
```

So a repeat on an already-relinquished IPA yields `pa == 0`, EL2 returns a NOP to the guest, and `pkvm_host_reclaim_page()` — hence the `pins--` — is never reached. EL2 also validates the page *state* per VM type (`PKVM_PAGE_OWNED` for protected, `PKVM_PAGE_SHARED_BORROWED` otherwise) and intercepts the call for **both** VM types (`pkvm.c:1746` and `pkvm.c:1777`).

**The suggested `kvm_vm_is_protected()` gate is itself WRONG and is deleted, not downgraded — 2026-07-30 (round 3).** Round 2 kept a survivor: "the missing `kvm_vm_is_protected()` gate at `hypercalls.c:371` is a consistency wart / defence-in-depth gap", on the grounds that its `MEM_SHARE`/`MEM_UNSHARE` neighbours at `:375-378` carry `if (!kvm_vm_is_protected(vcpu->kvm)) break;` and relinquish does not. **That reasoning was backwards, and adding the gate would break a legitimate protocol.** EL2 explicitly supports `MEM_RELINQUISH` for **non-protected** guests, in two independent places:

```c
/* __pkvm_guest_relinquish_to_host(), hyp/nvhe/mem_protect.c:429-432
 * — the expected page state is SELECTED BY VM TYPE, not used to reject one: */
	/* Expected page state depends on VM type. */
	data.expected_state = pkvm_hyp_vcpu_is_protected(vcpu) ?
		PKVM_PAGE_OWNED :
		PKVM_PAGE_SHARED_BORROWED;

/* hyp/nvhe/pkvm.c — dispatched from BOTH switch sites: */
	case ARM_SMCCC_VENDOR_HYP_KVM_MEM_RELINQUISH_FUNC_ID:      /* :1746, protected-VM handler   */
		return pkvm_memrelinquish_call(hyp_vcpu, exit_code);
	...
	case ARM_SMCCC_VENDOR_HYP_KVM_MEM_RELINQUISH_FUNC_ID:      /* :1777, kvm_hyp_handle_hvc64() */
		return pkvm_memrelinquish_call(hyp_vcpu, exit_code);   /* the NON-protected VM path     */
```

`pkvm.c:1767` is literally commented *"Handler for non-protected VM HVC calls"*, and `MEM_RELINQUISH` is one of only two functions it forwards (the other being `HYP_MEMINFO`). A non-protected guest that relinquishes a `PKVM_PAGE_SHARED_BORROWED` page therefore returns `pa != 0` to `pkvm_memrelinquish_call()`, which returns `false` so the host completes the operation at `hypercalls.c:371-373` → `pkvm_host_reclaim_page()`. Gating that on `kvm_vm_is_protected()` would silently drop the host half of a successful EL2 relinquish for every non-protected VM: the guest PTE is already zapped and ownership already returned to the host, but the `ppage` would keep its pin and its maple-tree entry forever. **That converts a working path into a guaranteed leak.** The asymmetry with `MEM_SHARE`/`MEM_UNSHARE` is not a wart either — those two cases do nothing but bump the `protected_shared_mem` statistic, so gating them on protectedness is exactly right; relinquish does real work.

**What genuinely needs fixing here instead — three items.** Verified against `/home/jose/ksrc-pkvmfix` (branch `review/8707`), which already carries the first two as cherry-picks `3ae13572d106` and `9a13ca20af8d`:

1. **Concurrent use-after-free: `pkvm_host_reclaim_page()` reads `ppage->pins` after `write_unlock`.** Current code (`arch/arm64/kvm/pkvm.c:536-563`) drops the lock at `:554`, then re-reads `ppage->pins` at `:557` and `ppage->page` at `:561`. Between those, a second vCPU relinquishing another subpage of the same ppage can take the lock, decrement `pins` to 0, `mtree_erase()` and `kfree(ppage)` — after which the first thread is reading freed memory, and both threads can reach the `unpin`/`kfree` path. The `ppage` content must be snapshotted *inside* the critical section. Upstream fix: **`63a99c1fb8e5`** "ANDROID: KVM: arm64: Fix THPs reclaim with ballooning" (Vincent Donnefort, 2025-03-05), whose commit message states the invariant directly — *"A ppage content can't be read without either that ppage out of the pinned_page tree or without the mmu_lock taken"* — and which introduces a local `u16 pins` copied under the lock.
2. **order>0 / THP paths un-account only ONE page.** Two distinct sites: `pkvm_host_reclaim_page()` calls `account_locked_vm(mm, 1, false)` at `pkvm.c:560` even when the ppage is order-9, so 2 MiB of `RLIMIT_MEMLOCK` is released 4 KiB at a time; and `pkvm_mem_abort()`'s THP→4 KiB retry at `mmu.c:1846-1847` sets `page_size = PAGE_SIZE` **before** the compensating `account_locked_vm(mm, page_size >> PAGE_SHIFT, false)`, so it gives back one page instead of the 512 it had just charged. Upstream fixes: `63a99c1fb8e5` again (`account_locked_vm(mm, 1 << ppage->order, false)`) for the first, and **`ed14b491ec76`** "ANDROID: KVM: arm64: Fix accounting of pinned THPs with pKVM" (Quentin Perret, 2024-10-08) for the second — a two-line reordering. Note this is an `RLIMIT_MEMLOCK` accounting bug, not itself a memory leak.
3. **Both fixes are already in the local object store and already cherry-picked.** `git log -1 63a99c1fb8e5` and `git log -1 ed14b491ec76` both resolve in `/home/jose/common-stage2mvp`; neither is in `HEAD` of `2030/bug930`. The clean sibling checkout `/home/jose/ksrc-pkvmfix` on `review/8707` has them as `3ae13572d106` ("ANDROID: KVM: arm64: Fix THPs reclaim with ballooning") and `9a13ca20af8d` ("ANDROID: KVM: arm64: Fix accounting of pinned THPs with pKVM"). So (1) and part of (2) need no local authorship — they need landing.

What remains genuinely open after those three: the `mt_find()` **granularity** question. `pkvm_host_reclaim_page()` does a `PAGE_SIZE`-granularity `mt_find()` that matches the whole order-9 range and decrements `pins` by 1, while EL2 zaps `PAGE_SIZE` at a time, so the intended 512-distinct-subpage path is correct by construction — but `mtree_erase(&…pinned_pages, ipa)` at `pkvm.c:552` erases by the *faulting* IPA rather than by `ppage->ipa`, and the interleaving of EL2's per-page zap with the host's `mt_find`/`pins--`/`mtree_erase` under concurrent vCPUs has not been audited even with `63a99c1fb8e5` applied. Treat as open, not as a finding.

**The new EL2-coverage code is NOT implicated in this crash** — confirmed by measurement, not inspection: `struct kvm_vcpu` / `kvm_vcpu_arch` / `kvm_mmu_memory_cache` DWARF is byte-identical across host C (`vmlinux`), EL2 C (`kvm_nvhe.o`) and the Rust bindgen view (`nvhe_rust.o`), so the layout-skew theory is dead; and `KVM_PKVM_COV_HVC` leaves the SMCCC arg list, register selection and `&res` store untouched, so the WARN is a genuine EL2 rejection, not an ABI artifact. It does carry a real documentation-vs-code defect, though the exploitable consequence is **build-dependent and unproven here**.

**The stated locking contract is void — this part is fact.** `pkvm_cov.c:77-82` and `:382-390` both justify their safety by asserting that `pkvm_cov_begin()`/`pkvm_cov_end()` "run under `kvm->mmu_lock` (preemption off on a non-RT kernel = an RCU read-side section)", and `pkvm_cov_begin()` passes exactly that as its lockdep condition: `rcu_dereference_check(pkvm_cov_host_ring, !preemptible())` (`:394-395`). That call site **no longer exists**. The pair is now invoked from the blanket HVC wrapper `KVM_PKVM_COV_HVC` (`asm/kvm_host.h:1109-1125`, twinned at `asm/kvm_pkvm.h:659-676`), which wraps *every* `kvm_call_hyp_nvhe()` / `kvm_call_hyp()` / `kvm_call_hyp_ret()` on the nVHE path — most of which are nowhere near `kvm->mmu_lock`. So the "preemption is off" premise is false in general, and with it two derived claims: that `smp_processor_id()` in the owner-CPU guard (`:400`) is stable, and the `kvm_pkvm_cov.h:55-62` argument that "the drain only ever runs on the buffer's owner CPU … so producer and consumer are the same PE" (which is what stands in for a missing release on `ring->nested`).

**But a use-after-free in the CURRENT build is NOT proven — corrected 2026-07-30 (round 3).** An earlier version asserted that "with `PREEMPT_VOLUNTARY` + `TREE_RCU` a preemptible context is a quiescent state, so `synchronize_rcu()` in `pkvm_cov_disable()` does not wait for an in-flight pair and `free_pages()` can land mid-drain". **That is wrong about classic RCU.** Checked against the archived config (`build/kernel.config`): `CONFIG_PREEMPT_VOLUNTARY=y`, `# CONFIG_PREEMPT is not set`, `# CONFIG_PREEMPT_DYNAMIC is not set`, `CONFIG_TREE_RCU=y` — and **no `CONFIG_PREEMPTION`, no `CONFIG_PREEMPT_RCU`** anywhere in the file. Under classic (non-preemptible) TREE_RCU, merely *being* in preemptible kernel context is **not** a quiescent state; the quiescent states are a context switch, going idle, or transitioning to user mode. And `KVM_PKVM_COV_HVC` expands to `begin(); arm_smccc_1_1_hvc(...); if (on) end();` — one HVC, no allocation, no `might_sleep()`, no `cond_resched()`, no scheduling point of any kind. So on this build `synchronize_rcu()` cannot complete inside the region, the region is an implicit RCU read-side critical section after all, and for the same reason the task cannot migrate between `begin()` and `end()` either. Neither the lifetime race nor the CPU-owner race is reachable in the kernel that produced these crashes. The honest statement is: **the locking contract documented in the code is void, and on a `CONFIG_PREEMPT=y` build (or `PREEMPT_DYNAMIC` resolved to full, or PREEMPT_RT) there is a real lifetime / CPU-owner race** — the ring can be freed under an in-flight drain, and the task can be migrated off the owner CPU mid-window. On the current `PREEMPT_VOLUNTARY` build it is a latent defect that happens to be masked by the preemption model, not an active bug.

**Fix advice, corrected.** The earlier "wrap begin→HVC→end in `rcu_read_lock()` / `preempt_disable()`" was loose in a way that matters: the guard must (a) span the **whole** `begin → HVC → end` region — not just `begin()`, since the ring pointer must stay valid until `end()` finishes draining — and (b) **prevent CPU migration**, because the owner-CPU guard and the same-PE publication argument both depend on it. `rcu_read_lock()` alone does **not** satisfy (b): under `PREEMPT_RCU` it neither disables preemption nor pins the CPU, so the task can be migrated while still inside the read-side section. Use `preempt_disable()` (or `migrate_disable()` plus an explicit `rcu_read_lock()`) around the entire region, and drop the `!preemptible()` lockdep condition in favour of a real `rcu_read_lock_held()`-style check so the contract is actually enforced rather than asserted. Also: bindgen runs with `--no-layout-tests`, removing the one automatic guard against the very skew that was ruled out here.

### The 12 × `arch_timer.c:461` WARNs are a SEPARATE, UNANALYSED issue (new 2026-07-30, round 3)

`n90-dmesg-history.log.gz` contains 12 instances of `WARNING: … at arch/arm64/kvm/arch_timer.c:461 kvm_timer_update_irq+0x1c8/0x1d8`. They have been listed in this document only as background noise alongside the TLB storm. They are **not** part of the TLB finding, and there is **no evidence whatsoever that they are caused by the HVC id-11 rejection.** Recorded here as an observation in its own right, unanalysed.

Line 461 is the error check on the interrupt injection, not anything TLB-related (`arch/arm64/kvm/arch_timer.c:447-463`):

```c
static void kvm_timer_update_irq(struct kvm_vcpu *vcpu, bool new_level,
				 struct arch_timer_context *timer_ctx)
{
	int ret;

	timer_ctx->irq.level = new_level;
	trace_kvm_timer_update_irq(...);

	if (!userspace_irqchip(vcpu->kvm)) {
		ret = kvm_vgic_inject_irq(vcpu->kvm, vcpu->vcpu_id,
					  timer_irq(timer_ctx),
					  timer_ctx->irq.level,
					  timer_ctx);
		WARN_ON(ret);                    /* :461 */
	}
}
```

So the WARN says exactly one thing: **`kvm_vgic_inject_irq()` returned an error** for an in-kernel irqchip. What the log adds:

| | observation |
|---|---|
| count | 12, in 6 pairs of 2 (same PID each time, 1 ms–300 ms apart — consistent with the vtimer and ptimer contexts being updated in turn) |
| build split | **10 on `6.6.30+ #47`, only 2 on `6.6.30-covfix #51`** — i.e. mostly *not* the build under investigation |
| timestamps | `[9526.97]`, `[9527.27]`, `[9630.56]`, `[9630.86]`, `[10322.05]`, `[10322.35]`, `[10323.96]`, `[10324.26]`, `[10334.05]`, `[10334.35]` (`#47`); `[33133.203]`, `[33133.204]` (`#51`) |
| call path (identical in all) | `kvm_timer_update_irq ← kvm_timer_vcpu_reset ← kvm_reset_vcpu ← kvm_arch_vcpu_ioctl ← kvm_vcpu_ioctl ← __arm64_sys_ioctl` |
| registers | `x19 = 0x1e` = 30 = the physical-timer PPI number, i.e. `timer_irq(timer_ctx)` — **not** the return value, so the errno is not recoverable from the dump |

Why this is clearly a different animal from the TLB WARN: it arrives on the **vCPU-reset ioctl path**, never through `kvm_vm_ioctl`/memslot commit; it does not involve any hypercall, so it cannot be downstream of `hcall_min`; it fired **five times more often on `#47` than on `#51`**, the opposite of the TLB WARN's distribution; and 12 occurrences against 27,674 is not a rate that suggests a shared trigger.

The obvious hypothesis — that the fuzzer reached `KVM_ARM_VCPU_INIT`/`KVM_RUN` before `KVM_CREATE_IRQCHIP`, so the vGIC is not yet initialised and `kvm_vgic_inject_irq()` rejects the injection — is **plausible but unverified**; the return value is not in the dump and no vgic-state trace was captured. `ioctl$KVM_CREATE_IRQCHIP`, `ioctl$KVM_CREATE_DEVICE` and `ioctl$KVM_ARM_VCPU_INIT` are all in the enabled-syscall set (`campaign/fleet.cfg`), so arbitrary orderings are reachable by construction. **Discriminator if anyone wants to close it:** print `ret` (or run with a temporary `WARN(ret, "ret=%d", ret)`) and correlate against `vgic_initialized(kvm)`. Until then this is an open, low-priority observation with no established connection to §1, §2, or the TLB finding.

---

## What to fix (not yet implemented)

1. **The union is only safe if exactly one view is ever live per vCPU.** The fix is to stop `user_mem_abort()`'s prologue from topping up `mmu_page_cache` when pKVM owns the union — **and nothing more than that**.

   *Corrected 2026-07-30.* An earlier version of this item said `user_mem_abort()` "must not be reached at all" under pKVM. **That was wrong.** Entering it on a permission fault is deliberate, and the code asserts it (`mmu.c:2138-2145`):

   ```c
   	if (is_protected_kvm_enabled()) {
   		if (WARN_ON(fault_status != ESR_ELx_FSC_PERM))
   			ret = -EINVAL;
   		else
   			ret = pkvm_relax_perms(vcpu, pfn, gfn, get_order(fault_granule),
   					       prot, logging_active);
   		goto mark_dirty;
   	}
   ```

   The `WARN_ON` fires only if pKVM arrives with a *non*-permission fault, i.e. the design intends exactly the entry we observe, and `pkvm_relax_perms()` correctly uses `stage2_mc`. Only the legacy prologue top-up is the defect. Upstream agrees: `fce886a60207` keeps `user_mem_abort()` on the pKVM path and changes only the memcache selection. Splitting the union (what mainline did) remains the more robust variant; the 32 bytes per vCPU are not worth this class of bug.
2. `kvm_arch_flush_remote_tlbs_range()` must mirror `kvm_arch_flush_remote_tlbs()`: under pKVM degrade to the **handle-based full flush**, `kvm_call_hyp_nvhe(__pkvm_tlb_flush_vmid, kvm->arch.pkvm.handle)` — which is what upstream `fce886a60207` does. Returning non-zero so the generic full-flush fallback engages is an acceptable alternative.

   *Corrected 2026-07-30.* An earlier version offered "alternatively move the TLB ids above `__pkvm_prot_finalize` in `kvm_asm.h`". **Do not do this — it is a deliberate weakening of the pKVM threat model.** `kvm_asm.h:53` marks the boundary explicitly (`/* Hypercalls available only prior to pKVM finalisation */`) and the two handlers differ in exactly the way that matters:

   ```c
   /* id 11 — pre-finalisation only: takes a RAW HOST POINTER and derefs it at EL2 */
   static void handle___kvm_tlb_flush_vmid(struct kvm_cpu_context *host_ctxt)
   {
   	DECLARE_REG(struct kvm_s2_mmu *, mmu, host_ctxt, 1);
   	__kvm_tlb_flush_vmid(kern_hyp_va(mmu));
   }

   /* id 28 — post-finalisation: takes an OPAQUE HANDLE, validated against EL2's own table */
   static void handle___pkvm_tlb_flush_vmid(struct kvm_cpu_context *host_ctxt)
   {
   	DECLARE_REG(pkvm_handle_t, handle, host_ctxt, 1);
   	vm = pkvm_get_hyp_vm(handle);
   	if (!vm) return;
   	if (!pkvm_hyp_vm_is_protected(vm))
   		__kvm_tlb_flush_vmid(&vm->kvm.arch.mmu);
   	pkvm_put_hyp_vm(vm);
   }
   ```

   (Rust twins at `hyp_main.rs:551-579` are identical in shape.) After finalisation the host is untrusted; re-exposing id 11 would let it make EL2 `kern_hyp_va()` and dereference an arbitrary pointer. The `hcall_min` gate is doing its job — the bug is that the host still *calls* the pre-finalisation ABI, not that the gate exists.
3. `mmu.c:1864` must not swallow `insert_ppage()` failure: unwind the EL2 mapping, `unpin_user_pages()`, `kfree(ppage)` and return an error — never 0. Move the overlap `mt_find()` out of the `page_size > PAGE_SIZE` condition and preallocate the maple node outside `write_lock(&kvm->mmu_lock)`. *(Note added 2026-07-30 round 3: upstream `9cfc94644ba4` already implements exactly this, and more cleanly — it reserves the range with a `KVM_DUMMY_PPAGE` sentinel **before** `pkvm_host_map_guest()`, so there is no mapping to unwind on either the `-ENOMEM` or the `-EEXIST` path, and the post-map store is an allocation-free `mtree_store_range()`. Prefer taking that commit over writing this by hand.)*
4. `kvm_arch_vcpu_destroy()`'s predicate should match whichever view was actually used, and `fpsimd.c:54`'s `BUG_ON(!current->mm)` should be demoted — any task dying with a loaded vCPU hits it.
5. **~~Add a `kvm_vm_is_protected()` gate to `MEM_RELINQUISH` at `hypercalls.c:371`~~ — DELETED 2026-07-30 (round 3). Do not do this.** Earlier versions of this item proposed the gate, then downgraded it to "defence-in-depth only". Both were wrong: **EL2 deliberately supports `MEM_RELINQUISH` for non-protected guests** (`__pkvm_guest_relinquish_to_host()` selects `data.expected_state` by VM type at `hyp/nvhe/mem_protect.c:429-432`, and `hyp/nvhe/pkvm.c` dispatches the call from *both* switch sites, `:1746` for protected VMs and `:1777` inside `kvm_hyp_handle_hvc64()`, the handler explicitly commented "for non-protected VM HVC calls"). Adding the gate would drop the host half of a successful EL2 relinquish for every non-protected VM and turn a working protocol into a guaranteed pin-and-record leak. See the retraction section above for the full argument. What actually needs doing at this call site:

   a. **Fix the concurrent use-after-free in `pkvm_host_reclaim_page()`** (`pkvm.c:536-563`): `ppage->pins` is re-read at `:557` and `ppage->page` at `:561`, *after* `write_unlock(&host_kvm->mmu_lock)` at `:554`, so a concurrent vCPU can decrement to zero, `mtree_erase()` and `kfree(ppage)` in between. Snapshot the fields under the lock. Already fixed upstream by **`63a99c1fb8e5`** ("ANDROID: KVM: arm64: Fix THPs reclaim with ballooning"), which is present in our object store and cherry-picked as `3ae13572d106` on `/home/jose/ksrc-pkvmfix` `review/8707`.
   b. **Fix the order>0 / THP un-accounting, which releases only ONE page.** `pkvm_host_reclaim_page()` calls `account_locked_vm(mm, 1, false)` at `pkvm.c:560` regardless of `ppage->order`; and `pkvm_mem_abort()`'s THP→4 KiB retry at `mmu.c:1846-1847` clobbers `page_size` *before* the compensating `account_locked_vm(..., false)`, so it returns 1 page instead of 512. Fixed upstream by `63a99c1fb8e5` (first site) and **`ed14b491ec76`** ("ANDROID: KVM: arm64: Fix accounting of pinned THPs with pKVM", cherry-picked as `9a13ca20af8d`) for the second. This is an `RLIMIT_MEMLOCK` accounting defect, not a leak of memory.
   c. **Note:** (a) and part of (b) therefore need **no local authorship at all** — both upstream commits already exist in the local object store and are staged on the sibling branch; they need landing on `2030/bug930`. What remains genuinely local and open is the **`mt_find` granularity / concurrency audit**: `pkvm_host_reclaim_page()` matches a whole order-9 range with a `PAGE_SIZE` `mt_find()` and erases with `mtree_erase(…, ipa)` — the faulting IPA, not `ppage->ipa` — while EL2 zaps one page at a time. Open, not a finding.

Fixes 1 and 2 are independent; either alone leaves a real bug.

---

## Independent verification log (2026-07-30)

**Scope — corrected 2026-07-30 (round 3).** An earlier version of this section opened with "Every load-bearing claim above was re-checked against the sources rather than accepted. All hold", and closed with "Nothing was found that contradicts this document." **Read literally, that contradicts the status table at the top of this file**, which marks the OOM residual as OPEN, leak candidates A and B as unconfirmed, and several claims as RETRACTED or withdrawn. The table below verifies **the CONFIRMED part only** — §1, the union type confusion and the `pgtable.c:639` / hypercall-ID mechanism — plus the specific factual sub-claims listed. Everything it says "✔" to holds. It says nothing about the open §2 leak, and rounds 2 and 3 did in fact find claims of mine that were wrong; those are recorded inline at each site and summarised in the status table, not here.

| claim (CONFIRMED-scope only) | check | result |
|---|---|---|
| union `mmu_page_cache` / `stage2_mc`, alias at +8 | read `kvm_host.h:678`, `kvm_types.h:93`, `kvm_host.h:86` | ✔ `kmem_cache` and `nr_pages` both at +8 |
| `x19` is `kmem_cache_alloc` arg1 | disassembled archived `vmlinux`: `+0x18 mov x19,x0`; fault at `+0x50` | ✔ matches `pc : kmem_cache_alloc+0x50` |
| register decode, both crashes | re-read reports; `x22 == x20 & 0xfff` twice; `head` page-aligned twice | ✔ (see refinement above) |
| dirty logging set unconditionally | `executor/common_kvm_arm64.h:117` — `KVM_MEM_LOG_DIRTY_PAGES` on `ARM64_ADDR_DIRTY_PAGES` | ✔ closes the gap `ROOT-CAUSE-ANALYSIS.md` left open |
| fault-type dispatch reaches both members | `mmu.c:2322-2325` | ✔ predicate is `is_protected_kvm_enabled() && fault_status != PERM` |
| N90 hit the identical crash twice | `zcat n90-dmesg-history.log.gz` | ✔ `[33369]` and `[34182]`, same Oops + `fpsimd.c:54` + recursive fault |
| all `B` taints belong to `#47`, none to `#51` | same log | ✔ 656 total, **0** on `6.6.30-covfix` |
| `flush_remote_tlbs_range` lacks the pKVM branch | `mmu.c:179` vs `mmu.c:188` | ✔ sibling has `is_protected_kvm_enabled()`, range version does not, and returns 0 |
| `MEM_RELINQUISH` ungated at `hypercalls.c:371` | `:371-373` vs the adjacent `MEM_SHARE`/`MEM_UNSHARE` at `:375-378` | ✔ the *fact* holds (neighbours carry `if (!kvm_vm_is_protected(...)) break;`, relinquish does not) — but the **inference drawn from it was wrong**: EL2 supports this call for non-protected guests, so the absence of the gate is correct, not a wart. See round 3 |
| EL2 supports `MEM_RELINQUISH` for non-protected VMs | `hyp/nvhe/mem_protect.c:429-432`; `hyp/nvhe/pkvm.c:1746` and `:1777` | ✔ `expected_state` selected by VM type; dispatched from both the protected and the `kvm_hyp_handle_hvc64()` non-protected switch (round 3) |
| `!system_supports_tlb_range()` is the branch taken | `hyp/pgtable.c:633-641` + the pc in every dump | ✔ line 639 is inside the no-FEAT_TLBIRANGE early-out; one HVC ⇒ one WARN (round 3) |
| WARN counts split by build | scripted over `n90-dmesg-history.log.gz` | ✔ 30,134 total = 27,674 (`#51`) + 2,460 (`#47`); `x21 == -1` in 27,674/27,674; one backtrace signature (round 3) |
| bytes per WARN report | measured over all 30,134 `cut here`→`end trace` blocks | ✔ median **3,297 B** / 36 lines — the earlier "~9 KiB" was unmeasured and ~2.7× too large (round 3) |
| zone `free` already includes free CMA | OOM report: `free_cma:9754` pages = 39,016 kB reappears inside `Node 0 DMA free:122620kB` | ✔ so adding `cma_free` to the accounted sum double-counted (round 3) |
| no `CONFIG_PREEMPTION` / `PREEMPT_RCU` in this build | `build/kernel.config` | ✔ `PREEMPT_VOLUNTARY=y`, `# CONFIG_PREEMPT is not set`, `TREE_RCU=y`; no `PREEMPTION`/`PREEMPT_RCU` line exists (round 3) |
| manager log has no per-machine exec counts | all 5,524 lines of `campaign/manager-overnight1.log` | ✔ fleet-aggregate `exec total` only; per-instance lines are restarts and crashes (round 3) |
| `arch_timer.c:461` is `WARN_ON(kvm_vgic_inject_irq(...))` | `arch/arm64/kvm/arch_timer.c:447-463` | ✔ and the 12 instances all arrive via `kvm_reset_vcpu`, unrelated to any hypercall (round 3) |
| CVE-2025-37996 affected range | NVD API record for `CVE-2025-37996` | ✔ upstream **6.14 → 6.14.6** and **6.15-rc1 … 6.15-rc5**; the "v6.12–v6.15-rc4" in earlier drafts was wrong on both ends (round 3) |

The `ROOT-CAUSE-ANALYSIS.md` hypothesis this document refutes — guest writes through stale stage-2 TLB entries — is correctly dead, and §1's register-exact decode is sufficient for that on its own. The N90 negative control (identical crash, 27,674 TLB WARNs, no runaway, 5.3 h later still healthy) corroborates it, but note the scoping above: that control shows the Oops and the TLB WARN are not *sufficient* to produce the leak, which is not the same as showing they cost nothing.

---

## Upstream status — all four are already fixed upstream, and we are behind

Searched the full Android Common object store with `git log --all --source` (4017 tags; a branch-only search finds none of this). Our base is **`ASB-2024-06-05_15-6.6`** (June 2024) and every fix below landed *after* it.

| Our finding | Upstream fix | Author / date | Lands in | In our HEAD? |
|---|---|---|---|---|
| **#1** union `{mmu_page_cache, stage2_mc}` type confusion | union **deleted** — two separate fields `mmu_page_cache` + `pkvm_memcache` (`d0bd3e6570ae`), and `user_mem_abort()` given an `is_protected_kvm_enabled()` memcache dispatch (`fce886a60207`) | Quentin Perret, 2024-12-18 | **mainline v6.14**; Android 6.12 as `73a4f4ffe584 BACKPORT: FROMGIT` | **NO** |
| **#2** `kvm_arch_flush_remote_tlbs_range()` missing pKVM branch | same commit `fce886a60207` | Quentin Perret, 2024-12-18 | mainline v6.14 / Android 6.12 | **NO** |
| **leak candidate A** — `insert_ppage()` → `mtree_insert_range()` doing `GFP_KERNEL` under `write_lock(&kvm->mmu_lock)`, **and** the swallowed failure at `mmu.c:1864` | `9cfc94644ba4` "ANDROID: KVM: arm64: Pre-alloctate mtree nodes in pkvm_mem_abort()" — reserves a `KVM_DUMMY_PPAGE` range **before** the EL2 map, so both `-ENOMEM` and `-EEXIST` are handled before anything is mapped, and the post-map insert becomes an allocation-free `mtree_store_range()` | Quentin Perret, 2024-10-08 | **`ASB-2024-12-05_15-6.6` … `ASB-2025-04-05_15-6.6` — our own 6.6 lineage** | **NO** |
| **`MEM_RELINQUISH` host path** — `ppage->pins` read after `write_unlock` (UAF), and order>0 un-accounting of 1 page | `63a99c1fb8e5` "ANDROID: KVM: arm64: Fix THPs reclaim with ballooning" + `ed14b491ec76` "ANDROID: KVM: arm64: Fix accounting of pinned THPs with pKVM" | Vincent Donnefort 2025-03-05 / Quentin Perret 2024-10-08 | both in our object store; cherry-picked as `3ae13572d106` / `9a13ca20af8d` on `ksrc-pkvmfix` `review/8707` | **NO** (staged, not landed) |

### #2 is a datable merge-miss regression, not a design choice

- `ad6e033c0464` "ANDROID: KVM: arm64: Introduce __pkvm_tlb_flush_vmid()" (Quentin Perret, **2022-07-07**, present in our base tag, **applied**) added the pKVM branch to `kvm_arch_flush_remote_tlbs()`. The range variant did not exist yet.
- `c42b6f0b1cde` "KVM: arm64: Implement kvm_arch_flush_remote_tlbs_range()" (Raghavendra Rao Ananta, **2023-08-11**, mainline) added it **13 months later**, with no pKVM branch — mainline had no EL2-managed guest stage-2 until 6.14.
- Android's rebase onto 6.6 pulled the new function in without the pKVM awareness its sibling already had, and nothing complained because it returns 0 unconditionally.

The upstream fix is byte-for-byte what this document independently proposed:

```c
 int kvm_arch_flush_remote_tlbs_range(struct kvm *kvm, gfn_t gfn, u64 nr_pages)
 {
+	u64 size = nr_pages << PAGE_SHIFT;
+	u64 addr = gfn << PAGE_SHIFT;
+
+	if (is_protected_kvm_enabled())
+		kvm_call_hyp_nvhe(__pkvm_tlb_flush_vmid, kvm->arch.pkvm.handle);
+	else
+		kvm_tlb_flush_vmid_range(&kvm->arch.mmu, addr, size);
 	return 0;
 }
```

### The union was Android-only; mainline never had it

`33204376bf09` (Vincent Donnefort, 2024-02-26) only *renamed* `pkvm_memcache` → `stage2_mc` **inside an already-existing union**; the union itself dates to the out-of-tree pKVM guest work of the 5.15 era (`2570e98d667b` "Add __pkvm_host_share_guest hypercall", ASB-2022-02-05). When Quentin upstreamed pKVM guest support in 6.14 he used **two separate fields**. Mainline is therefore structurally immune to #1, which only ever existed in the Android out-of-tree lineage — where we live.

### CVE-2025-37996 — the sequel, and a hard backport dependency

**CVE-2025-37996 = "KVM: arm64: Fix uninitialized memcache pointer in user_mem_abort()"**, fixed by `a26d50f8a4a5` (Sebastian Ott) and `157dbc4a321f`, `Fixes: fce886a60207`.

**Affected range — corrected 2026-07-30 (round 3).** An earlier version wrote "affecting **v6.12 – v6.15-rc4**". **That was wrong at both ends.** Authoritative source: <https://nvd.nist.gov/vuln/detail/cve-2025-37996> (fetched via the NVD API, which lists the CPE match set verbatim). For the **upstream** kernel the affected configurations are exactly:

| CPE entry | range |
|---|---|
| `cpe:2.3:o:linux:linux_kernel:*` | **from (including) 6.14, up to (excluding) 6.14.7** — i.e. 6.14 through 6.14.6 |
| `cpe:2.3:o:linux:linux_kernel:6.15:rc1` … `:rc5` | **6.15-rc1, rc2, rc3, rc4 and rc5** |

Both references are `git.kernel.org/stable/c/157dbc4a321f5bb6f8b6c724d12ba720a90f1a7c` and `.../a26d50f8a4a5049e956984797b5d0dedea4bbb18`. Nothing before 6.14 is in the upstream range, for the simple reason that the introducing commit `fce886a60207` first appears in **mainline v6.14** (`git tag --contains fce886a60207` in our object store resolves to `android-mainline-6.14` and `ASB-2025-04-05_mainline`, and nothing older).

**Android 6.12 is a separate, non-upstream case, and must be described as such.** It is **not** part of the upstream affected range and "v6.12" is not an upstream lower bound. What is true is that the Android 6.12 branch **backported the introducing commit** — `73a4f4ffe584 "BACKPORT: FROMGIT: KVM: arm64: Plumb the pKVM MMU in KVM"` (Quentin Perret, 2024-12-18), the same change as `fce886a60207` — so that branch inherits the defect on a base version that the CVE's upstream CPE list does not cover, and needs the two fixes on its own account. That is a property of the backport branch, not of upstream 6.12.

*(One divergence from the review that prompted this correction, recorded for honesty: the reviewer gave the range as "6.14 through 6.14.6 and 6.15-rc1 through 6.15-**rc4**". The first half matches NVD exactly; the second is off by one — NVD lists **rc1 through rc5**, five release candidates. The table above uses NVD's list.)*

It is **not our bug — it is the bug introduced by the fix for our bug**, on the same few lines. `fce886a60207` deleted the very declaration our bug lives on and moved the selection inside the guard:

```c
-	struct kvm_mmu_memory_cache *memcache = &vcpu->arch.mmu_page_cache;
+	void *memcache;                       /* now uninitialized on entry */
...
 	if (!fault_is_perm || (logging_active && write_fault)) {
+		if (!is_protected_kvm_enabled())
+			memcache = &vcpu->arch.mmu_page_cache;
+		else
+			memcache = &vcpu->arch.pkvm_memcache;
 		...
 	}
...
 	ret = KVM_PGT_FN(kvm_pgtable_stage2_map)(pgt, fault_ipa, vma_pagesize,
 				 __pfn_to_phys(pfn), prot, memcache, flags);
```

When the guard is **false**, that uninitialized stack pointer reaches the stage-2 map walker as its page-table allocator. The CVE fix hoists the selection above the permission-fault check so it is always valid.

**The two defects fire on opposite branches of the very same `if`:** ours needs `fault_is_perm && logging_active && write_fault` (guard **true**); the CVE needs a stage-2 allocation arriving *without* a permission fault or dirty logging (guard **false**).

Consequences for us:

- We are **not exposed** to CVE-2025-37996 — `fce886a60207` is not applied and we are on 6.6.
- But **backporting `fce886a60207` / `73a4f4ffe584` without `a26d50f8a4a5` + `157dbc4a321f` would trade our union bug for the CVE.** The ordering is mandatory.
- That upstream needed a CVE-grade follow-up on these exact lines is independent evidence this is genuinely hazardous code, not a local quirk.

### Recommended order if we go the backport route

1. `9cfc94644ba4` — same 6.6 lineage, self-contained, and it removes **both** of leak candidate A's failure preconditions, not just `-ENOMEM`. Cheapest and lowest risk; worth taking regardless of the rest.
2. `63a99c1fb8e5` + `ed14b491ec76` — also same lineage, also self-contained, and already cherry-picked on `ksrc-pkvmfix` `review/8707` as `3ae13572d106` / `9a13ca20af8d`. These are the real `MEM_RELINQUISH`-path fixes (concurrent `ppage->pins` UAF; order>0 `RLIMIT_MEMLOCK` un-accounting). Take them with (1).
3. Then either a minimal local `KYLIN:` fix for #1 and #2 (de-union plus the `kvm_arch_flush_remote_tlbs_range()` hunk above), **or** the full `fce886a60207` + `a26d50f8a4a5` + `157dbc4a321f` chain. The full chain drags in the entire pKVM-MMU abstraction, which on this tree also means porting it into the Rust EL2 — expensive. A minimal local fix citing these upstream commits is likely the right call for 6.6.
4. *(Corrected 2026-07-30, round 3.)* An earlier version of this list said "the `mmu.c:1864` error path has no upstream equivalent beyond `9cfc94644ba4`'s removal of the `-ENOMEM` case; **the `-EEXIST` case stays local**." **That is wrong — delete it.** `9cfc94644ba4` restructures the function so that the maple-tree range is **reserved before** anything is mapped: `ret = mtree_insert_range(mt, index, end, KVM_DUMMY_PPAGE, GFP_KERNEL);` runs *outside* `write_lock(&kvm->mmu_lock)` and *before* `pkvm_host_map_guest()`, and on failure it falls straight through to `dec_account:` → `account_locked_vm(..., false)` → `unpin:` → `unpin_user_pages(&page, 1)` → `free_ppage:` → `kfree(ppage)`. `-EEXIST` is explicitly folded into that exit (`if (ret == -EEXIST) ret = 0;`). So **both** the `-ENOMEM` and the `-EEXIST` cases now exit after full pin-and-account cleanup, with nothing mapped and nothing pinned — there is no residual local work for either. The only insert left after the map is `WARN_ON(mtree_store_range(mt, index, end, ppage, GFP_ATOMIC))` into a slot that is already reserved, which cannot fail for lack of a node. Separately: **`MEM_RELINQUISH` gating must not be filed at all** — not as a security fix and not as a consistency cleanup — because the gate would break non-protected guests (see the retraction above). File items (1) and (2) instead.
