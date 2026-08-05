---
id: 2026-08-05-deadlock-repro
kind: reproduction
board: N90 (10.42.27.17)
kernel: '6.6.103+ #3 SMP Tue Aug 4 09:36:07 — klinux pkvm-el2-kcov @39ee2e725c12'
cmdline: kvm-arm.mode=protected
manager_rev: n/a (no syz-manager involved)
config: n/a
filters:
  config_ignores: n/a
  reporter_ignores: n/a
enabled_syscalls: n/a
duration: ~5m including recovery
result: hung on first attempt, deterministically
ring: n/a
observed: [1]
not_observed: []
---

# Run 2026-08-05-deadlock-repro

Targeted reproduction of the hung task that stopped the 2026-07-31 campaign on both boards, run directly on N90 after [2026-08-05-klinux-n90-census](2026-08-05-klinux-n90-census.md) was stopped cleanly. No fuzzer involved.

Reproducer: `../001-pkvm-unmap-selfdeadlock/repro/repro-deadlock.c`, built statically for aarch64 and run unmodified from the 2026-07-29 bundle — i.e. written against the `common` 6.6.30 lineage and not adapted for klinux.

Result: hung at the second `KVM_ARM_VCPU_INIT`, first try, no retries. This is what promoted issue 001 to `state: reproduced` on the klinux lineage; before this run it was `observed` only, from campaign stacks.

## Recovery

The board was recovered **without physical access**, which corrects a standing assumption in the operator runbook.

`systemctl reboot` cannot work on a board in this state, because systemd's own `/proc` walk blocks on the poisoned `mm`. Sysrq defaulted to mask `176`, which lacks the `0x40` reboot bit. So:

```
echo 1 > /proc/sys/kernel/sysrq
echo b > /proc/sysrq-trigger
```

The board came back in ~90 s onto the same kernel (GRUB index 0 is the pKVM entry), with `pkvm-cov-arm.service` re-arming the EL2 ring by itself.

This works because the kernel is not broken — only tasks touching one `mm` are stuck, so ssh still logs in and dmesg still streams. The runbook line "the first real crash ends the run" holds for a panic, not for this hang.
