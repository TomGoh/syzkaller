# pKVM (arm64 EL2) syzkaller support — v1 implementation & deployment record

> Companion to the design spec `2026-07-20-pkvm-syzkaller-support-design.md` and the implementation plan `~/.claude/plans/generic-sniffing-puddle.md`. This document records what was actually built, how it was deployed to the physical board **N90**, and how the protected-VM lifecycle was verified on hardware. Dated 2026-07-20.

## 0. Status (honest)

**Current status: Stage 1/1.5 host→hyp lifecycle sub-goal ACHIEVED.** (Stage 1/1.5's *other* modeled surfaces — guest HVC + `kvm_smc_id`, SyzOS-in-protected-VM — remain deferred, §7; this milestone is specifically the host→hyp protected-VM lifecycle.) syzkaller runs a **sustained, coverage-guided, BUG1-avoiding** fuzzing campaign against the pKVM host→hyp lifecycle on the physical arm64 board **N90** — verified over a supervised ~2 h run (corpus 0→93, coverage 0→6407, **300k+ executions at ~39/s, 0 crashes**; details §6.3). *Boundary:* that `coverage=6407` is syzkaller's accumulated host-side KCOV **signal count** (most of it the executor's own sandbox setup; ~597 PCs in `arch/arm64/kvm`, of which the full pKVM lifecycle `pkvm_create_hyp_vm … __pkvm_destroy_hyp_vm` is covered) — it is **not** EL2/Rust-hyp line coverage, which remains Stage 2.

**The rest of this §0 (and §4) is historical progression, not current status** — it records the *starting* manual smoke test and its honest limits, which led to the milestone above via: manual lifecycle smoke test (Phase 3) → controlled `immediate_exit` run primitive (§6.2) → type-safe constrained campaign (§6.3). Read §0 below as "how we got here."

The starting smoke test (Phase 3, on N90): running the **7-call** lifecycle program (§1.4) via `syz-execprog`, entry kprobes on both host↔hyp lifecycle functions fire:

```
pcreate:  (pkvm_create_hyp_vm+0x0/0x560)   ← create path reached (at first KVM_RUN)
pdestroy: (pkvm_destroy_hyp_vm+0x0/0x48)   ← destroy path reached (at the final VM-fd close)
```

**Caveat (why this is still a *smoke test*, not a launched campaign).** An entry kprobe proves the path was *reached*, not that it *succeeded*. An earlier draft asserted three stronger signals that its recorded commands could not produce. **Phase B (2026-07-21) re-captured two of the three on N90** (evidence: `evidence/2026-07-21-phaseB-n90-reverification.md`); the third remains:
1. **kretprobe `$retval = 0`** on `pkvm_create_hyp_vm` — ✔ **captured** (`pcret … <- pkvm_create_hyp_vm ret=0x0`, caller `kvm_arch_vcpu_run_pid_change` = the first-`KVM_RUN` path).
2. **A real KCOV count on `arch/arm64/kvm/pkvm.c`** — ✔ **captured**: the protected `KVM_CREATE_VM` call yields `coverage 62372`, symbolizing to `pkvm.c:457-465 pkvm_init_host_vm` + the `mmu.c` host→hyp share path. (The earlier `-cover=0` command collected nothing — `syz-execprog`'s `-cover` is `flagSignal`, `execprog.go:130`.)
3. **ramoops crash-recovery** — ☐ **not available on N90 as-is** (see §6): N90 is **ACPI/no-DT**, so neither a DT `reserved-memory` node nor a safe cmdline RAM reservation applies, and `console-ramoops-0` never appears. So the campaign is **supervised-only, not auto-recoverable**; unattended recovery needs an **efi-pstore** adaptation (§6), not a `ramoops.mem_*` cmdline.

**Two Phase-B findings** (details in §4.1): `KVM_RUN` **blocks and is killed** by the executor timeout (it does *not* "fault out cleanly"); and because it is killed before KCOV readout, the `KVM_RUN`→`pkvm_create_hyp_vm` path itself contributes **0 coverage** — the rich pKVM coverage comes from `KVM_CREATE_VM` (call #1). Both are Phase-4/5 refinements, not blockers for the lifecycle.

Scope of v1 = **Strategy 1 (host→hyp protected-VM lifecycle)** only: create a protected VM → create + init a vCPU → `KVM_RUN` (builds the hyp VM) → close vCPU then VM (tears it down). No guest payload, no guest memory, **no pvmfw** (`SET_FW_IPA` is never issued). Guest-HVC fuzzing, EL2 in-hyp KCOV, and pvmfw/firmware handoff are deferred (§7).

Phase 1 (modeling) and Phase 2 (kernel/board bring-up) are complete. Phase 3 (lifecycle verification) re-captured its pending signals in Phase B (§4). **A first *supervised* `syz-manager` run has now happened (§6.1): the manager→N90 execution / log-collection / crash-detection chain works** — it connected as root, feature-probed the target, enabled the 8 host-lifecycle syscalls, and executed ~972 programs before a kernel WARN began dominating. **Two honest limits on that run:** (a) it was **not** a coverage-guided campaign — the manager's periodic stats stayed `corpus=0 coverage=0` throughout (only the KCOV *feature probe* passed; the real 62k coverage evidence is the separate manual `syz-execprog` run in §4, do not conflate them); (b) the WARN it surfaced — `arch/arm64/kvm/arch_timer.c:461 kvm_timer_update_irq` — is a **real userspace-triggerable warning but a *generic* arm64-KVM one that upstream already fixed** (`38d7aacca092` / stable `dd2f9861f275`, "KVM: arm64: Get rid of `userspace_irqchip_in_use`"), *not* a pKVM/EL2/Rust-hyp bug: the triggering `KVM_ARM_VCPU_INIT`+`KVM_RUN` fails the PMU check (`kvm_arm_pmu_v3_enable`, `arm.c:751`) and returns **before** `pkvm_create_hyp_vm` (`arm.c:757`), so it never enters the hyp path. So this is best described as *"fuzzing the pKVM lifecycle entry re-discovered a general arm64-KVM fix this vendor kernel is missing."* The saved artifact is the raw concurrent trigger log, **not** a minimized reproducer (`reproducing=0`). Board stays up (WARN non-fatal). Detail + plan: §6.1. Evidence: `evidence/2026-07-21-BUG1-kvm_timer_update_irq-*`.

**Then the Stage 1/1.5 goal was met (§6.2, §6.3).** A **controlled first `KVM_RUN`** primitive (`syz_kvm_vcpu_run_immediate$arm64`: sets `kvm_run.immediate_exit=1`) crosses the host→hyp first-run boundary — building the hyp VM and collecting its KCOV — then returns `-EINTR` before the guest-abort busy loop (all 5 acceptance criteria verified on N90, §6.2). The BUG1-triggering `VCPU_INIT` was then excluded by a **combination** (§6.3): the campaign **allowlist** drops the fuzzable generic `ioctl$KVM_ARM_VCPU_INIT`; the enabled `ioctl$KVM_ARM_VCPU_INIT_safe` carries a fixed `feature=const[0]` so it *cannot* request PMU_V3; and a **fresh corpus** avoids replaying an old PMU_V3 program. (The `fd_kvmcpu_protected` subresource only *routes* the safe init/run to a protected vCPU — like all syzkaller resource subtypes it can't itself enforce "already safe-init'd", the same permissive-compat limit that drove the slice-2 composite redesign, §6.5.) A **supervised coverage-guided campaign then ran clean for ~2 h**: the manager's own `corpus` 0→93 and `coverage` 0→6407 grew over **300k+ executions at ~39/s with 0 crashes** and **0** new `kvm_timer_update_irq` warnings. So sustained, coverage-guided, BUG1-free fuzzing of the pKVM host→hyp lifecycle on real hardware is **achieved**; deeper EL2/Rust-hyp coverage remains Stage 2.

---

## 1. syzkaller-side implementation (host, x86 — board-independent)

Branch `claude/syzkaller-source-learning-vvisp3` in `/home/jose/syzkaller`. Four edits; the design rationale (type-safety, why create-only, etc.) is in the design spec §5.3.

### 1.1 `sys/linux/dev_kvm_arm64.txt` (after `syz_kvm_add_vcpu$arm64`)

```
resource fd_kvmvm_protected[fd_kvmvm]

kvm_arm_ipa_size = 0, 32, 36, 40, 44, 48

syz_kvm_setup_protected_vm$arm64(fd fd_kvm, ipa_size flags[kvm_arm_ipa_size]) fd_kvmvm_protected

ioctl$KVM_CREATE_VM_PROTECTED(fd fd_kvm, cmd const[KVM_CREATE_VM], type int64[0x80000000:0x800000ff]) fd_kvmvm_protected

close$kvmvm_protected(fd fd_kvmvm_protected)
```

- `fd_kvmvm_protected` is a **subtype** of `fd_kvmvm` → the existing `ioctl$KVM_CREATE_VCPU` / `ioctl$KVM_ARM_VCPU_INIT` (which take `fd_kvmvm`) accept it unchanged.
- The raw `int64[0x80000000:0x800000ff]` range lies wholly inside the bit-31-set interval, so it **mechanically guarantees** `KVM_VM_TYPE_ARM_PROTECTED` (a `flags` set would default to 0 — `prog/types.go:515` — and lie about the returned resource).

### 1.2 `executor/common_kvm_arm64.h` (create-only helper, after `syz_kvm_add_vcpu`)

```c
#if SYZ_EXECUTOR || __NR_syz_kvm_setup_protected_vm
static long syz_kvm_setup_protected_vm(volatile long a0, volatile long a1)
{
	return ioctl(a0, KVM_CREATE_VM, 0x80000000 | (a1 & 0xff));
}
#endif
```

- `a0` = `/dev/kvm` fd, `a1` = IPA size. Bit 31 forced ⇒ the returned resource is truthfully protected.
- **Deliberately does NOT** call `setup_vm()`/`vm_set_user_memory_region()`: those build `KVM_MEM_READONLY|KVM_MEM_LOG_DIRTY_PAGES` memslots that a protected VM rejects with `-EPERM` (`arch/arm64/kvm/mmu.c:2499-2502`), and `vm_set_user_memory_region` ignores the ioctl return (`common_kvm_arm64.h:67`) — a silent success over a real failure. And **no `INFO`/`SET_FW_IPA`** (pvmfw-less).
- `KVM_CREATE_VM` resolves via `<linux/kvm.h>` (included through `common_linux.h:3621`), value `44545` = `0xAE01`.

### 1.3 `pkg/vminfo/linux_syscalls.go` (support map — mandatory)

Two additions (the map routes by CallName; the case gates by arch). Without these the pseudo-syscall is silently disabled.

```go
// in linuxSyscallChecks:
"syz_kvm_setup_protected_vm":    linuxSyzKvmSupported,
// in linuxSyzKvmSupported's arm64 case:
case "syz_kvm_setup_cpu$arm64", ..., "syz_kvm_setup_protected_vm$arm64", ...:
```

### 1.4 `sys/linux/test/arm64-syz_kvm_setup_protected_vm` (new)

Lifecycle test (`# requires: arch=arm64 -threaded`): `openat$kvm` → `syz_kvm_setup_protected_vm$arm64` → `ioctl$KVM_CREATE_VCPU` → `ioctl$KVM_ARM_VCPU_INIT` (`target = KVM_ARM_TARGET_GENERIC_V8 = 0x5`) → `ioctl$KVM_RUN` → `close`(vCPU) → `close$kvmvm_protected`(VM). vCPU is closed before the VM because vCPU fds hold a VM reference (`kvm_get_kvm` at create, `kvm_main.c:4013`).

### 1.5 Build & validation (x86)

```bash
cd /home/jose/syzkaller
make descriptions                 # compiles the syzlang (syz-sysgen)
make TARGETOS=linux TARGETARCH=arm64 TARGETVMARCH=arm64 executor manager execprog
                                  # NB: do NOT pass CC=/CXX= globally — that poisons the host Go/cgo
                                  # build (-m64). syz-make auto-selects aarch64-linux-gnu-* from TARGETARCH.
go build ./pkg/vminfo/            # support-map edit compiles
go test -run='TestParsing/linux' ./pkg/runtest/   # the new test parses; no regression
go test ./prog/                   # Target loads; gen/mutate OK
```

All green. Generated `executor/syscalls.h` gets the dispatch row `{"syz_kvm_setup_protected_vm$arm64", 0, {}, (syscall_t)syz_kvm_setup_protected_vm}`.

---

## 2. Kernel build & config (`/home/jose/common`, branch `2030/bug930`)

Target kernel `6.6.30-pkvm-fuzz`, arm64, cross-compiled on x86 with `aarch64-linux-gnu-*`.

### 2.1 defconfig deltas (`arch/arm64/configs/gwd3000_lenovox1d3000m_defconfig`)

Applied on top of the board defconfig (a backup `.pre-syzkaller.bak` was made):

```
# coverage + symbolization + determinism
CONFIG_KCOV=y
CONFIG_KCOV_INSTRUMENT_ALL=y
CONFIG_DEBUG_FS=y
# CONFIG_RANDOMIZE_BASE is not set        # KASLR off (was =y)
# oracle + crash capture
CONFIG_KASAN=y
CONFIG_KASAN_GENERIC=y
CONFIG_DETECT_HUNG_TASK=y
CONFIG_BOOTPARAM_HUNG_TASK_PANIC=y
CONFIG_PSTORE=y
CONFIG_PSTORE_RAM=y                        # was =m — builds the driver only; see the caveat below
CONFIG_PSTORE_CONSOLE=y
CONFIG_ARM_SBSA_WATCHDOG=y
# lifecycle-verification tracing (entry + return probes)
CONFIG_KPROBES=y
CONFIG_KPROBE_EVENTS=y
# (KRETPROBES is auto-selected by KPROBES on arm64 → kretprobe $retval works)
# X1_RMS (Rust hyp) is default y — verified =y in the generated .config
```

**Two corrections to an earlier draft of this section:**
- **`CONFIG_FPROBE`/`CONFIG_FPROBE_EVENTS` were removed.** They were written into the defconfig but **silently dropped from the generated `.config`** (this arm64 tree does not satisfy FPROBE's function-graph dependency). They gave nothing over KPROBE anyway — the return-value gate uses **kretprobes**, which `CONFIG_KPROBES=y` auto-selects (`CONFIG_KRETPROBES=y` is confirmed in the built `.config`). A stale duplicate `CONFIG_DEBUG_INFO_BTF=y` was also removed (the intended `# CONFIG_DEBUG_INFO_BTF is not set` remains).
- **`CONFIG_PSTORE_RAM=y` alone does not enable ramoops recovery — and on N90 it cannot be enabled safely at all.** It compiles the driver but reserves no memory. A ramoops region needs a DT `reserved-memory` node or a safe cmdline reservation, and **N90 has neither** (it is ACPI/no-DT; arm64 6.6 has no safe cmdline reservation — see §6). So on this board the crash-recovery chain is **not available as-is**; the fix is an **efi-pstore** adaptation, not this config delta. The campaign runs **supervised-only**.

### 2.2 Toolchain / GCC-15 build blockers (all had to be resolved)

The installed cross toolchain is **GCC 15.2.0**, far newer than this 6.6.30 tree; three of its stricter behaviours broke the build, plus a Rust-pin hazard:

| Symptom | Cause | Fix |
|---|---|---|
| host `tools/bpf/resolve_btfids/libbpf` fails `cc1: all warnings being treated as errors` | `CONFIG_DEBUG_INFO_BTF=y` builds a host libbpf that trips gcc-15 `-Werror` | `# CONFIG_DEBUG_INFO_BTF is not set` (BTF unused for KCOV; DWARF is separate) |
| driver compiles fail `-Werror=address` / `-Werror=unterminated-string-initialization` / `-Werror=frame-larger-than` (phytium DRM, btrfs, amdkfd) | gcc-15 new diagnostics promoted to errors by global `-Werror` | `# CONFIG_WERROR is not set` (benign warnings on a newer toolchain) |
| modpost: `"__sanitizer_cov_trace_cmpf" [ftd330-drm-dc.ko] undefined!` | gcc-15 `-fsanitize-coverage=trace-cmp` emits a **float**-comparison callback this kcov core doesn't implement | `# CONFIG_KCOV_ENABLE_COMPARISONS is not set` (keep core trace-pc KCOV; drop comparison coverage) |
| Rust hyp built with wrong rustc | `build_rust.sh:29` floats `RUSTUP_TOOLCHAIN=nightly` (a separate, newer toolchain) | pin to `nightly-2025-05-05` (installed default; `rust-src` present) |

### 2.3 Build commands

```bash
cd /home/jose/common
sed -i 's/^export RUSTUP_TOOLCHAIN=nightly$/export RUSTUP_TOOLCHAIN=nightly-2025-05-05/' \
    arch/arm64/kvm/hyp/nvhe/rust/build_rust.sh
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- gwd3000_lenovox1d3000m_defconfig
# verify: X1_RMS=y, KCOV=y, RANDOMIZE_BASE off, WERROR/BTF/KCOV_ENABLE_COMPARISONS off
LOCALVERSION=-pkvm-fuzz make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- -j"$(nproc)" Image modules
```

Artifacts: `arch/arm64/boot/Image` (61.7 MB), `vmlinux` (480 MB, full DWARF for symbolization), `arch/arm64/kvm/hyp/nvhe/rust/nvhe_rust.o` (Rust hyp), 577 `.ko`, `include/config/kernel.release = 6.6.30-pkvm-fuzz`.

---

## 3. N90 deployment

N90 = `kylin-pc`, Kylin V10 SP1 aarch64 (Phytium gwd3000-class). `ssh N90` → `kylin@10.42.27.17`, key `~/.ssh/id_ed25519`, passwordless sudo.

### 3.1 Kernel install (brick-safe, via the `kylin-v10-kernel-deploy` skill)

```bash
SKILL=~/.claude/skills/kylin-v10-kernel-deploy/scripts/deploy-kernel.sh
cd /home/jose/common
LOCALVERSION=-pkvm-fuzz "$SKILL" install N90 --no-build      # modules + Image + initramfs
"$SKILL" grub-entry N90 --label "6.6.30-pkvm-fuzz (pKVM protected)" \
                        --cmdline "kvm-arm.mode=protected loglevel=7"
```

The script installs modules into the **real** `/usr/lib/modules/<KVER>` (never extracting into `/`, which would sever the `/lib -> usr/lib` usrmerge symlink and brick the box — confirmed `/lib` intact), adds a **non-default** GRUB entry, and pins the default to the known-good 5.4 kernel.

### 3.2 Make every 6.6-fuzz entry boot protected

The first reboot booted the *auto-generated* plain `6.6.30-pkvm-fuzz` entry, whose cmdline lacked `kvm-arm.mode=protected` → protected mode did **not** activate. Fix — add the param to the common cmdline (5.4 ignores it):

```bash
ssh N90 'sudo sed -i "s/\(GRUB_CMDLINE_LINUX=\"[^\"]*\)\"/\1 kvm-arm.mode=protected\"/" /etc/default/grub && sudo update-grub'
```

Now every `6.6.30-pkvm-fuzz` menu entry (normal/backup/restore/recovery/custom) carries `kvm-arm.mode=protected`.

### 3.3 Board setup for the executor

```bash
ssh N90 'sudo usermod -aG kvm kylin'   # /dev/kvm is root:kvm 0660; kylin needs the group
```
`AllowTcpForwarding` is at its default (`yes`) — fine for syzkaller's reverse tunnels.

### 3.4 Confirmed after reboot

```
# uname -r → 6.6.30-pkvm-fuzz ; /proc/cmdline has kvm-arm.mode=protected
[    0.068381] CPU features: detected: Protected KVM
[    0.068487] CPU: All CPU(s) started at EL2
[    3.681816] No SMMU devices found for pKVM driver, continuing with initialization
# /sys/kernel/debug/kcov present ; /dev/kvm openable by kylin ; /sys/kernel/tracing/kprobe_events present
```

---

## 4. Phase-3 verification (on N90)

Copied `bin/linux_arm64/{syz-execprog,syz-executor}` + the program to `~/syzkaller` on N90. Confirmed `pkvm_create_hyp_vm` / `pkvm_destroy_hyp_vm` are global text symbols (`T`) in `/proc/kallsyms` (kprobe-able).

Program run (`sudo` required — see §5):

```
#0 -> openat$kvm(...)                         <- =0x3
#0 -> syz_kvm_setup_protected_vm$arm64(0x3,0) <- =0x4   (protected VM created; bit 31 accepted)
#0 -> ioctl$KVM_CREATE_VCPU(0x4, 0xae41, 0)   <- =0x5   (subtype flows into stock ioctl)
#0 -> ioctl$KVM_ARM_VCPU_INIT(0x5, ...)       <- =0x0   (target=GENERIC_V8)
#0 -> ioctl$KVM_RUN(0x5, ...)                          (BLOCKS → executor kills worker pid; completed=4)
#0    close(0x5) / close$kvmvm_protected(0x4)          NOT EXECUTED (worker already killed at KVM_RUN)
```
⇒ In this **`-threaded=0`** run only **4 calls completed** (`openat`, `setup_protected_vm`, `CREATE_VCPU`,
`VCPU_INIT`); `KVM_RUN` (call #4) blocked and the worker was killed, so the two `close` calls never ran.
Teardown still happened: `pkvm_destroy_hyp_vm` fires from **kernel fd-cleanup when the killed worker's VM fd
drops**, *not* from the explicit `close$kvmvm_protected` (which was never reached). The explicit-close teardown
path is exercised only when the harness runs past `KVM_RUN` — see the threaded characterization (§4.4).

```
CONFIRMED (Phase B, 2026-07-21, on N90 — evidence/2026-07-21-phaseB-n90-reverification.md):
  fds + returns for calls #0-#3, VCPU_INIT = 0
  entry kprobe:   pkvm_create_hyp_vm ✓   pkvm_destroy_hyp_vm ✓
  kretprobe:      pcret ... <- pkvm_create_hyp_vm ret=0x0   ← create RETURNED SUCCESS
  KCOV:           KVM_CREATE_VM(protected) → coverage 62372, symbolizes to
                  pkvm.c:457-465 pkvm_init_host_vm + mmu.c kvm_share_hyp/share_pfn_hyp
  dmesg diff:     no pKVM/hyp/WARN/BUG/oops/KASAN lines
  teardown:       via fd-cleanup on worker kill (NOT the explicit close, which didn't run)

STILL PENDING:
  §4.4 threaded (manager-like) characterization of KVM_RUN hang + explicit-close teardown + coverage recovery
  §4.3 step 3 ramoops recovery — no reserved RAM region on N90 yet; forced-panic test not run
```

**Two-stage finding (hardware corrected the paper design):** with the *first* program (no `KVM_RUN`) only `pkvm_destroy_hyp_vm` fired — `pkvm_create_hyp_vm` is called on the vCPU's **first `KVM_RUN`** (`arch/arm64/kvm/arm.c:755-759`, `kvm_arch_vcpu_run_pid_change`), **not** at `VCPU_INIT` as the review had assumed. Adding `KVM_RUN` made the hyp-VM create fire. Exactly the "static audit enumerates, hardware validates" principle.

### 4.1 Verification rigor — what this proves, and its limits (honest)

An **entry** kprobe is *not* proof of success — it only proves the function was *reached*. So the entry-kprobe evidence in §4 establishes the lifecycle *runs and reaches both host→hyp functions*, and no more. The stronger signals below are the real **acceptance gate**; they are captured in Phase B (§4.3), not claimed here.

- **The success signal is the kretprobe `$retval`, which was not captured.** `r:name pkvm_create_hyp_vm $retval` reading **0** would prove the hyp VM was created *successfully*, not merely entered. The recorded run armed only entry probes, so this is **pending** (`pkvm_destroy_hyp_vm` is `void` — its "retval" is meaningless; there, the entry probe plus a successful create is the intended proof).
- **A per-run dmesg diff must be clean, and must actually be recorded.** The plan is `dmesg | wc -l` before, then grep only the new tail after for `pkvm`/`hyp`/`WARN`/`BUG`/`error`/`fault`. No such capture exists in the record yet → **pending**.
- **Full acceptance gate** (what Phase B must show): valid fds + `VCPU_INIT = 0` + **kretprobe `$retval = 0`** + **clean dmesg diff** + **a non-zero `cover=` count on `pkvm.c`**. Failure would be detectable at every layer: `KVM_CREATE_VM` → fd `-1` (downstream `EBADF`); `VCPU_INIT` → `-errno` in `-debug`; hyp create → kretprobe `-errno` + a dmesg line.
- **"Protected" comes from the ABI, not the kprobes.** (This part *is* established.) Under `kvm-arm.mode=protected` the hyp manages *every* VM, so `pkvm_create_hyp_vm`/`pkvm_destroy_hyp_vm` fire for normal VMs too. What makes *this* VM protected is that `KVM_CREATE_VM` **accepted bit 31** (`KVM_VM_TYPE_ARM_PROTECTED`) — the kernel rejects unknown type bits with `EINVAL`, and `pkvm_init_host_vm` sets `pkvm.enabled` iff that bit is set. A rigorous protected-vs-normal discriminator (e.g. observing guest-memory isolation) is a Stage-2 / security-oracle concern, **not done here**.
- **`KVM_RUN` blocks — it does *not* fault out cleanly (Phase-B correction).** pvmfw-less with no guest memory, the vCPU enters at the host-set reset PC (`pkvm.rs:1294-1298`) but then **hangs**: the executor logs `killing hanging pid 2` and the program-timeout kills the worker thread (~3 s), after which `close` runs and `pkvm_destroy_hyp_vm` fires. The create (`ret=0`) → destroy lifecycle still completes, so this is not a lifecycle failure — but an earlier "faults out cleanly (no hang)" claim was wrong. **Consequence:** because `KVM_RUN` is killed before KCOV readout, that call reports `coverage 0`, so the `pkvm_create_hyp_vm` host path contributes no coverage; making the pvmfw-less vCPU exit `KVM_RUN` promptly (or shortening the run) to recover that coverage is a Phase-4/5 refinement.
- **KCOV is measured and lands on pKVM code (Phase B).** A separate `-cover=1` run (the `-cover=0` run collects nothing) shows the protected `KVM_CREATE_VM` at `coverage 62372`, symbolizing to `arch/arm64/kvm/pkvm.c:457-465 pkvm_init_host_vm` plus the `mmu.c` host→hyp share path — so KCOV instruments the target and the Phase-4 campaign will accrue real pKVM coverage.

### 4.2 Lifecycle + `$retval` capture (root) — the reproducible version

This is the run that produces the CONFIRMED evidence *plus* the pending kretprobe/dmesg signals. Note it arms a **return** probe (`r:pcret … ret=$retval`), which the earlier draft omitted, and it keeps `-cover=0` (coverage is a *separate* run, §4.3):
```bash
T=/sys/kernel/tracing
sudo sh -c "echo 'p:pcreate  pkvm_create_hyp_vm'          >  $T/kprobe_events"
sudo sh -c "echo 'r:pcret    pkvm_create_hyp_vm ret=\$retval' >> $T/kprobe_events"
sudo sh -c "echo 'p:pdestroy pkvm_destroy_hyp_vm'         >> $T/kprobe_events"
sudo sh -c "echo 1 > $T/events/kprobes/enable; echo > $T/trace"
N0=$(dmesg | wc -l)                                     # dmesg baseline
cd ~/syzkaller && sudo ./syz-execprog -executor=./syz-executor -arch=arm64 -os=linux \
    -sandbox=none -threaded=0 -cover=0 -debug pkvm_lifecycle.syz
sudo cat $T/trace | grep -E 'pcreate|pcret|pdestroy'    # expect: pcret ... ret=0x0
dmesg | tail -n +$((N0+1)) | grep -iE 'pkvm|hyp|WARN|BUG|error|fault' || echo "dmesg diff clean"
```
Acceptance: `pcreate` + `pdestroy` fire, `pcret` shows `ret=0x0`, and the dmesg diff is empty.

### 4.3 Phase B — re-verification on N90 (steps 1–2 DONE 2026-07-21, step 3 pending)

Three captures, saved into `evidence/2026-07-21-phaseB-n90-reverification.md`:

1. ✔ **DONE — Lifecycle + return value + dmesg** — ran §4.2; `pcret … <- pkvm_create_hyp_vm ret=0x0`, dmesg diff clean.
2. ✔ **DONE — KCOV on `pkvm.c`** — a *separate* run with signal collection on (protected `KVM_CREATE_VM` → `coverage 62372` → `pkvm.c:457-465 pkvm_init_host_vm` + `mmu.c` share path):
   ```bash
   cd ~/syzkaller && sudo ./syz-execprog -executor=./syz-executor -arch=arm64 -os=linux \
       -sandbox=none -threaded=0 -cover=1 -coverfile=/tmp/pkvm.cov -debug pkvm_lifecycle.syz
   wc -l /tmp/pkvm.cov            # non-zero PCs = KCOV collected signal
   ```
   Then confirm PCs land in `arch/arm64/kvm/pkvm.c` (symbolize the coverfile against `vmlinux`, or check on the manager side once Phase 4 runs). A zero/empty coverfile means KCOV is not instrumenting the path and must be fixed before the campaign.
3. ☐ **N/A on N90 — ramoops recovery is not available as-is** (superseded by §6's ACPI finding). N90 is ACPI/no-DT, so the `ramoops.mem_address=`/DT-node approaches do **not** safely apply and `console-ramoops-0` never appears. Unattended crash-recovery instead needs an **efi-pstore** adaptation (enable `efi_pstore` + teach the isolated backend to read `dmesg-efi-*` rather than `console-ramoops-0`). Until then the campaign is **supervised-only**, with live `ssh dmesg -w` as the oracle.

Only after 1–3 are green **and §4.4 is understood** does the status in §0 get upgraded and the Phase-4 campaign (§6) start.

### 4.4 Threaded (manager-like) characterization — DONE 2026-07-21

`syz-manager` runs **threaded** by default (`pkg/fuzzer/fuzzer.go` sets `ExecFlagThreaded`), and the `-threaded=0`
run above does *not* predict its behavior. Re-ran the same program with **`-threaded=1 -cover=1`** on N90:

```
#0 -> ioctl$KVM_RUN(0x5,...)            (issued on thread #0 — BLOCKS, never returns)
#1 -> close(0x5)              <- =0x0   (thread #1, ~1s later — the vCPU close RUNS)
#1 -> close$kvmvm_protected(0x4) <- =0x0 (the VM close RUNS)
handle completion: completed=7          (ALL 7 calls complete, unlike completed=4 non-threaded)
per-call coverage: #1 KVM_CREATE_VM=62884 ... #4 KVM_RUN=524287(=0x7FFFF cap) #5 close=117 #6 close=597
kretprobe: pcret <- pkvm_create_hyp_vm ret=0x0 ;  pdestroy fires ;  dmesg clean
```

Findings (these decide the campaign shape):
- **Threaded runs the whole program.** The hung `KVM_RUN` parks on one thread; after a ~1 s per-call timeout the
  executor issues the rest on another thread, so **the explicit `close`s execute** and teardown goes through them
  (plus final thread-kill cleanup). All 7 calls "complete" and every call yields coverage — the non-threaded
  coverage blind-spot on `KVM_RUN` does **not** occur here.
- **But `KVM_RUN` busy-loops.** Its `524287` = `0x7FFFF` is the KCOV buffer cap: **974 unique PCs repeated ~500×**.
  The loop is `kvm_arch_vcpu_ioctl_run → kvm_handle_guest_abort → inject_abt64 → re-enter → abort again` — a
  no-progress guest-abort loop (pvmfw-less, no guest memory, no exception vectors). It traverses real
  pKVM-adjacent code (`kvm_handle_guest_abort`, `__pkvm_create_hyp_vcpu`, `kvm_share_hyp`, timer/vgic), so it adds
  ~974 useful PCs — but the same set each time, and it **floods KCOV + costs ~1 s latency per program**.
- **Consequences for Phase 4:** (a) dedup (`ExecFlagDedupCover`) absorbs the flood, so signal quality is OK; (b)
  throughput takes a ~1 s hit on every program that reaches `KVM_RUN`, and since those programs *add* coverage the
  fuzzer will favor them → **do not make `KVM_RUN` the campaign's main call until it exits promptly.** A supervised
  first campaign should lean on the **host-side lifecycle** (`KVM_CREATE_VM` protected → `CREATE_VCPU` →
  `VCPU_INIT` → close), which is fast and already covers `pkvm_init_host_vm` + the host→hyp share path, while
  keeping `KVM_RUN` available but not dominant. Making the pvmfw-less vCPU exit `KVM_RUN` promptly (so
  `pkvm_create_hyp_vm`/guest-abort coverage is captured without the spin) is a Phase-4/5 refinement.

---

## 5. Key findings & gotchas (consolidated)

- **The executor needs root on the target.** The hard failure as `kylin` is the **`mount(tmpfs): Operation not permitted`** during executor setup (it needs `CAP_SYS_ADMIN`); the preceding `unshare(CLONE_NEWNS)` is not the blocker. Even `sandbox:none` hits this. Phase-3 used `sudo`; Phase-4 needs root SSH (§6).
- **`pkvm_create_hyp_vm` is at first `KVM_RUN`, not `VCPU_INIT`** (§4).
- **GCC 15 vs a 6.6.30 tree** breaks in three independent ways (WERROR, BTF host tool, `cmpf`) — all config-disabled (§2.2). If a matching older toolchain is available, comparison coverage (`KCOV_ENABLE_COMPARISONS`) and BTF could be restored.
- **pvmfw-less works** (`INFO.firmware_size` is *global*, not per-VM, so never assert on it; not issuing `SET_FW_IPA` keeps the VM firmware-less and the vCPU enters at the host-set PC — `pkvm.rs:1294-1298`).
- **usrmerge brick hazard** on Kylin V10 (`/lib -> usr/lib`) — always install modules via the skill's version-dir copy, never `tar -C /`.
- **Boot the labelled/protected GRUB entry**, or put `kvm-arm.mode=protected` in `GRUB_CMDLINE_LINUX` (the auto entries otherwise lack it).
- **syzkaller cross-build:** never set `CC=`/`CXX=` globally (poisons the host Go/cgo build with `-m64`); `syz-make` auto-selects from `TARGETARCH`.
- **`make generate` needs `flatc`** (absent here) for its `generate_rpc` sub-target; use `make descriptions` to regenerate just the syzlang.

---

## 6. Phase 4 — continuous campaign (§6.1 first run + BUG1; §6.2 controlled run; §6.3 coverage-guided campaign ACHIEVED)

Manager config `/home/jose/syzkaller/workdir/pkvm-arm64.cfg`: `type: isolated`, `target: linux/arm64`, `kernel_obj`/`kernel_src` = `/home/jose/common`, `sshkey` = `~/.ssh/id_ed25519`, `sandbox: none`, `procs: 1`, `target_reboot`+`pstore`, `vm.targets: ["10.42.27.17"]`.

Two board-side prerequisites remain (both change N90's security/boot posture — get user sign-off):
1. **Passwordless root SSH** on N90 (executor needs root, §5) → set `ssh_user: root`.
2. **Fuzzing kernel as GRUB default** (so `target_reboot` recovery returns to it, not 5.4).

**Campaign shape (from §4.4):** for a *supervised* first run, focus `enable_syscalls` on the **fast host-side
lifecycle** — `openat$kvm`, `syz_kvm_setup_protected_vm$arm64`, `ioctl$KVM_CREATE_VCPU`,
`ioctl$KVM_ARM_VCPU_INIT`, `close$kvmvm_protected` (+ the raw `ioctl$KVM_CREATE_VM_PROTECTED`) — which already
cover `pkvm_init_host_vm` + the host→hyp share path with no hang. Keep `ioctl$KVM_RUN` **available but not the
main driver** (it busy-loops ~1 s and the fuzzer would over-favor it) until it can be made to exit promptly.

**Recovery oracle on N90 — ramoops is NOT straightforward here (Phase-B read-only prep, 2026-07-21):**
- **N90 is ACPI/UEFI, not DT** (`no /sys/firmware/devicetree/base`; ACPI DSDT/FACP/GTDT/IORT present). So the
  design spec's **DT `reserved-memory` ramoops node is impossible** on this board, and arm64 6.6 has no safe
  cmdline RAM reservation (`memmap=` is x86-only; `reserve_mem=` is ≥6.12). A bare `ramoops.mem_address=` would
  point at un-reserved RAM → corruption. So **`pstore:true` + `console-ramoops-0` recovery is not available
  out-of-the-box.**
- syzkaller's isolated backend reads **exactly** `/sys/fs/pstore/console-ramoops-0` (`vm/isolated/isolated.go:26`).
  The board *does* have `CONFIG_EFI_VARS_PSTORE=y` + working efivarfs, so **efi-pstore** (UEFI-variable backed, no
  reserved RAM) is a candidate — but it writes `dmesg-efi-*`, not `console-ramoops-0`, so it needs a small
  syzkaller change to be consumed, plus care about UEFI NVRAM wear.
- **Recommendation:** run the *supervised* first campaign with **`pstore: false`**, using live `ssh dmesg -w` as
  the oracle (works today; a wedged board is power-cycled by hand while supervised). Solve persistent recovery
  (efi-pstore + small syzkaller patch, or another RAM-reservation route) **before any unattended run**.

Then: `cd /home/jose/syzkaller && ./bin/syz-manager -config=workdir/pkvm-arm64.cfg` and confirm coverage climbs on `arch/arm64/kvm/pkvm.c`. **Do not add `-debug`** for the campaign: the SSH `LogLevel=ERROR` fix (§6.1) only applies when *not* in debug mode, so `-debug` re-exposes the OpenSSH post-quantum banner into the console stream and re-triggers false crashes. Use `-debug` only for one-off connection troubleshooting.

### 6.1 First supervised campaign — DONE 2026-07-21 (pipeline works; first bug found)

Set up root SSH on N90 (`/root/.ssh/authorized_keys` = manager pubkey; `PermitRootLogin without-password` was
already set) and ran the manager with `ssh_user:root`, `pstore:false`, `target_reboot:false`, and
`enable_syscalls` = the 8 host-lifecycle calls. Results:

- **The transport chain works end-to-end (this is what the run proves):** manager connected as root,
  feature-probed the target (KCOV *feature probe* `Coverage: enabled`, `SandboxNone`, `Swap`), enabled
  **`syscalls: 8/8054`** (all 8 lifecycle calls supported), loaded a 54-seed corpus, and executed ~972 programs;
  its crash-detection then fired on a real kernel WARN. So manager→N90 execution + `dmesg -w` log capture + crash
  recognition all work.
- **NOT proven by this run: coverage-guided fuzzing.** The manager's periodic stats stayed `corpus=0 coverage=0`
  the whole run — the KCOV *feature* is present, but this run did **not** demonstrate KCOV producing signal that
  grows a corpus. The real coverage evidence remains the **separate manual `syz-execprog` run** (§4: 62372 on
  `pkvm.c` + share path). Do not conflate the two. A coverage-guided campaign is claimable only once the manager's
  own `corpus`/`coverage` go non-zero.
- **SSH post-quantum warning was a real blocker — fixed.** The OpenSSH client prints `WARNING: connection is not
  using a post-quantum key exchange algorithm` to stderr; syzkaller's `ssh … dmesg -w` console reader captured it
  and **misread it as a kernel crash**, looping the rpc. Fixed generally in `vm/vmimpl/util.go:sshArgs` — add
  `-o LogLevel=ERROR` when not in debug (silences client banners, not remote kernel output). After the fix:
  **0 post-quantum lines**. (Caveat: the fix is gated on non-debug, so `syz-manager -debug` bypasses it — see §6.)
- **The WARN it surfaced is a *generic arm64-KVM* issue upstream already fixed — not a pKVM bug.**
  `WARN_ON(kvm_vgic_inject_irq(...))` at `arch_timer.c:461` trips via `kvm_arch_vcpu_ioctl_vcpu_init →
  kvm_vcpu_set_target → kvm_reset_vcpu → kvm_timer_vcpu_reset → kvm_timer_update_irq`. Root cause: this tree still
  carries the fragile global `userspace_irqchip_in_use` (`arch_timer.c:208`), so `userspace_irqchip()` misjudges a
  VM that has neither an in-kernel vGIC nor a proper userspace irqchip, and the timer inject fails. Upstream
  **removed** this global: `38d7aacca092` (stable backport `dd2f9861f275`), *"KVM: arm64: Get rid of
  userspace_irqchip_in_use"*. **Crucially this trigger never enters the pKVM path:** the reproducer's
  `KVM_ARM_VCPU_INIT` requests PMU_V3 and then `KVM_RUN` fails the PMU check (`kvm_arm_pmu_v3_enable`, `arm.c:751`)
  which returns **before** `pkvm_create_hyp_vm` (`arm.c:757`). So classify it as *"the pKVM lifecycle entry
  re-discovered a general arm64-KVM fix this vendor kernel is missing,"* not a new/pKVM vulnerability. Board stays
  up (non-fatal). Hit 9×. The saved artifact is the **raw concurrent trigger log, not a minimized reproducer**
  (`reproducing=0`). Evidence: `evidence/2026-07-21-BUG1-kvm_timer_update_irq-{report,repro}.*`.
- **Why the run stalled:** that WARN fires on many `VCPU_INIT` variants (our hand test `target=GENERIC_V8,
  features=0` did *not* trip it — the fuzzer found variants that do), so with `target_reboot:false` the manager
  re-detected it and `exec total` froze at ~972.

**Agreed next plan (do NOT just add `ignores` — that only hides the report while the kernel keeps WARNing, log-
spamming, and dropping throughput):**
1. **Backport the upstream fix** `38d7aacca092` / `dd2f9861f275` ("Get rid of `userspace_irqchip_in_use`") into
   `/home/jose/common`, rebuild + redeploy to N90.
2. **Verify with the minimal flow** that both a *normal* VM and a *protected* VM no longer WARN.
3. **Save a minimized reproducer** for BUG1 first (so a real report is possible), then it can be reported as
   "vendor kernel missing upstream arm64-KVM fix."
4. **Phase 4a:** drop raw `KVM_RUN` from `enable_syscalls` (it busy-loops — §4.4) and fuzz only
   `create / VCPU_INIT / close`; only once the manager's own `corpus`/`coverage` go **non-zero** is it a
   coverage-guided campaign.
5. Persistent-crash recovery (efi-pstore + syzkaller patch; N90 is ACPI/no-DT) and prompt-`KVM_RUN`-exit remain
   before any *unattended* run.

### 6.2 Controlled first `KVM_RUN` (immediate_exit) — DONE + verified on N90 (2026-07-21)

Addresses the "prompt-`KVM_RUN`-exit" item and gives the campaign a stable way to *cross the first-run host→hyp
boundary* without the busy loop. New pseudo-syscall **`syz_kvm_vcpu_run_immediate$arm64(fd fd_kvmcpu)`**
(`executor/common_kvm_arm64.h`): map the shared `kvm_run`, set `immediate_exit=1`, then `KVM_RUN`. Kernel basis
(verified): `pkvm_create_hyp_vm` runs in `kvm_arch_vcpu_run_pid_change` (arm.c:717,757) which generic
`kvm_main.c` calls **before** `kvm_arch_vcpu_ioctl_run`; the `immediate_exit` check is at the top of the latter
(arm.c:1043 → `-EINTR`). So the first run **still builds the hyp VM** and yields its KCOV, then returns before
entering the guest.

Verified on N90 (test `arm64-syz_kvm_vcpu_run_immediate`, PMU_V3-free `VCPU_INIT`; evidence
`evidence/2026-07-21-controlled-kvm_run-immediate-exit.md`) — **all 5 acceptance criteria pass**:
1. run returns **`-EINTR`** fast (whole program 5 s, no hang);
2. kretprobe **`pkvm_create_hyp_vm ret=0x0`** (boundary crossed);
3. **no arch_timer WARN** (BUG1 avoided by excluding PMU_V3);
4. explicit **`close` → `pkvm_destroy_hyp_vm`** — and unlike the raw `-threaded=0` case, **all 7 calls execute**
   (closes cov 537 / 42166), so teardown goes through the explicit close, not worker-kill fd-cleanup;
5. the run yields **bounded real KCOV** — call #4 = 5457 (2276 unique PCs, *not* the 524287 busy-loop cap),
   symbolizing to `pkvm_create_hyp_vm → __pkvm_create_hyp_vm → __pkvm_create_hyp_vcpu → __pkvm_vcpu_hyp_created`
   plus `pkvm.c`/`mmu.c` host→hyp share.

⇒ This is the run primitive for the next campaign round (raw `ioctl$KVM_RUN` de-emphasised/removed from
`enable_syscalls`). It exercises the *real* first-run boundary — not just create-time prep, and not a busy loop.
(Deeper EL2/Rust-hyp coverage is still Stage 2.)

### 6.3 Type-safe constrained path + successful coverage-guided campaign — DONE 2026-07-21

Config-only exclusion of BUG1 is fragile (the fuzzer can still synth a PMU_V3 `VCPU_INIT` from the generic
call). So the safe path is now encoded in the **types** (`sys/linux/dev_kvm_arm64.txt`):
- `resource fd_kvmcpu_protected[fd_kvmcpu]` — a vCPU of a protected VM.
- `ioctl$KVM_CREATE_VCPU_protected(fd fd_kvmvm_protected, …) fd_kvmcpu_protected` — the only producer of that type.
- `kvm_vcpu_init_safe { target const[GENERIC_V8]; feature const[0]; pad … }` + `ioctl$KVM_ARM_VCPU_INIT_safe` —
  **feature is `const[0]`, not a fuzzable flags set**, so it can never request `KVM_ARM_VCPU_PMU_V3`.
- `syz_kvm_vcpu_run_immediate$arm64(fd fd_kvmcpu_protected)` — narrowed from `fd_kvmcpu` to the protected type.

`enable_syscalls` (`workdir/pkvm-arm64.cfg`) is the safe path **only**: `openat$kvm`,
`syz_kvm_setup_protected_vm$arm64`, `ioctl$KVM_CREATE_VM_PROTECTED`, `ioctl$KVM_CREATE_VCPU_protected`,
`ioctl$KVM_ARM_VCPU_INIT_safe`, `syz_kvm_vcpu_run_immediate$arm64`, `close$kvmvm_protected`, `close` — the generic
`KVM_CREATE_VCPU` / `KVM_ARM_VCPU_INIT` / raw `KVM_RUN` are **not** enabled. Constrained path re-verified on N90
(same 5 criteria as §6.2; evidence `evidence/2026-07-21-controlled-run-RAW-outputs.txt`): the new typed calls
execute (`KVM_CREATE_VCPU_protected=0x5`, `KVM_ARM_VCPU_INIT_safe=0x0`), run returns `-EINTR`, `pkvm_create_hyp_vm
ret=0x0`, dmesg clean.

**The campaign then hit the coverage-guided milestone** (supervised, ~2 h, 2026-07-21):
```
start   corpus=0  coverage=0     exec=142
+12min  corpus=76 coverage=6076  exec≈8.7k
+1.5h   corpus=88 coverage=6122→6382 (a real jump — new path found after long soak)
+2h07m  corpus=93 coverage=6407  exec=302,671 (39/sec)   crashes=0
```
So the manager's **own** corpus and coverage went non-zero and **grew** (round-1 was stuck at `0/0`), sustained
~39 exec/s over **300k+ programs**, and N90 logged **0** new `kvm_timer_update_irq` warnings — the type-level
PMU_V3 exclusion held across the entire run. Coverage plateaued early (~6076) then jumped to ~6407 at ~1.5 h,
so the constrained host-side surface has a long exploration tail but is essentially saturated; going higher needs
a broader safe surface (multi-vCPU, vGIC, memory regions) or Stage 2 EL2 KCOV. **This is the Stage 1/1.5 goal —
sustained, coverage-guided, BUG1-free fuzzing of the pKVM host→hyp lifecycle on real hardware — met.**

**Operational lesson (isolated backend on a live board):** clear **both** `dmesg -C` **and** `workdir/crashes/`
between runs. The isolated backend reuses the running DUT, so (a) `ssh dmesg -w` replays the ring buffer on
connect — round-1's warnings looked like fresh crashes and crash-looped a first round-2 attempt until `dmesg -C`;
and (b) a crash dir from that aborted attempt persisted into the clean run and tripped a false watcher alert. A
fresh-booted VM image gets both for free; a physical DUT does not.

### 6.4 Campaign progressive results (~2 h 17 m soak; snapshot) + the EL1/EL2 coverage boundary

Snapshot from `workdir/corpus.db` (93 programs) and the manager's `/rawcover` (6,406 PCs), symbolized vs `vmlinux`
(dashboard: `pkvm-campaign-dashboard.html`).

**Corpus (93 programs) — call frequency:** `openat$kvm` 129, `ioctl$KVM_CREATE_VM_PROTECTED` 100,
`syz_kvm_setup_protected_vm` 63, `ioctl$KVM_CREATE_VCPU_protected` 63, **`syz_kvm_vcpu_run_immediate` 34** (the
controlled run *did* enter the corpus — the colleague's acceptance criterion), `close$kvmvm_protected` 33, `close`
22, `ioctl$KVM_ARM_VCPU_INIT_safe` 16. Sizes: mostly 1–6-call minimal repros, a few up to 30; one keeps a *double*
controlled run.

**Coverage rate:** 0 → ~6,090 in the first ~10 min (bootstrap), a long plateau (6,090→6,122) as the constrained
8-call surface saturates, a **+260 discovery** to 6,382 at ~1 h 50 m, then flat at 6,407. Throughput held ~39
exec/s the whole time — flat coverage ≠ stalled.

**Coverage attribution (honest boundary — answers "how do we have `pkvm_destroy_hyp_vm` coverage with no EL2
KCOV?").** `coverage=6407` is the manager's accumulated **host-side KCOV signal count**, *not* lines of EL2 code:
- Most of it is the executor's own sandbox setup (`lib`/`mm`/`fs` ≈ 3,000+ PCs).
- The **target** slice is ~597 PCs in `arch/arm64/kvm`: `arch_timer.c` 113, `arm.c` 84, `mmu.c` 72, `sys_regs.c`
  60, **`pkvm.c` 35**, plus `debug.c`/`reset.c`/`hypercalls.c`/`vgic.c`.
- The covered `pkvm_create_hyp_vm`, `pkvm_destroy_hyp_vm`, `pkvm_init_host_vm`, `__pkvm_create_hyp_vm`,
  `__pkvm_create_hyp_vcpu`, `__pkvm_vcpu_hyp_created`, `__pkvm_destroy_hyp_vm` are **all HOST (EL1) functions
  defined in `arch/arm64/kvm/pkvm.c`** — plain symbols in `vmlinux` (e.g. `T pkvm_create_hyp_vm`), *not*
  `__kvm_nvhe_`-prefixed. They are the host's *driver for* hyp VMs (marshal args, issue the hypercall); the "hyp"
  in the name is what they *manage*, not where they *run*. KCOV instruments them normally
  (`CONFIG_KCOV_INSTRUMENT_ALL=y`).
- **Coverage stops at the EL1→EL2 boundary.** `__pkvm_create_hyp_vm` ends in
  `kvm_call_refill_hyp_nvhe(__pkvm_init_vm, …)` (pkvm.c:405); the EL2 handler is the *separate* symbol
  `__kvm_nvhe___pkvm_init_vm`, and the `__kvm_nvhe_` (EL2) image carries **0 `__sanitizer_cov_*` symbols** (vs 5 in
  the host). So everything past the hypercall — the real EL2 / Rust-hyp execution — is **dark**. Instrumenting it
  is **Stage 2** (in-hyp EL2 KCOV, design spec §6). What §6.1–6.4 prove is precisely Strategy 1: the **host→hyp
  lifecycle** driven and observed by KCOV up to the EL2 boundary.

### 6.5 Host-breadth slices (controlled EL1 pKVM state-machine expansion) — 2026-07-21

After the baseline, host coverage is grown in small, verified, archived slices — each a fixed regression test →
manual `syz-execprog` verification → short allowlisted campaign. All stay EL1 host-side (EL2 stays Stage 2).

- **Slice 1 — safe dual-vCPU lifecycle** (`test/arm64-syz_kvm_dual_vcpu`; evidence `evidence/2026-07-21-slice1-dual-vcpu.md`).
  2 vCPUs both `INIT_safe`, run one, close both then VM. The first run's `pkvm_create_hyp_vm` walks all vCPUs
  (`kvm_for_each_vcpu → __pkvm_create_hyp_vcpu`, pkvm.c:415). **Functional pass** (ret=0, no BUG1/hang, explicit
  closes run). **Coverage: +125 PCs but only ~4 in `arch/arm64/kvm`** — KCOV dedups the identical per-vCPU body,
  so multiplicity is a *regression test*, not a coverage win. No new syzlang (model already permits ≥2 vCPUs).
- **Slice 2 — controlled memslot rejects, COMPOSITE model** (`test/arm64-syz_kvm_memslot_{delete,move,dirty,readonly}_reject`;
  evidence `evidence/2026-07-21-slice2-memslot.md`). Hits the **two distinct** pKVM rejects in
  `kvm_arch_prepare_memory_region` (mmu.c:2492-2502): dirty/readonly → `-EPERM` with only `pkvm.enabled` (no run);
  DELETE/MOVE → `-EPERM` only after the first run sets `pkvm.handle`. **Redesigned from the first fragment cut**:
  fragments + resource subtypes (`fd_kvmvm_pslot`) could **not** enforce the "already ran" precondition, because
  syzkaller resource compatibility is permissive (a supertype resource satisfies a subtype arg,
  `prog/resources.go:109`) — a corpus program `setup_protected_vm(r1); memslot_move(r1)` proved it. Replaced by
  three **self-contained composite pseudo-calls** that each take only the `/dev/kvm` fd and build the whole state
  in C: `syz_kvm_memslot_reject_delete$arm64` / `_move$arm64` (create bit-31 pVM → register slot → vCPU →
  `INIT_safe` → controlled `immediate_exit` run, **required to return -EINTR** → reject op), and
  `syz_kvm_memslot_reject_flags$arm64(fd, flags[1:3])` (create pVM → flagged register, no run). Building **only
  bit-31 protected VMs** internally keeps this off BUG2 (dirty-log on a *normal* VM, §6.6). **All four verified on
  N90** (each reject `-1/EPERM`; cover ~98–118k; **0 WARN/BUG** in dmesg — no BUG1, no BUG2). Fresh-workdir
  campaign (`workdir-composite/`, no reused corpus.db; `syscalls: 10/8060`): all three composites entered the
  corpus under fuzzing (`reject_delete`×46, `reject_flags`×26, `reject_move`×23 program occurrences), coverage
  0→~6.2k, 0 crashes. *(The earlier fragment-era `+468 PCs` figure came from a contaminated-corpus campaign and is
  superseded.)*
- **Deliberately NOT in these slices:** real guest-memory execution / page-fault / donation — those bring pages
  into EL2 and need a stably-exiting protected guest (SyzOS/pvmfw subproject, §7), not `run_immediate`.

### 6.6 BUG2 — `kvm_tlb_flush_vmid_range` WARN (dirty-log on a normal VM) — minimized 2026-07-21

A second WARN (`arch/arm64/kvm/hyp/pgtable.c:639 kvm_tlb_flush_vmid_range`) appeared in dmesg during the
fragment-era slice-2 campaign. Root-caused by minimization (`evidence/2026-07-21-BUG2-kvm_tlb_flush_vmid_range-MINIMIZED.md`,
minimizer `-BUG2-minimizer.c`, three single-path cases on N90):
```
A) pVM MOVE pre-run        → 0 warnings
B) pVM MOVE post-run       → -EPERM, 0 warnings
C) NORMAL VM + LOG_DIRTY_PAGES → 4 warnings   ← the trigger
```
So BUG2 is **enabling `KVM_MEM_LOG_DIRTY_PAGES` on a *normal* (non-protected) VM on a pKVM host** — the
dirty-log write-protect walk (`kvm_mmu_wp_memory_region`, mmu.c:1327 → `kvm_tlb_flush_vmid_range`), a **generic
arm64-KVM-on-pKVM** issue, **not** a pKVM/EL2/Rust-hyp bug and unrelated to the memslot rejects. (An earlier
hypothesis — pre-run MOVE on a protected VM — was **refuted** by case A; the colleague's dirty-log-on-normal-VM
diagnosis was correct.) Not fixed, per the standing "don't fix generic KVM bugs this pass" call. The composite
slice-2 model **cannot reach it**: it only ever creates bit-31 protected VMs, and the campaign allowlist excludes
any normal-VM create + dirty-log path. Surfaced originally only via **old-corpus contamination** (a stale
`type=0x5` non-protected VM program that the current descriptions can no longer generate) — the fresh-workdir
campaign has none.

---

## 7. Deferred (Phase 5 — separate subtasks)

- **Guest HVC + `kvm_smc_id`** extension (guest-side): extract pKVM vendor-hyp func IDs in a throwaway git worktree (`make extract` `mrproper`s its `SOURCEDIR`), extend `kvm_smc_id`. The existing `@hvc` already carries x0–x5, so no new SyzOS command.
- **SyzOS-in-protected-VM** transport (needs a fuzzer-drivable guest under memory isolation; `setup_vm` memslot flags are rejected, and the post-`INIT` PC write may not take because the hyp captures the reset PC at RUN).
- **pvmfw + firmware handoff** (`request_firmware_direct("pvmfw.bin")`, `arm.c:2665`; adds `SET_FW_IPA` + fw-covering region variant).
- **Stage 2 — EL2 in-hyp KCOV** (design spec §6): SanCov the Rust crate, hyp-local `__sanitizer_cov_trace_pc` + shared ring, `kcov_add_pcs` at verified host→hyp boundaries, two-object symbolization (`__kvm_nvhe_` prefix). Plus the config-qualified static boundary table.

---

## 8. File inventory & commit status

**syzkaller (`/home/jose/syzkaller`) — pKVM modeling + the controlled-run/constrained path + composite memslot rejects + one general fix:**
- `sys/linux/dev_kvm_arm64.txt` — protected-VM syzlang: `fd_kvmvm_protected`, `syz_kvm_setup_protected_vm$arm64`, `ioctl$KVM_CREATE_VM_PROTECTED`, `close$kvmvm_protected`; **+ constrained path** `fd_kvmcpu_protected`, `ioctl$KVM_CREATE_VCPU_protected`, `kvm_vcpu_init_safe`/`ioctl$KVM_ARM_VCPU_INIT_safe`, `syz_kvm_vcpu_run_immediate$arm64` (§6.3); **+ composite memslot rejects** `syz_kvm_memslot_reject_{delete,move}$arm64(fd_kvm)`, `syz_kvm_memslot_reject_flags$arm64(fd_kvm, flags[1:3])` (§6.5, replaces the fragment `register_memslot`/`memslot_run`/`_delete`/`_move` + `fd_kvmvm_pslot` subtypes).
- `executor/common_kvm_arm64.h` — the `syz_kvm_setup_protected_vm` helper **+ `syz_kvm_vcpu_run_immediate`** (immediate_exit controlled run, §6.2) **+ the composite helpers** `pkvm_build_slotted_vm` / `syz_kvm_memslot_reject_{delete,move,flags}` (§6.5).
- `pkg/vminfo/linux_syscalls.go` — support-map entries (`syz_kvm_setup_protected_vm`, `syz_kvm_vcpu_run_immediate`, and `syz_kvm_memslot_reject_{delete,move,flags}`, in the map and the arm64 case).
- `sys/linux/test/arm64-syz_kvm_memslot_{delete,move,dirty,readonly}_reject` — single-call composite regression tests (§6.5).
- `vm/vmimpl/util.go` — **general SSH fix** (`-o LogLevel=ERROR` when not `-debug`) so the OpenSSH post-quantum banner does not leak into the `dmesg -w` console and get misread as a crash (§6.1). Not pKVM-specific.
- `sys/linux/test/arm64-syz_kvm_setup_protected_vm` — canonical 7-call lifecycle test (with `ioctl$KVM_RUN`).
- `sys/linux/test/arm64-syz_kvm_vcpu_run_immediate` — controlled-run test on the constrained path (§6.2/6.3).
- `workdir/pkvm-arm64.cfg` and `workdir-composite/pkvm-arm64-composite.cfg` — manager configs (both gitignored, local-only): `ssh_user:root`, `pstore:false`, `target_reboot:false`; the composite config's `enable_syscalls` is the 10 lifecycle + composite calls, on a **fresh workdir** (§6.5).
- `docs/superpowers/specs/2026-07-20-pkvm-syzkaller-support-design.md` — design spec; **this file**; and `docs/superpowers/specs/evidence/2026-07-21-*` (phaseB, BUG1 report+repro, controlled-run md + RAW outputs, pkvm-call1 coverage).
- **Build artifacts, gitignored:** `executor/syscalls.h`, `executor/defs.h`, `sys/linux/gen/*` (regenerated by `make descriptions`).
- **Reverted / not part of this change:** `pkg/ifuzz/{arm64,riscv64,x86}/generated/insns.go` churn (git-checkout reverted). Preserved aside: `workdir/crashes-round1-rawKVM_RUN/` (BUG1), `workdir/crashes-round2-attempt1-staledmesg/` (false crash-loop).

**kernel (`/home/jose/common`) — the pKVM-related edits are exactly these two (the BUG1 fix was deliberately NOT applied):**
- `arch/arm64/configs/gwd3000_lenovox1d3000m_defconfig` — fuzzing deltas (backup `.pre-syzkaller.bak`, untracked).
- `arch/arm64/kvm/hyp/nvhe/rust/build_rust.sh` — toolchain pin.
- The working tree may carry other, unrelated changes from time to time (not part of this record); as of writing, `git status` on `common` shows only the two above plus the untracked backup.

**N90:** `/boot/{vmlinuz,initrd.img,config,System.map}-6.6.30-pkvm-fuzz`, `/usr/lib/modules/6.6.30-pkvm-fuzz`, GRUB custom entry + `GRUB_CMDLINE_LINUX` param, `kylin` in `kvm` group, **root SSH enabled** (`/root/.ssh/authorized_keys` = manager pubkey; revoke by removing it). Backups: `/etc/default/grub.pre-pkvm.bak`.

**Commit status (2026-07-21).** The **curated syzkaller code** is committed on a dedicated branch **`pkvm-lifecycle-fuzzing`** (branched from `master`, so it carries none of the learning notes), pushed to `origin` (TomGoh fork):
- `8582e88ba` — Stage 1/1.5 protected-VM host→hyp lifecycle fuzzing (setup helper, constrained path, immediate_exit run).
- `03e6d12ec` — host-breadth slices: dual-vCPU regression + memslot state machine (**fragment-era**).
- *(next)* slice-2 **composite redesign** — supersedes the fragment memslot calls with the three composite pseudo-calls above; committed on `pkvm-lifecycle-fuzzing` on top of `03e6d12ec`.

Branch split: the code + tests + this `docs/superpowers/specs/` tree (design spec, this record, `evidence/*`, `campaign-runbook.sh`) live on **both** branches; the narrative **`notes/*`** (the Chinese learning notes) stay **only** on the working branch `claude/syzkaller-source-learning-vvisp3` — that is the "learning notes left behind." The **kernel repo** (`/home/jose/common`) changes are **kept local, uncommitted** by intent (fuzzing config + toolchain pin + the `pr_debug` debugfs-warning quiet; the BUG1 fix deliberately not applied). Both manager configs and the `workdir*/` trees are gitignored (local-only).
