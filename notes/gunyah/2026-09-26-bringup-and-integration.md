# Bringing up syzkaller against XHyper's /dev/gunyah (QEMU AArch64 TCG)

Date: 2026-09-26. A step-by-step record of standing up coverage-capable syzkaller
fuzzing for the Gunyah host interface (`/dev/gunyah`) exposed by **XHyper**, a
standalone Rust EL2 hypervisor that follows the C Gunyah ABI. The interesting
part of this write-up is not the happy path but the sequence of integration
problems each stage surfaced and how each was diagnosed and fixed — most were
invisible at the config/source level and only appeared when the whole chain was
run end to end.

Environment: no hardware virtualization is available for EL2 (the x86 build host
is TCG-only; the local aarch64 board lacks FEAT_NV/NV2), so everything runs under
`qemu-system-aarch64 -machine virt,virtualization=on` in TCG. The guest that
syzkaller drives is not a normal Linux image: QEMU's `-kernel` is `xhyper.img`
(XHyper + its resource manager + a Linux HLOS kernel), and XHyper boots the Linux
HLOS as its "host" VM. `/dev/gunyah` lives in that HLOS.

## Architecture

```
 syz-manager (host, aarch64) --ssh/scp--> HLOS (guest) --ioctl--> /dev/gunyah
        ^                                     |                        |
        |  extra coverage (later)             | (Gunyah driver, RM RPC)|
        +---- QEMU TCG plugin <--- EL2 -------+------- XHyper (EL2) <---+
```

Coverage of XHyper (EL2) is collected by a QEMU TCG plugin outside the guest, not
by KCOV inside it — XHyper is a separate image, not linked into the HLOS kernel.

## Stage 0a — EL2 coverage plugin

A drcov-style TCG plugin (`libxhcov`) records the translation blocks XHyper
executes at EL2 and publishes them per-vCPU in a shared-memory ring for an
external reader to drain.

Problems found and fixed while validating it against QEMU 10.2.1:

- **The instruction API has no guest physical address.** QEMU 10.2.1's plugin
  instruction interface exposes only a virtual address and a host pointer;
  guest-physical is available only for *data* accesses. Coverage must therefore
  be filtered by **virtual** address. XHyper has no KASLR and a fixed link VA, so
  a `.text` VA range from its ELF is a stable filter.
- **EL2 must be told apart from EL1 by PSTATE, not by VA alone.** XHyper (EL2)
  and the HLOS kernel (EL1) can share high VAs. The exception level is read from
  the register named exactly `"cpsr"` — there is **no** `"pstate"` or
  `"CurrentEL"` register in QEMU's AArch64 register list. Classification is done
  once and cached per translation block (a TB is EL-specific), so the register is
  not read on every execution.
- **`qemu_plugin_get_registers()` must never be called from
  `qemu_plugin_install()`** — QEMU asserts `current_cpu` and aborts the process.
  The `"cpsr"` handle is discovered lazily inside a vCPU callback instead.

Verified by booting a bare-metal payload at EL2 (`virtualization=on`) vs EL1
(no `virtualization=on`): EL2 records >0 in-range TBs, EL1 records 0.

## Stage 0b — transport: giving the HLOS a NIC + sshd under QEMU

syzkaller's `vm/qemu` needs to ssh into the guest. The QEMU HLOS came up with no
usable network and no sshd. This stage was a chain of corrections:

- **"XHyper preempts the virtio-mmio slot" was wrong.** Reading the runtime
  showed XHyper hands the HLOS the whole low device window `[0, 0x4000_0000)`
  minus firmware RAM, the GIC, and a high vdevice window — so virtio-mmio
  (0x0a000000) is *already passed through*. The real gap was that XHyper's
  **synthetic host device tree described no virtio-mmio nodes**, so Linux never
  probed them.
