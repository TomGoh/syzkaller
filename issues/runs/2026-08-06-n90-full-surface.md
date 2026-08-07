---
id: 2026-08-06-n90-full-surface
kind: campaign
board: N90 (10.42.27.17)
kernel: '6.6.103+ #16 SMP Thu Aug 6 14:37:42 — klinux pkvm-lifecycle-fuzzing @a35f0a8c899e'
cmdline: kvm-arm.mode=protected
manager_rev: 9fd30654e444ec028612efafdf1e3b00939ef685+
config: n90-full.cfg (copied below)
filters:
  config_ignores: '[] — nothing filtered at the config layer'
  reporter_ignores: 'ONE regexp only, and it is not a kernel message: `WARNING: connection is not using a post-quantum key exchange algorithm` (ssh client banner merged into the console by the isolated backend). All three pKVM/arm64 kernel WARN ignores were REMOVED for this campaign.'
enabled_syscalls: 91 of 8065 (config lists 111 entries; the manager resolves bare CallNames onto their $arm64 variants)
duration: started 2026-08-06 15:48, ongoing (13.5 h at first finding)
result: 'first campaign on a kernel carrying all five fixes, run with the full KVM surface and no kernel WARN filtering'
ring: armed on CPU 0, 32 pages / 16381 PC slots
observed: [6]
not_observed: []
---

# Run 2026-08-06-n90-full-surface

The first campaign since 001-005 were fixed and verified. Two things are deliberately different from every earlier run, and both change what a negative result from it means.

## What changed

**The full KVM surface is enabled — 91 syscalls.** Every KVM-related call present in the `linux/arm64` target is on. Two categories were previously off:

- `ioctl$KVM_ARM_VCPU_INIT` — masked during the 2026-08-05/06 work because a second `KVM_ARM_VCPU_INIT` on a vCPU that has already run was the trigger for issue 001, which wedged the board. `dev_kvm_arm64.txt` states the precondition outright: *"It also requires the pkvm_unmap_range deadlock fix on the board."* That fix is now in and verified 10/10, so the descriptor is back.
- The whole **protected-VM** surface, 16 entries: `syz_kvm_setup_protected_vm$arm64`, `ioctl$KVM_CREATE_VM_PROTECTED`, `close$kvmvm_protected`, `ioctl$KVM_CREATE_VCPU_protected`, `ioctl$KVM_ARM_VCPU_INIT_safe`, `syz_kvm_vcpu_run_immediate$arm64`, the three `syz_kvm_memslot_reject_*$arm64` composites, and `syz_kvm_pvm_info$arm64` / `syz_kvm_set_fw_ipa$arm64` / `syz_kvm_set_fw_ipa_busy$arm64`.

Other-architecture calls (`KVM_PPC_*`, `KVM_S390_*`, `KVM_ASSIGN_*`, `KVM_GET_REGS`/`SET_REGS`/`SREGS`, `KVM_INTERRUPT`, `KVM_KVMCLOCK_CTRL`, `KVM_DIRTY_TLB`) are enabled too. They return `-ENOTTY`/`-EINVAL` on arm64 and buy nothing but the breadth was asked for explicitly; they cost only generation budget.

**No kernel WARN is filtered.** Three ignores were removed from `pkg/report/linux.go`:

| removed regexp | why it was there | why it is gone |
| --- | --- | --- |
| `hyp/pgtable\.c:\d+ kvm_tlb_flush_vmid_range` | issue 002's WARN, banked 2026-07-29 | **002 is fixed** (`3608223e5012`, verified on `#13`). Keeping it would only hide a regression of a fix we now depend on. |
| `vmid\.c:\d+ kvm_arm_vmid_update` | VMID-rollover finding, fires every ~13k VM cycles | re-baselining; it is a real open EL2 bug and this campaign should see it |
| `arch_timer\.c:\d+ kvm_timer_update_irq` | generic arm64-KVM, fixed upstream by `38d7aacca092`, absent here | re-baselining; it was added in this session precisely so `KVM_ARM_VCPU_INIT` could be enabled, then removed on instruction |

The one surviving regexp matches the **ssh client's** post-quantum banner, which the isolated backend merges into the console stream where its leading `WARNING:` is otherwise parsed as a kernel oops. That is a false crash with no diagnostic content, not a kernel message, so it stays.

**The cost is understood and accepted.** Every WARN is now treated as a crash and tears the instance down. `arch_timer.c:461` historically fires within minutes of enabling the generic `KVM_ARM_VCPU_INIT`, so this campaign may spend much of its time re-triggering two already-known WARNs. That is the price of a clean baseline; the alternative — keeping filters calibrated against a kernel that carried five defects we have since fixed — hides exactly the regressions worth catching. If the log turns into a single repeated signature, the right response is a **scoped** ignore for that one signature, not a return to `"ignores": ["WARNING:"]`.

## Early state

```
15:48    started, 65 seeds, 38 skipped
15:50:40 candidates drained to 0
15:51:00 corpus=7  coverage=6377  exec total=1054
```

Coverage is flowing, which is worth stating explicitly: `coverage=0` was this project's long-standing failure mode (generated pKVM programs hanging on a bad `fw_ipa`, fixed by pinning the generated value). It is not recurring here.

Execution rate settles low — around 1 exec per 10 s after the initial burst — while the board stays healthy (load ~1.1, no D-state tasks, three live executors). That is expected rather than a stall: with `ioctl$KVM_RUN` enabled, many generated programs enter the pvmfw-less guest-abort busy loop and run until the executor's per-exec timeout.

