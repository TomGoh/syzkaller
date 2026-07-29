# Finding: self-deadlock in pkvm_unmap_range() hangs the machine (2026-07-29)

**`INFO: task syz.2.22849:254066 blocked for more than 122 seconds`**, N90 kernel 6.6.30+ #47. The board became completely unresponsive (GUI frozen) and required a manual power cycle. Full trace in `hung-task-report.txt` / `report0`.

**Severity: high.** Userspace holding only an open `/dev/kvm` can hard-hang the machine. No special privilege is required beyond the ability to create a VM.

## The observed failure (VERIFIED — this is the trace, not a theory)

```
task:syz.2.22849     state:D
 rwsem_down_write_slowpath        kernel/locking/rwsem.c:1242
 down_write                       kernel/locking/rwsem.c:1656
 mmap_write_lock                  include/linux/mmap_lock.h:108
 account_locked_vm                mm/util.c:540
 pkvm_unmap_range                 arch/arm64/kvm/mmu.c:342
 ___unmap_stage2_range            arch/arm64/kvm/mmu.c:390
 stage2_apply_range               arch/arm64/kvm/mmu.c:81
 __unmap_stage2_range             arch/arm64/kvm/mmu.c:410
 stage2_unmap_memslot             arch/arm64/kvm/mmu.c:1137
 stage2_unmap_vm                  arch/arm64/kvm/mmu.c:1162
 kvm_arch_vcpu_ioctl_vcpu_init    arch/arm64/kvm/arm.c:1440
 kvm_arch_vcpu_ioctl              arch/arm64/kvm/arm.c:1547
 kvm_vcpu_ioctl                   virt/kvm/kvm_main.c:4317
 __arm64_sys_ioctl
```

## The mechanism (VERIFIED — full read of all three functions)

`stage2_unmap_vm()` takes the caller's `mmap_lock` for **read** and holds it across the whole unmap:

```c
void stage2_unmap_vm(struct kvm *kvm)                 /* mmu.c:1150 */
{
	idx = srcu_read_lock(&kvm->srcu);
	mmap_read_lock(current->mm);                  /* (1) READ */
	write_lock(&kvm->mmu_lock);
	kvm_for_each_memslot(memslot, bkt, slots)
		stage2_unmap_memslot(kvm, memslot);   /* -> pkvm_unmap_range() */
	write_unlock(&kvm->mmu_lock);
	mmap_read_unlock(current->mm);
	srcu_read_unlock(&kvm->srcu, idx);
}
```

`pkvm_unmap_range()` then calls `account_locked_vm()` on **the same mm**:

```c
static int pkvm_unmap_range(struct kvm *kvm, u64 start, u64 end)   /* mmu.c:324 */
{
	struct mm_struct *mm = kvm->mm;               /* (2) SAME mm as current->mm */
	...
	/* account_locked_vm may sleep */
	write_unlock(&kvm->mmu_lock);                 /* (3) drops mmu_lock ... */
	account_locked_vm(mm, cnt, false);            /* (4) ... but not mmap_lock */
	write_lock(&kvm->mmu_lock);
	return ret;
}
```

```c
int account_locked_vm(struct mm_struct *mm, unsigned long pages, bool inc)  /* mm/util.c:533 */
{
	if (pages == 0 || !mm)
		return 0;                             /* (5) no-op at cnt == 0 */
	mmap_write_lock(mm);                          /* (6) SAME rwsem, WRITE */
	...
}
```

**A task that holds an rwsem for read cannot acquire it for write.** Steps (1) and (6) are the same `mm->mmap_lock`. This is an unconditional self-deadlock, not a race.

The comment at (3) shows the author knew `account_locked_vm()` can sleep — which is why `mmu_lock` is dropped and retaken around it. But the lock that actually blocks is `mmap_lock`, taken by the **caller two frames up**, and dropping `mmu_lock` does nothing about it.

## Why it is not hit constantly

Two conditions gate it, which is why the machine survived hours of fuzzing first:

1. **`cnt > 0`** — `account_locked_vm()` returns immediately when `pages == 0` (step 5). At least one *pinned* page must be unmapped, so the guest must have actually faulted memory in.
2. **`!cpus_have_final_cap(ARM64_HAS_STAGE2_FWB)`** — `arm.c:1438-1441` only calls `stage2_unmap_vm()` on CPUs without stage-2 Force-Write-Back; otherwise it does `icache_inval_all_pou()` instead. This board lacks FWB, so it takes the deadlocking branch. **On FWB-capable hardware this path is unreachable**, which likely explains why the bug has survived.

