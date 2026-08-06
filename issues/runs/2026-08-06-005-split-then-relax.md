---
id: 2026-08-06-005-split-then-relax
kind: reproduction
board: N90 (10.42.27.17)
kernel: '6.6.103+ #15 SMP Thu Aug 6 14:19:36 — klinux pkvm-lifecycle-fuzzing @cba248683e5c'
cmdline: kvm-arm.mode=protected
manager_rev: n/a (no syz-manager involved)
config: n/a
filters:
  config_ignores: n/a
  reporter_ignores: n/a
enabled_syscalls: n/a
duration: ~25m, two more kernels
result: 'issue 005 fixed and verified — the re-write-protect returns 0 instead of -EPERM 5/5, the dirty page is no longer lost, the teardown WARNING is gone, and 003 does not regress'
ring: n/a
observed: []
not_observed: [3, 5]
---

# Run 2026-08-06-005-split-then-relax

Two more kernels after the failed deployment earlier the same day.

| build | design | 005 criterion | 003 criterion |
| --- | --- | --- | --- |
| `#14` | pick the primitive by granule — `relax_perms` if 4 KiB, `stage2_map` if a block | **A=1 B=1**, fixed | **`rc=3`**, THP livelocked |
| `#15` | split with `stage2_map` if needed, then **always** `relax_perms` | **A=1 B=1** | **all three modes pass** |

## Result on `#15`

Five runs, with a kretprobe on `pkvm_call_hyp_nvhe_ppage` and `__stage2_wp_range`:

```
run1..5  round3[A=1 B=1]   reprotect: EPERM=0 ok=2   teardown-EPERM=0
unmap WARN: 0    donation msgs: 0    any WARNING: 0
```

Every measure moved, and the mechanism is observed rather than inferred:

- **the re-write-protect returns `0`**, twice per run — page A and the control page B — where it returned `-EPERM` 5/5 before. This is the hypercall the whole issue turns on.
- **the dirty page survives**: `A=1` in round 3, where the write was silently lost before.
- **the teardown unmap no longer fails**, and the `__unmap_stage2_range` WARNING this issue was originally filed on is gone.
- `dmesg` carries no `WARNING` at all, so issue 004 also stays fixed.
- `probe-dirtylog-thp` passes in all three modes including THP, so issue 003 did not regress.

## What `#14` taught, and why it was wrong

`#14` assumed the block case was already safe because the split leaves the target entry invalid. It does not. Reading `stage2_map_prefault_block()` **from its signature** — it had been read from the middle before — shows the skip test is

```c
	if ((ctx->level < (KVM_PGTABLE_LAST_LEVEL - 1)) ||
			(pa < ctx->addr) || (pa >= ctx->end)) {
		*ptep = pte;
```

where `pa` is a **physical** address and `ctx->addr`/`ctx->end` are **IPAs**. For a guest stage-2 those are different address spaces, so the comparison does not do what its shape suggests and the target entry is pre-populated as a valid, still write-protected leaf. The following leaf visit is then a permissions-only change — the same `-EAGAIN`-swallowed-into-`0` hole the 4 KiB case falls into.

Both branches hit the same wall, so the design collapses to one rule: **the mapping call must never be what grants write access.** `stage2_map` splits; `relax_perms` grants. The state still has to be written on the split path, because the entries the split creates are built from the prot handed in.

## Cost, recorded plainly

Three designs for this one function were wrong before one worked — `#10` (write the state back), `#12` (fall back on `-EAGAIN`), `#14` (pick by granule). Each was defensible from the code as read at the time, and each was falsified by the reproducer in about ten minutes.

Two of the three failed for the same reason: **a function was read from the middle instead of from its signature.** `stage2_map_prefault_block` was read starting at its loop, so its `pa`-versus-IPA comparison was missed; the `-EAGAIN` swallow in `kvm_pgtable_walk_continue()` was missed because only `kvm_pgtable_stage2_map()` itself had been read, not the walker underneath it. That is rule 1 of this project's CLAUDE.md, and it cost three build-and-boot cycles to relearn.

The reproducer is what made this cheap. Each wrong design survived roughly one hour of reasoning and died in ten minutes of hardware.

## Scope

Says nothing about issues 001 or 002.
