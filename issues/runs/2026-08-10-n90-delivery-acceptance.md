---
id: 2026-08-10-n90-delivery-acceptance
kind: acceptance
board: N90 (10.42.27.17)
kernel: '6.6.103+ #27 — klinux pkvm-el2-kcov @1f52c77ba624: the EL2 coverage bridge and ring_dump, and NONE of the pKVM fixes'
cmdline: kvm-arm.mode=protected
config: CONFIG_X1_RMS=y, CONFIG_PKVM_EL2_COV=y, CONFIG_KCOV=y
filters:
  config_ignores: 'delivery/n90-delivery.cfg — 108 entries; ioctl$KVM_ARM_VCPU_INIT{,_safe}, ioctl$KVM_SET_MP_STATE, syz_kvm_setup_cpu{,$arm64} and the unconstrained memslot ioctls excluded; syz_kvm_dirty_log_cycle never enabled'
  reporter_ignores: 'none — the ignores list is deliberately empty'
enabled_syscalls: '108 config entries, 90 resolved calls'
duration: 2026-08-10 11:32 start, snapshot at 17:00 (5h28m), still running
result: 'zero crashes over 204370 executions on a kernel carrying all seven known defects; 18 of 30 reachable EL2 hypercall handlers covered'
ring: 'armed on 8 CPUs, LOST_IN_RING 0, leaked_bytes 0'
observed: []
not_observed: [1, 2, 3, 4, 5, 6, 7]
---

# Run 2026-08-10-n90-delivery-acceptance

Acceptance run of the delivery configuration against the shipping-shaped kernel: EL2 coverage bridge present, **no pKVM fixes**. The question this run answers is not "are there bugs" but "can this configuration run for hours on a defective kernel without tripping any of the known defects".

## Why zero crashes is a meaningful result here

Only because the same kernel was shown to carry the defects first. Before this run, the reproducer pack was run on this exact build:

```
002 REPRODUCED   003 REPRODUCED   004 REPRODUCED   005 REPRODUCED   006 REPRODUCED
001 / 007        skipped (hazard: wedges-target)
```

(`delivery/evidence/2026-08-10-repro-pack-summary.txt`.) Without that half, a clean fuzzing run would be indistinguishable from a kernel that simply has no bugs.

## Numbers at the snapshot

```
duration      5h28m (still running)
exec total    204370          (~10/sec)
corpus        822
coverage      14907           (host+EL2 PCs, syzkaller's own counter)
crashes       0
EL2 handlers  18 / 30 reachable
ring          armed 8 CPUs, LOST_IN_RING 0, leaked_bytes 0
board         load ~9.8 on 8 cores, ssh responsive throughout
```

Execution is genuinely parallel: 13 executor processes with five program-runners each at ~100% CPU, `procs: 6`. The ~10/sec rate is not a concurrency limit — it is what a program costs once it actually builds a VM, enters a guest and tears it down. The early phase ran at 45-64/sec on short candidate programs that mostly never started a guest.

## What the three earlier attempts cost, and what they found

This is the fourth attempt. Each earlier one ended in a defect the surface filter had not closed, and each was found by the **empty ignores list** rather than by reading descriptions:

| attempt | ran for | ended as | the route that was missed |
| --- | --- | --- | --- |
| 1 | minutes | `WARNING in kvm_tlb_flush_vmid_range` (002) | `setup_vm()` registers a memslot with `KVM_MEM_LOG_DIRTY_PAGES`; it is in the shared arm64 VM setup, so no syscall filter reaches it |
| 2 | 31 min | `INFO: task hung in do_exit` (001) | `ioctl$KVM_ARM_VCPU_INIT_safe` — an `fd_kvmcpu` reached an argument typed `fd_kvmcpu_protected`, because resource subtypes are matched leniently |
| 3 | 19 min | tasks blocked in D state (001) | `syz_kvm_setup_cpu$arm64` takes an existing vcpu and inits it, so a vcpu that had already run got a second `KVM_ARM_VCPU_INIT` |

All three were the same shape: the trigger sat somewhere a syscall name does not describe — inside a composite's C, inside syzkaller's resource matching, inside a shared setup path. The method that actually worked was auditing the defective function's callers: for 001, all six `KVM_ARM_VCPU_INIT` sites in the executor, of which five create their own vcpu and one does not.

## The 12 uncovered handlers

| handler | why |
| --- | --- |
| `__pkvm_hibernate_{prepare,save,restore,cpu_resume,finalize}` | would suspend the board; not pursued |
| `__pkvm_host_dirty_log_guest`, `__pkvm_host_wrprotect_guest` | no caller other than dirty logging, which cannot be exercised on this kernel without triggering 002/004/005 |
| `__pkvm_hyp_alloc_mgt_reclaim{,able}` | out of scope for this delivery |
| `__pkvm_prot_finalize` | boot-time only. It is *id* 18, i.e. `hcall_min` itself, so it counts as reachable by the dispatch rule while nothing at runtime calls it |
| `__pkvm_host_unmap_guest` | reachable in principle via ordinary-VM teardown; not reached in this run. **Unexplained — worth a look before the next run** |
| `__pkvm_tlb_flush_vmid` | not reached in this run; unexplained |

The last two are recorded as open questions rather than glossed: they are not known to be blocked by anything.

## Reproducing this run

```
kernel:   klinux pkvm-el2-kcov @1f52c77ba624, built with pkvm_fuzz.config
config:   delivery/n90-delivery.cfg
commands: ./delivery/pkvm-fuzz.sh check && ./delivery/pkvm-fuzz.sh start
report:   ./delivery/pkvm-fuzz.sh report
```

Full instructions, including how to read the results and what to do when the board stops answering, are in [`delivery/README.md`](../../delivery/README.md).
