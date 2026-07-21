# Phase B re-verification on N90 — captured evidence (2026-07-21)

Board: `kylin-pc` (N90), `uname -r = 6.6.30-pkvm-fuzz`, `/proc/cmdline` has `kvm-arm.mode=protected`,
`CPU features: detected: Protected KVM`. Program: `pkvm_lifecycle.syz` (the canonical 7-call flow,
identical to `sys/linux/test/arm64-syz_kvm_setup_protected_vm`). Runs as root via `sudo`.

## Step 1 — lifecycle + kretprobe `$retval` + dmesg (`-cover=0`)

Probes: `p:pcreate pkvm_create_hyp_vm`, `r:pcret pkvm_create_hyp_vm ret=$retval`, `p:pdestroy pkvm_destroy_hyp_vm`.

```
syz.0.17-9945 [007] d....  1957.665935: pcreate:  (pkvm_create_hyp_vm+0x0/0x560)
syz.0.17-9945 [007] .....  1957.665986: pcret:    (kvm_arch_vcpu_run_pid_change+0x190/0x200 <- pkvm_create_hyp_vm) ret=0x0
syz.0.17-9945 [007] d....  1960.679780: pdestroy: (pkvm_destroy_hyp_vm+0x0/0x48)
```

- **`pcret ... ret=0x0`** — `pkvm_create_hyp_vm` returned **0 (success)**, not merely reached. This is the
  acceptance signal that was pending. ✔
- The return-probe caller frame is **`kvm_arch_vcpu_run_pid_change`** — independent confirmation that create
  fires on the **first `KVM_RUN`** (`arch/arm64/kvm/arm.c:755-759`), not at `VCPU_INIT`.
- `pdestroy` fired ~3 s after `pcreate` (see the KVM_RUN-hang finding below), on the final VM-fd close.
- dmesg diff (pKVM-focused filter, step 2): **no `pkvm`/`hyp`/oops/KASAN/WARN/BUG lines**. (A `bridge: filtering …`
  info line appears in the raw new-tail — that is benign executor network-sandbox setup, not a hyp fault.)

## Step 2 — KCOV (`-cover=1 -coverfile`)

```
call #0 openat$kvm                    : signal 735,  coverage 1234
call #1 syz_kvm_setup_protected_vm    : signal 2380, coverage 62372   (= ioctl KVM_CREATE_VM | bit31)
call #2 ioctl$KVM_CREATE_VCPU         : signal 645,  coverage 2090
call #3 ioctl$KVM_ARM_VCPU_INIT       : signal 366,  coverage 4323
call #4 ioctl$KVM_RUN                 : signal 0,    coverage 0        (hang → killed before readout)
call #5 close (vCPU)                  : signal 0,    coverage 0
call #6 close$kvmvm_protected (VM)    : signal 0,    coverage 0
```

Symbolized (`aarch64-linux-gnu-addr2line -e vmlinux`, KASLR off) — call #1's coverage lands on **real pKVM
host code**:

- **`arch/arm64/kvm/pkvm.c:457,463,465` → `pkvm_init_host_vm`** (sets `pkvm.enabled` from bit 31).
- **`arch/arm64/kvm/mmu.c` → `kvm_share_hyp`, `share_pfn_hyp`, `kvm_host_owns_hyp_mappings`, `kvm_init_stage2_mmu`**
  — the host→hyp share boundary (`__pkvm_host_share_hyp` path).
- `arch/arm64/kvm/arm.c:439-440 → kvm_arch_alloc_vm`, `reset.c:274 → get_kvm_ipa_limit`,
  `hypercalls.c → kvm_arm_init_hypercalls`, `vgic/vgic-init.c`.

Full pkvm.c hit list: `evidence/2026-07-21-pkvm-call1-coverage-pkvm-hits.txt`.

⇒ **KCOV instruments `arch/arm64/kvm/pkvm.c` and the host→hyp boundary; the campaign will accrue real pKVM
coverage.** ✔

## Findings (fold into the record)

1. **`KVM_RUN` blocks; it does not "fault out cleanly."** The executor logged `killing hanging pid 2`, and the
   ~3 s `pcreate→pdestroy` gap is the program-timeout kill followed by `close`. The create/destroy pair still
   completes (create `ret=0`), so the lifecycle is sound — but the earlier "faults out cleanly (no hang)" claim
   is wrong.
2. **The `KVM_RUN` hang costs the `pkvm_create_hyp_vm` coverage.** Because `KVM_RUN` is killed before the
   executor reads KCOV, call #4 reports `coverage 0` — so the host-side `pkvm_create_hyp_vm` path (which the
   kretprobe proves *executes*) contributes **no** coverage under this setup. The rich pKVM coverage comes from
   call #1 (`KVM_CREATE_VM` protected: `pkvm_init_host_vm` + share-to-hyp). Capturing the create-at-RUN path in
   coverage is a Phase-4/5 refinement (make the pvmfw-less vCPU exit `KVM_RUN` promptly, or shorten the run so
   readout still happens).

## Status after Phase B (steps 1–2 of 3)

- ✔ kretprobe `$retval = 0` — captured.
- ✔ real KCOV count on `pkvm.c` — captured (call #1 → `pkvm_init_host_vm` + host→hyp share path).
- ✔ clean dmesg diff — captured.
- ☐ **ramoops crash-recovery** (step 3) — NOT yet done; needs a reserved RAM region (cmdline
  `ramoops.mem_address=/mem_size=` or DT node) + a forced panic + reboot on N90. Invasive (crashes the board),
  so gated on user go-ahead.

## Step 4.4 — threaded (manager-like) run (`-threaded=1 -cover=1`)

syz-manager runs threaded by default; this run predicts campaign behavior (the -threaded=0 run does not).

```
#0 -> ioctl$KVM_RUN(0x5,...)              (thread #0 — BLOCKS, no return)
#1 -> close(0x5)                <- =0x0   (thread #1, ~1s later — RUNS)
#1 -> close$kvmvm_protected(0x4) <- =0x0  (RUNS)
handle completion: completed=7            (all 7 calls complete)
per-call coverage: #0=1269 #1(CREATE_VM)=62884 #2=2157 #3=4337 #4(KVM_RUN)=524287(cap) #5=117 #6=597
kretprobe: pcret <- pkvm_create_hyp_vm ret=0x0 ; pdestroy fires ; dmesg clean
```

- Threaded parks the hung KVM_RUN on one thread and issues the rest on another → the explicit closes RUN,
  teardown goes through them, all calls yield coverage. (Non-threaded: completed=4, closes never ran,
  teardown via fd-cleanup on worker kill.)
- KVM_RUN coverage 524287 = 0x7FFFF (KCOV cap) = **974 unique PCs repeated ~500x**. Symbolized: the loop is
  `kvm_arch_vcpu_ioctl_run -> kvm_handle_guest_abort -> inject_abt64 -> re-enter -> abort` (no-progress guest
  abort loop; pvmfw-less, no guest memory). Touches kvm_handle_guest_abort, __pkvm_create_hyp_vcpu,
  kvm_share_hyp, arch_timer, vgic — real pKVM-adjacent code, but the same set each iteration.
- Campaign implication: dedup absorbs the flood, but KVM_RUN costs ~1s/program and the fuzzer will favor it
  (adds coverage) → lean on the fast host-side lifecycle for a first supervised campaign; keep KVM_RUN
  available but not dominant. Making the vCPU exit KVM_RUN promptly is a Phase-4/5 refinement.
