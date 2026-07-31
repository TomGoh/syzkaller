# V11 campaign, first hour — three signatures on both boards

Campaign: `workdir-v11-fleet`, kernel `6.6.103+` (klinux `pkvm-el2-kcov`, source version
`82c426336042`), N90 + D3000, started 2026-07-31 09:55.

Both boards produced the *same three* signatures within ~20 minutes. Neither board
Oops'd, panicked, or went down — `grep -cE "BUG:|Oops|Unable to handle kernel"` is 0 on
both, and both still answer ssh.

| signature | N90 | D3000 |
| --- | --- | --- |
| `WARNING pgtable.c:654 kvm_tlb_flush_vmid_range` | 257 | 278 |
| `WARNING inject_fault.c:22 pend_sync_exception` | 4 | 1 |
| hung task, `account_locked_vm` under `stage2_unmap_vm` | 2 tasks | 2 tasks |

Raw evidence: `n90-dmesg-full.txt` (10044 lines), `d3000-dmesg-full.txt` (11101 lines).

---

## 1. Hung task — self-deadlock in `pkvm_unmap_guest()`  [KNOWN BUG, FIX EXISTS, NOT PORTED]

**Status: confirmed** — the stack matches a previously reproduced and fixed defect, and
the unpatched code is present in this tree (read in full at `mmu.c:320-344`).

```
task syz.1.1519:46333 blocked for more than 122/143/163/184 seconds.
  rwsem_down_write_slowpath / down_write
  account_locked_vm+0x4c/0x108
  __unmap_stage2_range+0x230/0x2e8
  stage2_unmap_vm+0x178/0x298
  kvm_arch_vcpu_ioctl+0x41c/0xc30
```

`stage2_unmap_vm()` holds `mmap_lock` for **read** across the whole unmap;
`pkvm_unmap_guest()` then calls `account_locked_vm()`, which takes the same rwsem for
**write**. A task holding an rwsem for read cannot acquire it for write, so this is an
unconditional self-deadlock, not a race. The existing code drops `mmu_lock` around the
call (`mmu.c:337-340`) because `account_locked_vm()` may sleep — but `mmu_lock` is not
the lock that blocks.

This is exactly `d7c317637aa2` on `futlab-fixes`:
**"KYLIN: KVM: arm64: pkvm: defer RLIMIT_MEMLOCK accounting out of mmap_lock"**, authored
2026-07-29, whose message documents the identical chain and records *"Verified on Phytium
D3000 and N90 (6.6.30): a standalone reproducer that hung every unpatched kernel returns 0
with this applied."* That fix landed on `android/common` `2030/bug930` and was
**deliberately not ported** into the klinux series, so `klad-v11-next` is unpatched.

Trigger per that commit: a second `KVM_ARM_VCPU_INIT` on a vCPU that has already run,
on a CPU lacking `ARM64_HAS_STAGE2_FWB`. `ioctl$KVM_ARM_VCPU_INIT` is in this campaign's
`enable_syscalls`, so the fuzzer reaches it immediately.

**Impact on the campaign**: each stuck task holds `mmap_lock` and a `kvm->srcu` read side
*forever*. They accumulate. This is the mechanism behind the earlier D3000 memory
exhaustion — srcu readers that never complete block reclaim of everything they pin.
Porting `d7c317637aa2` is the fix; nothing else in this list is load-bearing by comparison.

## 2. `kvm_tlb_flush_vmid_range` WARN — host calls a hypercall pKVM retires at finalisation

**Status: mechanism is a HYPOTHESIS from code reading — not empirically confirmed.**

What *is* verified from the logs: all 257 N90 instances share one call path, with zero
variance (257/257 by frame count):

```
kvm_vm_ioctl -> __kvm_set_memory_region -> kvm_set_memslot
  -> kvm_arch_commit_memory_region -> kvm_flush_remote_tlbs_memslot
  -> kvm_arch_flush_remote_tlbs_range -> kvm_tlb_flush_vmid_range
```

`pgtable.c:654` is not a check — it is the call itself:

```c
if (!system_supports_tlb_range()) {
        kvm_call_hyp(__kvm_tlb_flush_vmid, mmu);   /* line 654 */
```

The WARN lives inside the macro and is attributed to the call site:
`kvm_call_hyp_nvhe()` (`kvm_host.h:1257`) ends with
`WARN_ON(res.a0 != SMCCC_RET_SUCCESS)`. So EL2 returned non-success.

**This is not our instrumentation.** `KVM_PKVM_COV_HVC` (`kvm_host.h:1237-1245`) wraps
the *identical* `arm_smccc_1_1_hvc(...)` call in `pkvm_cov_begin/end` and never touches
`res`; the `WARN_ON` is pre-existing upstream code.

The unconfirmed part — why EL2 refuses. `kvm_asm.h:55-74` splits the enum with an explicit
comment, and `__kvm_tlb_flush_vmid` is on the wrong side of it:

```
/* Hypercalls available only prior to pKVM finalisation */
  ... __kvm_flush_vm_context, __kvm_tlb_flush_vmid_ipa, __kvm_tlb_flush_vmid,
      __kvm_tlb_flush_vmid_range, __kvm_flush_cpu_context, ...
  __pkvm_prot_finalize,
/* Hypercalls available after pKVM finalisation */
  ... __kvm_vcpu_run, ...
```

