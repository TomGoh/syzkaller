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

Neither touches `mmap_lock`. **ACK `android15-6.6` still ships the bug at HEAD** — verified first-hand from the unmodified reference tree, `git show aosp/android15-6.6:arch/arm64/kvm/mmu.c` at `742616e`, which shows `/* account_locked_vm may sleep */` and `account_locked_vm(mm, cnt, false)` at lines 349-351. Mainline is immune only as a side effect of the v6.14 rework that moved guest stage-2 into EL2 (`fce886a60207` and its series) — new infrastructure, not a backportable fix. Also checked and irrelevant: `ed14b491ec76` (THP accounting arithmetic), `3ae13572d106` (THP reclaim with ballooning), `04512258010d` (charges reclaim against `kvm->mm` rather than `current->mm` — landed upstream, `pkvm.c:355` on `torvalds/master`; klinux still uses `current->mm`).

*Caveat on the search:* `lore.kernel.org` is behind a challenge that blocked automated queries, so an unmerged list posting could exist that was not seen. The merged state of mainline and ACK was verified from source.

### The lineage map — which trees carry this, and why

The defect belongs to the **ACK pKVM guest-stage-2 series**, not to any one product tree. Everything that imports that series inherits it; everything that does not is immune for free. Laying the lines out is what makes "upstream has no fix" a bounded claim rather than a hopeful one.

Android-Common's LTS families are **6.6 (android15) → 6.12 (android16) → 6.18 (android17)**. There is **no ACK 6.14** — 6.14 is not an LTS and ACK skipped it. Dedicated `pkvm-experimental` branches exist only up to 6.6 (`android14-5.15-pkvm-experimental`, `android14-6.1-pkvm-experimental`, `android15-6.6-pkvm_experimental`); from 6.12 on there is none, because the work had landed in the mainline ACK branch by then.

The code arrived through this series, oldest first:

```
af919d2384e8  Handle guest stage-2 page-tables entirely at EL2   introduces pinned pages + account_locked_vm
8061523a64f0  Implement MEM_RELINQUISH SMCCC hypercall
b66e27a61e1f  Unshare pages from __unmap_stage2_range()          introduces pkvm_unmap_range()
78adeb53eea1  Fix account_locked_mm() call in non-preemptible section
5d9808b9071c  THP support for pKVM guests
e56d181356a4  Convert kvm_pinned_pages to an interval-tree       ← klinux has this, 2030/bug930 does not
```

That list is the complete history of `account_locked_vm` under `arch/arm64/kvm/` on `aosp/android15-6.6` — six commits, none of which addresses `mmap_lock`.

Two lineages descend from it, by different routes:

- **`android/common` `2030/bug930`** (the reference checkout `~/common`) descends **directly** from `android15-6.6-pkvm_experimental` — `git merge-base --is-ancestor` confirms it, and the merge base *is* that branch's head `e70cae0cbb35`. It stops before `e56d181356a4`, so it still batches a single `cnt` over a maple tree. This lineage is **not ours**; it appears here only to explain why a patch written for it does not apply.
- **klinux `klad-v11-next`** — the tree we work on — imported the same ANDROID series into the V11 6.6.103 kernel, *including* the interval-tree conversion. Hence per-ppage accounting in `pkvm_unmap_guest()`. The two share no object store, and their copies of this code are one ACK change apart, which is the entire reason one patch cannot serve both.

State of every line. Per the tree policy in [`issues/README.md`](../README.md), each row is read from an **unmodified** tree — `~/kernel-refs/ack`, `~/kernel-refs/linux`, or `~/common` — never from a locally patched checkout:

| tree | ref / snapshot | accounting in the unmap path | affected |
| --- | --- | --- | --- |
| mainline Linux | tag `v6.19.14` | none — no `account_locked_vm` anywhere under `arch/arm64` | no |
| mainline Linux | `torvalds/master` @ `c21bb4193` | present again (`mmu.c:1724`, `:1773`, `pkvm.c:355`) but on the **fault and reclaim** paths only | no |
| ACK `android15-6.6` | `aosp/android15-6.6` @ `742616e` | `___unmap_stage2_range` → `pkvm_unmap_range` → `account_locked_vm(mm, cnt, false)` (`mmu.c:349-351`) | **yes, still today** |
| ACK `android15-6.6-pkvm_experimental` | @ `e70cae0cbb35` | same | **yes** |
| `android/common` `2030/bug930` | `~/common` @ `da966ce9a047` | same shape (batched `cnt`, maple tree), `mmu.c:337-339` | **yes** — branch unfixed |
| klinux `klad-v11-next` | @ `348c94763cc6` | per-ppage, interval tree | **yes** — fixed by `348c94763cc6` |
| ACK `android16-6.12` | `aosp/android16-6.12` @ `f068f96` | **none — `pkvm_unmap_range`/`pkvm_unmap_guest` do not exist** | no |
| ACK `android17-6.18` | `aosp/android17-6.18` @ `42ab2c6` | none | no |

