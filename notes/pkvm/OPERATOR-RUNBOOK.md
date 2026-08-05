# pKVM EL2-coverage fuzzing — operator runbook (handoff)

Run syzkaller against the Rust pKVM hypervisor on N90 (and a batch of Kylin V10 /
gwd3000 boards), capturing **EL2 (hyp) coverage** symbolized to `.rs:line`. This is the
successor's operating guide. Read the "Day-1 must-do" box first.

---

## ⚠️ Day-1 MUST-DO before any weeks-long / unattended run

These are documented, NOT yet implemented (deliberately deferred — see rationale at end).
The first is a **SILENT failure** — you will not get an error, you will get zero coverage.

1. **RING ARMING — AUTOMATED (2026-07-28; was the #1 silent failure).**
   EL2 coverage is only captured while the per-CPU ring is armed, and it resets to
   disarmed (`nr_pages`→4) on every reboot — so after the first crash-reboot, EL2 coverage
   would silently go to zero (no error, `/cover` just stops growing on the `.rs` side).
   Fixed by a target-side systemd unit that re-arms on boot:
   `scripts/pkvm/target/pkvm-cov-arm.{sh,service}`, installed on N90 as
   `/usr/local/sbin/pkvm-cov-arm.sh` + `/etc/systemd/system/pkvm-cov-arm.service` and
   `systemctl enable`d. It sets `nr_pages=32` and arms on the owner CPU (default 0; override
   via the unit's `PKVM_COV_OWNER_CPU` / `PKVM_COV_PAGES`). It is **idempotent + campaign-safe**
   (skips if already armed; never disarms a live ring, so it's safe to run any time).
   Install on a new board: `install -m755 pkvm-cov-arm.sh /usr/local/sbin/ && install -m644
   pkvm-cov-arm.service /etc/systemd/system/ && systemctl daemon-reload && systemctl enable
   pkvm-cov-arm.service`. **Confirm on the first real reboot** that `owner_cpu`/`ring_pages`
   come up armed (the idempotent-skip path is hardware-tested; the fresh-boot arm path is
   logic-verified, to be confirmed on the next reboot).
2. **CRASH RECOVERY.** `efi-pstore` is dead on N90 (firmware can't `SetVariable`), so a
   panic loses its log and the manager can't auto-recover. Add **netconsole or serial**
   capture + manager auto-reboot-and-continue, or the first real crash ends the run.
   **A hang, however, is recoverable remotely** (verified 2026-08-05 on N90 while the
   `pkvm_unmap_guest` deadlock had the box wedged): the kernel is alive, only tasks touching
   the poisoned `mm` block, so ssh still logs in and `dmesg` still streams. Recover with
   `echo 1 > /proc/sys/kernel/sysrq; echo b > /proc/sysrq-trigger` — the stock `sysrq` mask is
   `176`, which does **not** include the `0x40` reboot bit, so it must be widened first.
   `systemctl reboot` cannot work in this state (systemd's own `/proc` walk blocks). On both
   V11 boards GRUB menuentry **index 0** is the pKVM fuzzing kernel, so the box comes back on
   the right build unattended and `pkvm-cov-arm.service` re-arms the ring. Collect evidence
   BEFORE rebooting: `/proc/<pid>/{stat,wchan,stack}` (never `cmdline` or `maps` — those take
   `mmap_lock` and will wedge your shell too) and `echo w > /proc/sysrq-trigger`.
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
- **Instrumented boundaries: ALL host→hyp HVCs, via macro auto-injection** (2026-07-28) —
  `KVM_PKVM_COV_HVC` is injected once into `kvm_call_hyp_nvhe` (kvm_host.h) and
  `kvm_call_refill_hyp_nvhe` (kvm_pkvm.h), so every boundary the fuzzer drives is covered with no
  per-site code; the old per-site wraps (#23 map, VM/vCPU create/teardown) are reverted. EL2
  source-line coverage from ONE `baseline.prog` input: **298 `.rs` lines** (was 133 with the create
  wrap, 79 with #23 alone). See `a1-macro-cov-injection.md` + `evidence/a1-macro-autoinject-2026-07-28/`.
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
Image-only swap on the target: hash-verify then `mv` into `/boot/vmlinuz-6.6.30+` (keep a
`.bak`), confirm `/lib -> usr/lib` intact. NOTE: on N90 the grubenv `next_entry` does NOT clear
after boot, so the box **keeps** booting the cov entry — fine for a campaign, but to revert to
known-good you must select it manually / clear grubenv.

**Rebuilding syzkaller (this checkout is a git worktree):** `syz-manager` runs on the HOST and is
where `pkg/report` lives, so a reporter change needs a manager rebuild (not a target redeploy):
`cd syzkaller-pkvm && GGFLAGS=-buildvcs=false make manager` (the worktree needs `-buildvcs=false`,
passed via `GGFLAGS` so it appends to `GOFLAGS` without clobbering the `-ldflags`). The N90-side
`syz-executor` is unaffected by reporter changes.

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

### Multi-board / overnight runs — one manager per board

`scripts/pkvm/tools/overnight-run.sh {status|start|stop} [n90|d3000|all]` wraps the below.
It refuses to start a board whose EL2 ring is not armed, and stops with `SIGINT` (not `KILL`)
so `corpus.db` is flushed.

| board | ip | config | `kernel_obj` | http |
|---|---|---|---|---|
| N90 | `10.42.27.17` | `workdir-macrocov-thru/maxcov.cfg` | `/home/jose/common-stage2mvp` | 56750 |
| D3000 | `10.42.27.18` | `workdir-d3000-overnight/d3000.cfg` | `/home/jose/ksrc-pkvmfix` | 56751 |

**Do not fold two boards into one manager's `vm.targets` while their kernels differ.**
syz-manager holds a single `kernel_obj` and symbolizes every PC it receives against that one
`vmlinux`. N90 runs `6.6.30+` (instrumented, *without* the unmap deadlock fix); D3000 runs
`6.6.30-pkvmfix` (instrumented *and* fixed). Sharing a manager would symbolize half the coverage
against the wrong binary — silently, with no error anywhere. Merge the campaigns only once both
boards boot the same `vmlinux`.

**Verify `kernel_obj` by build string, never by mtime.** A restore from `.n90-build-backup/`
rewrites mtimes without changing content, so a "newer" local `vmlinux` may be exactly the running
kernel — and a stale one may look current. Compare what is actually embedded:

```
strings -a <kernel_obj>/vmlinux | grep -m2 '^Linux version'   # e.g. 6.6.30+ #47 Tue Jul 28 15:36:00
ssh root@<board> 'uname -a'                                    # must match release, build #, and date
```

Both configs above were verified this way on 2026-07-29: N90 ↔ `#47 Jul 28 15:36:00`,
D3000 ↔ `#51 Jul 29 16:10:53`.

**A wedged target looks identical to a healthy idle one** in the manager's stats line. The tell is
`exec total` frozen while the timestamp keeps advancing — coverage and corpus freeze too, so
neither distinguishes the cases. If `exec total` has not moved for several minutes, get the serial
console before restarting; the manager log alone cannot tell a target deadlock from an SSH failure
(both end in `boot error: repair failed: SSH failed`).

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
  fix is the kernel owner's. **The suppression is a REPORTER change (`pkg/report/linux.go`
  `ctorLinux` ignore) compiled into `syz-manager` — it does NOT silence the kernel dmesg (the WARN
  still prints on the box; that's expected). It only stops syzkaller treating the WARN as a crash.
  CRITICAL: you must REBUILD `syz-manager` after any reporter change — a running binary built
  before the commit will still crash-detect the WARN, tear down the VM, and stall the campaign at
  the first rollover (~11-13k execs). Verify the fix is in the binary:
  `strings bin/syz-manager | grep 'arch/arm64/kvm/vmid'` (want ≥1).**
- **Create-path coverage has run-to-run variance** (allocator state); the deflake-stable subset
  is saved, the union accumulates over the campaign. `LOST_IN_RING=0` confirms it's real, not
  truncation.

## The "entire codebase" gap
Coverage = reachability. The **host→hyp** surface (create/teardown/share/map/relax-perms/PSCI/IOMMU
… the HVCs through `kvm_call_hyp_nvhe`/`kvm_call_refill_hyp_nvhe`) is now **fully instrumented in
one shot** by the macro auto-injection (a1, done 2026-07-28) — coverage there is bounded only by
which HVCs the fuzzer's inputs actually drive, not by how many sites are wrapped. The **guest→hyp**
surface (a compromised guest's HVCs/traps into EL2) is reached only by **B** — an in-guest fuzzer
agent + a transport that survives protected-VM memory isolation (a multi-week build, design-only
for now). At handoff: **host→hyp fully covered; B is the separate, remaining frontier.**

## Host→hyp coverage is automatic (macro auto-injection) — no per-boundary work
As of 2026-07-28, coverage is injected once into the HVC primitive macros
(`KVM_PKVM_COV_HVC` in `kvm_call_hyp_nvhe` + `kvm_call_refill_hyp_nvhe`), so **you do not
hand-instrument boundaries anymore**. See `a1-macro-cov-injection.md` for the design + safety
reasoning. To reach *more* host→hyp code, drive **new inputs** that exercise more HVCs (module ops,
IOMMU, PSCI, relax-perms, …) — the coverage follows automatically. Verify any rebuild the usual way:
new `.rs` lines attributable, **`LOST_IN_RING=0`**, `skip_not_owner=0`, no WARN/hang, exec/s healthy
(see `scripts/pkvm/a2/` for the capture + set-algebra harness).

## Methodology rules (hold these)
1. **The instrument is the limiter, not the DUT** (proven 3×: drains metric, tmpfs-chroot toggle,
   head-biased ring). On "no new coverage," first check `LOST_IN_RING=0` before concluding the DUT
   has nothing.
2. **A toggle must be a syscall arg** — the executor chroots into a tmpfs newroot, so env vars /
   `/root` files never reach the fault code.
3. **Code reading is a hypothesis; hardware gives the answer** — retract mechanistic predictions
   that the run refutes.
4. **The safe global wrap IS the design (macro auto-injection)** — supersedes the old
   "never global-wrap" rule. Safety comes from wrapping only the atomic HVC (refill's sleeping
   topup stays outside the window) + `pkvm_cov_begin`'s self-guards (owner-CPU + KCOV), NOT from a
   per-site allowlist. Operator caveat: disarm the ring only when the campaign is idle. See
   `a1-macro-cov-injection.md`.

## Committed work (branch pkvm-lifecycle-fuzzing; kernel stays uncommitted in common-stage2mvp)
`94a29c2a6` PQ-banner · `942bf6622` gen-args pin · `aca4da48c` a2 hp_mode · `81d7d4cc2` MAX_PAGES
16→32 · `f8c015bfe` VMID-WARN suppress · `1304a5854` a1 create boundary.
