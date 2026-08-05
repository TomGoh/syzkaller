---
id: 001
slug: pkvm-unmap-selfdeadlock
title: pkvm_unmap_guest() takes mmap_lock for write while stage2_unmap_vm() holds it for read
class: kernel-defect
signature: 'INFO: task hung in __unmap_stage2_range'
hazard: wedges-target
diagnosis: root-caused
disposition: fix-verified
repro: repro/repro-deadlock.c
observations:
  - target: 'klinux 6.6.103+ #3 @39ee2e725c12'
    state: reproduced
    run: 2026-08-05-deadlock-repro
    evidence: evidence/2026-08-05-task-stack.txt
  - target: 'klinux 6.6.103+ #4 @348c94763cc6'
    state: not-observed
    run: 2026-08-05-deadlock-fix-verify
---

# 001 — `pkvm_unmap_guest()` self-deadlock on `mmap_lock`

Any process with access to `/dev/kvm` can permanently wedge an unpatched board, and does not need the fuzzer to do it. Highest-severity issue currently tracked: it is the only one that costs the machine.

## Symptom

A task enters uninterruptible sleep inside `KVM_ARM_VCPU_INIT` and never returns. Live state of the reproducer process, read from `/proc/79770` while it was stuck (`evidence/2026-08-05-task-stack.txt`):

```
PID=79770  state=D  wchan=account_locked_vm
[<0>] account_locked_vm+0x4c/0x108
[<0>] __unmap_stage2_range+0x230/0x2e8
[<0>] stage2_unmap_vm+0x178/0x298
[<0>] kvm_arch_vcpu_ioctl+0x41c/0xc30
[<0>] kvm_vcpu_ioctl+0x5e4/0xac8
[<0>] __arm64_sys_ioctl+0x108/0x128
```

The hung-task watchdog then re-reports the same trace at 122 / 143 / 225 s (`evidence/2026-08-05-dmesg-live.txt`), confirming it never completes.

These frame offsets are byte-identical to the hung tasks the 2026-07-31 campaign produced on **both** N90 and D3000 (`notes/pkvm/evidence/v11-campaign-n90-2026-07-31/FINDING.md`), which is what identifies that campaign stall and this reproducer as one defect rather than two similar-looking ones.

## Mechanism

**Confidence: confirmed.** The lock identity is not inferred from source — `wchan` names `account_locked_vm` as the blocking function, and the frame directly above it is `stage2_unmap_vm`, the function that holds the conflicting lock.

`stage2_unmap_vm()` (`arch/arm64/kvm/mmu.c:1175-1192`) takes `mmap_read_lock(current->mm)` and holds it across the entire unmap. Below it, `pkvm_unmap_guest()` (`mmu.c:321-345`) calls `account_locked_vm()`, which takes the same rwsem for **write**:

```c
	write_unlock(&kvm->mmu_lock);
	account_locked_vm(mm, 1 << ppage->order, false);   /* -> mmap_write_lock(mm) */
	write_lock(&kvm->mmu_lock);
```

A task holding an rwsem for read cannot acquire it for write. There is no interleaving in which it succeeds, so this is a deadlock rather than a race — but it is a *self*-deadlock only when `current->mm == kvm->mm`, which is the normal case. `stage2_unmap_vm()` locks `current->mm` (`mmu.c:1182`) while `pkvm_unmap_guest()` locks `kvm->mm` (`mmu.c:323,339`). If the ioctl is issued from a task in a different mm — a forked child holding the inherited fd, or an fd passed over `SCM_RIGHTS` — those are two different rwsems and it does not deadlock.

The existing code does release a lock around the sleeping call, but it releases `kvm->mmu_lock`, which is not the lock that blocks. The comment above it explains the `mmu_lock` release and does not mention `mmap_lock` at all.

## Trigger

A second `KVM_ARM_VCPU_INIT` on a vCPU that has already run, on a CPU without `ARM64_HAS_STAGE2_FWB`, with at least one pinned page to unmap.

`kvm_arch_vcpu_ioctl_vcpu_init()` (`arch/arm64/kvm/arm.c:1592-1596`) reaches it:

```c
	if (vcpu_has_run_once(vcpu)) {
		if (!cpus_have_final_cap(ARM64_HAS_STAGE2_FWB))
			stage2_unmap_vm(vcpu->kvm);
		else
			icache_inval_all_pou();
	}
```

`stage2_unmap_vm()` has exactly one caller in the tree, this one. FWB-capable silicon takes the `icache_inval_all_pou()` branch and never reaches the defect, which is likely why it has gone unnoticed upstream. N90 and D3000 both lack FWB.

Two further preconditions gate it, and both must hold:

- **The host must be booted `kvm-arm.mode=protected`.** Otherwise `___unmap_stage2_range()` (`mmu.c:406-412`) takes the `kvm_pgtable_stage2_unmap()` branch, which never touches `account_locked_vm()`.
- **The guest must NOT be a protected VM.** `__unmap_stage2_range()` (`mmu.c:420`) returns immediately when `is_protected_kvm_enabled() && kvm->arch.pkvm.enabled`, and `pkvm.enabled` is set only for `KVM_VM_TYPE_ARM_PROTECTED` (`pkvm.c:462-469`).

So the single affected combination is a **plain `KVM_CREATE_VM` guest on a pKVM host** — which is what the reproducer creates. Such VMs still own pinned pages: every VM on a protected host faults through `pkvm_mem_abort()`, which charges at `mmu.c:1809` and inserts a ppage at `mmu.c:1844`.

At least one pinned page must exist, but not for the reason an earlier version of this note gave. `account_locked_vm()`'s early return (`mm/util.c:551`) tests its *argument*, which here is `1 << ppage->order` and is never zero. The real reason a pageless VM is safe is that `for_ppage_node_in_range` (`mmu.c:347-350`) iterates zero times, so the call never happens.

## Reproduction

`repro/repro-deadlock.c` — build and run:

```
aarch64-linux-gnu-gcc -O2 -static -o repro-deadlock repro/repro-deadlock.c
```

Deterministic: hung on the first attempt on klinux `6.6.103+ #3`, with no adaptation from the version written for the `common` 6.6.30 lineage (`evidence/2026-08-05-repro-stdout.txt`). A fixed kernel returns 0.

