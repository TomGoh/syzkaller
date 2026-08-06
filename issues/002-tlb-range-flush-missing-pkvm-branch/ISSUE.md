---
id: 002
slug: tlb-range-flush-missing-pkvm-branch
title: kvm_arch_flush_remote_tlbs_range() has no pKVM branch, so EL2 rejects the hypercall and the TLB invalidation never runs
class: kernel-defect
signature: 'WARNING in kvm_tlb_flush_vmid_range'
hazard: none
diagnosis: root-caused
disposition: fix-verified
repro: repro/repro-tlbflush-warn.c
observations:
  - target: 'klinux 6.6.103+ #3 @39ee2e725c12'
    state: observed
    run: 2026-08-05-klinux-n90-census
    evidence: evidence/2026-08-05-res-a0-distribution.txt
  - target: 'klinux 6.6.103+ #4 @348c94763cc6'
    state: reproduced
    run: 2026-08-05-tlb-warn-repro
    evidence: evidence/2026-08-05-repro-warn-kernel4.txt
  - target: 'klinux 6.6.103+ #13 @24714fe308bb'
    state: not-observed
    run: 2026-08-06-fix-deploy-verify
---

# 002 — the range TLB flush asks EL2 for a hypercall EL2 has retired

中文版本:[ISSUE_zh.md](ISSUE_zh.md)

The most frequent signature this project has produced: 257 on N90 and 278 on D3000 within twenty minutes on 2026-07-31, 270 in the last 3.5 minutes of the 2026-08-05 census. It is a warning, not a crash — the machine is unaffected. Its weight is that the operation the warning reports is not merely failing, it is not happening at all, and the caller is the dirty-logging write-protect path.

## Symptom

```
WARNING: CPU: 0 PID: 92660 at arch/arm64/kvm/hyp/pgtable.c:654 kvm_tlb_flush_vmid_range+0x74/0xe0
CPU: 0 PID: 92660 Comm: repro-tlbflush Not tainted 6.6.103+ #4
Call trace:
 kvm_tlb_flush_vmid_range+0x74/0xe0
 kvm_arch_flush_remote_tlbs_range+0x34/0x48
 kvm_flush_remote_tlbs_memslot+0x2c/0xa0
 kvm_arch_commit_memory_region+0x2e4/0x338
 kvm_set_memslot+0x1f0/0x728
 __kvm_set_memory_region+0x46c/0x550
 kvm_vm_ioctl+0xf08/0x1ca8
```

One call path, no variance: on 2026-07-31 all 257 N90 instances shared it, frame for frame. The entry ioctl is `KVM_SET_USER_MEMORY_REGION`.

`pgtable.c:654` is not a check, it is the hypercall itself:

```c
	if (!system_supports_tlb_range()) {
		kvm_call_hyp(__kvm_tlb_flush_vmid, mmu);   /* line 654 */
		return;
	}
```

The `WARN_ON` lives inside the macro and is attributed to the call site: `kvm_call_hyp_nvhe()` ends with `WARN_ON(res.a0 != SMCCC_RET_SUCCESS)`. So EL2 returned a non-success status.

**This is not our instrumentation.** `KVM_PKVM_COV_HVC` wraps the *identical* `arm_smccc_1_1_hvc(...)` in `pkvm_cov_begin`/`pkvm_cov_end` and never touches `res`; the disassembly shows `pkvm_cov_begin` before the `hvc` and `pkvm_cov_end` after `res.a0` has already been captured. The `WARN_ON` is pre-existing upstream code.

## Mechanism

**Confidence: confirmed.** The claim that EL2 *rejects* rather than *fails* rests on a measured register value, not on reading the dispatcher.

**Which register holds the status.** The function starts at `ffff8000800bbc20`; the reported `+0x74` is `ffff8000800bbc94`, which the disassembly shows is the `brk` immediately after `cbz x19`. Two instructions before the `hvc`, `mov x0, #0xa` selects hypercall id 10; immediately after it, `mov x19, x0` captures the status. So `x19` is `res.a0` at the warning (`evidence/2026-08-05-warn-site-disasm-and-ids.txt`).

