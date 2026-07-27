# BUG2 (minimized) — WARNING in kvm_tlb_flush_vmid_range (N90, 2026-07-21)

`WARNING: at arch/arm64/kvm/hyp/pgtable.c:639 kvm_tlb_flush_vmid_range` on 6.6.30-pkvm-fuzz
(kvm-arm.mode=protected). Minimized with `2026-07-21-BUG2-minimizer.c` — one memslot op per case,
each under a fresh `dmesg -C` boundary:

    A) real pVM (bit31): register normal slot -> MOVE before run   => both succeed, WARN=0
    B) real pVM (bit31): register -> controlled run -> MOVE         => MOVE=-EPERM (correct), WARN=0
    C) NORMAL VM (no bit31): register slot -> enable LOG_DIRTY_PAGES => succeeds, WARN=4  <== TRIGGER

## Root cause (corrects the first guess)
The trigger is **enabling dirty-logging (KVM_MEM_LOG_DIRTY_PAGES) on a VM, on a pKVM-enabled host** —
the FLAGS_ONLY commit write-protects the region (kvm_mmu_wp_memory_region, mmu.c:1327) which flushes the
stage-2 TLB via `kvm_call_hyp(__kvm_tlb_flush_vmid, mmu)` (pgtable.c:639), and that WARNs. Full stack
(from the campaign crash report): kvm_tlb_flush_vmid_range <- kvm_flush_remote_tlbs_memslot <-
kvm_mmu_wp_memory_region [inline] <- kvm_arch_commit_memory_region <- kvm_set_memslot.

An earlier guess ("pre-run MOVE on a protected VM") is REFUTED by case A (0 warnings). It is NOT
protected-VM-specific and NOT reached by the clean protected-VM memslot slice (dirty-log on a pVM is
-EPERM before commit; case C uses a NORMAL VM). It surfaced in the memslot campaign only via old-corpus
contamination: a stale program with `KVM_CREATE_VM_PROTECTED(..., 0x5)` (no bit 31 -> a normal VM,
un-generatable under the current syzlang) then dirty-log on it.

## Status
Real, userspace-reachable host (EL1) WARN; board stays up. A candidate generic arm64-KVM / pKVM-host
dirty-log bug (likely has an upstream analogue), reproducible in 3 ioctls (create normal VM, register
slot, add LOG_DIRTY). NOT fixed (standing call). Recorded here; kept OUT of the controlled host-slice
campaign by (a) fresh corpus and (b) composite slice-2 calls that only ever build bit-31 protected VMs.
