# pKVM EL2-coverage fuzzing — operator runbook (handoff)

Run syzkaller against the Rust pKVM hypervisor on N90 (and a batch of Kylin V10 /
gwd3000 boards), capturing **EL2 (hyp) coverage** symbolized to `.rs:line`. This is the
successor's operating guide. Read the "Day-1 must-do" box first.

---

## ⚠️ Day-1 MUST-DO before any weeks-long / unattended run

These are documented, NOT yet implemented (deliberately deferred — see rationale at end).
The first is a **SILENT failure** — you will not get an error, you will get zero coverage.

1. **AUTOMATE RING ARMING (silent failure — do this first).**
   EL2 coverage is only captured while the per-CPU ring is armed:
   `taskset -c <owner_cpu> sh -c 'echo 1 > /sys/kernel/debug/kvm/pkvm_cov/enable'`.
   This is a manual debugfs write that **resets to disarmed on every reboot** and also
   resets `nr_pages` to the default (4). A weeks-long campaign reboots the target on
   crashes; after the first reboot the ring is disarmed and **EL2 coverage silently goes
   to zero — no error, `/cover` just stops growing on the `.rs` side.** Fix: a target-side
   systemd unit (or a manager `vm` hook) that, on boot, sets `nr_pages=32` and arms the
   ring on the owner CPU. Until this exists, only run **supervised** (arm by hand, watch).
2. **CRASH RECOVERY.** `efi-pstore` is dead on N90 (firmware can't `SetVariable`), so a
   panic loses its log and the manager can't auto-recover. Add **netconsole or serial**
   capture + manager auto-reboot-and-continue, or the first real crash ends the run.
3. **syz-hub** for the batch: without a shared corpus, N machines redundantly rediscover
   the same paths instead of collectively covering the codebase.
4. **B (guest→hyp) design** — the biggest remaining coverage surface; see gap note below.

---

## What's built (state at handoff)

- **EL2 coverage bridge (Stage 2):** Rust EL2 SanCov → per-CPU host-shared ring → EL1 drain
  wrapped around chosen host→hyp HVC boundaries → `kcov_add_pcs` → task KCOV → syzkaller →
  `addr2line` to `rust/src/*.rs:line`. Kernel side lives in `common-stage2mvp` **uncommitted**
  (project constraint); the full snapshot is
  `notes/pkvm/evidence/kernel-instrumentation-2026-07-27/stage2-el2-kcov.patch`.
- **Instrumented boundaries: 2 of ~66 host→hyp HVCs** — `#23 __pkvm_host_map_guest` (mmu.c)
  and **VM/vCPU create** `#92/#93 __pkvm_init_vm/__pkvm_init_vcpu` (pkvm.c). EL2 source-line
  coverage: **~133 lines** across ~20 files (was 79 with #23 alone; create added +54).
- **Fuzzer input:** the generatable composite `syz_kvm_run_fw_fault_gen$arm64(fd, ipa_size,
  fw_ipa, hp_mode)` (safe args pinned; `hp_mode` mutatable → 2 MiB block donation). Full
  protected-VM lifecycle in C (executor/common_kvm_arm64.h).
- **Known-WARN suppression:** the pKVM VMID-rollover WARN (vmid.c) is ignored in the reporter
  so long runs don't stall on it (it's a real, non-fatal kernel finding handed to the kernel
  owner). Same for the OpenSSH post-quantum banner.

## Build + deploy a machine
Use the `kylin-v10-kernel-deploy` skill (encodes the `/lib`-symlink brick hazard — never
extract a modules tarball into `/`). For a **built-in-only** change (pkvm_cov is `=y`), the
kernel `Image` is the only artifact that changes, so an **Image-only** deploy is enough and
safe:
```
# in common-stage2mvp, .config already has CONFIG_PKVM_EL2_COV=y (KVER stays 6.6.30+):
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- -j"$(nproc)" Image
# backup + copy Image to the board's /boot/vmlinuz-6.6.30+ (hash-verify; confirm /lib intact)
```
Boot the non-default GRUB entry `6.6.30+ pKVM EL2-cov smoke` (`grub-reboot "..."` one-shot;
default stays the known-good `6.6.30-pkvm-fuzz`, so a bad build never bricks the box).

## Run a (supervised) campaign
```
# arm the ring on the owner CPU (see Day-1 #1 — manual until automated):
ssh <board> 'echo 0 > /sys/kernel/debug/kvm/pkvm_cov/enable; echo 32 > .../nr_pages;
             taskset -c 0 sh -c "echo 1 > .../pkvm_cov/enable"'
# launch the manager (config: procs=1, pkvm_serial, pkvm_owner_cpu=0, remote_cover=false,
#   type=isolated, target=<board>):
bin/syz-manager -config=<board>.cfg
```
Reference config: `/home/jose/syzkaller/workdir-a2confirm/pkvm-a2confirm.cfg`.

## Read coverage — the built-in dashboard
syzkaller's manager serves `http://127.0.0.1:<http-port>/cover` (file tree, per-file %,
line-level highlight, `/coverfile`, `/rawcover`). Open it and confirm the `rust/src/*.rs`
EL2 files show coverage. Ad-hoc: `curl .../rawcover | aarch64-linux-gnu-addr2line -e
<matching vmlinux>` (KASLR off, same build). Ring stats: `/sys/kernel/debug/kvm/pkvm_cov/stats`
(`LOST_IN_RING` must be 0; `drains`, `el2_hits`, `skip_not_owner`).

