# Composite campaign — symbolized host-side coverage + Slice-3 target (N90, 2026-07-21)

After the slice-2 composite redesign (`1a8add308`) + the errno fix (`b0dffddba`), a fresh-workdir
supervised campaign (errno-fixed executor, `enable_syscalls` = 10 lifecycle + composite calls,
`syscalls: 10/8060`) was symbolized against `vmlinux` (KASLR off) to turn the raw signal count into
actual source lines. This answers "what host-side pKVM code do the composites really reach, and where
are the gaps for the next slice?"

## Campaign (supervised, plateaued)
```
corpus 0→72, coverage 0→6465, ~5.2k execs @ ~20/s, 0 crashes, 0 WARN/BUG
```
Coverage plateaus ~6465 within minutes — the enabled surface is deliberately narrow (10 calls on the
pKVM host→hyp boundary), so a multi-hour run adds no new coverage on this surface; a longer run would
only be a stability soak (already established by the ~2 h lifecycle campaign, §6.3).

Archived bundle (raw + reproducible): `composite-campaign-2026-07-21/` — `rawcover.txt`, `rawcover.sym`,
`manager.log`, `n90-dmesg.txt`, `pkvm-arm64-composite.cfg`, `provenance.txt` (vmlinux sha256 + build-id),
`symbolize.sh` (recompute from a matching vmlinux).

## Symbolized `arch/arm64/kvm` coverage (`addr2line -f -i` over the accumulated rawcover)
**947 unique lines across the whole `arch/arm64/kvm` subsystem** the lifecycle touches — NOT 947 pKVM
lines. Most is **generic arm64-KVM plumbing** the lifecycle unavoidably runs (arm.c ioctl dispatch 218,
sys_regs.c 122, arch_timer.c 147, reset.c/fpsimd.c/debug.c/pmu.c), the same code any VM create/vcpu/run
exercises. By file:
```
arm.c 218   mmu.c 159   arch_timer.c 147   sys_regs.c 122   pkvm.c 72
reset.c 24  sys_regs.h 20  fpsimd.c 20  debug.c 14  pmu.c 8  ...
```
The **pKVM-specific** surface — what this work actually targets — is: **`pkvm.c` (72)** + the **host→hyp
share/reject subset of `mmu.c`** (below). That subset, not the 947 total, is the metric to grow.

**`pkvm.c` (host-side pKVM driver) — functions covered:**
`pkvm_init_host_vm` (at CREATE_VM), `pkvm_create_hyp_vm` + `__pkvm_create_hyp_vcpu` +
`__pkvm_vcpu_hyp_created` (at first run), `__pkvm_destroy_hyp_vm` (at teardown). This is the complete
create→run→destroy host driver path — exactly the lifecycle, nothing spurious.

**`mmu.c` — memslot/share paths covered:** `kvm_arch_prepare_memory_region` (18 lines — the function
holding the composites' `-EPERM` rejects), plus the full host↔hyp share path (`kvm_share_hyp`/
`kvm_unshare_hyp`/`share_pfn_hyp`/`unshare_pfn_hyp`/`find_shared_pfn`), `stage2_unmap_vm`,
`kvm_init_stage2_mmu`, `kvm_arch_flush_shadow_*`, `kvm_arch_commit_memory_region`.

This is genuine host-side pKVM + memslot coverage — not generic MM noise.

## Slice-3 target (uncovered, clear precondition)
The **`KVM_ENABLE_CAP` config paths — INFO query + `SET_FW_IPA`** (`pkvm.c:619-647`) are **NOT covered**
(no `pkvm_vm_ioctl_enable_cap`/INFO/`SET_FW_IPA`/firmware symbols in the rawcover). They are host-side
EL1, with a clear valid precondition (a bit-31 protected VM already created), and are exactly the plan's
Step 3 (protected-VM config/rejection paths: IPA-size combos, INFO query, clean invalid-param rejects).

Design note (apply the composite lesson): model these as **composite pseudo-calls from `fd_kvm`** (or
`fd_kvmvm_protected` where a real VM token suffices) that build the pVM and issue the cap in C — do NOT
add more state-carrying resource subtypes, which syzkaller's permissive resource compatibility cannot
enforce (§6.5). Verify each is a clean `-EINVAL`/`-EPERM`/success with no new WARN, then a fresh-workdir
campaign, then re-symbolize to confirm the new pkvm.c lines.