- **The HLOS kernel already had the driver.** An early, incomplete config
  extraction suggested `CONFIG_VIRTIO_MMIO` was missing; a full extract showed
  `CONFIG_VIRTIO_MMIO=y`, `CONFIG_VIRTIO_NET=y`, `CONFIG_VIRTIO_BLK=y` are all
  present. **No kernel rebuild was needed** — only a device-tree change.
- **Fix:** emit the 32 virtio-mmio transport nodes QEMU's virt machine provides
  (`0x0a000000 + n*0x200`, SPI `48+n`, edge-triggered) into the host device tree,
  gated on the QEMU machine so other platforms are unchanged. The gate must be
  the compile-time machine, **not** the `platform.is_none()` branch — that branch
  is test-only and never runs in a real boot. Interrupt forwarding needed no new
  code: XHyper already discovers usable SPIs and its resource manager binds every
  valid one to the HLOS (the same path the UART SPI uses).
- Confirmed on real boot: host DT gains 32 `virtio_mmio@…` nodes, `eth0` appears,
  DHCP obtains 10.0.2.15, gateway ping 2/2.
- **Contention with XHyper's own EL2 virtio-net.** XHyper's runtime has its own
  virtio-net driver that binds the same device. A fuzzing platform config disables
  it (`KFEAT_DRIVER_VIRTIO_NET` off) so the HLOS owns the single NIC cleanly; no
  core functionality is removed.
- **sshd must be static.** The initramfs is a libc-less static-busybox rootfs, so
  a dynamically linked sshd cannot run. A statically linked `dropbearmulti`
  (dropbear 2022.83, `MULTI=1` + `-static`) was built; an init hook brings up the
  interface and starts `dropbear -R -E -p 22` with root key auth.
  - Gotcha: dropbear enforces that `~/.ssh/authorized_keys` and its parents are
    owned by uid 0 and not group/other-writable. A non-root cpio repack stamps the
    builder's uid and login fails silently while the service looks up; the initrd
    is repacked with `cpio --owner 0:0`.

## Stage 2 (modeling) — syzlang for /dev/gunyah

`sys/linux/dev_gunyah.txt(.const)` models the VM lifecycle: open `/dev/gunyah`,
`GH_CREATE_VM`, `GH_VM_SET_USER_MEM_REGION`, `GH_VM_SET_DTB_CONFIG`,
`GH_VM_ADD_FUNCTION`/`GH_VM_REMOVE_FUNCTION` for VCPU/IRQFD/IOEVENTFD,
`GH_VM_START`, and vCPU run/mmap. Notes:

- Each ADD/REMOVE_FUNCTION type is a separate `ioctl$` so the `type`, `arg_size`,
  and the pointed-to argument struct stay consistent. The eventfd-backed
  functions reuse the existing `fd_event` resource.
- arm64 only. There is no in-tree `gunyah.h` to extract, so the command and enum
  values are committed in `dev_gunyah.txt.const` with `meta noextract`.
- These are plain ioctls; **no executor C support is needed** (only the
  guest-code-execution pseudo-syscalls need that, which is a later stage).
- **Verify the artifact, not the exit code.** `make descriptions` returning 0
  only means "no error"; syzkaller silently drops syscalls whose constants are
  undefined. The real check is that the compiled target actually contains them —
  decompress `sys/gen/linux_arm64.gob.flate` and confirm the gunyah syscalls are
  present there (and absent from other arches).

## Stage 2 (run loop) — the integration cascade

Standing up `syz-manager -mode smoke-test` against the image surfaced a chain of
problems, each hidden behind the previous one; each was found by running end to
end and, when needed, by executing pieces by hand in the guest.

1. **`syz-manager` FATAL: missing `vmlinux`.** It wants `kernel_obj/vmlinux` for
   symbolization at startup even with `cover:false`. Pointed `kernel_obj` at a
   directory whose `vmlinux` is a symlink to XHyper's `kernel.elf` (our actual
   symbolization target, and a valid ELF with debug info).
