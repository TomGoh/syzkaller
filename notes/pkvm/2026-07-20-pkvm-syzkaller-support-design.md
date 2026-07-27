# Fuzzing this kernel's Rust pKVM (arm64 EL2) with syzkaller — Design

> ⚠️ **HISTORICAL DESIGN — not the current source of truth.** The authoritative, hardware-verified record of what
> was actually built and how it behaves is **`2026-07-20-pkvm-syzkaller-v1-implementation-record.md`**. Read that
> first; treat this design as the original plan plus Phase-5 ideas. Two assumptions below were **corrected by the
> implementation** and must not be followed as-is:
> 1. **v1 is pvmfw-less.** The "stable helper runs `INFO` → `SET_FW_IPA` → register a firmware-covering region"
>    flow (§5.x here) is a **Phase-5** with-firmware variant, *not* what v1 does — v1 never issues `SET_FW_IPA`.
> 2. **The hyp VM is created at the first `KVM_RUN`, not at `KVM_ARM_VCPU_INIT`.** `pkvm_create_hyp_vm()` runs in
>    `kvm_arch_vcpu_run_pid_change()` (arm.c:717), which generic `kvm_main.c` calls on the vCPU's first run. Any
>    text here saying "hyp VM object is created … at `KVM_ARM_VCPU_INIT`" is stale — see record §0/§6.2.
>
> Refreshed design, 2026-07-20. Supersedes the 2026-07-11 plan (`~/.claude/plans/let-me-add-some-recursive-pony.md`), re-grounded against the **current** trees: kernel `common@2030/bug930` (HEAD `da966ce9a047`) and `syzkaller@claude/syzkaller-source-learning-vvisp3` (HEAD `3ccdc5297`). Every `file:line` below was re-verified on 2026-07-20; where the old plan drifted, the delta is called out. Anchor table in Appendix B.

---

## 0. Feasibility verdict

Fuzzing this kernel's pKVM with syzkaller is **feasible**, and splits into a half that works with today's syzkaller and a half that is genuine research:

- **Reachable today.** syzkaller ships mature arm64 KVM fuzzing via **SyzOS** (`executor/common_kvm_arm64_syzos.h`), a tiny in-guest library that already issues fuzzer-controlled `hvc #0` / `smc #0` with x0–x5 (`:287-330`). A guest HVC traps straight to EL2, so the **guest→hyp** path is exercisable now — it just doesn't yet know this kernel's pKVM function IDs.
- **Host-side (EL1) coverage works out of the box** on native arm64 hardware. The nVHE hyp itself is deliberately uninstrumented (`KCOV_INSTRUMENT := n`, see §1), but the EL1 host half of KVM (`arch/arm64/kvm/*.c`) is instrumented normally.
- **Coverage from *inside* the Rust EL2 hypervisor is the research investment** (Stage 2). EL2 is a separate world with a `__kvm_nvhe_`-prefixed, Rust-mangled symbol namespace; two-object symbolization is the crux. Its DWARF half is already de-risked (Appendix A).

Your three asks map onto three stages:

| Ask | Stage | Status |
|---|---|---|
| Physical DUT from `common` + gwd3000 defconfig + KCOV/syzkaller configs | **Stage 1** — pipeline bring-up | Standard isolated-mode + ramoops/pstore + SBSA watchdog |
| syzlang definition for the pKVM part of KVM | **Stage 1.5** — ABI modeling | Extend `kvm_smc_id`; add a protected-VM pseudo-call + a structured HVC command |
| EL2 KCOV support | **Stage 2** — in-hyp coverage | Prototype-first research direction (follow-on); symbolization + the coverage-boundary/teardown contract are the open work |

**Scope of the first implementation pass:** Stage 1 + Stage 1.5. Stage 2 is sketched here as a **prototype-first research direction** — its coverage-boundary/teardown contract (§6.3) and symbolization blocker (§6.4) must be closed before it is implementation-grade — and runs as its own follow-on project.

**Which hypervisor actually runs? Read this first — it decides what code the fuzzer can reach.** `CONFIG_X1_RMS` is `default y` (`arch/arm64/kvm/Kconfig:136`), and under it the **C** pKVM HVC handlers are `#ifndef CONFIG_X1_RMS`-compiled *out* (`arch/arm64/kvm/hyp/nvhe/pkvm.c:1663`) and replaced by the **Rust** hyp. This tree therefore runs the Rust EL2 code, whose guest-HVC surface is currently **thin**: normal-VM exits use only generic handlers (`hyp_main.rs:2085-2108` — no `HYP_MEMINFO`/`MEM_RELINQUISH`), and the protected-VM `handle_pvm_entry_hvc64` merely **stubs** `MEM_SHARE`/`MEM_UNSHARE`/`MEM_RELINQUISH` to `SMCCC_RET_SUCCESS` with no MMIO-guard / memory-interface logic (`hyp_main.rs:2171-2183`). The design forks on this config:

