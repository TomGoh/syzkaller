# repro-pack — run every filed pKVM defect against your own kernel

This pack builds and runs the reproducer for each issue in `issues/`, and prints one line per issue:

```
RESULT 003: REPRODUCED  (4/5 default-THP runs: second KVM_RUN returned -1/errno=7 (E2BIG) ...)
```

Three verdicts, and the third is the one that matters:

| verdict | exit | means |
| --- | --- | --- |
| `REPRODUCED` | 10 | the defect's documented signature was observed on **your** kernel |
| `NOT-REPRODUCED` | 11 | the reproducer ran to completion, every precondition held, and the signature did not appear |
| `INCONCLUSIVE` | 12 | **nothing was learned.** A timeout, a build failure, a missing precondition, an unreadable or wrapped `dmesg` |

`INCONCLUSIVE` is never a pass. That distinction is the whole reason this pack exists: every reproducer here bounds itself with `alarm()` + `_exit(3)`, `_exit()` does not flush stdio, and piped output is block-buffered — so a program that **hangs prints absolutely nothing**, which is visually identical to "the check passed and found nothing". On 2026-08-06 a verification run reported zeros in every column and was nearly read as success while the kernel under test was livelocking every guest that enabled dirty logging. `rc=3` is the alarm. The scripts check the exit status, print it next to every result, and route it to `INCONCLUSIVE`. See `issues/README.md` and `issues/runs/2026-08-06-fix-deploy-verify.md`.

---

## 1. What your host has to be

**An arm64 machine booted `kvm-arm.mode=protected`.** Every defect in this tracker lives behind pKVM being active; on an ordinary nVHE or VHE host none of them is reachable and every script will stop at `INCONCLUSIVE`.

`kvm-arm.mode=protected` is a **kernel command-line parameter**, set in the bootloader — there is no runtime switch and no module parameter for it (`arch/arm64/kvm/arm.c` registers it with `early_param()`). On a generic arm64 distro that means `GRUB_CMDLINE_LINUX` in `/etc/default/grub` followed by `update-grub`, or the platform's boot image / device-tree `chosen/bootargs`. On the Kylin V11 boards this project uses (N90, D3000) it is on the pKVM menuentry in GRUB; `issues/runs/2026-08-06-fix-deploy-verify.md` records the exact layout on those boards, including that the live menu is `/boot/grub/grub.cfg` and the pKVM kernel is entry 0.

Check it two ways, because the second is the one that proves the first took effect:

```
$ cat /proc/cmdline | tr ' ' '\n' | grep kvm-arm
kvm-arm.mode=protected

$ dmesg | grep 'mode initialized successfully'
kvm [1]: Protected nVHE mode initialized successfully
```

If that second line says `VHE mode` or `Hyp mode` instead, protected mode did not initialise and the scripts will tell you so.

You also need, for all issues:

- **root**, or at least read/write on `/dev/kvm` and an unrestricted `dmesg` (`kernel.dmesg_restrict=0`). Four of the six issues have a kernel log line as their only signature.
- **no syzkaller campaign running on the same board.** The fuzzer emits the same warnings, and `syz-manager` clears the dmesg ring per instance session (`vm/isolated/isolated.go:340`) — a cleared buffer between two snapshots is detected here and reported as `INCONCLUSIVE`, but you will have wasted the run. `run-all.sh` warns if it sees a `syz-*` process.

### Extra preconditions, per issue

**Issues 002–005 additionally need an ORDINARY (non-protected) guest and dirty logging.** A protected VM is turned away twice in code before it can reach any of them, so none of these reproducers creates one: they all use plain `KVM_CREATE_VM` with type 0, and then enable `KVM_MEM_LOG_DIRTY_PAGES` on an existing memslot. The two gates, and why this configuration is the interesting one rather than an artifact of how we fuzz, are laid out in [`../../REACHABILITY.md`](../../REACHABILITY.md) — read it before using these results to set priorities, because the obvious conclusion ("we mainly run protected VMs, so this matters less") is half wrong.

**Issue 003 additionally needs `transparent_hugepage/enabled` to be `[always]`.** `probe-dirtylog-thp.c` only ever calls `MADV_NOHUGEPAGE` (in its `nohuge` mode); it never asks for a huge page, so its default mode is THP-backed only when the system default is. Under `[madvise]` or `[never]` the huge-page precondition is simply absent, and `run-003.sh` stops at `INCONCLUSIVE` rather than reporting a clean run.

