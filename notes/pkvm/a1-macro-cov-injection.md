# a1 — EL2 coverage by macro auto-injection (host→hyp)

This is the **terminal form of a1** (expanding host→hyp boundary coverage). It replaces the
per-boundary hand-instrumentation described in earlier notes and in the runbook's
"boundary-instrumentation method". Validated on N90 2026-07-28 — see
`evidence/a1-macro-autoinject-2026-07-28/FINDING.md`.

## The idea
Every host→hyp hypercall funnels through one of two macros. Inject the EL2-coverage
arm/drain **into those macros once**, and every boundary — present and future — is covered
with **zero per-call-site edits**. The successor never hand-instruments a boundary again.

## Where (kernel; stays uncommitted in common-stage2mvp, snapshot = `evidence/kernel-instrumentation-2026-07-27/stage2-el2-kcov.patch`)

`KVM_PKVM_COV_HVC(_res, f, ...)` — defined identically (idempotent via `#ifndef`) in **both**
`arch/arm64/include/asm/kvm_host.h` and `arch/arm64/include/asm/kvm_pkvm.h` (kvm_pkvm.h does
not include kvm_host.h, so both need it):

```c
#ifndef KVM_PKVM_COV_HVC
#if defined(CONFIG_PKVM_EL2_COV) && !defined(__KVM_NVHE_HYPERVISOR__)
#include <asm/kvm_pkvm_cov.h>
#define KVM_PKVM_COV_HVC(_res, f, ...)					\
	do {								\
		struct pkvm_cov_ctx __cov;				\
		bool __cov_on = pkvm_cov_begin(&__cov);			\
		arm_smccc_1_1_hvc(KVM_HOST_SMCCC_FUNC(f), ##__VA_ARGS__, &(_res)); \
		if (__cov_on)						\
			pkvm_cov_end(&__cov);				\
	} while (0)
#else
#define KVM_PKVM_COV_HVC(_res, f, ...)					\
	arm_smccc_1_1_hvc(KVM_HOST_SMCCC_FUNC(f), ##__VA_ARGS__, &(_res))
#endif
#endif
```

Injection points (the raw `arm_smccc_1_1_hvc(...)` swapped for `KVM_PKVM_COV_HVC(res, f, ...)`):
- **`kvm_call_hyp_nvhe`** (kvm_host.h) → the 46 plain sites **+** the 15 `kvm_call_hyp` /
  `kvm_call_hyp_ret` sites (their nVHE path delegates to `kvm_call_hyp_nvhe`).
- **`kvm_call_refill_hyp_nvhe`** (kvm_pkvm.h) → the 5 refill sites (incl. VM/vCPU create). Wraps
  **only the inner atomic HVC**, NOT the sleeping `__pkvm_topup_hyp_alloc` topup.

Reverted (the macro subsumes them; keeping them would double-wrap and corrupt the window):
mmu.c #23 `__pkvm_host_map_guest`, pkvm.c `__pkvm_init_vm`/`__pkvm_init_vcpu` (create) and
`__pkvm_start_teardown_vm`/`__pkvm_reclaim_dying_guest_page` (teardown).

## Why it's safe (and why "never global-wrap" no longer applies)
The old rule "never a global `kvm_call_hyp_nvhe` wrap" targeted the **naive** wrap. The two real
hazards are both handled:
1. **No sleep in the arm→drain window** — the plain macro is a single atomic HVC; refill wraps
   only its inner atomic HVC. So the RCU read-side (`pkvm_cov_begin` → ... → `pkvm_cov_end`) never
   sleeps, and `pkvm_cov_disable()`'s `synchronize_rcu()` can't free the ring under a live drain.
2. **Attribution stays exact via self-guards, not an allowlist** — `pkvm_cov_begin()` arms only
   when `ring != NULL` AND `smp_processor_id() == owner_cpu` AND `kcov_current_trace_pc()`. Every
   other system HVC (other CPUs, kernel threads, boot, out-of-KCOV windows) is a cheap no-op.
   Measured: `skip_not_owner=0`, `skip_no_kcov` accrues correctly, throughput unchanged.

Residual caveat (same as the accepted create-boundary one): some sites call this from a
**preemptible** context, so the operator must disarm the ring only when the campaign is **idle**
(never race `pkvm_cov_disable()` against a running fuzzer).

## What this does NOT cover
- **guest→hyp** HVCs (a compromised guest trapping into EL2) — that is surface **B**, needing an
  in-guest agent + a transport that survives protected-VM memory isolation. Separate frontier.
- A brand-new host→hyp *primitive macro* (not `kvm_call_hyp_nvhe`/`refill`) — unlikely, but if one
  is added, wrap its inner `arm_smccc_1_1_hvc` with `KVM_PKVM_COV_HVC` the same way.

## Operating notes
- Coverage per exec is now the union over the whole lifecycle (create+map+share+teardown+…), so
  the 32-page ring (`PKVM_COV_MAX_PAGES=32`, cap 16381) matters: busiest measured window was 10202
  PCs. If a future input drives a bigger single window, watch `el2_overflow_drains` / `LOST_IN_RING`
  and bump `PKVM_COV_MAX_PAGES` (must stay a power of two).
- Verify any new build the usual way: `LOST_IN_RING=0`, `skip_not_owner=0`, no WARN, exec/s healthy.