## Trigger (DERIVED from code — NOT yet reproduced)

```
open /dev/kvm
KVM_CREATE_VM                      (pKVM pins guest pages on fault)
KVM_CREATE_VCPU
KVM_ARM_VCPU_INIT                  (first init)
KVM_RUN                            -> guest faults pages in, so cnt > 0 later
KVM_ARM_VCPU_INIT                  (second init: vcpu_has_run_once() is now true
                                    -> stage2_unmap_vm() -> DEADLOCK)
```

The second `KVM_ARM_VCPU_INIT` is the trigger: `arm.c:1437` gates `stage2_unmap_vm()` on `vcpu_has_run_once(vcpu)`, so the vCPU must have been run at least once before being re-initialised.

**This sequence is derived from reading the code and has not been executed.** Confirming it means deliberately hanging the board again.

## Reproduction status: syzkaller did NOT produce a reproducer

The crash directory contains `description`, `log0`, `machineInfo0`, `report0`, `title-stat` — **no `repro0` / `repro.prog` / `repro.cprog`**, and the manager reported `reproducing=0` throughout.

The reason is mechanical rather than a limitation of the finding: reproduction requires re-executing candidate programs *on the target*, and the target was wedged. The manager detected the crash at 13:29:19, the RPC connection died immediately after, and the board stayed hung until it was manually power-cycled at ~13:31. There was nothing alive to reproduce on.

So the status is:

| question | answer |
|---|---|
| Bug observed on real hardware? | **Yes** — the hung-task trace is the deadlock caught in the act |
| Root cause identified? | **Yes** — code-confirmed, all three functions read in full |
| Serialised reproducer? | **No** — the wedged board left nothing to replay on |
| Minimal trigger sequence proven? | **No** — derived from code, not yet executed |

## How it was found

This bug was **unreachable by the campaign as it existed that morning**. Until 2026-07-29 the fuzzer ran with three enabled system calls (`openat$kvm`, `syz_kvm_run_fw_fault_gen$arm64`, `close`), and the whole protected-VM lifecycle was performed inside one C composite that calls `KVM_ARM_VCPU_INIT` exactly once. A *second* init on an already-run vCPU was not expressible.

Widening `enable_syscalls` to expose the ordinary ARM64 KVM interface — including `ioctl$KVM_ARM_VCPU_INIT` as a first-class, independently-orderable call — made the sequence generatable. The deadlock appeared within roughly two hours of that change.

## Operational impact on the campaign

- The hung-task watchdog **warns but does not panic** (`panic_on_warn=0` on this target), so the machine does not reboot itself.
- The `isolated` backend cannot power-cycle a physical board, so syzkaller could only retry the connection. **Manual intervention was required.**
- With `ioctl$KVM_ARM_VCPU_INIT` enabled, an unattended campaign should be expected to re-wedge periodically until the kernel is fixed. That is the trade-off to weigh against the coverage that call unlocks.

## Suggested fix (for the kernel owner)

Move the `account_locked_vm()` accounting out from under the caller's `mmap_lock`. Options, roughly in order of intrusiveness:

1. Accumulate the unaccount count during the unmap and call `account_locked_vm()` **after** `stage2_unmap_vm()` releases `mmap_read_lock()`.
2. Use the `__account_locked_vm()` variant, which expects the caller to already hold `mmap_lock` — note this needs it held for **write**, so `stage2_unmap_vm()` would have to be promoted from `mmap_read_lock` to `mmap_write_lock`, which has its own contention cost.

Dropping `mmu_lock` around the call — what the current code does — cannot fix this, because `mmu_lock` is not the lock that blocks.

## Evidence in this directory

| file | contents |
|---|---|
| `report0`, `hung-task-report.txt` | the parsed hung-task report with the full call trace |
| `log0` | 146 KB raw console log covering the crash window |
| `machineInfo0` | target kernel/hardware identification |
| `manager-log-window.txt` | syz-manager log across the outage and recovery |
| `target-warn-history-summary.txt` | summary of the 51,916,800-byte on-target `/dev/kmsg` record (567,709 lines) captured before the hang: **15,528** `kvm_tlb_flush_vmid_range` and **130** `kvm_timer_update_irq` warnings, and no other event class |
| `description`, `title-stat` | syzkaller's crash title and statistics |
