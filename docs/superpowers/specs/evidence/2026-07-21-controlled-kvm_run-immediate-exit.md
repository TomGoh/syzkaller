# Controlled first KVM_RUN (immediate_exit) — verified on N90 (2026-07-21)

Primitive: `syz_kvm_vcpu_run_immediate$arm64(fd fd_kvmcpu)` — maps the shared `kvm_run`, sets
`immediate_exit=1`, then `KVM_RUN`. Program (`sys/linux/test/arm64-syz_kvm_vcpu_run_immediate`):
create protected VM → create vCPU → **PMU_V3-free** `VCPU_INIT` (target=GENERIC_V8, features=0) →
controlled run → close vCPU → close VM. Run as root, `-threaded=0 -cover=1`.

## Kernel basis (verified in /home/jose/common)
- `kvm_arch_vcpu_run_pid_change` (arm.c:717) → `pkvm_create_hyp_vm` (arm.c:757) runs from generic
  `kvm_main.c` **before** `kvm_arch_vcpu_ioctl_run` (kvm_main.c:4151 vs 4161).
- `kvm_arch_vcpu_ioctl_run` (arm.c:1030): `vcpu_load` (1041) then `if (run->immediate_exit) ret=-EINTR`
  (1043) — returns before entering the guest.
⇒ `immediate_exit=1` still builds the hyp VM, then returns `-EINTR` before the pvmfw-less guest-abort loop.

## Acceptance — all 5 PASS

```
[1] fast -EINTR:   syz_kvm_vcpu_run_immediate$arm64(0x5) = -1 errno=4 (EINTR); whole run 5s, no hang
[2] hyp create ok: pcret (kvm_arch_vcpu_run_pid_change+0x190 <- pkvm_create_hyp_vm) ret=0x0
[3] no BUG1:       dmesg diff CLEAN — no kvm_timer/arch_timer WARNING
[4] explicit dtor: pdestroy (pkvm_destroy_hyp_vm) fired; closes #5/#6 BOTH executed
                   (close vcpu cov=537, close VM cov=42166) — teardown via explicit close, not kill
[5] bounded KCOV:  call #4 (run) coverage=5457 (2276 unique PCs) — NOT the 524287 busy-loop cap
```

Per-call coverage (whole lifecycle, one fast clean run):
```
#0 openat=1259  #1 CREATE_VM(protected)=62511  #2 CREATE_VCPU=1934  #3 VCPU_INIT=4323
#4 run_immediate=5457  #5 close(vcpu)=537  #6 close(VM)=42166
```

Symbolized call #4 (the controlled run) — reaches the real hyp-VM-creation path:
```
pkvm_create_hyp_vm → __pkvm_create_hyp_vm (11 PCs) → __pkvm_create_hyp_vcpu (6) → __pkvm_vcpu_hyp_created (2)
kvm_arch_vcpu_run_pid_change (13)   kvm_arch_vcpu_ioctl_run (6)
files: arch/arm64/kvm/{pkvm.c 20, mmu.c 18, arm.c 32, arch_timer.c 72, debug.c 8, fpsimd.c 4, vmid.c 1}
```

## Why this matters (vs raw ioctl$KVM_RUN)
- `-threaded=0` raw: `completed=4`, closes never ran, `KVM_RUN` cov=0 (killed before readout), teardown via
  fd-cleanup on worker kill.
- threaded raw: all calls run but `KVM_RUN`=524287 (KCOV cap) — a busy-loop flood.
- **controlled (immediate_exit):** all 7 calls run in one 5s pass; the run yields **bounded** coverage on the
  genuine `__pkvm_create_hyp_vm`/`__pkvm_create_hyp_vcpu` path; explicit closes drive `pkvm_destroy_hyp_vm`.

⇒ This is the stable "cross the first-run host→hyp boundary" primitive for the next campaign round; raw
`ioctl$KVM_RUN` should be de-emphasised/disabled there.
