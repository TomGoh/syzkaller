---
id: 2026-08-07-006-fix-deploy-verify
kind: verification
board: N90 (10.42.27.17)
kernel: '6.6.103+ #18 SMP Fri Aug 7 10:47:05 — klinux pkvm-tlb-flush-range-fix @8bdbd1873442'
cmdline: kvm-arm.mode=protected
config: unchanged from the 2026-08-06 campaign build (CONFIG_X1_RMS=y, CONFIG_PKVM_EL2_COV=y)
filters:
  config_ignores: 'n/a — targeted reproduction, no manager involved'
  reporter_ignores: 'n/a'
enabled_syscalls: 'n/a — a single C reproducer, not a fuzzing run'
duration: 2026-08-07 11:02 boot, checks complete by 11:05
result: 'issue 006 fix confirmed: 0 WARNs and 0 KVM_CREATE_VCPU failures across 6 runs, against 3 WARNs and a poisoned-page failure on the pre-fix kernel'
ring: not armed — this run does not use EL2 coverage
observed: []
not_observed: [6]
---

# Run 2026-08-07-006-fix-deploy-verify

Targeted reproduction to decide whether `8bdbd1873442` fixes issue 006. Not a fuzzing campaign — one C reproducer, run seven times total.

## Why a reboot was mandatory

Issue 006 leaks an EL2 pin refcount **on physical pages**, and nothing recovers those pages for the life of the boot. Verifying on the running #16 kernel after installing #18 would have been meaningless: the already-poisoned pages produce `KVM_CREATE_VCPU: Invalid argument` regardless of whether the new code is correct. The board was rebooted onto #18 (`boot_id` changed, `uptime -s` 11:02:03) before any check ran.

## What was under test

`CONFIG_X1_RMS=y`, so `arch/arm64/kvm/hyp/nvhe/Makefile` builds `xhypervisor.o` and **not** `pkvm.o` / `hyp-main.o`. Confirmed after a full build: `nvhe/pkvm.o` does not exist, `xhypervisor.o` was rebuilt at 10:47. **The Rust EL2 is what ran.** The identical C fix is compile-checked only.

## Result

```
                                 pre-fix #16        post-fix #18
single run, mp_state=SUSPENDED   3 WARNs            0 WARNs
5 consecutive runs               FATAL by run 3     0 FATAL, 0 WARNs
                                 (KVM_CREATE_VCPU
                                  -EINVAL)
final dmesg                      —                  0 kvm_unshare_hyp
                                                    0 WARNING:/BUG:/Oops
control arm, mp_state=RUNNABLE   0 WARNs            0 WARNs
```

`KVM_RUN` still answers `-EINVAL` for a SUSPENDED vCPU on both kernels. That is unchanged and correct — EL2 accepts only `RUNNABLE` and `STOPPED`, and the fix concerns the cleanup on that rejection, not the rejection itself. Read dmesg, not the exit code.

## Boot health

New `boot_id`, `kvm-arm.mode=protected` present, `r8152` NIC module loaded and the board reachable, `/sys/kernel/debug/kvm/pkvm_cov/` present. `verify-boot.sh` reports **no new kernel errors versus the previous boot**; the failed units are all on the previous boot's list too, and `ksaf_main: disagrees about version of symbol module_layout` is the documented expected noise under any custom kernel.

## Scope — what this run does not establish

`not_observed: [6]` is the only claim. In particular:

- The three adjacent leaks in `2b4d43af6` (hyp-pool SVE allocation, unpinning never-pinned SVE pages, dangling `pvmfw_entry_vcpu`) are **not fixed and not tested**. Two are unreachable on N90, which has no SVE.
- The C implementation is not exercised by this board's configuration.
- Issues 001–005 were not re-run here.

## #18 changes four things, not one

It is the first deployed kernel to carry `c6c3a2503e4b` (hyp-donation accounting on a failed topup) and `22f8b90f89c7` (propagate the dirty-log granule probe's error). Both were committed after #16 was built and verified, in response to review, and had never run on hardware. A future regression on this kernel has four candidate causes.

## How the previous campaign ended

Not on its own. Rebuilding replaced `/home/jose/klinux/vmlinux` while `syz-manager` was using it, and the manager exits fatally on that by design. Final state `corpus=785 coverage=14052 exec total=113368`, one signature, 3 occurrences. **Stop the manager before rebuilding.**