> **This program wedges the board it runs on.** Recovery is possible over ssh without physical access, but only via sysrq — see [the run record](../runs/2026-08-05-deadlock-repro.md#recovery). Do not run it on a board you cannot reboot.

## Blast radius

One stuck task takes the whole machine, and this was measured rather than assumed. Reading different files of the poisoned PID from a healthy shell:

| read | takes `mmap_lock`? | result |
| --- | --- | --- |
| `/proc/79770/stat` | no | OK |
| `/proc/79770/cmdline` | read | blocks forever |
| `/proc/79770/maps` | read | blocks forever |

The blocked writer sits in the rwsem queue, and Linux rwsems are fair, so every subsequent *reader* of that `mm` queues behind a writer that can never be granted. `sysrq-w` showed the collateral, all in `__access_remote_vm` → `down_read_killable`:

```
task:OptiDaemon     state:D  pid:1965      <- stock Kylin service, no fuzzer involved
task:repro-deadlock state:D  pid:79770     <- the original victim
task:ps             state:D  pid:82448
task:pgrep          state:D  pid:83814
```

`OptiDaemon` polls `/proc` on its own schedule and wedged with no help from us. So the board decays on a timer as one daemon after another walks `/proc` — an unpatched machine does not need heavy fuzzing to die, a single `ps` finishes the job.

Two operational consequences follow. Blocked tasks are in `D` state and unkillable, so syz-manager cannot recover the target: it fails to redeploy with `scp: /root/syzkaller-fuzz/syz-executor: Text file busy` and the campaign stalls indefinitely rather than restarting. And each stuck task holds a `kvm->srcu` read side forever, which blocks reclaim of everything it pins — the leading candidate mechanism for the unexplained memory exhaustion seen earlier on D3000, though that link is unproven.

## Fix status

**FIXED and verified** — `348c94763cc6` on klinux branch `pkvm-unmap-deadlock-fix`, *"KYLIN: KVM: arm64: pkvm: defer RLIMIT_MEMLOCK accounting out of mmap_lock"*, `+52/-7` across `mmu.c` and `kvm_host.h`. Verified on N90 by [2026-08-05-deadlock-fix-verify](../runs/2026-08-05-deadlock-fix-verify.md): the same unmodified binary that hung the board deterministically now passes 10 of 10, with the kernel differing from the failing one by the patch alone. checkpatch: 0 errors.

The fix accumulates the count into a new per-VM atomic (`pending_unaccount`) and settles it in `pkvm_flush_unaccount()` from the three sites where every relevant lock has been dropped — `stage2_unmap_vm`, `kvm_uninit_stage2_mmu`, `kvm_arch_flush_shadow_memslot`. Removing the sleeping call also removed the reason to drop `mmu_lock`, which closed a second hole in the same loop: `for_ppage_node_in_range` caches the successor node across the body, and a concurrent `MEM_RELINQUISH` could `kfree` it inside the window the drop opened. A port that deferred the accounting but kept the drop would have fixed the deadlock and left the use-after-free.

### Nothing upstream fixes this

Searched Android-Common and mainline before writing it. Two commits look like matches and are not:

- `78adeb53eea1` *"Fix account_locked_mm() call in non-preemptible section"* (2024-05) — batches the accounting into `pkvm_unmap_range()` inside `write_unlock`/`write_lock`. Escapes `mmu_lock` only.
- `246414094770` *"Don't do account_locked_vm() while atomic"* (2024-11) — per-page `write_unlock`/`account`/`write_lock`. Escapes `mmu_lock` only. **This tree already carried the equivalent**, adapted for huge pages; it is the code the deadlock was found in.

Neither touches `mmap_lock`, and ACK `android15-6.6` still ships the bug at HEAD (2026-08-04). Mainline is immune only as a side effect of the v6.14 rework that moved guest stage-2 into EL2 (`fce886a60207` and its series) — new infrastructure, not a backportable fix. Also checked and irrelevant: `ed14b491ec76`/`9a13ca20af8d` (THP accounting arithmetic), `3ae13572d106` (THP reclaim with ballooning), `04512258010d` (charges reclaim against `kvm->mm` rather than `current->mm`).

*Caveat on the search:* `lore.kernel.org` is behind a challenge that blocked automated queries, so an unmerged list posting could exist that was not seen. The merged state of mainline and ACK was verified from source.

### Why our own sibling patch could not be cherry-picked

`d7c317637aa2` on `futlab-fixes` fixes the identical defect on `android/common` `2030/bug930`, and klinux does not even share an object store with it. The two lineages descend from the *different* partial fixes above, so the defective statement lives in a different function, in a different unit, over a different data structure.

Concretely: the sibling patch rewrites `pkvm_unmap_range()`, which batches a single `cnt` over a maple tree; klinux keeps pinned pages in an interval tree and un-accounts **per-ppage** inside `pkvm_unmap_guest()` in units of `1 << ppage->order` (`mmu.c:321-367`). The hunks have no matching context. What transferred is the design, and the field name, function name and comments were kept identical so the two remain recognisably the same fix if anyone reconciles them.

Because the change adds a field to `struct kvm_protected_vm`, it shifts offsets in `struct kvm_arch` and is read at EL2, so it required a full rebuild including the Rust nVHE bindings — an object-level compile test would not have proven it.

## Residual hazard

**Disabling `ioctl$KVM_ARM_VCPU_INIT` does not close this.** The 2026-08-05 census ran 10,356 execs with that syscall removed and did not hit the deadlock, but `syz_kvm_setup_cpu$arm64` issues `KVM_ARM_VCPU_INIT` from inside the C helper (`executor/common_kvm_arm64.h:225` → `:196`, declared at `sys/linux/dev_kvm_arm64.txt:156`) on a caller-supplied `fd_kvmcpu`. So the sequence `syz_kvm_add_vcpu$arm64` → `ioctl$KVM_RUN` → `syz_kvm_setup_cpu$arm64` reaches the same state and is generatable.

Treat the mitigation as reducing probability, not as protection. Any campaign against an unpatched kernel can still lose the board.