Mainline is cited by **tag** wherever the claim must stay true: `master` moves, and its line numbers shifted within a single afternoon (`mmu.c:1692`→`1724`).

### What ACK 6.12 shows, and why it matters here

6.12 is the interesting one, because it did not fix this defect — it *dissolved* it, and the way it did so is independent confirmation of the design chosen here.

`stage2_unmap_vm()` in 6.12 still takes `mmap_read_lock(current->mm)` across the whole unmap, exactly as 6.6 does. The lock did not change. What changed is that the accounting **left the unmap path**: `___unmap_stage2_range()` is now only `KVM_PGT_FN(kvm_pgtable_stage2_unmap)(...)`, and `pkvm_unmap_range()` / `pkvm_unmap_guest()` do not exist at all. The remaining un-accounting sites are:

- `pkvm.c:417`, the VM-teardown loop over `__pkvm_reclaim_dying_guest_page`, batched *after* the loop;
- `pkvm.c:762`, `pkvm_host_reclaim_page()`, after `write_unlock(&host_kvm->mmu_lock)`;
- `mmu.c:2072`, which is **not** an unmap-path decrement — it sits under the `free_ppages:` error label of the map path, rolling back the charge taken at `mmu.c:2059` for pages that were pinned but never mapped. Same shape as mainline's `dec_account:`.

None of the three runs with `mmap_lock` held, which is the property that matters. (An earlier version of this note claimed there were only two sites, both in `pkvm.c`; reading the fetched tree rather than a web view corrected that.)

So the ACK line converged on the same principle this fix applies — *un-accounting must not run inside the `mmap_lock`-held unmap* — and reached it by restructuring, across a major version, as part of the `__pkvm_pages_to_ppages` / `__pkvm_host_donate_guest` rework. Deferring through a per-VM atomic is the minimal expression of that principle on a 6.6-shaped tree, which is what makes it the right shape to carry rather than a local invention.

*Scope of this comparison:* 6.12 and 6.18 are read from the fetched reference tree `~/kernel-refs/ack` at the snapshots given in the table above. The present-day absence of the code is verified first-hand; the commits that removed it were not identified — those branches are fetched shallow — so no commit id is cited for it.

Two collateral notes from the same reading. `892713e97ca1` *"Sidestep stage2_unmap_vm() on vcpu reset when S2FWB is supported"* (2020) is why no one upstream trips over this: FWB silicon never enters the path. And 6.12's `pkvm_host_reclaim_page()` accounts against `host_kvm->mm`, where klinux still uses `current->mm` (`pkvm.c:311`, `:534`) — a *different* defect, matching `04512258010d` above, tracked separately and out of scope for this issue.

### Why our own sibling patch could not be cherry-picked

An equivalent fix, `d7c317637aa2`, was written earlier for the **`android/common` `2030/bug930` lineage**. That is a different lineage from ours and its status there is out of scope for this issue; what matters here is only why it cannot be reused.

It rewrites `pkvm_unmap_range()`, which batches a single `cnt` over a maple tree. klinux keeps pinned pages in an interval tree and un-accounts **per-ppage** inside `pkvm_unmap_guest()` in units of `1 << ppage->order` (`mmu.c:321-367`). Different function, different unit, different data structure — the hunks have no matching context, and the two trees share no object store. What transferred is the design, and the field name, function name and comments were kept identical so the two remain recognisably the same fix if anyone reconciles them.

Because the change adds a field to `struct kvm_protected_vm`, it shifts offsets in `struct kvm_arch` and is read at EL2, so it required a full rebuild including the Rust nVHE bindings — an object-level compile test would not have proven it.

## Residual hazard

**Disabling `ioctl$KVM_ARM_VCPU_INIT` does not close this.** The 2026-08-05 census ran 10,356 execs with that syscall removed and did not hit the deadlock, but `syz_kvm_setup_cpu$arm64` issues `KVM_ARM_VCPU_INIT` from inside the C helper (`executor/common_kvm_arm64.h:225` → `:196`, declared at `sys/linux/dev_kvm_arm64.txt:156`) on a caller-supplied `fd_kvmcpu`. So the sequence `syz_kvm_add_vcpu$arm64` → `ioctl$KVM_RUN` → `syz_kvm_setup_cpu$arm64` reaches the same state and is generatable.

Treat the mitigation as reducing probability, not as protection. Any campaign against an unpatched kernel can still lose the board.
