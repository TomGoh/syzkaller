# a1 boundary #1 — VM/vCPU create (#92/#93): +54 new EL2 lines (N90, 2026-07-28)

**The breadth lever, validated.** Instrumenting one more host→hyp boundary (VM/vCPU
create) captured EL2 code the gen composite already executed every run but wasn't
sampling — nearly doubling EL2 coverage from a single boundary.

## Change
`arch/arm64/kvm/pkvm.c` (kernel, uncommitted in common-stage2mvp; snapshot in
kernel-instrumentation-2026-07-27/stage2-el2-kcov.patch): wrap the two create HVCs
`kvm_call_refill_hyp_nvhe(__pkvm_init_vm)` (pkvm.c) and `(__pkvm_init_vcpu)` with the
same `#ifdef CONFIG_PKVM_EL2_COV` `pkvm_cov_begin`/`pkvm_cov_end` tight wrap as the #23
map site (mmu.c). Both fire on the first KVM_RUN (lazy hyp-VM creation via
kvm_arch_vcpu_run_pid_change), on the vCPU thread with KCOV active → correct attribution.

Safety (audited): the create site is preemptible (config_lock), not mmu_lock-atomic, but
PROVE_LOCKING + DEBUG_ATOMIC_SLEEP are OFF and pkvm_cov_disable() is operator-only (never
concurrent with a create in a campaign), so the naive tight wrap is safe in practice. The
refill macro's common path is one atomic HVC; it only sleeps on a rare -ENOMEM topup.
Operator caveat: disarm the ring only when the campaign is idle.

## Result (nr_pages=32, N=3, baseline.prog)
- drains 78 → **80** (create windows added); LOST_IN_RING=0; skip_not_owner=0; no WARN; rc=0.
- EL2 PCs 128 → **229** (union). **54 new .rs:line** (C_common − base32.union), incl. 6 new
  files: alloc.rs, alloc_mgt.rs, alternative.rs, kvm_mmu.rs, sys_regs.rs, deep pkvm.rs.
  init_vm=pkvm.rs:4014, init_vcpu=pkvm.rs:4166; sys_regs.rs 532/578/1595 (vcpu sysreg init);
  full hyp allocator (alloc.rs 8 lines, page_alloc.rs 5 lines).
- instability=27 (run1=223, run2/3=208): the create/allocator path has data-dependent
  run-to-run variance (real, not truncation — LOST_IN_RING=0). Stable subset (202 PCs, 54
  new lines) survives the ≥3-run deflake intersection; union accumulates over a campaign.

## Significance
EL2 source-line coverage 79 → ~133 (+68%) from ONE boundary. For contrast, a2 candidate #1
(input variation within #23) bought 7 lines. Confirms a1 (breadth) is the dominant lever
for covering the pKVM Rust codebase. Remaining host→hyp boundaries (teardown pending the
KCOV-context check, share = setup-only/low, relax-perms/PSCI/IOMMU/etc.) are the successor's
continuation; guest→hyp (B) is the separate frontier.

Artifacts: a1create.{union,common,new_pcs,new_lines,stats}, base32.union (the #23-only ref).