- **`CONFIG_X1_RMS=y` (this tree's default):** the Rust handlers are authoritative. Guest-HVC fuzzing (Strategy 2/3) reaches far less deep code than the C analysis below implies — verify/augment the Rust HVC implementation before expecting depth there. The well-implemented, high-value target is **Strategy 1** (the host→hyp protected-VM lifecycle), which the Rust hyp *does* implement.
- **`CONFIG_X1_RMS=n`:** the standard **C** pKVM HVC reachability (the `pkvm.c:1697/1767` analysis in §5.1/§5.4) applies.

What is genuinely **identical C-or-Rust** is the **host→hyp user ABI**: the `/dev/kvm` ioctls and the protected-VM lifecycle (Strategy 1), which live in the always-compiled host-side `arch/arm64/kvm/*.c`. So Stage 1 and the *host-side* of Stage 1.5 are implementation-agnostic; the *guest-side* HVC reachability (§5.1/§5.4) is **not**. The C-vs-Rust choice also drives Stage 2 (which object to instrument/symbolize) and the oracle (Rust panics vs. KASAN).

---

## 1. Why EL2 coverage is custom work (the KCOV-disable finding)

The nVHE hyp explicitly disables all compiler instrumentation, at `arch/arm64/kvm/hyp/nvhe/Makefile.nvhe:88-95`:

```make
# KVM nVHE code is run at a different exception [level] with a different map, so
# compiler instrumentation that inserts callbacks or checks into the code may
# cause crashes. Just disable it.
GCOV_PROFILE   := n
KASAN_SANITIZE := n
KCSAN_SANITIZE := n
UBSAN_SANITIZE := n
KCOV_INSTRUMENT := n
```

This is load-bearing for the whole design:

1. **You cannot get EL2 coverage by flipping `CONFIG_KCOV_INSTRUMENT_ALL`.** The stock `__sanitizer_cov_trace_pc` dereferences `current->kcov`; at EL2 there is no `current` and a different memory map — it crashes exactly as the comment warns. Stage 2 therefore adds a *hyp-local* `__sanitizer_cov_trace_pc` writing to a shared ring, and re-enables SanCov **only for the Rust crate** via `build_rust.sh`'s `RUSTFLAGS` — never via this Makefile.
2. **The disable is scoped to `nvhe/` only.** The EL1 host KVM code is instrumented normally, so Stage 1's host-side coverage is unaffected. This Makefile line *is* the boundary between "works today (EL1)" and "research (EL2)".
3. **`KASAN_SANITIZE := n` means EL2 has no KASAN** — so the Stage-2 oracle for EL2 memory-safety violations is **Rust panics** (`panic=abort` + overflow/bounds checks), not KASAN (§6.5).
4. **The two-object symbolization problem is created two lines away:** `Makefile.nvhe:77` `cmd_hypcopy = $(OBJCOPY) --prefix-symbols=__kvm_nvhe_` prefixes every hyp symbol, and the hyp is partially linked separately (`Makefile.nvhe:29-67`). Runtime EL2 PCs therefore resolve against a different object than `vmlinux` EL1 PCs (§6.4).

The Rust object (`nvhe_rust.o`) is *also* uninstrumented today — `build_rust.sh:52` passes no SanCov flags — so neither C nor Rust hyp emits coverage yet. Consistent.

---

## 2. Environment (verified)

- **Manager host** = the x86_64 box (this machine). Go toolchain present; the `aarch64-linux-gnu-*` cross toolchain is installed, satisfying both syzkaller cross-build and coverage symbolization (`docs/linux/coverage.md:18`, `sys/targets/targets.go:300,754-755,820-829` derive the `aarch64-linux-gnu-` prefix from `target: linux/arm64`). The manager **must** be a separate machine from the DUT (isolated mode reboots the DUT).
- **DUT** = **`N90`** (`ssh N90`), a physical Kylin V10 **Phytium / gwd3000-class** board — the same family as the `gwd3000_lenovox1d3000m_defconfig` base. Passwordless `su` is configured; Ethernet + serial available; an ARM SBSA GWDT watchdog can be added. This is the correct fuzzing host (see the KCOV note below). **Two setup facts:** (i) the isolated backend drives the DUT over *SSH* (never `su`) and the executor needs `/dev/kvm`, so the manager's `ssh_user` must log in **key-based** and reach `/dev/kvm` (root ssh, or a user in the `kvm` group / a device ACL) — passwordless `su` alone doesn't satisfy the SSH path; preflight per §4.1. (ii) **N90 must first be booted on the pKVM fuzzing kernel** (this defconfig + §4.1 deltas) — it does not run it out of the box; deploy via the existing Kylin-V10 kernel-deploy flow.
- **Rust hyp** lives at `arch/arm64/kvm/hyp/nvhe/rust/` (mirror `~/nvhe_rust_kcov_real`). Toolchain (authoritative, installed default): **`nightly-2025-05-05` = rustc 1.88.0-nightly, LLVM 20.1.4**. It x86-hosts and cross-compiles to bare-metal target **`aarch64-unknown-none`** (note: *not* `-softfloat`; `-C target-feature=-neon` supplies the soft-float behavior) via `-Zbuild-std=core,compiler_builtins,alloc` (`build_rust.sh:30,52,68`). Build: `staticlib libnvhe_rust.a` → `ld -r --whole-archive` → `objcopy --remove-section=.llvmbc` → single `nvhe_rust.o` (`build_rust.sh:85-90`); `panic=abort`, `opt-level=3`.
- **Toolchain-pinning hazard, still present:** `build_rust.sh:29` sets `RUSTUP_TOOLCHAIN=nightly` (floating), which grabs a newer rustc/LLVM on this box instead of 1.88/LLVM 20.1.4. **Fix first** (pin to `nightly-2025-05-05` or add a `rust-toolchain.toml`) — mixing rustc versions across the `lto`/build-std crate is the primary Stage-2 build hazard.
- **Config → Rust cfg bridge:** `build_rust.sh:61-63` appends `@include/generated/rustc_cfg` to `RUSTFLAGS` when present, so kernel `CONFIG_*` reach the Rust build as cfgs (this is how a Stage-2 `CONFIG_KCOV_PKVM_HYP` gate would be delivered). The file is generated by a kernel build; not present in a clean tree.

**KCOV-on-arm64 correction (improves the rationale).** The old plan said "arm64 KCOV is `nokcov` under TCG, so hardware is required." More precisely: `dashboard/config/linux/bits/base.yml:135-138` gates `KCOV`/`KCOV_INSTRUMENT_ALL`/`KCOV_ENABLE_COMPARISONS` off only for `-arm` (**32-bit** ARM), `-s390`, and the `-nokcov` label. **There is no `arm64` token** — native arm64 keeps KCOV. `nokcov` is a per-instance dashboard label applied only to the QEMU-*emulated* arm64 syzbot targets (`main.yml:16,18` `arm64_emu … nokcov`) for speed/stability; native arm64 targets (`main.yml:17,19,21,…`) keep it. So the physical DUT is not a workaround for broken arm64 KCOV — it is the intended path for arm64 coverage, and independently required because pKVM only boots on real hardware.

---

## 3. Architecture

Isolated-mode topology (your 阶段0 "拓扑 B"): the x86 manager drives the pre-existing arm64 DUT over SSH; the DUT runs its own booted pKVM kernel; syz-executor is a userspace VMM inside the DUT.

```
[x86 manager]  syz-manager ⇄ workdir/{corpus.db,crashes/}
     │  SCP executor → target_dir ; SSH start ; SSH -R reverse tunnel (FlatRPC)
     │  SSH remote console: `dmesg -w` ; (post-reboot) read /sys/fs/pstore
     ▼
[arm64 DUT · booted pKVM kernel, kvm-arm.mode=protected]
  sshd → syz-executor (the VMM)
     │ fork transient child per program
     ▼
  /dev/kvm ioctls + guest HVC/SMC
     │                              ┌───────────── EL2 (nVHE hyp) ─────────────┐
  EL1 host KVM  ── hvc/host↔hyp ──► │ Rust pKVM (nvhe_rust.o) + C hyp          │
  (KCOV-instrumented) ◄── exits ─── │ KCOV_INSTRUMENT:=n → no coverage today   │
     │  KCOV (EL1)                  └──────────────────────────────────────────┘
     ▼
  coverage/signal → FlatRPC → manager
```

Three fuzzing surfaces onto pKVM, in increasing difficulty:

- **Guest→hyp** (Strategy 2): a normal guest issues HVC/SMC → traps to EL2. Works today via SyzOS; Stage 1.5 teaches it the pKVM function IDs.
- **Host→hyp** (Strategy 1): the host issues `/dev/kvm` ioctls / enable-caps that drive the protected-VM lifecycle → pKVM's host↔hyp page-ownership state machine (its actual threat model: the host is the adversary). Stage 1.5 adds a pseudo-call for this.
- **EL2-internal coverage** (Stage 2): feedback from inside the Rust hyp.

---

## 4. Stage 1 — Pipeline bring-up

### 4.1 Kernel config (base = `gwd3000_lenovox1d3000m_defconfig`, verified current state)

Deltas to apply on top of the defconfig. Verified present/absent values are noted so the diff is exact.

```
# --- Coverage (mandatory; all currently UNSET) ---
CONFIG_KCOV=y
CONFIG_KCOV_INSTRUMENT_ALL=y
CONFIG_KCOV_ENABLE_COMPARISONS=y
CONFIG_DEBUG_FS=y                      # KCOV needs debugfs; not in defconfig

# --- Symbolization (already satisfied) ---
# CONFIG_DEBUG_INFO_DWARF_TOOLCHAIN_DEFAULT=y (defconfig:1214) + CONFIG_DEBUG_INFO_BTF=y (:1215)
# CONFIG_KALLSYMS_ALL=y (defconfig:48)          → keep

# --- KCOV correctness on arm64: KASLR/relocation off (currently KASLR ON) ---
# CONFIG_RANDOMIZE_BASE is not set      # defconfig:76 has =y → turn OFF
# CONFIG_RELOCATABLE is not set

# --- Host-side bug oracle (KASAN currently UNSET) ---
CONFIG_KASAN=y
CONFIG_KASAN_INLINE=y
CONFIG_DETECT_HUNG_TASK=y
CONFIG_BOOTPARAM_HUNG_TASK_PANIC=y     # hangs → recoverable panics

# --- Optional fault injection (syzkaller auto-uses) ---
CONFIG_FAULT_INJECTION=y
CONFIG_FAULT_INJECTION_DEBUG_FS=y
CONFIG_FAILSLAB=y
CONFIG_FAIL_PAGE_ALLOC=y

# --- KVM (already on) ---
# CONFIG_KVM=y (defconfig:113)          → keep; also `make kvm_guest.config`
# Verify CONFIG_X1_RMS=y in generated .config (default y ⇒ Rust hyp linked in)

# --- Crash capture across a wedged box (PSTORE_RAM currently =m) ---
CONFIG_PSTORE=y
CONFIG_PSTORE_RAM=y                    # defconfig:1117 has =m → make builtin
CONFIG_PSTORE_CONSOLE=y                # capture hyp-panic console into ramoops

# --- SBSA generic watchdog (reset lever; currently UNSET) ---
CONFIG_ARM_SBSA_WATCHDOG=y
```

- **Boot cmdline must select protected mode:** `kvm-arm.mode=protected`. Verify after boot: `dmesg | grep -i "protected\|pKVM\|hyp"`.
- **Sandbox — must be `none` for `/dev/kvm` access (not `setuid`).** `setuid` switches the test process to uid/gid `65534` (nobody) (`executor/common_linux.h:4696`), which cannot open `/dev/kvm` (owner `root:kvm`, mode `0660`) — every KVM ioctl would fail `EACCES`, silently starving the whole campaign. Use `sandbox: none` (executor runs as the SSH user = root) on a dedicated fuzz box, or grant explicit access (a udev ACL, or add the executor's uid to the `kvm` group). `namespace` (`CONFIG_USER_NS=y`, defconfig:43) is *not* the same drop — it maps in-namespace uid 0 to the real uid that launched the executor, then drops capabilities (`executor/common_linux.h:4716`), so `/dev/kvm` may open fine but a dropped capability could still bite; test it. (The syzkaller default sandbox is already `none`, `pkg/mgrconfig/load.go:100`.) **Stage-1 preflight:** confirm the chosen identity can `openat("/dev/kvm")` before starting.
- pKVM is already wired in the defconfig: `CONFIG_PKVM_SMC_FILTER=y` (:339), `CONFIG_PROTECTED_NVHE_STACKTRACE/FTRACE`, `CONFIG_NVHE_EL2_DEBUG`, `CONFIG_ARM_SMMU_V3_PKVM=y`, `CONFIG_SERIAL_PKVM_PL011=y`.

### 4.2 Crash capture + auto-recovery (the physical-board critical path)

The isolated backend captures the console **only** via a remote `ssh … "dmesg -w"` (`vm/vmimpl/console.go:91-94`, wired at `vm/isolated/isolated.go:308,354`). It has **no serial-device support** (the tty `OpenConsole()` is never referenced from isolated). So:

- **Primary oracle:** `ssh dmesg -w` — live host oops / KASAN / hyp-panic lines the host prints.
- **Recovery + missed-log oracle:** enable ramoops/pstore and set `"pstore": true`. After a crash+reset, `Diagnose()` (`isolated.go:383`, gated on `pstore` at `:384`) reads and clears `/sys/fs/pstore/console-ramoops-0` (`:26,:366-369`) — recovering logs lost when SSH died. Confirm nVHE **hyp panics land in ramoops** (the host prints them before going down, so `PSTORE_CONSOLE` captures them) — validate this early.
- **Reset lever:** run a watchdog daemon (systemd `RuntimeWatchdog` / `watchdogd`) petting the SBSA GWDT. When EL2 or the host wedges hard, petting stops → GWDT WS0 (interrupt) then WS1 (**hardware reset**) → the box reboots → `waitForReboot` (`isolated.go:262`, detects reboot when `ssh pwd` starts failing) sees it. Note `repair()` (`:216-241`) only reboots when SSH is *already* up and `target_reboot=true`; a fully-wedged box relies on the GWDT, not on `repair()`.
- ramoops needs a reserved-memory region: add a DT `reserved-memory` `ramoops` node, or `ramoops.mem_address=/mem_size=` on cmdline.
- Serial (minicom/picocom) stays a **human** debug channel, out-of-band from syzkaller.

### 4.3 Board access

- `/etc/ssh/sshd_config`: `AllowTcpForwarding yes` (reverse `-R` tunnels; `docs/linux/setup_linux-host_isolated.md:15-19`).
- Install the manager's SSH public key for passwordless login as root (executor needs `/dev/kvm`).
- Pick a `target_dir` (e.g. `/root/syzkaller`).

### 4.4 Build syzkaller (on the x86 manager)

```
cd /home/jose/syzkaller
make TARGETOS=linux TARGETARCH=arm64 TARGETVMARCH=arm64 \
     CC=aarch64-linux-gnu-gcc CXX=aarch64-linux-gnu-g++
```

Produces `bin/syz-manager` (x86) and `bin/linux_arm64/{syz-executor,syz-execprog,...}`. Symbolization uses the installed `aarch64-linux-gnu-{addr2line,nm,objdump,readelf}` (`llvm-addr2line` tried first, `sys/targets/targets.go:820-825`).

### 4.5 Manager config (`isolated`, arm64) — e.g. `workdir/pkvm-arm64.cfg`

```json
{
  "target": "linux/arm64",
  "http": "127.0.0.1:56741",
  "workdir": "/home/jose/syzkaller/workdir",
  "kernel_obj": "/path/to/pkvm-kernel-build",
  "kernel_src": "/home/jose/common",
  "syzkaller": "/home/jose/syzkaller",
  "sshkey": "/home/jose/.ssh/pkvm_board",
  "ssh_user": "root",
  "sandbox": "none",
  "cover": true,
  "type": "isolated",
  "vm": {
    "targets": ["N90"],
    "target_dir": "/root/syzkaller",
    "target_reboot": true,
    "pstore": true,
    "system_ssh_cfg": true
  }
}
```

Run `./bin/syz-manager -config=workdir/pkvm-arm64.cfg` (add `-debug` first time). `vm` knobs the isolated backend accepts: `host, targets, target_dir, target_reboot, usb_device_num, startup_script, pstore, system_ssh_cfg` (`isolated.go:34-43`). **`procs` defaults to 6** (`load.go:103`); the **Stage-2 EL2-coverage MVP requires `"procs": 1`** here (attribution, §6.3), while Stage 1 / 1.5 host-side fuzzing can keep the default for throughput.

### 4.6 Aim the fuzzer at pKVM

Host-KVM subsystem fuzzing is x86-only in upstream syzbot configs (`dashboard/config/linux/bits/subsystems.yml:75` → `- KVM: [-arm, -riscv]`); that file only affects syzbot's Kconfig generation, so for our hand-built kernel + hand-written manager config it is informational — it confirms arm pKVM is new ground. We aim the campaign directly:

- Set `"enable_syscalls"` to the KVM boundary: the `/dev/kvm` ioctls plus `syz_kvm_setup_syzos_vm$arm64`, `syz_kvm_add_vcpu$arm64`, `syz_kvm_vgic_v3_setup`, the `openat$kvm`/`KVM_CREATE_VM`/`KVM_RUN` family, and (after Stage 1.5) `syz_kvm_setup_protected_vm$arm64` + the pKVM HVC command.
- The existing `@smc`/`@hvc` SyzOS command already fires guest hypercalls with x0 from `kvm_smc_id`; Stage 1.5 makes those IDs pKVM-aware.

### 4.7 Validation — see §9.

---

## 5. Stage 1.5 — Model the pKVM ABI (syzkaller code changes)

Two ABI surfaces, three code changes. This maps directly onto the two syzlang mechanisms from your 阶段3 note: a `flags[...]` set (the guest→hyp function IDs) and a **pseudo-call** / `伪调用` (the host→hyp lifecycle). All follow the documented "add a SyzOS command" procedure (`docs/syzos.md:148-209` §6) and finish with a regression test (§7 `:213+`).

### 5.1 Guest→hyp function IDs — extend `kvm_smc_id` (Option A, cheap, do first)

Today `syzos_api_smccc.arg_id` (x0) is drawn from `kvm_smc_id` (`sys/linux/dev_kvm_arm64.txt:134`), which knows `ARM_SMCCC_*`/`PSCI_*` and the KVM vendor `FEATURES`/`PTP` IDs — but **not** the pKVM guest hypercalls. A repo-wide grep confirms `MEM_SHARE`/`MEM_UNSHARE`/`HYP_MEMINFO`/`MMIO_GUARD_*` are absent from all of `sys/` and `executor/`.

Change: add the pKVM `ARM_SMCCC_VENDOR_HYP_KVM_*` full function-ID symbols (and any custom IDs this Rust hyp defines) to the `kvm_smc_id` set at `dev_kvm_arm64.txt:134`, with the owning header so `syz-extract` resolves them. The canonical upstream pKVM guest set to add (confirm exact symbol names against **this kernel's** `include/linux/arm-smccc.h` and the Rust HVC handler): `…HYP_MEMINFO`, `…MEM_SHARE`, `…MEM_UNSHARE`, `…MMIO_GUARD_INFO`, `…MMIO_GUARD_ENROLL` (sets the VM's MMIO-guard flag — map/unmap are inert without it in the C path, `pkvm.c:1729`), `…MMIO_GUARD_MAP`, `…MMIO_GUARD_UNMAP`, `…MEM_RELINQUISH`.

Because the existing `@smc`/`@hvc` command already ships fuzzer-controlled x0–x5 through `struct api_call_smccc` (`common_kvm_arm64_syzos.h:45-49`, payload `dev_kvm_arm64.txt:136-139`), this single set-extension immediately lets a guest **issue** every pKVM function ID — no new handler needed. Regenerate: `make extract SOURCEDIR=/home/jose/common` (→ `dev_kvm_arm64.txt.const`), then `make generate`.

**Reachability caveat — implementation-dependent; what a func ID reaches depends on which hyp runs (see the §0 fork).** Under the default **Rust** hyp (`X1_RMS=y`): a normal-guest HVC hits only the generic exit handler (`hyp_main.rs:2101-2108`), and a protected-guest HVC reaches only the `MEM_SHARE`/`MEM_UNSHARE`/`MEM_RELINQUISH` **stubs** (`:2171-2183`) — issuing the IDs still fuzzes SyzOS dispatch, arg marshalling and the (thin) handlers, but the deep pKVM memory interface is *not implemented here yet*, so don't expect depth from Strategy 2/3 until the Rust HVC path is filled in. Under the **C** hyp (`X1_RMS=n`): the non-protected path services `HYP_MEMINFO`/`MEM_RELINQUISH` deeply (`pkvm.c:1767-1782` `kvm_hyp_handle_hvc64`) and the full `MEM_SHARE`/`MMIO_GUARD_*` handlers live in the protected path (`kvm_handle_pvm_hvc64`, `:1697-1759`), reachable only from a protected guest (Strategy 3, §5.4). Either way §5.1 buys breadth over the ID space + validation/rejection; the *depth ceiling is set by whichever hyp the target actually runs.*

### 5.2 Structured `hvc_pkvm` command (Option B, for IPA-bearing calls)

`MEM_SHARE`/`MEM_UNSHARE`/`MMIO_GUARD_MAP` need a **valid IPA** into guest memory to reach deep code; a random x1 bounces. Add a structured command whose args reference the SyzOS-mapped guest window, following `docs/syzos.md` §6:

- `executor/common_kvm_arm64_syzos.h`: add `SYZOS_API_HVC_PKVM = 310` to the `syzos_api_id` enum (`:19-33`). **Verified delta:** the current top real command is `SYZOS_API_SVC = 290` (the old plan's "300" does not exist; `SYZOS_API_STOP = 291`). The prime×10 rule (`:15-18`) still makes **310** the next convention-correct slot (÷10 = 31, prime); `STOP` shifts to 311. Add a `GUEST_CODE` prototype, a `case` in `guest_main` (`:109-163`), and a handler that loads x0–x5 with any IPA arg clamped to the mapped guest window, then `hvc #0`.
- `sys/linux/dev_kvm_arm64.txt`: add a payload struct (e.g. `syzos_api_hvc_pkvm { func flags[pkvm_hvc_id, int32]; ipa …; size …; arg int64 }`) and a `syzos_api[310, syzos_api_hvc_pkvm]` entry in the `syzos_api_call` union (`:216-229`, helper `:210-214`; current top NUM = 290). **The NUM must equal the C enum value.**
- Reuse `executor/common_kvm_syzos.h` `api_call_N` structs if a generic shape fits.

### 5.3 Protected-VM lifecycle — pseudo-call `syz_kvm_setup_protected_vm$arm64` (Strategy 1, primary pKVM target)

**syz-executor *is* the VMM** — normally crosvm creates the protected VM; here the executor replays the same ioctl sequence with fuzzer-controlled arguments, so crosvm is not needed at runtime. The sequence was cross-checked against this kernel's uAPI and every load-bearing constant **matches byte-for-byte** (this ACK fork carries the same downstream Android/pKVM ABI crosvm targets; these constants are *not* upstream):

| Constant | Value | This-kernel definition |
|---|---|---|
| `KVM_VM_TYPE_ARM_PROTECTED` | `0x80000000` (bit 31) | `include/uapi/linux/kvm.h:923` |
| `KVM_VM_TYPE_ARM_IPA_SIZE_MASK` | `0xff` | `include/uapi/linux/kvm.h:919` |
| `KVM_CAP_ARM_PROTECTED_VM` | `0xffbadab1` | `include/uapi/linux/kvm.h:1202` |
| `…_FLAGS_SET_FW_IPA` | `0` | `arch/arm64/include/uapi/asm/kvm.h:478` |
| `…_FLAGS_INFO` | `1` | `arch/arm64/include/uapi/asm/kvm.h:479` |
| `…_FLAGS_SET_FFA` | `2` (crosvm) | **not defined here — omit** |

The verified sequence the pseudo-call replays (`hypervisor/src/kvm/aarch64.rs`, handler in `arch/arm64/kvm/pkvm.c`):

```
1. vmfd = KVM_CREATE_VM(type = (ipa_size & 0xff) | 0x80000000)     # protected bit 31
     - ipa_size from KVM_CHECK_EXTENSION(KVM_CAP_ARM_VM_IPA_SIZE=165), 0 ⇒ default 40-bit
     - kernel: pkvm.enabled=true iff type & PROTECTED (pkvm.c:457,464)
2. KVM_ENABLE_CAP{ cap=0xffbadab1, flags=1 /*INFO*/,      args=[&info64, 0,0,0] }   # query pvmfw size
     - info = struct kvm_protected_vm_info { u64 firmware_size; u64 __reserved[7] } (asm/kvm.h:481)
3. KVM_ENABLE_CAP{ cap=0xffbadab1, flags=0 /*SET_FW_IPA*/, args=[fw_ipa, 0,0,0] }   # only RECORDS the IPA (pkvm.c:619-635)
4. KVM_SET_USER_MEMORY_REGION covering [fw_ipa, fw_ipa+firmware_size)   # THIS is what makes EL2 load pvmfw; no donation ioctl, no guest_memfd
5. KVM_CREATE_VCPU + KVM_ARM_PREFERRED_TARGET + KVM_ARM_VCPU_INIT   # generic; no pkvm feature bit
6. KVM_RUN
```

**Kernel-enforced constraints the model must honor** (they are themselves fuzz targets):
- The protected bit **must** be set at `KVM_CREATE_VM`; every enable-cap is rejected `-EINVAL` unless `kvm_vm_is_protected(kvm)` is already true (`pkvm.c:652`).
- `args[1..3]` must be zero (`pkvm.c:655`).
- INFO before SET_FW_IPA (crosvm validates `firmware_size`; kernel does not strictly require the order).
- Do **not** model `SET_FFA` (flags=2) — `-EINVAL` here.

**Why the existing model can't already do this — and why the pseudo-call must own VM creation.** `ioctl$KVM_CREATE_VM`'s `type intptr[0:64]` (`dev_kvm.txt:22`, per your 阶段3 note) restricts the value to 0..64, so it cannot set bit 31 and only ever yields a *normal* `fd_kvmvm`. Critically, **a normal VM cannot be upgraded to protected**: `pkvm_init_host_vm` sets `pkvm.enabled` solely from the `type` bit at `KVM_CREATE_VM` (`arch/arm64/kvm/pkvm.c:455-465`), and every later `KVM_ENABLE_CAP(KVM_CAP_ARM_PROTECTED_VM)` is rejected `-EINVAL` unless the VM is *already* protected (`:652`). So the pseudo-call must **own VM creation** — it cannot accept an existing `fd_kvmvm`. Model it on the `/dev/kvm` fd, returning a distinct protected-VM resource subtype so protected-only ioctls are never handed normal VMs (which would just `-EINVAL` and waste the budget):

```
resource fd_kvmvm_protected[fd_kvmvm]      # a VM created with KVM_VM_TYPE_ARM_PROTECTED

syz_kvm_setup_protected_vm$arm64(fd fd_kvm, usermem vma[...], fw_ipa intptr,
                                 ipa_size flags[kvm_arm_ipa_size]) fd_kvmvm_protected
```

The **stable** helper (`executor/common_kvm_arm64.h`) runs steps 1–4 internally — `CREATE_VM(bit31|ipa)` → `INFO` (read `firmware_size`) → `SET_FW_IPA` → **register a memory region covering `[fw_ipa, fw_ipa+firmware_size)`** (checking alignment/range) — because `SET_FW_IPA` alone only records the IPA; EL2 loads pvmfw only when that covering region is registered (`pkvm.c:619-635`; `arch/arm64/kvm/hyp/include/nvhe/pkvm.h:150`). It returns the real protected VM fd, so downstream `KVM_RUN`/vCPU calls consuming `fd_kvmvm_protected` reliably act on a fully-set-up protected VM — *depth*.

For *breadth* against the state machine itself, add **raw** variants that deliberately explore rejection paths (wrong order, wrong IPA, a region that doesn't cover the firmware, nonzero `args[1..3]`). Two type-safety rules keep the resource honest:
- `ioctl$KVM_ENABLE_CAP$ARM_PROTECTED_VM(fd fd_kvmvm_protected, ...)` draws the protected subtype (these caps `-EINVAL` on a normal VM).
- A raw creator may return `fd_kvmvm_protected` **only if it forces bit 31** — e.g. `ioctl$KVM_CREATE_VM_PROTECTED(fd fd_kvm, cmd, type kvm_arm_protected_type) fd_kvmvm_protected` where `type` is `const[0x80000000]`-OR'd (only the low IPA bits vary). A creator whose `type` is *not* guaranteed to carry bit 31 must return a plain `fd_kvmvm` (that is just the existing `ioctl$KVM_CREATE_VM`) — otherwise the resource type lies and downstream protected-only ioctls silently `-EINVAL`.

**pvmfw note:** the executor need not carry a real measured pvmfw blob — Strategy 1 fuzzes the *host-side sequence*, and many attempts legitimately fail (the point is to exercise creation/config/teardown, page donate/reclaim, and rejection paths). Reaching *post-firmware* guest code would need a valid pvmfw; treat that as an optional parameter, not a prerequisite.

### 5.4 Strategy 2 (parallel) and Strategy 3 (deferred)

- **Strategy 2 — normal-guest SyzOS HVC/SMC** (Options A/B above): a normal guest under pKVM still traps HVCs to EL2, so PSCI / vendor-hyp / normal-guest-reachable pKVM calls are fuzzed with the full SyzOS command channel intact. It can run from day one — but for coverage of the ABI, argument marshalling and the *thin* handlers, **not** deep pKVM fuzzing under the default `X1_RMS=y` (§0). (The existing SMC/HVC test uses `KVM_ARM_VM_SMCCC_FILTER` to forward a func-ID range to userspace — see the template in §7.)
- **Strategy 3 — SyzOS inside a *protected* guest** (HARD, defer): a protected VM isolates guest memory from the host, boots from measured pvmfw, and scrubs exits, so SyzOS's host↔guest command channel can't run as-is. Adapting it (bake a fuzzer-chosen program before sealing, share one page via a `MEM_SHARE` HVC for results) is real protected-guest hypercall fuzzing but needs SyzOS surgery. Later.

### 5.5 Constants extraction

The pKVM func IDs and protected-VM constants resolve to numbers only against **this kernel's** headers (your 阶段3 `.txt`/`.const` split). Run `make extract SOURCEDIR=/home/jose/common` so `dev_kvm_arm64.txt.const` picks up the new IDs (the `.const` currently has only KVM `FEATURES` 2248146944 / `PTP` 2248146945 among vendor-hyp IDs, plus `KVM_CAP_ARM_*`/`KVM_ENABLE_CAP`), then `make generate`.

### 5.6 Tests

Model new tests on `sys/linux/test/arm64-syz_kvm_setup_syzos_vm-smc` (which already covers both SMC and HVC; there is no separate `-hvc` file). That template shows the `@smc`/`@hvc` payloads, the `KVM_ARM_VM_SMCCC_FILTER` forward-to-userspace setup, `KVM_RUN` ×3, `mmap$KVM_VCPU`, and `syz_kvm_assert_syzos_uexit$arm64(…, 0xffffffffffffffff /*UEXIT_END*/)`. Add:
- `sys/linux/test/arm64-syz_kvm_setup_syzos_vm-pkvm` (guest pKVM HVCs).
- `sys/linux/test/arm64-syz_kvm_setup_protected_vm` (the lifecycle sequence).

### 5.7 Validation — see §9.

---

## 6. Stage 2 — EL2 coverage from inside the Rust hyp (research design, prototype-first; separate follow-on)

Goal: edge coverage of the Rust pKVM so mutation is guided by EL2 internals. This stage is a **research follow-on, not first-pass implementation.** Beyond the SanCov build (§6.1) and the EL2 ring (§6.2), three loops must be closed — each an open problem the first-pass sketch under-specified: (i) getting EL2 PCs into *this program's* coverage without tripping KCOV's own guards (§6.3); (ii) attributing them to the correct execution under concurrency (§6.3); and (iii) symbolizing hyp-VA PCs against the hyp object (§6.4 — a hard blocker). The `extra_cov`/`KCOV_REMOTE` path is **not** a drop-in hook here — see §6.3.

### 6.1 Instrument the Rust crate (SanitizerCoverage) via `build_rust.sh`

Add SanCov flags to `RUSTFLAGS` in `build_rust.sh:52`, gated on a new `CONFIG_KCOV_PKVM_HYP` delivered through the existing `@include/generated/rustc_cfg` mechanism (`:61-63`). **Match the kernel's KCOV flavor: `trace-pc`, not `trace-pc-guard`** — the kernel advertises `CC_HAS_SANCOV_TRACE_PC` and implements the nullary `__sanitizer_cov_trace_pc()`. **Exclude the coverage runtime itself** (the handler + ring code) from instrumentation to avoid infinite recursion. Keep everything on the pinned rustc 1.88 / LLVM 20.1.4 (fix `build_rust.sh:29` first); compile any C-side coverage code and do symbolization with LLVM 20 to match.

### 6.2 Hyp-local handler + per-CPU shared ring (EL2 side)

Reserve a **per-CPU ring in a page shared between EL2 and EL1** (donated by the host at pKVM init through the existing host↔hyp shared-memory mechanism). Place ring/metadata in a dedicated hyp section mirroring `.hyp.xcore_test_cases` — the `CONFIG_XCORE_UNIT_TEST` framework already adds a custom hyp section and wires it through the nVHE linker script (`arch/arm64/kvm/hyp/nvhe/hyp.lds.S:42-46`, `BEGIN_HYP_SECTION(.xcore_test_cases)` with `__xcore_test_cases_{start,end}`; Rust side in `rust/src/unit_test.rs`; gating in `build_rust.sh:56-59` and `Kconfig:116-118`). The hyp-local `__sanitizer_cov_trace_pc()` reads its return address and appends the PC to the current CPU's ring (drop-on-full). Reentrancy-safe by construction (per-CPU, EL2 interrupts controlled). The `__kvm_nvhe_` prefix (`Makefile.nvhe:77`) keeps this symbol from clashing with the EL1 kcov runtime.

### 6.3 EL1 drain → coverage delivery, attribution-correct

The first-pass idea — wrap EL2 entry with `kcov_remote_start(pkvm_handle)` / `kcov_remote_stop()` on the executor thread — **does not work**, and this is a hard constraint. syzkaller's *main* per-thread cover is enabled with **local `KCOV_ENABLE`** (`executor/executor_linux.h:189`; only the separate `extra_cov` uses `KCOV_REMOTE_ENABLE`), so `kcov_start()` sets the executor thread's `t->kcov_mode` to an enabled mode (`kernel/kcov.c:346-359`). `kcov_remote_start()` then hits `WARN_ON(in_task() && kcov_mode_enabled(mode))` (`kernel/kcov.c:859-863`) and returns immediately — the EL2 PCs are silently dropped. `KCOV_REMOTE` is built for *background* contexts (softirqs, kernel threads) with **no** existing KCOV, which is not the executor thread running `KVM_RUN`.

**Chosen mechanism — batch-append into the current task's coverage area, in-kernel.** The drain runs **in the kernel, on the executor thread's KVM-ioctl return path** — where `t->kcov_area` is live and correctly attributed to the running program. There is no separate "EL1 pKVM driver running after `KVM_RUN`"; the executor just issues the ioctl, so the drain must live in the ioctl handler itself. Add a small kcov-core API — `kcov_add_pcs(const unsigned long *pcs, n)` — that, under the same `local_lock` + `t->kcov_mode`/`t->kcov_sequence` rules `__sanitizer_cov_trace_pc` obeys, appends the (translated, §6.4) EL2 PCs into the current task's area. The EL2 PCs then land in the **same per-program coverage buffer** as the surrounding EL1 syscall — for the *boundary* drains this needs no new executor-side plumbing, no `KCOV_SUBSYSTEM_PKVM` handle, no `extra_cov` merge (teardown is the exception — see the contract, point 4). (Alternative, if a separate stream is ever wanted: run the KVM op in a dedicated worker context that legitimately holds a *remote* kcov area — more machinery, worse attribution; not recommended.)

**Coverage-boundary & attribution contract (not "drain at every ioctl return").** "All host→hyp boundaries" ≠ "every ioctl return," and naming boundaries by ioctl is unsafe — the real EL2 entries don't line up with ioctl names, and teardown isn't an ioctl at all. The contract:

1. **Enumerate boundaries by *verified call site*, synchronous on the executor thread — not by ioctl name.** Audit each; drain at its return. Confirmed EL2 entries: `KVM_CREATE_VM` and `KVM_CREATE_VCPU` each share their struct pages with the hyp — `kvm_share_hyp()` (`arm.c:175`, `:497`) → `kvm_call_hyp_nvhe(__pkvm_host_share_hyp)` (`mmu.c:591`); the hyp **VM object** is created later, at `KVM_ARM_VCPU_INIT` (`pkvm_create_hyp_vm()`, `arm.c:759`), *not* at `KVM_CREATE_VM`; and `KVM_RUN` via `kvm_arch_vcpu_ioctl_run` — which also covers the **guest memory-fault** path, where the real host→page handoff happens (`pkvm_mem_abort` → `pkvm_host_map_guest` → `kvm_call_hyp_nvhe(__pkvm_host_map_guest)`, `mmu.c:1669→1581→1584`). **Not** boundaries: `KVM_ENABLE_CAP` INFO/SET_FW_IPA (EL1-only — INFO `copy_to_user`s the size, SET_FW_IPA records `pvmfw_load_addr`, `pkvm.c:619-647`); and `KVM_SET_USER_MEMORY_REGION`, which installs/validates the EL1 memslot only (`kvm_arch_prepare_memory_region`, `mmu.c:2484`) — pages reach the pVM later, fault-driven **within `KVM_RUN`**, not at registration. Both are already covered by ordinary KCOV / the `KVM_RUN` drain. The ioctl→EL2 mapping is non-obvious — confirm every entry against code.
2. **Bracket each boundary:** clear/mark the ring *before* EL2 entry; drain on **both** the success *and* the failure return paths.
3. **Append only in local-KCOV mode:** `kcov_add_pcs` must no-op unless the current task has `t->kcov_mode` enabled, so hyp calls made outside a fuzz request never corrupt anyone's area.
4. **Teardown needs explicit handling — it is *not* an ioctl.** `pkvm_destroy_hyp_vm()` runs from VM destruction (`kvm_arch_destroy_vm`, `arch/arm64/kvm/arm.c:223`) when the last fd reference drops. The executor closes leftover fds via `close_fds()` **after** every per-call output is already written (`executor/executor.cc:1158-1160`; only `write_extra_output()` follows at `:1162`), so teardown coverage landing in the main area has *no call to attribute to* and is dropped. So **"no executor change needed" is false for teardown** — pick one: (a) model an explicit `close$kvmvm_protected` as a syz call, attributing teardown to it; or (b) add an executor end-of-program coverage flush (mirroring `write_extra_output`, which *does* run post-`close_fds`). Do not treat teardown as ordinary ioctl coverage. **Ordering matters:** each vCPU fd holds a VM reference (`kvm_get_kvm` at vCPU creation, `virt/kvm/kvm_main.c:4013`; dropped in `kvm_vcpu_release`, `:3886`), so closing the VM fd is usually *not* the last reference. The prototype teardown must `munmap$KVM_VCPU` (if mapped) → close all `fd_kvmcpu` → close `fd_kvmvm_protected` **last**, with a test confirming that final close actually reaches `pkvm_destroy_hyp_vm()` and its coverage attributes there — otherwise destroy falls back to `close_fds()` and orphans again.
5. **Do not globally wrap `kvm_call_hyp_nvhe()`.** It is a low-level primitive invoked from many non-fuzz contexts (init, other CPUs); wrapping it would over-collect, inflate overhead, and break attribution. Hook the specific verified boundaries instead.

**How the inventory is established vs. validated.** The *potential* boundary inventory is produced by a **config-qualified static audit** of the relevant KVM ABI paths and their `kvm_call_hyp_nvhe()` call sites (for a fixed kernel version + config) — that is what *enumerates* boundaries. Instrumented hardware runs then **validate** observed reachability and the attribution machinery (clear/drain, CPU attribution, injection); they are a validation step, **not** a proof that an unobserved path is unreachable. So the static boundary table and a minimal hardware prototype are complementary — neither alone closes the list.

**Attribution & isolation — required for correctness, not optional.** A per-CPU ring stores only PCs; it says nothing about which proc / vCPU / program produced them. Two hazards, not one:
1. **Multiple procs.** syzkaller defaults to **`procs: 6`** (`pkg/mgrconfig/load.go:103`), so the manager config **must set `procs: 1`** for the MVP (§4.5).
2. **CPU migration.** Even at `procs=1`, the executor thread can migrate CPUs between or within boundary ioctls, so "drain this CPU's ring" can pick up a stale neighbour's records.

So the MVP must pick one — `{cpu,generation}` **cannot** be wholly deferred to a "concurrent version," because migration corrupts attribution even at `procs=1`:
- **pin** the executor thread to a fixed CPU (`sched_setaffinity`) and read only that CPU's ring; **or**
- carry a `{cpu, generation}` id **from v1** — the driver stamps the current execution id before entering the hyp, EL2 tags each record, and the drain scans *all* CPU rings keeping only matching ones.

Reset the relevant ring(s) at each boundary. Concurrency (multiple procs) then just reuses the same tagging — **don't advertise it until the tags exist.**

**Reuse existing host↔hyp ring machinery.** Rather than invent a bespoke "donated shared page," evaluate reusing the kernel's existing host↔hyp trace-buffer page-ownership + mapping code (`arch/arm64/kvm/hyp_trace.c` — `hyp_trace_buffer_load` / `rb_page_desc` / `hyp_trace_desc`, ~:295), which already solves EL2↔EL1 ring pages and is a safer foundation than rolling a new one.

### 6.4 Symbolization across two objects (the hard Stage-2 blocker)

syz-manager symbolizes every PC against a single ELF, but pKVM produces PCs from **two objects in two address spaces**: EL1 host PCs resolve against `vmlinux` (standard); EL2 PCs are hyp VAs whose code carries `__kvm_nvhe_`-prefixed, Rust-mangled symbols. This is a **blocker** for Stage 2, not a residual risk.

**`module_obj` does *not* solve it.** The first-pass fallback — feed `kvm_nvhe.o` as a module object — cannot work: syzkaller's module discovery ingests only `.ko` files (`pkg/cover/backend/modules.go:76` filters on `filepath.Ext(path) != ".ko"`) and treats each as a loadable module with a runtime load address. `kvm_nvhe.o` is *linked into `vmlinux`* (`Makefile.nvhe:66` → `obj-y := kvm_nvhe.o`), has no `.ko` form and no module load address, so `locateModules` skips it. Rule it out.

**The two real options** (decide in the prototype, before building the loop):
1. **Resolve within `vmlinux`.** The hyp code and symbols *are* in `vmlinux` (with the `__kvm_nvhe_` prefix). If the Rust hyp's DWARF survives the `.llvmbc` strip + `--prefix-symbols` steps into the final image, `kernel_obj` symbolization already holds the line tables; the remaining work is (a) translating runtime hyp-VA PCs to link addresses (subtract the hyp base, `hyp_physvirt_offset` / `kern_hyp_va`, in the EL1 drain before `kcov_add_pcs`), and (b) teaching `pkg/cover` to demangle and route the `__kvm_nvhe_`-prefixed hyp-range PCs.
2. **Add first-class hyp-image object support** to `pkg/cover` — a dedicated object with its own runtime address range and PC→symbol map (i.e. build the thing `module_obj` is *not*) — if the DWARF does not survive into `vmlinux`.

**Either option also needs the SanCov callback recognized.** The hyp objcopy prefixes *every* symbol with `__kvm_nvhe_` (`Makefile.nvhe:77`), so the Rust crate's trace-pc callback is emitted as `__kvm_nvhe___sanitizer_cov_trace_pc`. syzkaller's `getTraceCallbackType` (`pkg/cover/backend/elf.go:50-59`) matches only the *unprefixed* `__sanitizer_cov_trace_pc` (+ its `_veneer`), returning `TraceCbNone` for the prefixed name. So `pkg/cover` must additionally recognize `__kvm_nvhe___sanitizer_cov_trace_pc` (and prefixed `__sanitizer_cov_trace_*` cmp callbacks + veneers), accept that prefixed target in the arm64 callback scan, and add those call sites to `CallbackPoints`. Without this the fuzzer can still *use* the new PCs as feedback, but the coverage **report** flags every Rust EL2 PC as "no matching coverage callback."

**De-risk first (Appendix A).** The DWARF half is already proven: with `debug = true` (`CARGO_PROFILE_RELEASE_DEBUG=2`), `objdump -dl` on `nvhe_rust::hyp_main::handle_vm_exit_abt` maps to `src/hyp_main.rs:2603…`; symbols are legacy-mangled (`_ZN…`), demangled by `addr2line -C`. The open half is exactly (1) hyp-VA→link-addr translation and (2) confirming the hyp DWARF reaches the final `vmlinux` (option 1) or wiring a second object (option 2). Prove this end-to-end before any ring/driver work.

### 6.5 Oracle for pKVM security bugs

pKVM's job is to **reject** a malicious host, so an isolation violation often produces **no crash**. Because EL2 has no KASAN (`Makefile.nvhe:92`), leverage Rust: build the crate with overflow/bounds checks and `panic=abort` so a violated invariant becomes a **hyp panic** the host prints → ramoops → syzkaller. Keep/add `debug_assert!` and invariant checks in the EL2 Rust deliberately — that *is* the security oracle.

---

## 7. Scope, sequencing, risks

**First implementation pass = Stage 1 + Stage 1.5.** Stage 2 is a designed follow-on.

**Recommended code order** (smallest reversible step first; all pure-syzkaller and testable before the board is fully wired):
1. Stage 1.5 §5.1 — extend `kvm_smc_id` + `make extract`/`generate`. Immediate reach of all pKVM func IDs.
2. Stage 1.5 §5.3 — `syz_kvm_setup_protected_vm$arm64` pseudo-call + raw protected variants + tests.
3. Stage 1.5 §5.2 — structured `hvc_pkvm` (310) command for IPA-bearing calls.
4. Stage 1 — board/config/manager bring-up (can proceed in parallel; needs the physical board).
5. Stage 2 — separate follow-on.

**Risks / open items** (▲ = must resolve before the relevant stage runs):
- ▲ **Baseline — confirm which hyp runs.** `CONFIG_X1_RMS=y` (default) runs the **Rust** EL2, whose guest-HVC surface is thin/stubbed (`hyp_main.rs:2085-2108,2171-2183`); the C-path reachability in §5.1/§5.4 applies only at `X1_RMS=n`. Set Strategy 2/3 expectations to the actual image before promising guest-HVC depth (§0).
- ▲ **Stage 1 — `/dev/kvm` access:** `sandbox: none` (or an explicit device ACL), never `setuid` (drops to uid 65534); `namespace` maps ns-uid-0→real-uid + drops caps, so test it. Preflight `openat("/dev/kvm")` (§4.1).
- Serial is **not** an isolated-mode oracle → recovery relies entirely on ramoops + GWDT watchdog; validate a real hyp panic reaches ramoops early.
- KCOV is disabled only on *emulated* arm64 (`nokcov`); native hardware keeps it, so throughput scales with board count — optimize depth first.
- ▲ **Stage 1.5 — protected VMs must be *created* protected:** the stable helper owns `KVM_CREATE_VM(bit31)` **and** registers a firmware-covering memory region (so pvmfw actually loads), returning `fd_kvmvm_protected`; a normal VM can't be upgraded, and a raw creator may return the protected subtype only if it forces bit 31 (§5.3).
- Protected VMs isolate memory from the host → the tractable target is the host-side lifecycle (Strategy 1); normal-guest HVC/SMC (Strategy 2) in parallel; protected-guest depth (Strategy 3) deferred and, under Rust, largely unimplemented.
- ▲ **Stage 2 — research follow-on, unclosed loops:** (i) delivery = in-kernel `kcov_add_pcs` at each *verified* host→hyp boundary (non-obvious: `CREATE_VM`/`_VCPU` share structs to EL2, the hyp-VM object is at `VCPU_INIT`, `ENABLE_CAP` is EL1-only), on success+failure paths, **plus a teardown story** (vCPUs-then-VM close ordering + modeled `close` or an executor end-of-program flush, since `close_fds` runs after per-call output) — not `kcov_remote_start`, not only `KVM_RUN`, not a global `kvm_call_hyp_nvhe` wrap (§6.3); (ii) attribution needs `procs:1` **plus** CPU-pin or `{cpu,generation}` tags from v1 (§6.3); (iii) **symbolization is a hard blocker** — `module_obj` doesn't apply; needs hyp-VA→link translation, hyp-range routing, **and** teaching `pkg/cover` the `__kvm_nvhe_`-prefixed SanCov callback (§6.4). Resolve in the de-risk prototype first.
- Confirm the exact pKVM guest hypercall ID symbols against this kernel's `include/linux/arm-smccc.h` and the *live* handlers (Rust `hyp_main.rs` at `X1_RMS=y`, else C `pkvm.c:1697,1767`); the Rust hyp may define custom IDs.

---

## 8. Files to create / modify

**Operational (manager host / board):** `workdir/pkvm-arm64.cfg` (new); board sshd config, SSH key, ramoops DT node, watchdog daemon unit. (Cross toolchain already installed.)

**Stage 1.5 (syzkaller repo):**
- `sys/linux/dev_kvm_arm64.txt` — extend `kvm_smc_id` (:134); add `syzos_api_hvc_pkvm` struct + `syzos_api[310,…]` union entry (:216-229); add `pkvm_hvc_id`/`kvm_arm_vm_type`/`kvm_arm_ipa_size` flag sets, the `resource fd_kvmvm_protected[fd_kvmvm]`, and the `ioctl$KVM_CREATE_VM_PROTECTED` + `ioctl$KVM_ENABLE_CAP$ARM_PROTECTED_VM` variants (both drawing `fd_kvmvm_protected`).
- `sys/linux/dev_kvm_arm64.txt.const` — regenerated via `make extract SOURCEDIR=/home/jose/common`.
- `executor/common_kvm_arm64_syzos.h` — `SYZOS_API_HVC_PKVM=310` enum + prototype + `guest_main` case (:109-163) + handler.
- `executor/common_kvm_arm64.h` — `syz_kvm_setup_protected_vm$arm64` helper: takes `fd_kvm`, runs create(bit31)→INFO→SET_FW_IPA→**register a fw-covering memory region** (§5.3), returns `fd_kvmvm_protected`.
- `sys/linux/test/arm64-syz_kvm_setup_syzos_vm-pkvm`, `sys/linux/test/arm64-syz_kvm_setup_protected_vm` — new tests.

**Stage 2 (syzkaller repo):** mostly `pkg/cover` — recognize the `__kvm_nvhe___sanitizer_cov_trace_pc` callback (`elf.go:50-59`), plus hyp-VA→link-addr routing + `__kvm_nvhe_` demangling so EL2 PCs (which arrive in the normal coverage buffer via `kcov_add_pcs`, §6.3) symbolize against the hyp code in `vmlinux` (§6.4). `module_obj` does **not** apply. The batch-append design needs no `KCOV_SUBSYSTEM_PKVM`/`extra_cov` handle — **except for teardown**, which needs either a modeled `close$kvmvm_protected` call or an executor end-of-program coverage flush (§6.3).

**Stage 2 (pKVM Rust + kernel tree, separate work):** `build_rust.sh` — pin toolchain (:29) + add trace-pc SanCov to `RUSTFLAGS` (:52) gated via `rustc_cfg`; a hyp-local `__sanitizer_cov_trace_pc` + per-CPU shared ring in a `.hyp.*` section (mirror `hyp.lds.S:42-46`; evaluate reusing `hyp_trace.c` ring pages); a new kcov-core `kcov_add_pcs()` (append to the current task's area, local-KCOV-mode only); drain calls at each *verified* host→hyp boundary — `kvm_share_hyp` at `KVM_CREATE_VM`/`_VCPU` (`arm.c:175,497`→`mmu.c:591`), `pkvm_create_hyp_vm` at VCPU_INIT (`arm.c:759`), `kvm_arch_vcpu_ioctl_run`, memory-registration (audit); **not** `KVM_ENABLE_CAP` (EL1-only) — on success *and* failure paths, plus teardown (`pkvm_destroy_hyp_vm`, `arm.c:223`) with vCPUs-then-VM close ordering; CPU-pin or `{cpu,generation}` tags from v1; **do not** wrap `kvm_call_hyp_nvhe` globally (§6.3); new `CONFIG_KCOV_PKVM_HYP`.

---

## 9. Verification plan

**Stage 1 (pipeline):**
- `syz-manager` web UI reachable; the board shows as a live VM; no repair loops (`-debug`).
- Coverage climbs and includes host KVM (`arch/arm64/kvm/…`); corpus grows.
- HVC/SMC reach hyp: scp `bin/linux_arm64/syz-execprog` + `sys/linux/test/arm64-syz_kvm_setup_syzos_vm-smc` to the board, run, expect `UEXIT_END`.
- pKVM active on the board (dmesg protected-mode line).
- **Recovery loop end-to-end:** stop petting the watchdog (or `echo c > /proc/sysrq-trigger` with pstore on) → box resets → manager detects reboot → box returns → crash log recovered from `/sys/fs/pstore/console-ramoops-0`.

**Stage 1.5 (ABI + protected-VM modeling):**
- `make extract && make generate` succeed; new pKVM IDs + protected-VM consts appear in `dev_kvm_arm64.txt.const`.
- New tests pass under the executor test harness.
- Short run: generated programs emit the new pKVM `func` IDs and the protected-VM creation sequence; coverage reaches host-side pKVM hypercall dispatch and protected-VM management code (`arch/arm64/kvm/pkvm.c`).

**Stage 2 (EL2 coverage):**
- Host driver unit check: known PCs through the ring → correct PCs pushed.
- Feature detection reports EL2 coverage injection enabled. For ordinary host→hyp boundaries, injected EL2 PCs appear in the coverage of the **corresponding KVM call** (not `extra`/call index −1) and **symbolize to `arch/arm64/kvm/hyp/nvhe/rust/src/*.rs` lines**.
- Teardown attribution is tested per the selected option: under (a) the PCs belong to the explicit `close$kvmvm_protected` call; under (b) the end-of-program output format and its attribution are specified and tested separately.

---

## Appendix A — Stage-2 symbolization prototype (de-risked 2026-07-11; still valid)

Builds the pKVM Rust hyp with debug info on the pinned 1.88 toolchain and proves an EL2 code address resolves to `src/*.rs:line`. Touches nothing in `/home/jose/common` (config → scratch objtree, build → scratch target dir). Result: `debug=false` yields no DWARF; `CARGO_PROFILE_RELEASE_DEBUG=2` yields full `.debug_{info,line,str}` in ~15s incremental; `objdump -dl` on `nvhe_rust::hyp_main::handle_vm_exit_abt` mapped to `src/hyp_main.rs:2603/2607/2612/…`; symbols legacy-mangled (`_ZN…`), demangled by `addr2line -C`. Open half remains: per-PC `addr2line` needs the *linked* `vmlinux` (Rust emits per-function sections based at 0 in the relocatable `.o`), i.e. VA translation + routing (§6.4).

```bash
SC=/tmp/scratch ; PROJ=~/nvhe_rust_kcov_real
rustc +nightly-2025-05-05 --version                 # rustc 1.88.0-nightly, LLVM 20.1.4
rustup component add rust-src --toolchain nightly-2025-05-05
make -C /home/jose/common O="$SC/kbuild" ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- gwd3000_lenovox1d3000m_defconfig
cd "$PROJ"
export RUSTUP_TOOLCHAIN=nightly-2025-05-05 OUT_DIR="$PWD/src/bindings" CARGO_PROFILE_RELEASE_DEBUG=2 CARGO_TARGET_DIR="$SC/nvhe_dbg"
export RUSTFLAGS="-C relocation-model=static -C code-model=small -C opt-level=3 -C panic=abort -C force-unwind-tables=no -C target-feature=-neon --cfg CONFIG_NUMA"
cargo rustc --profile release -Z build-std=core,compiler_builtins,alloc -Z build-std-features=compiler-builtins-mem
LIB="$SC/nvhe_dbg/aarch64-unknown-none/release/libnvhe_rust.a"
aarch64-linux-gnu-ld -r --whole-archive -o "$SC/nvhe_rust_dbg.o" "$LIB"
aarch64-linux-gnu-objdump -h "$SC/nvhe_rust_dbg.o" | grep -E '\.debug_(line|info|str)'
FN=$(aarch64-linux-gnu-nm "$SC/nvhe_rust_dbg.o" | grep hyp_main | grep handle_vm_exit_abt | awk '{print $3}' | head -1)
aarch64-linux-gnu-objdump -dl --disassemble="$FN" "$SC/nvhe_rust_dbg.o" | grep -E '\.rs:[0-9]'
```

(In production, drop `--cfg CONFIG_NUMA` — the kernel's `include/generated/rustc_cfg` supplies it — and gate `debug` on the coverage config.)

---

## Appendix B — Verified anchor table (re-checked 2026-07-20)

**Kernel (`/home/jose/common`, `2030/bug930`):**
| Fact | Anchor |
|---|---|
| nVHE instrumentation disabled (KCOV/GCOV/KASAN/KCSAN/UBSAN := n) + rationale | `arch/arm64/kvm/hyp/nvhe/Makefile.nvhe:88-95` |
| `__kvm_nvhe_` symbol prefix (two-object cause) | `arch/arm64/kvm/hyp/nvhe/Makefile.nvhe:77` |
| nVHE partial-link pipeline | `arch/arm64/kvm/hyp/nvhe/Makefile.nvhe:29-67` |
| Rust hyp linked in iff `CONFIG_X1_RMS` | `arch/arm64/kvm/hyp/nvhe/Makefile:18-22,36-38` |
| `.hyp.xcore_test_cases` section template (Stage-2 ring) | `arch/arm64/kvm/hyp/nvhe/hyp.lds.S:42-46` |
| `CONFIG_X1_RMS` default y; `XCORE_UNIT_TEST` deps | `arch/arm64/kvm/Kconfig:116-118,136-139` |
| `build_rust.sh`: floating pin / target / RUSTFLAGS / rustc_cfg / collapse | `…/rust/build_rust.sh:29,30,52,61-63,85-90` |
| defconfig: KASLR on / KCOV unset / KVM on / KALLSYMS_ALL / DWARF+BTF / PSTORE_RAM=m / USER_NS / pKVM bits | `arch/arm64/configs/gwd3000_lenovox1d3000m_defconfig:76,48,113,1214-1215,1117,43,339` |
| protected-VM uAPI constants | `include/uapi/linux/kvm.h:919,923,1202`; `arch/arm64/include/uapi/asm/kvm.h:478,479,481` |
| protected enable-cap handler + constraints | `arch/arm64/kvm/pkvm.c:457,464,638-665` |
| can't upgrade normal→protected (`pkvm.enabled` set at create) | `arch/arm64/kvm/pkvm.c:455-465` |
| C guest HVC handlers — **compiled out when `X1_RMS=y`** | `pkvm.c:1663` (`#ifndef CONFIG_X1_RMS`); `:1767` non-prot (MEMINFO/RELINQUISH), `:1697-1759` prot (full) |
| **live Rust HVC handlers** (thin: generic + stubs) | `…/rust/src/hyp_main.rs:2085-2108` (normal VM), `:2171-2183` (protected stubs) |
| `SET_FW_IPA` only records IPA; a covering region loads pvmfw | `arch/arm64/kvm/pkvm.c:619-635`; `arch/arm64/kvm/hyp/include/nvhe/pkvm.h:150` |
| `CREATE_VM`/`_VCPU` share structs to EL2 via `kvm_share_hyp`→`__pkvm_host_share_hyp` | `arch/arm64/kvm/arm.c:175,497`; `mmu.c:591` |
| hyp-VM **object** create at `VCPU_INIT`; destroy on **fd teardown** (not an ioctl) | `arm.c:759` `pkvm_create_hyp_vm`; `:223` `kvm_arch_destroy_vm`→`pkvm_destroy_hyp_vm`; run via `kvm_arch_vcpu_ioctl_run` |
| `KVM_ENABLE_CAP` INFO/SET_FW_IPA are **EL1-only** (no hyp entry) | `arch/arm64/kvm/pkvm.c:619-647` |
| vCPU fd holds a VM ref (`kvm_get_kvm`); dropped on vCPU release | `virt/kvm/kvm_main.c:4013, 3886` |
| `kcov_start` sets `t->kcov_mode`; `KCOV_REMOTE_ENABLE` does not; remote-start guard | `kernel/kcov.c:346-359, 620-675, 859-863` |
| host↔hyp trace-buffer ring pages (Stage-2 reuse candidate) | `arch/arm64/kvm/hyp_trace.c:~295` |

**syzkaller (`claude/syzkaller-source-learning-vvisp3`, HEAD `3ccdc5297`):**
| Fact | Anchor |
|---|---|
| SyzOS `guest_main` switch | `executor/common_kvm_arm64_syzos.h:99,109-163` |
| SMC/HVC handlers (`smc/hvc #0`) | `executor/common_kvm_arm64_syzos.h:287-330` |
| `api_call_smccc` (func_id + params[5]) | `executor/common_kvm_arm64_syzos.h:45-49` |
| `syzos_api_id` enum (SVC=290 top; 310 free) | `executor/common_kvm_arm64_syzos.h:15-33` |
| `kvm_smc_id` set / `syzos_api_smccc` / union | `sys/linux/dev_kvm_arm64.txt:134 / 136-139 / 210-229` |
| KVM pseudo-call signatures | `sys/linux/dev_kvm_arm64.txt:18,22,27,30` |
| SMC/HVC test template (VM type 0x0, SMCCC filter, UEXIT_END) | `sys/linux/test/arm64-syz_kvm_setup_syzos_vm-smc` |
| "Add a SyzOS command" procedure | `docs/syzos.md:148-209` (§6), tests `:213+` |
| KCOV_SUBSYSTEM_* + `kcov_remote_handle` | `executor/executor_linux.h:42-46,48-53` |
| `cover_enable`: main cover = local `KCOV_ENABLE`; only `extra` uses `KCOV_REMOTE_ENABLE` | `executor/executor_linux.h:182,189,194-204` |
| `write_extra_output` (index −1, errno 997) + packing | `executor/executor.cc:1443-1451,1466-1481` |
| extra→host merge | `pkg/rpcserver/runner.go:454-463`; `pkg/flatrpc/flatrpc.fbs:243-248` |
| isolated console = `ssh dmesg -w` | `vm/vmimpl/console.go:91-94`; `vm/isolated/isolated.go:308,354` |
| pstore Diagnose / waitForReboot / repair / knobs | `vm/isolated/isolated.go:26,34-43,216-241,262,383` |
| KCOV gating (`-arm`,`-s390`,`-nokcov`; NO arm64) | `dashboard/config/linux/bits/base.yml:135-138`; `main.yml:16-19` |
| host-KVM fuzzing x86-only (arm is new ground) | `dashboard/config/linux/bits/subsystems.yml:75` |
| module discovery is `.ko`-only (`module_obj` can't take `kvm_nvhe.o`) | `pkg/cover/backend/modules.go:19,76`; `pkg/mgrconfig/config.go:44-48` |
| cross-tool prefix from `linux/arm64` | `docs/linux/coverage.md:18`; `sys/targets/targets.go:300,820-829` |
| isolated setup requirements | `docs/linux/setup_linux-host_isolated.md:15-19,26-35,56-65,81-87` |
| `sandbox: setuid` → uid/gid 65534 (nobody); can't open `/dev/kvm` | `executor/common_linux.h:4696` |
| `namespace` sandbox maps ns-uid-0→real-uid + drops caps (not 65534) | `executor/common_linux.h:4716` |
| defaults: `Sandbox: none`, `Procs: 6` | `pkg/mgrconfig/load.go:100,103` |
| `close_fds()` runs *after* per-call output (teardown cov orphaned) | `executor/executor.cc:1152,1158-1162` |
| `getTraceCallbackType` matches only *unprefixed* `__sanitizer_cov_trace_pc` | `pkg/cover/backend/elf.go:50-59` |
| `KVM_CREATE_VM` sig (normal `fd_kvmvm`; `type intptr[0:64]` can't set bit 31) | `sys/linux/dev_kvm.txt:22` |
