# Overnight fleet run 2026-07-29/30 — two findings, one reproduced

Evidence captured **before** any reboot, because D3000's state was volatile and a
power cycle destroys it. Every claim below is labelled **verified** (captured
output) or **hypothesis** (reasoning not yet confirmed).

Campaign: single `syz-manager` driving N90 (`10.42.27.17`) and D3000
(`10.42.27.18`), both on `6.6.30-covfix #51 SMP Wed Jul 29 18:12:39`, 62 enabled
syscalls including the re-enabled `ioctl$KVM_ARM_VCPU_INIT`. Started 18:35
2026-07-29. Final campaign state: `corpus=675 coverage=13522 exec total=126188`.

---

## 1. NULL pointer dereference in `__kvm_mmu_topup_memory_cache` — REPRODUCED

**Verified.** Title:

```
BUG: unable to handle kernel NULL pointer dereference in __kvm_mmu_topup_memory_cache
```

Fault at virtual address `0x1f`, level 2 translation fault, in
`kmem_cache_alloc` (`mm/slub.c:3527`) called from `mmu_memory_cache_alloc_obj`
(`virt/kvm/kvm_main.c:409`) → `__kvm_mmu_topup_memory_cache`
(`virt/kvm/kvm_main.c:438`). Reached through `el0_svc` — **userspace-triggerable
via an ordinary syscall**. `x1 = 0` in the register dump is consistent with a
NULL `kmem_cache` pointer reaching `kmem_cache_alloc`.

**syzkaller reproduced it.** `crashes/a854c8a8*/repro.prog`, five calls:

```
r0 = openat$kvm(0x0, &(0x7f0000000040), 0x0, 0x0)
r1 = ioctl$KVM_CREATE_VM(r0, 0xae01, 0x0)
r2 = ioctl$KVM_CREATE_VCPU(r1, 0xae41, 0x0)
syz_kvm_setup_cpu$arm64(r1, r2, &(0x7f0000e8a000/0x18000)=nil,
    &(0x7f0000000080)=[{0x0, &(0x7f0000000000)=[@memwrite={0x6e, 0x30,
    @generic={0xdddd1000, 0x2f0}}], 0x30}], 0x1, 0x0, 0x0, 0x0)
ioctl$KVM_RUN(r2, 0xae80, 0x0)
```

`Repeat:true`, `Procs:1`, `Sandbox:none`. Note `KVM_CREATE_VM(..., 0x0)` — an
**ordinary** VM, not protected (no bit 31). Same shape as the `pkvm_unmap_range`
deadlock reproducer, and consistent with the pKVM-host-but-normal-VM path being
where our bugs keep landing.

Reproduction cost (`repro.stats`): extract 57s, minimise 8m36s, simplify options
1m20s. **C reproduction was attempted and failed** — `Extracting C: 33s`,
`Simplifying C: 0s`, and no `repro.cprog` was produced. So the syzlang program
reproduces but a standalone C program did not; anyone chasing this should drive
it with `syz-execprog`, not expect a C file.

Only the syzkaller-side artifacts exist. Confirming this against a mainline or
stock kernel, and finding which allocation site passes the NULL cache, is
outstanding.

## 2. 24 GB leaked out of host accounting → global OOM on D3000 — NOT root-caused

**Verified from the serial capture** (`serial/d3000-serial-oom-tlbwarn.log`,
operator-collected; it is a tail excerpt with one representative TLB warning, not
the full burst — the operator observed thousands).

At `[37344]` ≈ 10.4 h uptime, repeated:

```
WARNING: CPU: 0 PID: 361953 at arch/arm64/kvm/hyp/pgtable.c:639
         kvm_tlb_flush_vmid_range+0x210/0x248
Comm: syz.8.79  Tainted: G      D W  6.6.30-covfix #51
```

`pgtable.c:639` is `kvm_call_hyp(__kvm_tlb_flush_vmid, mmu);` — verified by
reading the file. The WARN means **EL2 returned non-success**, which is the
already-recorded finding (3), the TLB-flush WARN.

