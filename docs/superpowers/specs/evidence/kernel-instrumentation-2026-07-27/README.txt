pKVM EL2-coverage kernel instrumentation — snapshot 2026-07-27
==============================================================
The kernel changes live UNCOMMITTED in the local kernel tree (by project
constraint); this patch is the safety-net snapshot of that working state.

Kernel repo : android/common
Base HEAD   : da966ce9a047bedffb02e0bdc87f3ccb5fb3f9d9  (da966ce9a047 KYLIN: xcore: rust: Fix state masking in guest_request_walker())
Branch      : 2030/bug930
Files       : 11 modified + 3 new (arch/arm64/kvm/pkvm_cov.c, hyp/nvhe/cov.c, include/asm/kvm_pkvm_cov.h)

Apply with:  git -C <kernel> apply stage2-el2-kcov.patch
Supersedes the earlier snapshots under step1-measurement-2026-07-24/ and
step2-multipage-ring-2026-07-24/ (this is the current tree state).
