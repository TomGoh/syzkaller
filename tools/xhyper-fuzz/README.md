# syzkaller × XHyper /dev/gunyah fuzzing (QEMU TCG)

End-to-end fuzzing of the Gunyah host interface exposed by XHyper under QEMU.

## Pieces
- Descriptions: `sys/linux/dev_gunyah.txt(.const)` in the syzkaller tree (branch `gunyah-fuzzing`, based on pkvm-lifecycle-fuzzing), committed d7fa0291f.
- Fuzz XHyper image: built from worktree `.wt-fuzz` (branch fuzz/qemu-host-net) with `platforms/kplat-aarch64/qemu_fuzz_defconfig` (host-owned virtio-mmio NIC via host_boot.rs; XHyper's own EL2 virtio-net disabled). `make build image ROOTVM_GPKG=<qemu rootvm> HOST_IMAGE=imgs/Image` (needs XHYPER_ABI_ROOT).
- Fuzz initramfs: `hostnet/fuzz-initrd.img` from `hostnet/build-fuzz-initrd.sh` (static dropbear sshd + scp, root key auth, /etc/hosts+lo+/tmp for syzkaller).
- SSH key: `hostnet/fuzz_id_rsa`.
- Manager config: `gunyah-qemu.cfg` (type qemu; kernel=xhyper.img, initrd=fuzz-initrd.img, network_device=virtio-net-device, cover=false, procs=1, enable_syscalls=gunyah set; kernel_obj points at a dir whose `vmlinux` is a symlink to XHyper's kernel.elf).

## Run
    ./run-gunyah-fuzz.sh smoke   # boot+ssh+executor+gunyah machine-check, exits 0 on success
    ./run-gunyah-fuzz.sh fuzz    # full campaign

## Hard-won integration gotchas (all fixed above)
1. Executor MUST be plain static NON-PIE (`-static -no-pie`); syzkaller's arm64 default `-static-pie` segfaults at startup in the libc-less guest.
2. Guest needs `scp` (dropbear multi), a writable `/tmp` (syzkaller upload dir), loopback `lo` UP (reverse ssh forward binds 127.0.0.1), and `localhost` resolvable (`/etc/hosts` + `hosts: files`) for the executor→manager reverse-forward.
3. `qemu_args` REPLACES syzkaller's default machine, so it must carry `-machine virt,virtualization=on,...`; syzkaller adds its own NIC/hostfwd/-kernel/-initrd.
4. `image` = a 0-byte file → initrd-only boot.
5. `kernel_obj/vmlinux` → XHyper kernel.elf (our symbolization target), else syz-manager FATALs on missing vmlinux.

## Status
Minimal loop verified 2026-09-26 (smoke-test exit 0, 18 gunyah syscalls enabled). cover=false; TCG-plugin coverage injection into pkg/rpcserver/runner.go handleExecResult is the next step.