**Issue 001 additionally needs a CPU WITHOUT `ARM64_HAS_STAGE2_FWB`.** On FWB-capable silicon `kvm_arch_vcpu_ioctl_vcpu_init()` takes the `icache_inval_all_pou()` branch and never calls `stage2_unmap_vm()` at all, so the deadlock is unreachable — which is very likely why it survived upstream unnoticed. **N90 and D3000 both lack FWB; most upstream test hardware has it.** There is no userspace-visible bit for this, so `run-001.sh` does *not* check it and cannot: on an FWB machine you will get `NOT-REPRODUCED`, which is indistinguishable from a fixed kernel. The verdict line says so. If you need certainty, run it on hardware known to lack FWB.

---

## 2. Getting it onto the target

The scripts read the reproducer sources out of the tracker, so copy the whole `issues/` tree (it is a few hundred KB of `.md` and `.c`):

```
rsync -a --exclude runs/ /path/to/syzkaller-pkvm/issues/ root@BOARD:/root/issues/
ssh root@BOARD 'cd /root/issues/tools/repro-pack && ./run-all.sh'
```

`uname -m` is checked: on `aarch64` the scripts build natively with `cc`, otherwise they cross-build with `aarch64-linux-gnu-gcc` and stop at `INCONCLUSIVE` before running anything.

**If the board has no compiler**, build on your workstation and carry the binaries over:

```
# on the workstation (needs gcc-aarch64-linux-gnu and glibc's static libs)
OUTDIR=/tmp/pack ./run-all.sh --build-only
rsync -a /tmp/pack/ root@BOARD:/root/pack/
# on the board
BINDIR=/root/pack ./run-all.sh
```

Nothing is ever compiled into the work tree: `OUTDIR` defaults to a fresh `mktemp -d`, and everything — binaries, per-run logs, dmesg snapshots — lands there. The tracker's `repro/` directories hold `.c` files only, and this pack keeps it that way.

### Knobs

| variable | default | effect |
| --- | --- | --- |
| `OUTDIR` | `mktemp -d` | where binaries and logs go |
| `BINDIR` | unset | use pre-built binaries from here instead of compiling |
| `CC` | `cc` / `aarch64-linux-gnu-gcc` | compiler |
| `CFLAGS_OPT` | `-O1` | set `-O2` to match the exact build 001–005's recorded observations were made with |
| `REPS` | `5` | repetitions of issue 003's non-deterministic arm |
| `SETTLE` | `5` (2 for 002) | seconds to wait for teardown-time `printk` before reading `dmesg` |
| `TIMEOUT_00N` | 60–180 | hard outer bound per reproducer |

---

## 3. Running it

```
./run-all.sh                      # issues 002 003 004 005 006, in that order
./run-all.sh --include-hazardous  # ... then 001
./run-all.sh --build-only         # compile everything, execute nothing
```

The order is not arbitrary. 002–005 are harmless and do not perturb each other. **006 leaks EL2 pins permanently** — each triggering VM poisons a few more pages of the vCPU slab, nothing recovers them short of a reboot, and eventually `KVM_CREATE_VCPU` itself starts failing — so it runs after everything that needs a working vCPU, and you should reboot before re-running it or before testing a fix for it. **001 is last and opt-in**, because it wedges the machine.

### The 001 warning, in full

`hazard: wedges-target`. On an affected kernel `run-001.sh` leaves a task in `D` state holding `mmap_lock` for read while it waits to take the same rwsem for write. It is unkillable. Linux rwsems are fair, so every subsequent *reader* of that `mm` queues behind a writer that can never be granted: `ps`, `pgrep`, and ordinary system daemons that walk `/proc` freeze one after another, and the board decays on a timer. This was measured, not assumed — a stock Kylin `OptiDaemon` wedged with no help from the test. Recovery is sysrq over an ssh session that is still alive, or **physical access**. Do not run it on a board you cannot power-cycle, and run nothing else from this pack afterwards until it has rebooted.

`run-all.sh` requires `--include-hazardous`, prints the warning, and counts down ten seconds (`--yes` skips the countdown). Without the flag, 001 shows as `SKIPPED` in the table.

---

## 4. What you should see, per issue

Line numbers and offsets below come from the recorded evidence in each issue's `evidence/` directory and will differ on your build; the scripts match on function names, not line numbers.

### 001 — `pkvm_unmap_guest()` self-deadlock on `mmap_lock`

