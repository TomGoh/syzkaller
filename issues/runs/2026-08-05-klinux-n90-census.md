---
id: 2026-08-05-klinux-n90-census
kind: campaign
board: N90 (10.42.27.17)
kernel: '6.6.103+ #3 SMP Tue Aug 4 09:36:07 — klinux pkvm-el2-kcov @39ee2e725c12'
cmdline: kvm-arm.mode=protected
manager_rev: ca4c146418b5bff108bb17d52af5f6512c951dfa
config: 2026-08-05-klinux-n90-census/klinux-n90.cfg
filters:
  config_ignores: []
  reporter_ignores:
    - 'WARNING: connection is not using a post-quantum key exchange algorithm'
    - 'WARNING:.* at arch/arm64/kvm/vmid\.c:\d+ kvm_arm_vmid_update'
    - 'WARNING:.* at arch/arm64/kvm/hyp/pgtable\.c:\d+ kvm_tlb_flush_vmid_range'
enabled_syscalls: 61
duration: 24m24s (09:31:38 – 09:56:14)
result: corpus=271 coverage=10747 exec total=10356 (422/min)
ring: LOST_IN_RING=0 skip_not_owner=0 LOST_IN_KCOV_AREA=10913821436
observed: []
not_observed: []
---

# Run 2026-08-05-klinux-n90-census

First campaign against klinux `6.6.103+ #3` on N90 after the board was rebooted onto that build. Stopped deliberately, not by a crash, to run the targeted deadlock reproduction ([2026-08-05-deadlock-repro](2026-08-05-deadlock-repro.md)).

No kernel warning, Oops, panic or hung task was reported during the run.

## What this run could NOT observe

Both limits matter, because neither is visible from the config alone.

**`ioctl$KVM_ARM_VCPU_INIT` was removed from `enable_syscalls`** (61 syscalls, down from 62) specifically to avoid re-triggering issue 001, which had wedged both boards on 2026-07-31. So a clean run says nothing about 001, and `not_observed` is empty rather than listing it.

That mitigation is also incomplete: `syz_kvm_setup_cpu$arm64` issues `KVM_ARM_VCPU_INIT` from inside the C helper (`executor/common_kvm_arm64.h:225` → `:196`), so `syz_kvm_add_vcpu$arm64` → `ioctl$KVM_RUN` → `syz_kvm_setup_cpu$arm64` still reaches the trigger state. It did not fire in 10,356 execs, but it is generatable — 001 was avoided by luck as much as by configuration.

**The config's `"ignores": []` is not the whole filter set.** Three ignores were compiled into the `syz-manager` binary, two of them for pKVM KVM warnings (see `filters.reporter_ignores`, derived from `manager_rev`). Any warning at `arch/arm64/kvm/vmid.c` or `arch/arm64/kvm/hyp/pgtable.c:* kvm_tlb_flush_vmid_range` was invisible to this run regardless of how long it ran.

## Anomaly: 98.96% of EL2 coverage was dropped at KCOV delivery

Recorded here because it is an observation, not an inference. From `2026-08-05-klinux-n90-census/pkvm_cov-stats.txt`:

```
kcov_requested      11028383127
kcov_accepted         114561691      <- 1.04%
LOST_IN_KCOV_AREA   10913821436      <- 98.96%
LOST_IN_RING                  0
```

The ring is healthy — `LOST_IN_RING=0`, `el2_dropped_ringful=0`, `el2_hits_max=2179` against a capacity of 16381 — so the loss is entirely at the task-KCOV delivery stage. The counter is a plain identity (`pkvm_cov.c:558`: `kcov_requested − kcov_accepted − kcov_mode_lost`) and the three figures satisfy it exactly, so this is not a reporting artifact.

Clean runs in July recorded `LOST_IN_KCOV_AREA = 0` (`notes/pkvm/evidence/step2-multipage-ring-2026-07-24/`), and a previous episode that reached ~600 M was treated as a buffer-size problem and mitigated by raising `kCoverSize` 512K→4M entries (`a6e8cc02b`). This run is roughly eighteen times that episode's loss.

Consequence for reading this run: `coverage=10747` was computed from a ~1% sample of EL2 PCs. That does not make the covered lines wrong — a PC that arrived is real — but it does mean absent coverage is uninformative, which is the same instrument-versus-target confusion the runbook's first methodology rule warns about.

Not yet filed as an issue; mechanism unknown and no code has been read for it.

## Artifacts

`2026-08-05-klinux-n90-census/` — `klinux-n90.cfg` (verbatim), `manager.log.gz`, `final-stats.txt`, `pkvm_cov-stats.txt` (taken at end of run, before the reproduction).