**What that register contains.** Across every warning of the 2026-08-05 census, `x19` is `0xffffffffffffffff` in **270 of 270**, with no variance (`evidence/2026-08-05-res-a0-distribution.txt`). `SMCCC_RET_NOT_SUPPORTED` is `-1` and `SMCCC_RET_SUCCESS` is `0` (`include/linux/arm-smccc.h:339-340`). The targeted reproduction on the following kernel reproduces the same value, with warning and disassembly taken from the same binary.

**Why `-1` identifies the rejection uniquely.** The Rust dispatcher (`hyp_main.rs:1707-1765`) raises `hcall_min` to `__pkvm_prot_finalize` once protected mode is initialised, and:

```rust
if unlikely(id < hcall_min || (id as usize) >= HOST_HCALL.len()) {
    self.regs.regs[0] = SMCCC_RET_NOT_SUPPORTED;   // :1751 — before any handler
    return;
}
if let Some(hfn) = HOST_HCALL[id as usize] {
    self.regs.regs[0] = SMCCC_RET_SUCCESS;          // :1758 — before calling it
    hfn(self);
}
```

The success path writes `SMCCC_RET_SUCCESS` *before* invoking the handler, so no handler can leave `-1` behind. `-1` can only come from `:1751`.

**The ids line up.** From the bindgen output the EL2 crate is built against, for this config: `__kvm_tlb_flush_vmid` = **10**, `__pkvm_prot_finalize` = **18**, `__pkvm_tlb_flush_vmid` = **39**. The issued id (10, read out of the disassembly) is below `hcall_min` (18), and the handle-based flush (39) is above it. Counting the enum by hand gives different numbers — it contains conditional entries — so the generated constants are the authority.

**Therefore the TLB invalidation never executes.** Not "fails": EL2 refuses the id before dispatch, so no handler runs.

## Trigger

A `KVM_MR_FLAGS_ONLY` memslot change that adds `KVM_MEM_LOG_DIRTY_PAGES` to a slot that already exists, on a host booted `kvm-arm.mode=protected`. Creating a slot with the flag already set is a different path, and `KVM_CAP_MANUAL_DIRTY_LOG_PROTECT2` must not be enabled or the kernel returns early and defers to `CLEAR` ioctls.

Hardware without `FEAT_TLBIRANGE` — both Phytium D3000 and Great Wall N90 — always takes the `!system_supports_tlb_range()` branch at `pgtable.c:654`, i.e. hypercall id 10. On TLBIRANGE-capable silicon the sibling call at `:661` uses `__kvm_tlb_flush_vmid_range`, id 11, which sits in the same pre-finalisation block and is refused the same way.

No vCPU and no guest code are needed.

## Reproduction

`repro/repro-tlbflush-warn.c`:

```
aarch64-linux-gnu-gcc -O2 -static -o repro-tlbflush repro/repro-tlbflush-warn.c
```

One run, one warning, attributable by PID and `Comm`. Harmless — the board is unaffected, unlike issue 001's reproducer.

## Blast radius

The warning itself is noise. What it reports is not.

The caller is the dirty-logging write-protect path, and EL2's `__pkvm_wrprotect` deliberately performs no TLB invalidation of its own because it relies on this flush. With the flush refused, a guest can keep writing through stale writable stage-2 TLB entries, and those writes are not recorded in the dirty bitmap — silent loss for migration or snapshot. **This consequence is derived from source and has not been demonstrated on this tree**; the measured part is that the invalidation does not run.

Nothing surfaces the failure, either: `kvm_arch_flush_remote_tlbs_range()` returns 0 unconditionally, which disables the generic layer's fall back to a full flush. The full-flush path it would have fallen back to, `kvm_arch_flush_remote_tlbs()`, *does* carry a pKVM branch and would have succeeded.

For the campaign itself the cost is throughput and blindness: the signature is in the manager's compiled-in ignore list, so a run produces hundreds of these in `dmesg` while reporting a clean bill of health.

## Fix status

> **Verified on hardware 2026-08-06** (`#13`, run [2026-08-06-fix-deploy-verify](../runs/2026-08-06-fix-deploy-verify.md)). The `pgtable.c:654` warning is gone, and the criterion this issue asked for is met: a vCPU that has run, dirty logging enabled, and a subsequent guest write landing in the dirty bitmap (`after round2: A(page 16)=1`). That check only became possible once issue **003** was fixed — before that the guest could not survive enabling dirty logging at all, so 003 was the blocker on verifying 002, not the other way round.