## Known issues / limits
- **Single-CPU serialization:** the ring is per-CPU single-owner → `procs=1`, executor pinned
  to the owner CPU (~13 exec/s/machine). Scale **horizontally** (batch + syz-hub). Per-online-CPU
  rings would lift this but are a kernel change (successor optimization).
- **VMID-rollover WARN** every ~13k VM create/destroys: suppressed in the reporter; the EL2-side
  fix is the kernel owner's.
- **Create-path coverage has run-to-run variance** (allocator state); the deflake-stable subset
  is saved, the union accumulates over the campaign. `LOST_IN_RING=0` confirms it's real, not
  truncation.

## The "entire codebase" gap
Coverage = reachability. **host→hyp** surface (create/teardown/share/map/relax-perms/PSCI/IOMMU
… ~66 HVCs) is reached by **a1** (instrument more boundaries — each = a new EL2 region; the
proven, mechanical lever). **guest→hyp** surface (a compromised guest's HVCs/traps) is reached
only by **B** (an in-guest fuzzer agent + a transport that survives protected-VM memory
isolation — a multi-week build, design-only for now). At handoff: 2/66 host→hyp boundaries
instrumented; continue a1 incrementally; B is the separate frontier.

## The boundary-instrumentation method (repeatable, per new boundary)
1. **Audit** the host call site: which locks held, preempt on/off, stable owner CPU, sleepable?,
   same task as the KCOV collector (KVM_RUN/pseudo-syscall thread)? — board-free code read.
2. **Wrap** only the atomic HVC with `#ifdef CONFIG_PKVM_EL2_COV { cov; armed=pkvm_cov_begin(&cov);
   <the kvm_call_...hvc>; if(armed) pkvm_cov_end(&cov); }` — never a sleeping region, never a
   global wrap.
3. Incremental `make Image` → Image-only deploy → reboot.
4. **Verify:** new EL2 `.rs` lines appear + attributable, **`LOST_IN_RING=0`**, `skip_not_owner=0`,
   no new WARN/hang. (See scripts/pkvm/a2/ for the capture + set-algebra harness.)

## Methodology rules (hold these)
1. **The instrument is the limiter, not the DUT** (proven 3×: drains metric, tmpfs-chroot toggle,
   head-biased ring). On "no new coverage," first check `LOST_IN_RING=0` before concluding the DUT
   has nothing.
2. **A toggle must be a syscall arg** — the executor chroots into a tmpfs newroot, so env vars /
   `/root` files never reach the fault code.
3. **Code reading is a hypothesis; hardware gives the answer** — retract mechanistic predictions
   that the run refutes.
4. **Per-boundary audit, never a global `kvm_call_hyp_nvhe` wrap** — each boundary has its own
   lock/CPU/RCU/sleepability context.

## Committed work (branch pkvm-lifecycle-fuzzing; kernel stays uncommitted in common-stage2mvp)
`94a29c2a6` PQ-banner · `942bf6622` gen-args pin · `aca4da48c` a2 hp_mode · `81d7d4cc2` MAX_PAGES
16→32 · `f8c015bfe` VMID-WARN suppress · `1304a5854` a1 create boundary.
