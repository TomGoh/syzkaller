---
id: 2026-08-05-unmap-warn-verify
kind: reproduction
board: N90 (10.42.27.17)
kernel: '6.6.103+ #4 SMP Wed Aug 5 11:28:51 — klinux pkvm-unmap-deadlock-fix @348c94763cc6'
cmdline: kvm-arm.mode=protected
manager_rev: n/a (no syz-manager involved)
config: n/a
filters:
  config_ignores: n/a
  reporter_ignores: n/a
enabled_syscalls: n/a
duration: ~2m, 3 modes x 5 runs plus one capture
result: 'mmu.c:456 unmap WARN fires 5/5 in nohuge, 0/5 in default, 0/5 in nodirty — perfectly anti-correlated with the -E2BIG of issue 003'
ring: n/a
observed: [2, 3, 4, 5]
not_observed: []
---

# Run 2026-08-05-unmap-warn-verify

An independent re-run of the three-mode matrix, made to check a colleague's report of a fourth signature before filing it as issue 005. Same kernel (`#4`), same reproducer, counts taken by this project rather than accepted from the report.

## Result

| mode | `-E2BIG` (003) | `mmu.c:456` WARN (005) | donation msg (004) | `pgtable.c:654` (002) |
| --- | --- | --- | --- | --- |
| default — THP + dirty logging | **5/5** | **0/5** | 5 | 5 |
| `nohuge` — 4 KiB + dirty logging | 0/5 | **5/5** | 5 | 5 |
| `nodirty` — THP, no dirty logging | 0/5 | **0/5** | 0 | 0 |

The reported anti-correlation reproduces exactly: the new warning fires precisely when the dirty-logging hypercall **succeeds**, and never when it fails. Everything the colleague reported about this signature is confirmed; nothing needed correcting.

Full capture in `../005-unmap-guest-fails-after-dirty-log/evidence/2026-08-05-mode-matrix-and-instance.txt`.

## A side confirmation for issue 004

The captured register dump carries `x24 = 0x0000010000000000` = 2^40. `kvm_uninit_stage2_mmu()` calls the unmap with `size = BIT(VTCR_EL2_IPA(...))`, so that register is the IPA span — **independent confirmation that this board's IPA is 40 bits**, which is the input to issue 004's magnitude closure (`kvm_mmu_cache_min_pages()` = levels − 1 = 2, matching the measured −2 pages). That number had come from the colleague's reading of the journal; it now also falls out of a register this project captured.

## Method

Counts taken after a three-second `dmesg -w` settle, because both this warning and issue 004's message are emitted at process exit — the `mmu.c:456` warning arrives via `exit_mmap → __mmu_notifier_release`. Reading `dmesg` synchronously after the program returns misses them intermittently; see [2026-08-05-dirtylog-e2big](2026-08-05-dirtylog-e2big.md) for how that produced a wrong conclusion earlier.

## Scope

Says nothing about issue 001: no second `KVM_ARM_VCPU_INIT` is issued.
