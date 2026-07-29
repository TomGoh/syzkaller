# Finding: WARNING in kvm_timer_update_irq (2026-07-29)

**`WARNING: CPU: 0 PID: 154224 at arch/arm64/kvm/arch_timer.c:461 kvm_timer_update_irq`**, N90 kernel 6.6.30+ #47, task `syz.2.127`. Surfaced within minutes of enabling the generic KVM target (Stage A/B). Full console capture in `report.txt`.

## Site (verified — full function read)

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
		WARN_ON(ret);            /* <-- arch_timer.c:461 */
	}
}
```

The WARN fires when **`kvm_vgic_inject_irq()` returns non-zero**, i.e. the host's vgic refused a timer interrupt injection.

## This is NOT the VMID/TLB class

Important distinction for whoever fixes these. The other two banked host↔EL2 WARNs (`kvm_arm_vmid_update`, `kvm_tlb_flush_vmid_range`) are both `WARN_ON(res.a0 != SMCCC_RET_SUCCESS)` inside the HVC wrapper `kvm_call_hyp_nvhe` (`asm/kvm_host.h:1124`) — i.e. **EL2 returning a failure status**, a pKVM hypervisor-side defect.

This one is entirely host-side: no HVC is involved, and EL2 is not implicated. It is generic arm64 KVM code (`arch/arm64/kvm/arch_timer.c`), reached because the broadened syscall set now drives vgic/timer configuration that the protected-VM composite never touched.

## Likely cause (HYPOTHESIS — not empirically confirmed)

Timer IRQ injection into a vgic that is not fully initialised: the fuzzer can now reach `KVM_RUN` on a VM whose irqchip was created but not configured (or configured inconsistently) via the newly-enabled `KVM_CREATE_IRQCHIP` / `KVM_CREATE_DEVICE` / `KVM_*_DEVICE_ATTR` ioctls, so `kvm_vgic_inject_irq()` returns an error. **Not confirmed** — no minimised reproducer was produced. Confirming it requires isolating the ioctl sequence from the corpus.

## Significance

A **userspace-reachable `WARN_ON`** in generic arm64 KVM. That is a robustness issue in its own right independent of pKVM: an unprivileged-but-KVM-capable process can spam the kernel log, and on a kernel built with `panic_on_warn=1` (which syzkaller normally sets, though **not** on this target — N90 runs `panic_on_warn=0`) it would be a denial of service.

## Provenance

- **First observed 2026-07-29** during the Stage A/B broadened run. Checked every workdir under `/home/jose/syzkaller/` — no earlier occurrence, so unlike the `kvm_tlb_flush_vmid_range` WARN (which has a 2026-07-21 precedent) this one is genuinely new to this effort.
- Reached only after the generic KVM target was enabled; the protected-only campaign never drove it.
- Not searched against upstream syzkaller dashboards — **it may still be a known upstream arm64 KVM issue**, and should be checked before reporting externally.

## Campaign handling

Suppressed campaign-wide by the config policy `"ignores": ["WARNING:"]` in `maxcov.cfg` (not by a source-level reporter edit), because a WARN that fires during ordinary fuzzing tears the VM down on every hit and collapses throughput. The full record of every occurrence is preserved on the target at `/root/syzkaller-fuzz/warn-history.log` (continuous `dmesg --follow`) and `/root/syzkaller-fuzz/dmesg-history.log` (archived on each reconnect).