and `hyp_main.rs:1710-1752` raises `hcall_min` to `__pkvm_prot_finalize` once protected
mode is initialised, returning `SMCCC_RET_NOT_SUPPORTED` for anything below it. That
reading predicts a host-side bug: `kvm_tlb_flush_vmid_range()`'s `!system_supports_tlb_range()`
fallback is not pKVM-aware and calls a hypercall the hypervisor deliberately retired.

**To confirm**, capture the actual `res.a0` at the WARN (kprobe on the return, or a
temporary `WARN_ONCE` printing `res.a0`) and show it equals `SMCCC_RET_NOT_SUPPORTED`,
and that `kvm_protected_mode_initialized` is true at that moment. Until then this is a
code-only theory — do not build a fix on it. Note it is a *different* function from the
previously recorded `__kvm_flush_vm_context` VMID-rollover finding, though both ids sit
in the same pre-finalisation block, which is what suggests one shared cause.

## 3. `pend_sync_exception` WARN — exception injected with INCREMENT_PC already pending

**Status: log verified. Mechanism RETRACTED — see the correction below.**

> **Correction (2026-07-31).** An earlier version of this section explained the
> warning as a PC increment left pending by the HVC pre-increment at
> `handle_exit.c:76`. That explanation is withdrawn. `INCREMENT_PC` is `BIT(1)`
> and `EXCEPT_MASK` is `GENMASK(3, 1)` — they **overlap by design**
> (`kvm_host.h:935-942`, "Overlaps with EXCEPT_MASK on purpose"). Since the
> exception target is stored shifted left by one, any target with an odd
> encoding (`EL1_IRQ`=1, `EL1_SERR`=3, `EL2_IRQ`=5, `AA32_IABT`=1) reads back as
> `INCREMENT_PC`. So the flag being set does **not** imply a pending PC
> increment, and no `KVM_RUN` need be involved.
>
> The upstream syzbot reproducer accordingly uses two plain `KVM_SET_VCPU_EVENTS`
> calls and no `KVM_RUN`. That does not reproduce on *this* tree: four
> combinations were tested on N90 with no `KVM_RUN` at all
> (`repro-vcpuevents-4cases.c`) and all produced **zero** warnings, because here
> `kvm_inject_dabt()` on a 64-bit guest pends `EXCEPT_AA64_EL1_SYNC` (encoding 0,
> which *clears* BIT(1)) and `kvm_set_sei_esr()` bypasses the flag machinery
> entirely, writing `HCR_VSE` directly. Upstream later moved SError injection
> into the flag machinery; this 6.6.103 tree still has the older form.
>
> **Update — upstream reproducer obtained and run.** The syzbot C reproducer
> (bug `66ade132c7a9`) is exactly: open /dev/kvm, CREATE_VM, CREATE_VCPU(2),
> then two `KVM_SET_VCPU_EVENTS`, no `KVM_RUN`. Decoding its two structs, both
> set `serror_pending`, `serror_has_esr` **and** `ext_dabt_pending`
> (esr=6 then esr=1). That combination is gated in
> `__kvm_arm_vcpu_set_events()`:
>
> ```c
> if (serror_pending && has_esr) {
>         if (!cpus_have_final_cap(ARM64_HAS_RAS_EXTN))
>                 return -EINVAL;
> ```
>
> Phytium N90/D3000 have no RAS extension, so both calls return **-EINVAL
> (errno 22) before `kvm_inject_dabt()` is ever reached** — measured on N90,
> 0 new warnings. The upstream reproducer therefore cannot fire on this
> hardware, whatever the tree.
>
> Narrowing what *can* set BIT(1) here: enumerating every `kvm_pend_exception()`
> call site in this tree, only two use an odd-encoded target —
> `EXCEPT_AA64_EL2_IRQ` (nested-virt only) and `EXCEPT_AA32_IABT` (32-bit guest
> only). Our warning came from `inject_abt64` at `inject_fault.c:22`, which is
> inside the `if (likely(!vcpu_has_nv(vcpu)))` branch, i.e. a 64-bit, non-nested
> vCPU — neither of those two paths is reachable for it.
>
> Net: on this tree a genuine `kvm_incr_pc()` is once again the leading
> candidate (the original hypothesis), but it remains **unproven** — the
> HVC-based reproducer did not demonstrate it. The bit-overlap caution stands
> and is what makes "INCREMENT_PC is set" ambiguous in general; it just does not
> resolve *this* instance.

```
pend_sync_exception+0x110/0x188
inject_abt64 / kvm_inject_dabt
__kvm_arm_vcpu_set_events+0x94/0xd0     <- KVM_SET_VCPU_EVENTS from userspace
kvm_arch_vcpu_ioctl / kvm_vcpu_ioctl
```

`inject_fault.c:22` is `kvm_pend_exception(vcpu, EXCEPT_AA64_EL1_SYNC)`, whose macro
(`kvm_emulate.h:718-723`) opens with `WARN_ON(vcpu_get_flag((v), INCREMENT_PC))`. So
userspace used `KVM_SET_VCPU_EVENTS` to request a data-abort injection on a vCPU that
already had a pending PC increment — two mutually exclusive states. Low frequency (4 and 1),
non-fatal, but it is a userspace-reachable `WARN` and therefore a real finding.
