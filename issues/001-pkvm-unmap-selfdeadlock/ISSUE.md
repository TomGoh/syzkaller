---
id: 001
slug: pkvm-unmap-selfdeadlock
title: pkvm_unmap_guest() takes mmap_lock for write while stage2_unmap_vm() holds it for read
class: kernel-defect
signature: 'INFO: task hung in __unmap_stage2_range'
hazard: wedges-target
diagnosis: root-caused
disposition: fix-proposed
repro: repro/repro-deadlock.c
observations:
  - target: 'klinux 6.6.103+ #3 @39ee2e725c12'
    state: reproduced
    run: 2026-08-05-deadlock-repro
    evidence: evidence/2026-08-05-task-stack.txt
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

A task holding an rwsem for read cannot acquire it for write. This is an unconditional self-deadlock, not a race — there is no interleaving in which it succeeds.

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

`stage2_unmap_vm()` has exactly one caller in the tree, this one. FWB-capable silicon takes the `icache_inval_all_pou()` branch and never reaches the defect, which is likely why it has gone unnoticed upstream. N90 and D3000 both lack FWB. The pinned-page requirement matters because `account_locked_vm()` returns early on a zero count.

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

A fix exists on another lineage and is **not** applicable unchanged.

`d7c317637aa2` — *"KYLIN: KVM: arm64: pkvm: defer RLIMIT_MEMLOCK accounting out of mmap_lock"*, on `futlab-fixes`, landed on `android/common` `2030/bug930`. It defers the decrement into a per-VM atomic (`pending_unaccount`) that each unmap path settles through `pkvm_flush_unaccount()` after its own locks are dropped. Its message records verification on D3000 and N90 at 6.6.30: the reproducer that hung every unpatched kernel returns 0 with it applied.

It does not cherry-pick into klinux. That patch rewrites `pkvm_unmap_range()`, which batches a single `cnt` for the whole range; klinux has refactored pinned pages into an interval tree where each page is unmapped by `pkvm_unmap_guest()`, so the accounting call sits **per-ppage** (`mmu.c:352-367`). The port is an adaptation, and it changes `struct kvm_protected_vm`, which shifts offsets in `struct kvm_arch` and therefore requires a full rebuild including the Rust nVHE bindings — an object-level compile test would not prove it.

The defect was verified present in the kernel N90 is running, not only in the tree: `__unmap_stage2_range` in the built `vmlinux` disassembles with a direct `bl account_locked_vm`.

## Residual hazard

**Disabling `ioctl$KVM_ARM_VCPU_INIT` does not close this.** The 2026-08-05 census ran 10,356 execs with that syscall removed and did not hit the deadlock, but `syz_kvm_setup_cpu$arm64` issues `KVM_ARM_VCPU_INIT` from inside the C helper (`executor/common_kvm_arm64.h:225` → `:196`, declared at `sys/linux/dev_kvm_arm64.txt:156`) on a caller-supplied `fd_kvmcpu`. So the sequence `syz_kvm_add_vcpu$arm64` → `ioctl$KVM_RUN` → `syz_kvm_setup_cpu$arm64` reaches the same state and is generatable.

Treat the mitigation as reducing probability, not as protection. Any campaign against an unpatched kernel can still lose the board.