**Fix committed, not yet verified at runtime** — `3608223e5012` on klinux branch `pkvm-tlb-flush-range-fix` (based on the branch carrying issue 001's fix), *"KYLIN: KVM: arm64: pkvm: route range TLB flush through the VM handle"*, `+7/-2` in `mmu.c`. `mmu.o` compiles clean; checkpatch reports 0 errors, 0 warnings.

### Upstream has this one

Unlike issue 001, a fix exists upstream. `kvm_arch_flush_remote_tlbs_range()` gained its pKVM branch in mainline **`fce886a6020734d6253c2c5a3bc285e385cc5496`** — *"KVM: arm64: Plumb the pKVM MMU in KVM"*, Quentin Perret, v6.14 via kvmarm/next — backported to ACK `android16-6.12` as **`73a4f4ffe584`** (2024-12-18, `Change-Id: I58367c5b21366b9bc973efe2f36dc82d3b6dfc23`).

It is the *same* commit issue 001 cites as the reason mainline is immune to that defect. One upstream rework closes both: it moved the pinned-page accounting out of the unmap path (001) and gave the range flush a pKVM branch (002). This tree sits before it.

The commit cannot be cherry-picked: `+96/-226` across `mmu.c` and `kvm_mmu.h`, introducing the `KVM_PGT_FN()` indirection that hands non-protected guest stage-2 to pKVM wholesale — infrastructure this tree does not have, and the ACK backport itself notes "Small conflicts all over the place." Its hunk for this one function is self-contained, depends only on symbols already present here, and was taken unchanged.

ACK `android15-6.6` — the branch this lineage descends from — still lacks it at HEAD, checked directly against the branch tip rather than a local mirror.

**How it was found**, since the method is reusable: `android.googlesource.com` serves file contents anonymously but requires sign-in for history pages, so the commit could not be identified from the web. The local `kernel-refs/ack` mirror is `--depth=1`, giving file contents but no history. Two `git fetch --shallow-since` steps deepened `android16-6.12` to the branch point, at which the boundary commit no longer contained the fix — proving the change lay inside the range. A pickaxe over 29,877 commits was killed by lazy blob fetching; binary-searching the 67 commits that touch `mmu.c` found it in 6 probes.

### The reproducer cannot verify the fix

Established by review and confirmed against source. After the fix, this reproducer will stop warning **without proving anything**:

`kvm->arch.pkvm.handle` is written only in `__pkvm_create_hyp_vm()` (`pkvm.c:412`), reached from `arm.c:879` on the first vCPU run. The reproducer creates no vCPU, so the handle is still 0. `kvm_flush_remote_tlbs_memslot()` has no `pkvm_is_hyp_created()` guard, so the new branch runs anyway, and at EL2 `get_vm_by_handle()` maps handle 0 to an out-of-range index and returns null (`pkvm.rs:1208-1224`); `handle___pkvm_tlb_flush_vmid` then returns without flushing and **without touching `a0`**, which the dispatcher already set to `SMCCC_RET_SUCCESS`.

Net: no warning, no flush. Correct behaviour — a VM with no hyp VM has no EL2 stage-2 entries to invalidate — but the warning disappears because the call became a no-op.

Worth noting that the bounds check is what makes handle 0 merely useless rather than dangerous. Without `idx >= KVM_MAX_PVMS`, a host-supplied handle of 0 would be an out-of-bounds read at EL2.

**A meaningful check therefore needs a different program** from the one that exposed the defect: create a VM, run a vCPU so a hyp VM exists and pages are mapped, enable dirty logging, have the guest write, and confirm the writes land in the dirty bitmap. "The warning stopped" is not evidence. This distinction is why the `repro:` field above names the trigger program only — the verification program is a separate artifact and does not exist yet.

## Residual hazard

The fix leaves the handle-0 case reporting success while doing nothing. That is correct today, and it also removes the only signal that anything was skipped: before the fix a refused flush at least warned. If a future path can reach this function with a live guest and a zero handle, it will now fail silently.

Both `kvm_arch_flush_remote_tlbs_range()` and its sibling still `return 0` unconditionally, so the generic layer's fall-back-to-full-flush remains disabled for every caller.

The pKVM branch invalidates the whole VMID and ignores `addr`/`size`, so a dirty-logging workload takes a full stage-2 flush per memslot operation — coarser than the non-pKVM path. This matches mainline and is not worth diverging over.