The `D` in `Tainted: G D W` means the kernel **had already oopsed** before this
warning. The Oops in §1 carried only `G W`, so the ordering was: warnings → Oops
→ more warnings (now `G D W`) → OOM.

Then 12 × global OOM starting `[37491]`, killing `systime`, `modprobe`,
`syz-executor`, `QThread`, `gvfs-afc-volume`, `gmain`, `systemd-udevd`,
`NetworkManager`, **`sshd`**, `systemd-journal`, `ukui-screensave`. All
`constraint=CONSTRAINT_NONE ... global_oom`.

Memory at the first OOM:

| counter | value |
|---|---|
| Normal zone managed | 25.3 GB |
| free | 21 MB |
| anon | 125 MB |
| file | 1 MB |
| slab unreclaimable | 1.17 GB |
| pagetables | 58 MB |
| **unaccounted** | **23.9 GB** |
| **Free swap** | **37 GB (never touched)** |

24 GB present in no host counter, while 37 GB of swap sat unused, is the
signature of pages the host **no longer owns** — donated to EL2 and not
reclaimed. The OOM killer could not free them because they are on no LRU, which
is why it killed eleven more processes and still failed.

This also explains D3000's observed state exactly: **it responds to ping but
SSH hangs, because `sshd` was OOM-killed.** It is *not* deadlocked. An earlier
call of "wedged kernel" was wrong and is retracted here.

### Why the obvious causal chain is NOT yet established

The tempting story is: TLB invalidation fails → stage-2 unmap never completes →
pages stay donated → leak → OOM. **N90 argues against it.** Same kernel, same
workload, captured live at 14:45 uptime with **1,328** `kvm_tlb_flush_vmid_range`
warnings (`target-state/n90-live-*.txt`):

```
MemTotal 24.6 GB · MemAvailable 11.2 GB · AnonPages 10.5 GB · Mapped 753 MB
```

Healthy, accounted, reclaimable. D3000 at OOM had the inverted profile —
`Mapped 8.6 GB` against `anon 125 MB`.

So longer uptime plus plenty of TLB warnings did **not** reproduce the leak.
Whatever happened on D3000 is more specific than "the TLB WARN fires". Treat the
chain as a **hypothesis** and note the one concrete link found so far: the TLB
WARN's registers (`x23=0xdddd1000`, `x26=0xdddd3000`, `x28=0xdddd2fff`) cover
exactly the guest address the §1 reproducer writes to (`@memwrite ... 0xdddd1000`).
That is suggestive, not conclusive — the fuzzer uses that address constant widely.

### What would settle it

- The **earlier** part of D3000's serial log, before the WARN burst — where the
  Oops and the onset of growth are. Only the tail was captured.
- Sampling `MemAvailable` on N90 while it keeps fuzzing, to catch onset live
  rather than post-mortem.
- Per-VM pKVM donated-page accounting, to attribute the 24 GB directly.

---

## Contents of this bundle

```
crashes/a854c8a8…/    the reproduced Oops: repro.prog, repro.report, repro.log,
                      repro.stats, report0/1, log0/1, machineInfo0/1
crashes/439c37d2…/    "lost connection to test machine" — D3000 dying of OOM
serial/               D3000 serial: the TLB WARN + all 12 OOM reports
target-state/         N90 live snapshot (meminfo, vmstat, zoneinfo, slabinfo,
                      buddyinfo, pkvm_cov stats, ps), n90-dmesg.txt,
                      n90-dmesg-history.log (1.1M lines), D3000 ping/ssh probes
campaign/             manager-overnight1.log, fleet.cfg, corpus.db
build/                kernel.config, System.map, kernel.release,
                      vmlinux build string, sha256 of vmlinux + Image
```

`vmlinux` itself (462 MB, too large to commit) is archived at
`/home/jose/evidence-archive/2026-07-30-oom-oops/vmlinux-6.6.30-covfix`,
sha256 `7df1b262b7a8638c9ccb8a5adb73c447a3a7610371b8cc35d44d2ae9cba6e2f3`.
**Without it none of the reports above can be re-symbolized** — `common-stage2mvp`
gets rebuilt routinely and the matching binary would be lost. Verify any future
symbolization against that hash.
