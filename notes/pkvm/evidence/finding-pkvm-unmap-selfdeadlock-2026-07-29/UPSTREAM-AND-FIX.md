# pkvm_unmap_range() self-deadlock — upstream status and proposed fix (2026-07-29)

Companion to `FINDING.md`, which records the observation and root cause. This document covers three things: what the defect is in precise terms, what the upstream/community situation is, and a concrete proposed fix.

**Every claim below is labelled with how it was established.** Three confidence levels are used:

- **[VERIFIED-LOCAL]** — read directly from the source tree at `/home/jose/common-stage2mvp`, complete function bodies, file:line cited.
- **[VERIFIED-HW]** — observed on the target hardware (N90, kernel 6.6.30+ #47).
- **[FETCHED]** — obtained by fetching a web page (android.googlesource.com) and having it summarised. **The quoted code is trusted; any conclusion the fetch itself drew is not.** These have not been confirmed by cloning the tree.

---

## 1. The defect

**[VERIFIED-LOCAL]** `stage2_unmap_vm()` acquires the calling process's `mmap_lock` for **read** and holds it across the entire unmap (`arch/arm64/kvm/mmu.c:1150-1168`):

```c
void stage2_unmap_vm(struct kvm *kvm)
{
	idx = srcu_read_lock(&kvm->srcu);
	mmap_read_lock(current->mm);                 /* (1) mmap_lock, READ */
	write_lock(&kvm->mmu_lock);

	slots = kvm_memslots(kvm);
	kvm_for_each_memslot(memslot, bkt, slots)
		stage2_unmap_memslot(kvm, memslot);  /* -> ... -> pkvm_unmap_range() */

	write_unlock(&kvm->mmu_lock);
	mmap_read_unlock(current->mm);
	srcu_read_unlock(&kvm->srcu, idx);
}
```

**[VERIFIED-LOCAL]** Deep inside that region, `pkvm_unmap_range()` calls `account_locked_vm()` on the **same** mm (`mmu.c:324-346`):

```c
static int pkvm_unmap_range(struct kvm *kvm, u64 start, u64 end)
{
	struct mm_struct *mm = kvm->mm;              /* (2) same mm as current->mm */
	...
	/* account_locked_vm may sleep */
	write_unlock(&kvm->mmu_lock);                /* (3) drops mmu_lock ... */
	account_locked_vm(mm, cnt, false);           /* (4) ... but not mmap_lock */
	write_lock(&kvm->mmu_lock);
	return ret;
}
```

**[VERIFIED-LOCAL]** `account_locked_vm()` takes the same rwsem for **write** (`mm/util.c:533-547`):

```c
int account_locked_vm(struct mm_struct *mm, unsigned long pages, bool inc)
{
	if (pages == 0 || !mm)
		return 0;                            /* (5) no-op when cnt == 0 */
	mmap_write_lock(mm);                         /* (6) same rwsem, WRITE */
	ret = __account_locked_vm(mm, pages, inc, current, capable(CAP_IPC_LOCK));
	mmap_write_unlock(mm);
	return ret;
}
```

A task holding an rwsem for read cannot acquire it for write. **(1) and (6) are the same `mm->mmap_lock`.** This is an unconditional self-deadlock, not a race.

The comment at (3) shows the author knew `account_locked_vm()` can sleep — which is why `mmu_lock` is dropped and retaken around it. **The lock that actually blocks is `mmap_lock`, taken by the caller two frames up, and dropping `mmu_lock` does nothing about it.**

### 1.1 Only one of the unmap paths is affected

**[VERIFIED-LOCAL]** Four call sites reach the stage-2 unmap machinery. Their locking differs:

| call site | locks held | reaches `pkvm_unmap_range()`? |
|---|---|---|
| `kvm_unmap_gfn_range()` `mmu.c:2285` | — | **No** — returns early: `if (is_protected_kvm_enabled()) return false;` |
| `kvm_arch_flush_shadow_all()` `mmu.c:2603` | `mmu_lock` only | yes — **no `mmap_lock`, safe** |
| `kvm_arch_flush_shadow_memslot()` `mmu.c:2616` | `mmu_lock` only | yes — **no `mmap_lock`, safe** |
| `stage2_unmap_vm()` `mmu.c:1157` | `mmu_lock` + **`mmap_read_lock`** | **yes → deadlocks** |

This matters for the fix: only one call path needs to change behaviour.

### 1.2 Why the `mmap_read_lock` is there at all

**[VERIFIED-LOCAL]** It is genuinely required. `stage2_unmap_memslot()` (`mmu.c:1101-1140`) walks the process's VMAs with `find_vma_intersection(current->mm, hva, reg_end)` to skip `VM_PFNMAP` regions. That walk needs `mmap_lock`. So the lock cannot simply be removed.

---

## 2. Trigger and gating conditions

**[VERIFIED-LOCAL]** The entry point is a **second** `KVM_ARM_VCPU_INIT` on a vCPU that has already run (`arch/arm64/kvm/arm.c:1437-1441`):

```c
	if (vcpu_has_run_once(vcpu)) {
		if (!cpus_have_final_cap(ARM64_HAS_STAGE2_FWB))
			stage2_unmap_vm(vcpu->kvm);
		else
			icache_inval_all_pou();
	}
```

Two conditions gate it, which is why the machine survived hours of fuzzing before the first hit:

1. **`cnt > 0`** — `account_locked_vm()` returns immediately when `pages == 0` (step 5 above), so at least one *pinned* page must be unmapped. The guest must have faulted memory in.
2. **`!ARM64_HAS_STAGE2_FWB`** — FWB-capable CPUs take the `icache_inval_all_pou()` branch and never reach `stage2_unmap_vm()` here. **[VERIFIED-HW]** N90 lacks FWB and therefore takes the deadlocking branch. This plausibly explains how the defect has survived: much arm64 server/phone silicon has FWB.

---

## 3. Evidence

**[VERIFIED-HW]** Three occurrences on N90 (kernel 6.6.30+ #47) on 2026-07-29 — `13:29:19`, `13:55:22`, `14:36:46` — with byte-identical call traces, archived as `report0`, `report1`, `report2`:

```
INFO: task syz.2.22849:254066 blocked for more than 122 seconds.   state:D
 rwsem_down_write_slowpath      kernel/locking/rwsem.c:1242
 down_write                     kernel/locking/rwsem.c:1656
 mmap_write_lock                include/linux/mmap_lock.h:108
 account_locked_vm              mm/util.c:540
 pkvm_unmap_range               arch/arm64/kvm/mmu.c:342
 ___unmap_stage2_range          arch/arm64/kvm/mmu.c:390
 __unmap_stage2_range           arch/arm64/kvm/mmu.c:410
 stage2_unmap_memslot           arch/arm64/kvm/mmu.c:1137
 stage2_unmap_vm                arch/arm64/kvm/mmu.c:1162
 kvm_arch_vcpu_ioctl_vcpu_init  arch/arm64/kvm/arm.c:1440
 kvm_vcpu_ioctl                 virt/kvm/kvm_main.c:4317
```

The machine became fully unresponsive (GUI frozen) and required a manual power cycle each time.

**Not established:** no minimised reproducer was produced. Reproduction requires replaying programs on the target, and the target was wedged — syzkaller reported `reproducing=0` throughout and no `repro0` exists. The trigger sequence in §2 is **derived from code**, not executed.

---

## 4. Upstream status

### 4.1 Android Common Kernel

**[FETCHED]** `android15-6.6` — **affected**. The fetched page for `arch/arm64/kvm/mmu.c` shows both halves of the defect present and unmodified:

```c
/* pkvm_unmap_range() */
write_unlock(&kvm->mmu_lock);
account_locked_vm(mm, cnt, false);
write_lock(&kvm->mmu_lock);
```
```c
/* stage2_unmap_vm() */
mmap_read_lock(current->mm);
write_lock(&kvm->mmu_lock);
...
```

**Important caveat about this source:** the fetch's own summary concluded *"the problematic pattern does not exist in this branch"* — that conclusion is **wrong**, and is rejected here. It checked only that `mmu_lock` is released before the sleeping call, which is precisely the mistake the original author made; it did not check `mmap_lock`. The **quoted code**, which is what this document relies on, shows the defect. This has not been confirmed by cloning the tree, and should be before any upstream report.

**[FETCHED]** `android16-6.12` — **not affected**, but by restructuring rather than by a fix:

- `stage2_unmap_vm()` still holds `mmap_read_lock(current->mm)` across the unmap loop (quoted verbatim, plus a new `kvm_nested_s2_unmap()` call).
- **`pkvm_unmap_range()` no longer exists in the file.**
- `account_locked_vm()` now appears only in `pkvm_mem_abort()` — the *fault* path — and in its cleanup loop, which runs after `mmu_lock` is released. Unmap-time accounting has moved to `pkvm_release_ppage()` and is no longer performed beneath `stage2_unmap_vm()`.

So the newer branch resolves the problem as a side effect of redesigning pinned-page handling, not through a targeted patch. **No commit specifically fixing this deadlock was found.**

### 4.2 Community searches — what was looked for and what was found

Searches were run against the Linux kernel and Android kernel communities for a direct fix. **None was found.** What surfaced was adjacent but distinct:

| result | relation to this defect |
|---|---|
| *"kvm: arm/arm64: Fix locking for kvm_free_stage2_pgd"* (Suzuki Poulose, 2017) — patchwork/lore | Same *area* (`unmap_stage2_range` locking), different bug: `kvm_free_stage2_pgd()` not holding `mmu_lock`, and `cond_resched_lock()` for long unmaps. **Not a fix for this.** |
| pKVM `pkvm_vcpu_init_psci()` returning `-EINVAL` when a second RUNNABLE vCPU of a pvmfw VM claims the primary slot, causing a hyp panic; fixed by propagating the error and unwinding via `teardown_hyp_vcpu_init()` (android16-6.12 / android17-6.18) | **Adjacent and worth noting** — same second-vCPU / `pvmfw_entry_vcpu` region we analysed, and the same `-EINVAL` return at `hyp/nvhe/pkvm.c:571-573`. But it concerns the *hypervisor* panicking on vCPU init, not the *host-side* `mmap_lock` deadlock. Different bug. |
| *"KVM: arm64: Non-protected guest stage-2 support for pKVM"* (Quentin Perret, 2024) — LWN/patchew | Part of the broader pinned-page/stage-2 redesign that eventually removes `pkvm_unmap_range`. Context, not a fix. |

**Conclusion:** the defect appears unreported. Given android15-6.6 is affected **[FETCHED]**, it is worth reporting to the Android kernel team — any non-FWB arm64 platform running pKVM on that branch has a userspace-triggerable machine hang.

---

## 5. Proposed fix

### 5.1 Design

Because only `stage2_unmap_vm()` holds `mmap_lock` (§1.1), the accounting can be deferred out of that region without touching the other two live paths and without changing any function signature.

```c
/* 1. New per-VM counter, alongside the existing pkvm state in struct kvm_arch. */
+	atomic_long_t pending_unaccount;

/* 2. pkvm_unmap_range(): accumulate only. Never touch mmap_lock here. */
 static int pkvm_unmap_range(struct kvm *kvm, u64 start, u64 end)
 {
-	struct mm_struct *mm = kvm->mm;
 	unsigned long index = start;
 	unsigned long cnt = 0;
 	void *entry;
 	int ret = 0;

 	mt_for_each(&kvm->arch.pkvm.pinned_pages, entry, index, end - 1) {
 		struct kvm_pinned_page *ppage = entry;
 		ret = pkvm_unmap_guest(kvm, ppage);
 		if (ret)
 			break;
 		cnt++;
 	}

-	/* account_locked_vm may sleep */
-	write_unlock(&kvm->mmu_lock);
-	account_locked_vm(mm, cnt, false);
-	write_lock(&kvm->mmu_lock);
+	atomic_long_add(cnt, &kvm->arch.pkvm.pending_unaccount);
 	return ret;
 }

/* 3. Flush helper — call ONLY where no mmap_lock and no mmu_lock is held. */
+static void pkvm_flush_unaccount(struct kvm *kvm)
+{
+	long n = atomic_long_xchg(&kvm->arch.pkvm.pending_unaccount, 0);
+
+	if (n > 0)
+		account_locked_vm(kvm->mm, n, false);
+}
```

Flush at exactly three points, each **after** the relevant locks are dropped:

| function | flush position |
|---|---|
| `stage2_unmap_vm()` `mmu.c:1166` | after `mmap_read_unlock(current->mm)` |
| `kvm_arch_flush_shadow_all()` `mmu.c:2607` | after `write_unlock(&kvm->mmu_lock)` |
| `kvm_arch_flush_shadow_memslot()` `mmu.c:2618` | after `write_unlock(&kvm->mmu_lock)` |

### 5.2 Secondary benefit

The change also removes a latent hazard unrelated to the deadlock. The current code drops and retakes `mmu_lock` **in the middle of an unmap**, solely to make room for a sleeping call. That silently breaks the caller's atomicity: `stage2_unmap_vm()` believes it holds `mmu_lock` across its whole memslot loop, but `pkvm_unmap_range()` opens a window inside it during which the `pinned_pages` maple tree can be modified. Deferring the accounting removes the lock dance entirely, making the unmap genuinely atomic under `mmu_lock` — which is what every caller already assumes.

### 5.3 Alternatives considered and rejected

**Drop `mmu_lock` more aggressively.** Already done, and it is the wrong lock. This is exactly the mistake the current code makes.

**Use `__account_locked_vm()` instead.** **[VERIFIED-LOCAL]** Its contract (`mm/util.c:483-484`) states: *"Assumes @task and @mm are valid ... and that mmap_lock is held as writer."* `stage2_unmap_vm()` holds it for **read**. Promoting to `mmap_write_lock` would serialise the entire unmap against every page fault in the process, and — decisively — the other two live call paths (`kvm_arch_flush_shadow_all`, `kvm_arch_flush_shadow_memslot`) hold no `mmap_lock` at all, so they would violate the contract and race on `mm->locked_vm`.

**Skip the accounting under pKVM.** Would leak `mm->locked_vm` accounting monotonically and eventually trip `RLIMIT_MEMLOCK` for the process. Not viable.

**Backport the android16-6.12 restructure.** The upstream-sanctioned direction and the right long-term answer, but a substantially larger change to pinned-page lifetime handling. The fix above is the minimal correct change for this tree.

### 5.4 Verification status

**The proposed fix is untested.** It has not been compiled, deployed, or exercised. What is established is the defect (§1, §3) and that the fix addresses the specific locking inversion that causes it.

A test is feasible: D3000 (Phytium D3000, `6.6.30-pkvmcov`, provisioned 2026-07-29) is a second instrumented board that is not running the fuzzing campaign, so a patched kernel can be built and deployed there without disturbing N90. Confirming the fix requires driving the §2 trigger sequence, which also has not yet been executed against the *unpatched* kernel — so the same test would upgrade the trigger from "derived" to "confirmed" in both directions.

---

## 6. Summary of what is and is not established

| claim | status |
|---|---|
| The deadlock exists and hangs the machine | **[VERIFIED-HW]** — three identical traces |
| Root cause is read-then-write on the same `mmap_lock` | **[VERIFIED-LOCAL]** — all three functions read in full |
| Only `stage2_unmap_vm()` is affected of the live paths | **[VERIFIED-LOCAL]** |
| Gated by `cnt > 0` and `!ARM64_HAS_STAGE2_FWB` | **[VERIFIED-LOCAL]** + **[VERIFIED-HW]** for the FWB condition on N90 |
| Trigger is a second `KVM_ARM_VCPU_INIT` on a run vCPU | **derived from code — NOT executed** |
| android15-6.6 is affected | **[FETCHED]** — quoted code only; confirm by cloning before reporting |
| android16-6.12 is not affected | **[FETCHED]** — `pkvm_unmap_range` absent |
| No community fix exists for this defect | searched; none found. Absence of evidence only |
| The proposed fix is correct | **untested** — reasoning only |