## Board health at start

```
uptime 1:13, load 1.09
no D-state tasks, 3 syz-executor processes
dmesg: only "KVM: debugfs: duplicate directory" (cosmetic) and Bluetooth tx timeouts
no WARNING, no BUG, no Oops
```

## Known risk, accepted knowingly

**N90 still has no out-of-band crash channel.** This campaign enables the syscall that used to wedge the machine. Issue 001's fix is verified 10/10, so the probability is much lower than before, but if the board does wedge there is no remote recovery — it needs physical access. Giving N90 an out-of-band channel remains the open task it has been all along, and this run raises rather than lowers its priority.

## Left unattended overnight, 2026-08-06 ~17:50

The run was deliberately left going overnight rather than being stopped to apply coverage changes. A baseline was frozen first — `2026-08-06-n90-full-surface/baseline-2026-08-06T1748.txt` — so the morning numbers can be read as a delta instead of an absolute.

State at that point, 2h00m in:

```
corpus=504  coverage=12959  exec total=27240 (225/min)
crashes: none (crashes/ does not exist; 0 lines in manager.log match crash|WARNING|BUG:|panic|Oops)
board:   up 3:11, load 1.11, 0 D-state tasks, 3 executors, MemAvailable 21.2 GiB, Mlocked 92 kB
dmesg:   0 WARNING / BUG: / Oops / Call trace
ring:    LOST_IN_RING 0, link_dropped 0 at 9.0e10 EL2 PC deliveries
```

Hourly trajectory, which is the number to compare against in the morning:

```
15:49  corpus=0    coverage=0
16:49  corpus=401  coverage=12375
17:49  corpus=503  coverage=12956     <- +581 in the second hour
```

Coverage is decelerating steeply. **Prediction, recorded so it can be falsified:** an overnight run plateaus somewhere around 13.2–13.6k with no new hypercall handlers reached, because the four-step sequence needed to enter `__pkvm_host_dirty_log_guest` (memslot → vCPU → `KVM_RUN` → re-add memslot with `KVM_MEM_LOG_DIRTY_PAGES` → `KVM_RUN` again) is not something random generation produces. If the morning shows materially more than that, the reasoning behind the proposed `syz_kvm_dirty_log_cycle$arm64` composite is wrong and should be re-derived before any descriptor is written.

Morning triage: `issues/tools/triage-overnight.sh /home/jose/syzkaller/workdir-n90-full 10.42.27.17 issues/runs/2026-08-06-n90-full-surface/baseline-2026-08-06T1748.txt`. It separates "wedged board" from "healthy plateau" by checking the log's mtime, not only its contents — the two are indistinguishable from the numbers alone.

## Overnight result, 2026-08-07 05:35 — one finding, and the prediction checked

13.5 h in. The board never wedged, the manager never stopped, and one new signature appeared.

```
05:22:32  VM 0: crash(tail2): WARNING in kvm_unshare_hyp     <- filed as issue 006
05:22:32  start reproducing
05:26:57  failed to extract reproducer  (4m25s, "replaying the whole log did not cause a kernel crash")
05:35     corpus=753  coverage=13992  exec total=91449 (110/min)
board:    healthy throughout, 0 D-state, 3 executors, campaign still running
```

**Exactly one distinct signature in 13.5 hours, and it is a new one.** Neither of the two WARNs this run was expected to re-trigger — `vmid.c` VMID rollover, `arch_timer.c:461` — fired at all. That is itself worth recording: the "this campaign will drown in known noise" cost accepted in the section above **did not materialise**.

### The recorded prediction, scored honestly

| claim | predicted | actual | verdict |
| --- | --- | --- | --- |
| coverage plateau | 13.2–13.6k | **13992** | **slightly wrong** — 2.9% above the top of the range |
| no new hypercall handlers | 16/68 unchanged | **16/68, identical set** | **held** |

The EL2 side is flat: 516 → **525** distinct `.rs` PCs across the whole night, +9 in 11.5 h. The +1033 host-side coverage is breadth in the generic KVM/ioctl surface, not depth into EL2.

So the numeric range was a little tight, but the load-bearing claim — that random generation does not assemble the four-step dirty-logging sequence, and the composite descriptor is the only lever that opens those handlers — **survived its own falsification test**. `handle___pkvm_host_dirty_log_guest` and `handle___pkvm_host_wrprotect_guest` are still at zero after 91k executions.

## Reproducing the exact conditions

```
config:   /home/jose/syzkaller/workdir-n90-full/n90-full.cfg
workdir:  /home/jose/syzkaller/workdir-n90-full
http:     0.0.0.0:56760
manager:  bin/syz-manager @ 9fd30654e444ec028612efafdf1e3b00939ef685+
ring:     echo 32 > /sys/kernel/debug/kvm/pkvm_cov/nr_pages
          taskset -c 0 sh -c 'echo 1 > /sys/kernel/debug/kvm/pkvm_cov/enable'
```

`pkvm_cov/enable` is **write-only** — `cat` returns `Permission denied`, which is not a failure. Confirm arming from `dmesg | grep pkvm_cov`, which prints `buffer armed on CPU 0 (32 pages, 16381 PC slots)`.

## Scope

`observed` and `not_observed` are deliberately empty while the run is in progress. Neither list may be filled until the campaign ends, and `not_observed` for any issue additionally requires that the run was capable of reaching it — which, for the first time, the syscall set and the empty filter set both allow.