*Defective.* The reproducer stops after its last flushed line and never returns; the script reads the live task out of `/proc` (never `/cmdline` or `/maps` — 001's own blast-radius table records that both block forever):

```
    first KVM_RUN returned -1 (errno 4) -- pages should now be pinned
    second KVM_ARM_VCPU_INIT -- hangs here on an affected kernel...
--- live task state of pid 79770 ---
    state=D
    wchan=account_locked_vm
    [<0>] account_locked_vm+0x4c/0x108
    [<0>] __unmap_stage2_range+0x230/0x2e8
    [<0>] stage2_unmap_vm+0x178/0x298
RESULT 001: REPRODUCED  (pid 79770 stuck in D state inside the second KVM_ARM_VCPU_INIT, wchan=account_locked_vm, 1 hung-task report(s) in dmesg)
```

*Fixed (or FWB silicon).*

```
    PASS: second KVM_ARM_VCPU_INIT returned 0 (errno 0) -- no deadlock
RESULT 001: NOT-REPRODUCED  (second KVM_ARM_VCPU_INIT returned promptly, no deadlock. NB this is also what an FWB-capable CPU looks like ...)
```

### 002 — the range TLB flush asks EL2 for a retired hypercall

*Defective.* One new warning per run, naming your pid and `Comm`:

```
    WARNING: CPU: 0 PID: 92660 at arch/arm64/kvm/hyp/pgtable.c:654 kvm_tlb_flush_vmid_range+0x74/0xe0
    CPU: 0 PID: 92660 Comm: repro-tlbflush Not tainted 6.6.103+ #4
RESULT 002: REPRODUCED  (1 new dmesg line(s) naming kvm_tlb_flush_vmid_range, 1 of the surrounding blocks tagged Comm: repro-tlbflush)
```

*Fixed.* No warning, `RESULT 002: NOT-REPRODUCED`.

> **Do not read that as "the flush works."** 002's `ISSUE.md` establishes that this reproducer *cannot* verify the fix: it creates no vCPU, so `kvm->arch.pkvm.handle` is still 0, and after the fix EL2 maps handle 0 to an out-of-range index and returns success without flushing. The warning disappears because the call became a **no-op**. The script therefore also runs `verify-dirtylog.c`, which does create a vCPU, and prints its result as a clearly-labelled `NOTE` — but that program is confounded on an unpatched kernel, where it fails for an issue-003 reason (its 2 MiB mapping is THP-backed and it never calls `MADV_NOHUGEPAGE`). Only its explicit `PASS:` / `FAIL: ... TLB invalidation did not happen` lines say anything about 002.

### 003 — dirty logging is unusable for a huge-page-backed guest

*Defective.* Frequent but **not deterministic** while issue 002 is also unfixed — 22 of 25 on build `#4` — which is why the script repeats the arm `REPS` times:

```
    second KVM_RUN ret=-1 errno=7 (Argument list too long) exit=0
RESULT 003: REPRODUCED  (4/5 default-THP runs: second KVM_RUN returned -1/errno=7 (E2BIG) after KVM_MEM_LOG_DIRTY_PAGES)
```

*Fixed.* `second KVM_RUN ret=0 errno=0 (-) exit=6`, five times out of five, and both controls clean:

```
RESULT 003: NOT-REPRODUCED  (0/5 default-THP runs returned -E2BIG, and both controls passed (on build #4 this was 22/25))
```

### 004 — the hyp-donation leak check is broken, in both directions

*Defective.* One line per dirty-logging VM teardown, printed after the process has already exited:

```
    kvm [51957]: 18446744073709543424B of donations to the nVHE hyp are missing
RESULT 004: REPRODUCED  (1 donation-accounting message(s) at VM teardown after dirty logging)
```

That number is `-8192` printed through `%llu` — the counter is short by two pages, not over by 18 exabytes.

*Fixed.* Nothing at all, in any wording. The check only prints on a non-zero residual, so silence here **is** the pass. The script matches three wordings, because part (b) of the fix rewrites the message (`are missing` → `were never returned`, plus a new `nVHE hyp donation accounting is ...B short` for the negative direction); note that 004's front-matter `signature:` still quotes the old wording only.

```
RESULT 004: NOT-REPRODUCED  (no donation-accounting message in any wording after a dirty-logging VM lifecycle: the residual is zero)
```

### 005 — a successful dirty-log call wipes the page's EL2 state

Two halves, and the script accepts either.

*Defective.* The silent half first — page A is written under dirty logging, reported, re-write-protected (which now fails with `-EPERM` and the error is discarded), and its next write is lost:

```
    after round2:          A(page 16)=1  B(page 32)=0
    after round3:          A(page 16)=0  B(page 32)=1
    WARNING: CPU: 7 PID: 94783 at arch/arm64/kvm/mmu.c:456 __unmap_stage2_range+0x26c/0x2b8
RESULT 005: REPRODUCED  (page A's round-3 write under dirty logging is absent from the bitmap (A=0, B=1), and 1 teardown WARN(s) in __unmap_stage2_range)
```

*Fixed.* `A=1` in round 3 and no WARN. **`B` is deliberately not part of the criterion** — the control page is tracked only 3/5 on current builds for an unrelated and still-unexplained reason, so requiring `B=1` produces false failures.

```
RESULT 005: NOT-REPRODUCED  (A=1: the re-write-protected page's next write was recorded, and no teardown WARN appeared)
```

Run this in `nohuge` mode, which the script does: under THP, issue 003 aborts the dirty-log hypercall before EL2 mutates anything and 005 is masked. If you see `INCONCLUSIVE (... issue 003 fired even in nohuge mode ...)`, fix or work around 003 first.

### 006 — a suspended vCPU leaks its EL2 pin

*Defective.* The `KVM_RUN` in the SUSPENDED arm is refused by EL2, and the pin is never released:

```
    [test-SUSPENDED] KVM_RUN = -1  errno=22 (Invalid argument)
    WARNING: CPU: 0 PID: 474382 at arch/arm64/kvm/mmu.c:728 kvm_unshare_hyp+0x12c/0x140
    ... one WARN per page of struct kvm_vcpu (three on this build)
RESULT 006: REPRODUCED  (3 new WARN(s) naming kvm_unshare_hyp after the control+test pair, ...)
```

On the pre-fix kernel `#16` this was three WARNs on the very first run, and by the third repeat a `FATAL: KVM_CREATE_VCPU: Invalid argument` from a vCPU landing on an already-poisoned page (`006-*/evidence/2026-08-07-fix-verified-on-18.txt`).

*Fixed.* Zero WARNs from **both** arms, across repeated runs, with no `FATAL`. Note that `KVM_RUN = -1 errno=22` **stays** on a fixed kernel — EL2 still refuses `mp_state=SUSPENDED`, and the fix (`2b4d43af6`, hoisting the `hyp_vcpu->host_vcpu` assignment above the checks) is about the cleanup on that rejection, not about accepting the state. The same evidence file puts it as *"expect the -EINVAL and check dmesg, not the exit code"*, which is exactly what `run-006.sh` does.

```
RESULT 006: NOT-REPRODUCED  (both arms completed and no kvm_unshare_hyp WARN appeared, so no pin was left behind)
```

Two limits, both reported in the output rather than hidden:

- The probe runs its control and test arms **in one process**, so they share a pid and a `Comm` and a single `dmesg` delta cannot attribute a WARN to the SUSPENDED arm. The split (control 0, test 3) was measured separately on hardware and is recorded in 006's `ISSUE.md`; this script cannot re-derive it.
- **The leak is permanent.** If the board already carries poisoned slab pages, the probe fails up front with `FATAL: KVM_CREATE_VCPU: Invalid argument` — which is itself a consequence of the defect but not a clean observation, so the script returns `INCONCLUSIVE` and tells you to reboot.

---

## 5. Notes on the sources

- **003, 004 and 005 share a filename, and the file really is the same one.** `003-*/repro/probe-dirtylog-thp.c`, `004-*/repro/probe-dirtylog-thp.c` and `005-*/repro/probe-dirtylog-thp.c` are byte-identical (`cmp` clean, md5 `871b6f4189b6b019f61c9d50d78f9663`). Each script builds from its own issue's copy anyway, so the pack keeps working if they ever diverge.
- **Each script uses the program named in its issue's `repro:` front-matter field**, which is what the tracker treats as the reproducer. 005's prose also mentions `probe-dirtylog-thp.c nohuge`; its front-matter names `probe-dirtylog-twice.c`, which is also the program its `Fix status` section designates as the regression test, so that is the one used here.
- **002 ships two programs.** `repro-tlbflush-warn.c` is the reproducer (and the verdict); `verify-dirtylog.c` is a fix-verifier, run as a supplementary check for the reasons above.
- **006's reproducer is hand-written, not syzkaller-extracted.** Syzkaller saw the crash twice during the 2026-08-06 campaign and failed to reduce either occurrence to a program (`006-*/evidence/2026-08-07-repro-attempt-FAILED.txt`); `probe-mpstate-suspended-pin-leak.c` was written by hand the same morning from the root cause, and confirmed on N90.
- **`REACHABILITY.md` predates issue 006** — it opens "which guests can actually reach these **five** defects" and its table covers 001–005 only. 006 is not a dirty-logging issue and needs only `/dev/kvm` on a pKVM host.
- **All six issues are `disposition: fix-verified` on klinux as of 2026-08-07.** So on the klinux tree this pack is a regression test and every line should read `NOT-REPRODUCED`. It is on a *clean, unpatched* kernel — ACK `android15-6.6`, or any tree that imported the ACK pKVM guest-stage-2 series without these fixes — that it should light up. Which issues a given unpatched tree carries is set out per-tree in the `Fix status` section of each `ISSUE.md`; 002 is the only one with a usable upstream patch (`fce886a60207`), and 003/005 were removed rather than fixed upstream.
