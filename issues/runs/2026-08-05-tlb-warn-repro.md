---
id: 2026-08-05-tlb-warn-repro
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
duration: <1s, one shot
result: one WARNING at pgtable.c:654, x19 (res.a0) = 0xffffffffffffffff, board unaffected
ring: n/a
observed: [2]
not_observed: []
---

# Run 2026-08-05-tlb-warn-repro

Targeted reproduction of issue 002 on the kernel N90 is currently running, to replace a cross-build inference with a self-consistent measurement. The 270 register dumps that first established `res.a0 == -1` came from [2026-08-05-klinux-n90-census](2026-08-05-klinux-n90-census.md) on `#3`, but the disassembly used to prove *which register* holds `res.a0` was taken from the `#4` vmlinux, because `#3`'s was overwritten by the next build. This run closes that gap: warning and disassembly now come from the same binary.

`repro/repro-tlbflush-warn.c` from the issue directory, built with
`aarch64-linux-gnu-gcc -O2 -static`. It registers a memslot, then re-registers the *same* slot with `KVM_MEM_LOG_DIRTY_PAGES` added — a `KVM_MR_FLAGS_ONLY` change, which is what reaches the write-protect and flush path. No vCPU, no guest code.

## Result

One warning, immediately, attributable to this process by both PID and `Comm`:

```
WARNING: CPU: 0 PID: 92660 at arch/arm64/kvm/hyp/pgtable.c:654 kvm_tlb_flush_vmid_range+0x74/0xe0
CPU: 0 PID: 92660 Comm: repro-tlbflush Not tainted 6.6.103+ #4
x20: ffff00208033d400 x19: ffffffffffffffff x18: ffff800081f5eb30
```

The program self-reports `pid=92660`, matching the warning header. Full capture in `../002-tlb-range-flush-missing-pkvm-branch/evidence/2026-08-05-repro-warn-kernel4.txt`.

`x19 = 0xffffffffffffffff` — identical to all 270 dumps from the campaign on `#3`.

The board was unaffected: no hang, no D-state task, no other warning. This reproducer is safe to run on a machine you cannot power-cycle, unlike the one for issue 001.

## The binary and the vmlinux are the same build

The warning's `x9 = ffff8000800bbc6c` falls inside `kvm_tlb_flush_vmid_range` as disassembled from the local `/home/jose/klinux/vmlinux` (function base `ffff8000800bbc20`, `and w4, w0, #0xff` at `bbc6c`), and the reported `+0x74` lands exactly on the `brk` that follows `cbz x19` in that same disassembly. Register attribution is therefore measured against the binary that produced the register dump, not inferred across builds.

## Trap: `Source Version:` does not identify the code in the binary

The kernel prints `Source Version: 39ee2e725c122bbee2cc1cec1956a23db2670543`, which is the commit *before* the deadlock fix — yet this build demonstrably contains that fix ([2026-08-05-deadlock-fix-verify](2026-08-05-deadlock-fix-verify.md): 10 of 10 passes on a reproducer that hung every kernel without it).

The string records `HEAD` at build time, so a tree built from the working directory and committed afterwards reports the parent commit forever. Identify a build by its release string and build number (`6.6.103+ #4`), or by disassembling it — never by `Source Version:` alone.

## Scope

This run says nothing about issue 001: the reproducer creates no vCPU, so it never reaches the unmap path.
