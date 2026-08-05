---
id: 2026-08-05-deadlock-fix-verify
kind: reproduction
board: N90 (10.42.27.17)
kernel: '6.6.103+ #4 SMP Wed Aug  5 11:28:51 — klinux pkvm-unmap-deadlock-fix @348c94763cc6'
cmdline: kvm-arm.mode=protected
manager_rev: n/a (no syz-manager involved)
config: "identical to the #3 build config, taken from the running kernel's /proc/config.gz"
filters:
  config_ignores: n/a
  reporter_ignores: n/a
enabled_syscalls: n/a
duration: ~4m
result: 10 runs, 10 PASS, 0 hung tasks
ring: 'LOST_IN_RING=0 (ring auto-re-armed on boot: 32 pages, owner_cpu 0)'
observed: []
not_observed: [1]
---

# Run 2026-08-05-deadlock-fix-verify

Verification that `348c94763cc6` fixes issue 001, run immediately after N90 was rebooted onto the patched kernel.

## Why this run qualifies to license `not-observed`

The reproducer used is the **same unmodified binary** that hung this board deterministically, on the first attempt, earlier the same day ([2026-08-05-deadlock-repro](2026-08-05-deadlock-repro.md)) — `repro-deadlock.aarch64` from the 2026-07-29 bundle, not rebuilt and not adapted. Nothing about the test changed between the two runs.

The kernel differs from the one that failed by **the patch alone**. The tree's `.config` had drifted 22 options away from the running kernel (Rust drivers, BTF, sched_ext — from unrelated work), so the build used the config read back from the failing kernel's own `/proc/config.gz`. Both builds are `6.6.103+`, same toolchain, same source except `arch/arm64/kvm/mmu.c` and `arch/arm64/include/asm/kvm_host.h`.

## Result

```
first KVM_RUN returned -1 (errno 4) -- pages should now be pinned
second KVM_ARM_VCPU_INIT -- hangs here on an affected kernel...
PASS: second KVM_ARM_VCPU_INIT returned 0 (errno 0) -- no deadlock
EXIT=0
```

Repeated 10 times: **10 PASS, 0 FAIL**. After the series, `ps` reports **0 tasks in D state**, and `dmesg` since being cleared contains **0** matches for `WARNING:|BUG:|Oops|Unable to handle kernel|hung` — and no KVM messages at all.

## Binary-level confirmation

Independent of the runtime result, the built `vmlinux` shows the change took effect where intended:

- `__unmap_stage2_range` no longer contains `bl account_locked_vm` — it did in `#3`, at the offset matching the `+0x230` frame in the hung-task stacks.
- `bl queued_write_lock_slowpath` is likewise gone from that function: the `mmu_lock` drop/retake was removed, not merely bypassed.
- `pkvm_flush_unaccount()` was inlined; `account_locked_vm` is now called from `stage2_unmap_vm`, `kvm_uninit_stage2_mmu` and `kvm_arch_flush_shadow_memslot` — the three post-unlock sites, and only those.

## Deploy notes

Image-only, in place: same `KVER`, written over `/boot/ostree/kylin-<bootcsum>/vmlinuz-6.6.103+`, which the existing GRUB entry already points at. No GRUB edit was made — this board reports `GRUB_AUTOREGEN=yes`, so a hand-added menuentry would not have survived the next boot anyway. The previous `#3` image is kept at `/root/kernel-backups/vmlinuz-6.6.103+.build3`, and stock `6.6.0-76-generic` remains as GRUB menuentry index 1.

**Module ABI: measured, no impact.** `CONFIG_MODVERSIONS=y`, so growing `struct kvm_protected_vm` was expected to shift CRCs for exported symbols whose signature expands `struct kvm`. Tested directly rather than predicted, by loading a `#3`-era module on the `#4` kernel:

- `vfio.ko` — the only module of 1823 that references `kvm_` at all — **loads cleanly** on `#4`, exit 0, no dmesg complaint.
- Its `__versions` section contains no KVM symbol whatsoever; the only match is `__kvmalloc_node_noprof`. The `kvm_get_kvm_safe` / `kvm_put_kvm` references are `symbol_get()` string literals resolved at runtime, which bypasses modversions.
- Since boot on `#4`: **0** occurrences of "disagrees about version", "no symbol version" or "version magic" in dmesg, with 102 modules loaded.

An earlier version of this record predicted `vfio.ko` would fail to load. That prediction came from a substring grep for `kvm_` in the `.ko`, which matched `__kvmalloc_node_noprof` and the `symbol_get` literals — not from checking what the module actually version-checks. The board was restored to its pre-test state (`vfio` unloaded).
