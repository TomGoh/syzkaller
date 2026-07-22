# Host slice 3a — protected-VM config paths via KVM_ENABLE_CAP (N90, 2026-07-22)

Reaches the pKVM `KVM_ENABLE_CAP(KVM_CAP_ARM_PROTECTED_VM)` config surface in `pkvm_vm_ioctl_enable_cap`
(`arch/arm64/kvm/pkvm.c:650`) — INFO and SET_FW_IPA — which the lifecycle and memslot slices never touch.

## Board firmware finding (corrects a shared premise)

The first design assumed N90 is pvmfw-less at the `pkvm_firmware_mem` level, so `SET_FW_IPA` would hit the
no-firmware `-EINVAL` reject (`pkvm.c:623`). A direct probe (`slice3a-enable-cap-2026-07-22/fw_probe.c`,
static arm64) showed otherwise:
```
CREATE_VM(bit31) = 4
INFO           ret=0  firmware_size=978944          <- ~956 KB GLOBAL pkvm_firmware_mem is present
SET_FW_IPA pre = ret=0                              <- succeeds: writes pvmfw_load_addr (pkvm.c:632)
KVM_RUN(imm)   ret=-1 EINTR                         <- sets pkvm.handle
SET_FW_IPA post= ret=-1 EBUSY (errno 16)           <- rejected once run (pkvm.c:627-629)
```
`SET_FW_IPA` returning 0 is impossible unless `pkvm_firmware_mem != NULL` (pkvm.c:623), and INFO's
`firmware_size` is `pkvm_firmware_mem->size` (pkvm.c:642) — so N90 **reserves a ~956 KB pvmfw region at
boot**. The only non-NULL setter in the tree is `pkvm_firmware_rmem_init` (pkvm.c:593), an FDT
`reserved-memory` node (`linux,pkvm-guest-firmware-memory`); that early-boot scan runs even though N90
exposes no `/sys/firmware/devicetree/base` (it uses ACPI for device enumeration).

**Reconciliation with `crosvm --protected-vm-without-firmware`:** that flag is a *per-VM* opt-out — crosvm
simply does not issue `SET_FW_IPA`, so the guest boots its payload directly and the hyp reads the reset
state from the host vCPU. It does not remove the boot-reserved region. So "the board reserves a pvmfw
region" and "I run pVMs without firmware" are both true and independent.

⇒ On N90 the reachable `SET_FW_IPA` outcomes are **success (pre-run)** and **-EBUSY (post-run)**, not the
no-firmware `-EINVAL`. (A board with no firmware region would instead give `-EINVAL`; the composites
still exercise the path there, only the return differs — the pseudo-calls return the raw ioctl result.)

## Composites (from fd_kvm; no raw KVM_ENABLE_CAP; errno saved across close)
- `syz_kvm_pvm_info$arm64(fd_kvm)` — create bit-31 pVM → INFO → **0** (success; `firmware_size` is global,
  not asserted).
- `syz_kvm_set_fw_ipa$arm64(fd_kvm)` — create bit-31 pVM (not run) → SET_FW_IPA → **0** (pvmfw_load_addr
  write, pkvm.c:632).
- `syz_kvm_set_fw_ipa_busy$arm64(fd_kvm)` — `pkvm_build_slotted_vm(a0,1)` (pVM that has run, handle set) →
  SET_FW_IPA → **-EBUSY** (pkvm.c:627-629). Reuses the verified slotted-VM+run helper.

A raw `ioctl$KVM_ENABLE_CAP` is deliberately NOT modeled — it would let the fuzzer aim the cap at a normal
VM or fuzz the reserved args; the composites guarantee the protected-VM precondition (pkvm.c:652) in C.

## Functional acceptance — ALL PASS (N90, syz-execprog -debug)
```
pvm_info        = 0                    cover=97084
set_fw_ipa      = 0                    cover=96825
set_fw_ipa_busy = -1 errno=16 (EBUSY)  cover=113148   (higher: adds the vCPU INIT + immediate_exit run)
dmesg: 0 WARN / 0 BUG (no BUG1 arch_timer, no BUG2 pgtable)
```

## Coverage — new pkvm.c functions (the metric to grow)
Fresh-workdir campaign (`syscalls: 13/8063`), all three composites enter the corpus
(`pvm_info`×16, `set_fw_ipa`×9, `set_fw_ipa_busy`×6); coverage ~6421, 0 crashes. Symbolized vs the prior
(lifecycle+memslot) campaign, Slice 3a adds **three new pkvm.c functions** —
`pkvm_vm_ioctl_enable_cap`, `pkvm_vm_ioctl_info`, `pkvm_vm_ioctl_set_fw_ipa` (**+16 unique pkvm.c lines**).
The pKVM host-driver surface is now 8 functions (5 lifecycle + 3 config).

## Deferred to Phase 5 (real pvmfw handoff)
The success side of SET_FW_IPA only *records* `pvmfw_load_addr`; actually loading pvmfw + the firmware
handoff / measured boot needs a with-firmware VM run (guest execution channel), which is Phase 5.

## Bundle
`slice3a-enable-cap-2026-07-22/`: `fw_probe.c` + `fw_probe-output.txt` (the firmware finding),
`rawcover.txt`, `manager.log`, `pkvm-functions-covered.txt`.
