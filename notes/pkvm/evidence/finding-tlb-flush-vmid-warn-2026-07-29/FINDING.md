# Finding: EL2 __kvm_tlb_flush_vmid returns non-success (WARN) — found by the generic KVM target (2026-07-29)

**WARNING**, N90 kernel 6.6.30+ #47, tripped by a fuzzer program (`syz.2.155`) the moment the generic
(non-protected) KVM target was enabled (Stage A).

- **Site:** `WARNING at arch/arm64/kvm/hyp/pgtable.c:639 kvm_tlb_flush_vmid_range`. Line 639 is
  `kvm_call_hyp(__kvm_tlb_flush_vmid, mmu)` inside the `!system_supports_tlb_range()` branch — so the
  WARN is the `WARN_ON(res.a0 != SMCCC_RET_SUCCESS)` **inside the HVC macro**: the EL2
  `__kvm_tlb_flush_vmid` handler returned a **non-success SMCCC status** for a flush the host issued
  while servicing a generic-KVM memory operation.
- **Class:** same as the VMID-rollover WARN (`vmid.c:69 kvm_arm_vmid_update`, __kvm_flush_vm_context) —
  a host↔EL2 flush HVC returning non-success — but a **different, newly-reached path** (`__kvm_tlb_flush_vmid`,
  census #11). Whether it's a real EL2 bug or the EL2 being strict about VMID/mmu state on a
  fuzzer-created non-protected VM is the kernel owner's call.
- Full dmesg: `kvm_tlb_flush_vmid-warn-dmesg.txt`.

## Significance (validates the input-expansion reversal)
This is the first hardware evidence that enabling the ordinary KVM target (which the campaign had
disabled) **reaches EL2 code the protected composite never drove** and finds host↔EL2 issues there.
During the Stage-A run: `drains=11987`, `el2_hits_max=10113`, `LOST_IN_RING=0` — real EL2 capture from
the generic ops, plus this WARN. Confirms the earlier "composite lever exhausted" read was wrong.

## Operational consequence
The manager treats the WARN as a crash and (with `type: isolated`) re-reads the persistent dmesg report
on each reconnect → crash-loop (exec stalled at 153, corpus not building). Sustained Stage A/B requires
suppressing this WARN in the reporter (regex on `pgtable.c:\d+ kvm_tlb_flush_vmid_range`), same as the
VMID WARN, and clearing dmesg (`dmesg -C`) so the stale report isn't re-detected. Stage B (guest exec)
will likely surface further WARNs to triage the same way.