2. **Reverse SSH forward failed** (`remote port forwarding failed for listen port
   N`). syzkaller reverse-forwards the manager's rpc port onto the guest's
   `127.0.0.1`; the init hook brought up `eth0` but not **loopback**. Bringing
   `lo` up fixed the bind.
3. **`localhost` did not resolve.** The executor connects back to
   `localhost:<port>`; the static libc needs `/etc/hosts` with `localhost` and an
   `nsswitch.conf` `hosts: files` line. Added both to the initramfs.
4. **No writable `/tmp`.** syzkaller `scp`s `syz-executor` into `/tmp` (the
   `vm/qemu` default target dir); the initramfs had none. The init hook now
   creates a writable `/tmp`. (The guest also needs an `scp` binary in PATH —
   provided by the dropbear multi-binary.)
5. **The executor segfaulted at startup.** With everything else fixed, the ssh
   channel reported the runner dying by signal. Running `/syz-executor` by hand in
   the guest showed an immediate `Segmentation fault` even with no arguments — the
   binary itself would not start. Cause: syzkaller's arm64 default builds the
   executor as **`-static-pie`**, whose self-relocation fails in this libc-less
   guest. Rebuilding it as **plain static, non-PIE**
   (`CXXFLAGS=-no-pie LDFLAGS="-static -no-pie"`) fixed it; `/syz-executor` then
   printed its usage instead of crashing.

After these, `syz-manager -mode smoke-test` completes with exit 0: it boots the
image, ssh's in, uploads the executor, and runs the gunyah machine check with the
gunyah syscall set enabled. The XHyper console confirms the chain end to end —
the virtio-mmio SPIs (`intid 48+n`) are bound to the HLOS.

## How to run

See `tools/xhyper-fuzz/` in the workspace (`run-gunyah-fuzz.sh`, `gunyah-qemu.cfg`,
and `README.md` with the full recipe). Config highlights: `type: qemu`,
`kernel: xhyper.img`, `initrd: fuzz-initrd.img`, `network_device: virtio-net-device`,
`image` = a 0-byte file (initrd-only), `cover: false`, `procs: 1`,
`enable_syscalls` limited to the gunyah set, and `qemu_args` carrying
`-machine virt,virtualization=on,gic-version=3,dtb-randomness=on -cpu cortex-a76`
(supplying `qemu_args` replaces syzkaller's default machine, so it must include
`virtualization=on`; syzkaller adds its own NIC, hostfwd, `-kernel`, `-initrd`).

## Stability

An 11-minute `cover:false` fuzzing run (`procs:1`, TCG) was stable: the manager
never faulted (0 FATAL/panic), the VM stayed up (1 restart, from a target crash,
not a restart loop), and programs executed steadily at ~100-170/min (1030 total).

The run also produced its first findings — two guest-kernel oopses reached through
the `/dev/gunyah` surface, i.e. in the HLOS Linux Gunyah v14 driver rather than in
XHyper (EL2), which is expected for this ioctl surface:

- `Internal error in gh_vm_ioctl` (the Gunyah driver's VM ioctl handler)
- `Internal error in eventfd_release` (reached via the IRQFD/IOEVENTFD eventfd path)

Neither reproduced within the run (`repro=false`); they are leads to triage, not
yet confirmed bugs. The point for this milestone is that the harness is stable and
that real programs reach the driver and are detected when they fault.

## Still to do

- Coverage: drain the TCG plugin's per-vCPU shared memory and splice the EL2 PCs
  into `pkg/rpcserver/runner.go` `handleExecResult` (the extra-signal merge point),
  add `-plugin libxhcov.so,...,shm=/dev/shm/xh{{INDEX}}` to `qemu_args`, and set
  `cover: true`. Plugin PCs are not guest-kernel text, so keep `filter_signal` off
  for the extra set and tag the PCs to avoid colliding with kernel addresses.
- Guest-side code execution (arbitrary HVC/SMC/MMIO/sysreg sequences) via a
  SYZOS-style pseudo-syscall, mirroring `syz_kvm_setup_syzos_vm`.
