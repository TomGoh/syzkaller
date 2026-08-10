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
duration: 2026-08-10 11:32 start, stopped 23:02 (11h30m)
result: 'none of the seven known defects triggered over 11h30m; found TWO defects the surface filter was not hiding -- a pend_sync_exception WARN (previously observed 2026-07-31, never filed) and issue 008, a NULL-pointer Oops in vgic_its_save_ite reproduced 213 times'
ring: 'armed on 8 CPUs, LOST_IN_RING 0, leaked_bytes 0'
observed: [8]
not_observed: [1, 2, 3, 4, 5, 6, 7]
---

# Run 2026-08-10-n90-delivery-acceptance

Acceptance run of the delivery configuration against the shipping-shaped kernel: EL2 coverage bridge present, **no pKVM fixes**. The question this run answers is not "are there bugs" but "can this configuration run for hours on a defective kernel without tripping any of the known defects".

## What it found

None of the seven known defects fired in 11h30m -- the surface filter held. But the run was not clean, and that is the interesting part:

| finding | status |
| --- | --- |
| `WARNING in pend_sync_exception`, 1616 occurrences | previously observed 2026-07-31 (`notes/pkvm/evidence/v11-campaign-n90-2026-07-31`), **never filed as an issue**. syzkaller caught it and attempted a reproducer |
| `Internal error: Oops` in `vgic_its_save_ite`, 213 occurrences | **new** -- filed as [issue 008](../008-vgic-its-save-tables-null-collection/ISSUE.md). syzkaller did NOT record it |

The run was stopped at 23:02 once the Oops began repeating about once per second: it was hammering one input and producing no new information, and leaving it overnight risked losing the board and its live log buffer.

## Why "no known defect fired" is a meaningful result here

Only because the same kernel was shown to carry the defects first. Before this run, the reproducer pack was run on this exact build:

```
002 REPRODUCED   003 REPRODUCED   004 REPRODUCED   005 REPRODUCED   006 REPRODUCED
001 / 007        skipped (hazard: wedges-target)
```

(`delivery/evidence/2026-08-10-repro-pack-summary.txt`.) Without that half, a clean fuzzing run would be indistinguishable from a kernel that simply has no bugs.

## Numbers at the snapshot

```
duration      11h30m (stopped deliberately)
exec total    383928+         (~10/sec)
corpus        897
coverage      15060           (host+EL2 PCs, syzkaller's own counter)
EL2 handlers  18 / 30 reachable   (measured at the 5h28m snapshot)
ring          armed 8 CPUs, LOST_IN_RING 0, leaked_bytes 0
board         load ~9.8 on 8 cores, survived 213 Oopses, ssh responsive throughout
syzkaller crashes  1            <- pend_sync_exception only
dmesg WARN/Oops    1616 + 213   <- the two do NOT agree; see below
```

**The last two lines disagree, and that disagreement is itself a finding.** syzkaller's crash count is the number the report leads with, and it missed a kernel Oops entirely. Anything that its console pipeline does not see is invisible in the headline. `make-report.py` should read the board's `dmesg-history.log` as an independent second source and flag the mismatch rather than trusting one of them.

Execution is genuinely parallel: 13 executor processes with five program-runners each at ~100% CPU, `procs: 6`. The ~10/sec rate is not a concurrency limit — it is what a program costs once it actually builds a VM, enters a guest and tears it down. The early phase ran at 45-64/sec on short candidate programs that mostly never started a guest.

## What the three earlier attempts cost, and what they found

This is the fourth attempt. Each earlier one ended in a defect the surface filter had not closed, and each was found by the **empty ignores list** rather than by reading descriptions:

| attempt | ran for | ended as | the route that was missed |
| --- | --- | --- | --- |
| 1 | minutes | `WARNING in kvm_tlb_flush_vmid_range` (002) | `setup_vm()` registers a memslot with `KVM_MEM_LOG_DIRTY_PAGES`; it is in the shared arm64 VM setup, so no syscall filter reaches it |
| 2 | 31 min | `INFO: task hung in do_exit` (001) | `ioctl$KVM_ARM_VCPU_INIT_safe` — an `fd_kvmcpu` reached an argument typed `fd_kvmcpu_protected`, because resource subtypes are matched leniently |
| 3 | 19 min | tasks blocked in D state (001) | `syz_kvm_setup_cpu$arm64` takes an existing vcpu and inits it, so a vcpu that had already run got a second `KVM_ARM_VCPU_INIT` |

All three were the same shape: the trigger sat somewhere a syscall name does not describe — inside a composite's C, inside syzkaller's resource matching, inside a shared setup path. The method that actually worked was auditing the defective function's callers: for 001, all six `KVM_ARM_VCPU_INIT` sites in the executor, of which five create their own vcpu and one does not.

## Evidence

Everything is preserved under `delivery/evidence/2026-08-10-oops-vgic-its/`: a full live-buffer snapshot, the 161k-line accumulated `dmesg-history.log`, and Oops blocks with the faulting address. A reconnecting `dmesg --follow` capture was started at 23:02 and writes to `/home/jose/syzkaller/n90-console-2026-08-10.log`.

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
