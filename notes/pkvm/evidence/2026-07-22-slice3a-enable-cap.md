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
`firmware_size` is `pkvm_firmware_mem->size` (pkvm.c:642) — so `pkvm_firmware_mem` is set with size 978944.

**Mechanism (verified — NOT the DT path).** N90 has no device tree, so the upstream FDT `reserved-memory`
setter (`pkvm_firmware_rmem_init`, pkvm.c:593) never fires. Instead `pkvm_firmware_mem` is set by the
**vendor** `xcore_pkvm_dice_init()` (arm.c:2654, called at arm.c:2755): the kernel is built with
`CONFIG_EXTRA_FIRMWARE="pvmfw.bin"` (source `common/firmware/pvmfw.bin`, **970992 B**, **file** SHA-256
`c368e64b6dfd…`), so `request_firmware_direct("pvmfw.bin")` finds the **built-in** copy (there is no
`/lib/firmware/pvmfw.bin` file), copies it into freshly allocated pages, and sets `pkvm_firmware_mem`
(arm.c:2691) with `size = PAGE_ALIGN(970992 + 4096) = 978944` (firmware + one DICE-config page). The boot
log hashes the **loaded region** (firmware + DICE page + padding, NOT the raw file), which is why it
differs from the file hash: `SHA-256 Hash of custom_pvmfw.bin: b72048ef…`. So **there IS a real pvmfw on N90** — a
built-in binary, loaded and resident at boot — not a bare reservation and not absent. (DICE chain retrieval
from the secure world failed — `tee_client_open_session failed` — but that is non-fatal, arm.c:2711.)

**Reconciliation with `crosvm --protected-vm-without-firmware`** (verified against the crosvm source): that
flag selects `ProtectionType::ProtectedWithoutFirmware` ("booted directly without pVM firmware… won't be
given any secrets"). crosvm only calls `load_protected_vm_firmware` — which issues `SET_FW_IPA` — for
protection types that `needs_firmware_loaded()`; `ProtectedWithoutFirmware` skips it, so that VM never
issues `SET_FW_IPA` and boots its payload directly. It's a *per-VM* opt-out; the board's built-in pvmfw is
still loaded and available. So "the board has a resident pvmfw" and "I run pVMs without firmware" are both
true and independent.

⇒ On N90 the reachable `SET_FW_IPA` outcomes are **success (pre-run)** and **-EBUSY (post-run)**, not the
no-firmware `-EINVAL`. (A board where `request_firmware("pvmfw.bin")` fails — no built-in and no
`/lib/firmware` copy — would leave `pkvm_firmware_mem == NULL` and give `-EINVAL`; the composites still
exercise the path there, only the return differs — the pseudo-calls return the raw ioctl result.)

## Composites (from fd_kvm; no raw KVM_ENABLE_CAP; errno saved across close)
- `syz_kvm_pvm_info$arm64(fd_kvm)` — create bit-31 pVM → INFO → **0** (success; `firmware_size` is global,
  not asserted).
- `syz_kvm_set_fw_ipa$arm64(fd_kvm)` — create bit-31 pVM (not run) → SET_FW_IPA → **0** (pvmfw_load_addr
  write, pkvm.c:632).
- `syz_kvm_set_fw_ipa_busy$arm64(fd_kvm)` — `pkvm_build_slotted_vm(a0,1)` (pVM that has run, handle set) →
  SET_FW_IPA → **-EBUSY** (pkvm.c:627-629). Reuses the verified slotted-VM+run helper.

A raw `ioctl$KVM_ENABLE_CAP` is deliberately NOT modeled — it would let the fuzzer aim the cap at a normal
VM or fuzz the reserved args; the composites guarantee the protected-VM precondition (pkvm.c:652) in C.

The fixed `PKVM_FW_IPA = 0x7FC00000` matches crosvm's arm64 pvmfw window start
(`AARCH64_PHYS_MEM_START 0x80000000 − 4 MiB`, crosvm `aarch64/src/lib.rs:114-119`) — page-aligned and
distinct from the Slice-2 memslot GPA (0x40000000). `SET_FW_IPA` only *records* it (pkvm.c:632, no range
check), so the address is not yet exercised as a guest load address here; a real firmware handoff (Phase 5)
must place actual guest memory at this window and run with firmware.

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
The success side of SET_FW_IPA only *records* `pvmfw_load_addr`; actually loading pvmfw into the guest + the
firmware handoff / measured boot needs a with-firmware VM run (guest execution channel), which is Phase 5.
Since N90 already carries a resident built-in pvmfw (above), Phase 5 is viable here without supplying a
firmware binary — the missing piece is the guest execution channel, not the firmware.

## Bundle
`slice3a-enable-cap-2026-07-22/`: `fw_probe.c` + `fw_probe-output.txt` (the firmware finding),
`rawcover.txt`, `rawcover.sym`, `manager.log`, `pkvm-functions-covered.txt`, `provenance.txt` (vmlinux
sha256 + build-id, executor sha256, pvmfw.bin sha256 + loaded-region hash, kernel release, commit), and
`symbolize.sh` to recompute the pkvm.c attribution from a matching vmlinux.
